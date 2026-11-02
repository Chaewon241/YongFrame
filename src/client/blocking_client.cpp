#include "client/blocking_client.h"

#include <array>
#include <utility>
#include <vector>

#include "common/error_text.h"
#include "protocol/message_ids.h"

namespace game::client {

namespace {

// 서버가 연결을 닫았을 때 읽기에서 나오는 오류들. 윈도우는 강제 종료 시 reset/aborted로 나온다.
bool IsClosedError(const boost::system::error_code& ec) {
  return ec == boost::asio::error::eof || ec == boost::asio::error::connection_reset ||
         ec == boost::asio::error::connection_aborted || ec == boost::asio::error::broken_pipe;
}

}  // namespace

BlockingClient::BlockingClient() : socket_(io_) {}

template <typename StartFn>
bool BlockingClient::RunWithTimeout(Millis timeout, StartFn&& start, boost::system::error_code& ec,
                                    std::size_t& transferred) {
  bool done = false;
  start([&](const boost::system::error_code& e, std::size_t n) {
    ec = e;
    transferred = n;
    done = true;
  });

  io_.restart();
  io_.run_for(timeout);
  if (done) {
    return true;
  }

  // 시간 초과: 진행 중인 작업을 취소하고, 취소 결과를 알리는 핸들러가 호출될 때까지 돌린다.
  // 이 핸들러는 이 함수의 지역 변수(done, ec)를 참조하므로 반드시 호출이 끝난 뒤에 반환해야 한다.
  boost::system::error_code ignored;
  socket_.cancel(ignored);
  io_.restart();
  io_.run();

  // 취소 직전에 작업이 이미 끝났다면(데이터가 막 도착한 경우) 그 결과를 살린다.
  return ec != boost::asio::error::operation_aborted;
}

bool BlockingClient::Connect(const std::string& host, std::uint16_t port, Millis timeout) {
  boost::system::error_code ec;
  boost::asio::ip::tcp::resolver resolver(io_);
  const auto endpoints = resolver.resolve(host, std::to_string(port), ec);
  if (ec) {
    last_error_ = "주소 해석 실패: " + common::ErrorText(ec);
    return false;
  }

  // 후보 주소를 차례로 시도한다. 하나가 제한 시간 안에 연결되면 성공.
  for (const auto& entry : endpoints) {
    Close();
    socket_.open(entry.endpoint().protocol(), ec);
    if (ec) {
      continue;
    }

    std::size_t unused = 0;
    boost::system::error_code connect_ec;
    const bool finished = RunWithTimeout(
        timeout,
        [&](auto handler) {
          socket_.async_connect(entry.endpoint(),
                                [handler](const boost::system::error_code& e) { handler(e, 0); });
        },
        connect_ec, unused);

    if (finished && !connect_ec) {
      // 작은 요청/응답을 즉시 주고받는 용도라 Nagle 알고리즘을 끈다.
      socket_.set_option(boost::asio::ip::tcp::no_delay(true), ec);
      decoder_ = protocol::FrameDecoder();
      return true;
    }
    last_error_ = finished ? "연결 실패: " + common::ErrorText(connect_ec) : "연결 시간 초과";
  }
  Close();
  if (last_error_.empty()) {
    last_error_ = "연결할 주소가 없음";
  }
  return false;
}

bool BlockingClient::SendRaw(std::span<const std::uint8_t> bytes, Millis timeout) {
  boost::system::error_code ec;
  std::size_t written = 0;
  const bool finished = RunWithTimeout(
      timeout,
      [&](auto handler) {
        boost::asio::async_write(socket_, boost::asio::buffer(bytes.data(), bytes.size()), handler);
      },
      ec, written);

  if (!finished) {
    last_error_ = "전송 시간 초과";
    return false;
  }
  if (ec) {
    last_error_ = "전송 실패: " + common::ErrorText(ec);
    return false;
  }
  return true;
}

bool BlockingClient::SendFrame(std::uint16_t message_id, std::span<const std::uint8_t> payload,
                               Millis timeout) {
  const auto wire = protocol::EncodeFrame(message_id, payload);
  if (!wire) {
    last_error_ = "payload가 최대 크기를 초과";
    return false;
  }
  return SendRaw(*wire, timeout);
}

bool BlockingClient::SendPing(std::uint64_t request_id, std::string_view payload, Millis timeout) {
  protocol::PingRequest request;
  request.set_request_id(request_id);
  request.set_payload(std::string(payload));

  std::vector<std::uint8_t> body(request.ByteSizeLong());
  request.SerializeToArray(body.data(), static_cast<int>(body.size()));
  return SendFrame(protocol::ToUint16(protocol::MessageId::kPingRequest), body, timeout);
}

BlockingClient::ReceiveStatus BlockingClient::ReadSome(std::span<std::uint8_t> buffer, Millis timeout,
                                                       std::size_t& bytes) {
  boost::system::error_code ec;
  bytes = 0;
  const bool finished = RunWithTimeout(
      timeout,
      [&](auto handler) {
        socket_.async_read_some(boost::asio::buffer(buffer.data(), buffer.size()), handler);
      },
      ec, bytes);

  if (!finished) {
    return ReceiveStatus::kTimeout;
  }
  if (ec) {
    if (IsClosedError(ec)) {
      return ReceiveStatus::kClosed;
    }
    last_error_ = "수신 실패: " + common::ErrorText(ec);
    return ReceiveStatus::kError;
  }
  return ReceiveStatus::kFrame;  // "읽기 성공"의 의미로 재사용. 호출하는 쪽에서 bytes를 본다.
}

BlockingClient::ReceiveStatus BlockingClient::ReceiveFrame(protocol::Frame& out, Millis timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::array<std::uint8_t, 8192> buffer{};

  while (true) {
    // 이미 받아 둔 바이트로 프레임이 완성되는지 먼저 본다. (한 번에 여러 프레임이 올 수 있다)
    switch (decoder_.Next(out)) {
      case protocol::DecodeStatus::kFrame:
        return ReceiveStatus::kFrame;
      case protocol::DecodeStatus::kError:
        last_error_ = "서버가 잘못된 프레임을 보냄";
        return ReceiveStatus::kProtocolError;
      case protocol::DecodeStatus::kNeedMoreData:
        break;
    }

    const auto remaining = std::chrono::duration_cast<Millis>(deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      return ReceiveStatus::kTimeout;
    }

    std::size_t bytes = 0;
    const ReceiveStatus status = ReadSome(buffer, remaining, bytes);
    if (status != ReceiveStatus::kFrame) {
      return status;  // 타임아웃, 닫힘, 오류
    }
    decoder_.Append(std::span<const std::uint8_t>(buffer.data(), bytes));
  }
}

BlockingClient::ReceiveStatus BlockingClient::ReceivePingResponse(protocol::PingResponse& out,
                                                                  Millis timeout) {
  protocol::Frame frame;
  const ReceiveStatus status = ReceiveFrame(frame, timeout);
  if (status != ReceiveStatus::kFrame) {
    return status;
  }
  if (frame.message_id != protocol::ToUint16(protocol::MessageId::kPingResponse)) {
    last_error_ = "PingResponse가 아닌 메시지 ID: " + std::to_string(frame.message_id);
    return ReceiveStatus::kProtocolError;
  }
  if (!out.ParseFromArray(frame.payload.data(), static_cast<int>(frame.payload.size()))) {
    last_error_ = "PingResponse 파싱 실패";
    return ReceiveStatus::kProtocolError;
  }
  return ReceiveStatus::kFrame;
}

bool BlockingClient::WaitForServerClose(Millis timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::array<std::uint8_t, 4096> buffer{};

  while (true) {
    const auto remaining = std::chrono::duration_cast<Millis>(deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      return false;
    }
    std::size_t bytes = 0;
    const ReceiveStatus status = ReadSome(buffer, remaining, bytes);
    if (status == ReceiveStatus::kClosed) {
      return true;
    }
    if (status != ReceiveStatus::kFrame) {
      return false;  // 타임아웃 또는 다른 오류. 데이터가 오면(kFrame) 버리고 계속 기다린다.
    }
  }
}

void BlockingClient::Close() {
  boost::system::error_code ignored;
  socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
  socket_.close(ignored);
}

}  // namespace game::client
