// 하루와 잠의 순수 로직 테스트: 방 시계, 잠자기/깨어나기, 밤 정산 판정과 내용(Room). 소켓도 시계도 없다.
// 시간은 월드 틱(Step 호출 횟수)으로만 흐른다. docs/content-day.md 완료 기준 1~6, 8.

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include "play/room.h"

namespace game::play {
namespace {

constexpr float kDt = 0.05f;

// 노드가 없는 설정(잠과 시계만 따로 시험한다). 시작 집은 기본값 그대로 있다.
PlayConfig NoNodes() {
  PlayConfig config;
  config.tree_count = 0;
  config.rock_count = 0;
  return config;
}

void Steps(Room& room, std::uint64_t ticks) {
  for (std::uint64_t i = 0; i < ticks; ++i) {
    room.world().Step(kDt);
  }
}

// 방에 n명을 넣는다(client 1..n, 칸 0..n-1).
void Fill(Room& room, std::uint64_t n) {
  for (std::uint64_t id = 1; id <= n; ++id) {
    room.AddMember(id, "p" + std::to_string(id));
  }
}

std::uint32_t PlayerOf(const Room& room, std::uint64_t client_id) { return room.FindByClient(client_id)->player_id; }

// ============================================================ 시계

TEST(DayClockTest, ANewRoomStartsOnDayOneAtSixOClockAndAMinuteIsTenTicks) {
  const PlayConfig config;
  EXPECT_EQ(config.DayLengthTicks(), 12000u) << "(1560 - 360) x 10";
  Room room("111111", config);
  Fill(room, 1);
  EXPECT_EQ(room.day(), 1u);
  EXPECT_EQ(room.day_start_tick(), 0);
  EXPECT_EQ(room.DayElapsed(), 0u);
  EXPECT_EQ(room.GameMinute(), 360u) << "06:00";
  Steps(room, 9);
  EXPECT_EQ(room.GameMinute(), 360u);
  Steps(room, 1);
  EXPECT_EQ(room.GameMinute(), 361u) << "10틱에 1분";
  Steps(room, 10 * 60 - 10);
  EXPECT_EQ(room.GameMinute(), 420u) << "600틱에 07:00";
}

TEST(DayClockTest, WithSomeoneAwakeTheDayIsNotEndedAt11999ButIsForcedAt12000) {
  Room room("111111", NoNodes());
  Fill(room, 2);
  EXPECT_EQ(room.ShouldEndDay(0), NightTrigger::kNone);
  EXPECT_EQ(room.ShouldEndDay(11999), NightTrigger::kNone);
  EXPECT_EQ(room.ShouldEndDay(12000), NightTrigger::kForced);
  EXPECT_EQ(room.ShouldEndDay(12001), NightTrigger::kForced) << "넘어서도(따라잡기) 강제";
}

TEST(DayClockTest, BenchDayTicksReplacesTheDayLength) {
  PlayConfig config = NoNodes();
  config.bench_day_ticks = 200;
  EXPECT_EQ(config.DayLengthTicks(), 200u);
  Room room("111111", config);
  Fill(room, 1);
  EXPECT_EQ(room.ShouldEndDay(199), NightTrigger::kNone);
  EXPECT_EQ(room.ShouldEndDay(200), NightTrigger::kForced);
}

TEST(DayClockTest, ARestoredClockContinuesFromTheSavedElapsedEvenIfTheStartTickIsNegative) {
  Room room("111111", NoNodes());
  EXPECT_FALSE(room.RestoreClock(3, 5000));
  Fill(room, 1);
  EXPECT_EQ(room.day(), 3u);
  EXPECT_EQ(room.day_start_tick(), -5000) << "월드 틱 0 - 경과 5000";
  EXPECT_EQ(room.DayElapsed(), 5000u);
  EXPECT_EQ(room.GameMinute(), 360u + 500u);
  EXPECT_EQ(room.ShouldEndDay(6999), NightTrigger::kNone);
  EXPECT_EQ(room.ShouldEndDay(7000), NightTrigger::kForced) << "경과 5000에서 7000틱 뒤에 강제 정산";
  const ClockState exported = room.ExportClock();
  EXPECT_EQ(exported.day, 3u);
  EXPECT_EQ(exported.elapsed_ticks, 5000u);
}

TEST(DayClockTest, AnElapsedLongerThanTheDayIsCutToTheDayLengthAndForcesTheNextNight) {
  Room room("111111", NoNodes());
  EXPECT_TRUE(room.RestoreClock(2, 50000)) << "잘랐다고 알린다";
  Fill(room, 1);
  EXPECT_EQ(room.DayElapsed(), 12000u);
  Steps(room, 1);
  EXPECT_EQ(room.ShouldEndDay(room.world().tick()), NightTrigger::kForced);
  Room exact("222222", NoNodes());
  EXPECT_FALSE(exact.RestoreClock(2, 12000)) << "하루 길이와 같으면 자르지 않는다";
  Room zero("333333", NoNodes());
  zero.RestoreClock(0, 0);
  EXPECT_EQ(zero.day(), 1u) << "날 0은 1로 둔다";
}

TEST(DayClockTest, ARestoredClockOnARoomThatHasAlreadyTickedUsesTheCurrentTick) {
  Room room("111111", NoNodes());
  Steps(room, 100);
  room.RestoreClock(1, 30);
  EXPECT_EQ(room.day_start_tick(), 70);
  EXPECT_EQ(room.DayElapsed(), 30u);
}

// ============================================================ 잠자기와 깨어나기

TEST(SleepTest, TheNorthSlotsCanSleepRightAwayButTheSouthSlotsMustWalk) {
  Room room("111111", NoNodes());
  Fill(room, 4);
  // 시작 집 아래 면 y = 2700. 칸 0, 1은 y = 2400(300 cm), 칸 2, 3은 y = 2600(100 cm).
  EXPECT_EQ(room.Sleep(1, true).status, SleepStatus::kTooFar);
  EXPECT_EQ(room.Sleep(2, true).status, SleepStatus::kTooFar);
  EXPECT_EQ(room.Sleep(3, true).status, SleepStatus::kOk);
  EXPECT_EQ(room.Sleep(4, true).status, SleepStatus::kOk);
}

TEST(SleepTest, TheFarmhouseRangeIncludesTheBoundary) {
  // 시작 집 중심을 옮겨 아래 면을 정확히 150 cm(또는 150.1 cm) 떨어지게 한다. 칸 2는 (2400, 2600).
  PlayConfig at = NoNodes();
  at.farmhouse_y_cm = 2900.0f;  // 아래 면 2750
  Room room("111111", at);
  Fill(room, 3);
  EXPECT_EQ(room.Sleep(3, true).status, SleepStatus::kOk) << "150.0 cm는 허용";

  PlayConfig beyond = NoNodes();
  beyond.farmhouse_y_cm = 2900.1f;  // 아래 면 약 2750.1
  Room far("222222", beyond);
  Fill(far, 3);
  EXPECT_EQ(far.Sleep(3, true).status, SleepStatus::kTooFar) << "150.1 cm는 거부";
}

TEST(SleepTest, ACompletedHouseCanBeSleptAtButAHouseUnderConstructionCannot) {
  PlayConfig config = NoNodes();
  Room room("111111", config);
  Fill(room, 1);  // (2400, 2400). 시작 집까지 300 cm
  // 집(300 x 300) 위 면이 정확히 150 cm 아래(y = 2250)에 오게 짓는다.
  room.structures().Add(StructureType::kHouse, 2400.0f, 2100.0f);
  EXPECT_EQ(room.Sleep(1, true).status, SleepStatus::kTooFar) << "건설 중인 집 앞에서는 잘 수 없다";
  room.structures().CompleteAll(room.world().tick());
  const SleepOutcome out = room.Sleep(1, true);
  EXPECT_EQ(out.status, SleepStatus::kOk) << "완공된 집 앞(150.0 cm)에서는 잔다";
  EXPECT_TRUE(out.sleeping);
  EXPECT_TRUE(out.changed);

  Room far("222222", config);
  Fill(far, 1);
  far.structures().Add(StructureType::kHouse, 2400.0f, 2099.9f);  // 150.1 cm
  far.structures().CompleteAll(0);
  EXPECT_EQ(far.Sleep(1, true).status, SleepStatus::kTooFar);
}

TEST(SleepTest, TheRangeIsMeasuredToTheNearestPointOfTheFootprintIncludingCorners) {
  PlayConfig config = NoNodes();
  config.farmhouse_width_cm = 0.0f;  // 시작 집 없이 집 하나로 시험
  Room room("111111", config);
  Fill(room, 1);  // (2400, 2400)
  // 집 모서리(2550, 2550)까지 대각선 거리 150 * sqrt(2) = 212: 축마다는 150이지만 거리로는 멀다.
  room.structures().Add(StructureType::kHouse, 2700.0f, 2700.0f);
  room.structures().CompleteAll(0);
  EXPECT_EQ(room.Sleep(1, true).status, SleepStatus::kTooFar);
  // 모서리까지 정확히 (90, 120) -> 150
  Room corner("222222", config);
  Fill(corner, 1);
  corner.structures().Add(StructureType::kHouse, 2400.0f + 90.0f + 150.0f, 2400.0f + 120.0f + 150.0f);
  corner.structures().CompleteAll(0);
  EXPECT_EQ(corner.Sleep(1, true).status, SleepStatus::kOk);
}

TEST(SleepTest, AlreadyAsleepAndAlreadyAwakeAreSuccessesThatChangeNothing) {
  Room room("111111", NoNodes());
  Fill(room, 3);
  SleepOutcome out = room.Sleep(3, false);
  EXPECT_EQ(out.status, SleepStatus::kOk) << "이미 깨어 있으면 성공";
  EXPECT_FALSE(out.sleeping);
  EXPECT_FALSE(out.changed);
  ASSERT_EQ(room.Sleep(3, true).status, SleepStatus::kOk);
  out = room.Sleep(3, true);
  EXPECT_EQ(out.status, SleepStatus::kOk) << "이미 잠들었으면 성공";
  EXPECT_TRUE(out.sleeping);
  EXPECT_FALSE(out.changed);
  out = room.Sleep(3, false);
  EXPECT_EQ(out.status, SleepStatus::kOk);
  EXPECT_FALSE(out.sleeping);
  EXPECT_TRUE(out.changed) << "깨어나기";
  EXPECT_FALSE(room.IsSleeping(3));
}

TEST(SleepTest, ClientsNotInTheRoomAreRejected) {
  Room room("111111", NoNodes());
  Fill(room, 3);
  EXPECT_EQ(room.Sleep(99, true).status, SleepStatus::kNotInRoom);
  EXPECT_EQ(room.Sleep(99, false).status, SleepStatus::kNotInRoom);
}

TEST(SleepTest, ARejectedSleepChangesNothingNotEvenTheDirection) {
  Room room("111111", NoNodes());
  Fill(room, 1);
  const std::uint32_t player = PlayerOf(room, 1);
  ASSERT_EQ(room.ApplyInput(1, 1, 1.0f, 0.0f), InputResult::kApplied);
  const SleepOutcome out = room.Sleep(1, true);  // 300 cm 떨어져 있다
  EXPECT_EQ(out.status, SleepStatus::kTooFar);
  EXPECT_FALSE(out.sleeping);
  EXPECT_FALSE(room.IsSleeping(1));
  EXPECT_EQ(room.world().Find(player)->dir_x, 1.0f) << "거부되면 계속 움직인다";
}

TEST(SleepTest, FallingAsleepStopsTheBodyAndWakingKeepsItsPosition) {
  Room room("111111", NoNodes());
  Fill(room, 3);
  const std::uint32_t player = PlayerOf(room, 3);  // (2400, 2600)
  ASSERT_EQ(room.ApplyInput(3, 1, 1.0f, 0.0f), InputResult::kApplied);  // 오른쪽으로 걷는 중
  Steps(room, 2);
  ASSERT_EQ(room.Sleep(3, true).status, SleepStatus::kOk);
  const PlayerBody asleep = *room.world().Find(player);
  EXPECT_EQ(asleep.dir_x, 0.0f);
  EXPECT_EQ(asleep.dir_y, 0.0f);
  Steps(room, 20);
  EXPECT_EQ(room.world().Find(player)->x, asleep.x) << "잠든 몸은 움직이지 않는다";
  ASSERT_EQ(room.Sleep(3, false).status, SleepStatus::kOk);
  EXPECT_EQ(room.world().Find(player)->x, asleep.x) << "깨어나도 위치는 그대로";
  EXPECT_EQ(room.world().Find(player)->y, asleep.y);
}

TEST(SleepTest, MoveInputsWhileAsleepAreIgnoredWholeIncludingTheSequenceNumber) {
  Room room("111111", NoNodes());
  Fill(room, 3);
  const std::uint32_t player = PlayerOf(room, 3);
  ASSERT_EQ(room.ApplyInput(3, 5, 0.0f, 0.0f), InputResult::kApplied);
  ASSERT_EQ(room.Sleep(3, true).status, SleepStatus::kOk);
  const PlayerBody before = *room.world().Find(player);
  EXPECT_EQ(room.ApplyInput(3, 6, -1.0f, 0.0f), InputResult::kSleeping);
  EXPECT_EQ(room.ApplyInput(3, 100, -1.0f, -1.0f), InputResult::kSleeping);
  Steps(room, 10);
  const PlayerBody after = *room.world().Find(player);
  EXPECT_EQ(after.x, before.x);
  EXPECT_EQ(after.y, before.y);
  EXPECT_EQ(after.last_input_seq, 5u) << "잠든 동안에는 입력 번호도 올리지 않는다";
  EXPECT_EQ(after.dir_x, 0.0f);

  ASSERT_EQ(room.Sleep(3, false).status, SleepStatus::kOk);
  EXPECT_EQ(room.ApplyInput(3, 6, -1.0f, 0.0f), InputResult::kApplied) << "깨어나면 다음 번호부터 받는다";
  EXPECT_EQ(room.ApplyInput(99, 7, 1.0f, 0.0f), InputResult::kUnknownPlayer);
}

TEST(SleepTest, LeavingDropsTheSleepAndARejoinerIsAwake) {
  Room room("111111", NoNodes());
  Fill(room, 3);
  ASSERT_EQ(room.Sleep(3, true).status, SleepStatus::kOk);
  room.RemoveMember(3);
  EXPECT_FALSE(room.IsSleeping(3));
  room.AddMember(3, "p3");
  EXPECT_FALSE(room.IsSleeping(3)) << "들어오는 사람은 깨어 있다";
  EXPECT_TRUE(room.SleepingPlayerIds().empty());
}

TEST(SleepTest, TheSleepChangeIsReportedOnlyWhenTheListActuallyChanges) {
  Room room("111111", NoNodes());
  Fill(room, 4);
  std::vector<std::uint32_t> ids;
  EXPECT_FALSE(room.TakeSleepChange(ids)) << "처음에는 아무도 자지 않는다";
  ASSERT_EQ(room.Sleep(4, true).status, SleepStatus::kOk);
  ASSERT_EQ(room.Sleep(3, true).status, SleepStatus::kOk);
  ASSERT_TRUE(room.TakeSleepChange(ids));
  EXPECT_EQ(ids, (std::vector<std::uint32_t>{PlayerOf(room, 3), PlayerOf(room, 4)})) << "입장한 순서";
  EXPECT_FALSE(room.TakeSleepChange(ids)) << "바뀌지 않았으면 다시 알리지 않는다";
  // 같은 틱 안에서 깼다가 다시 자면 목록이 같으므로 알리지 않는다.
  room.Sleep(3, false);
  room.Sleep(3, true);
  EXPECT_FALSE(room.TakeSleepChange(ids));
  room.Sleep(4, false);
  ASSERT_TRUE(room.TakeSleepChange(ids));
  EXPECT_EQ(ids, (std::vector<std::uint32_t>{PlayerOf(room, 3)}));
  room.RemoveMember(3);  // 잠든 채 나감
  ASSERT_TRUE(room.TakeSleepChange(ids));
  EXPECT_TRUE(ids.empty());
}

// ============================================================ 밤 정산이 일어나는 때

TEST(NightTriggerTest, EveryoneAsleepEndsTheDayButOneAwakeDoesNot) {
  Room room("111111", NoNodes());
  Fill(room, 4);
  room.Sleep(3, true);
  room.Sleep(4, true);
  EXPECT_EQ(room.ShouldEndDay(1), NightTrigger::kNone) << "둘이 깨어 있다";
  // 칸 0, 1도 시작 집 앞으로 걸어가서 잔다.
  room.ApplyInput(1, 1, 0.0f, 1.0f);
  room.ApplyInput(2, 1, 0.0f, 1.0f);
  Steps(room, 10);  // 180 cm 북쪽: y = 2580
  ASSERT_EQ(room.Sleep(1, true).status, SleepStatus::kOk);
  EXPECT_EQ(room.ShouldEndDay(room.world().tick()), NightTrigger::kNone) << "한 명이라도 깨어 있으면 하지 않는다";
  ASSERT_EQ(room.Sleep(2, true).status, SleepStatus::kOk);
  EXPECT_EQ(room.ShouldEndDay(room.world().tick()), NightTrigger::kAllAsleep);
}

TEST(NightTriggerTest, TheLastAwakePlayerLeavingEndsTheDayAndANewJoinerBlocksIt) {
  Room room("111111", NoNodes());
  Fill(room, 3);
  room.Sleep(3, true);
  EXPECT_EQ(room.ShouldEndDay(5), NightTrigger::kNone);
  room.RemoveMember(1);
  EXPECT_EQ(room.ShouldEndDay(5), NightTrigger::kNone);
  room.RemoveMember(2);
  EXPECT_EQ(room.ShouldEndDay(5), NightTrigger::kAllAsleep) << "깨어 있던 마지막 사람이 나갔다";
  room.AddMember(7, "late");
  EXPECT_EQ(room.ShouldEndDay(5), NightTrigger::kNone) << "새로 들어온 사람(깨어 있음)이 막는다";
}

TEST(NightTriggerTest, TheForcedNightHappensWhereverPeopleAreAndAllAsleepComesFirst) {
  Room room("111111", NoNodes());
  Fill(room, 1);
  room.ApplyInput(1, 1, -1.0f, -1.0f);
  Steps(room, 50);  // 시작 집에서 멀어졌다
  EXPECT_EQ(room.ShouldEndDay(12000), NightTrigger::kForced) << "위치와 상관없이 강제";

  Room asleep("222222", NoNodes());
  Fill(asleep, 3);
  asleep.RemoveMember(1);
  asleep.RemoveMember(2);
  asleep.Sleep(3, true);
  EXPECT_EQ(asleep.ShouldEndDay(12000), NightTrigger::kAllAsleep) << "모두 잠듦이 강제보다 먼저(forced = false)";
}

TEST(NightTriggerTest, AnEmptyRoomIsNeverAllAsleep) {
  Room room("111111", NoNodes());
  EXPECT_EQ(room.ShouldEndDay(5), NightTrigger::kNone);
}

// ============================================================ 밤 정산의 내용

// 노드와 집이 있고, 사람 셋이 흩어져 일부는 자는 방을 만든다(같은 조작이면 같은 상태).
void PrepareBusyDay(Room& room) {
  Fill(room, 3);
  room.GrantResources(1, 100, 100);
  ASSERT_EQ(room.Build(1, 0, 2600.0f, 2100.0f).status, BuildStatus::kOk);
  ASSERT_EQ(room.Build(1, 0, 2000.0f, 2100.0f).status, BuildStatus::kOk);
  room.ApplyInput(1, 7, -1.0f, -0.5f);
  room.ApplyInput(2, 3, 1.0f, -0.25f);
  Steps(room, 40);
  room.Sleep(3, true);
  // 노드 여러 개를 고갈시킨다(거리 판정을 피하려고 노드 쪽 Hit를 직접 부른다).
  for (std::uint32_t id = 1; id <= 12; ++id) {
    while (room.resources().Find(id)->hp > 0) room.resources().Hit(id);
  }
  room.resources().TakeChanges();
  room.structures().TakeChanges();
  std::vector<std::uint32_t> ids;
  room.TakeSleepChange(ids);  // 틱 끝의 알림처럼 "잠든 사람 [3]"은 이미 나갔다고 친다
}

TEST(NightTest, TheNightCompletesBuildsRespawnsWakesMovesAndTurnsTheDay) {
  const PlayConfig config;
  Room room("111111", config);
  PrepareBusyDay(room);
  const std::uint32_t seq1 = room.world().Find(PlayerOf(room, 1))->last_input_seq;
  const std::uint64_t tick = room.world().tick();

  const NightReport report = room.RunNight(tick, false);
  EXPECT_EQ(report.completed, 2u);
  EXPECT_EQ(report.respawned, 12u);
  EXPECT_EQ(report.day, 2u);
  EXPECT_FALSE(report.forced);

  for (const Structure& structure : room.structures().structures()) {
    EXPECT_EQ(structure.state, StructureState::kComplete) << structure.id;
    EXPECT_EQ(structure.ready_tick, tick);
  }
  for (const ResourceNode& node : room.resources().nodes()) {
    EXPECT_EQ(node.hp, node.max_hp) << "고갈된 모든 노드가 체력을 가득 채워 돌아온다 " << node.id;
  }
  const float expected[][2] = {{2400.0f, 2400.0f}, {2600.0f, 2400.0f}, {2400.0f, 2600.0f}};
  for (std::uint64_t id = 1; id <= 3; ++id) {
    EXPECT_FALSE(room.IsSleeping(id)) << "모두 깨어난다 " << id;
    const PlayerBody* body = room.world().Find(PlayerOf(room, id));
    EXPECT_EQ(body->x, expected[id - 1][0]) << "자기 칸의 시작 위치 " << id;
    EXPECT_EQ(body->y, expected[id - 1][1]);
    EXPECT_EQ(body->dir_x, 0.0f);
    EXPECT_EQ(body->dir_y, 0.0f);
  }
  EXPECT_EQ(room.world().Find(PlayerOf(room, 1))->last_input_seq, seq1) << "입력 번호는 그대로";
  EXPECT_EQ(room.day(), 2u);
  EXPECT_EQ(room.day_start_tick(), static_cast<std::int64_t>(tick)) << "새 날은 그 틱에 시작";
  EXPECT_EQ(room.DayElapsed(), 0u);
  EXPECT_EQ(room.GameMinute(), 360u);
  EXPECT_EQ(room.ShouldEndDay(tick), NightTrigger::kNone);
  EXPECT_EQ(room.ShouldEndDay(tick + 11999), NightTrigger::kNone);
  EXPECT_EQ(room.ShouldEndDay(tick + 12000), NightTrigger::kForced) << "다음 하루도 12,000틱";

  // 바뀐 노드와 구조물이 기록되어 알림으로 나간다.
  EXPECT_EQ(room.resources().TakeChanges().size(), 12u);
  EXPECT_EQ(room.structures().TakeChanges().size(), 2u);
  std::vector<std::uint32_t> ids;
  EXPECT_TRUE(room.TakeSleepChange(ids)) << "잠든 사람이 있었으므로 목록이 바뀌었다";
  EXPECT_TRUE(ids.empty());
}

TEST(NightTest, AfterTheNightAWokenPlayerTakesTheNextInputAndTheOldSequenceIsStale) {
  Room room("111111", NoNodes());
  Fill(room, 3);
  room.ApplyInput(3, 5, 0.0f, 0.0f);
  room.Sleep(3, true);
  room.RunNight(10, false);
  EXPECT_EQ(room.ApplyInput(3, 5, 1.0f, 0.0f), InputResult::kStale);
  EXPECT_EQ(room.ApplyInput(3, 6, 1.0f, 0.0f), InputResult::kApplied);
}

TEST(NightTest, AForcedNightReportsForcedAndStillWakesEveryone) {
  Room room("111111", NoNodes());
  Fill(room, 2);
  room.ApplyInput(1, 1, -1.0f, 0.0f);
  Steps(room, 30);
  const NightReport report = room.RunNight(12000, true);
  EXPECT_TRUE(report.forced);
  EXPECT_EQ(report.day, 2u);
  EXPECT_FALSE(room.IsSleeping(1));
  EXPECT_FALSE(room.IsSleeping(2));
  EXPECT_EQ(room.world().Find(PlayerOf(room, 1))->x, 2400.0f) << "깨어 있던 사람도 아침 위치로";
  std::vector<std::uint32_t> ids;
  EXPECT_FALSE(room.TakeSleepChange(ids)) << "잠든 사람이 없던 강제 정산은 목록이 그대로다";
}

TEST(NightTest, TheSameStateAlwaysGivesTheSameNight) {
  const PlayConfig config;
  Room a("111111", config);
  Room b("222222", config);
  PrepareBusyDay(a);
  PrepareBusyDay(b);
  a.RunNight(a.world().tick(), false);
  b.RunNight(b.world().tick(), false);
  ASSERT_EQ(a.resources().nodes().size(), b.resources().nodes().size());
  for (std::size_t i = 0; i < a.resources().nodes().size(); ++i) {
    EXPECT_EQ(a.resources().nodes()[i].x, b.resources().nodes()[i].x) << i;
    EXPECT_EQ(a.resources().nodes()[i].y, b.resources().nodes()[i].y) << i;
    EXPECT_EQ(a.resources().nodes()[i].hp, b.resources().nodes()[i].hp) << i;
  }
  for (std::size_t i = 0; i < a.world().players().size(); ++i) {
    EXPECT_EQ(a.world().players()[i].x, b.world().players()[i].x);
    EXPECT_EQ(a.world().players()[i].y, b.world().players()[i].y);
  }
  EXPECT_EQ(a.day(), b.day());
  EXPECT_EQ(a.day_start_tick(), b.day_start_tick());
}

TEST(NightTest, ANodeWithNoFreeSpotAtNightIsRetriedEveryTwentyTicksDuringTheDay) {
  // 작은 맵(600 x 600, 노드 자리 [200, 400])에 몸이 큰 사람 하나가 가운데 서면 모든 자리를 막는다.
  PlayConfig config;
  config.map_width_cm = 600.0f;
  config.map_height_cm = 600.0f;
  config.tree_count = 1;
  config.rock_count = 0;
  config.tree_hp = 1;
  config.spawn_clear_radius_cm = 0.0f;
  config.build_zone_half_size_cm = 0.0f;
  config.farmhouse_width_cm = 0.0f;
  config.player_radius_cm = 300.0f;  // 막는 반지름 360 > 자리 정사각형의 대각선 반(141)
  Room room("111111", config);
  ASSERT_EQ(room.resources().nodes().size(), 1u);
  room.resources().Hit(1);  // 먼저 고갈시켜야 사람이 가운데(300, 300)에 설 수 있다
  Fill(room, 1);
  ASSERT_EQ(room.world().Find(PlayerOf(room, 1))->x, 300.0f);

  const NightReport report = room.RunNight(100, false);
  EXPECT_EQ(report.respawned, 0u) << "자리가 없으면 고갈 상태로 남는다";
  EXPECT_EQ(room.resources().Find(1)->hp, 0u);
  room.RemoveMember(1);  // 막던 사람이 사라졌다
  EXPECT_EQ(room.AdvanceResources(119), 0u) << "재시도 시각(20틱 뒤) 전에는 찾지 않는다";
  EXPECT_EQ(room.AdvanceResources(120), 1u) << "정확히 20틱 뒤에 다시 찾아 되살린다";
  EXPECT_EQ(room.resources().Find(1)->hp, 1u);
}

TEST(NightTest, NightsInOneRoomDoNotTouchAnotherRoom) {
  const PlayConfig config;
  Room a("111111", config);
  Room b("222222", config);
  PrepareBusyDay(a);
  PrepareBusyDay(b);
  a.RunNight(a.world().tick(), false);
  EXPECT_EQ(b.day(), 1u);
  EXPECT_EQ(b.structures().Find(1)->state, StructureState::kBuilding);
  EXPECT_EQ(b.resources().Find(1)->hp, 0u);
  EXPECT_TRUE(b.IsSleeping(3));
}

TEST(NightTest, ARestoredHouseOnTheFarmhouseSpotDoesNotBreakAnything) {
  // 이 조각 전에 저장된 농장에는 시작 집 자리에 집이 있을 수 있다. 복원은 그대로 하고 죽지 않아야 한다.
  Room room("111111", PlayConfig{});
  ASSERT_TRUE(room.RestoreStructure(1, 0, 2500.0f, 2850.0f, true));
  ASSERT_TRUE(room.RestoreStructure(2, 0, 2500.0f, 2600.0f, false));  // 칸 2, 3 자리를 덮는 건설 중인 집
  Fill(room, 4);
  for (const auto& body : room.world().players()) {
    EXPECT_TRUE(room.world().IsFree(body.x, body.y)) << "갇히지 않는 빈 자리에 선다 (" << body.x << ", " << body.y << ")";
  }
  room.ApplyInput(3, 1, 0.0f, 1.0f);
  Steps(room, 20);
  room.Sleep(1, true);
  const NightReport report = room.RunNight(room.world().tick(), true);
  EXPECT_EQ(report.completed, 1u);
  for (const auto& body : room.world().players()) {
    EXPECT_TRUE(room.world().IsFree(body.x, body.y));
  }
}

}  // namespace
}  // namespace game::play
