// 이동 충돌 테스트: 구조물과 고갈되지 않은 노드가 플레이어의 이동을 막는가. 소켓도 시계도 없고, 시간은 Step 횟수다.
// docs/content-roadmap.md 완료 기준 4.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "play/geometry.h"
#include "play/room.h"

namespace game::play {
namespace {

constexpr float kDt = 0.05f;

PlayConfig Empty() {
  PlayConfig config;
  config.tree_count = 0;
  config.rock_count = 0;
  config.build_zone_half_size_cm = 0.0f;  // 구역 제한 없음: 노드가 중앙 가까이에도 놓인다
  return config;
}

void Go(Room& room, std::uint32_t player, std::uint32_t& seq, float dx, float dy) {
  room.world().SetInput(player, ++seq, dx, dy);
}

void RunTicks(Room& room, std::uint32_t ticks) {
  for (std::uint32_t i = 0; i < ticks; ++i) {
    room.world().Step(kDt);
    room.resources().Advance(room.world().tick());
    room.structures().Advance(room.world().tick());
  }
}

// 몸(반지름 r)이 어떤 구조물이나 살아 있는 노드와 겹치는가.
bool Overlapping(const Room& room, const PlayConfig& config, const PlayerBody& body) {
  for (const Structure& structure : room.structures().structures()) {
    if (CircleOverlapsRect(body.x, body.y, config.player_radius_cm, structure.Footprint())) return true;
  }
  for (const ResourceNode& node : room.resources().nodes()) {
    if (node.hp > 0 &&
        CirclesOverlap(body.x, body.y, config.player_radius_cm, node.x, node.y, config.node_collision_radius_cm)) {
      return true;
    }
  }
  return false;
}

// ============================================================ 구조물

TEST(CollisionTest, AWallStopsThePlayerCloseToItAndItStaysThere) {
  const PlayConfig config = Empty();
  Room room("111111", config);
  const std::uint32_t player = room.AddMember(1, "alice");  // (2400, 2400)
  room.structures().Add(StructureType::kHouse, 2800.0f, 2400.0f, 0);  // x 2650~2950
  std::uint32_t seq = 0;
  Go(room, player, seq, 1.0f, 0.0f);

  for (int i = 0; i < 60; ++i) {
    RunTicks(room, 1);
    EXPECT_LE(room.world().Find(player)->x, 2610.0f + 1e-3f) << "벽(2650) - 몸 반지름(40)을 넘지 않는다";
  }
  const float x = room.world().Find(player)->x;
  EXPECT_GE(x, 2610.0f - 5.0f - 1e-3f) << "하위 이동 한 번(5 cm) 이내까지 다가간다";
  RunTicks(room, 20);
  EXPECT_EQ(room.world().Find(player)->x, x) << "더 가지 않는다";
  EXPECT_EQ(room.world().Find(player)->y, 2400.0f);
}

TEST(CollisionTest, ADiagonalMoveSlidesAlongTheWall) {
  const PlayConfig config = Empty();
  Room room("111111", config);
  const std::uint32_t player = room.AddMember(1, "alice");
  room.structures().Add(StructureType::kHouse, 2800.0f, 2400.0f, 0);  // x 2650~2950, y 2250~2550
  std::uint32_t seq = 0;
  Go(room, player, seq, 1.0f, 0.2f);  // 거의 벽 쪽(+x)이지만 조금씩 위(+y)로: 약 14틱에 벽에 닿는다

  RunTicks(room, 14);
  const float y_at_contact = room.world().Find(player)->y;
  RunTicks(room, 26);  // 벽에 닿은 뒤에도 y는 계속 올라가야 한다
  const PlayerBody* body = room.world().Find(player);
  EXPECT_LE(body->x, 2610.0f + 1e-3f) << "x는 벽에 막힘";
  EXPECT_GE(body->x, 2600.0f) << "벽 앞까지는 다가갔다";
  EXPECT_GT(body->y, y_at_contact + 50.0f) << "막힌 뒤에도 y는 벽을 따라 계속 올라감(미끄러짐)";
  EXPECT_LT(body->y, 2590.0f) << "아직 벽의 위쪽 모서리를 지나지 않았다(이 구간 내내 벽 옆)";
}

TEST(CollisionTest, ABlockedAxisDoesNotStopTheFreeAxisAtACorner) {
  const PlayConfig config = Empty();
  Room room("111111", config);
  const std::uint32_t player = room.AddMember(1, "alice");
  room.structures().Add(StructureType::kHouse, 2800.0f, 2400.0f, 0);  // 위쪽 끝 y 2550
  std::uint32_t seq = 0;
  Go(room, player, seq, 1.0f, 1.0f);
  RunTicks(room, 200);  // 벽을 따라 올라가서 모서리를 지나고 맵 끝까지
  const PlayerBody* body = room.world().Find(player);
  EXPECT_GT(body->x, 2950.0f) << "모서리를 돌아 벽 반대편으로 넘어간다";
}

TEST(CollisionTest, NoInputSequenceEverGetsInsideAnObstacle) {
  PlayConfig config = Empty();
  config.tree_count = 30;
  config.rock_count = 20;
  Room room("111111", config);
  const std::uint32_t player = room.AddMember(1, "alice");
  // 중앙 주위에 집 여러 채
  room.structures().Add(StructureType::kHouse, 2200.0f, 2800.0f, 0);
  room.structures().Add(StructureType::kHouse, 2800.0f, 2800.0f, 0);
  room.structures().Add(StructureType::kHouse, 2800.0f, 2100.0f, 0);
  room.structures().Add(StructureType::kHouse, 2100.0f, 2100.0f, 0);

  std::mt19937 random(12345);
  std::uniform_real_distribution<float> any(-1.5f, 1.5f);
  std::uint32_t seq = 0;
  for (int tick = 0; tick < 6000; ++tick) {
    if (tick % 7 == 0) Go(room, player, seq, any(random), any(random));
    RunTicks(room, 1);
    const PlayerBody* body = room.world().Find(player);
    ASSERT_FALSE(Overlapping(room, config, *body)) << "틱 " << tick << " (" << body->x << ", " << body->y << ")";
    ASSERT_GE(body->x, config.player_radius_cm);
    ASSERT_LE(body->x, config.map_width_cm - config.player_radius_cm);
    ASSERT_GE(body->y, config.player_radius_cm);
    ASSERT_LE(body->y, config.map_height_cm - config.player_radius_cm);
  }
}

TEST(CollisionTest, TheSameInputsGiveTheSameResultsEveryTime) {
  auto run = [] {
    PlayConfig config = Empty();
    config.tree_count = 30;
    config.rock_count = 20;
    Room room("111111", config);
    const std::uint32_t player = room.AddMember(1, "alice");
    room.structures().Add(StructureType::kHouse, 2800.0f, 2400.0f, 0);
    std::mt19937 random(777);
    std::uniform_real_distribution<float> any(-1.0f, 1.0f);
    std::uint32_t seq = 0;
    std::vector<float> positions;
    for (int tick = 0; tick < 2000; ++tick) {
      if (tick % 5 == 0) Go(room, player, seq, any(random), any(random));
      RunTicks(room, 1);
      positions.push_back(room.world().Find(player)->x);
      positions.push_back(room.world().Find(player)->y);
    }
    return positions;
  };
  EXPECT_EQ(run(), run()) << "같은 입력 열이면 같은 위치(결정성)";
}

TEST(CollisionTest, AMoveSpeedFasterThanTheThinnestObstacleCouldNeverTunnel) {
  // 한 틱 이동량이 가장 얇은 장애물(노드 지름 120 cm, 몸 지름 80 cm을 더한 판정 폭)보다 훨씬 작다는 전제를 확인한다.
  const PlayConfig config;
  const float step = config.move_speed_cm_per_s * config.TickSeconds();
  EXPECT_LT(step * 2.0f, 2.0f * (config.node_collision_radius_cm + config.player_radius_cm));
}

// ============================================================ 노드

TEST(CollisionTest, ALiveNodeBlocksUntilItIsDepletedAndThenLetsThePlayerThrough) {
  PlayConfig config = Empty();
  config.tree_count = 1;
  config.tree_hp = 1;
  config.respawn_ticks = 100000;
  Room room("111111", config);
  const std::uint32_t player = room.AddMember(1, "alice");
  const ResourceNode node = room.resources().nodes()[0];

  const PlayerBody* body = room.world().Find(player);
  const double length = std::hypot(node.x - body->x, node.y - body->y);
  const float dx = static_cast<float>((node.x - body->x) / length);
  const float dy = static_cast<float>((node.y - body->y) / length);
  std::uint32_t seq = 0;
  Go(room, player, seq, dx, dy);

  const double blocked_distance = config.player_radius_cm + config.node_collision_radius_cm;
  double closest = 1.0e9;
  for (int i = 0; i < 150; ++i) {
    RunTicks(room, 1);
    body = room.world().Find(player);
    closest = std::min(closest, static_cast<double>(std::hypot(body->x - node.x, body->y - node.y)));
  }
  EXPECT_GE(closest, blocked_distance - 1e-3) << "살아 있는 노드의 충돌 반경(40+60)보다 가까이 가지 못한다";
  EXPECT_LE(closest, blocked_distance + 6.0) << "닿을 때까지는 다가간다";

  room.resources().Hit(node.id, room.world().tick());  // 체력 1이라 고갈
  // 막힌 채로 노드를 돌아 미끄러졌으므로 노드 중심을 다시 겨냥해서 걸어 들어간다.
  body = room.world().Find(player);
  const double back = std::hypot(node.x - body->x, node.y - body->y);
  Go(room, player, seq, static_cast<float>((node.x - body->x) / back), static_cast<float>((node.y - body->y) / back));
  double through = 1.0e9;  // 지나가는 동안의 가장 가까운 거리(노드 중심을 지나 반대편으로 나간다)
  for (int i = 0; i < 30; ++i) {
    RunTicks(room, 1);
    body = room.world().Find(player);
    through = std::min(through, static_cast<double>(std::hypot(body->x - node.x, body->y - node.y)));
  }
  EXPECT_LT(through, blocked_distance - 20.0) << "고갈된 노드는 통과한다";
}

TEST(CollisionTest, ARespawningNodeUnderThePlayerDoesNotTrapThem) {
  PlayConfig config = Empty();
  config.tree_count = 1;
  config.tree_hp = 1;
  config.respawn_ticks = 100000;
  Room room("111111", config);
  const std::uint32_t player = room.AddMember(1, "alice");
  const ResourceNode node = room.resources().nodes()[0];
  room.resources().Hit(node.id, 0);  // 고갈: 이제 통과할 수 있다

  // 노드 중심 가까이(20 cm 안)까지 걸어 들어간다.
  std::uint32_t seq = 0;
  const PlayerBody* body = room.world().Find(player);
  for (int i = 0; i < 400; ++i) {
    const double length = std::hypot(node.x - body->x, node.y - body->y);
    if (length < 20.0) break;
    Go(room, player, seq, static_cast<float>((node.x - body->x) / length), static_cast<float>((node.y - body->y) / length));
    RunTicks(room, 1);
    body = room.world().Find(player);
  }
  Go(room, player, seq, 0.0f, 0.0f);
  ASSERT_LT(std::hypot(node.x - body->x, node.y - body->y), 20.0) << "노드 안까지 걸어 들어갔다";

  // 노드가 재생한다. 재생 자리는 플레이어 몸을 피해 고르므로 노드는 몸 밖에 나타나고 몸은 갇히지 않는다.
  room.AdvanceResources(room.world().tick() + 200000);
  const ResourceNode respawned = *room.resources().Find(node.id);
  ASSERT_GT(respawned.hp, 0u);
  EXPECT_FALSE(Overlapping(room, config, *body)) << "재생 자리가 플레이어 몸과 겹치면 안 된다";
  EXPECT_GE(std::hypot(respawned.x - body->x, respawned.y - body->y),
            config.player_radius_cm + config.node_collision_radius_cm);
}

// ============================================================ 겹쳐 있는 채로 시작, 입장 위치

TEST(CollisionTest, APlayerAlreadyInsideAStructureCanWalkOut) {
  const PlayConfig config = Empty();
  Room room("111111", config);
  const std::uint32_t player = room.AddMember(1, "alice");  // (2400, 2400)
  room.structures().Add(StructureType::kHouse, 2500.0f, 2400.0f, 0);  // 2350~2650: 몸이 안에 있다
  std::uint32_t seq = 0;
  Go(room, player, seq, -1.0f, 0.0f);
  RunTicks(room, 20);
  EXPECT_LT(room.world().Find(player)->x, 2350.0f - config.player_radius_cm + 1.0f) << "왼쪽 벽 밖으로 나왔다";
}

TEST(CollisionTest, ALateJoinerIsPlacedOnTheNearestFreeSpotNotInsideTheHouse) {
  const PlayConfig config = Empty();
  Room room("111111", config);
  room.AddMember(1, "alice");
  room.structures().Add(StructureType::kHouse, 2500.0f, 2500.0f, 0);  // 2350~2650: 모든 시작 칸을 덮는다
  for (std::uint64_t id = 2; id <= 4; ++id) {
    const std::uint32_t player = room.AddMember(id, "p" + std::to_string(id));
    const PlayerBody* body = room.world().Find(player);
    EXPECT_TRUE(room.world().IsFree(body->x, body->y)) << "(" << body->x << ", " << body->y << ")";
    EXPECT_LT(std::hypot(body->x - 2500.0, body->y - 2500.0), 500.0) << "중앙에서 멀지 않은 빈 자리";
  }
}

TEST(CollisionTest, WithoutObstaclesTheMovementIsExactlyTheOldFormula) {
  // 장애물을 연결하지 않은 월드는 기존 계산(단일 이동, 경계 클램프)과 같다.
  const PlayConfig config;
  World world(config);
  world.AddPlayer(1, 0);
  world.SetInput(1, 1, 1.0f, 0.0f);
  world.Step(kDt);
  EXPECT_EQ(world.Find(1)->x, 2400.0f + 360.0f * kDt);
}

}  // namespace
}  // namespace game::play
