#pragma once

// 채팅 통합 테스트들이 공유하는 클라이언트 쪽 도우미.
//
// 서버가 보내는 프레임에는 다른 사람의 입장/퇴장 알림(PresenceNotice)과 전파(ChatBroadcast)가 응답 사이에
// 끼어들 수 있다. 그래서 Receive()는 "기다리는 종류가 아닌 지정한 종류"를 건너뛰며 읽는다.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "client/blocking_client.h"
#include "protocol/frame.h"
#include "protocol/message_ids.h"
#include "test_support.h"

namespace game::testing_support {

using client::BlockingClient;
using protocol::MessageId;
using Millis = std::chrono::milliseconds;
using Status = BlockingClient::ReceiveStatus;

inline constexpr Millis kTimeout{3000};

// 요청을 보내거나 응답을 받는 데 실패했음을 나타내는 값. proto3 열거형은 정의되지 않은 숫자도 담을 수 있어서
// 어떤 정상 결과 코드와도 겹치지 않는 값을 쓴다.
inline constexpr protocol::ResultCode kTransportFailure = static_cast<protocol::ResultCode>(-1);


inline std::unique_ptr<BlockingClient> Connect(const TestServer& server) {
  auto c = std::make_unique<BlockingClient>();
  EXPECT_TRUE(c->Connect("127.0.0.1", server.port())) << c->last_error();
  return c;
}

inline bool SendProto(BlockingClient& c, MessageId id, const google::protobuf::MessageLite& message) {
  std::vector<std::uint8_t> body(message.ByteSizeLong());
  message.SerializeToArray(body.data(), static_cast<int>(body.size()));
  return c.SendFrame(protocol::ToUint16(id), body);
}

// 다음 프레임들을 읽다가 expected 종류가 나오면 T로 해석해 돌려준다.
// skip에 든 종류는 읽고 버린다. 그 밖의 종류가 먼저 오면 실패로 본다.
template <typename T>
::testing::AssertionResult Receive(BlockingClient& c, MessageId expected, T& out,
                                   std::vector<MessageId> skip = {MessageId::kPresenceNotice},
                                   Millis timeout = kTimeout) {
  while (true) {
    protocol::Frame frame;
    const Status status = c.ReceiveFrame(frame, timeout);
    if (status != Status::kFrame) {
      return ::testing::AssertionFailure() << "프레임을 받지 못함 (상태 " << static_cast<int>(status)
                                           << ", 기다린 ID " << static_cast<int>(expected) << "): " << c.last_error();
    }
    if (frame.message_id == protocol::ToUint16(expected)) {
      if (!out.ParseFromArray(frame.payload.data(), static_cast<int>(frame.payload.size()))) {
        return ::testing::AssertionFailure() << "파싱 실패 (ID " << frame.message_id << ")";
      }
      return ::testing::AssertionSuccess();
    }
    const bool skippable = std::any_of(skip.begin(), skip.end(), [&](MessageId id) {
      return frame.message_id == protocol::ToUint16(id);
    });
    if (!skippable) {
      return ::testing::AssertionFailure() << "예상하지 못한 ID: 기다린 " << static_cast<int>(expected)
                                           << ", 받은 " << frame.message_id;
    }
  }
}

// 입장을 요청하고 결과 코드를 돌려준다.
inline protocol::ResultCode JoinResult(BlockingClient& c, const std::string& nickname) {
  protocol::JoinRequest request;
  request.set_nickname(nickname);
  protocol::JoinResponse response;
  if (!SendProto(c, MessageId::kJoinRequest, request) || !Receive(c, MessageId::kJoinResponse, response)) {
    ADD_FAILURE() << "입장 요청/응답 실패: " << c.last_error();
    return protocol::RESULT_STORAGE_FAILURE;
  }
  return response.result();
}

inline ::testing::AssertionResult JoinOk(BlockingClient& c, const std::string& nickname) {
  const auto result = JoinResult(c, nickname);
  if (result != protocol::RESULT_OK) {
    return ::testing::AssertionFailure() << nickname << " 입장 실패, 결과 코드 " << result;
  }
  return ::testing::AssertionSuccess();
}

// 채팅을 보내고 ChatResponse를 돌려준다. 그 사이 끼어드는 알림과 전파는 건너뛴다.
inline protocol::ChatResponse SendChat(BlockingClient& c, const std::string& text) {
  protocol::ChatRequest request;
  request.set_text(text);
  protocol::ChatResponse response;
  response.set_result(kTransportFailure);
  if (!SendProto(c, MessageId::kChatRequest, request) ||
      !Receive(c, MessageId::kChatResponse, response, {MessageId::kPresenceNotice, MessageId::kChatBroadcast})) {
    ADD_FAILURE() << "채팅 요청/응답 실패: " << c.last_error();
  }
  return response;
}

inline protocol::HistoryResponse RequestHistory(BlockingClient& c, std::uint32_t limit) {
  protocol::HistoryRequest request;
  request.set_limit(limit);
  protocol::HistoryResponse response;
  response.set_result(kTransportFailure);
  if (!SendProto(c, MessageId::kHistoryRequest, request) ||
      !Receive(c, MessageId::kHistoryResponse, response, {MessageId::kPresenceNotice, MessageId::kChatBroadcast})) {
    ADD_FAILURE() << "기록 요청/응답 실패: " << c.last_error();
  }
  return response;
}

// 입장 여부와 상관없이 Ping이 되는지 본다. (연결이 아직 쓸 수 있는 상태인지 확인하는 용도)
inline ::testing::AssertionResult PingWorks(BlockingClient& c, std::uint64_t request_id = 4242) {
  if (!c.SendPing(request_id, "ping")) {
    return ::testing::AssertionFailure() << "Ping 전송 실패: " << c.last_error();
  }
  protocol::PingResponse response;
  const auto received = Receive(c, MessageId::kPingResponse, response,
                                {MessageId::kPresenceNotice, MessageId::kChatBroadcast});
  if (!received) return received;
  if (response.request_id() != request_id || response.payload() != "ping") {
    return ::testing::AssertionFailure() << "Ping 응답 내용이 다름";
  }
  return ::testing::AssertionSuccess();
}

// 새 연결로 Ping이 되는지 본다. 비정상 입력을 받은 뒤에도 서버가 살아 있는지 확인한다.
inline ::testing::AssertionResult ServerStillWorks(const TestServer& server) {
  BlockingClient c;
  if (!c.Connect("127.0.0.1", server.port())) {
    return ::testing::AssertionFailure() << "새 연결 실패: " << c.last_error();
  }
  return PingWorks(c, 777);
}

}  // namespace game::testing_support
