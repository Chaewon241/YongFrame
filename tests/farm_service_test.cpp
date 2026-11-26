// GameService의 농장 규칙 단위 시험: 소켓 없이 가짜 연결과 가짜 계정 함수, 인메모리 농장 저장소로 방 코드 예약과
// 코드 충돌 재시도, 농장 방과 임시 방의 분리, 저장 알림을 확인한다.

#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "account_test_support.h"
#include "farm/in_memory_farm_repository.h"
#include "play/game_service.h"
#include "server/log.h"

namespace game::play {
namespace {

using namespace testing_support;
using Millis = std::chrono::milliseconds;

class QuietLog : public ::testing::Environment {
 public:
  void SetUp() override { server::SetMinLogLevel(server::LogLevel::kError); }
};
const ::testing::Environment* const kQuiet = ::testing::AddGlobalTestEnvironment(new QuietLog);

// 계정 서비스 없이 로그인 상태를 흉내 내는 표(연결 번호 -> 로그인 정보).
struct FakeAccounts {
  std::map<std::uint64_t, account::LoginInfo> logins;
  std::map<std::uint64_t, account::AccountStats> stats;

  AccountHooks Hooks() {
    AccountHooks hooks;
    hooks.login_of = [this](std::uint64_t id) -> std::optional<account::LoginInfo> {
      const auto it = logins.find(id);
      if (it == logins.end()) return std::nullopt;
      return it->second;
    };
    hooks.stats_of = [this](std::uint64_t id) -> std::optional<account::AccountStats> {
      const auto it = stats.find(id);
      if (it == stats.end()) return std::nullopt;
      return it->second;
    };
    hooks.add_progress = [this](std::uint64_t id, const account::AccountStats& d) -> std::optional<account::AccountStats> {
      auto it = stats.find(id);
      if (it == stats.end()) return std::nullopt;
      it->second.wood_gathered += d.wood_gathered;
      it->second.stone_gathered += d.stone_gathered;
      it->second.houses_built += d.houses_built;
      return it->second;
    };
    return hooks;
  }

  std::shared_ptr<FakeClient> Login(std::uint64_t id, std::uint64_t account_id, const std::string& name) {
    logins[id] = account::LoginInfo{account_id, name};
    stats[id] = account::AccountStats{};
    return std::make_shared<FakeClient>(id);
  }
};

PlayConfig TestConfig() {
  PlayConfig config;
  config.farm_flush_interval_ticks = 0;  // 주기 저장은 끈다(시험이 직접 Flush되는 시점을 본다)
  return config;
}

struct Fixture {
  explicit Fixture(RoomCodeGenerator generator = nullptr, PlayConfig config = TestConfig())
      : accounts_repo(std::make_shared<account::InMemoryAccountRepository>(io)),
        repo(std::make_shared<farm::InMemoryFarmRepository>(io, accounts_repo)),
        service(io, config, std::move(generator)) {
    service.SetAccountHooks(accounts.Hooks());
    service.SetFarmRepository(repo);
  }

  protocol::CreateRoomResponse Create(const std::shared_ptr<FakeClient>& c) {
    const auto before = c->count();
    service.HandleCreateRoom(c, protocol::CreateRoomRequest{});
    EXPECT_TRUE(PumpUntil(io, [&] { return c->count() > before; })) << "방 만들기 응답이 오지 않음";
    return c->Last<protocol::CreateRoomResponse>(protocol::MessageId::kCreateRoomResponse);
  }

  protocol::JoinRoomResponse Join(const std::shared_ptr<FakeClient>& c, const std::string& code, const std::string& nick) {
    protocol::JoinRoomRequest request;
    request.set_room_code(code);
    request.set_nickname(nick);
    service.HandleJoinRoom(c, request);
    return c->Last<protocol::JoinRoomResponse>(protocol::MessageId::kJoinRoomResponse);
  }

  void Settle(Millis duration = Millis(100)) {
    const auto end = std::chrono::steady_clock::now() + duration;
    PumpUntil(io, [&] { return std::chrono::steady_clock::now() >= end; }, duration + Millis(500));
  }

  boost::asio::io_context io;
  std::shared_ptr<account::InMemoryAccountRepository> accounts_repo;
  std::shared_ptr<farm::InMemoryFarmRepository> repo;
  FakeAccounts accounts;
  GameService service;
};

// 생성기가 정해진 코드를 차례로 돌려주게 한다(다 쓰면 마지막 값을 반복한다).
RoomCodeGenerator Sequence(std::vector<std::string> codes) {
  auto state = std::make_shared<std::pair<std::vector<std::string>, std::size_t>>(std::move(codes), 0);
  return [state] {
    auto& [list, index] = *state;
    const std::string value = list[std::min(index, list.size() - 1)];
    ++index;
    return value;
  };
}

TEST(FarmService, AGuestRoomNeverTakesTheCodeOfAnUnopenedFarm) {
  Fixture f(Sequence({"111111", "222222"}));
  f.repo->Seed(1, "111111");           // 아직 열리지 않은 농장
  f.service.LoadFarmCodes();           // 서버가 시작할 때 농장 코드를 예약한다
  f.Settle();

  auto guest = std::make_shared<FakeClient>(7);
  protocol::CreateRoomRequest request;
  request.set_nickname("guest_one");
  f.service.HandleCreateRoom(guest, request);
  const auto ok = guest->Last<protocol::CreateRoomResponse>(protocol::MessageId::kCreateRoomResponse);
  ASSERT_EQ(ok.result(), protocol::GAME_OK);
  EXPECT_EQ(ok.state().room_code(), "222222") << "예약된 농장 코드(111111)를 건너뛰어야 한다";
}

TEST(FarmService, WithoutLoadingFarmCodesAGuestCanTakeThemAndTheFarmIsRefusedWhenItOpens) {
  // 농장 코드를 읽지 못한 경우(LoadFarmCodes 실패)를 흉내 낸다: 예약이 없어 게스트 방이 같은 코드를 쓴다.
  Fixture f(Sequence({"111111"}));
  f.repo->Seed(1, "111111");
  auto guest = std::make_shared<FakeClient>(7);
  protocol::CreateRoomRequest request;
  request.set_nickname("guest_one");
  f.service.HandleCreateRoom(guest, request);
  ASSERT_EQ(guest->Last<protocol::CreateRoomResponse>(protocol::MessageId::kCreateRoomResponse).state().room_code(), "111111");

  auto alice = f.accounts.Login(1, 1, "alice_01");
  const auto response = f.Create(alice);
  EXPECT_EQ(response.result(), protocol::GAME_SERVER_BUSY) << "같은 코드의 임시 방이 열려 있으면 농장을 열 수 없다";
  EXPECT_EQ(f.service.stats().farm_open_failures.load(), 1u);
  EXPECT_EQ(f.service.room_count(), 1u);
}

TEST(FarmService, ADuplicateFarmCodeIsRetriedWithAnotherCode) {
  Fixture f(Sequence({"111111", "111111", "333333"}));
  f.repo->Seed(9, "111111");  // 다른 계정의 농장이 이미 이 코드를 쓴다(농장 코드를 아직 읽지 않은 상태)
  auto alice = f.accounts.Login(1, 1, "alice_01");
  const auto response = f.Create(alice);
  ASSERT_EQ(response.result(), protocol::GAME_OK);
  EXPECT_EQ(response.state().room_code(), "333333") << "DB가 코드 중복을 알려 주면 다른 코드로 다시 만든다";
  EXPECT_EQ(f.repo->Get(1)->code, "333333");
}

TEST(FarmService, TheNewFarmCodeIsReservedAgainstGuestRoomsFromTheMomentItIsCreated) {
  Fixture f(Sequence({"444444", "444444", "555555"}));
  auto alice = f.accounts.Login(1, 1, "alice_01");
  ASSERT_EQ(f.Create(alice).state().room_code(), "444444");
  f.service.HandleLeaveRoom(alice);  // 농장이 닫힌다. 코드는 계속 예약되어 있어야 한다
  f.Settle();
  ASSERT_EQ(f.service.room_count(), 0u);

  auto guest = std::make_shared<FakeClient>(7);
  protocol::CreateRoomRequest request;
  request.set_nickname("guest_one");
  f.service.HandleCreateRoom(guest, request);
  EXPECT_EQ(guest->Last<protocol::CreateRoomResponse>(protocol::MessageId::kCreateRoomResponse).state().room_code(), "555555")
      << "닫힌 농장의 코드를 게스트 방이 가져가면 안 된다";
}

TEST(FarmService, ACodeIsReservedWhileTheFarmIsStillBeingCreated) {
  // 농장을 만드는 중(저장소 응답을 기다리는 동안)에 게스트가 같은 코드로 방을 만들 수 없어야 한다.
  Fixture f(Sequence({"666666", "666666", "777777"}));
  f.repo->SetDelay(Millis(200));
  auto alice = f.accounts.Login(1, 1, "alice_01");
  f.service.HandleCreateRoom(alice, protocol::CreateRoomRequest{});  // 불러오기(200 ms) 뒤에 코드를 정해 만든다(200 ms)
  f.Settle(Millis(300));                                             // 지금은 만드는 중이다. 코드 666666이 정해졌다.
  ASSERT_EQ(alice->count(), 0u);

  auto guest = std::make_shared<FakeClient>(7);
  protocol::CreateRoomRequest request;
  request.set_nickname("guest_one");
  f.service.HandleCreateRoom(guest, request);
  EXPECT_EQ(guest->Last<protocol::CreateRoomResponse>(protocol::MessageId::kCreateRoomResponse).state().room_code(), "777777")
      << "만드는 중인 농장의 코드를 게스트가 가져가면 안 된다";

  ASSERT_TRUE(PumpUntil(f.io, [&] { return alice->count() >= 1; }, Millis(3000)));
  const auto response = alice->Last<protocol::CreateRoomResponse>(protocol::MessageId::kCreateRoomResponse);
  ASSERT_EQ(response.result(), protocol::GAME_OK);
  EXPECT_EQ(response.state().room_code(), "666666");
}

TEST(FarmService, FarmRoomsAndGuestRoomsAreKeptApart) {
  Fixture f;
  auto alice = f.accounts.Login(1, 1, "alice_01");
  const auto farm = f.Create(alice);
  ASSERT_EQ(farm.result(), protocol::GAME_OK);

  auto guest = std::make_shared<FakeClient>(7);
  EXPECT_EQ(f.Join(guest, farm.state().room_code(), "guest_one").result(), protocol::GAME_LOGIN_REQUIRED);

  protocol::CreateRoomRequest request;
  request.set_nickname("guest_two");
  auto guest2 = std::make_shared<FakeClient>(8);
  f.service.HandleCreateRoom(guest2, request);
  const std::string guest_code = guest2->Last<protocol::CreateRoomResponse>(protocol::MessageId::kCreateRoomResponse).state().room_code();
  EXPECT_EQ(f.Join(alice, guest_code, "x").result(), protocol::GAME_ALREADY_IN_ROOM);
  auto bob = f.accounts.Login(2, 2, "bobby_02");
  EXPECT_EQ(f.Join(bob, guest_code, "x").result(), protocol::GAME_ROOM_NOT_FOUND);
}

TEST(FarmService, WithoutAFarmRepositoryEveryRoomIsAGuestRoom) {
  boost::asio::io_context io;
  GameService service(io, TestConfig());
  FakeAccounts accounts;
  service.SetAccountHooks(accounts.Hooks());  // 로그인한 연결이 있어도 농장 저장소가 없으면 임시 방이다
  auto alice = accounts.Login(1, 1, "alice_01");
  protocol::CreateRoomRequest request;
  request.set_nickname("whatever");
  service.HandleCreateRoom(alice, request);
  const auto created = alice->Last<protocol::CreateRoomResponse>(protocol::MessageId::kCreateRoomResponse);
  ASSERT_EQ(created.result(), protocol::GAME_OK);
  EXPECT_EQ(created.state().players(0).nickname(), "whatever");
  EXPECT_EQ(service.FindRoom(created.state().room_code())->farm_id(), 0u);
}

TEST(FarmService, ALoggedInPlayerIsAskedForNoNicknameAndUsesTheAccountName) {
  Fixture f;
  auto alice = f.accounts.Login(1, 1, "alice_01");
  const auto response = f.Create(alice);  // 닉네임을 비워서 보내도 계정 이름을 쓴다
  ASSERT_EQ(response.result(), protocol::GAME_OK);
  ASSERT_EQ(response.state().players_size(), 1);
  EXPECT_EQ(response.state().players(0).nickname(), "alice_01");
  const Room* room = f.service.FindRoom(response.state().room_code());
  ASSERT_NE(room, nullptr);
  EXPECT_NE(room->farm_id(), 0u);
}

TEST(FarmService, ADoubleOpenRequestWhileTheFarmIsLoadingIsRefusedAsBusy) {
  Fixture f;
  f.repo->SetDelay(Millis(100));
  auto alice = f.accounts.Login(1, 1, "alice_01");
  f.service.HandleCreateRoom(alice, protocol::CreateRoomRequest{});
  f.service.HandleCreateRoom(alice, protocol::CreateRoomRequest{});  // 첫 요청이 처리 중
  ASSERT_GE(alice->count(), 1u);
  EXPECT_EQ(alice->Last<protocol::CreateRoomResponse>(protocol::MessageId::kCreateRoomResponse).result(), protocol::GAME_SERVER_BUSY);
  ASSERT_TRUE(PumpUntil(f.io, [&] { return alice->count() >= 2; }, Millis(3000)));
  EXPECT_EQ(f.repo->farm_count(), 1u) << "겹친 요청이 농장을 둘 만들면 안 된다";
}

TEST(FarmService, ShutdownSavesTheOpenFarmsAndTheirPlayers) {
  Fixture f;
  auto alice = f.accounts.Login(1, 1, "alice_01");
  const auto created = f.Create(alice);
  ASSERT_EQ(created.result(), protocol::GAME_OK);
  Room* room = const_cast<Room*>(f.service.FindRoom(created.state().room_code()));
  ASSERT_NE(room, nullptr);
  ASSERT_TRUE(room->GrantResources(1, 7, 3));
  f.accounts.stats[1].wood_gathered = 7;

  f.service.Shutdown();
  ASSERT_TRUE(PumpUntil(f.io, [&] { return f.repo->save_calls() == 1; }));
  const auto saved = f.repo->Get(1);
  ASSERT_TRUE(saved.has_value());
  ASSERT_EQ(saved->players.size(), 1u);
  EXPECT_EQ(saved->players[0].wood, 7u);
  EXPECT_EQ(saved->players[0].stone, 3u);
  const auto stats = f.accounts_repo->StatsOf(1);
  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(stats->wood_gathered, 7u);
}


// ============================================================ 시계 저장과 복원 (docs/content-day.md 7절, 완료 기준 9)

PlayConfig ClockConfig(std::uint32_t bench_day_ticks = 0, std::uint64_t flush_ticks = 0) {
  PlayConfig config = TestConfig();
  config.bench_day_ticks = bench_day_ticks;
  config.farm_flush_interval_ticks = flush_ticks;
  config.gather_range_cm = 100000.0f;
  config.gather_cooldown_ticks = 0;
  return config;
}

bool SavedClockIs(const Fixture& f, std::uint32_t day, std::uint64_t elapsed) {
  const auto saved = f.repo->Get(1);
  return saved && saved->clock && saved->clock->day == day && saved->clock->day_elapsed_ticks == elapsed;
}

TEST(FarmClock, TheNightSavesTheNewDayAtOnceWithoutWaitingForThePeriodicSave) {
  Fixture f(nullptr, ClockConfig(/*bench_day_ticks=*/50, /*flush_ticks=*/0));  // 주기 저장 없음
  auto alice = f.accounts.Login(1, 1, "alice_01");
  ASSERT_EQ(f.Create(alice).result(), protocol::GAME_OK);
  f.service.Tick(49);
  f.Settle();
  EXPECT_EQ(f.repo->save_calls(), 0u) << "밤 전에는 저장하지 않는다";
  f.service.Tick();  // 50틱: 강제 정산
  ASSERT_EQ(alice->CountOf(protocol::MessageId::kDayStarted), 1u);
  EXPECT_TRUE(PumpUntil(f.io, [&] { return SavedClockIs(f, 2, 0); })) << "정산한 틱에 새 날(경과 0)이 바로 저장된다";
}

TEST(FarmClock, ClosingSavesTheClockAndReopeningContinuesFromTheSameMomentLaterOnlyByTheRemainingTicks) {
  Fixture f(nullptr, ClockConfig());  // 기본 하루 12,000틱
  auto alice = f.accounts.Login(1, 1, "alice_01");
  ASSERT_EQ(f.Create(alice).result(), protocol::GAME_OK);
  f.service.Tick(5000);
  f.service.HandleLeaveRoom(alice);  // 경과 5,000에 농장이 닫힌다
  ASSERT_TRUE(PumpUntil(f.io, [&] { return SavedClockIs(f, 1, 5000); })) << "닫힌 시각의 시계가 바로 저장된다";

  // 닫혀 있는 동안 서버의 다른 시간이 흘러도 농장 시계는 그대로다.
  auto guest = std::make_shared<FakeClient>(9);
  protocol::CreateRoomRequest guest_room;
  guest_room.set_nickname("guest");
  f.service.HandleCreateRoom(guest, guest_room);
  f.service.Tick(3000);

  const auto reopened = f.Create(alice);
  ASSERT_EQ(reopened.result(), protocol::GAME_OK);
  EXPECT_EQ(reopened.state().clock().day(), 1u);
  EXPECT_EQ(reopened.state().clock().day_start_tick(), -5000) << "새 방의 월드 틱 0에서 경과 5,000";
  f.service.Tick(6999);
  EXPECT_EQ(alice->CountOf(protocol::MessageId::kDayStarted), 0u) << "다시 연 뒤 6,999틱(경과 11,999)에는 아직";
  f.service.Tick();
  ASSERT_EQ(alice->CountOf(protocol::MessageId::kDayStarted), 1u) << "다시 연 뒤 7,000틱에 강제 정산";
  EXPECT_TRUE(alice->LastOf<protocol::DayStarted>(protocol::MessageId::kDayStarted).forced());
}

TEST(FarmClock, AFarmSavedBeforeTheClockExistedStartsOnDayOneAtSixOClock) {
  Fixture f(nullptr, ClockConfig());
  f.repo->Seed(1, "123456");  // 시계 행이 없다
  auto alice = f.accounts.Login(1, 1, "alice_01");
  const auto opened = f.Create(alice);
  ASSERT_EQ(opened.result(), protocol::GAME_OK);
  EXPECT_EQ(opened.state().room_code(), "123456");
  EXPECT_EQ(opened.state().clock().day(), 1u);
  EXPECT_EQ(opened.state().clock().day_start_tick(), 0);
}

TEST(FarmClock, AStoredElapsedLongerThanTheDayIsForcedOnTheNextTick) {
  Fixture f(nullptr, ClockConfig());
  f.repo->Seed(1, "123456", {}, {}, farm::StoredClock{3, 999999});
  auto alice = f.accounts.Login(1, 1, "alice_01");
  const auto opened = f.Create(alice);
  ASSERT_EQ(opened.result(), protocol::GAME_OK);
  EXPECT_EQ(opened.state().clock().day(), 3u);
  EXPECT_EQ(opened.state().clock().day_start_tick(), -12000) << "경과를 하루 길이로 자른다";
  f.service.Tick();
  ASSERT_EQ(alice->CountOf(protocol::MessageId::kDayStarted), 1u) << "다음 틱에 강제 정산";
  const auto night = alice->LastOf<protocol::DayStarted>(protocol::MessageId::kDayStarted);
  EXPECT_TRUE(night.forced());
  EXPECT_EQ(night.clock().day(), 4u);
}

TEST(FarmClock, AStoredHouseUnderConstructionCompletesAtTheNextNightWhateverItsRemainingTicks) {
  Fixture f(nullptr, ClockConfig(/*bench_day_ticks=*/20));
  farm::StoredStructure house;
  house.structure_id = 1;
  house.x = 2400.0f;
  house.y = 2000.0f;
  house.state = 0;               // 건설 중
  house.remaining_ticks = 77777;  // 옛 규칙의 남은 틱(이제 쓰지 않는다)
  f.repo->Seed(1, "123456", {house});
  auto alice = f.accounts.Login(1, 1, "alice_01");
  const auto opened = f.Create(alice);
  ASSERT_EQ(opened.result(), protocol::GAME_OK);
  ASSERT_EQ(opened.state().structures_size(), 1);
  EXPECT_EQ(opened.state().structures(0).state(), protocol::STRUCTURE_BUILDING);
  EXPECT_EQ(opened.state().structures(0).ready_tick(), 0u);
  f.service.Tick(20);  // 밤 정산
  EXPECT_EQ(alice->LastOf<protocol::StructureUpdate>(protocol::MessageId::kStructureUpdate).structures(0).state(),
            protocol::STRUCTURE_COMPLETE);
  EXPECT_TRUE(PumpUntil(f.io, [&] {
    const auto saved = f.repo->Get(1);
    return saved && saved->structures.size() == 1 && saved->structures[0].state == 1u &&
           saved->structures[0].remaining_ticks == 0u;
  })) << "완공된 집이 밤 정산과 함께 저장된다";
}

TEST(FarmClock, AFailedNightSaveKeepsTheGameGoingAndIsRetriedOnTheNextPeriod) {
  Fixture f(nullptr, ClockConfig(/*bench_day_ticks=*/30, /*flush_ticks=*/5));
  auto alice = f.accounts.Login(1, 1, "alice_01");
  ASSERT_EQ(f.Create(alice).result(), protocol::GAME_OK);
  f.repo->SetSaveFailing(true);
  f.service.Tick(30);  // 밤 정산, 저장 실패
  ASSERT_EQ(alice->CountOf(protocol::MessageId::kDayStarted), 1u);
  ASSERT_TRUE(PumpUntil(f.io, [&] { return f.repo->failed_saves() >= 1; }));
  const std::size_t snapshots = alice->CountOf(protocol::MessageId::kWorldSnapshot);
  f.service.Tick();
  f.service.Tick();
  EXPECT_EQ(alice->CountOf(protocol::MessageId::kWorldSnapshot), snapshots + 2) << "저장이 실패해도 게임은 계속된다";
  EXPECT_FALSE(f.repo->Get(1)->clock.has_value());

  f.repo->SetSaveFailing(false);
  for (int i = 0; i < 3; ++i) f.service.Tick();  // 35틱: 다음 주기 저장
  EXPECT_TRUE(PumpUntil(f.io, [&] { return SavedClockIs(f, 2, 5); })) << "다음 주기에 (지금의) 2일차 시계로 다시 저장된다";
}

TEST(FarmClock, ThePeriodicSaveDoesNotWriteAFarmWhoseOnlyChangeIsTheClock) {
  Fixture f(nullptr, ClockConfig(/*bench_day_ticks=*/0, /*flush_ticks=*/5));
  auto alice = f.accounts.Login(1, 1, "alice_01");
  ASSERT_EQ(f.Create(alice).result(), protocol::GAME_OK);
  f.service.Tick(20);
  f.Settle();
  EXPECT_EQ(f.repo->save_calls(), 0u) << "시계만 흐른 농장은 주기마다 쓰지 않는다";
  protocol::GatherRequest gather;
  gather.set_node_id(1);
  f.service.HandleGather(alice, gather);  // 채집은 모아서 저장한다
  for (int i = 0; i < 5; ++i) f.service.Tick();
  EXPECT_TRUE(PumpUntil(f.io, [&] { return SavedClockIs(f, 1, 25); })) << "어차피 저장할 때 지금 시계를 함께 싣는다";
}

TEST(FarmClock, ShutdownSavesTheClockOfEveryOpenFarm) {
  Fixture f(nullptr, ClockConfig());
  auto alice = f.accounts.Login(1, 1, "alice_01");
  ASSERT_EQ(f.Create(alice).result(), protocol::GAME_OK);
  f.service.Tick(123);
  f.service.Shutdown();
  EXPECT_TRUE(PumpUntil(f.io, [&] { return SavedClockIs(f, 1, 123); }));
}

TEST(FarmClock, AGuestRoomsNightIsNeverSaved) {
  Fixture f(nullptr, ClockConfig(/*bench_day_ticks=*/10, /*flush_ticks=*/5));
  auto guest = std::make_shared<FakeClient>(9);
  protocol::CreateRoomRequest request;
  request.set_nickname("guest");
  f.service.HandleCreateRoom(guest, request);
  ASSERT_EQ(guest->LastOf<protocol::CreateRoomResponse>(protocol::MessageId::kCreateRoomResponse).result(), protocol::GAME_OK);
  for (int i = 0; i < 25; ++i) f.service.Tick();
  EXPECT_EQ(guest->CountOf(protocol::MessageId::kDayStarted), 2u);
  f.service.HandleLeaveRoom(guest);
  f.Settle();
  EXPECT_EQ(f.repo->save_calls(), 0u) << "게스트 방은 시계를 저장하지 않는다";
  EXPECT_EQ(f.repo->farm_count(), 0u);
}

}  // namespace
}  // namespace game::play
