// 집 짓기의 순수 로직 테스트: StructureSet(건설 진행)과 Room::Build(거리, 겹침, 비용, 판정 순서).
// 소켓도 시계도 없다. 시간은 월드 틱으로만 흐른다. docs/content-roadmap.md 완료 기준 1~3, 5~6.

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "play/room.h"
#include "play/structures.h"

namespace game::play {
namespace {

constexpr float kDt = 0.05f;

// 노드가 없고 허용 거리가 맵 전체인 설정(판정 규칙을 하나씩 따로 시험하기 위해)
PlayConfig OpenField() {
  PlayConfig config;
  config.tree_count = 0;
  config.rock_count = 0;
  config.build_range_cm = 100000.0f;
  return config;
}

void Tick(Room& room, std::uint32_t ticks) {
  for (std::uint32_t i = 0; i < ticks; ++i) {
    room.world().Step(kDt);
    room.structures().Advance(room.world().tick());
  }
}

// 방에 alice(client 1)를 넣고 짓기에 넉넉한 자원을 준다. 시작 위치는 (2400, 2400).
void Prepare(Room& room, std::uint32_t wood = 100, std::uint32_t stone = 100) {
  room.AddMember(1, "alice");
  room.GrantResources(1, wood, stone);
}

// ============================================================ StructureSet

TEST(StructureSetTest, NumbersFromOneStartsBuildingAndCompletesAtTheExactTick) {
  PlayConfig config;
  StructureSet set(config);
  EXPECT_EQ(set.Add(StructureType::kHouse, 1000.0f, 1000.0f, 50), 1u);
  EXPECT_EQ(set.Add(StructureType::kHouse, 2000.0f, 2000.0f, 60), 2u);

  const Structure* first = set.Find(1);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->state, StructureState::kBuilding);
  EXPECT_EQ(first->ready_tick, 50u + config.build_ticks);
  EXPECT_EQ(first->width, config.house_width_cm);
  EXPECT_EQ(first->height, config.house_height_cm);

  EXPECT_EQ(set.Advance(149), 0u) << "99틱 뒤에는 아직";
  EXPECT_EQ(set.Find(1)->state, StructureState::kBuilding);
  EXPECT_EQ(set.Advance(150), 1u) << "100틱 뒤에 첫 번째 완료";
  EXPECT_EQ(set.Find(1)->state, StructureState::kComplete);
  EXPECT_EQ(set.Find(2)->state, StructureState::kBuilding);
  EXPECT_EQ(set.Advance(160), 1u);
  EXPECT_EQ(set.Advance(1000), 0u) << "완료한 것을 다시 세지 않는다";
  EXPECT_EQ(set.Find(0), nullptr);
  EXPECT_EQ(set.Find(3), nullptr);
}

TEST(StructureSetTest, ChangesAreCollectedOnceSortedAndClearedWithTheLatestState) {
  PlayConfig config;
  config.build_ticks = 10;
  StructureSet set(config);
  set.Add(StructureType::kHouse, 1000.0f, 1000.0f, 0);
  set.Add(StructureType::kHouse, 2000.0f, 2000.0f, 0);
  EXPECT_TRUE(set.HasChanges());
  set.Advance(10);  // 같은 틱 안에서 "생김"과 "완료"가 겹쳐도 한 번만, 마지막 상태로

  const auto changes = set.TakeChanges();
  ASSERT_EQ(changes.size(), 2u);
  EXPECT_EQ(changes[0].id, 1u);
  EXPECT_EQ(changes[1].id, 2u);
  EXPECT_EQ(changes[0].state, StructureState::kComplete);
  EXPECT_FALSE(set.HasChanges());
  EXPECT_TRUE(set.TakeChanges().empty());
}

TEST(StructureSetTest, CompletionIsReportedAsAChange) {
  PlayConfig config;
  config.build_ticks = 5;
  StructureSet set(config);
  set.Add(StructureType::kHouse, 1000.0f, 1000.0f, 0);
  set.TakeChanges();  // 생김 알림은 이미 나갔다고 치고 비운다
  EXPECT_EQ(set.Advance(5), 1u);
  const auto changes = set.TakeChanges();
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].state, StructureState::kComplete);
}

// ============================================================ Room::Build

TEST(RoomBuildTest, ClientsNotInTheRoomAreRejected) {
  Room room("111111", OpenField());
  Prepare(room);
  EXPECT_EQ(room.Build(99, 0, 2600.0f, 2400.0f).status, BuildStatus::kNotInRoom);
  room.RemoveMember(1);
  EXPECT_EQ(room.Build(1, 0, 2600.0f, 2400.0f).status, BuildStatus::kNotInRoom);
}

TEST(RoomBuildTest, BuildingSpendsTheExactCostAndCreatesABuildingStructureOnTheGrid) {
  Room room("111111", OpenField());
  Prepare(room, 25, 12);
  Tick(room, 7);  // 건설 시각을 0이 아닌 틱에서 시험한다
  const BuildOutcome out = room.Build(1, 0, 2649.0f, 2351.0f);  // 격자(100 cm) 중심 (2600, 2400)으로 맞춰진다
  ASSERT_EQ(out.status, BuildStatus::kOk);
  EXPECT_EQ(out.structure_id, 1u);
  EXPECT_EQ(out.wood, 5u);
  EXPECT_EQ(out.stone, 2u);
  EXPECT_EQ(room.FindByClient(1)->wood, 5u);

  const Structure* built = room.structures().Find(1);
  ASSERT_NE(built, nullptr);
  EXPECT_EQ(built->x, 2600.0f);
  EXPECT_EQ(built->y, 2400.0f);
  EXPECT_EQ(built->state, StructureState::kBuilding);
  EXPECT_EQ(built->ready_tick, 7u + 100u) << "건설한 틱(7) + build_ticks";
}

TEST(RoomBuildTest, TheBuildingCompletesAfterExactlyTheConfiguredTicks) {
  Room room("111111", OpenField());
  Prepare(room);
  ASSERT_EQ(room.Build(1, 0, 2600.0f, 2400.0f).status, BuildStatus::kOk);
  Tick(room, 99);
  EXPECT_EQ(room.structures().Find(1)->state, StructureState::kBuilding);
  Tick(room, 1);
  EXPECT_EQ(room.structures().Find(1)->state, StructureState::kComplete);
}

TEST(RoomBuildTest, OneUnitShortOfTheCostIsRejectedAndTheExactCostIsAccepted) {
  struct Case {
    std::uint32_t wood;
    std::uint32_t stone;
    bool ok;
  };
  for (const Case c : {Case{19, 10, false}, Case{20, 9, false}, Case{0, 0, false}, Case{20, 10, true}, Case{21, 11, true}}) {
    Room room("111111", OpenField());
    Prepare(room, c.wood, c.stone);
    const BuildOutcome out = room.Build(1, 0, 2600.0f, 2400.0f);
    EXPECT_EQ(out.status == BuildStatus::kOk, c.ok) << c.wood << "/" << c.stone;
    if (!c.ok) {
      EXPECT_EQ(out.status, BuildStatus::kNotEnoughResources);
      EXPECT_EQ(room.structures().size(), 0u) << "거부는 구조물을 만들지 않는다";
      EXPECT_EQ(room.FindByClient(1)->wood, c.wood);
      EXPECT_EQ(room.FindByClient(1)->stone, c.stone);
      EXPECT_EQ(out.wood, c.wood) << "거부 응답도 현재 인벤토리를 담는다";
    }
  }
}

TEST(RoomBuildTest, ReachIsInclusiveAtTheExactRange) {
  PlayConfig config = OpenField();
  config.build_range_cm = 500.0f;
  Room room("111111", config);
  Prepare(room);
  // 시작 위치 (2400, 2400)에서 (2900, 2400)은 정확히 500 cm
  EXPECT_EQ(room.Build(1, 0, 2900.0f, 2400.0f).status, BuildStatus::kOk) << "허용 거리와 같으면 허용";

  Room far("222222", config);
  Prepare(far);
  EXPECT_EQ(far.Build(1, 0, 2900.0f, 2500.0f).status, BuildStatus::kTooFar) << "격자 한 칸 더 멀면 거부(약 509.9 cm)";
  EXPECT_EQ(far.Build(1, 0, 2900.0f, 2300.0f).status, BuildStatus::kTooFar);
}

TEST(RoomBuildTest, TheDistanceIsMeasuredToTheSnappedCenter) {
  PlayConfig config = OpenField();
  config.build_range_cm = 500.0f;
  Room room("111111", config);
  Prepare(room);
  // 요청은 2949(거리 549)지만 격자로 맞추면 2900(거리 500)이라 허용된다
  EXPECT_EQ(room.Build(1, 0, 2949.0f, 2400.0f).status, BuildStatus::kOk);
  // 요청은 2951(거리 551)이고 맞추면 3000(거리 600)이라 거부
  Room other("222222", config);
  Prepare(other);
  EXPECT_EQ(other.Build(1, 0, 2951.0f, 2400.0f).status, BuildStatus::kTooFar);
}

TEST(RoomBuildTest, TheMapEdgeIsInclusiveAndOutsideIsBlocked) {
  PlayConfig config = OpenField();
  config.build_grid_cm = 50.0f;  // 중심이 150의 배수일 수 있게
  Room room("111111", config);
  Prepare(room);
  // 중심 (150, 150), 크기 300이면 0~300으로 맵 모서리에 정확히 닿는다
  EXPECT_EQ(room.Build(1, 0, 150.0f, 150.0f).status, BuildStatus::kOk) << "맵 경계에 닿는 것은 허용";

  Room outside("222222", config);
  Prepare(outside);
  EXPECT_EQ(outside.Build(1, 0, 100.0f, 150.0f).status, BuildStatus::kBlocked) << "한 칸만 나가도 거부";
  EXPECT_EQ(outside.Build(1, 0, 150.0f, 100.0f).status, BuildStatus::kBlocked);
  EXPECT_EQ(outside.Build(1, 0, 4900.0f, 2500.0f).status, BuildStatus::kBlocked) << "오른쪽 끝 밖";
  EXPECT_EQ(outside.Build(1, 0, 4850.0f, 2500.0f).status, BuildStatus::kOk) << "오른쪽 끝에 닿음(4700~5000)";
}

TEST(RoomBuildTest, StructuresMayTouchButNotOverlap) {
  Room room("111111", OpenField());
  Prepare(room);
  ASSERT_EQ(room.Build(1, 0, 2600.0f, 2400.0f).status, BuildStatus::kOk);  // 2450~2750
  EXPECT_EQ(room.Build(1, 0, 2800.0f, 2400.0f).status, BuildStatus::kBlocked) << "50 cm 겹침";
  EXPECT_EQ(room.Build(1, 0, 2600.0f, 2400.0f).status, BuildStatus::kBlocked) << "같은 자리";
  EXPECT_EQ(room.Build(1, 0, 2600.0f, 2700.0f).status, BuildStatus::kOk) << "위아래로 면이 닿기만 함(2550~2850 아래 2250~2550)";
}

TEST(RoomBuildTest, AStructureCannotBeBuiltOnTopOfAnyPlayerIncludingTheBuilder) {
  Room room("111111", OpenField());
  Prepare(room);
  room.AddMember(2, "bob");  // 시작 위치 (2600, 2400)
  room.GrantResources(2, 100, 100);
  EXPECT_EQ(room.Build(1, 0, 2400.0f, 2400.0f).status, BuildStatus::kBlocked) << "내 몸 위";
  EXPECT_EQ(room.Build(1, 0, 2600.0f, 2400.0f).status, BuildStatus::kBlocked) << "다른 사람의 몸 위";
  EXPECT_EQ(room.structures().size(), 0u);
  EXPECT_EQ(room.FindByClient(1)->wood, 100u) << "막히면 비용을 받지 않는다";
}

TEST(RoomBuildTest, AFootprintTouchingAPlayersBodyIsAllowedButOneCentimeterCloserIsNot) {
  PlayConfig config = OpenField();
  config.build_grid_cm = 1.0f;  // 1 cm 단위로 위치를 고를 수 있게
  // 시작 위치 (2400, 2400), 몸 반지름 40: 집(300 cm) 왼쪽 면이 x = 2440이면 정확히 닿는다(중심 2590)
  Room touching("111111", config);
  Prepare(touching);
  EXPECT_EQ(touching.Build(1, 0, 2590.0f, 2400.0f).status, BuildStatus::kOk) << "몸에 정확히 닿기만 함";

  Room overlapping("222222", config);
  Prepare(overlapping);
  EXPECT_EQ(overlapping.Build(1, 0, 2589.0f, 2400.0f).status, BuildStatus::kBlocked) << "1 cm 겹침";
}

TEST(RoomBuildTest, LiveNodesBlockBuildingButDepletedNodesDoNot) {
  PlayConfig config = OpenField();
  config.tree_count = 1;
  config.tree_hp = 1;
  Room room("111111", config);
  Prepare(room);
  const ResourceNode node = room.resources().nodes()[0];
  const float grid = config.build_grid_cm;
  const float x = std::round(node.x / grid) * grid;
  const float y = std::round(node.y / grid) * grid;

  EXPECT_EQ(room.Build(1, 0, x, y).status, BuildStatus::kBlocked) << "살아 있는 노드 위";
  room.resources().Hit(node.id, room.world().tick());  // 체력 1이라 고갈
  EXPECT_EQ(room.Build(1, 0, x, y).status, BuildStatus::kOk) << "고갈된 노드는 막지 않는다";
}

TEST(RoomBuildTest, TheRejectionOrderIsRoomTypeLimitReachThenOverlapThenCost) {
  PlayConfig config = OpenField();
  config.build_range_cm = 500.0f;
  config.max_structures = 1;
  Room room("111111", config);
  Prepare(room, 0, 0);  // 자원도 없다

  // 종류가 틀리면 다른 모든 문제보다 먼저(멀고 자원 없음에도)
  EXPECT_EQ(room.Build(1, 7, 4000.0f, 4000.0f).status, BuildStatus::kInvalidType);

  // 빈 방에서는 상한이 없으므로 먼저 하나 지어 상한을 채운다(자원이 있어야 한다)
  room.GrantResources(1, 20, 10);
  ASSERT_EQ(room.Build(1, 0, 2600.0f, 2400.0f).status, BuildStatus::kOk);
  // 상한은 멀리 있음/겹침/자원 부족보다 먼저
  EXPECT_EQ(room.Build(1, 0, 4000.0f, 4000.0f).status, BuildStatus::kLimit);
  EXPECT_EQ(room.Build(1, 0, 2600.0f, 2400.0f).status, BuildStatus::kLimit);

  // 상한이 넉넉한 방에서: 멀리 있음은 겹침/맵 밖/자원 부족보다 먼저
  config.max_structures = 20;
  Room other("222222", config);
  Prepare(other, 0, 0);
  EXPECT_EQ(other.Build(1, 0, 999999.0f, 0.0f).status, BuildStatus::kTooFar) << "멀리 있음이 맵 밖보다 먼저";
  EXPECT_EQ(other.Build(1, 0, 2400.0f, 2400.0f).status, BuildStatus::kBlocked) << "겹침이 자원 부족보다 먼저";
  EXPECT_EQ(other.Build(1, 0, 2600.0f, 2400.0f).status, BuildStatus::kNotEnoughResources);
}

TEST(RoomBuildTest, InvalidCoordinatesAreBlockedWithoutBreakingAnything) {
  Room room("111111", OpenField());
  Prepare(room);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  EXPECT_EQ(room.Build(1, 0, nan, 2400.0f).status, BuildStatus::kBlocked);
  EXPECT_EQ(room.Build(1, 0, 2600.0f, inf).status, BuildStatus::kBlocked);
  EXPECT_EQ(room.Build(1, 0, -inf, 0.0f).status, BuildStatus::kBlocked);
  EXPECT_EQ(room.Build(1, 0, 3.0e38f, 0.0f).status, BuildStatus::kBlocked);
  EXPECT_EQ(room.Build(1, 0, 1.0e8f, 1.0e8f).status, BuildStatus::kTooFar) << "유한하고 큰 값은 거리로 거부";
  EXPECT_EQ(room.structures().size(), 0u);
  EXPECT_EQ(room.FindByClient(1)->wood, 100u);
}

TEST(RoomBuildTest, WhenTwoPlayersBuildOnTheSameSpotInTheSameTickExactlyOneSucceeds) {
  Room room("111111", OpenField());
  Prepare(room);
  room.AddMember(2, "bob");
  room.GrantResources(2, 100, 100);
  const BuildOutcome first = room.Build(1, 0, 2500.0f, 2800.0f);
  const BuildOutcome second = room.Build(2, 0, 2500.0f, 2800.0f);
  EXPECT_EQ(first.status, BuildStatus::kOk);
  EXPECT_EQ(second.status, BuildStatus::kBlocked);
  EXPECT_EQ(room.structures().size(), 1u);
  EXPECT_EQ(room.FindByClient(2)->wood, 100u) << "진 사람은 비용을 내지 않는다";
  EXPECT_EQ(room.FindByClient(1)->wood, 80u);
}

TEST(RoomBuildTest, TheStructureLimitIsEnforced) {
  PlayConfig config = OpenField();
  config.max_structures = 3;
  Room room("111111", config);
  Prepare(room, 1000, 1000);
  ASSERT_EQ(room.Build(1, 0, 2900.0f, 2400.0f).status, BuildStatus::kOk);
  ASSERT_EQ(room.Build(1, 0, 3200.0f, 2400.0f).status, BuildStatus::kOk);
  ASSERT_EQ(room.Build(1, 0, 3500.0f, 2400.0f).status, BuildStatus::kOk);
  EXPECT_EQ(room.Build(1, 0, 3800.0f, 2400.0f).status, BuildStatus::kLimit);
}

TEST(RoomBuildTest, RoomsDoNotShareStructuresOrInventories) {
  Room a("111111", OpenField());
  Room b("222222", OpenField());
  Prepare(a);
  Prepare(b);
  ASSERT_EQ(a.Build(1, 0, 2600.0f, 2400.0f).status, BuildStatus::kOk);
  EXPECT_EQ(b.structures().size(), 0u);
  EXPECT_EQ(b.FindByClient(1)->wood, 100u);
  EXPECT_EQ(b.Build(1, 0, 2600.0f, 2400.0f).status, BuildStatus::kOk) << "다른 방의 같은 자리는 비어 있다";
}

TEST(RoomBuildTest, TheInventoryIsLostWhenLeavingAndBuildingKeepsStandingForTheOthers) {
  Room room("111111", OpenField());
  Prepare(room);
  room.AddMember(2, "bob");
  ASSERT_EQ(room.Build(1, 0, 2600.0f, 2900.0f).status, BuildStatus::kOk);
  room.RemoveMember(1);
  EXPECT_EQ(room.structures().size(), 1u) << "지은 사람이 나가도 건물은 남는다";
  EXPECT_EQ(room.Build(1, 0, 2600.0f, 2900.0f).status, BuildStatus::kNotInRoom);
}

}  // namespace
}  // namespace game::play
