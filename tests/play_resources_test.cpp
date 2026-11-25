// 자원 채집의 순수 로직 테스트: ResourceField(배치, 체력, 고갈, 재생)와 Room::Gather(거리, 쿨다운, 인벤토리).
// 소켓도 시계도 없다. 시간은 월드 틱(Step 호출 횟수)으로만 흐른다. docs/stage4-gather.md 완료 기준 1~8.
// 재생은 틱이 아니라 밤 정산(RespawnAllDepleted)에 일어난다(docs/content-day.md 5.4). 낮 동안의 Advance는 재시도 전용이다.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "play/resources.h"
#include "play/room.h"

namespace game::play {
namespace {

constexpr float kDt = 0.05f;

// 거리 판정에 걸리지 않도록 허용 거리를 맵 전체보다 크게 한 설정.
PlayConfig WideRange() {
  PlayConfig config;
  config.gather_range_cm = 100000.0f;
  return config;
}

void Advance(Room& room, std::uint32_t ticks) {
  for (std::uint32_t i = 0; i < ticks; ++i) {
    room.world().Step(kDt);
    room.resources().Advance(room.world().tick());
  }
}

// 플레이어 위치에서 노드까지의 거리(cm).
double DistanceTo(const Room& room, std::uint32_t player_id, const ResourceNode& node) {
  const PlayerBody* body = room.world().Find(player_id);
  return std::hypot(static_cast<double>(body->x) - node.x, static_cast<double>(body->y) - node.y);
}

// ============================================================ 배치

TEST(ResourceFieldTest, DefaultLayoutHasTheConfiguredNodes) {
  const PlayConfig config;
  const ResourceField field(config);
  ASSERT_EQ(field.nodes().size(), config.tree_count + config.rock_count);
  std::uint32_t trees = 0;
  std::uint32_t rocks = 0;
  for (std::size_t i = 0; i < field.nodes().size(); ++i) {
    const ResourceNode& node = field.nodes()[i];
    EXPECT_EQ(node.id, i + 1) << "번호는 1부터 차례로";
    if (node.kind == ResourceKind::kTree) {
      ++trees;
      EXPECT_EQ(node.hp, config.tree_hp);
      EXPECT_EQ(node.max_hp, config.tree_hp);
    } else {
      ++rocks;
      EXPECT_EQ(node.hp, config.rock_hp);
      EXPECT_EQ(node.max_hp, config.rock_hp);
    }
  }
  EXPECT_EQ(trees, config.tree_count);
  EXPECT_EQ(rocks, config.rock_count);
}

TEST(ResourceFieldTest, SameConfigGivesTheSameLayoutAndADifferentSeedDoesNot) {
  const PlayConfig config;
  const ResourceField a(config);
  const ResourceField b(config);
  ASSERT_EQ(a.nodes().size(), b.nodes().size());
  for (std::size_t i = 0; i < a.nodes().size(); ++i) {
    EXPECT_EQ(a.nodes()[i].x, b.nodes()[i].x) << i;
    EXPECT_EQ(a.nodes()[i].y, b.nodes()[i].y) << i;
    EXPECT_EQ(a.nodes()[i].kind, b.nodes()[i].kind) << i;
  }

  PlayConfig other = config;
  other.layout_seed = config.layout_seed + 1;
  const ResourceField c(other);
  bool differs = false;
  for (std::size_t i = 0; i < std::min(a.nodes().size(), c.nodes().size()); ++i) {
    differs = differs || a.nodes()[i].x != c.nodes()[i].x || a.nodes()[i].y != c.nodes()[i].y;
  }
  EXPECT_TRUE(differs);
}

TEST(ResourceFieldTest, NodesStayInsideTheMapKeepApartAndLeaveTheSpawnAreaEmpty) {
  const PlayConfig config;
  const ResourceField field(config);
  const double center_x = config.map_width_cm * 0.5;
  const double center_y = config.map_height_cm * 0.5;
  for (const ResourceNode& node : field.nodes()) {
    EXPECT_GE(node.x, config.node_edge_margin_cm);
    EXPECT_LE(node.x, config.map_width_cm - config.node_edge_margin_cm);
    EXPECT_GE(node.y, config.node_edge_margin_cm);
    EXPECT_LE(node.y, config.map_height_cm - config.node_edge_margin_cm);
    EXPECT_GE(std::hypot(node.x - center_x, node.y - center_y), config.spawn_clear_radius_cm) << node.id;
  }
  for (std::size_t i = 0; i < field.nodes().size(); ++i) {
    for (std::size_t j = i + 1; j < field.nodes().size(); ++j) {
      const ResourceNode& a = field.nodes()[i];
      const ResourceNode& b = field.nodes()[j];
      EXPECT_GE(std::hypot(static_cast<double>(a.x) - b.x, static_cast<double>(a.y) - b.y), config.node_min_gap_cm)
          << a.id << " " << b.id;
    }
  }
}

TEST(ResourceFieldTest, NoNodesWhenCountsOrHealthAreZero) {
  PlayConfig config;
  config.tree_count = 0;
  config.rock_count = 0;
  EXPECT_TRUE(ResourceField(config).nodes().empty());

  PlayConfig no_health;
  no_health.tree_hp = 0;
  no_health.rock_hp = 0;
  EXPECT_TRUE(ResourceField(no_health).nodes().empty());

  PlayConfig tiny;  // 가장자리 여백이 맵보다 커서 둘 곳이 없다
  tiny.map_width_cm = 300.0f;
  tiny.map_height_cm = 300.0f;
  EXPECT_TRUE(ResourceField(tiny).nodes().empty());
}

// ============================================================ 판정(Check)

TEST(ResourceFieldTest, CheckSeparatesMissingDepletedTooFarAndOk) {
  PlayConfig config;
  config.tree_hp = 1;
  ResourceField field(config);
  const ResourceNode node = field.nodes()[0];

  EXPECT_EQ(field.Check(0, node.x, node.y), NodeCheck::kNotFound);
  EXPECT_EQ(field.Check(static_cast<std::uint32_t>(field.nodes().size()) + 1, node.x, node.y), NodeCheck::kNotFound);
  EXPECT_EQ(field.Check(std::numeric_limits<std::uint32_t>::max(), node.x, node.y), NodeCheck::kNotFound);
  EXPECT_EQ(field.Check(node.id, node.x, node.y), NodeCheck::kOk);
  EXPECT_EQ(field.Check(node.id, node.x + config.gather_range_cm + 50.0f, node.y), NodeCheck::kTooFar);

  field.Hit(node.id);  // 체력 1이라 고갈된다
  EXPECT_EQ(field.Check(node.id, node.x, node.y), NodeCheck::kDepleted);
  // 고갈과 너무 멂이 겹치면 고갈이 먼저다(문서의 판정 순서)
  EXPECT_EQ(field.Check(node.id, node.x + 5000.0f, node.y), NodeCheck::kDepleted);
}

TEST(ResourceFieldTest, ReachIsInclusiveAtTheExactRange) {
  const PlayConfig base;
  const ResourceField probe(base);
  const ResourceNode node = probe.nodes()[0];

  // 노드에서 x축으로 정확히 같은 거리만큼 떨어진 점. 두 float의 차이는 double로 정확히 구해진다.
  const float query_x = node.x + 200.0f;
  const double exact = static_cast<double>(query_x) - static_cast<double>(node.x);

  PlayConfig at_range = base;
  at_range.gather_range_cm = static_cast<float>(exact);
  ASSERT_EQ(static_cast<double>(at_range.gather_range_cm), exact) << "경계값이 float로 정확히 표현되어야 한다";
  EXPECT_EQ(ResourceField(at_range).Check(node.id, query_x, node.y), NodeCheck::kOk) << "허용 거리와 같으면 허용";

  PlayConfig just_short = base;
  just_short.gather_range_cm = std::nextafter(static_cast<float>(exact), 0.0f);
  EXPECT_EQ(ResourceField(just_short).Check(node.id, query_x, node.y), NodeCheck::kTooFar) << "조금이라도 멀면 거부";
}

TEST(ResourceFieldTest, ReachUsesBothAxes) {
  const PlayConfig config;
  const ResourceField field(config);
  const ResourceNode node = field.nodes()[0];
  const float r = config.gather_range_cm;

  // 한 축으로만 멀어도, 두 축이 합쳐서 멀어도 거리로 센다.
  EXPECT_EQ(field.Check(node.id, node.x, node.y + 1.5f * r), NodeCheck::kTooFar) << "y축으로만 멂";
  EXPECT_EQ(field.Check(node.id, node.x + 1.5f * r, node.y), NodeCheck::kTooFar) << "x축으로만 멂";
  EXPECT_EQ(field.Check(node.id, node.x + 0.8f * r, node.y + 0.8f * r), NodeCheck::kTooFar)
      << "각 축은 허용 거리 안이지만 대각선 거리는 밖";
  EXPECT_EQ(field.Check(node.id, node.x - 0.6f * r, node.y - 0.6f * r), NodeCheck::kOk) << "대각선 거리 0.85r은 안";
}

// ============================================================ 체력, 고갈, 재생

TEST(ResourceFieldTest, EachHitTakesOneHealthAndReturnsTheKind) {
  PlayConfig config;
  ResourceField field(config);
  const ResourceNode tree = field.nodes()[0];
  const ResourceNode rock = field.nodes()[config.tree_count];
  ASSERT_EQ(tree.kind, ResourceKind::kTree);
  ASSERT_EQ(rock.kind, ResourceKind::kRock);

  EXPECT_EQ(field.Hit(tree.id), ResourceKind::kTree);
  EXPECT_EQ(field.Find(tree.id)->hp, config.tree_hp - 1);
  EXPECT_EQ(field.Hit(rock.id), ResourceKind::kRock);
  EXPECT_EQ(field.Find(rock.id)->hp, config.rock_hp - 1);
}

TEST(ResourceFieldTest, HealthNeverGoesBelowZeroAndDepletedNodesAreNotHit) {
  PlayConfig config;
  config.tree_hp = 2;
  ResourceField field(config);
  const std::uint32_t id = field.nodes()[0].id;
  field.Hit(id);
  field.Hit(id);
  EXPECT_EQ(field.Find(id)->hp, 0u);
  field.Hit(id);  // 검사 없이 불려도 0 아래로 내려가지 않는다
  field.Hit(id);
  EXPECT_EQ(field.Find(id)->hp, 0u);
  field.Hit(0);  // 없는 번호는 무시
  field.Hit(999999);
}

TEST(ResourceFieldTest, ADepletedNodeStaysDepletedThroughTheDayAndComesBackAtTheNight) {
  PlayConfig config;
  config.tree_hp = 1;
  ResourceField field(config);
  const std::uint32_t id = field.nodes()[0].id;

  field.Hit(id);  // 고갈
  EXPECT_EQ(field.Find(id)->hp, 0u);
  // 옛 규칙(고갈 뒤 600틱 재생)의 시각이 지나도, 낮 동안의 Advance는 채집으로 고갈된 노드를 건드리지 않는다.
  EXPECT_EQ(field.Advance(600), 0u);
  EXPECT_EQ(field.Advance(601), 0u);
  EXPECT_EQ(field.Advance(1000000), 0u);
  EXPECT_EQ(field.Find(id)->hp, 0u) << "밤 정산 전에는 고갈 상태";
  EXPECT_EQ(field.RespawnAllDepleted(1000001), 1u) << "밤 정산에 재생";
  EXPECT_EQ(field.Find(id)->hp, config.tree_hp);
  EXPECT_EQ(field.RespawnAllDepleted(1000002), 0u) << "이미 재생했으면 다시 세지 않는다";
}

TEST(ResourceFieldTest, TheNightBringsBackEveryDepletedNodeAtOnceAndLeavesTheOthersAlone) {
  PlayConfig config;
  config.tree_hp = 1;
  config.rock_hp = 3;
  ResourceField field(config);
  field.Hit(1);
  field.Hit(2);
  field.Hit(5);
  const std::uint32_t rock = config.tree_count + 1;
  field.Hit(rock);  // 체력 3 -> 2(고갈 아님)
  field.TakeChanges();
  EXPECT_EQ(field.depleted_count(), 3u);
  EXPECT_EQ(field.RespawnAllDepleted(50), 3u);
  EXPECT_EQ(field.depleted_count(), 0u);
  EXPECT_EQ(field.Find(rock)->hp, 2u) << "고갈되지 않은 노드는 체력을 채우지 않는다";
  const auto changes = field.TakeChanges();
  ASSERT_EQ(changes.size(), 3u) << "되살아난 노드만 바뀜으로 기록된다";
  EXPECT_EQ(changes[0].node_id, 1u);
  EXPECT_EQ(changes[1].node_id, 2u);
  EXPECT_EQ(changes[2].node_id, 5u);
}

TEST(ResourceFieldTest, RespawnLeavesTheStructureSpotAndIsDeterministic) {
  PlayConfig config;
  config.tree_count = 1;
  config.rock_count = 0;
  config.tree_hp = 1;

  // 옛 자리 위에 집이 있다고 하자. 재생한 노드는 집 밖, 맵 안쪽에 나타난다.
  ResourceField field(config);
  const std::uint32_t id = field.nodes()[0].id;
  const ResourceNode before = *field.Find(id);
  field.Hit(id);
  SpotBlockers blockers;
  blockers.rects.push_back(Rect{static_cast<double>(before.x), static_cast<double>(before.y), 150.0, 150.0});
  EXPECT_EQ(field.RespawnAllDepleted(10, blockers), 1u);
  const ResourceNode after = *field.Find(id);
  EXPECT_FALSE(CircleOverlapsRect(after.x, after.y, config.node_collision_radius_cm, blockers.rects[0])) << "집 밖에 나타난다";
  EXPECT_GE(after.x, config.node_edge_margin_cm);
  EXPECT_LE(after.x, config.map_width_cm - config.node_edge_margin_cm);
  EXPECT_GE(after.y, config.node_edge_margin_cm);
  EXPECT_LE(after.y, config.map_height_cm - config.node_edge_margin_cm);

  // 같은 설정, 같은 틱, 같은 막힌 곳이면 같은 자리다.
  ResourceField again(config);
  again.Hit(id);
  EXPECT_EQ(again.RespawnAllDepleted(10, blockers), 1u);
  EXPECT_FLOAT_EQ(again.Find(id)->x, after.x);
  EXPECT_FLOAT_EQ(again.Find(id)->y, after.y);
}

TEST(ResourceFieldTest, StaysDepletedWhileNoSpotIsFreeAndRespawnsLater) {
  PlayConfig config;
  config.tree_count = 1;
  config.rock_count = 0;
  config.tree_hp = 1;
  ResourceField field(config);
  const std::uint32_t id = field.nodes()[0].id;
  field.Hit(id);

  // 밤 정산 때 맵 전체가 막혀 있으면 자리가 없다. 고갈 상태로 남고 세지 않는다.
  SpotBlockers everywhere;
  everywhere.rects.push_back(Rect{2500.0, 2500.0, 2500.0, 2500.0});
  EXPECT_EQ(field.RespawnAllDepleted(10, everywhere), 0u);
  EXPECT_EQ(field.Find(id)->hp, 0u);

  // 자리를 못 찾으면 매 틱 다시 찾지 않고 respawn_retry_ticks(20) 뒤에 다시 찾는다. 막힌 것이 없어져도 그때까지는 기다린다.
  EXPECT_EQ(field.Advance(11, SpotBlockers{}), 0u) << "재시도 시각(30틱) 전에는 찾지 않는다";
  EXPECT_EQ(field.Advance(29, SpotBlockers{}), 0u);
  EXPECT_EQ(field.Advance(30, SpotBlockers{}), 1u) << "정확히 재시도 시각에 다시 찾아 재생한다";
  EXPECT_EQ(field.Find(id)->hp, config.tree_hp);
}

TEST(ResourceFieldTest, ARetryThatFailsAgainWaitsAnotherInterval) {
  PlayConfig config;
  config.tree_count = 1;
  config.rock_count = 0;
  config.tree_hp = 1;
  config.respawn_retry_ticks = 7;
  ResourceField field(config);
  const std::uint32_t id = field.nodes()[0].id;
  field.Hit(id);
  SpotBlockers everywhere;
  everywhere.rects.push_back(Rect{2500.0, 2500.0, 2500.0, 2500.0});
  EXPECT_EQ(field.RespawnAllDepleted(10, everywhere), 0u);  // 밤 정산의 첫 시도 실패 -> 17틱에 다시
  EXPECT_EQ(field.Advance(16, SpotBlockers{}), 0u);
  EXPECT_EQ(field.Advance(17, everywhere), 0u);  // 둘째 시도도 실패 -> 24틱에 다시
  EXPECT_EQ(field.Advance(23, SpotBlockers{}), 0u);
  EXPECT_EQ(field.Advance(24, SpotBlockers{}), 1u);
}

// ============================================================ 구역 분리 (건설 구역 안에는 노드를 두지 않는다)

// 노드 중심이 건설 구역(노드 충돌 반지름만큼 넓힌 정사각형) 안인가.
bool InsideZone(const PlayConfig& config, const ResourceNode& node) {
  const double reach = config.build_zone_half_size_cm + config.node_collision_radius_cm;
  return std::abs(node.x - config.map_width_cm * 0.5) < reach && std::abs(node.y - config.map_height_cm * 0.5) < reach;
}

TEST(ResourceFieldTest, NodesAreNeverPlacedInsideTheBuildZone) {
  const PlayConfig config;
  const ResourceField field(config);
  ASSERT_EQ(field.nodes().size(), config.tree_count + config.rock_count) << "구역 밖에 자리가 충분해서 모두 놓인다";
  for (const ResourceNode& node : field.nodes()) {
    EXPECT_FALSE(InsideZone(config, node)) << node.id << " (" << node.x << ", " << node.y << ")";
  }
}

TEST(ResourceFieldTest, NodesJustOutsideTheZoneEdgeAreAllowedButNotInside) {
  // 구역 크기를 키우면 노드가 더 바깥으로 밀려난다.
  PlayConfig wide;
  wide.build_zone_half_size_cm = 2000.0f;
  const ResourceField field(wide);
  ASSERT_GT(field.nodes().size(), 0u);
  for (const ResourceNode& node : field.nodes()) {
    EXPECT_FALSE(InsideZone(wide, node)) << node.id;
  }
}

TEST(ResourceFieldTest, WithoutAZoneNodesMayBeNearTheCenterAsBefore) {
  PlayConfig config;
  config.build_zone_half_size_cm = 0.0f;  // 구역 제한 없음
  const ResourceField field(config);
  bool nearCenter = false;
  for (const ResourceNode& node : field.nodes()) {
    nearCenter = nearCenter || (std::abs(node.x - 2500.0f) < 1200.0f && std::abs(node.y - 2500.0f) < 1200.0f);
  }
  EXPECT_TRUE(nearCenter) << "구역을 끄면 중앙 가까이에도 놓인다(구역 규칙이 이 차이를 만든다)";
}

TEST(ResourceFieldTest, RespawnedNodesNeverLandInsideTheBuildZone) {
  PlayConfig config;
  config.tree_count = 10;
  config.rock_count = 0;
  config.tree_hp = 1;
  ResourceField field(config);
  for (std::uint64_t round = 0; round < 200; ++round) {
    const std::uint64_t tick = round * 3;
    for (const ResourceNode& node : field.nodes()) field.Hit(node.id);
    field.RespawnAllDepleted(tick + 1);
    for (const ResourceNode& node : field.nodes()) {
      ASSERT_GT(node.hp, 0u) << "라운드 " << round;
      ASSERT_FALSE(InsideZone(config, node)) << "라운드 " << round << " 노드 " << node.id << " (" << node.x << ", " << node.y << ")";
    }
  }
}

TEST(ResourceFieldTest, AdvanceWithNothingDepletedChangesNothing) {
  ResourceField field(PlayConfig{});
  EXPECT_EQ(field.Advance(100000), 0u);
  EXPECT_EQ(field.RespawnAllDepleted(100000), 0u);
  EXPECT_FALSE(field.HasChanges());
}

TEST(ResourceFieldTest, ANightThatFindsASpotCancelsAPendingRetry) {
  PlayConfig config;
  config.tree_count = 1;
  config.rock_count = 0;
  config.tree_hp = 1;
  ResourceField field(config);
  SpotBlockers everywhere;
  everywhere.rects.push_back(Rect{2500.0, 2500.0, 2500.0, 2500.0});
  field.Hit(1);
  ASSERT_EQ(field.RespawnAllDepleted(10, everywhere), 0u);  // 재시도 예약(30틱)
  ASSERT_EQ(field.RespawnAllDepleted(20), 1u) << "다음 밤 정산이 먼저 되살린다";
  field.Hit(1);  // 다시 고갈(채집)
  EXPECT_EQ(field.Advance(30), 0u) << "옛 재시도 예약이 남아 낮에 되살리지 않는다";
  EXPECT_EQ(field.Find(1)->hp, 0u);
}

TEST(ResourceFieldTest, ChangesAreCollectedOncePerNodeSortedAndThenCleared) {
  PlayConfig config;
  config.tree_hp = 3;
  ResourceField field(config);
  EXPECT_FALSE(field.HasChanges());
  field.Hit(3);
  field.Hit(1);
  field.Hit(3);  // 같은 노드를 두 번 쳐도 한 번만, 마지막 값으로
  EXPECT_TRUE(field.HasChanges());

  const auto changes = field.TakeChanges();
  ASSERT_EQ(changes.size(), 2u);
  EXPECT_EQ(changes[0].node_id, 1u);
  EXPECT_EQ(changes[0].hp, 2u);
  EXPECT_EQ(changes[1].node_id, 3u);
  EXPECT_EQ(changes[1].hp, 1u);
  EXPECT_FALSE(field.HasChanges());
  EXPECT_TRUE(field.TakeChanges().empty());
}

TEST(ResourceFieldTest, RespawnIsReportedAsAChange) {
  PlayConfig config;
  config.tree_hp = 1;
  ResourceField field(config);
  field.Hit(1);
  field.TakeChanges();  // 고갈 알림은 이미 나갔다고 치고 비운다
  field.RespawnAllDepleted(5);
  const auto changes = field.TakeChanges();
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].node_id, 1u);
  EXPECT_EQ(changes[0].hp, config.tree_hp);
}

// ============================================================ Room::Gather

TEST(RoomGatherTest, ClientsNotInTheRoomAreRejected) {
  Room room("111111", WideRange());
  room.AddMember(1, "alice");
  EXPECT_EQ(room.Gather(99, 1).status, GatherStatus::kNotInRoom);
  room.RemoveMember(1);
  EXPECT_EQ(room.Gather(1, 1).status, GatherStatus::kNotInRoom);
}

TEST(RoomGatherTest, TreesGiveWoodAndRocksGiveStoneToTheOneWhoHit) {
  PlayConfig config = WideRange();
  Room room("111111", config);
  room.AddMember(1, "alice");
  room.AddMember(2, "bob");
  const std::uint32_t tree = room.resources().nodes()[0].id;
  const std::uint32_t rock = room.resources().nodes()[config.tree_count].id;

  GatherOutcome out = room.Gather(1, tree);
  EXPECT_EQ(out.status, GatherStatus::kOk);
  EXPECT_EQ(out.wood, 1u);
  EXPECT_EQ(out.stone, 0u);
  EXPECT_EQ(room.resources().Find(tree)->hp, config.tree_hp - 1);

  out = room.Gather(2, rock);
  EXPECT_EQ(out.status, GatherStatus::kOk);
  EXPECT_EQ(out.wood, 0u);
  EXPECT_EQ(out.stone, 1u) << "인벤토리는 플레이어별이다";
  EXPECT_EQ(room.FindByClient(1)->wood, 1u);
  EXPECT_EQ(room.FindByClient(1)->stone, 0u);
}

TEST(RoomGatherTest, RejectionOrderIsMissingDepletedTooFarThenCooldown) {
  PlayConfig config = WideRange();
  config.tree_hp = 1;
  config.rock_hp = 1;
  Room room("111111", config);
  const std::uint32_t player = room.AddMember(1, "alice");

  // 가장 가까운 노드와 가장 먼 노드, 그 사이 허용 거리를 구한다.
  const auto& nodes = room.resources().nodes();
  const auto [near_it, far_it] = std::minmax_element(nodes.begin(), nodes.end(), [&](const auto& a, const auto& b) {
    return DistanceTo(room, player, a) < DistanceTo(room, player, b);
  });
  const std::uint32_t near_id = near_it->id;
  const std::uint32_t far_id = far_it->id;
  ASSERT_LT(DistanceTo(room, player, *near_it), DistanceTo(room, player, *far_it));
  const double middle = (DistanceTo(room, player, *near_it) + DistanceTo(room, player, *far_it)) / 2.0;

  PlayConfig limited = config;
  limited.gather_range_cm = static_cast<float>(middle);
  Room limited_room("222222", limited);
  limited_room.AddMember(1, "alice");

  EXPECT_EQ(limited_room.Gather(1, 0).status, GatherStatus::kNodeNotFound);
  EXPECT_EQ(limited_room.Gather(1, 9999).status, GatherStatus::kNodeNotFound);
  EXPECT_EQ(limited_room.Gather(1, far_id).status, GatherStatus::kTooFar);
  EXPECT_EQ(limited_room.Gather(1, near_id).status, GatherStatus::kOk);  // 이제 쿨다운 시작, near는 고갈

  // 고갈 > 너무 멂 > 쿨다운 순서
  EXPECT_EQ(limited_room.Gather(1, near_id).status, GatherStatus::kNodeDepleted) << "고갈이 쿨다운보다 먼저";
  EXPECT_EQ(limited_room.Gather(1, far_id).status, GatherStatus::kTooFar) << "너무 멂이 쿨다운보다 먼저";
  // 쿨다운에 걸리는 경우: 가까운 다른 노드를 찾는다
  std::uint32_t other_near = 0;
  for (const auto& node : limited_room.resources().nodes()) {
    if (node.id != near_id && node.hp > 0 &&
        limited_room.resources().Check(node.id, limited_room.world().Find(1)->x, limited_room.world().Find(1)->y) ==
            NodeCheck::kOk) {
      other_near = node.id;
      break;
    }
  }
  ASSERT_NE(other_near, 0u) << "허용 거리 안에 다른 노드가 하나는 있어야 쿨다운을 시험할 수 있다";
  EXPECT_EQ(limited_room.Gather(1, other_near).status, GatherStatus::kCooldown);
}

TEST(RoomGatherTest, CooldownAllowsTheNextHitExactlyAfterTheConfiguredTicks) {
  PlayConfig config = WideRange();
  config.gather_cooldown_ticks = 10;
  Room room("111111", config);
  room.AddMember(1, "alice");
  room.AddMember(2, "bob");

  EXPECT_EQ(room.Gather(1, 1).status, GatherStatus::kOk);
  EXPECT_EQ(room.Gather(1, 1).status, GatherStatus::kCooldown) << "같은 틱의 두 번째";
  EXPECT_EQ(room.Gather(2, 1).status, GatherStatus::kOk) << "다른 플레이어는 영향이 없다";

  Advance(room, 9);
  EXPECT_EQ(room.Gather(1, 1).status, GatherStatus::kCooldown) << "9틱 뒤에는 아직";
  Advance(room, 1);
  EXPECT_EQ(room.Gather(1, 1).status, GatherStatus::kOk) << "10틱 뒤에 허용(경계 포함)";
}

TEST(RoomGatherTest, RejectedRequestsChangeNothingNotEvenTheCooldownBase) {
  PlayConfig config = WideRange();
  config.gather_cooldown_ticks = 10;
  Room room("111111", config);
  room.AddMember(1, "alice");

  ASSERT_EQ(room.Gather(1, 1).status, GatherStatus::kOk);
  const std::uint32_t hp_after = room.resources().Find(1)->hp;
  Advance(room, 5);
  for (int i = 0; i < 3; ++i) {
    const GatherOutcome out = room.Gather(1, 1);  // 쿨다운 중 거부
    EXPECT_EQ(out.status, GatherStatus::kCooldown);
    EXPECT_EQ(out.wood, 1u) << "거부 응답도 현재 인벤토리를 담는다";
  }
  EXPECT_EQ(room.resources().Find(1)->hp, hp_after);
  EXPECT_EQ(room.FindByClient(1)->wood, 1u);

  Advance(room, 5);  // 첫 채집으로부터 10틱. 거부가 기준 틱을 밀었다면 아직 쿨다운이다.
  EXPECT_EQ(room.Gather(1, 1).status, GatherStatus::kOk);
}

TEST(RoomGatherTest, ADepletedNodeStaysDepletedAllDayAndReturnsAfterTheNight) {
  PlayConfig config = WideRange();
  config.tree_hp = 1;
  Room room("111111", config);
  room.AddMember(1, "alice");

  ASSERT_EQ(room.Gather(1, 1).status, GatherStatus::kOk);
  EXPECT_EQ(room.resources().Find(1)->hp, 0u);
  Advance(room, 600);  // 옛 재생 시각
  EXPECT_EQ(room.resources().Find(1)->hp, 0u);
  EXPECT_EQ(room.Gather(1, 1).status, GatherStatus::kNodeDepleted);
  Advance(room, 5000);
  EXPECT_EQ(room.resources().Find(1)->hp, 0u) << "밤 정산 전에는 하루 종일 고갈";
  const NightReport report = room.RunNight(room.world().tick(), false);
  EXPECT_EQ(report.respawned, 1u);
  EXPECT_EQ(room.resources().Find(1)->hp, 1u);
  EXPECT_EQ(room.Gather(1, 1).status, GatherStatus::kOk) << "재생한 노드는 다시 칠 수 있다";
}

TEST(RoomGatherTest, WhenFourPlayersHitTheLastHealthInTheSameTickExactlyOneSucceeds) {
  PlayConfig config = WideRange();
  config.tree_hp = 1;
  Room room("111111", config);
  for (std::uint64_t id = 1; id <= 4; ++id) room.AddMember(id, "p" + std::to_string(id));

  int ok = 0;
  int depleted = 0;
  for (std::uint64_t id = 1; id <= 4; ++id) {
    const GatherStatus status = room.Gather(id, 1).status;
    ok += status == GatherStatus::kOk;
    depleted += status == GatherStatus::kNodeDepleted;
  }
  EXPECT_EQ(ok, 1);
  EXPECT_EQ(depleted, 3);
  EXPECT_EQ(room.resources().Find(1)->hp, 0u);
  std::uint32_t total_wood = 0;
  for (const auto& member : room.members()) total_wood += member.wood;
  EXPECT_EQ(total_wood, 1u) << "마지막 한 번의 몫은 한 명만 받는다";
}

TEST(RoomGatherTest, RoomsDoNotShareNodesOrInventories) {
  PlayConfig config = WideRange();
  config.tree_hp = 1;
  Room a("111111", config);
  Room b("222222", config);
  a.AddMember(1, "alice");
  b.AddMember(1, "alice");  // 같은 client_id와 같은 node_id를 다른 방에서 쓴다

  ASSERT_EQ(a.Gather(1, 1).status, GatherStatus::kOk);
  EXPECT_EQ(a.resources().Find(1)->hp, 0u);
  EXPECT_EQ(b.resources().Find(1)->hp, 1u);
  EXPECT_EQ(b.FindByClient(1)->wood, 0u);
  EXPECT_EQ(b.Gather(1, 1).status, GatherStatus::kOk);
}

TEST(RoomGatherTest, TheInventoryIsLostWhenLeavingAndStartsEmptyOnRejoin) {
  Room room("111111", WideRange());
  room.AddMember(1, "alice");
  ASSERT_EQ(room.Gather(1, 1).status, GatherStatus::kOk);
  room.RemoveMember(1);
  room.AddMember(1, "alice");
  EXPECT_EQ(room.FindByClient(1)->wood, 0u);
  EXPECT_EQ(room.Gather(1, 1).wood, 1u);
}

TEST(RoomGatherTest, ASleepingPlayerCannotGatherAndNothingChanges) {
  PlayConfig config = WideRange();
  config.sleep_range_cm = 100000.0f;  // 어디서든 잘 수 있게(잠 거리는 따로 시험한다)
  config.gather_cooldown_ticks = 10;
  Room room("111111", config);
  room.AddMember(1, "alice");
  ASSERT_EQ(room.Gather(1, 1).status, GatherStatus::kOk);
  Advance(room, 20);  // 쿨다운이 끝났다
  ASSERT_EQ(room.Sleep(1, true).status, SleepStatus::kOk);
  const std::uint32_t hp = room.resources().Find(1)->hp;
  const Member before = *room.FindByClient(1);

  const GatherOutcome out = room.Gather(1, 1);
  EXPECT_EQ(out.status, GatherStatus::kSleeping);
  EXPECT_EQ(out.wood, 1u) << "거부 응답도 현재 인벤토리를 담는다";
  EXPECT_EQ(room.resources().Find(1)->hp, hp) << "노드가 바뀌지 않는다";
  EXPECT_EQ(room.FindByClient(1)->wood, before.wood);
  EXPECT_EQ(room.FindByClient(1)->last_gather_tick, before.last_gather_tick) << "쿨다운 기준이 바뀌지 않는다";

  // 판정 순서: 방에 없음 -> 잠듦 -> 노드 없음 -> 고갈 -> 너무 멂 -> 쿨다운
  EXPECT_EQ(room.Gather(99, 1).status, GatherStatus::kNotInRoom);
  EXPECT_EQ(room.Gather(1, 0).status, GatherStatus::kSleeping) << "잠듦이 노드 없음보다 먼저";
  EXPECT_EQ(room.Gather(1, 9999).status, GatherStatus::kSleeping);

  ASSERT_EQ(room.Sleep(1, false).status, SleepStatus::kOk);
  EXPECT_EQ(room.Gather(1, 1).status, GatherStatus::kOk) << "깨어나면 다시 칠 수 있다";
}

TEST(RoomGatherTest, WorksInARoomWithNoNodes) {
  PlayConfig config = WideRange();
  config.tree_count = 0;
  config.rock_count = 0;
  Room room("111111", config);
  room.AddMember(1, "alice");
  EXPECT_EQ(room.Gather(1, 1).status, GatherStatus::kNodeNotFound);
}

}  // namespace
}  // namespace game::play
