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
  for (int i = 0; i < 20; ++i) service_.Tick();  // 20틱 = 1초 = 300 cm

  const auto from_bob = bob->Last<protocol::WorldSnapshot>(MessageId::kWorldSnapshot);
  EXPECT_EQ(from_bob.tick(), 20u);
  EXPECT_NEAR(from_bob.players(0).x(), 2700.0f, 0.01f);
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
  EXPECT_NEAR(snapshot.players(0).x(), 2700.0f, 0.01f);
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
  EXPECT_NEAR(snapshot.players(0).x(), 2700.0f, 0.01f);  // 첫 입력만 반영되었다
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
  EXPECT_NEAR(snapshot.players(0).x(), 2400.0f + 45.0f, 0.01f);  // 3틱 x 15 cm
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
  EXPECT_NEAR(snapshot.players(0).x(), 3015.0f, 0.01f);  // 계속 걸어간 alice (41틱 x 15 cm)
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

TEST_F(GatherServiceTest, DepletedNodeComesBackAfterExactlyTheRespawnTicksAndTheRoomIsTold) {
  TwoPlayers();
  auto bob = clients_[2];
  ASSERT_EQ(DoGather(service_, clients_[1], 1).result(), protocol::GAME_OK);
  service_.Tick();  // 1틱: 고갈 알림
  ASSERT_EQ(bob->CountOf(MessageId::kResourceUpdate), 1u);

  const PlayConfig config = service_.config();
  // 고갈은 월드 틱 0에서 일어났으므로 600틱째에 재생한다. 위에서 1틱 지났으니 598틱을 더 돌리면 599틱.
  for (std::uint32_t i = 0; i < config.respawn_ticks - 2; ++i) service_.Tick();
  EXPECT_EQ(bob->CountOf(MessageId::kResourceUpdate), 1u) << "599틱까지는 알림이 없다";
  EXPECT_EQ(DoGather(service_, clients_[1], 1).result(), protocol::GAME_NODE_DEPLETED);
  service_.Tick();  // 600틱
  ASSERT_EQ(bob->CountOf(MessageId::kResourceUpdate), 2u);
  const auto update = bob->Last<protocol::ResourceUpdate>(MessageId::kResourceUpdate);
  ASSERT_EQ(update.nodes_size(), 1);
  EXPECT_EQ(update.nodes(0).node_id(), 1u);
  EXPECT_EQ(update.nodes(0).hp(), 1u);
  EXPECT_EQ(update.tick(), config.respawn_ticks);
  EXPECT_EQ(service_.stats().nodes_respawned.load(), 1u);
  EXPECT_EQ(DoGather(service_, clients_[1], 1).result(), protocol::GAME_OK) << "재생한 노드는 다시 칠 수 있다";
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

  const std::uint32_t respawn = service_.config().respawn_ticks;
  service_.Tick(respawn - 3);  // 정확히 재생 틱까지 한꺼번에
  ASSERT_EQ(alice->CountOf(MessageId::kResourceUpdate), 2u);
  EXPECT_EQ(alice->Last<protocol::ResourceUpdate>(MessageId::kResourceUpdate).nodes(0).hp(), 1u);
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

}  // namespace
}  // namespace game::play
