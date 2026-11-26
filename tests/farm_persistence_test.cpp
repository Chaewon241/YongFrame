// FarmPersistence 단위 시험: 쓰기 대기열이 "건설은 바로, 채집은 모아서" 저장하고, 실패하면 값을 잃지 않고 다시
// 저장하며, 더 새로운 값이 오래된 값에 덮이지 않는지를 인메모리 농장 저장소로 확인한다.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>

#include "account_test_support.h"
#include "farm/in_memory_farm_repository.h"
#include "play/farm_persistence.h"
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

constexpr std::uint64_t kOwner = 1;

struct Fixture {
  explicit Fixture(std::uint64_t interval = 0) {
    accounts = std::make_shared<account::InMemoryAccountRepository>(io);
    repo = std::make_shared<farm::InMemoryFarmRepository>(io, accounts);
    farm = repo->Seed(kOwner, "123456");
    persistence = FarmPersistence::Create(repo, interval);
  }

  farm::StoredPlayer Player(std::uint64_t account, std::uint32_t wood, std::uint32_t stone = 0) const {
    return farm::StoredPlayer{account, wood, stone};
  }

  // 저장소 호출이 모두 끝날 때까지 돌린다.
  void Settle(Millis duration = Millis(150)) {
    const auto end = std::chrono::steady_clock::now() + duration;
    PumpUntil(io, [&] { return std::chrono::steady_clock::now() >= end; }, duration + Millis(500));
  }

  std::uint32_t SavedWood(std::uint64_t account = kOwner) const {
    const auto saved = repo->Get(kOwner);
    if (!saved) return 0xFFFFFFFF;
    for (const auto& p : saved->players) {
      if (p.account_id == account) return p.wood;
    }
    return 0xFFFFFFFE;  // 행이 없다
  }

  boost::asio::io_context io;
  std::shared_ptr<account::InMemoryAccountRepository> accounts;
  std::shared_ptr<farm::InMemoryFarmRepository> repo;
  farm::FarmData farm;
  std::shared_ptr<FarmPersistence> persistence;
};

TEST(FarmPersistence, NothingIsSavedUntilFlushAndThenEverythingGoesInOneSave) {
  Fixture f;
  farm::StoredStructure house;
  house.structure_id = 1;
  house.x = 2400;
  house.y = 2800;
  f.persistence->UpdateStructures(f.farm.farm_id, {house});
  f.persistence->UpdatePlayer(f.farm.farm_id, f.Player(kOwner, 7, 3), account::AccountStats{7, 3, 1});
  f.Settle();
  EXPECT_EQ(f.repo->save_calls(), 0u) << "Flush 전에는 저장소에 쓰지 않는다";
  EXPECT_TRUE(f.persistence->HasPending(f.farm.farm_id));

  f.persistence->Flush(f.farm.farm_id);
  ASSERT_TRUE(PumpUntil(f.io, [&] { return f.repo->save_calls() == 1; }));
  const auto saved = f.repo->Get(kOwner);
  ASSERT_TRUE(saved.has_value());
  ASSERT_EQ(saved->structures.size(), 1u);
  EXPECT_EQ(saved->structures[0].x, 2400.0f);
  EXPECT_EQ(f.SavedWood(), 7u);
  f.Settle();
  EXPECT_FALSE(f.persistence->HasPending(f.farm.farm_id));
  EXPECT_EQ(f.persistence->stats().saves_ok.load(), 1u);
  EXPECT_EQ(f.persistence->stats().flushes.load(), 1u);
}

TEST(FarmPersistence, AccountStatsAreSavedTogetherAndVisibleToTheAccountRepository) {
  Fixture f;
  f.persistence->UpdatePlayer(f.farm.farm_id, f.Player(kOwner, 5), account::AccountStats{12, 4, 2});
  f.persistence->Flush(f.farm.farm_id);
  ASSERT_TRUE(PumpUntil(f.io, [&] { return f.repo->save_calls() == 1; }));

  std::optional<account::RepoResult<account::AccountStats>> loaded;
  f.accounts->LoadStats(kOwner, [&](auto r) { loaded = std::move(r); });
  ASSERT_TRUE(PumpUntil(f.io, [&] { return loaded.has_value(); }));
  EXPECT_EQ(loaded->value.wood_gathered, 12u);
  EXPECT_EQ(loaded->value.stone_gathered, 4u);
  EXPECT_EQ(loaded->value.houses_built, 2u);
}

TEST(FarmPersistence, TheLatestValueWinsBeforeAFlush) {
  Fixture f;
  f.persistence->UpdatePlayer(f.farm.farm_id, f.Player(kOwner, 1), account::AccountStats{1, 0, 0});
  f.persistence->UpdatePlayer(f.farm.farm_id, f.Player(kOwner, 2), account::AccountStats{2, 0, 0});
  f.persistence->UpdatePlayer(f.farm.farm_id, f.Player(kOwner, 9), account::AccountStats{9, 0, 0});
  f.persistence->Flush(f.farm.farm_id);
  ASSERT_TRUE(PumpUntil(f.io, [&] { return f.repo->save_calls() == 1; }));
  EXPECT_EQ(f.SavedWood(), 9u);
  EXPECT_EQ(f.repo->save_calls(), 1u) << "모아서 한 번에 저장한다";
}

TEST(FarmPersistence, OnTickFlushesOnlyWhenTheIntervalHasPassed) {
  Fixture f(/*interval=*/10);
  f.persistence->UpdatePlayer(f.farm.farm_id, f.Player(kOwner, 4), std::nullopt);
  f.persistence->OnTick(4);
  f.persistence->OnTick(5);
  f.Settle();
  EXPECT_EQ(f.repo->save_calls(), 0u) << "9틱까지는 저장하지 않는다";
  f.persistence->OnTick(1);  // 합계 10틱
  ASSERT_TRUE(PumpUntil(f.io, [&] { return f.repo->save_calls() == 1; }));
  EXPECT_EQ(f.SavedWood(), 4u);

  // 다음 주기에는 변경이 없으면 저장하지 않는다.
  f.persistence->OnTick(10);
  f.Settle();
  EXPECT_EQ(f.repo->save_calls(), 1u);
}

TEST(FarmPersistence, AZeroIntervalNeverFlushesOnTick) {
  Fixture f(/*interval=*/0);
  f.persistence->UpdatePlayer(f.farm.farm_id, f.Player(kOwner, 4), std::nullopt);
  f.persistence->OnTick(100000);
  f.Settle();
  EXPECT_EQ(f.repo->save_calls(), 0u);
}

TEST(FarmPersistence, AFailedSaveKeepsTheValuesAndTheNextIntervalRetriesThem) {
  Fixture f(/*interval=*/10);
  f.repo->SetSaveFailing(true);
  f.persistence->UpdatePlayer(f.farm.farm_id, f.Player(kOwner, 6), account::AccountStats{6, 0, 0});
  f.persistence->Flush(f.farm.farm_id);
  ASSERT_TRUE(PumpUntil(f.io, [&] { return f.repo->failed_saves() == 1; }));
  f.Settle();
  EXPECT_TRUE(f.persistence->HasPending(f.farm.farm_id)) << "실패한 값은 대기열에 남아야 한다";
  EXPECT_EQ(f.persistence->stats().saves_failed.load(), 1u);
  EXPECT_EQ(f.persistence->stats().consecutive_failures.load(), 1u);
  EXPECT_EQ(f.repo->failed_saves(), 1u) << "실패한 직후에 바로 다시 시도하지 않는다";

  f.repo->SetSaveFailing(false);
  f.persistence->OnTick(10);
  ASSERT_TRUE(PumpUntil(f.io, [&] { return f.repo->save_calls() == 1; }));
  EXPECT_EQ(f.SavedWood(), 6u);
  f.Settle();
  EXPECT_FALSE(f.persistence->HasPending(f.farm.farm_id));
  EXPECT_EQ(f.persistence->stats().consecutive_failures.load(), 0u) << "성공하면 연속 실패가 0으로 돌아간다";
}

TEST(FarmPersistence, ANewerValueIsNotOverwrittenByTheFailedOlderOne) {
  Fixture f(/*interval=*/10);
  f.repo->SetSaveFailing(true);
  f.repo->SetDelay(Millis(60));  // 저장이 진행되는 동안 더 새로운 값이 들어오게 한다
  f.persistence->UpdatePlayer(f.farm.farm_id, f.Player(kOwner, 1), account::AccountStats{1, 0, 0});
  f.persistence->Flush(f.farm.farm_id);  // 값 1을 저장 중(곧 실패)
  f.persistence->UpdatePlayer(f.farm.farm_id, f.Player(kOwner, 2), account::AccountStats{2, 0, 0});
  ASSERT_TRUE(PumpUntil(f.io, [&] { return f.repo->failed_saves() == 1; }));
  f.Settle(Millis(100));

  f.repo->SetSaveFailing(false);
  f.repo->SetDelay(Millis(0));
  f.persistence->OnTick(10);
  ASSERT_TRUE(PumpUntil(f.io, [&] { return f.repo->save_calls() == 1; }));
  EXPECT_EQ(f.SavedWood(), 2u) << "실패한 오래된 값(1)이 더 새로운 값(2)을 덮으면 안 된다";
}

TEST(FarmPersistence, OnlyOneSaveIsInFlightPerFarmAndTheFollowUpKeepsTheOrder) {
  Fixture f;
  f.repo->SetDelay(Millis(50));
  f.persistence->UpdatePlayer(f.farm.farm_id, f.Player(kOwner, 1), std::nullopt);
  f.persistence->Flush(f.farm.farm_id);
  f.persistence->UpdatePlayer(f.farm.farm_id, f.Player(kOwner, 2), std::nullopt);
  f.persistence->Flush(f.farm.farm_id);  // 저장 중이므로 끝난 뒤에 이어서 저장된다
  EXPECT_EQ(f.persistence->stats().flushes.load(), 1u) << "진행 중인 저장이 있으면 새 저장을 겹쳐 보내지 않는다";
  ASSERT_TRUE(PumpUntil(f.io, [&] { return f.repo->save_calls() == 2; }));
  EXPECT_EQ(f.SavedWood(), 2u) << "나중 값이 최종 값이어야 한다";
  f.Settle();
  EXPECT_FALSE(f.persistence->HasPending(f.farm.farm_id));
}

TEST(FarmPersistence, FlushAllSavesEveryFarmAndDoesNotRetryFailures) {
  Fixture f;
  const auto second = f.repo->Seed(2, "654321");
  f.persistence->UpdatePlayer(f.farm.farm_id, f.Player(kOwner, 3), std::nullopt);
  f.persistence->UpdatePlayer(second.farm_id, f.Player(2, 8), std::nullopt);
  f.persistence->FlushAll();
  ASSERT_TRUE(PumpUntil(f.io, [&] { return f.repo->save_calls() == 2; }));
  EXPECT_EQ(f.SavedWood(), 3u);
  const auto saved_second = f.repo->Get(2);
  ASSERT_TRUE(saved_second.has_value());
  ASSERT_EQ(saved_second->players.size(), 1u);
  EXPECT_EQ(saved_second->players[0].wood, 8u);

  // 종료 중의 실패는 다시 시도하지 않는다(서버가 끝나는 중이다).
  f.repo->SetSaveFailing(true);
  f.persistence->UpdatePlayer(f.farm.farm_id, f.Player(kOwner, 5), std::nullopt);
  f.persistence->FlushAll();
  f.Settle(Millis(300));
  EXPECT_EQ(f.repo->failed_saves(), 1u);
}

TEST(FarmPersistence, FlushWithNothingPendingDoesNothing) {
  Fixture f;
  f.persistence->Flush(f.farm.farm_id);
  f.persistence->Flush(12345);  // 모르는 농장
  f.persistence->FlushAll();
  f.Settle();
  EXPECT_EQ(f.repo->save_calls(), 0u);
  EXPECT_EQ(f.persistence->stats().flushes.load(), 0u);
}

TEST(FarmPersistence, SeparateFarmsAreSavedIndependently) {
  Fixture f;
  const auto second = f.repo->Seed(2, "654321");
  f.persistence->UpdatePlayer(f.farm.farm_id, f.Player(kOwner, 3), std::nullopt);
  f.persistence->UpdatePlayer(second.farm_id, f.Player(2, 8), std::nullopt);
  f.persistence->Flush(second.farm_id);  // 두 번째 농장만
  ASSERT_TRUE(PumpUntil(f.io, [&] { return f.repo->save_calls() == 1; }));
  EXPECT_TRUE(f.persistence->HasPending(f.farm.farm_id)) << "저장하지 않은 농장은 대기열에 남는다";
  EXPECT_EQ(f.SavedWood(), 0xFFFFFFFEu);
}


// ============================================================ 시계 (docs/content-day.md 7절)

TEST(FarmPersistence, AQueuedClockAloneIsSavedOnFlush) {
  Fixture f;
  f.persistence->UpdateClock(f.farm.farm_id, farm::StoredClock{3, 120});
  EXPECT_TRUE(f.persistence->HasPending(f.farm.farm_id)) << "시계만으로도 저장할 것이 있다";
  f.persistence->Flush(f.farm.farm_id);
  ASSERT_TRUE(PumpUntil(f.io, [&] { return f.repo->save_calls() == 1; }));
  const auto saved = f.repo->Get(kOwner);
  ASSERT_TRUE(saved->clock.has_value());
  EXPECT_EQ(saved->clock->day, 3u);
  EXPECT_EQ(saved->clock->day_elapsed_ticks, 120u);
}

TEST(FarmPersistence, ThePeriodicSaveCarriesTheLiveClockOnlyWhenSomethingElseChanged) {
  Fixture f(/*interval=*/10);
  f.persistence->SetClockSource([](std::uint64_t) { return std::optional<farm::StoredClock>(farm::StoredClock{2, 77}); });
  f.persistence->OnTick(10);
  f.Settle();
  EXPECT_EQ(f.repo->save_calls(), 0u) << "시계만 바뀐 농장은 주기마다 쓰지 않는다";

  f.persistence->UpdatePlayer(f.farm.farm_id, f.Player(kOwner, 4), std::nullopt);
  f.persistence->OnTick(10);
  ASSERT_TRUE(PumpUntil(f.io, [&] { return f.repo->save_calls() == 1; }));
  const auto saved = f.repo->Get(kOwner);
  ASSERT_TRUE(saved->clock.has_value()) << "다른 변경을 저장할 때 지금 시계를 함께 싣는다";
  EXPECT_EQ(saved->clock->day, 2u);
  EXPECT_EQ(saved->clock->day_elapsed_ticks, 77u);
}

TEST(FarmPersistence, AnOpenFarmSavesItsLiveClockAndAClosedFarmItsQueuedClock) {
  Fixture f;
  std::optional<farm::StoredClock> live = farm::StoredClock{1, 60};
  f.persistence->SetClockSource([&live](std::uint64_t) { return live; });
  f.persistence->UpdateClock(f.farm.farm_id, farm::StoredClock{1, 50});
  f.persistence->Flush(f.farm.farm_id);
  ASSERT_TRUE(PumpUntil(f.io, [&] { return f.repo->save_calls() == 1; }));
  EXPECT_EQ(f.repo->Get(kOwner)->clock->day_elapsed_ticks, 60u) << "열린 농장은 지금 시계가 가장 새 값이다";

  live.reset();  // 농장이 닫혔다
  f.persistence->UpdateClock(f.farm.farm_id, farm::StoredClock{1, 70});
  f.persistence->Flush(f.farm.farm_id);
  ASSERT_TRUE(PumpUntil(f.io, [&] { return f.repo->save_calls() == 2; }));
  EXPECT_EQ(f.repo->Get(kOwner)->clock->day_elapsed_ticks, 70u) << "닫힌 농장은 넣어 둔 값(닫힌 시각)";
}

TEST(FarmPersistence, AFailedSaveKeepsTheClockAndANewerClockWins) {
  Fixture f;
  f.repo->SetSaveFailing(true);
  f.persistence->UpdateClock(f.farm.farm_id, farm::StoredClock{4, 5});
  f.persistence->Flush(f.farm.farm_id);
  ASSERT_TRUE(PumpUntil(f.io, [&] { return f.repo->failed_saves() == 1; }));
  EXPECT_TRUE(f.persistence->HasPending(f.farm.farm_id)) << "실패한 시계는 대기열로 돌아온다";
  f.repo->SetSaveFailing(false);
  f.persistence->Flush(f.farm.farm_id);
  ASSERT_TRUE(PumpUntil(f.io, [&] { return f.repo->save_calls() == 1; }));
  EXPECT_EQ(f.repo->Get(kOwner)->clock->day, 4u);
  EXPECT_EQ(f.repo->Get(kOwner)->clock->day_elapsed_ticks, 5u);

  // 저장하는 동안 더 새로운 시계가 들어오면, 저장이 실패해도 새 값이 이긴다.
  f.repo->SetSaveFailing(true);
  f.persistence->UpdateClock(f.farm.farm_id, farm::StoredClock{5, 10});
  f.persistence->Flush(f.farm.farm_id);
  f.persistence->UpdateClock(f.farm.farm_id, farm::StoredClock{5, 20});
  ASSERT_TRUE(PumpUntil(f.io, [&] { return f.repo->failed_saves() == 2; }));
  f.repo->SetSaveFailing(false);
  f.persistence->Flush(f.farm.farm_id);
  ASSERT_TRUE(PumpUntil(f.io, [&] { return f.repo->save_calls() == 2; }));
  EXPECT_EQ(f.repo->Get(kOwner)->clock->day_elapsed_ticks, 20u);
}

}  // namespace
}  // namespace game::play
