// 농장 저장 통합 시험: 테스트 프로세스 안에서 진짜 서버를 띄우고 실제 TCP로 로그인한 사람이 농장을 열고, 채집하고,
// 집을 짓고, 나가고, 다시 열었을 때 상태가 이어지는지, 저장 시점(건설은 바로, 채집은 모아서)과 저장소 장애를 확인한다.
// 저장소는 인메모리 구현이고(장애와 지연 주입), 실제 MySQL로 같은 것을 확인하는 시험은 account_mysql_test에 있다.

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <thread>
#include <vector>

#include "account_test_client.h"

namespace game {
namespace {

using namespace testing_support;

const ::testing::Environment* const kQuiet = ::testing::AddGlobalTestEnvironment(new QuietLogEnvironment);

// 채집이 빠르고(쿨다운 없음, 체력 넉넉, 어디서든 칠 수 있음) 건설이 빨리 끝나는 서버. 모아서 저장하는 주기는 길게 둔다.
server::ServerConfig FarmConfig(std::uint32_t build_ticks = 10, std::uint64_t flush_ticks = 100000) {
  server::ServerConfig config = DefaultServerConfig();
  config.play.gather_range_cm = 100000.0f;
  config.play.gather_cooldown_ticks = 0;
  config.play.tree_hp = 1000;
  config.play.rock_hp = 1000;
  config.play.build_ticks = build_ticks;
  config.play.farm_flush_interval_ticks = flush_ticks;
  return config;
}

// 나무(1~30번)에서 목재를, 돌(31~50번)에서 석재를 모은다.
void Collect(BlockingClient& client, int wood, int stone = 0) {
  for (int i = 0; i < wood; ++i) ASSERT_EQ(Gather(client, 1).result(), protocol::GAME_OK);
  for (int i = 0; i < stone; ++i) ASSERT_EQ(Gather(client, 31).result(), protocol::GAME_OK);
}

// 계정 번호는 가입한 순서대로 1, 2, 3...이다(인메모리 저장소).
std::optional<farm::StoredPlayer> SavedPlayer(TestServer& server, std::uint64_t owner, std::uint64_t account) {
  const auto farm = server.farms().Get(owner);
  if (!farm) return std::nullopt;
  for (const auto& p : farm->players) {
    if (p.account_id == account) return p;
  }
  return std::nullopt;
}

bool SavedWoodIs(TestServer& server, std::uint64_t owner, std::uint64_t account, std::uint32_t wood) {
  const auto p = SavedPlayer(server, owner, account);
  return p && p->wood == wood;
}

std::string OpenFarmCode(BlockingClient& c) {
  const auto created = CreateRoom(c, "ignored");
  EXPECT_EQ(created.result(), protocol::GAME_OK);
  return created.state().room_code();
}

// ============================================================ 열기

TEST(FarmIntegration, TheFarmGetsAStableCodeAndReopensWithTheSameCode) {
  TestServer server(FarmConfig());
  auto alice = ConnectLoggedIn(server, "alice_01");
  const std::string code = OpenFarmCode(*alice);
  ASSERT_EQ(code.size(), 6u);
  EXPECT_EQ(server.farms().farm_count(), 1u);
  EXPECT_EQ(server.farms().Get(1)->code, code);
  EXPECT_EQ(server.game_stats().farms_opened.load(), 1u);

  ASSERT_EQ(LeaveRoom(*alice).result(), protocol::GAME_OK);
  ASSERT_TRUE(WaitUntil([&] { return server.game_stats().current_rooms.load() == 0; }));
  EXPECT_EQ(OpenFarmCode(*alice), code) << "농장 코드는 바뀌지 않는다";
  EXPECT_EQ(server.farms().farm_count(), 1u) << "같은 계정이 농장을 또 만들면 안 된다";
  EXPECT_EQ(server.game_stats().farms_opened.load(), 2u);
}

TEST(FarmIntegration, EachAccountGetsItsOwnFarmWithDistinctCodes) {
  TestServer server(FarmConfig());
  constexpr int kAccounts = 8;
  std::vector<std::unique_ptr<BlockingClient>> clients;
  for (int i = 0; i < kAccounts; ++i) {
    clients.push_back(ConnectLoggedIn(server, "farmer_" + std::to_string(10 + i)));
  }
  // 모두에게 방 만들기 요청을 먼저 보낸 뒤 응답을 모아 저장소 호출이 겹치게 한다.
  for (auto& c : clients) {
    protocol::CreateRoomRequest request;
    request.set_nickname("x");
    ASSERT_TRUE(SendProto(*c, MessageId::kCreateRoomRequest, request));
  }
  std::set<std::string> codes;
  for (auto& c : clients) {
    protocol::CreateRoomResponse response;
    ASSERT_TRUE(Receive(*c, MessageId::kCreateRoomResponse, response, kGameSkip));
    EXPECT_EQ(response.result(), protocol::GAME_OK);
    codes.insert(response.state().room_code());
  }
  EXPECT_EQ(codes.size(), static_cast<std::size_t>(kAccounts));
  EXPECT_EQ(server.farms().farm_count(), static_cast<std::size_t>(kAccounts));
}

TEST(FarmIntegration, OpeningFailsCleanlyWhenTheStorageIsDownAndWorksAgainAfterRecovery) {
  TestServer server(FarmConfig());
  auto alice = ConnectLoggedIn(server, "alice_01");
  server.farms().SetFailing(true);
  EXPECT_EQ(CreateRoom(*alice, "x").result(), protocol::GAME_STORAGE_UNAVAILABLE);
  EXPECT_TRUE(PingWorksInGame(*alice)) << "저장소가 죽어도 연결은 살아 있어야 한다";
  EXPECT_EQ(server.game_stats().farm_open_failures.load(), 1u);
  EXPECT_EQ(server.game_stats().current_rooms.load(), 0u) << "열지 못했으면 방이 남으면 안 된다";

  server.farms().SetFailing(false);
  EXPECT_EQ(CreateRoom(*alice, "x").result(), protocol::GAME_OK) << "복구되면 같은 연결이 다시 열 수 있어야 한다";
}

TEST(FarmIntegration, ASlowStorageDoesNotBlockOtherPlayers) {
  TestServer server(FarmConfig());
  auto alice = ConnectLoggedIn(server, "alice_01");
  auto guest = Connect(server);
  ASSERT_EQ(CreateRoom(*guest, "guest_one").result(), protocol::GAME_OK);

  server.farms().SetDelay(Millis(300));
  protocol::CreateRoomRequest request;
  request.set_nickname("x");
  ASSERT_TRUE(SendProto(*alice, MessageId::kCreateRoomRequest, request));  // 농장을 불러오는 중(300 ms 이상)

  const auto start = std::chrono::steady_clock::now();
  EXPECT_TRUE(PingWorksInGame(*guest));
  EXPECT_LT(std::chrono::steady_clock::now() - start, Millis(250)) << "느린 농장 저장소가 다른 연결을 막았다";

  protocol::CreateRoomResponse response;
  ASSERT_TRUE(Receive(*alice, MessageId::kCreateRoomResponse, response, kGameSkip, Millis(5000)));
  EXPECT_EQ(response.result(), protocol::GAME_OK);
}

TEST(FarmIntegration, ADisconnectWhileTheFarmIsOpeningLeavesNoRoomBehind) {
  TestServer server(FarmConfig());
  {
    auto alice = ConnectLoggedIn(server, "alice_01");
    server.farms().SetDelay(Millis(150));
    protocol::CreateRoomRequest request;
    request.set_nickname("x");
    ASSERT_TRUE(SendProto(*alice, MessageId::kCreateRoomRequest, request));
    alice->Close();  // 응답을 받기 전에 끊는다
  }
  std::this_thread::sleep_for(Millis(700));  // 불러오기와 만들기가 모두 끝날 시간
  server.farms().SetDelay(Millis(0));
  EXPECT_EQ(server.game_stats().current_rooms.load(), 0u) << "끊긴 연결을 위해 방을 만들면 안 된다";
  EXPECT_EQ(server.game_stats().current_players.load(), 0u);

  // 계정은 다시 로그인할 수 있고, 농장은 한 개만 있다.
  auto again = Connect(server);
  ASSERT_EQ(LoginAccount(*again, "alice_01", "password123").result(), protocol::ACCOUNT_OK);
  EXPECT_EQ(CreateRoom(*again, "x").result(), protocol::GAME_OK);
  EXPECT_EQ(server.farms().farm_count(), 1u);
}

// ============================================================ 저장 시점

TEST(FarmIntegration, ResourcesAndRecordsSurviveLeavingAndReopening) {
  TestServer server(FarmConfig());
  auto alice = ConnectLoggedIn(server, "alice_01");
  OpenFarmCode(*alice);
  Collect(*alice, 25, 12);
  ASSERT_EQ(LeaveRoom(*alice).result(), protocol::GAME_OK);
  ASSERT_TRUE(WaitUntil([&] { return SavedWoodIs(server, 1, 1, 25); })) << "나갈 때 인벤토리가 저장되어야 한다";
  EXPECT_EQ(SavedPlayer(server, 1, 1)->stone, 12u);

  ASSERT_TRUE(WaitUntil([&] { return server.game_stats().current_rooms.load() == 0; }));
  OpenFarmCode(*alice);
  const auto after = Gather(*alice, 1);  // 한 번 더 치면 되살린 인벤토리(25)에 1이 더해진다
  ASSERT_EQ(after.result(), protocol::GAME_OK);
  EXPECT_EQ(after.wood(), 26u);
  EXPECT_EQ(after.stone(), 12u);
}

TEST(FarmIntegration, RecordsPersistAcrossLogins) {
  TestServer server(FarmConfig());
  {
    auto alice = ConnectLoggedIn(server, "alice_01");
    OpenFarmCode(*alice);
    Collect(*alice, 6, 4);
    alice->Close();  // 나가기 없이 연결이 끊긴다
  }
  ASSERT_TRUE(WaitUntil([&] { return server.accounts().StatsOf(1).has_value(); })) << "끊겨도 누적 기록이 저장되어야 한다";

  auto again = Connect(server);
  const auto login = LoginAccount(*again, "alice_01", "password123");
  ASSERT_EQ(login.result(), protocol::ACCOUNT_OK);
  EXPECT_EQ(login.profile().wood_gathered(), 6u);
  EXPECT_EQ(login.profile().stone_gathered(), 4u);
  EXPECT_EQ(login.profile().houses_built(), 0u);
}

TEST(FarmIntegration, TheProfileShowsGatheringImmediatelyBeforeItIsSaved) {
  TestServer server(FarmConfig());
  auto alice = ConnectLoggedIn(server, "alice_01");
  OpenFarmCode(*alice);
  Collect(*alice, 4, 2);
  const auto profile = GetProfile(*alice);
  ASSERT_EQ(profile.result(), protocol::ACCOUNT_OK);
  EXPECT_EQ(profile.profile().wood_gathered(), 4u) << "저장 주기를 기다리지 않고 바로 보여야 한다";
  EXPECT_EQ(profile.profile().stone_gathered(), 2u);
  EXPECT_FALSE(server.accounts().StatsOf(1).has_value()) << "아직 저장하지 않았다(주기가 길다)";
}

TEST(FarmIntegration, GatheringIsSavedInBatchesNotOnEveryHit) {
  TestServer server(FarmConfig());  // 모아서 저장하는 주기가 매우 길다
  auto alice = ConnectLoggedIn(server, "alice_01");
  OpenFarmCode(*alice);
  Collect(*alice, 3);
  std::this_thread::sleep_for(Millis(300));
  EXPECT_FALSE(SavedPlayer(server, 1, 1).has_value()) << "채집마다 저장소에 쓰면 안 된다";
  EXPECT_EQ(server.farms().save_calls(), 0u);

  ASSERT_EQ(LeaveRoom(*alice).result(), protocol::GAME_OK);
  ASSERT_TRUE(WaitUntil([&] { return SavedWoodIs(server, 1, 1, 3); }));
  EXPECT_EQ(server.farms().save_calls(), 1u) << "나갈 때 한 번에 저장한다";
}

TEST(FarmIntegration, AShortIntervalSavesGatheringWhileTheFarmIsStillOpen) {
  TestServer server(FarmConfig(10, /*flush_ticks=*/5));  // 5틱 = 0.25초
  auto alice = ConnectLoggedIn(server, "alice_01");
  OpenFarmCode(*alice);
  Collect(*alice, 3);
  EXPECT_TRUE(WaitUntil([&] { return SavedWoodIs(server, 1, 1, 3); })) << "주기가 되면 방에 있는 채로 저장된다";
  EXPECT_TRUE(WaitUntil([&] { return server.accounts().StatsOf(1).has_value() && server.accounts().StatsOf(1)->wood_gathered == 3; }));
}

TEST(FarmIntegration, StoppingTheServerSavesProgressThatWasNotSavedYet) {
  TestServer server(FarmConfig());
  auto alice = ConnectLoggedIn(server, "alice_01");
  OpenFarmCode(*alice);
  Collect(*alice, 5, 1);
  EXPECT_FALSE(SavedPlayer(server, 1, 1).has_value());
  server.Stop();  // 종료하면서 모아 둔 변경을 저장한다
  EXPECT_TRUE(SavedWoodIs(server, 1, 1, 5)) << "종료할 때 저장하지 못했다";
  ASSERT_TRUE(server.accounts().StatsOf(1).has_value());
  EXPECT_EQ(server.accounts().StatsOf(1)->wood_gathered, 5u);
}

TEST(FarmIntegration, SaveFailuresDoNotStopTheGameAndAreRetriedUntilTheyWork) {
  TestServer server(FarmConfig(10, /*flush_ticks=*/5));
  auto alice = ConnectLoggedIn(server, "alice_01");
  OpenFarmCode(*alice);
  server.farms().SetSaveFailing(true);
  Collect(*alice, 3);  // 저장이 실패해도 채집은 계속 된다
  EXPECT_TRUE(PingWorksInGame(*alice));
  ASSERT_TRUE(WaitUntil([&] { return server.farms().failed_saves() >= 1; })) << "저장을 시도하지 않았다";
  ASSERT_EQ(LeaveRoom(*alice).result(), protocol::GAME_OK);  // 방이 닫혀도 값은 대기열에 남는다
  ASSERT_TRUE(WaitUntil([&] { return server.game_stats().current_rooms.load() == 0; }));

  server.farms().SetSaveFailing(false);
  EXPECT_TRUE(WaitUntil([&] { return SavedWoodIs(server, 1, 1, 3); }, Millis(5000))) << "복구되면 대기열의 값이 저장되어야 한다";
}

// ============================================================ 건설

TEST(FarmIntegration, BuildingIsSavedImmediatelyEvenWhenTheFlushIntervalIsLong) {
  TestServer server(FarmConfig(/*build_ticks=*/1000));  // 건설 중인 채로 유지
  auto alice = ConnectLoggedIn(server, "alice_01");
  OpenFarmCode(*alice);
  Collect(*alice, 20, 10);
  const auto built = Build(*alice, 2400.0f, 2800.0f);
  ASSERT_EQ(built.result(), protocol::GAME_OK);

  ASSERT_TRUE(WaitUntil([&] {
    const auto farm = server.farms().Get(1);
    return farm && farm->structures.size() == 1;
  })) << "건설은 주기를 기다리지 않고 바로 저장되어야 한다";
  const auto farm = server.farms().Get(1);
  EXPECT_EQ(farm->structures[0].structure_id, 1u);
  EXPECT_EQ(farm->structures[0].x, 2400.0f);
  EXPECT_EQ(farm->structures[0].y, 2800.0f);
  EXPECT_EQ(farm->structures[0].state, 0u) << "건설 중";
  EXPECT_GT(farm->structures[0].remaining_ticks, 900u);
  EXPECT_EQ(farm->structures[0].built_by_account_id, 1u);
  // 같은 저장에 줄어든 인벤토리와 늘어난 집 수가 함께 들어간다.
  EXPECT_TRUE(SavedWoodIs(server, 1, 1, 0));
  EXPECT_EQ(SavedPlayer(server, 1, 1)->stone, 0u);
  ASSERT_TRUE(server.accounts().StatsOf(1).has_value());
  EXPECT_EQ(server.accounts().StatsOf(1)->houses_built, 1u);
  EXPECT_EQ(server.accounts().StatsOf(1)->wood_gathered, 20u);
}

TEST(FarmIntegration, CompletionIsSavedImmediately) {
  TestServer server(FarmConfig(/*build_ticks=*/10));
  auto alice = ConnectLoggedIn(server, "alice_01");
  OpenFarmCode(*alice);
  Collect(*alice, 20, 10);
  ASSERT_EQ(Build(*alice, 2400.0f, 2800.0f).result(), protocol::GAME_OK);
  protocol::StructureUpdate update;
  ASSERT_TRUE(WaitStructureUpdate(*alice, update, [](const protocol::StructureUpdate& u) {
    return u.structures_size() == 1 && u.structures(0).state() == protocol::STRUCTURE_COMPLETE;
  }));
  EXPECT_TRUE(WaitUntil([&] {
    const auto farm = server.farms().Get(1);
    return farm && farm->structures.size() == 1 && farm->structures[0].state == 1u && farm->structures[0].remaining_ticks == 0u;
  })) << "완공도 바로 저장되어야 한다";
}

TEST(FarmIntegration, ABuiltHouseIsStillThereAfterTheFarmClosesAndReopens) {
  TestServer server(FarmConfig(/*build_ticks=*/10));
  auto alice = ConnectLoggedIn(server, "alice_01");
  OpenFarmCode(*alice);
  Collect(*alice, 20, 10);
  ASSERT_EQ(Build(*alice, 2400.0f, 2800.0f).result(), protocol::GAME_OK);
  protocol::StructureUpdate update;
  ASSERT_TRUE(WaitStructureUpdate(*alice, update, [](const protocol::StructureUpdate& u) {
    return u.structures_size() == 1 && u.structures(0).state() == protocol::STRUCTURE_COMPLETE;
  }));
  ASSERT_EQ(LeaveRoom(*alice).result(), protocol::GAME_OK);
  ASSERT_TRUE(WaitUntil([&] { return server.game_stats().current_rooms.load() == 0; }));

  const auto reopened = CreateRoom(*alice, "x");
  ASSERT_EQ(reopened.result(), protocol::GAME_OK);
  ASSERT_EQ(reopened.state().structures_size(), 1) << "집이 사라졌다";
  EXPECT_EQ(reopened.state().structures(0).x(), 2400.0f);
  EXPECT_EQ(reopened.state().structures(0).y(), 2800.0f);
  EXPECT_EQ(reopened.state().structures(0).state(), protocol::STRUCTURE_COMPLETE);

  // 집은 이동을 막는다(복원된 구조물도 충돌에 쓰인다): 같은 자리에 또 지을 수 없다.
  Collect(*alice, 20, 10);
  EXPECT_EQ(Build(*alice, 2400.0f, 2800.0f).result(), protocol::GAME_BUILD_BLOCKED);
}

TEST(FarmIntegration, ConstructionProgressIsSavedAndTheClosedFarmDoesNotAdvanceTime) {
  TestServer server(FarmConfig(/*build_ticks=*/40));  // 40틱 = 2초
  auto alice = ConnectLoggedIn(server, "alice_01");
  OpenFarmCode(*alice);
  Collect(*alice, 20, 10);
  ASSERT_EQ(Build(*alice, 2400.0f, 2800.0f).result(), protocol::GAME_OK);
  std::this_thread::sleep_for(Millis(400));  // 건설이 조금 진행된다(8틱쯤). 닫을 때 남은 틱을 다시 저장해야 한다
  ASSERT_EQ(LeaveRoom(*alice).result(), protocol::GAME_OK);  // 건설이 끝나기 전에 농장이 닫힌다
  ASSERT_TRUE(WaitUntil([&] { return server.game_stats().current_rooms.load() == 0; }));
  ASSERT_TRUE(WaitUntil([&] {
    const auto farm = server.farms().Get(1);
    return farm && farm->structures.size() == 1 && farm->structures[0].state == 0u &&
           farm->structures[0].remaining_ticks < 40u;
  })) << "닫힐 때 건설이 진행된 만큼 줄어든 남은 틱이 저장되어야 한다";
  const auto saved_remaining = server.farms().Get(1)->structures[0].remaining_ticks;
  EXPECT_GT(saved_remaining, 0u);
  EXPECT_LT(saved_remaining, 40u);

  std::this_thread::sleep_for(Millis(2500));  // 건설 시간(2초)보다 오래 닫아 둔다. 닫힌 동안 시간은 흐르지 않는다.
  const auto reopened = CreateRoom(*alice, "x");
  ASSERT_EQ(reopened.result(), protocol::GAME_OK);
  ASSERT_EQ(reopened.state().structures_size(), 1);
  EXPECT_EQ(reopened.state().structures(0).state(), protocol::STRUCTURE_BUILDING) << "닫혀 있는 동안 건설이 진행되면 안 된다";
  EXPECT_EQ(reopened.state().structures(0).ready_tick(), saved_remaining) << "남은 틱만큼 더 있어야 완공된다";

  protocol::StructureUpdate update;
  EXPECT_TRUE(WaitStructureUpdate(*alice, update, [](const protocol::StructureUpdate& u) {
    return u.structures_size() == 1 && u.structures(0).state() == protocol::STRUCTURE_COMPLETE;
  }, Millis(5000))) << "다시 열면 남은 시간 뒤에 완공된다";
}

// ============================================================ 여러 사람

TEST(FarmIntegration, PlayersKeepSeparateInventoriesInTheSameFarm) {
  TestServer server(FarmConfig());
  auto alice = ConnectLoggedIn(server, "alice_01");
  const std::string code = OpenFarmCode(*alice);
  auto bob = ConnectLoggedIn(server, "bobby_02");
  ASSERT_EQ(JoinRoom(*bob, code, "x").result(), protocol::GAME_OK);

  Collect(*alice, 2);
  Collect(*bob, 5, 1);
  ASSERT_EQ(LeaveRoom(*bob).result(), protocol::GAME_OK);
  ASSERT_EQ(LeaveRoom(*alice).result(), protocol::GAME_OK);

  ASSERT_TRUE(WaitUntil([&] { return SavedWoodIs(server, 1, 1, 2) && SavedWoodIs(server, 1, 2, 5); }));
  EXPECT_EQ(SavedPlayer(server, 1, 2)->stone, 1u);
  ASSERT_TRUE(WaitUntil([&] { return server.accounts().StatsOf(2).has_value(); }));
  EXPECT_EQ(server.accounts().StatsOf(1)->wood_gathered, 2u);
  EXPECT_EQ(server.accounts().StatsOf(2)->wood_gathered, 5u) << "누적 기록은 계정별이다";
}

TEST(FarmIntegration, AFriendsInventoryIsRestoredWhenTheyJoinTheFarmAgain) {
  TestServer server(FarmConfig());
  auto alice = ConnectLoggedIn(server, "alice_01");
  const std::string code = OpenFarmCode(*alice);
  auto bob = ConnectLoggedIn(server, "bobby_02");
  ASSERT_EQ(JoinRoom(*bob, code, "x").result(), protocol::GAME_OK);
  Collect(*bob, 5);
  ASSERT_EQ(LeaveRoom(*bob).result(), protocol::GAME_OK);

  ASSERT_EQ(JoinRoom(*bob, code, "x").result(), protocol::GAME_OK);  // 농장은 alice 때문에 계속 열려 있다
  const auto hit = Gather(*bob, 1);
  EXPECT_EQ(hit.wood(), 6u) << "방을 나갔다 다시 들어와도 인벤토리가 이어져야 한다";
}

TEST(FarmIntegration, TheOwnerReconnectingWhileAFriendIsInsideRejoinsTheLiveFarm) {
  TestServer server(FarmConfig());
  auto alice = ConnectLoggedIn(server, "alice_01");
  const std::string code = OpenFarmCode(*alice);
  auto bob = ConnectLoggedIn(server, "bobby_02");
  ASSERT_EQ(JoinRoom(*bob, code, "x").result(), protocol::GAME_OK);

  alice->Close();  // alice의 연결이 끊긴다(bob은 남아 있다)
  ASSERT_TRUE(WaitUntil([&] { return server.account_stats().current_logged_in.load() == 1; }));

  auto again = Connect(server);
  ASSERT_EQ(LoginAccount(*again, "alice_01", "password123").result(), protocol::ACCOUNT_OK);
  const auto rejoined = CreateRoom(*again, "x");  // 방 만들기는 "내 농장 열기"다. 이미 열려 있으면 그 방에 들어간다
  ASSERT_EQ(rejoined.result(), protocol::GAME_OK);
  EXPECT_EQ(rejoined.state().room_code(), code);
  EXPECT_EQ(rejoined.state().players_size(), 2);
  EXPECT_EQ(server.farms().farm_count(), 1u);
  EXPECT_EQ(server.game_stats().farms_opened.load(), 1u) << "이미 열린 농장을 다시 열면 안 된다";

  protocol::RoomEvent event;
  EXPECT_TRUE(WaitEvent(*bob, protocol::RoomEvent::PLAYER_JOINED, event));
  EXPECT_EQ(event.player().nickname(), "alice_01");
}

}  // namespace
}  // namespace game
