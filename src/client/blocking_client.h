#pragma once

// BlockingClient: 테스트와 수동 실험에 쓰는 "동기식" TCP 클라이언트.
//
// 서버 쪽(Session)은 비동기로 만들었지만, 테스트 코드는 "보내고, 기다리고, 확인한다"는 순서대로
// 읽히는 편이 낫다. 그래서 호출하는 쪽에는 동기 함수로 보이게 하고, 내부에서는 Asio 비동기 작업을
// io_context::run_for(제한 시간)으로 돌려서 모든 대기에 타임아웃을 건다. (Asio의 동기 소켓 함수에는
// 타임아웃이 없어서, 서버가 응답하지 않으면 테스트가 영원히 멈춘다)
//
// 용도
//  - tests/server_integration_test.cpp: 비정상 입력을 포함한 서버 시나리오 검증
//  - src/client/ping_client.cpp: 수동으로 Ping을 보내 보는 실행 파일
//
// 스레드 안전하지 않다. 객체 하나는 한 스레드에서만 쓴다.

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <boost/asio.hpp>

#include "ping.pb.h"
#include "protocol/frame.h"

namespace game::client {

class BlockingClient {
 public:
  using Millis = std::chrono::milliseconds;

  // ReceiveFrame / ReceivePingResponse의 결과.
  enum class ReceiveStatus {
    kFrame,          // 프레임(또는 응답)을 하나 받았다.
    kTimeout,        // 제한 시간 안에 오지 않았다.
    kClosed,         // 서버가 연결을 닫았다. (정상 종료 또는 리셋)
    kError,          // 그 밖의 소켓 오류. last_error()에 설명이 있다.
    kProtocolError,  // 서버가 보낸 바이트가 프레임 규약에 맞지 않거나, 예상한 응답이 아니다.
  };

  BlockingClient();
  BlockingClient(const BlockingClient&) = delete;
  BlockingClient& operator=(const BlockingClient&) = delete;

  // 서버에 연결한다. host는 "127.0.0.1" 같은 주소 또는 이름. 성공하면 true.
  bool Connect(const std::string& host, std::uint16_t port, Millis timeout = Millis(5000));

  // ---- 보내기 ----
  // 원시 바이트를 그대로 보낸다. 잘못된 길이나 쪼개 보내기 같은 비정상 입력을 만들 때 쓴다.
  bool SendRaw(std::span<const std::uint8_t> bytes, Millis timeout = Millis(5000));

  // [길이][ID][payload] 프레임을 만들어 보낸다.
  bool SendFrame(std::uint16_t message_id, std::span<const std::uint8_t> payload,
                 Millis timeout = Millis(5000));

  // PingRequest(request_id, payload)를 만들어 프레임으로 보낸다.
  bool SendPing(std::uint64_t request_id, std::string_view payload, Millis timeout = Millis(5000));

  // ---- 받기 ----
  // 프레임 하나를 받는다. 쪼개져 오는 바이트는 내부 FrameDecoder가 이어 붙인다.
  ReceiveStatus ReceiveFrame(protocol::Frame& out, Millis timeout);

  // PingResponse 하나를 받는다. 다른 메시지 ID이거나 파싱에 실패하면 kProtocolError.
  ReceiveStatus ReceivePingResponse(protocol::PingResponse& out, Millis timeout);

  // 서버가 연결을 닫을 때까지 기다린다. 그 사이 도착하는 데이터는 버린다.
  // 닫히면 true, 제한 시간까지 닫히지 않으면 false.
  bool WaitForServerClose(Millis timeout);

  void Close();

  // 마지막 실패의 설명. 테스트 실패 메시지에 붙이는 용도.
  const std::string& last_error() const { return last_error_; }

 private:
  // 비동기 작업 하나를 시작하고 timeout 안에 끝나기를 기다린다.
  // start는 완료 핸들러(error_code, size_t)를 받아 비동기 작업을 시작하는 함수이다.
  // 제한 시간이 지나면 작업을 취소하고, 취소된 핸들러가 호출될 때까지 io_context를 마저 돌린다.
  // 반환값: 작업이 (성공이든 실패든) 실제로 끝났으면 true, 시간 초과로 취소됐으면 false.
  template <typename StartFn>
  bool RunWithTimeout(Millis timeout, StartFn&& start, boost::system::error_code& ec,
                      std::size_t& transferred);

  // 소켓에서 최대 buffer 크기만큼 읽는다.
  ReceiveStatus ReadSome(std::span<std::uint8_t> buffer, Millis timeout, std::size_t& bytes);

  boost::asio::io_context io_;
  boost::asio::ip::tcp::socket socket_;
  protocol::FrameDecoder decoder_;
  std::string last_error_;
};

}  // namespace game::client
