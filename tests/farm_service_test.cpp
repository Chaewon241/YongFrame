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
  explicit Fixture(RoomCodeGenerator generator = nullptr)
      : accounts_repo(std::make_shared<account::InMemoryAccountRepository>(io)),
        repo(std::make_shared<farm::InMemoryFarmRepository>(io, accounts_repo)),
        service(io, TestConfig(), std::move(generator)) {
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

}  // namespace
}  // namespace game::play
