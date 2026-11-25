// GameService 단위 테스트: 소켓 없이 가짜 연결(FakeClient)로 방 만들기, 입장, 나가기, 방장 변경, 이동,
// 틱, 틱 중 연결 종료를 검증한다. 틱은 Tick()을 직접 불러 시간 없이 결정적으로 돌린다.
// 타이머로 도는 틱만 StartTicking()으로 따로 확인한다.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "play/game_service.h"

namespace game::play {
namespace {

using protocol::MessageId;

// 방이 보낸 메시지를 받아 적어 두는 가짜 연결.
class FakeClient : public GameClient {
 public:
  explicit FakeClient(std::uint64_t id) : id_(id) {}

  std::uint64_t client_id() const override { return id_; }

  void Deliver(MessageId id, const google::protobuf::MessageLite& message) override {
    received.emplace_back(id, message.SerializeAsString());
    if (on_deliver) on_deliver(id);
  }

  std::size_t CountOf(MessageId id) const {
    std::size_t count = 0;
    for (const auto& entry : received) {
      if (entry.first == id) ++count;
    }
    return count;
  }

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
    ADD_FAILURE() << "message " << static_cast<int>(id) << " #" << n << " not found";
    return T();
  }

  template <typename T>
  T Last(MessageId id) const {
    const std::size_t count = CountOf(id);
    if (count == 0) {
      ADD_FAILURE() << "no message " << static_cast<int>(id);
      return T();
    }
    return Nth<T>(id, count - 1);
  }

  // 받은 RoomEvent 종류들을 순서대로.
  std::vector<protocol::RoomEvent::Kind> EventKinds() const {
    std::vector<protocol::RoomEvent::Kind> kinds;
    for (std::size_t i = 0; i < CountOf(MessageId::kRoomEvent); ++i) {
      kinds.push_back(Nth<protocol::RoomEvent>(MessageId::kRoomEvent, i).kind());
    }
    return kinds;
  }

  std::function<void(MessageId)> on_deliver;
  std::vector<std::pair<MessageId, std::string>> received;

 private:
  const std::uint64_t id_;
};

using ClientPtr = std::shared_ptr<FakeClient>;

class GameServiceTest : public ::testing::Test {
 protected:
  explicit GameServiceTest(PlayConfig config = {}) : service_(io_, config) {}

  ClientPtr NewClient(std::uint64_t id) {
    auto client = std::make_shared<FakeClient>(id);
    clients_[id] = client;
    return client;
  }

  // 방을 만들고 응답을 돌려준다.
  protocol::CreateRoomResponse Create(const ClientPtr& client, const std::string& nickname) {
    protocol::CreateRoomRequest request;
    request.set_nickname(nickname);
    service_.HandleCreateRoom(client, request);
    return client->Last<protocol::CreateRoomResponse>(MessageId::kCreateRoomResponse);
  }

  protocol::JoinRoomResponse Join(const ClientPtr& client, const std::string& code, const std::string& nickname) {
    protocol::JoinRoomRequest request;
    request.set_room_code(code);
    request.set_nickname(nickname);
    service_.HandleJoinRoom(client, request);
    return client->Last<protocol::JoinRoomResponse>(MessageId::kJoinRoomResponse);
  }

  protocol::LeaveRoomResponse Leave(const ClientPtr& client) {
    service_.HandleLeaveRoom(client);
    return client->Last<protocol::LeaveRoomResponse>(MessageId::kLeaveRoomResponse);
  }

  protocol::SleepResponse Sleep(const ClientPtr& client, bool sleep) {
    protocol::SleepRequest request;
    request.set_sleep(sleep);
    service_.HandleSleep(client, request);
    return client->Last<protocol::SleepResponse>(MessageId::kSleepResponse);
  }

  // 칸 0, 1(시작 집에서 300 cm)의 사람을 시작 집 앞으로 걷게 한다: 9틱(162 cm) 북쪽으로 가서 멈춘다.
  // 입력 번호는 seq, seq + 1을 쓴다.
  void WalkToFarmhouse(const std::vector<ClientPtr>& walkers, std::uint32_t seq = 1000) {
    for (const auto& client : walkers) Move(client, seq, 0.0f, 1.0f);
    for (int i = 0; i < 9; ++i) service_.Tick();
    for (const auto& client : walkers) Move(client, seq + 1, 0.0f, 0.0f);
  }

  void Move(const ClientPtr& client, std::uint32_t seq, float x, float y) {
    protocol::MoveInput input;
    input.set_sequence(seq);
    input.set_dir_x(x);
    input.set_dir_y(y);
    service_.HandleMoveInput(client, input);
  }

  // 방 코드 방을 만든 호스트와 그 방에 들어간 사람들을 한 번에 준비한다.
  std::string CreateRoomWith(std::uint64_t host_id, const std::string& host_nick) {
    auto host = NewClient(host_id);
    return Create(host, host_nick).state().room_code();
  }

  boost::asio::io_context io_;
  GameService service_;
  std::map<std::uint64_t, ClientPtr> clients_;
};

// ============================================================ 방 만들기

TEST_F(GameServiceTest, CreateRoomReturnsASixDigitCodeAndTheState) {
  auto alice = NewClient(1);
  const auto response = Create(alice, "alice");
  ASSERT_EQ(response.result(), protocol::GAME_OK);
  const auto& state = response.state();
  ASSERT_EQ(state.room_code().size(), 6u);
  EXPECT_TRUE(std::all_of(state.room_code().begin(), state.room_code().end(),
                          [](char c) { return c >= '0' && c <= '9'; }));
  EXPECT_EQ(state.your_player_id(), 1u);
  EXPECT_EQ(state.host_player_id(), 1u);
  ASSERT_EQ(state.players_size(), 1);
  EXPECT_EQ(state.players(0).nickname(), "alice");
  EXPECT_EQ(service_.room_count(), 1u);
  EXPECT_EQ(service_.RoomCodeOf(1), state.room_code());
}

TEST_F(GameServiceTest, CreateRoomRejectsInvalidNicknamesAndCreatesNothing) {
  auto client = NewClient(1);
  for (const char* nickname : {"", " alice", "alice ", "line\nbreak", "01234567890123456"}) {
    const auto response = Create(client, nickname);
    EXPECT_EQ(response.result(), protocol::GAME_INVALID_NICKNAME) << "[" << nickname << "]";
    EXPECT_FALSE(response.has_state());
  }
  EXPECT_EQ(service_.room_count(), 0u);
  EXPECT_EQ(service_.RoomCodeOf(1), "");
}

TEST_F(GameServiceTest, CannotCreateOrJoinWhileAlreadyInARoom) {
  const std::string code = CreateRoomWith(1, "alice");
  auto alice = clients_[1];
  EXPECT_EQ(Create(alice, "alice2").result(), protocol::GAME_ALREADY_IN_ROOM);

  auto bob = NewClient(2);
  const std::string other = Create(bob, "bob").state().room_code();
  EXPECT_EQ(Join(bob, code, "bob").result(), protocol::GAME_ALREADY_IN_ROOM);
  EXPECT_EQ(service_.room_count(), 2u);
  EXPECT_EQ(service_.RoomCodeOf(2), other);  // 원래 방에 그대로 있다
}

TEST_F(GameServiceTest, DefaultGeneratorMakesUniqueSixDigitCodes) {
  std::set<std::string> codes;
  for (std::uint64_t id = 1; id <= 300; ++id) {
    auto client = NewClient(id);
    const auto response = Create(client, "p" + std::to_string(id));
    ASSERT_EQ(response.result(), protocol::GAME_OK);
    const std::string& code = response.state().room_code();
    ASSERT_EQ(code.size(), 6u) << code;
    ASSERT_TRUE(std::all_of(code.begin(), code.end(), [](char c) { return c >= '0' && c <= '9'; })) << code;
    codes.insert(code);
  }
  EXPECT_EQ(codes.size(), 300u);  // 모두 다르다
}

// ============================================================ 방 코드 충돌

TEST(GameServiceCodes, CollidingCodesAreRedrawn) {
  boost::asio::io_context io;
  std::vector<std::string> sequence = {"123456", "123456", "123456", "654321"};
  std::size_t next = 0;
  GameService service(io, PlayConfig{}, [&]() { return sequence[std::min(next++, sequence.size() - 1)]; });

  auto a = std::make_shared<FakeClient>(1);
  auto b = std::make_shared<FakeClient>(2);
  protocol::CreateRoomRequest request;
  request.set_nickname("a");
  service.HandleCreateRoom(a, request);
  request.set_nickname("b");
  service.HandleCreateRoom(b, request);

  EXPECT_EQ(a->Last<protocol::CreateRoomResponse>(MessageId::kCreateRoomResponse).state().room_code(), "123456");
  const auto second = b->Last<protocol::CreateRoomResponse>(MessageId::kCreateRoomResponse);
  ASSERT_EQ(second.result(), protocol::GAME_OK);
  EXPECT_EQ(second.state().room_code(), "654321");  // 겹치는 123456은 버리고 다시 뽑았다
  EXPECT_EQ(service.room_count(), 2u);
}

TEST(GameServiceCodes, MalformedCodesFromTheGeneratorAreDiscarded) {
  boost::asio::io_context io;
  std::vector<std::string> sequence = {"", "12345", "1234567", "abcdef", "12 456", "000042"};
  std::size_t next = 0;
  GameService service(io, PlayConfig{}, [&]() { return sequence[std::min(next++, sequence.size() - 1)]; });

  auto a = std::make_shared<FakeClient>(1);
  protocol::CreateRoomRequest request;
  request.set_nickname("a");
  service.HandleCreateRoom(a, request);
  const auto response = a->Last<protocol::CreateRoomResponse>(MessageId::kCreateRoomResponse);
  ASSERT_EQ(response.result(), protocol::GAME_OK);
  EXPECT_EQ(response.state().room_code(), "000042");  // 앞자리 0도 그대로 보존된다
}

TEST(GameServiceCodes, GivesUpWithServerBusyWhenEveryDrawCollides) {
  boost::asio::io_context io;
  PlayConfig config;
  config.room_code_attempts = 5;
  GameService service(io, config, []() { return std::string("111111"); });

  auto a = std::make_shared<FakeClient>(1);
  auto b = std::make_shared<FakeClient>(2);
  protocol::CreateRoomRequest request;
  request.set_nickname("a");
  service.HandleCreateRoom(a, request);
  request.set_nickname("b");
  service.HandleCreateRoom(b, request);

  EXPECT_EQ(b->Last<protocol::CreateRoomResponse>(MessageId::kCreateRoomResponse).result(), protocol::GAME_SERVER_BUSY);
  EXPECT_EQ(service.room_count(), 1u);
  EXPECT_EQ(service.RoomCodeOf(2), "");
}

TEST(GameServiceCodes, RoomCountIsCapped) {
  boost::asio::io_context io;
  PlayConfig config;
  config.max_rooms = 2;
  GameService service(io, config);
  std::vector<std::shared_ptr<FakeClient>> clients;
  for (std::uint64_t id = 1; id <= 3; ++id) {
    auto client = std::make_shared<FakeClient>(id);
    clients.push_back(client);
    protocol::CreateRoomRequest request;
    request.set_nickname("p");
    service.HandleCreateRoom(client, request);
  }
  EXPECT_EQ(clients[0]->Last<protocol::CreateRoomResponse>(MessageId::kCreateRoomResponse).result(), protocol::GAME_OK);
  EXPECT_EQ(clients[1]->Last<protocol::CreateRoomResponse>(MessageId::kCreateRoomResponse).result(), protocol::GAME_OK);
  EXPECT_EQ(clients[2]->Last<protocol::CreateRoomResponse>(MessageId::kCreateRoomResponse).result(),
            protocol::GAME_SERVER_BUSY);

  // 방이 비면 다시 만들 수 있다.
  service.OnDisconnect(1);
  protocol::CreateRoomRequest request;
  request.set_nickname("p");
  service.HandleCreateRoom(clients[2], request);
  EXPECT_EQ(clients[2]->Last<protocol::CreateRoomResponse>(MessageId::kCreateRoomResponse).result(), protocol::GAME_OK);
}

// ============================================================ 입장

TEST_F(GameServiceTest, JoinWithTheCodeShowsEveryoneAndNotifiesTheOthers) {
  const std::string code = CreateRoomWith(1, "alice");
  auto alice = clients_[1];
  auto bob = NewClient(2);

  const auto response = Join(bob, code, "bob");
  ASSERT_EQ(response.result(), protocol::GAME_OK);
  EXPECT_EQ(response.state().room_code(), code);
  EXPECT_EQ(response.state().your_player_id(), 2u);
  EXPECT_EQ(response.state().host_player_id(), 1u);
  ASSERT_EQ(response.state().players_size(), 2);
  EXPECT_EQ(response.state().players(0).nickname(), "alice");  // 입장한 순서
  EXPECT_EQ(response.state().players(1).nickname(), "bob");

  // 기존 사람에게는 입장 알림이 가고, 입장한 본인에게는 가지 않는다(응답으로 상태를 받는다).
  ASSERT_EQ(alice->CountOf(MessageId::kRoomEvent), 1u);
  const auto event = alice->Nth<protocol::RoomEvent>(MessageId::kRoomEvent);
  EXPECT_EQ(event.kind(), protocol::RoomEvent::PLAYER_JOINED);
  EXPECT_EQ(event.player().player_id(), 2u);
  EXPECT_EQ(event.player().nickname(), "bob");
  EXPECT_EQ(event.host_player_id(), 1u);
  EXPECT_EQ(bob->CountOf(MessageId::kRoomEvent), 0u);
}

TEST_F(GameServiceTest, JoinRejectsMalformedCodes) {
  CreateRoomWith(1, "alice");
  auto bob = NewClient(2);
  for (const char* code : {"", "12345", "1234567", "abcdef", "12345a", "12 456", " 123456", "١٢٣٤٥٦"}) {
    EXPECT_EQ(Join(bob, code, "bob").result(), protocol::GAME_INVALID_ROOM_CODE) << "[" << code << "]";
  }
  EXPECT_EQ(service_.RoomCodeOf(2), "");
}

TEST_F(GameServiceTest, JoinRejectsAnUnknownCode) {
  const std::string code = CreateRoomWith(1, "alice");
  auto bob = NewClient(2);
  const std::string other = code == "999999" ? "000000" : "999999";
  const auto response = Join(bob, other, "bob");
  EXPECT_EQ(response.result(), protocol::GAME_ROOM_NOT_FOUND);
  EXPECT_FALSE(response.has_state());
}

TEST_F(GameServiceTest, JoinRejectsInvalidNicknames) {
  const std::string code = CreateRoomWith(1, "alice");
  auto bob = NewClient(2);
  for (const char* nickname : {"", " bob", "bob\t"}) {
    EXPECT_EQ(Join(bob, code, nickname).result(), protocol::GAME_INVALID_NICKNAME) << "[" << nickname << "]";
  }
  EXPECT_EQ(service_.FindRoom(code)->members().size(), 1u);
}

TEST_F(GameServiceTest, JoinRejectsADuplicateNicknameInTheSameRoomOnly) {
  const std::string code = CreateRoomWith(1, "alice");
  auto bob = NewClient(2);
  EXPECT_EQ(Join(bob, code, "alice").result(), protocol::GAME_NICKNAME_TAKEN);
  EXPECT_EQ(service_.RoomCodeOf(2), "");

  // 다른 방에서는 같은 닉네임을 써도 된다.
  auto carol = NewClient(3);
  const std::string other = Create(carol, "alice").state().room_code();
  EXPECT_EQ(Create(carol, "x").result(), protocol::GAME_ALREADY_IN_ROOM);
  EXPECT_NE(other, code);
}

TEST_F(GameServiceTest, RoomHoldsAtMostFourPlayers) {
  const std::string code = CreateRoomWith(1, "p1");
  for (std::uint64_t id = 2; id <= 4; ++id) {
    auto client = NewClient(id);
    ASSERT_EQ(Join(client, code, "p" + std::to_string(id)).result(), protocol::GAME_OK);
  }
  auto fifth = NewClient(5);
  const auto response = Join(fifth, code, "p5");
  EXPECT_EQ(response.result(), protocol::GAME_ROOM_FULL);
  EXPECT_EQ(service_.FindRoom(code)->members().size(), 4u);

  // 한 명이 나가면 다시 들어갈 수 있다.
  EXPECT_EQ(Leave(clients_[3]).result(), protocol::GAME_OK);
  EXPECT_EQ(Join(fifth, code, "p5").result(), protocol::GAME_OK);
}

TEST_F(GameServiceTest, JoiningFromAnotherRoomOnlyAfterLeaving) {
  const std::string code_a = CreateRoomWith(1, "alice");
  auto bob = NewClient(2);
  const std::string code_b = Create(bob, "bob").state().room_code();
  EXPECT_EQ(Join(bob, code_a, "bob").result(), protocol::GAME_ALREADY_IN_ROOM);
  ASSERT_EQ(Leave(bob).result(), protocol::GAME_OK);
  EXPECT_EQ(service_.FindRoom(code_b), nullptr);  // 혼자였으니 방이 사라졌다
  EXPECT_EQ(Join(bob, code_a, "bob").result(), protocol::GAME_OK);
}

// ============================================================ 나가기, 방장, 방 삭제

TEST_F(GameServiceTest, LeaveNotifiesTheRemainingPlayers) {
  const std::string code = CreateRoomWith(1, "alice");
  auto alice = clients_[1];
  auto bob = NewClient(2);
  auto carol = NewClient(3);
  Join(bob, code, "bob");
  Join(carol, code, "carol");
  alice->received.clear();
  bob->received.clear();
  carol->received.clear();

  EXPECT_EQ(Leave(carol).result(), protocol::GAME_OK);
  EXPECT_EQ(service_.RoomCodeOf(3), "");

  for (const auto& client : {alice, bob}) {
    ASSERT_EQ(client->CountOf(MessageId::kRoomEvent), 1u);
    const auto event = client->Nth<protocol::RoomEvent>(MessageId::kRoomEvent);
    EXPECT_EQ(event.kind(), protocol::RoomEvent::PLAYER_LEFT);
    EXPECT_EQ(event.player().nickname(), "carol");
    EXPECT_EQ(event.host_player_id(), 1u);  // 방장은 그대로
  }
  EXPECT_EQ(carol->CountOf(MessageId::kRoomEvent), 0u);  // 나간 본인에게는 알림이 없다
}

TEST_F(GameServiceTest, LeaveWithoutBeingInARoomIsRejected) {
  auto client = NewClient(1);
  EXPECT_EQ(Leave(client).result(), protocol::GAME_NOT_IN_ROOM);
}

TEST_F(GameServiceTest, HostMigratesToTheEarliestRemainingPlayer) {
  const std::string code = CreateRoomWith(1, "alice");
  auto alice = clients_[1];
  auto bob = NewClient(2);
  auto carol = NewClient(3);
  Join(bob, code, "bob");
  Join(carol, code, "carol");
  bob->received.clear();
  carol->received.clear();

  ASSERT_EQ(Leave(alice).result(), protocol::GAME_OK);

  EXPECT_EQ(service_.FindRoom(code)->host_player_id(), 2u);  // 먼저 들어온 bob
  for (const auto& client : {bob, carol}) {
    const auto kinds = client->EventKinds();
    ASSERT_EQ(kinds.size(), 2u);
    EXPECT_EQ(kinds[0], protocol::RoomEvent::PLAYER_LEFT);
    EXPECT_EQ(kinds[1], protocol::RoomEvent::HOST_CHANGED);
    EXPECT_EQ(client->Nth<protocol::RoomEvent>(MessageId::kRoomEvent, 1).host_player_id(), 2u);
  }
}

TEST_F(GameServiceTest, TheRoomIsDeletedWhenTheLastPlayerLeaves) {
  const std::string code = CreateRoomWith(1, "alice");
  auto alice = clients_[1];
  auto bob = NewClient(2);
  Join(bob, code, "bob");

  Leave(alice);
  EXPECT_EQ(service_.room_count(), 1u);
  Leave(bob);
  EXPECT_EQ(service_.room_count(), 0u);
  EXPECT_EQ(service_.FindRoom(code), nullptr);
  EXPECT_EQ(service_.stats().rooms_closed.load(), 1u);
  EXPECT_EQ(service_.stats().current_rooms.load(), 0u);
  EXPECT_EQ(service_.stats().current_players.load(), 0u);

  auto carol = NewClient(3);
  EXPECT_EQ(Join(carol, code, "carol").result(), protocol::GAME_ROOM_NOT_FOUND);  // 닫힌 방에는 들어갈 수 없다
}

TEST_F(GameServiceTest, DisconnectingWithoutLeavingIsTheSameAsLeaving) {
  const std::string code = CreateRoomWith(1, "alice");
  auto alice = clients_[1];
  auto bob = NewClient(2);
  Join(bob, code, "bob");
  alice->received.clear();
  const std::size_t bob_messages = bob->received.size();

  service_.OnDisconnect(2);

  EXPECT_EQ(service_.RoomCodeOf(2), "");
  ASSERT_EQ(alice->CountOf(MessageId::kRoomEvent), 1u);
  EXPECT_EQ(alice->Nth<protocol::RoomEvent>(MessageId::kRoomEvent).kind(), protocol::RoomEvent::PLAYER_LEFT);
  EXPECT_EQ(bob->received.size(), bob_messages);  // 끊긴 연결에는 아무것도 보내지 않는다

  service_.OnDisconnect(1);
  EXPECT_EQ(service_.room_count(), 0u);
}

TEST_F(GameServiceTest, DisconnectOfAHostMigratesTheHostToo) {
  const std::string code = CreateRoomWith(1, "alice");
  auto bob = NewClient(2);
  Join(bob, code, "bob");
  bob->received.clear();
  service_.OnDisconnect(1);
  EXPECT_EQ(service_.FindRoom(code)->host_player_id(), 2u);
  EXPECT_EQ(bob->CountOf(MessageId::kRoomEvent), 2u);  // 퇴장 + 방장 변경
}

TEST_F(GameServiceTest, DisconnectOfAConnectionThatIsNotInARoomChangesNothing) {
  const std::string code = CreateRoomWith(1, "alice");
  service_.OnDisconnect(99);
  service_.OnDisconnect(99);
  EXPECT_EQ(service_.room_count(), 1u);
  EXPECT_EQ(service_.FindRoom(code)->members().size(), 1u);
}

TEST_F(GameServiceTest, APlayerCanCreateAgainAfterLeaving) {
  auto alice = NewClient(1);
  const std::string first = Create(alice, "alice").state().room_code();
  Leave(alice);
  const auto second = Create(alice, "alice");
  ASSERT_EQ(second.result(), protocol::GAME_OK);
  EXPECT_EQ(second.state().your_player_id(), 1u);  // 새 방이라 번호가 다시 1부터
  (void)first;
}

// ============================================================ 이동과 틱

TEST_F(GameServiceTest, TickSendsASnapshotOfTheWholeRoomToEveryPlayerInIt) {
  const std::string code = CreateRoomWith(1, "alice");
  auto alice = clients_[1];
  auto bob = NewClient(2);
  Join(bob, code, "bob");

  service_.Tick();

  for (const auto& client : {alice, bob}) {
    ASSERT_EQ(client->CountOf(MessageId::kWorldSnapshot), 1u);
    const auto snapshot = client->Nth<protocol::WorldSnapshot>(MessageId::kWorldSnapshot);
    EXPECT_EQ(snapshot.tick(), 1u);
    ASSERT_EQ(snapshot.players_size(), 2);
    EXPECT_EQ(snapshot.players(0).player_id(), 1u);
    EXPECT_FLOAT_EQ(snapshot.players(0).x(), 2400.0f);
    EXPECT_EQ(snapshot.players(1).player_id(), 2u);
    EXPECT_FLOAT_EQ(snapshot.players(1).x(), 2600.0f);
  }
  EXPECT_EQ(service_.stats().snapshots_sent.load(), 2u);
}

TEST_F(GameServiceTest, MoveInputMovesThePlayerAtTheServerSpeedAndEveryoneSeesIt) {
  const std::string code = CreateRoomWith(1, "alice");
  auto alice = clients_[1];
  auto bob = NewClient(2);
  Join(bob, code, "bob");

  Move(alice, 1, 1.0f, 0.0f);
  for (int i = 0; i < 20; ++i) service_.Tick();  // 20틱 = 1초 = 360 cm

  const auto from_bob = bob->Last<protocol::WorldSnapshot>(MessageId::kWorldSnapshot);
  EXPECT_EQ(from_bob.tick(), 20u);
  EXPECT_NEAR(from_bob.players(0).x(), 2760.0f, 0.01f);
  EXPECT_FLOAT_EQ(from_bob.players(0).y(), 2400.0f);
  EXPECT_FLOAT_EQ(from_bob.players(1).x(), 2600.0f);  // 입력이 없는 bob은 제자리
  EXPECT_EQ(from_bob.players(0).last_input_seq(), 1u);

  const auto from_alice = alice->Last<protocol::WorldSnapshot>(MessageId::kWorldSnapshot);
  EXPECT_FLOAT_EQ(from_alice.players(0).x(), from_bob.players(0).x());  // 같은 방은 같은 상태를 본다
  EXPECT_EQ(service_.stats().inputs_applied.load(), 1u);
}

TEST_F(GameServiceTest, OversizedInputCannotMoveFasterThanTheServerSpeed) {
  CreateRoomWith(1, "alice");
  Move(clients_[1], 1, 100.0f, 0.0f);
  for (int i = 0; i < 20; ++i) service_.Tick();
  const auto snapshot = clients_[1]->Last<protocol::WorldSnapshot>(MessageId::kWorldSnapshot);
  EXPECT_NEAR(snapshot.players(0).x(), 2760.0f, 0.01f);
}

TEST_F(GameServiceTest, SnapshotsStayInsideTheirOwnRoom) {
  const std::string code_a = CreateRoomWith(1, "alice");
  auto bob = NewClient(2);
  const std::string code_b = Create(bob, "bob").state().room_code();
  auto carol = NewClient(3);
  Join(carol, code_a, "carol");

  Move(clients_[1], 1, 1.0f, 0.0f);
  service_.Tick();

  EXPECT_EQ(clients_[1]->Last<protocol::WorldSnapshot>(MessageId::kWorldSnapshot).players_size(), 2);
  EXPECT_EQ(carol->Last<protocol::WorldSnapshot>(MessageId::kWorldSnapshot).players_size(), 2);
  const auto other = bob->Last<protocol::WorldSnapshot>(MessageId::kWorldSnapshot);
  ASSERT_EQ(other.players_size(), 1);  // 다른 방 사람은 보이지 않는다
  EXPECT_EQ(other.players(0).player_id(), 1u);
  EXPECT_FLOAT_EQ(other.players(0).x(), 2400.0f);  // 다른 방의 입력에 영향받지 않는다
  (void)code_b;
}

TEST_F(GameServiceTest, NoSnapshotsAreSentToPlayersWhoLeftOrToNoRooms) {
  auto alice = clients_[1];
  service_.Tick();  // 방이 하나도 없어도 안전하다
  EXPECT_EQ(service_.stats().ticks.load(), 1u);
  EXPECT_EQ(service_.stats().snapshots_sent.load(), 0u);

  const std::string code = CreateRoomWith(1, "alice");
  alice = clients_[1];
  Leave(alice);
  alice->received.clear();
  service_.Tick();
  EXPECT_EQ(alice->CountOf(MessageId::kWorldSnapshot), 0u);
  (void)code;
}

TEST_F(GameServiceTest, InputFromAConnectionNotInARoomIsIgnoredWithoutHarm) {
  auto stranger = NewClient(9);
  Move(stranger, 1, 1.0f, 0.0f);
  EXPECT_EQ(service_.stats().inputs_ignored.load(), 1u);
  EXPECT_EQ(stranger->received.size(), 0u);  // 응답도 오류도 보내지 않는다

  // 방을 나간 직후 늦게 도착한 입력도 무시된다.
  CreateRoomWith(1, "alice");
  Leave(clients_[1]);
  Move(clients_[1], 5, 1.0f, 0.0f);
  EXPECT_EQ(service_.stats().inputs_ignored.load(), 2u);
}

TEST_F(GameServiceTest, InvalidAndStaleInputsAreIgnoredAndCounted) {
  CreateRoomWith(1, "alice");
  auto alice = clients_[1];
  Move(alice, 5, 1.0f, 0.0f);                                              // 반영
  Move(alice, 5, 0.0f, 1.0f);                                              // 같은 번호: 오래된 입력
  Move(alice, 6, std::numeric_limits<float>::quiet_NaN(), 0.0f);           // NaN
  EXPECT_EQ(service_.stats().inputs_applied.load(), 1u);
  EXPECT_EQ(service_.stats().inputs_ignored.load(), 2u);

  for (int i = 0; i < 20; ++i) service_.Tick();
  const auto snapshot = alice->Last<protocol::WorldSnapshot>(MessageId::kWorldSnapshot);
  EXPECT_NEAR(snapshot.players(0).x(), 2760.0f, 0.01f);  // 첫 입력만 반영되었다
  EXPECT_FLOAT_EQ(snapshot.players(0).y(), 2400.0f);
}

TEST_F(GameServiceTest, TickWithSeveralStepsAdvancesTheWorldButSendsOneSnapshot) {
  CreateRoomWith(1, "alice");
  auto alice = clients_[1];
  Move(alice, 1, 1.0f, 0.0f);
  service_.Tick(3);  // 밀린 틱을 따라잡는 경우
  EXPECT_EQ(alice->CountOf(MessageId::kWorldSnapshot), 1u);
  const auto snapshot = alice->Nth<protocol::WorldSnapshot>(MessageId::kWorldSnapshot);
  EXPECT_EQ(snapshot.tick(), 3u);
  EXPECT_NEAR(snapshot.players(0).x(), 2400.0f + 54.0f, 0.01f);  // 3틱 x 18 cm
  EXPECT_EQ(service_.stats().ticks.load(), 3u);
}

TEST_F(GameServiceTest, LateJoinersStartAtTheirSlotNotWhereTheOthersWalked) {
  const std::string code = CreateRoomWith(1, "alice");
  Move(clients_[1], 1, 1.0f, 0.0f);
  for (int i = 0; i < 40; ++i) service_.Tick();
  auto bob = NewClient(2);
  Join(bob, code, "bob");
  service_.Tick();
  const auto snapshot = bob->Last<protocol::WorldSnapshot>(MessageId::kWorldSnapshot);
  ASSERT_EQ(snapshot.players_size(), 2);
  EXPECT_NEAR(snapshot.players(0).x(), 3138.0f, 0.01f);  // 계속 걸어간 alice (41틱 x 18 cm)
  EXPECT_FLOAT_EQ(snapshot.players(1).x(), 2600.0f);     // 칸 1의 시작 위치
  EXPECT_EQ(snapshot.tick(), 41u);
}

// ============================================================ 틱 중 연결 종료 (재진입)

// 스냅샷을 보내는 도중에 연결이 닫혀 OnDisconnect가 불리는 상황(느린 수신자의 송신 큐 초과 등)을 흉내 낸다.
TEST_F(GameServiceTest, TickSurvivesAClientDisconnectingWhileSnapshotsAreBeingDelivered) {
  const std::string code = CreateRoomWith(1, "alice");
  auto alice = clients_[1];
  auto bob = NewClient(2);
  auto carol = NewClient(3);
  Join(bob, code, "bob");
  Join(carol, code, "carol");

  // alice가 스냅샷을 받는 순간 bob의 연결이 끊긴다.
  alice->on_deliver = [&](MessageId id) {
    if (id == MessageId::kWorldSnapshot) service_.OnDisconnect(2);
  };
  service_.Tick();

  EXPECT_EQ(service_.FindRoom(code)->members().size(), 2u);
  EXPECT_EQ(service_.RoomCodeOf(2), "");
  service_.Tick();  // 다음 틱도 정상
  EXPECT_EQ(carol->CountOf(MessageId::kWorldSnapshot), 2u);
}

TEST_F(GameServiceTest, TickSurvivesTheRoomBeingDeletedWhileSnapshotsAreBeingDelivered) {
  const std::string code_a = CreateRoomWith(1, "alice");
  auto alice = clients_[1];
  auto bob = NewClient(2);
  Join(bob, code_a, "bob");
  auto carol = NewClient(3);
  const std::string code_b = Create(carol, "carol").state().room_code();

  // 어느 방이 먼저 전송되든, 첫 스냅샷을 받는 순간 그 방의 모든 사람이 끊기고 다른 방도 끊긴다.
  auto kill_everyone = [&](MessageId id) {
    if (id == MessageId::kWorldSnapshot) {
      service_.OnDisconnect(1);
      service_.OnDisconnect(2);
      service_.OnDisconnect(3);
    }
  };
  alice->on_deliver = kill_everyone;
  bob->on_deliver = kill_everyone;
  carol->on_deliver = kill_everyone;

  service_.Tick();  // 순회 중 방이 모두 지워져도 죽지 않아야 한다
  EXPECT_EQ(service_.room_count(), 0u);
  service_.Tick();
  EXPECT_EQ(service_.stats().current_players.load(), 0u);
  (void)code_b;
}

TEST_F(GameServiceTest, DisconnectDuringARoomEventDeliveryIsSafe) {
  const std::string code = CreateRoomWith(1, "alice");
  auto alice = clients_[1];
  auto bob = NewClient(2);
  auto carol = NewClient(3);
  Join(bob, code, "bob");
  Join(carol, code, "carol");

  // carol이 나가는 알림을 받는 순간 alice도 끊긴다(방장이 도중에 바뀐다).
  bob->on_deliver = [&](MessageId id) {
    if (id == MessageId::kRoomEvent) service_.OnDisconnect(1);
  };
  alice->on_deliver = nullptr;
  service_.OnDisconnect(3);

  ASSERT_NE(service_.FindRoom(code), nullptr);
  EXPECT_EQ(service_.FindRoom(code)->members().size(), 1u);
  EXPECT_EQ(service_.FindRoom(code)->host_player_id(), 2u);
}

// ============================================================ 종료

TEST_F(GameServiceTest, ShutdownStopsNotificationsAndNewRooms) {
  const std::string code = CreateRoomWith(1, "alice");
  auto alice = clients_[1];
  auto bob = NewClient(2);
  Join(bob, code, "bob");
  alice->received.clear();

  service_.Shutdown();
  service_.OnDisconnect(2);  // 종료 중에는 알림을 보내지 않는다
  EXPECT_EQ(alice->received.size(), 0u);
  EXPECT_EQ(service_.RoomCodeOf(2), "");

  auto carol = NewClient(3);
  EXPECT_EQ(Create(carol, "carol").result(), protocol::GAME_SERVER_BUSY);
  EXPECT_EQ(Join(carol, code, "carol").result(), protocol::GAME_SERVER_BUSY);
  service_.Tick();  // 종료 후의 틱은 아무것도 하지 않는다
  EXPECT_EQ(alice->CountOf(MessageId::kWorldSnapshot), 0u);
}

// ============================================================ 타이머로 도는 틱

TEST(GameServiceTimer, TicksRunByThemselvesAndStopAfterShutdown) {
  boost::asio::io_context io;
  PlayConfig config;
  config.tick_rate_hz = 100;  // 10 ms 간격으로 시험을 빠르게
  GameService service(io, config);
  auto alice = std::make_shared<FakeClient>(1);
  protocol::CreateRoomRequest request;
  request.set_nickname("alice");
  service.HandleCreateRoom(alice, request);

  service.StartTicking();
  service.StartTicking();  // 두 번 불러도 타이머는 하나

  io.run_for(std::chrono::milliseconds(400));
  const auto steps = service.stats().ticks.load();
  const auto skipped = service.stats().ticks_skipped.load();
  // 400 ms / 10 ms = 40틱이 이상적이다. 윈도우의 기본 타이머 해상도(약 15.6 ms)에서는 타이머가 늦게 깨어나
  // 틱을 몰아서 처리(따라잡기)하므로 스냅샷 수는 틱 수보다 적을 수 있지만, 절대 시각 격자에 맞추기 때문에
  // 처리한 틱과 건너뛴 틱의 합은 격자 간격을 넘지 않고, 시간이 누적되어 밀리지도 않는다.
  EXPECT_GE(steps, 20u);
  EXPECT_LE(steps + skipped, 45u);
  const std::size_t snapshots = alice->CountOf(MessageId::kWorldSnapshot);
  EXPECT_GE(snapshots, 5u);
  EXPECT_LE(snapshots, steps);  // 따라잡은 틱마다 스냅샷을 보내지 않고 마지막 상태를 한 번만 보낸다
  EXPECT_EQ(alice->Last<protocol::WorldSnapshot>(MessageId::kWorldSnapshot).tick(), steps);

  service.Shutdown();
  io.restart();
  io.run_for(std::chrono::milliseconds(100));
  EXPECT_EQ(service.stats().ticks.load(), steps);  // 종료 뒤에는 더 돌지 않는다
}

// ============================================================ 자원 채집 (docs/stage4-gather.md)

protocol::GatherResponse DoGather(GameService& service, const ClientPtr& client, std::uint32_t node_id) {
  protocol::GatherRequest request;
  request.set_node_id(node_id);
  service.HandleGather(client, request);
  return client->Last<protocol::GatherResponse>(MessageId::kGatherResponse);
}

// 허용 거리를 맵 전체보다 크게, 나무 체력을 1로 해 한 번에 고갈되게 한 설정. 나무는 1~30번, 돌은 31~50번이다.
class GatherServiceTest : public GameServiceTest {
 protected:
  GatherServiceTest() : GameServiceTest(MakeConfig()) {}
  static PlayConfig MakeConfig() {
    PlayConfig config;
    config.gather_range_cm = 100000.0f;
    config.tree_hp = 1;
    return config;
  }

  // 방을 만들고 두 번째 사람을 입장시킨다. alice(1), bob(2).
  std::string TwoPlayers() {
    const std::string code = CreateRoomWith(1, "alice");
    auto bob = NewClient(2);
    EXPECT_EQ(Join(bob, code, "bob").result(), protocol::GAME_OK);
    return code;
  }
};

TEST_F(GameServiceTest, RoomStateCarriesEveryNodeWithItsKindPositionAndHealth) {
  auto alice = NewClient(1);
  const auto state = Create(alice, "alice").state();
  const PlayConfig config;
  ASSERT_EQ(state.nodes_size(), static_cast<int>(config.tree_count + config.rock_count));
  int trees = 0;
  int rocks = 0;
  for (int i = 0; i < state.nodes_size(); ++i) {
    const auto& node = state.nodes(i);
    EXPECT_EQ(node.node_id(), static_cast<std::uint32_t>(i + 1));
    EXPECT_EQ(node.hp(), node.max_hp());
    EXPECT_GT(node.x(), 0.0f);
    EXPECT_GT(node.y(), 0.0f);
    (node.kind() == protocol::RESOURCE_TREE ? trees : rocks)++;
  }
  EXPECT_EQ(trees, static_cast<int>(config.tree_count));
  EXPECT_EQ(rocks, static_cast<int>(config.rock_count));
}

TEST_F(GatherServiceTest, SuccessfulGatherRepliesAtOnceAndTheRoomLearnsOnTheNextTick) {
  TwoPlayers();
  auto alice = clients_[1];
  auto bob = clients_[2];

  const auto response = DoGather(service_, alice, 1);
  EXPECT_EQ(response.result(), protocol::GAME_OK);
  EXPECT_EQ(response.node_id(), 1u);
  EXPECT_EQ(response.wood(), 1u);
  EXPECT_EQ(response.stone(), 0u);
  EXPECT_EQ(alice->CountOf(MessageId::kResourceUpdate), 0u) << "알림은 틱 끝에 간다";
  EXPECT_EQ(service_.stats().gathers_ok.load(), 1u);

  service_.Tick();
  for (const auto& client : {alice, bob}) {
    ASSERT_EQ(client->CountOf(MessageId::kResourceUpdate), 1u) << "채집한 본인과 다른 사람 모두 받는다";
    const auto update = client->Nth<protocol::ResourceUpdate>(MessageId::kResourceUpdate);
    EXPECT_EQ(update.tick(), 1u);
    ASSERT_EQ(update.nodes_size(), 1);
    EXPECT_EQ(update.nodes(0).node_id(), 1u);
    EXPECT_EQ(update.nodes(0).hp(), 0u) << "나무 체력 1이라 고갈";
  }
  EXPECT_EQ(service_.stats().nodes_depleted.load(), 1u);
  EXPECT_EQ(service_.stats().resource_updates_sent.load(), 2u);

  service_.Tick();
  EXPECT_EQ(alice->CountOf(MessageId::kResourceUpdate), 1u) << "바뀐 것이 없으면 보내지 않는다";
}

TEST_F(GatherServiceTest, RocksGiveStoneAndInventoriesAreSeparate) {
  TwoPlayers();
  const auto rock = DoGather(service_, clients_[2], 31);
  EXPECT_EQ(rock.result(), protocol::GAME_OK);
  EXPECT_EQ(rock.wood(), 0u);
  EXPECT_EQ(rock.stone(), 1u);
  const auto tree = DoGather(service_, clients_[1], 1);
  EXPECT_EQ(tree.wood(), 1u);
  EXPECT_EQ(tree.stone(), 0u);
}

TEST_F(GatherServiceTest, EachRejectionHasItsOwnCodeAndChangesNothing) {
  TwoPlayers();
  auto alice = clients_[1];
  ASSERT_EQ(DoGather(service_, alice, 1).result(), protocol::GAME_OK);  // 1번 고갈, 쿨다운 시작
  service_.Tick();
  const std::size_t updates = alice->CountOf(MessageId::kResourceUpdate);

  EXPECT_EQ(DoGather(service_, alice, 9999).result(), protocol::GAME_NODE_NOT_FOUND);
  EXPECT_EQ(DoGather(service_, alice, 0).result(), protocol::GAME_NODE_NOT_FOUND);
  EXPECT_EQ(DoGather(service_, alice, 1).result(), protocol::GAME_NODE_DEPLETED);
  const auto cooldown = DoGather(service_, alice, 2);
  EXPECT_EQ(cooldown.result(), protocol::GAME_GATHER_COOLDOWN);
  EXPECT_EQ(cooldown.wood(), 1u) << "거부 응답도 내 현재 인벤토리를 담는다";

  service_.Tick();
  EXPECT_EQ(alice->CountOf(MessageId::kResourceUpdate), updates) << "거부는 노드를 바꾸지 않는다";
  EXPECT_EQ(service_.stats().gathers_ok.load(), 1u);
  EXPECT_EQ(service_.stats().gathers_rejected.load(), 4u);
  EXPECT_EQ(service_.room_count(), 1u) << "연결도 방도 그대로";
}

TEST_F(GameServiceTest, GatheringFromTooFarAwayIsRejectedByTheServerPosition) {
  CreateRoomWith(1, "alice");  // 기본 설정: 허용 거리 200 cm, 노드는 시작 위치에서 멀리 있다
  const auto response = DoGather(service_, clients_[1], 1);
  EXPECT_EQ(response.result(), protocol::GAME_TOO_FAR);
  EXPECT_EQ(response.wood(), 0u);
}

TEST_F(GameServiceTest, GatheringOutsideARoomIsRejectedAndKeepsTheConnection) {
  auto loner = NewClient(7);
  const auto response = DoGather(service_, loner, 1);
  EXPECT_EQ(response.result(), protocol::GAME_NOT_IN_ROOM);
  EXPECT_EQ(service_.stats().gathers_rejected.load(), 1u);
  // 나간 뒤의 요청도 마찬가지
  CreateRoomWith(8, "alice");
  Leave(clients_[8]);
  EXPECT_EQ(DoGather(service_, clients_[8], 1).result(), protocol::GAME_NOT_IN_ROOM);
  EXPECT_EQ(service_.room_count(), 0u);
}

TEST_F(GatherServiceTest, ALateJoinerReceivesTheCurrentNodeHealth) {
  const std::string code = CreateRoomWith(1, "alice");
  ASSERT_EQ(DoGather(service_, clients_[1], 1).result(), protocol::GAME_OK);
  service_.Tick();
  auto bob = NewClient(2);
  const auto joined = Join(bob, code, "bob");
  ASSERT_EQ(joined.result(), protocol::GAME_OK);
  ASSERT_GT(joined.state().nodes_size(), 2);
  EXPECT_EQ(joined.state().nodes(0).hp(), 0u) << "고갈된 노드";
  EXPECT_EQ(joined.state().nodes(1).hp(), 1u) << "그대로인 노드";
}

TEST_F(GatherServiceTest, ADepletedNodeStaysDepletedUntilTheNightAndThenTheRoomIsTold) {
  TwoPlayers();
  auto alice = clients_[1];
  auto bob = clients_[2];
  ASSERT_EQ(DoGather(service_, alice, 1).result(), protocol::GAME_OK);
  service_.Tick();  // 1틱: 고갈 알림
  ASSERT_EQ(bob->CountOf(MessageId::kResourceUpdate), 1u);

  for (int i = 0; i < 600; ++i) service_.Tick();  // 옛 재생 시각(600틱)이 지나도
  EXPECT_EQ(bob->CountOf(MessageId::kResourceUpdate), 1u) << "밤 정산 전에는 재생 알림이 없다";
  EXPECT_EQ(DoGather(service_, alice, 1).result(), protocol::GAME_NODE_DEPLETED);
  EXPECT_EQ(service_.stats().nodes_respawned.load(), 0u);

  WalkToFarmhouse({alice, bob});
  ASSERT_EQ(Sleep(alice, true).result(), protocol::GAME_OK);
  ASSERT_EQ(Sleep(bob, true).result(), protocol::GAME_OK);
  service_.Tick();  // 모두 잠든 다음 틱에 밤 정산
  ASSERT_EQ(bob->CountOf(MessageId::kDayStarted), 1u);
  const auto night = bob->Last<protocol::DayStarted>(MessageId::kDayStarted);
  ASSERT_EQ(bob->CountOf(MessageId::kResourceUpdate), 2u);
  const auto update = bob->Last<protocol::ResourceUpdate>(MessageId::kResourceUpdate);
  ASSERT_EQ(update.nodes_size(), 1) << "고갈되었던 노드만";
  EXPECT_EQ(update.nodes(0).node_id(), 1u);
  EXPECT_EQ(update.nodes(0).hp(), 1u);
  EXPECT_EQ(update.tick(), night.tick()) << "정산한 그 틱";
  EXPECT_EQ(service_.stats().nodes_respawned.load(), 1u);
  EXPECT_EQ(DoGather(service_, alice, 1).result(), protocol::GAME_OK) << "재생한 노드는 다시 칠 수 있다";
}

TEST_F(GatherServiceTest, ChangesInOneTickAreMergedIntoOneUpdateSortedByNode) {
  TwoPlayers();
  ASSERT_EQ(DoGather(service_, clients_[1], 5).result(), protocol::GAME_OK);
  ASSERT_EQ(DoGather(service_, clients_[2], 2).result(), protocol::GAME_OK);
  service_.Tick();
  ASSERT_EQ(clients_[1]->CountOf(MessageId::kResourceUpdate), 1u);
  const auto update = clients_[1]->Nth<protocol::ResourceUpdate>(MessageId::kResourceUpdate);
  ASSERT_EQ(update.nodes_size(), 2);
  EXPECT_EQ(update.nodes(0).node_id(), 2u);
  EXPECT_EQ(update.nodes(1).node_id(), 5u);
}

TEST_F(GatherServiceTest, ACatchUpTickStillDeliversEveryChangeAndRespawnsInOrder) {
  CreateRoomWith(1, "alice");
  auto alice = clients_[1];
  ASSERT_EQ(DoGather(service_, alice, 1).result(), protocol::GAME_OK);
  service_.Tick(3);  // 따라잡기: 세 틱을 한꺼번에
  ASSERT_EQ(alice->CountOf(MessageId::kResourceUpdate), 1u);
  EXPECT_EQ(alice->Last<protocol::ResourceUpdate>(MessageId::kResourceUpdate).tick(), 3u);

  // 하루 끝(12,000틱)을 넘는 따라잡기 틱: 정산은 한 번이고 새 날은 그 틱에서 시작한다.
  service_.Tick(12000);  // 3 -> 12,003
  EXPECT_EQ(alice->CountOf(MessageId::kDayStarted), 1u) << "경계를 넘어도 정산은 한 번";
  const auto night = alice->Last<protocol::DayStarted>(MessageId::kDayStarted);
  EXPECT_TRUE(night.forced());
  EXPECT_EQ(night.tick(), 12003u);
  EXPECT_EQ(night.clock().day_start_tick(), 12003) << "새 날은 그 틱에 시작(넘은 틱 수는 버린다)";
  ASSERT_EQ(alice->CountOf(MessageId::kResourceUpdate), 2u);
  EXPECT_EQ(alice->Last<protocol::ResourceUpdate>(MessageId::kResourceUpdate).nodes(0).hp(), 1u);
  service_.Tick();
  EXPECT_EQ(alice->CountOf(MessageId::kDayStarted), 1u) << "다음 틱에 또 정산하지 않는다";
}

TEST_F(GatherServiceTest, UpdatesStayInsideTheirOwnRoom) {
  CreateRoomWith(1, "alice");
  CreateRoomWith(2, "bob");  // 다른 방
  ASSERT_EQ(DoGather(service_, clients_[1], 1).result(), protocol::GAME_OK);
  service_.Tick();
  EXPECT_EQ(clients_[1]->CountOf(MessageId::kResourceUpdate), 1u);
  EXPECT_EQ(clients_[2]->CountOf(MessageId::kResourceUpdate), 0u);
  EXPECT_EQ(DoGather(service_, clients_[2], 1).result(), protocol::GAME_OK) << "다른 방의 1번 노드는 멀쩡하다";
}

TEST_F(GatherServiceTest, ALeaverDuringUpdateDeliveryDoesNotBreakTheTick) {
  TwoPlayers();
  // 알림을 받는 순간 다른 사람의 연결이 닫히는 경우(송신 큐 초과 등)
  clients_[1]->on_deliver = [this](MessageId id) {
    if (id == MessageId::kResourceUpdate) service_.OnDisconnect(2);
  };
  ASSERT_EQ(DoGather(service_, clients_[1], 1).result(), protocol::GAME_OK);
  service_.Tick();
  EXPECT_EQ(service_.RoomCodeOf(2), "");
  EXPECT_EQ(service_.room_count(), 1u);
  service_.Tick();  // 이어서 틱이 계속 돈다
  EXPECT_EQ(service_.stats().current_players.load(), 1u);
}

// ============================================================ 집 짓기 (docs/content-roadmap.md)

protocol::BuildResponse DoBuild(GameService& service, const ClientPtr& client, float x, float y, std::uint32_t type = 0) {
  protocol::BuildRequest request;
  request.set_structure_type(static_cast<protocol::StructureType>(type));
  request.set_x(x);
  request.set_y(y);
  service.HandleBuild(client, request);
  return client->Last<protocol::BuildResponse>(MessageId::kBuildResponse);
}

// 쿨다운 없이 자원을 모을 수 있고(나무 1번 20개, 돌 31번 10개), 건설 허용 거리는 500 cm인 설정.
class BuildServiceTest : public GameServiceTest {
 protected:
  BuildServiceTest() : GameServiceTest(MakeConfig()) {}
  static PlayConfig MakeConfig() {
    PlayConfig config;
    config.gather_range_cm = 100000.0f;
    config.gather_cooldown_ticks = 0;
    config.tree_hp = 1000;
    config.rock_hp = 1000;
    return config;
  }

  void Equip(const ClientPtr& client, int wood = 20, int stone = 10) {
    for (int i = 0; i < wood; ++i) ASSERT_EQ(DoGather(service_, client, 1).result(), protocol::GAME_OK);
    for (int i = 0; i < stone; ++i) ASSERT_EQ(DoGather(service_, client, 31).result(), protocol::GAME_OK);
  }

  // alice(1)가 방을 만들고 bob(2)이 입장한다.
  std::string TwoPlayers() {
    const std::string code = CreateRoomWith(1, "alice");
    auto bob = NewClient(2);
    EXPECT_EQ(Join(bob, code, "bob").result(), protocol::GAME_OK);
    return code;
  }
};

TEST_F(BuildServiceTest, BuildingRepliesAtOnceAndTheRoomLearnsOnTheNextTickThenOnCompletion) {
  TwoPlayers();
  auto alice = clients_[1];
  auto bob = clients_[2];
  Equip(alice);

  const auto response = DoBuild(service_, alice, 2400.0f, 2000.0f);
  EXPECT_EQ(response.result(), protocol::GAME_OK);
  EXPECT_EQ(response.structure_id(), 1u);
  EXPECT_EQ(response.wood(), 0u) << "비용만큼 차감된 현재 인벤토리";
  EXPECT_EQ(response.stone(), 0u);
  EXPECT_EQ(alice->CountOf(MessageId::kStructureUpdate), 0u) << "알림은 틱 끝에 간다";
  EXPECT_EQ(service_.stats().builds_ok.load(), 1u);

  service_.Tick();
  for (const auto& client : {alice, bob}) {
    ASSERT_EQ(client->CountOf(MessageId::kStructureUpdate), 1u) << "짓는 본인과 다른 사람 모두 받는다";
    const auto update = client->Nth<protocol::StructureUpdate>(MessageId::kStructureUpdate);
    ASSERT_EQ(update.structures_size(), 1);
    EXPECT_EQ(update.structures(0).state(), protocol::STRUCTURE_BUILDING);
    EXPECT_EQ(update.structures(0).ready_tick(), 0u) << "건설 중이면 0(다음 밤 정산에 완공)";
  }

  for (int i = 0; i < 200; ++i) service_.Tick();  // 옛 완공 시각(100틱)이 지나도
  EXPECT_EQ(bob->CountOf(MessageId::kStructureUpdate), 1u) << "밤 정산 전에는 완공 알림이 없다";
  WalkToFarmhouse({alice, bob});
  ASSERT_EQ(Sleep(alice, true).result(), protocol::GAME_OK);
  ASSERT_EQ(Sleep(bob, true).result(), protocol::GAME_OK);
  EXPECT_EQ(bob->CountOf(MessageId::kStructureUpdate), 1u);
  service_.Tick();  // 밤 정산
  ASSERT_EQ(bob->CountOf(MessageId::kStructureUpdate), 2u);
  const auto night = bob->Last<protocol::DayStarted>(MessageId::kDayStarted);
  const auto done = bob->Last<protocol::StructureUpdate>(MessageId::kStructureUpdate);
  EXPECT_EQ(done.tick(), night.tick());
  EXPECT_EQ(done.structures(0).state(), protocol::STRUCTURE_COMPLETE);
  EXPECT_EQ(done.structures(0).ready_tick(), night.tick()) << "완공이면 완공된 틱";
  EXPECT_EQ(service_.stats().structures_completed.load(), 1u);

  service_.Tick();
  EXPECT_EQ(bob->CountOf(MessageId::kStructureUpdate), 2u) << "바뀐 것이 없으면 보내지 않는다";
}

TEST_F(BuildServiceTest, EachRejectionHasItsOwnCodeAndChangesNothing) {
  TwoPlayers();
  auto alice = clients_[1];
  EXPECT_EQ(DoBuild(service_, alice, 2400.0f, 2000.0f).result(), protocol::GAME_NOT_ENOUGH_RESOURCES);
  Equip(alice);

  EXPECT_EQ(DoBuild(service_, alice, 2400.0f, 2000.0f, 5).result(), protocol::GAME_BUILD_INVALID_TYPE);
  EXPECT_EQ(DoBuild(service_, alice, 2400.0f, 2400.0f).result(), protocol::GAME_BUILD_BLOCKED);
  EXPECT_EQ(DoBuild(service_, alice, 4500.0f, 4500.0f).result(), protocol::GAME_BUILD_TOO_FAR);
  const auto rejected = DoBuild(service_, alice, 4500.0f, 4500.0f);
  EXPECT_EQ(rejected.wood(), 20u) << "거부 응답도 현재 인벤토리를 담는다";
  EXPECT_EQ(rejected.stone(), 10u);

  service_.Tick();
  EXPECT_EQ(alice->CountOf(MessageId::kStructureUpdate), 0u) << "거부는 알림을 만들지 않는다";
  EXPECT_EQ(service_.stats().builds_ok.load(), 0u);
  EXPECT_EQ(service_.stats().builds_rejected.load(), 5u);
  EXPECT_EQ(service_.room_count(), 1u) << "연결도 방도 그대로";
}

TEST_F(BuildServiceTest, BuildingOutsideARoomIsRejectedAndKeepsTheConnection) {
  auto loner = NewClient(7);
  EXPECT_EQ(DoBuild(service_, loner, 2400.0f, 2000.0f).result(), protocol::GAME_NOT_IN_ROOM);
  EXPECT_EQ(service_.stats().builds_rejected.load(), 1u);
  CreateRoomWith(8, "alice");
  Leave(clients_[8]);
  EXPECT_EQ(DoBuild(service_, clients_[8], 2400.0f, 2000.0f).result(), protocol::GAME_NOT_IN_ROOM);
}

TEST_F(BuildServiceTest, ALateJoinerReceivesTheCurrentStructuresWithTheirState) {
  const std::string code = CreateRoomWith(1, "alice");
  Equip(clients_[1], 40, 20);
  ASSERT_EQ(DoBuild(service_, clients_[1], 2400.0f, 2000.0f).result(), protocol::GAME_OK);
  service_.Tick(12000);  // 강제 밤 정산: 첫 집 완공
  ASSERT_EQ(clients_[1]->CountOf(MessageId::kDayStarted), 1u);
  ASSERT_EQ(DoBuild(service_, clients_[1], 2800.0f, 2200.0f).result(), protocol::GAME_OK);
  service_.Tick();  // 둘째 집은 건설 중

  auto bob = NewClient(2);
  const auto joined = Join(bob, code, "bob");
  ASSERT_EQ(joined.result(), protocol::GAME_OK);
  ASSERT_EQ(joined.state().structures_size(), 2);
  EXPECT_EQ(joined.state().structures(0).state(), protocol::STRUCTURE_COMPLETE);
  EXPECT_EQ(joined.state().structures(1).state(), protocol::STRUCTURE_BUILDING);
}

TEST_F(BuildServiceTest, UpdatesStayInsideTheirOwnRoom) {
  CreateRoomWith(1, "alice");
  CreateRoomWith(2, "bob");  // 다른 방
  Equip(clients_[1]);
  ASSERT_EQ(DoBuild(service_, clients_[1], 2400.0f, 2000.0f).result(), protocol::GAME_OK);
  service_.Tick();
  EXPECT_EQ(clients_[1]->CountOf(MessageId::kStructureUpdate), 1u);
  EXPECT_EQ(clients_[2]->CountOf(MessageId::kStructureUpdate), 0u);
}

TEST_F(BuildServiceTest, ACatchUpTickStillDeliversTheCompletion) {
  CreateRoomWith(1, "alice");
  auto alice = clients_[1];
  Equip(alice);
  ASSERT_EQ(DoBuild(service_, alice, 2400.0f, 2000.0f).result(), protocol::GAME_OK);
  service_.Tick(3);
  ASSERT_EQ(alice->CountOf(MessageId::kStructureUpdate), 1u);
  service_.Tick(11999);  // 하루 끝을 넘는 따라잡기 틱
  EXPECT_EQ(alice->CountOf(MessageId::kDayStarted), 1u);
  ASSERT_EQ(alice->CountOf(MessageId::kStructureUpdate), 2u);
  EXPECT_EQ(alice->Last<protocol::StructureUpdate>(MessageId::kStructureUpdate).structures(0).state(), protocol::STRUCTURE_COMPLETE);
}

TEST_F(BuildServiceTest, ALeaverDuringUpdateDeliveryDoesNotBreakTheTick) {
  TwoPlayers();
  Equip(clients_[1]);
  clients_[1]->on_deliver = [this](MessageId id) {
    if (id == MessageId::kStructureUpdate) service_.OnDisconnect(2);
  };
  ASSERT_EQ(DoBuild(service_, clients_[1], 2400.0f, 2000.0f).result(), protocol::GAME_OK);
  service_.Tick();
  EXPECT_EQ(service_.RoomCodeOf(2), "");
  service_.Tick();
  EXPECT_EQ(service_.stats().current_players.load(), 1u);
}

TEST_F(BuildServiceTest, TheServerStopsAPlayerAtABuiltWallEvenWhenTheClientKeepsPushing) {
  CreateRoomWith(1, "alice");
  auto alice = clients_[1];
  Equip(alice);
  ASSERT_EQ(DoBuild(service_, alice, 2400.0f, 2000.0f).result(), protocol::GAME_OK);  // y 1850~2150

  protocol::MoveInput input;
  input.set_sequence(1);
  input.set_dir_x(0.0f);
  input.set_dir_y(-50.0f);  // 부풀린 방향(남쪽, 집 쪽)
  service_.HandleMoveInput(alice, input);
  float min_y = 1.0e9f;
  for (int i = 0; i < 80; ++i) {
    service_.Tick();
    const auto snapshot = alice->Last<protocol::WorldSnapshot>(MessageId::kWorldSnapshot);
    min_y = std::min(min_y, snapshot.players(0).y());
  }
  EXPECT_GE(min_y, 2189.99f) << "벽(2150) + 몸 반지름(40)";
  EXPECT_LE(min_y, 2200.0f) << "벽 앞까지는 간다";
}

TEST_F(BuildServiceTest, TwoBuildersOnTheSameSpotInTheSameTickExactlyOneSucceeds) {
  TwoPlayers();
  Equip(clients_[1]);
  Equip(clients_[2]);
  const auto first = DoBuild(service_, clients_[1], 2500.0f, 2000.0f);
  const auto second = DoBuild(service_, clients_[2], 2500.0f, 2000.0f);
  EXPECT_EQ(first.result(), protocol::GAME_OK);
  EXPECT_EQ(second.result(), protocol::GAME_BUILD_BLOCKED);
  EXPECT_EQ(second.wood(), 20u) << "진 사람은 비용을 내지 않는다";
}

// 건설 허용 거리를 맵 전체로 넓혀 구역 경계를 시험하는 설정(구역은 기본값: 맵 중앙 [1300, 3700])
class BuildZoneServiceTest : public GameServiceTest {
 protected:
  BuildZoneServiceTest() : GameServiceTest(MakeConfig()) {}
  static PlayConfig MakeConfig() {
    PlayConfig config;
    config.gather_range_cm = 100000.0f;
    config.gather_cooldown_ticks = 0;
    config.tree_hp = 1000;
    config.rock_hp = 1000;
    config.build_range_cm = 100000.0f;
    return config;
  }
};

TEST_F(BuildZoneServiceTest, OutsideTheBuildZoneHasItsOwnCodeAndKeepsTheInventory) {
  CreateRoomWith(1, "alice");
  auto alice = clients_[1];
  for (int i = 0; i < 20; ++i) ASSERT_EQ(DoGather(service_, alice, 1).result(), protocol::GAME_OK);
  for (int i = 0; i < 10; ++i) ASSERT_EQ(DoGather(service_, alice, 31).result(), protocol::GAME_OK);

  const auto outside = DoBuild(service_, alice, 1000.0f, 2500.0f);  // 발자국 850~1150: 구역(1300~) 밖
  EXPECT_EQ(outside.result(), protocol::GAME_BUILD_OUT_OF_ZONE);
  EXPECT_EQ(outside.wood(), 20u) << "거부 응답도 현재 인벤토리를 담는다";
  EXPECT_EQ(DoBuild(service_, alice, 1500.0f, 2500.0f).result(), protocol::GAME_OK) << "발자국 1350~1650은 구역 안";
  EXPECT_EQ(service_.stats().builds_rejected.load(), 1u);
}

TEST_F(BuildZoneServiceTest, EveryNodeInTheRoomStateLiesOutsideTheZone) {
  auto alice = NewClient(1);
  const auto state = Create(alice, "alice").state();
  ASSERT_GT(state.nodes_size(), 0);
  for (const auto& node : state.nodes()) {
    const bool inside = std::abs(node.x() - 2500.0f) < 1260.0f && std::abs(node.y() - 2500.0f) < 1260.0f;
    EXPECT_FALSE(inside) << node.node_id() << " (" << node.x() << ", " << node.y() << ")";
  }
}

TEST(GameServiceNoNodes, ARoomWithoutNodesWorks) {
  boost::asio::io_context io;
  PlayConfig config;
  config.tree_count = 0;
  config.rock_count = 0;
  GameService service(io, config);
  auto alice = std::make_shared<FakeClient>(1);
  protocol::CreateRoomRequest request;
  request.set_nickname("alice");
  service.HandleCreateRoom(alice, request);
  EXPECT_EQ(alice->Last<protocol::CreateRoomResponse>(MessageId::kCreateRoomResponse).state().nodes_size(), 0);
  EXPECT_EQ(DoGather(service, alice, 1).result(), protocol::GAME_NODE_NOT_FOUND);
  service.Tick();
  EXPECT_EQ(alice->CountOf(MessageId::kResourceUpdate), 0u);
}

// ============================================================ 하루와 잠 (docs/content-day.md)

// 하루가 200틱(bench_day_ticks)이고, 어디서든 쿨다운 없이 채집할 수 있고, 나무는 한 번에 고갈되는 설정.
class DayServiceTest : public GameServiceTest {
 protected:
  DayServiceTest() : GameServiceTest(MakeConfig()) {}
  static PlayConfig MakeConfig() {
    PlayConfig config;
    config.bench_day_ticks = 200;
    config.gather_range_cm = 100000.0f;
    config.gather_cooldown_ticks = 0;
    config.tree_hp = 1;
    return config;
  }

  // alice(1, 칸 0)가 방을 만들고 bob(2, 칸 1), 원하면 carol(3, 칸 2: 시작 집 바로 앞)이 들어온다.
  std::string Players(int count) {
    const std::string code = CreateRoomWith(1, "alice");
    const char* names[] = {"alice", "bob", "carol", "dave"};
    for (int i = 1; i < count; ++i) {
      auto client = NewClient(static_cast<std::uint64_t>(i + 1));
      EXPECT_EQ(Join(client, code, names[i]).result(), protocol::GAME_OK);
    }
    return code;
  }

  // 목재 20(나무 1~20번, 각 한 번에 고갈), 석재 10(돌 31번 8, 32번 2)을 모은다.
  void Equip(const ClientPtr& client) {
    for (std::uint32_t id = 1; id <= 20; ++id) ASSERT_EQ(DoGather(service_, client, id).result(), protocol::GAME_OK);
    for (int i = 0; i < 8; ++i) ASSERT_EQ(DoGather(service_, client, 31).result(), protocol::GAME_OK);
    for (int i = 0; i < 2; ++i) ASSERT_EQ(DoGather(service_, client, 32).result(), protocol::GAME_OK);
  }

  // client가 지금까지 받은 메시지 수(이후에 받은 것만 보려고 쓴다).
  static std::size_t Mark(const ClientPtr& client) { return client->received.size(); }
  static std::vector<MessageId> IdsSince(const ClientPtr& client, std::size_t mark) {
    std::vector<MessageId> ids;
    for (std::size_t i = mark; i < client->received.size(); ++i) ids.push_back(client->received[i].first);
    return ids;
  }
};

TEST_F(DayServiceTest, SleepAlwaysRepliesWithTheResultAndTheCurrentStateAndKeepsTheConnection) {
  auto loner = NewClient(9);
  auto outside = Sleep(loner, true);
  EXPECT_EQ(outside.result(), protocol::GAME_NOT_IN_ROOM);
  EXPECT_FALSE(outside.sleeping());
  EXPECT_EQ(Sleep(loner, false).result(), protocol::GAME_NOT_IN_ROOM);

  Players(1);
  auto alice = clients_[1];
  auto too_far = Sleep(alice, true);  // 칸 0은 시작 집에서 300 cm
  EXPECT_EQ(too_far.result(), protocol::GAME_SLEEP_TOO_FAR);
  EXPECT_FALSE(too_far.sleeping());
  WalkToFarmhouse({alice});
  auto ok = Sleep(alice, true);
  EXPECT_EQ(ok.result(), protocol::GAME_OK);
  EXPECT_TRUE(ok.sleeping());
  auto again = Sleep(alice, true);
  EXPECT_EQ(again.result(), protocol::GAME_OK) << "이미 잠들었으면 성공";
  EXPECT_TRUE(again.sleeping());
  auto awake = Sleep(alice, false);
  EXPECT_EQ(awake.result(), protocol::GAME_OK);
  EXPECT_FALSE(awake.sleeping());
  EXPECT_EQ(service_.stats().sleeps_ok.load(), 3u);
  EXPECT_EQ(service_.stats().sleeps_rejected.load(), 3u);
  EXPECT_EQ(service_.room_count(), 1u) << "연결도 방도 그대로";
}

TEST_F(DayServiceTest, SleepUpdateGoesToEveryoneOnlyOnTicksWhereTheListChanged) {
  Players(3);
  auto carol = clients_[3];  // 칸 2: 시작 집 바로 앞
  ASSERT_EQ(Sleep(carol, true).result(), protocol::GAME_OK);
  EXPECT_EQ(carol->CountOf(MessageId::kSleepUpdate), 0u) << "알림은 틱 끝에 간다";
  service_.Tick();
  for (std::uint64_t id = 1; id <= 3; ++id) {
    ASSERT_EQ(clients_[id]->CountOf(MessageId::kSleepUpdate), 1u) << "본인을 포함한 방 전원 " << id;
    const auto update = clients_[id]->Last<protocol::SleepUpdate>(MessageId::kSleepUpdate);
    ASSERT_EQ(update.sleeping_player_ids_size(), 1);
    EXPECT_EQ(update.sleeping_player_ids(0), 3u);
    EXPECT_EQ(update.tick(), 1u);
  }
  service_.Tick();
  service_.Tick();
  EXPECT_EQ(carol->CountOf(MessageId::kSleepUpdate), 1u) << "바뀌지 않았으면 보내지 않는다";
  Sleep(carol, false);
  Sleep(carol, true);  // 같은 틱 안에서 깼다가 다시 잔다: 목록은 그대로
  service_.Tick();
  EXPECT_EQ(carol->CountOf(MessageId::kSleepUpdate), 1u);
  Sleep(carol, false);
  service_.Tick();
  ASSERT_EQ(clients_[1]->CountOf(MessageId::kSleepUpdate), 2u);
  EXPECT_EQ(clients_[1]->Last<protocol::SleepUpdate>(MessageId::kSleepUpdate).sleeping_player_ids_size(), 0)
      << "모두 깨면 빈 목록";
}

TEST_F(DayServiceTest, WhenEveryoneSleepsTheNextTickStartsANewDayWithDayStartedSentFirst) {
  Players(2);
  auto alice = clients_[1];
  auto bob = clients_[2];
  Equip(alice);
  ASSERT_EQ(DoBuild(service_, alice, 2400.0f, 2000.0f).result(), protocol::GAME_OK);
  service_.Tick();  // 고갈된 노드와 건설 중인 집 알림
  WalkToFarmhouse({alice, bob});
  ASSERT_EQ(Sleep(alice, true).result(), protocol::GAME_OK);
  service_.Tick();
  EXPECT_EQ(alice->CountOf(MessageId::kDayStarted), 0u) << "한 명이 깨어 있으면 넘어가지 않는다";
  ASSERT_EQ(Sleep(bob, true).result(), protocol::GAME_OK);
  EXPECT_EQ(alice->CountOf(MessageId::kDayStarted), 0u) << "잠자기 요청 즉시가 아니라 다음 틱에 정산";

  const std::size_t alice_mark = Mark(alice);
  const std::size_t bob_mark = Mark(bob);
  service_.Tick();
  const std::vector<MessageId> expected = {MessageId::kDayStarted, MessageId::kWorldSnapshot, MessageId::kResourceUpdate,
                                           MessageId::kStructureUpdate, MessageId::kSleepUpdate};
  EXPECT_EQ(IdsSince(alice, alice_mark), expected) << "DayStarted가 가장 먼저, 그다음 스냅샷, 노드, 구조물, 잠든 사람";
  EXPECT_EQ(IdsSince(bob, bob_mark), expected);

  const auto night = bob->Last<protocol::DayStarted>(MessageId::kDayStarted);
  EXPECT_FALSE(night.forced());
  EXPECT_EQ(night.clock().day(), 2u);
  EXPECT_EQ(night.clock().day_start_tick(), static_cast<std::int64_t>(night.tick()));
  EXPECT_EQ(night.clock().day_length_ticks(), 200u);
  EXPECT_EQ(night.clock().ticks_per_game_minute(), 10u);
  EXPECT_EQ(night.clock().day_start_minute(), 360u);

  const auto morning = bob->Last<protocol::WorldSnapshot>(MessageId::kWorldSnapshot);
  EXPECT_EQ(morning.tick(), night.tick());
  ASSERT_EQ(morning.players_size(), 2);
  EXPECT_EQ(morning.players(0).x(), 2400.0f) << "스냅샷은 아침 위치";
  EXPECT_EQ(morning.players(0).y(), 2400.0f);
  EXPECT_EQ(morning.players(1).x(), 2600.0f);
  EXPECT_EQ(morning.players(1).y(), 2400.0f);
  EXPECT_EQ(morning.players(0).last_input_seq(), 1001u) << "입력 번호는 그대로";

  EXPECT_EQ(bob->Last<protocol::ResourceUpdate>(MessageId::kResourceUpdate).nodes_size(), 21) << "고갈된 노드 21개가 돌아온다";
  const auto built = bob->Last<protocol::StructureUpdate>(MessageId::kStructureUpdate);
  ASSERT_EQ(built.structures_size(), 1);
  EXPECT_EQ(built.structures(0).state(), protocol::STRUCTURE_COMPLETE);
  EXPECT_EQ(bob->Last<protocol::SleepUpdate>(MessageId::kSleepUpdate).sleeping_player_ids_size(), 0)
      << "밤 정산 뒤에는 잠든 사람이 없다";
  EXPECT_EQ(service_.stats().days_started.load(), 1u);
  EXPECT_EQ(service_.stats().forced_nights.load(), 0u);
  EXPECT_EQ(service_.FindRoom(service_.RoomCodeOf(1))->day(), 2u);

  // 깨어났으니 다시 움직이고 채집할 수 있다.
  Move(alice, 1002, 1.0f, 0.0f);
  service_.Tick();
  EXPECT_GT(alice->Last<protocol::WorldSnapshot>(MessageId::kWorldSnapshot).players(0).x(), 2400.0f);
  EXPECT_EQ(DoGather(service_, alice, 1).result(), protocol::GAME_OK);
}

TEST_F(DayServiceTest, OneAwakePlayerKeepsTheDayGoingUntilItIsForced) {
  Players(3);
  ASSERT_EQ(Sleep(clients_[3], true).result(), protocol::GAME_OK);
  for (int i = 0; i < 199; ++i) service_.Tick();
  EXPECT_EQ(clients_[1]->CountOf(MessageId::kDayStarted), 0u) << "경과 199틱: 깨어 있는 사람이 있으면 정산하지 않는다";
  service_.Tick();  // 200틱
  ASSERT_EQ(clients_[1]->CountOf(MessageId::kDayStarted), 1u) << "하루 끝(200틱)에 강제 정산";
  const auto night = clients_[1]->Last<protocol::DayStarted>(MessageId::kDayStarted);
  EXPECT_TRUE(night.forced());
  EXPECT_EQ(night.tick(), 200u);
  EXPECT_EQ(service_.stats().forced_nights.load(), 1u);
  for (int i = 0; i < 199; ++i) service_.Tick();
  EXPECT_EQ(clients_[1]->CountOf(MessageId::kDayStarted), 1u);
  service_.Tick();
  EXPECT_EQ(clients_[1]->CountOf(MessageId::kDayStarted), 2u) << "다음 하루도 200틱";
}

TEST_F(GameServiceTest, TheDefaultDayIsForcedAtExactly12000TicksWhereverThePlayerIs) {
  CreateRoomWith(1, "alice");
  auto alice = clients_[1];
  Move(alice, 1, -1.0f, -1.0f);  // 시작 집에서 멀어진다
  for (int i = 0; i < 11999; ++i) service_.Tick();
  EXPECT_EQ(alice->CountOf(MessageId::kDayStarted), 0u) << "경과 11,999틱에는 정산하지 않는다";
  service_.Tick();
  ASSERT_EQ(alice->CountOf(MessageId::kDayStarted), 1u) << "12,000틱에 강제 정산";
  const auto night = alice->Last<protocol::DayStarted>(MessageId::kDayStarted);
  EXPECT_TRUE(night.forced());
  EXPECT_EQ(night.tick(), 12000u);
  EXPECT_EQ(night.clock().day(), 2u);
  EXPECT_EQ(night.clock().day_length_ticks(), 12000u);
  const auto morning = alice->Last<protocol::WorldSnapshot>(MessageId::kWorldSnapshot);
  EXPECT_EQ(morning.players(0).x(), 2400.0f) << "깨어 있던 사람도 아침 위치로";
  EXPECT_EQ(morning.players(0).y(), 2400.0f);
}

TEST_F(DayServiceTest, TheLastAwakePlayerLeavingEndsTheDayOnTheNextTick) {
  Players(3);
  ASSERT_EQ(Sleep(clients_[3], true).result(), protocol::GAME_OK);
  service_.Tick();
  Leave(clients_[1]);
  service_.Tick();
  EXPECT_EQ(clients_[3]->CountOf(MessageId::kDayStarted), 0u) << "bob이 아직 깨어 있다";
  service_.OnDisconnect(2);  // 깨어 있던 마지막 사람의 연결이 끊긴다
  service_.Tick();
  ASSERT_EQ(clients_[3]->CountOf(MessageId::kDayStarted), 1u);
  EXPECT_FALSE(clients_[3]->Last<protocol::DayStarted>(MessageId::kDayStarted).forced());
}

TEST_F(DayServiceTest, AJoinerArrivingBeforeTheNextTickKeepsTheDayGoing) {
  const std::string code = Players(1);
  auto alice = clients_[1];
  WalkToFarmhouse({alice});
  ASSERT_EQ(Sleep(alice, true).result(), protocol::GAME_OK);
  auto bob = NewClient(2);
  ASSERT_EQ(Join(bob, code, "bob").result(), protocol::GAME_OK);  // 깨어 있는 채로 들어온다
  service_.Tick();
  EXPECT_EQ(alice->CountOf(MessageId::kDayStarted), 0u) << "새로 들어온 사람이 정산을 막는다";
}

TEST_F(DayServiceTest, ALateJoinerReceivesTheClockTheFarmhouseAndWhoIsAsleep) {
  const std::string code = Players(1);
  auto alice = clients_[1];
  const auto created = alice->Last<protocol::CreateRoomResponse>(MessageId::kCreateRoomResponse);
  EXPECT_EQ(created.state().clock().day(), 1u) << "새 방은 1일차";
  EXPECT_EQ(created.state().clock().day_start_tick(), 0);
  EXPECT_EQ(created.state().sleeping_player_ids_size(), 0);

  for (int i = 0; i < 200; ++i) service_.Tick();  // 강제 정산 한 번: 2일차, 200틱에 시작
  for (int i = 0; i < 30; ++i) service_.Tick();
  auto bob = NewClient(2);
  ASSERT_EQ(Join(bob, code, "bob").result(), protocol::GAME_OK);
  auto carol = NewClient(3);
  ASSERT_EQ(Join(carol, code, "carol").result(), protocol::GAME_OK);
  ASSERT_EQ(Sleep(carol, true).result(), protocol::GAME_OK);

  auto dave = NewClient(4);
  const auto joined = Join(dave, code, "dave");
  ASSERT_EQ(joined.result(), protocol::GAME_OK);
  const auto& clock = joined.state().clock();
  EXPECT_EQ(clock.day(), 2u);
  EXPECT_EQ(clock.day_start_tick(), 200);
  EXPECT_EQ(clock.day_length_ticks(), 200u);
  EXPECT_EQ(clock.ticks_per_game_minute(), 10u);
  EXPECT_EQ(clock.day_start_minute(), 360u);
  ASSERT_EQ(joined.state().landmarks_size(), 1);
  const auto& farmhouse = joined.state().landmarks(0);
  EXPECT_EQ(farmhouse.kind(), protocol::LANDMARK_FARMHOUSE);
  EXPECT_EQ(farmhouse.x(), 2500.0f);
  EXPECT_EQ(farmhouse.y(), 2850.0f);
  EXPECT_EQ(farmhouse.width(), 400.0f);
  EXPECT_EQ(farmhouse.height(), 300.0f);
  ASSERT_EQ(joined.state().sleeping_player_ids_size(), 1);
  EXPECT_EQ(joined.state().sleeping_player_ids(0), 3u) << "지금 잠든 사람";
}

TEST(GameServiceNoFarmhouse, ARoomWithoutAFarmhouseSendsNoLandmarks) {
  boost::asio::io_context io;
  PlayConfig config;
  config.farmhouse_width_cm = 0.0f;
  GameService service(io, config);
  auto alice = std::make_shared<FakeClient>(1);
  protocol::CreateRoomRequest request;
  request.set_nickname("alice");
  service.HandleCreateRoom(alice, request);
  EXPECT_EQ(alice->Last<protocol::CreateRoomResponse>(MessageId::kCreateRoomResponse).state().landmarks_size(), 0);
}

TEST_F(DayServiceTest, ANightInOneRoomDoesNotTouchAnotherRoom) {
  CreateRoomWith(1, "alice");
  const std::string other = CreateRoomWith(2, "bob");
  WalkToFarmhouse({clients_[1]});
  ASSERT_EQ(Sleep(clients_[1], true).result(), protocol::GAME_OK);
  service_.Tick();
  EXPECT_EQ(clients_[1]->CountOf(MessageId::kDayStarted), 1u);
  EXPECT_EQ(clients_[2]->CountOf(MessageId::kDayStarted), 0u) << "다른 방에는 알림이 없다";
  EXPECT_EQ(clients_[2]->CountOf(MessageId::kSleepUpdate), 0u);
  EXPECT_EQ(service_.FindRoom(other)->day(), 1u);
}

TEST_F(DayServiceTest, MoveInputsFromASleeperAreIgnoredAndCounted) {
  Players(3);
  auto carol = clients_[3];
  ASSERT_EQ(Sleep(carol, true).result(), protocol::GAME_OK);
  const std::uint64_t ignored = service_.stats().inputs_ignored.load();
  Move(carol, 5, 1.0f, 0.0f);
  EXPECT_EQ(service_.stats().inputs_ignored.load(), ignored + 1);
  for (int i = 0; i < 5; ++i) service_.Tick();
  const auto snapshot = carol->Last<protocol::WorldSnapshot>(MessageId::kWorldSnapshot);
  EXPECT_EQ(snapshot.players(2).x(), 2400.0f) << "잠든 사람은 움직이지 않는다";
  EXPECT_EQ(snapshot.players(2).last_input_seq(), 0u) << "입력 번호도 반영하지 않는다";
}

TEST_F(DayServiceTest, GatheringAndBuildingWhileAsleepGetTheirOwnCode) {
  Players(3);
  auto carol = clients_[3];
  ASSERT_EQ(DoGather(service_, carol, 31).result(), protocol::GAME_OK);
  ASSERT_EQ(Sleep(carol, true).result(), protocol::GAME_OK);
  const auto gathered = DoGather(service_, carol, 31);
  EXPECT_EQ(gathered.result(), protocol::GAME_SLEEPING);
  EXPECT_EQ(gathered.stone(), 1u) << "거부 응답도 현재 인벤토리를 담는다";
  const auto built = DoBuild(service_, carol, 2400.0f, 2000.0f);
  EXPECT_EQ(built.result(), protocol::GAME_SLEEPING);
  EXPECT_EQ(built.stone(), 1u);
  service_.Tick();
  EXPECT_EQ(carol->CountOf(MessageId::kStructureUpdate), 0u);
  EXPECT_EQ(service_.stats().gathers_rejected.load(), 1u);
  EXPECT_EQ(service_.stats().builds_rejected.load(), 1u);
}

TEST_F(DayServiceTest, ADisconnectWhileDayStartedIsBeingDeliveredIsSafe) {
  Players(3);
  clients_[1]->on_deliver = [this](MessageId id) {
    if (id == MessageId::kDayStarted) service_.OnDisconnect(2);  // 알림을 보내는 중 다른 사람의 연결이 닫힌다
  };
  clients_[3]->on_deliver = [this](MessageId id) {
    if (id == MessageId::kDayStarted) service_.HandleLeaveRoom(clients_[3]);  // 정산 틱에 자기가 나간다
  };
  for (int i = 0; i < 200; ++i) service_.Tick();  // 강제 정산
  EXPECT_EQ(clients_[1]->CountOf(MessageId::kDayStarted), 1u);
  EXPECT_EQ(service_.RoomCodeOf(2), "");
  EXPECT_EQ(service_.RoomCodeOf(3), "");
  service_.Tick();
  EXPECT_EQ(service_.stats().current_players.load(), 1u);
  EXPECT_EQ(service_.room_count(), 1u);
}

TEST_F(DayServiceTest, ASleeperDisconnectingIsSafeAndOnlyTheLeftoverSleepersCount) {
  Players(3);
  ASSERT_EQ(Sleep(clients_[3], true).result(), protocol::GAME_OK);
  service_.Tick();
  service_.OnDisconnect(3);  // 잠든 채로 끊김
  service_.Tick();
  EXPECT_EQ(clients_[1]->CountOf(MessageId::kDayStarted), 0u) << "나간 잠든 사람은 정산을 일으키지 않는다";
  ASSERT_EQ(clients_[1]->CountOf(MessageId::kSleepUpdate), 2u);
  EXPECT_EQ(clients_[1]->Last<protocol::SleepUpdate>(MessageId::kSleepUpdate).sleeping_player_ids_size(), 0);
  EXPECT_EQ(Sleep(clients_[3], true).result(), protocol::GAME_NOT_IN_ROOM);
}

TEST_F(DayServiceTest, TheStatsLineShowsTheNights) {
  Players(1);
  for (int i = 0; i < 200; ++i) service_.Tick();
  const std::string summary = service_.stats().Summary();
  EXPECT_NE(summary.find("하루{정산=1 강제=1 "), std::string::npos) << summary;
  EXPECT_NE(summary.find("틱시간{이동="), std::string::npos) << "기존 묶음 모양은 그대로";
}

}  // namespace
}  // namespace game::play
