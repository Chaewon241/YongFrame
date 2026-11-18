// 게임 통합 테스트: 테스트 프로세스 안에서 진짜 서버를 띄우고 실제 TCP로 방 만들기, 입장, 이동, 스냅샷,
// 연결 끊김, 비정상 입력을 검증한다. 틱은 실제 타이머(20Hz)로 돈다.
//
// 시간에 의존하는 값(걸린 시간)은 정확히 단정하지 않고, 서버가 보장하는 성질(틱당 이동 거리 상한,
// 입력 번호 반영, 방별 격리)로 검증한다.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

#include "game_test_client.h"

namespace game {
namespace {

using namespace testing_support;

const ::testing::Environment* const kQuiet = ::testing::AddGlobalTestEnvironment(new QuietLogEnvironment);

// 사람 한 명이 접속해 방에 들어간 상태를 묶어 둔 도우미.
struct Player {
  std::unique_ptr<BlockingClient> client;
  std::uint32_t id = 0;
};

Player CreateRoomPlayer(const TestServer& server, const std::string& nickname, std::string* code_out) {
  Player player;
  player.client = Connect(server);
  const auto response = CreateRoom(*player.client, nickname);
  EXPECT_EQ(response.result(), protocol::GAME_OK);
  player.id = response.state().your_player_id();
  if (code_out) *code_out = response.state().room_code();
  return player;
}

Player JoinRoomPlayer(const TestServer& server, const std::string& code, const std::string& nickname) {
  Player player;
  player.client = Connect(server);
  const auto response = JoinRoom(*player.client, code, nickname);
  EXPECT_EQ(response.result(), protocol::GAME_OK) << nickname;
  player.id = response.state().your_player_id();
  return player;
}

// ============================================================ 방 만들기와 입장

TEST(GameIntegration, CreateAndJoinWithTheRoomCodeOverTcp) {
  TestServer server;
  auto alice = Connect(server);
  const auto created = CreateRoom(*alice, "alice");
  ASSERT_EQ(created.result(), protocol::GAME_OK);
  const std::string code = created.state().room_code();
  ASSERT_EQ(code.size(), 6u);
  EXPECT_EQ(created.state().your_player_id(), created.state().host_player_id());

  auto bob = Connect(server);
  const auto joined = JoinRoom(*bob, code, "bob");
  ASSERT_EQ(joined.result(), protocol::GAME_OK);
  EXPECT_EQ(joined.state().room_code(), code);
  EXPECT_EQ(joined.state().players_size(), 2);
  EXPECT_NE(joined.state().your_player_id(), joined.state().host_player_id());

  protocol::RoomEvent event;
  ASSERT_TRUE(WaitEvent(*alice, protocol::RoomEvent::PLAYER_JOINED, event));
  EXPECT_EQ(event.player().nickname(), "bob");
  EXPECT_EQ(event.player().player_id(), joined.state().your_player_id());

  EXPECT_EQ(server.game_stats().current_rooms.load(), 1u);
  EXPECT_EQ(server.game_stats().current_players.load(), 2u);
}

TEST(GameIntegration, RoomCodesAreUniqueSixDigitNumbers) {
  TestServer server;
  std::set<std::string> codes;
  std::vector<Player> players;
  for (int i = 0; i < 25; ++i) {
    std::string code;
    players.push_back(CreateRoomPlayer(server, "p" + std::to_string(i), &code));
    ASSERT_EQ(code.size(), 6u);
    for (char c : code) ASSERT_TRUE(c >= '0' && c <= '9') << code;
    codes.insert(code);
  }
  EXPECT_EQ(codes.size(), 25u);
  EXPECT_EQ(server.game_stats().current_rooms.load(), 25u);
}

// 규칙 위반은 결과 코드로 알리고 연결은 그대로 쓸 수 있다.
TEST(GameIntegration, RuleViolationsReturnCodesAndKeepTheConnection) {
  TestServer server;
  std::string code;
  auto alice = CreateRoomPlayer(server, "alice", &code);

  auto bob = Connect(server);
  EXPECT_EQ(JoinRoom(*bob, "12345", "bob").result(), protocol::GAME_INVALID_ROOM_CODE);
  EXPECT_EQ(JoinRoom(*bob, "abcdef", "bob").result(), protocol::GAME_INVALID_ROOM_CODE);
  EXPECT_EQ(JoinRoom(*bob, code == "000000" ? "999999" : "000000", "bob").result(), protocol::GAME_ROOM_NOT_FOUND);
  EXPECT_EQ(JoinRoom(*bob, code, " bob").result(), protocol::GAME_INVALID_NICKNAME);
  EXPECT_EQ(JoinRoom(*bob, code, "alice").result(), protocol::GAME_NICKNAME_TAKEN);
  EXPECT_EQ(LeaveRoom(*bob).result(), protocol::GAME_NOT_IN_ROOM);
  EXPECT_EQ(CreateRoom(*bob, "").result(), protocol::GAME_INVALID_NICKNAME);

  // 같은 연결로 계속 쓸 수 있다.
  EXPECT_EQ(JoinRoom(*bob, code, "bob").result(), protocol::GAME_OK);
  EXPECT_EQ(JoinRoom(*bob, code, "bob2").result(), protocol::GAME_ALREADY_IN_ROOM);
  EXPECT_EQ(CreateRoom(*bob, "bob3").result(), protocol::GAME_ALREADY_IN_ROOM);
  EXPECT_EQ(Load(server.stats().unknown_message_ids), 0u);
  EXPECT_EQ(Load(server.stats().parse_errors), 0u);
}

TEST(GameIntegration, RoomHoldsAtMostFourPlayers) {
  TestServer server;
  std::string code;
  std::vector<Player> players;
  players.push_back(CreateRoomPlayer(server, "p0", &code));
  for (int i = 1; i < 4; ++i) players.push_back(JoinRoomPlayer(server, code, "p" + std::to_string(i)));

  auto fifth = Connect(server);
  EXPECT_EQ(JoinRoom(*fifth, code, "p4").result(), protocol::GAME_ROOM_FULL);

  ASSERT_EQ(LeaveRoom(*players[3].client).result(), protocol::GAME_OK);
  EXPECT_EQ(JoinRoom(*fifth, code, "p4").result(), protocol::GAME_OK);
}

// ============================================================ 이동과 스냅샷

TEST(GameIntegration, BothPlayersSeeEachOthersMovementFromTheServer) {
  TestServer server;
  std::string code;
  auto alice = CreateRoomPlayer(server, "alice", &code);
  auto bob = JoinRoomPlayer(server, code, "bob");

  ASSERT_TRUE(SendMove(*alice.client, 1, 1.0f, 0.0f));

  // bob이 받는 스냅샷에서 alice가 오른쪽으로 150 cm 이상 걸어간 것을 본다. 서버가 계산한 위치다.
  protocol::WorldSnapshot seen_by_bob;
  ASSERT_TRUE(WaitSnapshot(*bob.client, seen_by_bob, [&](const protocol::WorldSnapshot& s) {
    const auto* p = FindPlayer(s, alice.id);
    return p != nullptr && p->x() >= 2400.0f + 150.0f;
  }));
  const auto* alice_in_bob = FindPlayer(seen_by_bob, alice.id);
  const auto* bob_in_bob = FindPlayer(seen_by_bob, bob.id);
  ASSERT_NE(alice_in_bob, nullptr);
  ASSERT_NE(bob_in_bob, nullptr);
  EXPECT_FLOAT_EQ(alice_in_bob->y(), 2400.0f);
  EXPECT_EQ(alice_in_bob->last_input_seq(), 1u);
  EXPECT_FLOAT_EQ(bob_in_bob->x(), 2600.0f);  // 움직이지 않은 bob은 제자리

  // alice 본인도 같은 방 상태를 받는다.
  protocol::WorldSnapshot seen_by_alice;
  ASSERT_TRUE(WaitSnapshot(*alice.client, seen_by_alice, [&](const protocol::WorldSnapshot& s) {
    const auto* p = FindPlayer(s, alice.id);
    return p != nullptr && p->x() >= 2400.0f + 150.0f;
  }));
  EXPECT_EQ(seen_by_alice.players_size(), 2);

  // 정지 입력을 보내면 위치가 멈춘다.
  ASSERT_TRUE(SendMove(*alice.client, 2, 0.0f, 0.0f));
  protocol::WorldSnapshot stopped;
  ASSERT_TRUE(WaitSnapshot(*bob.client, stopped, [&](const protocol::WorldSnapshot& s) {
    const auto* p = FindPlayer(s, alice.id);
    return p != nullptr && p->last_input_seq() == 2;
  }));
  const float stop_x = FindPlayer(stopped, alice.id)->x();
  for (int i = 0; i < 3; ++i) {
    protocol::WorldSnapshot later;
    ASSERT_TRUE(WaitSnapshot(*bob.client, later, [&](const protocol::WorldSnapshot& s) { return s.tick() > stopped.tick(); }));
    EXPECT_FLOAT_EQ(FindPlayer(later, alice.id)->x(), stop_x);
  }
}

// 서버 판정: 클라이언트가 방향 벡터를 부풀려도 틱당 이동 거리(15 cm)를 넘을 수 없다.
TEST(GameIntegration, ClientCannotMoveFasterThanTheServerAllows) {
  TestServer server;
  std::string code;
  auto alice = CreateRoomPlayer(server, "alice", &code);
  auto bob = JoinRoomPlayer(server, code, "bob");

  ASSERT_TRUE(SendMove(*alice.client, 1, 1000.0f, 0.0f));

  protocol::WorldSnapshot previous;
  ASSERT_TRUE(WaitSnapshot(*bob.client, previous, [&](const protocol::WorldSnapshot& s) {
    const auto* p = FindPlayer(s, alice.id);
    return p != nullptr && p->last_input_seq() == 1;
  }));
  const float start_x = FindPlayer(previous, alice.id)->x();
  const std::uint64_t start_tick = previous.tick();
  float max_dx_per_tick = 0.0f;
  for (int i = 0; i < 12; ++i) {
    protocol::WorldSnapshot next;
    ASSERT_TRUE(WaitSnapshot(*bob.client, next, [&](const protocol::WorldSnapshot& s) { return s.tick() > previous.tick(); }));
    const float dx = FindPlayer(next, alice.id)->x() - FindPlayer(previous, alice.id)->x();
    const auto ticks = static_cast<float>(next.tick() - previous.tick());
    max_dx_per_tick = std::max(max_dx_per_tick, dx / ticks);
    EXPECT_LE(dx, 15.0f * ticks + 0.01f) << "틱 " << previous.tick() << " -> " << next.tick();
    previous = next;
  }
  // 움직이기는 했고(입력이 반영됨), 틱당 15 cm(300 cm/s)를 넘지 않았다.
  EXPECT_GT(FindPlayer(previous, alice.id)->x(), start_x + 50.0f);
  EXPECT_LE(max_dx_per_tick, 15.01f);
  EXPECT_LE(FindPlayer(previous, alice.id)->x() - start_x, 15.0f * static_cast<float>(previous.tick() - start_tick) + 0.01f);
}

TEST(GameIntegration, SnapshotsAreIsolatedPerRoom) {
  TestServer server;
  std::string code_a;
  std::string code_b;
  auto alice = CreateRoomPlayer(server, "alice", &code_a);
  auto carol = CreateRoomPlayer(server, "carol", &code_b);  // 다른 방. player_id도 1로 같다.
  ASSERT_NE(code_a, code_b);

  ASSERT_TRUE(SendMove(*alice.client, 1, 1.0f, 0.0f));
  protocol::WorldSnapshot moved;
  ASSERT_TRUE(WaitSnapshot(*alice.client, moved, [&](const protocol::WorldSnapshot& s) {
    return FindPlayer(s, alice.id)->x() > 2500.0f;
  }));

  protocol::WorldSnapshot other;
  ASSERT_TRUE(WaitSnapshot(*carol.client, other, [](const protocol::WorldSnapshot&) { return true; }));
  ASSERT_EQ(other.players_size(), 1);  // 다른 방 사람은 보이지 않는다
  EXPECT_FLOAT_EQ(other.players(0).x(), 2400.0f);  // 다른 방의 이동에 영향받지 않는다
}

// ============================================================ 나가기와 연결 끊김

TEST(GameIntegration, LeavingNotifiesTheOthersAndHostMigrates) {
  TestServer server;
  std::string code;
  auto alice = CreateRoomPlayer(server, "alice", &code);  // 방장
  auto bob = JoinRoomPlayer(server, code, "bob");
  auto carol = JoinRoomPlayer(server, code, "carol");

  ASSERT_EQ(LeaveRoom(*alice.client).result(), protocol::GAME_OK);

  for (auto* player : {&bob, &carol}) {
    protocol::RoomEvent left;
    ASSERT_TRUE(WaitEvent(*player->client, protocol::RoomEvent::PLAYER_LEFT, left));
    EXPECT_EQ(left.player().nickname(), "alice");
    protocol::RoomEvent host;
    ASSERT_TRUE(WaitEvent(*player->client, protocol::RoomEvent::HOST_CHANGED, host));
    EXPECT_EQ(host.host_player_id(), bob.id);  // 먼저 들어온 bob
  }

  // 나간 alice의 연결은 계속 쓸 수 있다(나가기 전에 이미 큐에 있던 스냅샷은 건너뛴다).
  EXPECT_TRUE(PingWorksInGame(*alice.client));
  EXPECT_EQ(server.game_stats().current_players.load(), 2u);
}

TEST(GameIntegration, AbruptDisconnectCleansUpTheRoomAndNotifiesTheRest) {
  TestServer server;
  std::string code;
  auto alice = CreateRoomPlayer(server, "alice", &code);
  auto bob = JoinRoomPlayer(server, code, "bob");
  auto carol = JoinRoomPlayer(server, code, "carol");

  bob.client->Close();  // 나가기 요청 없이 소켓만 닫는다
  protocol::RoomEvent event;
  ASSERT_TRUE(WaitEvent(*alice.client, protocol::RoomEvent::PLAYER_LEFT, event));
  EXPECT_EQ(event.player().nickname(), "bob");
  EXPECT_TRUE(WaitUntil([&] { return server.game_stats().current_players.load() == 2; }));

  alice.client->Close();  // 방장이 끊겼다
  protocol::RoomEvent host;
  ASSERT_TRUE(WaitEvent(*carol.client, protocol::RoomEvent::HOST_CHANGED, host));
  EXPECT_EQ(host.host_player_id(), carol.id);

  carol.client->Close();
  EXPECT_TRUE(WaitUntil([&] { return server.game_stats().current_rooms.load() == 0; }));
  EXPECT_EQ(server.game_stats().rooms_closed.load(), 1u);
  EXPECT_EQ(server.game_stats().current_players.load(), 0u);

  // 닫힌 방에는 들어갈 수 없다. 서버는 살아 있다.
  auto late = Connect(server);
  EXPECT_EQ(JoinRoom(*late, code, "late").result(), protocol::GAME_ROOM_NOT_FOUND);
  EXPECT_TRUE(ServerStillWorks(server));
}

// 채팅 방과 게임 방 양쪽에 들어가 있던 연결이 끊기면 둘 다에서 정리된다.
TEST(GameIntegration, DisconnectLeavesBothTheChatRoomAndTheGameRoom) {
  TestServer server;
  auto client = Connect(server);
  ASSERT_TRUE(JoinOk(*client, "alice"));  // 채팅 입장
  ASSERT_EQ(CreateRoom(*client, "alice").result(), protocol::GAME_OK);
  EXPECT_EQ(server.chat_stats().current_members.load(), 1u);
  EXPECT_EQ(server.game_stats().current_rooms.load(), 1u);

  client->Close();
  EXPECT_TRUE(WaitUntil([&] { return server.chat_stats().current_members.load() == 0; }));
  EXPECT_TRUE(WaitUntil([&] { return server.game_stats().current_rooms.load() == 0; }));
}

TEST(GameIntegration, ChatAndGameMessagesCanBeUsedOnTheSameConnection) {
  TestServer server;
  auto client = Connect(server);
  ASSERT_TRUE(JoinOk(*client, "alice"));  // 채팅 입장
  ASSERT_EQ(CreateRoom(*client, "alice").result(), protocol::GAME_OK);  // 게임 방도 만든다

  // 게임 방에 있으면 스냅샷이 끼어들지만 채팅 응답은 그대로 받을 수 있다.
  protocol::ChatRequest request;
  request.set_text("hello");
  protocol::ChatResponse response;
  ASSERT_TRUE(SendProto(*client, MessageId::kChatRequest, request));
  ASSERT_TRUE(Receive(*client, MessageId::kChatResponse, response, kGameSkip));
  EXPECT_EQ(response.result(), protocol::RESULT_OK);
  EXPECT_TRUE(PingWorksInGame(*client));
}

// ============================================================ 견고함

TEST(GameIntegration, MoveInputOutsideARoomIsIgnoredAndDoesNotCloseTheConnection) {
  TestServer server;
  auto client = Connect(server);
  ASSERT_TRUE(SendMove(*client, 1, 1.0f, 0.0f));
  ASSERT_TRUE(SendMove(*client, 2, 1.0f, 0.0f));
  EXPECT_TRUE(PingWorks(*client));  // 연결이 살아 있고 응답이 온다
  EXPECT_TRUE(WaitUntil([&] { return server.game_stats().inputs_ignored.load() == 2; }));
  EXPECT_EQ(Load(server.stats().unknown_message_ids), 0u);
}

TEST(GameIntegration, InvalidAndStaleMoveInputsAreIgnored) {
  TestServer server;
  std::string code;
  auto alice = CreateRoomPlayer(server, "alice", &code);
  ASSERT_TRUE(SendMove(*alice.client, 5, 1.0f, 0.0f));
  ASSERT_TRUE(SendMove(*alice.client, 5, 0.0f, 1.0f));                         // 중복 번호
  ASSERT_TRUE(SendMove(*alice.client, 6, std::nanf(""), 0.0f));                // NaN
  ASSERT_TRUE(SendMove(*alice.client, 7, INFINITY, 0.0f));                     // 무한대
  EXPECT_TRUE(WaitUntil([&] { return server.game_stats().inputs_ignored.load() == 3; }));
  EXPECT_EQ(server.game_stats().inputs_applied.load(), 1u);

  protocol::WorldSnapshot snapshot;
  ASSERT_TRUE(WaitSnapshot(*alice.client, snapshot, [&](const protocol::WorldSnapshot& s) {
    return FindPlayer(s, alice.id)->x() > 2450.0f;
  }));
  EXPECT_FLOAT_EQ(FindPlayer(snapshot, alice.id)->y(), 2400.0f);  // 버린 입력의 방향은 반영되지 않았다
  EXPECT_TRUE(std::isfinite(FindPlayer(snapshot, alice.id)->x()));
}

// 게임 요청의 본문이 Protobuf로 해석되지 않으면 기존 규칙대로 연결을 닫는다.
class BadGamePayloadTest : public ::testing::TestWithParam<MessageId> {};

TEST_P(BadGamePayloadTest, UnparsableRequestClosesTheConnection) {
  TestServer server;
  auto client = Connect(server);
  const std::vector<std::uint8_t> garbage{0x08, 0xFF, 0xFF};  // 끝나지 않는 varint
  ASSERT_TRUE(client->SendFrame(protocol::ToUint16(GetParam()), garbage));
  EXPECT_TRUE(client->WaitForServerClose(kTimeout));
  EXPECT_TRUE(WaitUntil([&] { return Load(server.stats().parse_errors) == 1; }));
  EXPECT_TRUE(ServerStillWorks(server));
}

INSTANTIATE_TEST_SUITE_P(Ids, BadGamePayloadTest,
                         ::testing::Values(MessageId::kCreateRoomRequest, MessageId::kJoinRoomRequest,
                                           MessageId::kLeaveRoomRequest, MessageId::kMoveInput,
                                           MessageId::kGatherRequest, MessageId::kBuildRequest));

// 서버가 보내는 쪽 메시지를 클라이언트가 보내면 알 수 없는 ID와 똑같이 연결을 닫는다.
class GameServerOnlyIdTest : public ::testing::TestWithParam<MessageId> {};

TEST_P(GameServerOnlyIdTest, ServerToClientIdsAreRejected) {
  TestServer server;
  auto client = Connect(server);
  ASSERT_TRUE(client->SendFrame(protocol::ToUint16(GetParam()), std::vector<std::uint8_t>{}));
  EXPECT_TRUE(client->WaitForServerClose(kTimeout));
  EXPECT_TRUE(WaitUntil([&] { return Load(server.stats().unknown_message_ids) == 1; }));
  EXPECT_TRUE(ServerStillWorks(server));
}

INSTANTIATE_TEST_SUITE_P(Ids, GameServerOnlyIdTest,
                         ::testing::Values(MessageId::kCreateRoomResponse, MessageId::kJoinRoomResponse,
                                           MessageId::kLeaveRoomResponse, MessageId::kRoomEvent,
                                           MessageId::kWorldSnapshot, MessageId::kGatherResponse,
                                           MessageId::kResourceUpdate, MessageId::kBuildResponse,
                                           MessageId::kStructureUpdate));

// ============================================================ 집 짓기와 이동 충돌 (docs/content-roadmap.md)

// 자원을 빨리 모을 수 있고(쿨다운 없음, 체력 넉넉) 건설이 빨리 끝나는 서버.
server::ServerConfig BuildServerConfig(std::uint32_t build_ticks = 10) {
  server::ServerConfig config = DefaultServerConfig();
  config.play.gather_range_cm = 100000.0f;
  config.play.gather_cooldown_ticks = 0;
  config.play.tree_hp = 1000;
  config.play.rock_hp = 1000;
  config.play.build_ticks = build_ticks;
  return config;
}

// 목재 20, 석재 10을 모은다(나무는 1~30번, 돌은 31~50번).
void Equip(BlockingClient& client, int wood = 20, int stone = 10) {
  for (int i = 0; i < wood; ++i) ASSERT_EQ(Gather(client, 1).result(), protocol::GAME_OK);
  for (int i = 0; i < stone; ++i) ASSERT_EQ(Gather(client, 31).result(), protocol::GAME_OK);
}

TEST(GameIntegration, BuildingOverTcpTellsTheRoomAndLateJoinersAndCompletes) {
  TestServer server(BuildServerConfig());
  std::string code;
  auto alice = CreateRoomPlayer(server, "alice", &code);
  auto bob = JoinRoomPlayer(server, code, "bob");
  Equip(*alice.client);

  const auto built = Build(*alice.client, 2400.0f, 2800.0f);
  ASSERT_EQ(built.result(), protocol::GAME_OK);
  EXPECT_EQ(built.structure_id(), 1u);
  EXPECT_EQ(built.wood(), 0u);
  EXPECT_EQ(built.stone(), 0u);

  protocol::StructureUpdate update;
  const auto is_state = [](protocol::StructureState state) {
    return [state](const protocol::StructureUpdate& u) {
      return u.structures_size() == 1 && u.structures(0).structure_id() == 1 && u.structures(0).state() == state;
    };
  };
  EXPECT_TRUE(WaitStructureUpdate(*alice.client, update, is_state(protocol::STRUCTURE_BUILDING))) << "짓는 본인도 알림";
  EXPECT_EQ(update.structures(0).x(), 2400.0f);
  EXPECT_EQ(update.structures(0).y(), 2800.0f);
  EXPECT_EQ(update.structures(0).width(), 300.0f);
  EXPECT_TRUE(WaitStructureUpdate(*bob.client, update, is_state(protocol::STRUCTURE_BUILDING)));
  EXPECT_TRUE(WaitStructureUpdate(*alice.client, update, is_state(protocol::STRUCTURE_COMPLETE))) << "실제 타이머로 완공";
  EXPECT_TRUE(WaitStructureUpdate(*bob.client, update, is_state(protocol::STRUCTURE_COMPLETE)));

  auto carol = Connect(server);
  const auto joined = JoinRoom(*carol, code, "carol");
  ASSERT_EQ(joined.result(), protocol::GAME_OK);
  ASSERT_EQ(joined.state().structures_size(), 1);
  EXPECT_EQ(joined.state().structures(0).state(), protocol::STRUCTURE_COMPLETE) << "늦은 입장자는 현재 상태를 받는다";
  EXPECT_EQ(server.game_stats().builds_ok.load(), 1u);
  EXPECT_EQ(server.game_stats().structures_completed.load(), 1u);
}

TEST(GameIntegration, BuildRejectionsKeepTheConnectionAndTheInventory) {
  TestServer server(BuildServerConfig());
  auto alice = CreateRoomPlayer(server, "alice", nullptr);
  EXPECT_EQ(Build(*alice.client, 2400.0f, 2800.0f).result(), protocol::GAME_NOT_ENOUGH_RESOURCES);
  Equip(*alice.client);
  EXPECT_EQ(Build(*alice.client, 2400.0f, 2800.0f, 9).result(), protocol::GAME_BUILD_INVALID_TYPE);
  EXPECT_EQ(Build(*alice.client, 2400.0f, 2400.0f).result(), protocol::GAME_BUILD_BLOCKED) << "내 몸 위";
  EXPECT_EQ(Build(*alice.client, 4000.0f, 4000.0f).result(), protocol::GAME_BUILD_TOO_FAR);
  const auto ok = Build(*alice.client, 2400.0f, 2800.0f);
  EXPECT_EQ(ok.result(), protocol::GAME_OK) << "거부된 것들은 아무것도 바꾸지 않았다";
  EXPECT_EQ(ok.wood(), 0u);
  EXPECT_TRUE(PingWorksInGame(*alice.client));
  EXPECT_EQ(server.game_stats().builds_rejected.load(), 4u);
}

TEST(GameIntegration, BuildingOutsideARoomIsRejectedWithoutClosingTheConnection) {
  TestServer server;
  auto client = Connect(server);
  EXPECT_EQ(Build(*client, 2400.0f, 2800.0f).result(), protocol::GAME_NOT_IN_ROOM);
  EXPECT_TRUE(ServerStillWorks(server));
}

// 서버 판정 이동이 건물에 막힌다: 스냅샷의 y가 벽(2650) - 몸 반지름(40) = 2610을 넘지 않는다.
TEST(GameIntegration, APlayerWalkingIntoABuiltHouseIsStoppedByTheServer) {
  TestServer server(BuildServerConfig());
  auto alice = CreateRoomPlayer(server, "alice", nullptr);
  Equip(*alice.client);
  ASSERT_EQ(Build(*alice.client, 2400.0f, 2800.0f).result(), protocol::GAME_OK);  // y 2650~2950

  ASSERT_TRUE(SendMove(*alice.client, 1, 0.0f, 1.0f));
  protocol::WorldSnapshot snapshot;
  float max_y = 0.0f;
  int near_wall = 0;
  const bool done = WaitSnapshot(*alice.client, snapshot, [&](const protocol::WorldSnapshot& s) {
    const auto* me = FindPlayer(s, 1);
    if (me == nullptr) return false;
    max_y = std::max(max_y, me->y());
    near_wall = me->y() > 2600.0f ? near_wall + 1 : 0;
    return near_wall >= 10;  // 벽에 닿은 채로 10개의 스냅샷(0.5초) 동안 머문다
  }, Millis(6000));
  EXPECT_TRUE(done) << "벽까지 걸어가서 멈춘다(max y " << max_y << ")";
  EXPECT_LE(max_y, 2610.01f) << "서버가 벽을 뚫고 지나가는 것을 허용하지 않는다";
}

TEST(GameIntegration, TheTickKeepsRunningWhileAPlayerPushesAgainstAWallAndAnotherLeaves) {
  TestServer server(BuildServerConfig());
  std::string code;
  auto alice = CreateRoomPlayer(server, "alice", &code);
  {
    auto bob = JoinRoomPlayer(server, code, "bob");
    Equip(*bob.client);
    ASSERT_EQ(Build(*bob.client, 2600.0f, 2900.0f).result(), protocol::GAME_OK);
    ASSERT_TRUE(SendMove(*alice.client, 1, 0.5f, 1.0f));
  }  // bob의 연결이 갑자기 닫힌다
  protocol::WorldSnapshot snapshot;
  EXPECT_TRUE(WaitSnapshot(*alice.client, snapshot, [](const protocol::WorldSnapshot& s) { return s.tick() > 40; }));
  EXPECT_TRUE(ServerStillWorks(server));
}

// ============================================================ 자원 채집 (docs/stage4-gather.md)

// 허용 거리를 맵 전체보다 크게 해서 시작 위치에서 어느 노드든 칠 수 있게 한 서버.
server::ServerConfig GatherServerConfig(std::uint32_t tree_hp = 5, std::uint32_t respawn_ticks = 600) {
  server::ServerConfig config = DefaultServerConfig();
  config.play.gather_range_cm = 100000.0f;
  config.play.tree_hp = tree_hp;
  config.play.respawn_ticks = respawn_ticks;
  return config;
}

TEST(GameIntegration, GatheringOverTcpUpdatesEveryoneInTheRoomAndLateJoiners) {
  TestServer server(GatherServerConfig());
  std::string code;
  auto alice = CreateRoomPlayer(server, "alice", &code);
  auto bob = JoinRoomPlayer(server, code, "bob");

  const auto hit = Gather(*alice.client, 1);
  ASSERT_EQ(hit.result(), protocol::GAME_OK);
  EXPECT_EQ(hit.node_id(), 1u);
  EXPECT_EQ(hit.wood(), 1u);
  EXPECT_EQ(hit.stone(), 0u);

  const auto hp_is_four = [](const protocol::ResourceUpdate& u) {
    return u.nodes_size() == 1 && u.nodes(0).node_id() == 1 && u.nodes(0).hp() == 4;
  };
  protocol::ResourceUpdate update;
  EXPECT_TRUE(WaitResourceUpdate(*alice.client, update, hp_is_four)) << "친 본인도 방 알림을 받는다";
  EXPECT_TRUE(WaitResourceUpdate(*bob.client, update, hp_is_four));

  // 다른 사람이 같은 노드를 치면 이어서 줄어든다. 인벤토리는 각자 것이다.
  const auto bob_hit = Gather(*bob.client, 1);
  EXPECT_EQ(bob_hit.result(), protocol::GAME_OK);
  EXPECT_EQ(bob_hit.wood(), 1u);
  EXPECT_TRUE(WaitResourceUpdate(*alice.client, update, [](const protocol::ResourceUpdate& u) {
    return u.nodes_size() == 1 && u.nodes(0).hp() == 3;
  }));

  // 늦게 들어온 사람은 현재 체력을 입장 응답으로 받는다.
  auto carol = Connect(server);
  const auto joined = JoinRoom(*carol, code, "carol");
  ASSERT_EQ(joined.result(), protocol::GAME_OK);
  ASSERT_GT(joined.state().nodes_size(), 1);
  EXPECT_EQ(joined.state().nodes(0).hp(), 3u);
  EXPECT_EQ(joined.state().nodes(1).hp(), joined.state().nodes(1).max_hp());

  EXPECT_EQ(server.game_stats().gathers_ok.load(), 2u);
  EXPECT_EQ(server.game_stats().gathers_rejected.load(), 0u);
}

TEST(GameIntegration, GatheringFromTheStartPositionIsTooFarWithDefaultRange) {
  TestServer server;  // 기본 허용 거리 200 cm
  auto alice = CreateRoomPlayer(server, "alice", nullptr);
  EXPECT_EQ(Gather(*alice.client, 1).result(), protocol::GAME_TOO_FAR);
  EXPECT_EQ(Gather(*alice.client, 9999).result(), protocol::GAME_NODE_NOT_FOUND);
  EXPECT_TRUE(PingWorksInGame(*alice.client)) << "거부해도 연결은 그대로";
  EXPECT_EQ(server.game_stats().gathers_rejected.load(), 2u);
}

TEST(GameIntegration, GatheringOutsideARoomIsRejectedWithoutClosingTheConnection) {
  TestServer server;
  auto client = Connect(server);
  EXPECT_EQ(Gather(*client, 1).result(), protocol::GAME_NOT_IN_ROOM);
  EXPECT_TRUE(ServerStillWorks(server));
  EXPECT_TRUE(client->SendPing(1, "x"));
}

TEST(GameIntegration, ADepletedNodeRespawnsOnTheRealTimerAndTheRoomIsTold) {
  TestServer server(GatherServerConfig(/*tree_hp=*/1, /*respawn_ticks=*/10));  // 10틱 = 0.5초
  auto alice = CreateRoomPlayer(server, "alice", nullptr);
  ASSERT_EQ(Gather(*alice.client, 1).result(), protocol::GAME_OK);

  protocol::ResourceUpdate update;
  ASSERT_TRUE(WaitResourceUpdate(*alice.client, update, [](const protocol::ResourceUpdate& u) {
    return u.nodes_size() == 1 && u.nodes(0).hp() == 0;
  })) << "고갈 알림";
  ASSERT_TRUE(WaitResourceUpdate(*alice.client, update, [](const protocol::ResourceUpdate& u) {
    return u.nodes_size() == 1 && u.nodes(0).node_id() == 1 && u.nodes(0).hp() == 1;
  })) << "재생 알림";
  EXPECT_EQ(Gather(*alice.client, 1).result(), protocol::GAME_OK) << "재생한 노드를 다시 칠 수 있다";
}

TEST(GameIntegration, GatheringFlowsSurviveAnAbruptDisconnectOfAnotherPlayer) {
  TestServer server(GatherServerConfig());
  std::string code;
  auto alice = CreateRoomPlayer(server, "alice", &code);
  {
    auto bob = JoinRoomPlayer(server, code, "bob");
    ASSERT_EQ(Gather(*bob.client, 1).result(), protocol::GAME_OK);
  }  // bob 연결이 갑자기 닫힌다
  ASSERT_EQ(Gather(*alice.client, 2).result(), protocol::GAME_OK);
  protocol::ResourceUpdate update;
  EXPECT_TRUE(WaitResourceUpdate(*alice.client, update, [](const protocol::ResourceUpdate& u) {
    return u.nodes_size() >= 1;
  }));
  EXPECT_TRUE(ServerStillWorks(server));
}

// 방에 사람이 있는 채로 서버를 멈춰도 제한 시간 안에 정상 종료한다(TestServer 소멸자가 확인한다).
TEST(GameIntegration, ServerStopsCleanlyWhileRoomsAreActive) {
  TestServer server;
  std::string code;
  auto alice = CreateRoomPlayer(server, "alice", &code);
  auto bob = JoinRoomPlayer(server, code, "bob");
  ASSERT_TRUE(SendMove(*alice.client, 1, 1.0f, 1.0f));
  protocol::WorldSnapshot snapshot;
  ASSERT_TRUE(WaitSnapshot(*bob.client, snapshot, [](const protocol::WorldSnapshot&) { return true; }));
  server.Stop();
  EXPECT_TRUE(bob.client->WaitForServerClose(kTimeout));
  EXPECT_EQ(server.game_stats().current_players.load(), 0u);
}

// 여러 방이 동시에 도는 동안 틱이 계속 돌고 처리 시간이 관측된다.
TEST(GameIntegration, ManyRoomsKeepTickingAndStatsAreRecorded) {
  TestServer server;
  std::vector<Player> players;
  for (int i = 0; i < 20; ++i) {
    std::string code;
    players.push_back(CreateRoomPlayer(server, "a" + std::to_string(i), &code));
    players.push_back(JoinRoomPlayer(server, code, "b" + std::to_string(i)));
  }
  for (auto& player : players) {
    ASSERT_TRUE(SendMove(*player.client, 1, 1.0f, -1.0f));
  }
  for (auto& player : players) {
    protocol::WorldSnapshot snapshot;
    ASSERT_TRUE(WaitSnapshot(*player.client, snapshot, [&](const protocol::WorldSnapshot& s) { return s.tick() >= 10; }, Millis(5000)));
    EXPECT_EQ(snapshot.players_size(), 2);
  }
  EXPECT_EQ(server.game_stats().current_rooms.load(), 20u);
  EXPECT_GE(server.game_stats().ticks.load(), 10u);
  EXPECT_GT(server.game_stats().snapshots_sent.load(), 0u);
  EXPECT_LT(server.game_stats().max_tick_micros.load(), 50000u);  // 한 틱 처리가 틱 간격(50 ms)보다 짧다
}

}  // namespace
}  // namespace game
