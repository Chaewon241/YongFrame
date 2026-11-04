// 채팅 통합 테스트: 진짜 서버를 띄우고 실제 TCP 연결로 docs/protocol-chat.md의 "2단계 검증 입력"을
// 확인한다. 방 로직의 세부(순서, 시간 초과 등)는 chat_room_test가 소켓 없이 검증하므로, 여기서는
// 메시지가 네트워크를 거쳐 올바른 연결에 올바른 순서로 닿는지와 연결 단위 동작(끊김, 느린 수신자)에 집중한다.
//
// 서버가 보내는 프레임에는 다른 사람의 입장/퇴장 알림(PresenceNotice)과 전파(ChatBroadcast)가 응답
// 사이에 끼어들 수 있다. 그래서 Receive()는 "기다리는 종류가 아닌 지정한 종류"를 건너뛰며 읽는다.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "client/blocking_client.h"
#include "protocol/frame.h"
#include "protocol/message_ids.h"
#include "test_support.h"

namespace game {
namespace {

using client::BlockingClient;
using protocol::MessageId;
using testing_support::DefaultServerConfig;
using testing_support::Load;
using testing_support::TestServer;
using testing_support::WaitUntil;
using Millis = std::chrono::milliseconds;
using Status = BlockingClient::ReceiveStatus;

constexpr Millis kTimeout{3000};

// 요청을 보내거나 응답을 받는 데 실패했음을 나타내는 값. proto3 열거형은 정의되지 않은 숫자도 담을 수 있어서
// 어떤 정상 결과 코드와도 겹치지 않는 값을 쓴다.
constexpr protocol::ResultCode kTransportFailure = static_cast<protocol::ResultCode>(-1);

[[maybe_unused]] ::testing::Environment* const kQuietLog =
    ::testing::AddGlobalTestEnvironment(new testing_support::QuietLogEnvironment);

// ============================================================ 도우미

std::unique_ptr<BlockingClient> Connect(const TestServer& server) {
  auto c = std::make_unique<BlockingClient>();
  EXPECT_TRUE(c->Connect("127.0.0.1", server.port())) << c->last_error();
  return c;
}

bool SendProto(BlockingClient& c, MessageId id, const google::protobuf::MessageLite& message) {
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
protocol::ResultCode JoinResult(BlockingClient& c, const std::string& nickname) {
  protocol::JoinRequest request;
  request.set_nickname(nickname);
  protocol::JoinResponse response;
  if (!SendProto(c, MessageId::kJoinRequest, request) || !Receive(c, MessageId::kJoinResponse, response)) {
    ADD_FAILURE() << "입장 요청/응답 실패: " << c.last_error();
    return protocol::RESULT_STORAGE_FAILURE;
  }
  return response.result();
}

::testing::AssertionResult JoinOk(BlockingClient& c, const std::string& nickname) {
  const auto result = JoinResult(c, nickname);
  if (result != protocol::RESULT_OK) {
    return ::testing::AssertionFailure() << nickname << " 입장 실패, 결과 코드 " << result;
  }
  return ::testing::AssertionSuccess();
}

// 채팅을 보내고 ChatResponse를 돌려준다. 그 사이 끼어드는 알림과 전파는 건너뛴다.
protocol::ChatResponse SendChat(BlockingClient& c, const std::string& text) {
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

protocol::HistoryResponse RequestHistory(BlockingClient& c, std::uint32_t limit) {
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
::testing::AssertionResult PingWorks(BlockingClient& c, std::uint64_t request_id = 4242) {
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
::testing::AssertionResult ServerStillWorks(const TestServer& server) {
  BlockingClient c;
  if (!c.Connect("127.0.0.1", server.port())) {
    return ::testing::AssertionFailure() << "새 연결 실패: " << c.last_error();
  }
  return PingWorks(c, 777);
}

// ============================================================ 기본 채팅

TEST(ChatIntegration, TwoUsersExchangeMessagesAndBothReceiveBroadcast) {
  TestServer server;
  auto alice = Connect(server);
  auto bob = Connect(server);
  ASSERT_TRUE(JoinOk(*alice, "alice"));
  ASSERT_TRUE(JoinOk(*bob, "bob"));

  const auto sent = SendChat(*alice, "안녕하세요");
  EXPECT_EQ(sent.result(), protocol::RESULT_OK);
  EXPECT_EQ(sent.message_id(), 1u);

  // 보낸 사람과 다른 사람 모두 같은 전파를 받는다.
  for (BlockingClient* member : {alice.get(), bob.get()}) {
    protocol::ChatBroadcast broadcast;
    ASSERT_TRUE(Receive(*member, MessageId::kChatBroadcast, broadcast));
    EXPECT_EQ(broadcast.entry().message_id(), 1u);
    EXPECT_EQ(broadcast.entry().nickname(), "alice");
    EXPECT_EQ(broadcast.entry().text(), "안녕하세요");
  }

  // 반대 방향도 된다.
  EXPECT_EQ(SendChat(*bob, "반갑습니다").message_id(), 2u);
  protocol::ChatBroadcast reply;
  ASSERT_TRUE(Receive(*alice, MessageId::kChatBroadcast, reply));
  EXPECT_EQ(reply.entry().nickname(), "bob");
  EXPECT_EQ(reply.entry().text(), "반갑습니다");
}

TEST(ChatIntegration, JoinResponseListsCurrentMembers) {
  TestServer server;
  auto alice = Connect(server);
  auto bob = Connect(server);
  ASSERT_TRUE(JoinOk(*alice, "alice"));

  protocol::JoinRequest request;
  request.set_nickname("bob");
  protocol::JoinResponse response;
  ASSERT_TRUE(SendProto(*bob, MessageId::kJoinRequest, request));
  ASSERT_TRUE(Receive(*bob, MessageId::kJoinResponse, response));
  ASSERT_EQ(response.members_size(), 2);
  EXPECT_EQ(response.members(0), "alice");
  EXPECT_EQ(response.members(1), "bob");
}

TEST(ChatIntegration, PresenceNoticesAreSentToOthersOnly) {
  TestServer server;
  auto alice = Connect(server);
  auto bob = Connect(server);
  ASSERT_TRUE(JoinOk(*alice, "alice"));

  ASSERT_TRUE(JoinOk(*bob, "bob"));
  protocol::PresenceNotice joined;
  ASSERT_TRUE(Receive(*alice, MessageId::kPresenceNotice, joined, {}));  // 건너뛰는 종류 없음: 바로 와야 한다
  EXPECT_EQ(joined.kind(), protocol::PresenceNotice::KIND_JOINED);
  EXPECT_EQ(joined.nickname(), "bob");

  protocol::LeaveRequest leave;
  protocol::LeaveResponse leave_response;
  ASSERT_TRUE(SendProto(*bob, MessageId::kLeaveRequest, leave));
  ASSERT_TRUE(Receive(*bob, MessageId::kLeaveResponse, leave_response, {}));  // bob 자신은 알림을 받지 않는다
  EXPECT_EQ(leave_response.result(), protocol::RESULT_OK);

  protocol::PresenceNotice left;
  ASSERT_TRUE(Receive(*alice, MessageId::kPresenceNotice, left, {}));
  EXPECT_EQ(left.kind(), protocol::PresenceNotice::KIND_LEFT);
  EXPECT_EQ(left.nickname(), "bob");
}

TEST(ChatIntegration, DisconnectIsAnnouncedAndNicknameCanBeReused) {
  TestServer server;
  auto alice = Connect(server);
  ASSERT_TRUE(JoinOk(*alice, "alice"));
  {
    auto bob = Connect(server);
    ASSERT_TRUE(JoinOk(*bob, "bob"));
    protocol::PresenceNotice joined;
    ASSERT_TRUE(Receive(*alice, MessageId::kPresenceNotice, joined, {}));
    bob->Close();  // 퇴장 요청 없이 연결만 끊는다
  }

  protocol::PresenceNotice left;
  ASSERT_TRUE(Receive(*alice, MessageId::kPresenceNotice, left, {}));
  EXPECT_EQ(left.kind(), protocol::PresenceNotice::KIND_LEFT);
  EXPECT_EQ(left.nickname(), "bob");
  EXPECT_TRUE(WaitUntil([&] { return Load(server.chat_stats().current_members) == 1; }));

  auto bob_again = Connect(server);
  EXPECT_TRUE(JoinOk(*bob_again, "bob"));  // 끊긴 닉네임을 바로 다시 쓸 수 있다
}

// ============================================================ 업무 규칙 위반: 오류 코드 응답, 연결 유지

TEST(ChatIntegration, JoinRuleViolationsReturnCodesAndKeepTheConnection) {
  TestServer server;
  auto alice = Connect(server);
  auto bob = Connect(server);
  ASSERT_TRUE(JoinOk(*alice, "alice"));

  EXPECT_EQ(JoinResult(*bob, "alice"), protocol::RESULT_NICKNAME_TAKEN);
  EXPECT_EQ(JoinResult(*bob, ""), protocol::RESULT_INVALID_NICKNAME);
  EXPECT_EQ(JoinResult(*bob, std::string(17, 'x')), protocol::RESULT_INVALID_NICKNAME);
  EXPECT_EQ(JoinResult(*bob, " bob"), protocol::RESULT_INVALID_NICKNAME);
  EXPECT_EQ(JoinResult(*bob, "b\nb"), protocol::RESULT_INVALID_NICKNAME);

  // 계속 같은 연결로 올바르게 입장할 수 있어야 한다.
  EXPECT_TRUE(PingWorks(*bob));
  EXPECT_EQ(JoinResult(*bob, "bob"), protocol::RESULT_OK);
  EXPECT_EQ(JoinResult(*bob, "bob2"), protocol::RESULT_ALREADY_JOINED);
  EXPECT_EQ(Load(server.stats().current_connections), 2u);  // 어느 것도 끊기지 않았다
}

TEST(ChatIntegration, RequestsBeforeJoiningAreRejectedButConnectionStays) {
  TestServer server;
  auto stranger = Connect(server);

  EXPECT_EQ(SendChat(*stranger, "hello").result(), protocol::RESULT_NOT_JOINED);
  EXPECT_EQ(RequestHistory(*stranger, 5).result(), protocol::RESULT_NOT_JOINED);

  protocol::LeaveRequest leave;
  protocol::LeaveResponse response;
  ASSERT_TRUE(SendProto(*stranger, MessageId::kLeaveRequest, leave));
  ASSERT_TRUE(Receive(*stranger, MessageId::kLeaveResponse, response));
  EXPECT_EQ(response.result(), protocol::RESULT_NOT_JOINED);

  EXPECT_TRUE(PingWorks(*stranger));  // Ping은 입장 여부와 상관없이 된다
  EXPECT_EQ(server.repository().stored_count(), 0u);
}

TEST(ChatIntegration, InvalidMessagesAreRejectedAndNeverStored) {
  TestServer server;
  auto alice = Connect(server);
  auto bob = Connect(server);
  ASSERT_TRUE(JoinOk(*alice, "alice"));
  ASSERT_TRUE(JoinOk(*bob, "bob"));

  EXPECT_EQ(SendChat(*alice, "").result(), protocol::RESULT_EMPTY_MESSAGE);
  EXPECT_EQ(SendChat(*alice, "    ").result(), protocol::RESULT_EMPTY_MESSAGE);
  EXPECT_EQ(SendChat(*alice, std::string(1001, 'a')).result(), protocol::RESULT_MESSAGE_TOO_LONG);
  EXPECT_EQ(SendChat(*alice, "a\nb").result(), protocol::RESULT_INVALID_ARGUMENT);

  EXPECT_EQ(server.repository().stored_count(), 0u);
  // 거부된 요청 뒤에도 같은 연결에서 정상 채팅이 된다. 그리고 거부된 메시지는 전파되지 않았다.
  EXPECT_EQ(SendChat(*alice, "ok").result(), protocol::RESULT_OK);
  protocol::ChatBroadcast broadcast;
  ASSERT_TRUE(Receive(*bob, MessageId::kChatBroadcast, broadcast));
  EXPECT_EQ(broadcast.entry().text(), "ok");
  EXPECT_EQ(broadcast.entry().message_id(), 1u);
}

// ============================================================ 프로토콜 위반: 연결 종료

TEST(ChatIntegration, MalformedUtf8NicknameClosesTheConnection) {
  TestServer server;
  auto client = Connect(server);

  // JoinRequest { nickname = 0xFF 0xFE }: 필드 1(길이 지정), 길이 2, 잘못된 UTF-8 바이트
  const std::vector<std::uint8_t> body{0x0A, 0x02, 0xFF, 0xFE};
  ASSERT_TRUE(client->SendFrame(protocol::ToUint16(MessageId::kJoinRequest), body));

  EXPECT_TRUE(client->WaitForServerClose(kTimeout));
  EXPECT_TRUE(WaitUntil([&] { return Load(server.stats().parse_errors) == 1; }));
  EXPECT_TRUE(ServerStillWorks(server));
}

class ServerOnlyIdTest : public ::testing::TestWithParam<MessageId> {};

TEST_P(ServerOnlyIdTest, IdsReservedForServerMessagesAreRejectedAsUnknown) {
  TestServer server;
  auto client = Connect(server);

  ASSERT_TRUE(client->SendFrame(protocol::ToUint16(GetParam()), std::vector<std::uint8_t>{}));
  EXPECT_TRUE(client->WaitForServerClose(kTimeout));
  EXPECT_TRUE(WaitUntil([&] { return Load(server.stats().unknown_message_ids) == 1; }));
  EXPECT_TRUE(ServerStillWorks(server));
}

INSTANTIATE_TEST_SUITE_P(Ids, ServerOnlyIdTest,
                         ::testing::Values(MessageId::kPingResponse, MessageId::kJoinResponse,
                                           MessageId::kLeaveResponse, MessageId::kChatResponse,
                                           MessageId::kChatBroadcast, MessageId::kHistoryResponse,
                                           MessageId::kPresenceNotice));

// ============================================================ 순서와 동시성

// 여러 사람이 동시에 보내도 방의 모든 입장자가 같은 순서(message_id 증가)로 받는다.
TEST(ChatIntegration, ConcurrentSendersAllObserveTheSameOrder) {
  constexpr int kSenders = 5;
  constexpr int kPerSender = 20;
  constexpr int kTotal = kSenders * kPerSender;

  TestServer server;
  std::vector<std::unique_ptr<BlockingClient>> clients;
  for (int i = 0; i < kSenders; ++i) {
    clients.push_back(Connect(server));
    ASSERT_TRUE(JoinOk(*clients.back(), "user" + std::to_string(i)));
  }

  struct Observed {
    bool ok = true;
    std::string error;
    std::vector<std::uint64_t> broadcast_ids;  // 받은 전파의 message_id (받은 순서대로)
    std::vector<std::uint64_t> response_ids;   // 내 요청에 대한 응답의 message_id
  };
  std::vector<Observed> observed(kSenders);

  std::vector<std::thread> threads;
  for (int i = 0; i < kSenders; ++i) {
    threads.emplace_back([&, i] {
      BlockingClient& c = *clients[static_cast<std::size_t>(i)];
      Observed& seen = observed[static_cast<std::size_t>(i)];

      // 응답을 기다리지 않고 한꺼번에 보낸 뒤, 응답과 전파를 모두 읽는다.
      for (int j = 0; j < kPerSender; ++j) {
        protocol::ChatRequest request;
        request.set_text("u" + std::to_string(i) + "-" + std::to_string(j));
        if (!SendProto(c, MessageId::kChatRequest, request)) {
          seen.ok = false;
          seen.error = "전송 실패: " + c.last_error();
          return;
        }
      }
      while (seen.response_ids.size() < static_cast<std::size_t>(kPerSender) ||
             seen.broadcast_ids.size() < static_cast<std::size_t>(kTotal)) {
        protocol::Frame frame;
        const Status status = c.ReceiveFrame(frame, Millis(5000));
        if (status != Status::kFrame) {
          seen.ok = false;
          seen.error = "수신 실패(상태 " + std::to_string(static_cast<int>(status)) + ")";
          return;
        }
        if (frame.message_id == protocol::ToUint16(MessageId::kChatResponse)) {
          protocol::ChatResponse response;
          response.ParseFromArray(frame.payload.data(), static_cast<int>(frame.payload.size()));
          if (response.result() != protocol::RESULT_OK) {
            seen.ok = false;
            seen.error = "채팅 응답 실패 " + std::to_string(response.result());
            return;
          }
          seen.response_ids.push_back(response.message_id());
        } else if (frame.message_id == protocol::ToUint16(MessageId::kChatBroadcast)) {
          protocol::ChatBroadcast broadcast;
          broadcast.ParseFromArray(frame.payload.data(), static_cast<int>(frame.payload.size()));
          seen.broadcast_ids.push_back(broadcast.entry().message_id());
        }
        // 입장 알림은 무시한다.
      }
    });
  }
  for (auto& t : threads) t.join();

  std::vector<std::uint64_t> expected(kTotal);
  for (int i = 0; i < kTotal; ++i) expected[static_cast<std::size_t>(i)] = static_cast<std::uint64_t>(i) + 1;

  for (int i = 0; i < kSenders; ++i) {
    const Observed& seen = observed[static_cast<std::size_t>(i)];
    ASSERT_TRUE(seen.ok) << "사용자 " << i << ": " << seen.error;
    EXPECT_EQ(seen.broadcast_ids, expected) << "사용자 " << i << "가 받은 전파 순서가 다름";
    EXPECT_TRUE(std::is_sorted(seen.response_ids.begin(), seen.response_ids.end()))
        << "사용자 " << i << ": 내 요청의 응답 번호가 요청 순서를 따르지 않음";
  }
  EXPECT_EQ(server.repository().stored_count(), static_cast<std::size_t>(kTotal));
}

// ============================================================ 느린 수신자

// 응답을 읽지 않는 입장자 한 명이 한도를 넘어 끊겨도, 다른 입장자는 모든 메시지를 정상으로 받아야 한다.
TEST(ChatIntegration, SlowMemberIsDisconnectedWithoutAffectingOthers) {
  auto config = DefaultServerConfig();
  config.session.max_pending_send_bytes = 100000;
  TestServer server(config);

  auto sender = Connect(server);
  auto reader = Connect(server);
  auto slow = Connect(server);  // 입장만 하고 아무것도 읽지 않는다
  ASSERT_TRUE(JoinOk(*sender, "sender"));
  ASSERT_TRUE(JoinOk(*reader, "reader"));
  ASSERT_TRUE(JoinOk(*slow, "slow"));

  std::atomic<bool> done{false};
  std::vector<std::uint64_t> reader_ids;
  std::thread reader_thread([&] {
    while (true) {
      protocol::Frame frame;
      const Status status = reader->ReceiveFrame(frame, Millis(500));
      if (status == Status::kFrame) {
        if (frame.message_id == protocol::ToUint16(MessageId::kChatBroadcast)) {
          protocol::ChatBroadcast broadcast;
          broadcast.ParseFromArray(frame.payload.data(), static_cast<int>(frame.payload.size()));
          reader_ids.push_back(broadcast.entry().message_id());
        }
      } else if (status == Status::kTimeout) {
        if (done) break;  // 보내는 쪽이 끝났고 더 올 것이 없다
      } else {
        break;
      }
    }
  });

  // slow의 송신 큐가 한도를 넘을 때까지 계속 보낸다. (OS 소켓 버퍼 크기에 의존하지 않도록 횟수가 아니라 결과로 멈춘다)
  const std::string text(900, 'x');
  std::uint64_t accepted = 0;
  for (int i = 0; i < 30000; ++i) {
    if (Load(server.stats().send_queue_overflows) >= 1) break;
    const auto response = SendChat(*sender, text);
    if (response.result() != protocol::RESULT_OK) break;
    ++accepted;
  }
  done = true;
  reader_thread.join();

  EXPECT_TRUE(WaitUntil([&] { return Load(server.stats().send_queue_overflows) == 1; }, Millis(5000)))
      << "느린 수신자가 끊기지 않음 (보낸 메시지 " << accepted << "건)";
  EXPECT_TRUE(slow->WaitForServerClose(kTimeout));

  // 다른 입장자는 영향이 없다: 저장에 성공한 모든 메시지를 빠짐없이, 같은 순서로 받았다.
  ASSERT_EQ(reader_ids.size(), accepted);
  for (std::size_t i = 0; i < reader_ids.size(); ++i) {
    ASSERT_EQ(reader_ids[i], i + 1) << "전파 번호가 연속이 아님: 위치 " << i;
  }
  EXPECT_EQ(Load(server.stats().send_queue_overflows), 1u);  // 끊긴 것은 slow 하나뿐
  EXPECT_TRUE(PingWorks(*sender));
  EXPECT_TRUE(PingWorks(*reader));
}

// ============================================================ 기록 조회

TEST(ChatIntegration, ReconnectedUserCanRecoverRecentHistory) {
  TestServer server;
  {
    auto alice = Connect(server);
    ASSERT_TRUE(JoinOk(*alice, "alice"));
    for (const char* text : {"first", "second", "third"}) {
      ASSERT_EQ(SendChat(*alice, text).result(), protocol::RESULT_OK);
    }
    alice->Close();  // 대화에 참여했던 사람이 나간다
  }
  ASSERT_TRUE(WaitUntil([&] { return Load(server.chat_stats().current_members) == 0; }));

  auto bob = Connect(server);  // 새로 접속한 사람
  ASSERT_TRUE(JoinOk(*bob, "bob"));

  const auto all = RequestHistory(*bob, 0);  // 0은 기본값(20개)
  ASSERT_EQ(all.result(), protocol::RESULT_OK);
  ASSERT_EQ(all.entries_size(), 3);
  EXPECT_EQ(all.entries(0).text(), "first");  // 오래된 것부터
  EXPECT_EQ(all.entries(1).text(), "second");
  EXPECT_EQ(all.entries(2).text(), "third");
  EXPECT_EQ(all.entries(0).nickname(), "alice");
  EXPECT_LT(all.entries(0).message_id(), all.entries(2).message_id());

  const auto last_two = RequestHistory(*bob, 2);
  ASSERT_EQ(last_two.entries_size(), 2);
  EXPECT_EQ(last_two.entries(0).text(), "second");
  EXPECT_EQ(last_two.entries(1).text(), "third");

  EXPECT_EQ(RequestHistory(*bob, 51).result(), protocol::RESULT_INVALID_ARGUMENT);
}

// ============================================================ 저장소 장애

// "DB 중단 중 채팅/기록 조회는 STORAGE_FAILURE, 연결 유지, Ping 정상 -> 복구 후 정상" 시나리오.
TEST(ChatIntegration, StorageOutageIsReportedAndServiceRecoversWithoutRestart) {
  TestServer server;
  auto alice = Connect(server);
  auto bob = Connect(server);
  ASSERT_TRUE(JoinOk(*alice, "alice"));
  ASSERT_TRUE(JoinOk(*bob, "bob"));
  ASSERT_EQ(SendChat(*alice, "before outage").result(), protocol::RESULT_OK);

  server.repository().SetFailing(true);  // 저장소 중단
  EXPECT_EQ(SendChat(*alice, "during outage").result(), protocol::RESULT_STORAGE_FAILURE);
  EXPECT_EQ(RequestHistory(*bob, 5).result(), protocol::RESULT_STORAGE_FAILURE);
  EXPECT_TRUE(PingWorks(*alice));  // 서버와 연결은 그대로다
  EXPECT_TRUE(PingWorks(*bob));
  auto carol = Connect(server);
  EXPECT_TRUE(JoinOk(*carol, "carol"));  // 입장은 저장소가 필요 없어서 계속 된다
  EXPECT_EQ(Load(server.stats().current_connections), 3u);

  server.repository().SetFailing(false);  // 복구
  EXPECT_EQ(SendChat(*alice, "after recovery").result(), protocol::RESULT_OK);
  const auto history = RequestHistory(*bob, 10);
  ASSERT_EQ(history.result(), protocol::RESULT_OK);
  ASSERT_EQ(history.entries_size(), 2);  // 중단 중의 메시지는 저장되지 않았다
  EXPECT_EQ(history.entries(0).text(), "before outage");
  EXPECT_EQ(history.entries(1).text(), "after recovery");
}

TEST(ChatIntegration, SlowStorageTimesOutAndConnectionStaysUsable) {
  auto config = DefaultServerConfig();
  config.chat.storage_timeout = Millis(100);
  TestServer server(config);
  auto alice = Connect(server);
  ASSERT_TRUE(JoinOk(*alice, "alice"));

  server.repository().SetDelay(Millis(400));  // 제한 시간(100ms)보다 느린 저장소
  const auto started = std::chrono::steady_clock::now();
  EXPECT_EQ(SendChat(*alice, "too slow").result(), protocol::RESULT_STORAGE_FAILURE);
  const auto elapsed = std::chrono::duration_cast<Millis>(std::chrono::steady_clock::now() - started);
  EXPECT_LT(elapsed.count(), 350) << "저장 완료(400ms)를 기다리지 않고 시간 초과로 응답해야 한다";
  EXPECT_EQ(Load(server.chat_stats().storage_timeouts), 1u);

  server.repository().SetDelay(Millis(0));
  EXPECT_TRUE(PingWorks(*alice));
  EXPECT_EQ(SendChat(*alice, "fast again").result(), protocol::RESULT_OK);
}

}  // namespace
}  // namespace game
