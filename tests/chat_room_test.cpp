// ChatRoom 단위 테스트. 소켓 없이 가짜 연결(FakeClient)과 장애/지연을 주입할 수 있는 인메모리 저장소로
// 방 로직만 검증한다.
//
// 비동기 완료를 다루는 방법
//  - 지연이 없는 저장소: 완료가 io_context에 post 되므로 Drain()으로 처리할 일이 없어질 때까지 돌린다.
//  - 지연이나 시간 초과가 있는 시험: RunFor(시간)으로 실제 시간을 흘려 보낸다.
//  테스트가 하나의 스레드에서 io_context를 직접 돌리므로 서버와 달리 스레드 경쟁이 없고, 결과가 결정적이다.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "chat/chat_room.h"
#include "chat/in_memory_chat_repository.h"

namespace game::chat {
namespace {

using protocol::MessageId;
using Millis = std::chrono::milliseconds;

// 방이 보낸 메시지를 받아 적어 두는 가짜 연결.
class FakeClient : public ChatClient {
 public:
  explicit FakeClient(std::uint64_t id) : id_(id) {}

  std::uint64_t client_id() const override { return id_; }

  void Deliver(MessageId id, const google::protobuf::MessageLite& message) override {
    received.emplace_back(id, message.SerializeAsString());
    if (on_deliver) {
      on_deliver(id);
    }
  }

  // 받은 메시지 중 종류가 id인 것의 개수.
  std::size_t CountOf(MessageId id) const {
    std::size_t count = 0;
    for (const auto& entry : received) {
      if (entry.first == id) ++count;
    }
    return count;
  }

  // 받은 메시지 중 종류가 id인 것의 n번째(0부터)를 T로 해석해 돌려준다.
  template <typename T>
  T Nth(MessageId id, std::size_t n = 0) const {
    std::size_t seen = 0;
    for (const auto& entry : received) {
      if (entry.first == id && seen++ == n) {
        T message;
        EXPECT_TRUE(message.ParseFromString(entry.second));
        return message;
      }
    }
    ADD_FAILURE() << "메시지 ID " << static_cast<int>(id) << "의 " << n << "번째가 없음";
    return T{};
  }

  // 받은 메시지의 종류만 순서대로 나열한다. 순서를 검사할 때 쓴다.
  std::vector<MessageId> Kinds() const {
    std::vector<MessageId> kinds;
    for (const auto& entry : received) kinds.push_back(entry.first);
    return kinds;
  }

  std::vector<std::pair<MessageId, std::string>> received;
  std::function<void(MessageId)> on_deliver;  // 메시지를 받는 순간 실행할 동작 (재진입 시험용)

 private:
  std::uint64_t id_;
};

class ChatRoomTest : public ::testing::Test {
 protected:
  ChatRoomTest() { Reset(ChatRoomOptions{}); }

  // 새 옵션으로 방을 다시 만든다. (같은 저장소를 쓴다)
  void Reset(ChatRoomOptions options) {
    repo = std::make_shared<InMemoryChatRepository>(io);
    room = std::make_unique<ChatRoom>(io, repo, options);
  }

  // 연결을 만들고 입장 요청을 보낸 뒤 그 연결을 돌려준다.
  std::shared_ptr<FakeClient> Join(std::uint64_t id, const std::string& nickname) {
    auto client = std::make_shared<FakeClient>(id);
    protocol::JoinRequest request;
    request.set_nickname(nickname);
    room->HandleJoin(client, request);
    return client;
  }

  void Chat(const std::shared_ptr<FakeClient>& client, const std::string& text) {
    protocol::ChatRequest request;
    request.set_text(text);
    room->HandleChat(client, request);
  }

  void History(const std::shared_ptr<FakeClient>& client, std::uint32_t limit) {
    protocol::HistoryRequest request;
    request.set_limit(limit);
    room->HandleHistory(client, request);
  }

  // 지금 실행할 수 있는 비동기 작업을 모두 처리한다. 지연(타이머)이 걸린 작업은 기다리지 않는다.
  void Drain() {
    io.restart();
    while (io.poll() > 0) {
    }
  }

  // 실제 시간을 흘려 보내며 비동기 작업을 처리한다. 할 일이 없어지면 일찍 끝난다.
  void RunFor(Millis duration) {
    io.restart();
    io.run_for(duration);
  }

  // 선언 순서가 곧 생성 순서다. 소멸은 반대 순서라 방 -> 저장소 -> io_context 순으로 정리된다.
  boost::asio::io_context io;
  std::shared_ptr<InMemoryChatRepository> repo;
  std::unique_ptr<ChatRoom> room;
};

// ============================================================ 입장

TEST_F(ChatRoomTest, JoinSucceedsAndReturnsMemberList) {
  auto alice = Join(1, "alice");

  const auto response = alice->Nth<protocol::JoinResponse>(MessageId::kJoinResponse);
  EXPECT_EQ(response.result(), protocol::RESULT_OK);
  ASSERT_EQ(response.members_size(), 1);
  EXPECT_EQ(response.members(0), "alice");
  EXPECT_EQ(room->stats().current_members.load(), 1u);
}

TEST_F(ChatRoomTest, SecondJoinerSeesBothAndFirstIsNotified) {
  auto alice = Join(1, "alice");
  auto bob = Join(2, "bob");

  const auto response = bob->Nth<protocol::JoinResponse>(MessageId::kJoinResponse);
  ASSERT_EQ(response.members_size(), 2);
  EXPECT_EQ(response.members(0), "alice");  // 입장한 순서대로
  EXPECT_EQ(response.members(1), "bob");

  // 먼저 있던 사람만 입장 알림을 받는다. 본인에게는 가지 않는다.
  ASSERT_EQ(alice->CountOf(MessageId::kPresenceNotice), 1u);
  const auto notice = alice->Nth<protocol::PresenceNotice>(MessageId::kPresenceNotice);
  EXPECT_EQ(notice.kind(), protocol::PresenceNotice::KIND_JOINED);
  EXPECT_EQ(notice.nickname(), "bob");
  EXPECT_EQ(bob->CountOf(MessageId::kPresenceNotice), 0u);
}

TEST_F(ChatRoomTest, JoinRulesAreEnforcedWithoutDisconnecting) {
  auto alice = Join(1, "alice");

  auto bob = Join(2, "alice");  // 닉네임 중복
  EXPECT_EQ(bob->Nth<protocol::JoinResponse>(MessageId::kJoinResponse).result(), protocol::RESULT_NICKNAME_TAKEN);

  auto carol = Join(3, "");  // 규칙 위반
  EXPECT_EQ(carol->Nth<protocol::JoinResponse>(MessageId::kJoinResponse).result(), protocol::RESULT_INVALID_NICKNAME);

  auto dave = Join(4, std::string(17, 'x'));
  EXPECT_EQ(dave->Nth<protocol::JoinResponse>(MessageId::kJoinResponse).result(), protocol::RESULT_INVALID_NICKNAME);

  // 이미 입장한 연결이 다시 입장 요청
  protocol::JoinRequest again;
  again.set_nickname("alice2");
  room->HandleJoin(alice, again);
  EXPECT_EQ(alice->Nth<protocol::JoinResponse>(MessageId::kJoinResponse, 1).result(), protocol::RESULT_ALREADY_JOINED);

  // 실패한 입장은 방 상태를 바꾸지 않는다.
  EXPECT_EQ(room->stats().current_members.load(), 1u);
  EXPECT_EQ(alice->CountOf(MessageId::kPresenceNotice), 0u);
  EXPECT_EQ(room->stats().requests_rejected.load(), 4u);
}

// ============================================================ 퇴장 / 연결 끊김

TEST_F(ChatRoomTest, LeaveNotifiesOthersAndFreesTheNickname) {
  auto alice = Join(1, "alice");
  auto bob = Join(2, "bob");

  room->HandleLeave(bob);
  EXPECT_EQ(bob->Nth<protocol::LeaveResponse>(MessageId::kLeaveResponse).result(), protocol::RESULT_OK);

  ASSERT_EQ(alice->CountOf(MessageId::kPresenceNotice), 2u);  // bob 입장, bob 퇴장
  const auto left = alice->Nth<protocol::PresenceNotice>(MessageId::kPresenceNotice, 1);
  EXPECT_EQ(left.kind(), protocol::PresenceNotice::KIND_LEFT);
  EXPECT_EQ(left.nickname(), "bob");
  EXPECT_EQ(room->stats().current_members.load(), 1u);

  // 퇴장하면 같은 닉네임을 바로 다시 쓸 수 있다.
  auto bob_again = Join(3, "bob");
  EXPECT_EQ(bob_again->Nth<protocol::JoinResponse>(MessageId::kJoinResponse).result(), protocol::RESULT_OK);
}

TEST_F(ChatRoomTest, LeaveWithoutJoiningIsRejected) {
  auto stranger = std::make_shared<FakeClient>(9);
  room->HandleLeave(stranger);
  EXPECT_EQ(stranger->Nth<protocol::LeaveResponse>(MessageId::kLeaveResponse).result(), protocol::RESULT_NOT_JOINED);
}

TEST_F(ChatRoomTest, DisconnectActsLikeLeave) {
  auto alice = Join(1, "alice");
  auto bob = Join(2, "bob");

  room->OnDisconnect(2);  // 연결이 끊김

  const auto left = alice->Nth<protocol::PresenceNotice>(MessageId::kPresenceNotice, 1);
  EXPECT_EQ(left.kind(), protocol::PresenceNotice::KIND_LEFT);
  EXPECT_EQ(left.nickname(), "bob");
  EXPECT_EQ(room->stats().current_members.load(), 1u);

  room->OnDisconnect(2);  // 두 번 불러도, 입장한 적 없는 연결이어도 아무 일이 없어야 한다.
  room->OnDisconnect(99);
  EXPECT_EQ(alice->CountOf(MessageId::kPresenceNotice), 2u);
}

// ============================================================ 채팅: 거부

TEST_F(ChatRoomTest, ChatBeforeJoinIsRejected) {
  auto stranger = std::make_shared<FakeClient>(9);
  Chat(stranger, "hello");
  Drain();

  EXPECT_EQ(stranger->Nth<protocol::ChatResponse>(MessageId::kChatResponse).result(), protocol::RESULT_NOT_JOINED);
  EXPECT_EQ(repo->stored_count(), 0u);
}

TEST_F(ChatRoomTest, InvalidMessagesAreRejectedAndNeverStored) {
  auto alice = Join(1, "alice");
  auto bob = Join(2, "bob");

  Chat(alice, "");
  Chat(alice, "   ");
  Chat(alice, std::string(1001, 'a'));
  Chat(alice, "line1\nline2");
  Drain();

  EXPECT_EQ(alice->Nth<protocol::ChatResponse>(MessageId::kChatResponse, 0).result(), protocol::RESULT_EMPTY_MESSAGE);
  EXPECT_EQ(alice->Nth<protocol::ChatResponse>(MessageId::kChatResponse, 1).result(), protocol::RESULT_EMPTY_MESSAGE);
  EXPECT_EQ(alice->Nth<protocol::ChatResponse>(MessageId::kChatResponse, 2).result(), protocol::RESULT_MESSAGE_TOO_LONG);
  EXPECT_EQ(alice->Nth<protocol::ChatResponse>(MessageId::kChatResponse, 3).result(), protocol::RESULT_INVALID_ARGUMENT);
  EXPECT_EQ(repo->stored_count(), 0u);
  EXPECT_EQ(bob->CountOf(MessageId::kChatBroadcast), 0u);  // 거부된 메시지는 전파되지 않는다
}

// ============================================================ 채팅: 저장 후 전파

TEST_F(ChatRoomTest, SavedMessageIsAcknowledgedThenBroadcastToEveryone) {
  auto alice = Join(1, "alice");
  auto bob = Join(2, "bob");

  Chat(alice, "안녕하세요");
  Drain();

  // 보낸 사람: 결과 응답이 먼저, 그다음 전파된 메시지(자신 포함)
  const auto response = alice->Nth<protocol::ChatResponse>(MessageId::kChatResponse);
  EXPECT_EQ(response.result(), protocol::RESULT_OK);
  EXPECT_EQ(response.message_id(), 1u);

  const auto kinds = alice->Kinds();
  const auto response_pos = std::find(kinds.begin(), kinds.end(), MessageId::kChatResponse) - kinds.begin();
  const auto broadcast_pos = std::find(kinds.begin(), kinds.end(), MessageId::kChatBroadcast) - kinds.begin();
  ASSERT_LT(response_pos, static_cast<std::ptrdiff_t>(kinds.size()));
  ASSERT_LT(broadcast_pos, static_cast<std::ptrdiff_t>(kinds.size()));
  EXPECT_LT(response_pos, broadcast_pos);  // 결과 응답이 전파보다 먼저 도착한다

  // 모든 입장자가 같은 내용을 받는다.
  for (const auto& member : {alice, bob}) {
    ASSERT_EQ(member->CountOf(MessageId::kChatBroadcast), 1u);
    const auto entry = member->Nth<protocol::ChatBroadcast>(MessageId::kChatBroadcast).entry();
    EXPECT_EQ(entry.message_id(), 1u);
    EXPECT_EQ(entry.nickname(), "alice");
    EXPECT_EQ(entry.text(), "안녕하세요");
    EXPECT_GT(entry.sent_at_unix_ms(), 0);
  }
  EXPECT_EQ(bob->CountOf(MessageId::kChatResponse), 0u);  // 응답은 보낸 사람에게만 간다
  EXPECT_EQ(repo->stored_count(), 1u);
  EXPECT_EQ(room->stats().messages_saved.load(), 1u);
}

// 앞 메시지의 저장이 더 오래 걸려도 전파 순서는 도착 순서와 message_id 순서를 따라야 한다.
// 저장을 병렬로 했다면 뒤 메시지가 먼저 끝나 순서가 뒤바뀐다. 방 단위 직렬화를 검증한다.
TEST_F(ChatRoomTest, BroadcastOrderFollowsSubmissionEvenWhenEarlierSaveIsSlower) {
  auto alice = Join(1, "alice");
  auto bob = Join(2, "bob");

  repo->SetDelay(Millis(80));
  Chat(alice, "first");   // 저장에 80ms
  repo->SetDelay(Millis(5));
  Chat(bob, "second");    // 저장에 5ms: 병렬이라면 이쪽이 먼저 끝난다
  RunFor(Millis(600));

  for (const auto& member : {alice, bob}) {
    ASSERT_EQ(member->CountOf(MessageId::kChatBroadcast), 2u);
    const auto a = member->Nth<protocol::ChatBroadcast>(MessageId::kChatBroadcast, 0).entry();
    const auto b = member->Nth<protocol::ChatBroadcast>(MessageId::kChatBroadcast, 1).entry();
    EXPECT_EQ(a.text(), "first");
    EXPECT_EQ(b.text(), "second");
    EXPECT_LT(a.message_id(), b.message_id());
  }
}

TEST_F(ChatRoomTest, MessageFromADisconnectedSenderIsStillSavedAndBroadcast) {
  auto alice = Join(1, "alice");
  auto bob = Join(2, "bob");

  repo->SetDelay(Millis(50));
  Chat(alice, "bye");
  room->OnDisconnect(1);  // 저장이 끝나기 전에 보낸 사람이 나간다
  RunFor(Millis(300));

  EXPECT_EQ(repo->stored_count(), 1u);
  ASSERT_EQ(bob->CountOf(MessageId::kChatBroadcast), 1u);
  EXPECT_EQ(bob->Nth<protocol::ChatBroadcast>(MessageId::kChatBroadcast).entry().text(), "bye");
  EXPECT_EQ(alice->CountOf(MessageId::kChatBroadcast), 0u);  // 나간 사람에게는 가지 않는다
}

// ============================================================ 저장 실패 / 시간 초과 / 혼잡

TEST_F(ChatRoomTest, StorageFailureIsReportedAndNothingIsBroadcast) {
  auto alice = Join(1, "alice");
  auto bob = Join(2, "bob");

  repo->SetFailing(true);
  Chat(alice, "will fail");
  Drain();

  EXPECT_EQ(alice->Nth<protocol::ChatResponse>(MessageId::kChatResponse).result(), protocol::RESULT_STORAGE_FAILURE);
  EXPECT_EQ(alice->CountOf(MessageId::kChatBroadcast), 0u);
  EXPECT_EQ(bob->CountOf(MessageId::kChatBroadcast), 0u);
  EXPECT_EQ(repo->stored_count(), 0u);
  EXPECT_EQ(room->stats().storage_failures.load(), 1u);
}

TEST_F(ChatRoomTest, RoomRecoversWhenStorageComesBack) {
  auto alice = Join(1, "alice");

  repo->SetFailing(true);
  Chat(alice, "fail 1");
  Chat(alice, "fail 2");
  Drain();
  repo->SetFailing(false);  // 저장소 복구
  Chat(alice, "works again");
  Drain();

  EXPECT_EQ(alice->Nth<protocol::ChatResponse>(MessageId::kChatResponse, 0).result(), protocol::RESULT_STORAGE_FAILURE);
  EXPECT_EQ(alice->Nth<protocol::ChatResponse>(MessageId::kChatResponse, 1).result(), protocol::RESULT_STORAGE_FAILURE);
  EXPECT_EQ(alice->Nth<protocol::ChatResponse>(MessageId::kChatResponse, 2).result(), protocol::RESULT_OK);
  EXPECT_EQ(alice->CountOf(MessageId::kChatBroadcast), 1u);
}

TEST_F(ChatRoomTest, SlowStorageTimesOutAndLateResultIsDiscarded) {
  ChatRoomOptions options;
  options.storage_timeout = Millis(50);
  Reset(options);
  auto alice = Join(1, "alice");
  auto bob = Join(2, "bob");

  repo->SetDelay(Millis(250));
  Chat(alice, "slow");
  RunFor(Millis(120));  // 제한 시간(50ms)은 지났고 저장(250ms)은 아직 끝나지 않은 시점

  EXPECT_EQ(alice->Nth<protocol::ChatResponse>(MessageId::kChatResponse).result(), protocol::RESULT_STORAGE_FAILURE);
  EXPECT_EQ(room->stats().storage_timeouts.load(), 1u);

  // 방은 계속 쓸 수 있어야 한다. 저장소가 빨라지면 다음 채팅은 정상 처리된다.
  repo->SetDelay(Millis(0));
  Chat(alice, "after timeout");
  RunFor(Millis(30));
  EXPECT_EQ(alice->Nth<protocol::ChatResponse>(MessageId::kChatResponse, 1).result(), protocol::RESULT_OK);

  // 뒤늦게 끝난 첫 저장 결과는 버려진다: 전파되지 않는다. 다만 저장소에는 반영될 수 있다(결과 불명).
  RunFor(Millis(500));
  ASSERT_EQ(bob->CountOf(MessageId::kChatBroadcast), 1u);
  EXPECT_EQ(bob->Nth<protocol::ChatBroadcast>(MessageId::kChatBroadcast).entry().text(), "after timeout");
  EXPECT_EQ(repo->stored_count(), 2u);  // "slow"도 결국 저장되었다
  EXPECT_EQ(alice->CountOf(MessageId::kChatResponse), 2u);  // 늦은 결과로 응답이 또 오지 않는다
}

TEST_F(ChatRoomTest, FullPendingQueueRejectsWithServerBusy) {
  ChatRoomOptions options;
  options.max_pending_chats = 2;
  Reset(options);
  auto alice = Join(1, "alice");

  repo->SetDelay(Millis(100));
  Chat(alice, "1");  // 저장 중 (대기열에서 꺼내져 진행 중)
  Chat(alice, "2");  // 대기 1
  Chat(alice, "3");  // 대기 2
  Chat(alice, "4");  // 대기열이 가득 참

  // 거부 응답은 저장을 기다리지 않고 즉시 간다.
  ASSERT_EQ(alice->CountOf(MessageId::kChatResponse), 1u);
  EXPECT_EQ(alice->Nth<protocol::ChatResponse>(MessageId::kChatResponse).result(), protocol::RESULT_SERVER_BUSY);
  EXPECT_EQ(room->stats().busy_rejections.load(), 1u);

  RunFor(Millis(1000));
  EXPECT_EQ(repo->stored_count(), 3u);  // 앞의 세 건은 정상 처리된다
  EXPECT_EQ(room->stats().max_pending_depth.load(), 2u);
}

// ============================================================ 기록 조회

TEST_F(ChatRoomTest, HistoryReturnsRecentMessagesOldestFirst) {
  auto alice = Join(1, "alice");
  for (const char* text : {"m1", "m2", "m3"}) Chat(alice, text);
  Drain();

  History(alice, 2);
  Drain();

  const auto response = alice->Nth<protocol::HistoryResponse>(MessageId::kHistoryResponse);
  EXPECT_EQ(response.result(), protocol::RESULT_OK);
  ASSERT_EQ(response.entries_size(), 2);
  EXPECT_EQ(response.entries(0).text(), "m2");  // 최근 2개, 오래된 것부터
  EXPECT_EQ(response.entries(1).text(), "m3");
  EXPECT_LT(response.entries(0).message_id(), response.entries(1).message_id());
}

TEST_F(ChatRoomTest, HistoryLimitZeroUsesDefaultAndTooLargeIsRejected) {
  auto alice = Join(1, "alice");
  for (int i = 0; i < 25; ++i) Chat(alice, "m" + std::to_string(i));
  Drain();

  History(alice, 0);  // 기본 20개
  History(alice, 50);
  Drain();
  EXPECT_EQ(alice->Nth<protocol::HistoryResponse>(MessageId::kHistoryResponse, 0).entries_size(), 20);
  EXPECT_EQ(alice->Nth<protocol::HistoryResponse>(MessageId::kHistoryResponse, 1).entries_size(), 25);

  // 범위를 벗어난 요청은 저장소를 거치지 않고 즉시 거부한다. (Drain 없이 바로 응답이 와 있다)
  History(alice, 51);
  EXPECT_EQ(alice->Nth<protocol::HistoryResponse>(MessageId::kHistoryResponse, 2).result(), protocol::RESULT_INVALID_ARGUMENT);
}

TEST_F(ChatRoomTest, HistoryRequiresJoinAndReportsStorageFailure) {
  auto stranger = std::make_shared<FakeClient>(9);
  History(stranger, 5);
  EXPECT_EQ(stranger->Nth<protocol::HistoryResponse>(MessageId::kHistoryResponse).result(), protocol::RESULT_NOT_JOINED);

  auto alice = Join(1, "alice");
  repo->SetFailing(true);
  History(alice, 5);
  Drain();
  const auto response = alice->Nth<protocol::HistoryResponse>(MessageId::kHistoryResponse);
  EXPECT_EQ(response.result(), protocol::RESULT_STORAGE_FAILURE);
  EXPECT_EQ(response.entries_size(), 0);
}

TEST_F(ChatRoomTest, HistoryTimesOut) {
  ChatRoomOptions options;
  options.storage_timeout = Millis(40);
  Reset(options);
  auto alice = Join(1, "alice");

  repo->SetDelay(Millis(300));
  History(alice, 5);
  RunFor(Millis(100));
  EXPECT_EQ(alice->Nth<protocol::HistoryResponse>(MessageId::kHistoryResponse).result(), protocol::RESULT_STORAGE_FAILURE);

  RunFor(Millis(500));  // 늦게 도착한 조회 결과로 응답이 또 오면 안 된다
  EXPECT_EQ(alice->CountOf(MessageId::kHistoryResponse), 1u);
}

// 기록 응답이 프레임 최대 크기를 넘으면 가장 오래된 항목부터 덜어내야 한다.
TEST_F(ChatRoomTest, HistoryResponseIsTrimmedToFitOneFrame) {
  ChatRoomOptions options;
  options.max_history_limit = 200;  // 기본 50개 제한으로는 크기 초과를 만들 수 없어서 상한을 높인다
  Reset(options);
  auto alice = Join(1, "alice");
  const std::string text(1000, 'x');  // 항목 하나가 약 1KB
  for (int i = 0; i < 100; ++i) Chat(alice, text);
  Drain();

  History(alice, 100);  // 100개 x 약 1KB는 65,534바이트를 넘는다
  Drain();

  const auto response = alice->Nth<protocol::HistoryResponse>(MessageId::kHistoryResponse);
  EXPECT_EQ(response.result(), protocol::RESULT_OK);
  EXPECT_GT(response.entries_size(), 0);
  EXPECT_LT(response.entries_size(), 100);
  EXPECT_LE(response.ByteSizeLong(), 65534u);
  // 가장 최근 항목은 남아 있다.
  EXPECT_EQ(response.entries(response.entries_size() - 1).message_id(), 100u);
}

// ============================================================ 견고성

// 전파 도중 어떤 연결이 닫혀(송신 큐 초과 등) 방에서 빠져도 순회가 깨지지 않아야 한다.
TEST_F(ChatRoomTest, DisconnectDuringBroadcastDoesNotBreakDelivery) {
  auto alice = Join(1, "alice");
  auto bob = Join(2, "bob");
  auto carol = Join(3, "carol");
  bob->on_deliver = [this](MessageId id) {
    if (id == MessageId::kChatBroadcast) {
      room->OnDisconnect(2);  // 메시지를 받는 순간 bob의 연결이 닫히는 상황
    }
  };

  Chat(alice, "hello");
  Drain();

  EXPECT_EQ(alice->CountOf(MessageId::kChatBroadcast), 1u);
  EXPECT_EQ(bob->CountOf(MessageId::kChatBroadcast), 1u);
  EXPECT_EQ(carol->CountOf(MessageId::kChatBroadcast), 1u);  // bob 뒤의 수신자도 빠짐없이 받는다
  EXPECT_EQ(room->stats().current_members.load(), 2u);
}

TEST_F(ChatRoomTest, ExpiredClientIsSkippedWithoutCrashing) {
  auto alice = Join(1, "alice");
  auto ghost = Join(2, "ghost");
  ghost.reset();  // OnDisconnect 없이 연결 객체가 사라진 상황

  Chat(alice, "anyone there?");
  Drain();

  EXPECT_EQ(alice->CountOf(MessageId::kChatBroadcast), 1u);
}

// 서버 종료 시 진행 중인 저장 대기가 종료를 붙잡거나, 종료 뒤에 메시지를 보내면 안 된다.
TEST_F(ChatRoomTest, ShutdownCancelsPendingWorkQuickly) {
  ChatRoomOptions options;
  options.storage_timeout = Millis(5000);  // 취소되지 않으면 run()이 5초를 기다리게 된다
  Reset(options);
  auto alice = Join(1, "alice");
  repo->SetDelay(Millis(200));

  Chat(alice, "pending");
  Chat(alice, "queued");
  room->Shutdown();

  const auto started = std::chrono::steady_clock::now();
  io.restart();
  io.run();  // 저장소의 지연 타이머(200ms)가 끝나면 할 일이 없어 반환해야 한다
  const auto elapsed = std::chrono::duration_cast<Millis>(std::chrono::steady_clock::now() - started);

  EXPECT_LT(elapsed.count(), 2000);
  EXPECT_EQ(alice->CountOf(MessageId::kChatResponse), 0u);
  EXPECT_EQ(alice->CountOf(MessageId::kChatBroadcast), 0u);
}

}  // namespace
}  // namespace game::chat
