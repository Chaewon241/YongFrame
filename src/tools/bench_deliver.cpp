// bench_deliver: Deliver 한 번의 비용을 단계별로 가르는 마이크로벤치(서버 없이 한 프로세스에서).
//
// 서버 틱 시간의 약 98%가 Deliver(직렬화 + 프레임 + 송신 큐 + 송신 시작)라는 계측 결과가 나왔을 때, 그 안에서
// 무엇이 큰지 알아보려고 만들었다. 측정 항목(스냅샷은 4명 분, 서버가 보내는 것과 같은 모양):
//   1) 직렬화: WorldSnapshot::SerializeToArray
//   2) 프레임 인코딩: EncodeFrame (헤더 붙이고 vector 할당)
//   3) 송신: 루프백 TCP로 프레임을 async_write 연쇄로 보내고 다른 스레드가 읽어 비운다(완료 처리 포함)
//   4) 송신(동기): write_some 시스템 호출만
// 이 숫자는 "이 PC에서 한 번에 한 연결"의 비용이다. 서버의 연결 수천 개, 캐시, 스레드 경쟁은 반영하지 못한다.
// 사용: bench_deliver [반복 횟수, 기본 200000]

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

#include <boost/asio.hpp>

#include "game.pb.h"
#include "protocol/frame.h"
#include "protocol/message_ids.h"

namespace {
using Clock = std::chrono::steady_clock;
using boost::asio::ip::tcp;

double Micros(Clock::duration d) { return std::chrono::duration<double, std::micro>(d).count(); }

game::protocol::WorldSnapshot MakeSnapshot() {
  game::protocol::WorldSnapshot snapshot;
  snapshot.set_tick(123456);
  for (std::uint32_t id = 1; id <= 4; ++id) {
    auto* state = snapshot.add_players();
    state->set_player_id(id);
    state->set_x(2400.5f + static_cast<float>(id) * 100.25f);
    state->set_y(2399.75f - static_cast<float>(id) * 50.5f);
    state->set_last_input_seq(1000 + id);
  }
  return snapshot;
}

// 루프백 연결 한 쌍을 만든다. peer는 다른 스레드가 읽어서 비운다.
struct Loopback {
  boost::asio::io_context io;
  tcp::socket writer{io};
  tcp::socket peer{io};
  std::atomic<bool> stop{false};
  std::thread drain;

  Loopback() {
    tcp::acceptor acceptor(io, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
    writer.connect(acceptor.local_endpoint());
    acceptor.accept(peer);
    writer.set_option(tcp::no_delay(true));
    drain = std::thread([this] {
      std::vector<std::uint8_t> buffer(1 << 16);
      boost::system::error_code ec;
      while (!stop.load()) {
        peer.read_some(boost::asio::buffer(buffer), ec);
        if (ec) break;
      }
    });
  }
  ~Loopback() {
    stop = true;
    boost::system::error_code ignored;
    writer.shutdown(tcp::socket::shutdown_both, ignored);
    writer.close(ignored);
    peer.close(ignored);
    if (drain.joinable()) drain.join();
  }
};

// async_write를 하나씩 이어서(앞 쓰기가 끝나면 다음) n번 보내는 데 걸리는 시간. 서버 Session의 송신 큐와 같은 모양이다.
double AsyncWriteChain(const std::vector<std::uint8_t>& frame, int n) {
  Loopback lo;
  int remaining = n;
  std::function<void()> next = [&] {
    if (remaining-- <= 0) return;
    boost::asio::async_write(lo.writer, boost::asio::buffer(frame), [&](const boost::system::error_code& ec, std::size_t) {
      if (!ec) next();
    });
  };
  const auto start = Clock::now();
  next();
  lo.io.run();
  return Micros(Clock::now() - start) / n;
}

double SyncWrite(const std::vector<std::uint8_t>& frame, int n) {
  Loopback lo;
  boost::system::error_code ec;
  const auto start = Clock::now();
  for (int i = 0; i < n; ++i) {
    boost::asio::write(lo.writer, boost::asio::buffer(frame), ec);
    if (ec) break;
  }
  return Micros(Clock::now() - start) / n;
}
}  // namespace

int main(int argc, char** argv) {
  const int n = argc > 1 ? std::atoi(argv[1]) : 200000;
  const auto snapshot = MakeSnapshot();
  std::vector<std::uint8_t> body(snapshot.ByteSizeLong());

  // 1) 직렬화
  auto t = Clock::now();
  std::size_t sink = 0;
  for (int i = 0; i < n; ++i) {
    snapshot.SerializeToArray(body.data(), static_cast<int>(body.size()));
    sink += body[0];
  }
  const double serialize_us = Micros(Clock::now() - t) / n;

  // 2) 프레임 인코딩
  t = Clock::now();
  std::vector<std::uint8_t> frame;
  for (int i = 0; i < n; ++i) {
    auto wire = game::protocol::EncodeFrame(game::protocol::ToUint16(game::protocol::MessageId::kWorldSnapshot), body);
    sink += wire->size();
    if (i == 0) frame = *wire;
  }
  const double encode_us = Micros(Clock::now() - t) / n;

  // 3), 4) 송신
  const int sends = n;
  const double async_us = AsyncWriteChain(frame, sends);
  const double sync_us = SyncWrite(frame, sends);

  std::cout << "스냅샷 본문 " << body.size() << "B, 프레임 " << frame.size() << "B, 반복 " << n << "회 (sink=" << sink << ")\n"
            << "직렬화(SerializeToArray)        : " << serialize_us << " us\n"
            << "프레임 인코딩(EncodeFrame)       : " << encode_us << " us\n"
            << "송신(async_write 연쇄, 완료 포함) : " << async_us << " us\n"
            << "송신(write 시스템 호출만)        : " << sync_us << " us\n"
            << "직렬화 + 프레임 / 송신(async)    : " << (serialize_us + encode_us) / async_us * 100.0 << " %\n";
  return 0;
}
