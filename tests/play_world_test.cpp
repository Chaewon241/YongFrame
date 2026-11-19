// World 단위 테스트: 이동 계산만 검증한다. 소켓, 방, 시간이 필요 없는 순수 계산이라 결과가 결정적이다.
// 기본값: 맵 5,000 x 5,000 cm, 속도 360 cm/s, 반지름 40 cm, 틱 20Hz(dt = 0.05초).

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "play/world.h"

namespace game::play {
namespace {

constexpr float kDt = 0.05f;  // 20Hz

PlayConfig Config() { return PlayConfig{}; }

// 한 플레이어를 칸 0에 둔 월드를 만든다. 칸 0의 시작 위치는 (2400, 2400).
World WorldWithOne(std::uint32_t id = 1) {
  World world(Config());
  world.AddPlayer(id, 0);
  return world;
}

void StepSeconds(World& world, float seconds) {
  const int steps = static_cast<int>(std::lround(seconds / kDt));
  for (int i = 0; i < steps; ++i) world.Step(kDt);
}

TEST(WorldTest, PlayersStartAtMapCenterOffsetBySlot) {
  World world(Config());
  world.AddPlayer(1, 0);
  world.AddPlayer(2, 1);
  world.AddPlayer(3, 2);
  world.AddPlayer(4, 3);
  ASSERT_EQ(world.players().size(), 4u);
  EXPECT_FLOAT_EQ(world.Find(1)->x, 2400.0f);
  EXPECT_FLOAT_EQ(world.Find(1)->y, 2400.0f);
  EXPECT_FLOAT_EQ(world.Find(2)->x, 2600.0f);
  EXPECT_FLOAT_EQ(world.Find(2)->y, 2400.0f);
  EXPECT_FLOAT_EQ(world.Find(3)->x, 2400.0f);
  EXPECT_FLOAT_EQ(world.Find(3)->y, 2600.0f);
  EXPECT_FLOAT_EQ(world.Find(4)->x, 2600.0f);
  EXPECT_FLOAT_EQ(world.Find(4)->y, 2600.0f);
}

TEST(WorldTest, SlotsBeyondFourStartAtTheCenter) {
  World world(Config());
  world.AddPlayer(1, 9);
  EXPECT_FLOAT_EQ(world.Find(1)->x, 2500.0f);
  EXPECT_FLOAT_EQ(world.Find(1)->y, 2500.0f);
}

TEST(WorldTest, AddingTheSameIdTwiceDoesNothing) {
  World world(Config());
  world.AddPlayer(1, 0);
  world.AddPlayer(1, 1);
  EXPECT_EQ(world.players().size(), 1u);
  EXPECT_FLOAT_EQ(world.Find(1)->x, 2400.0f);
}

TEST(WorldTest, PlayersDoNotMoveWithoutInput) {
  World world = WorldWithOne();
  StepSeconds(world, 2.0f);
  EXPECT_FLOAT_EQ(world.Find(1)->x, 2400.0f);
  EXPECT_FLOAT_EQ(world.Find(1)->y, 2400.0f);
}

TEST(WorldTest, MovesAtTheConfiguredSpeed) {
  World world = WorldWithOne();
  ASSERT_EQ(world.SetInput(1, 1, 1.0f, 0.0f), InputResult::kApplied);
  StepSeconds(world, 1.0f);  // 360 cm/s x 1초
  EXPECT_NEAR(world.Find(1)->x, 2760.0f, 0.01f);
  EXPECT_FLOAT_EQ(world.Find(1)->y, 2400.0f);
}

TEST(WorldTest, SubUnitDirectionMovesProportionallySlower) {
  World world = WorldWithOne();
  ASSERT_EQ(world.SetInput(1, 1, 0.0f, -0.5f), InputResult::kApplied);
  StepSeconds(world, 1.0f);
  EXPECT_NEAR(world.Find(1)->y, 2400.0f - 180.0f, 0.01f);
}

// 속도 조작 방지: 길이가 1을 넘는 방향은 1로 줄어야 한다.
TEST(WorldTest, OversizedDirectionIsNormalizedSoItCannotSpeedHack) {
  World world = WorldWithOne();
  ASSERT_EQ(world.SetInput(1, 1, 10.0f, 10.0f), InputResult::kApplied);
  StepSeconds(world, 1.0f);
  const float dx = world.Find(1)->x - 2400.0f;
  const float dy = world.Find(1)->y - 2400.0f;
  EXPECT_NEAR(std::sqrt(dx * dx + dy * dy), 360.0f, 0.05f);
  EXPECT_NEAR(dx, dy, 0.001f);  // 대각선 방향이 유지된다
  EXPECT_LE(std::sqrt(world.Find(1)->dir_x * world.Find(1)->dir_x + world.Find(1)->dir_y * world.Find(1)->dir_y),
            1.0001f);
}

TEST(WorldTest, HugeButFiniteDirectionIsStillLimited) {
  World world = WorldWithOne();
  ASSERT_EQ(world.SetInput(1, 1, 1e30f, 0.0f), InputResult::kApplied);
  StepSeconds(world, 1.0f);
  EXPECT_NEAR(world.Find(1)->x, 2760.0f, 0.05f);
}

// 길이 제곱이 float 범위를 넘치는 값(회귀 시험): 1e30 이상에서 제곱이 무한대가 되어 방향이 0이나 NaN이 되던 버그.
TEST(WorldTest, DirectionsThatOverflowWhenSquaredAreStillNormalized) {
  const float big = std::numeric_limits<float>::max();
  const float cases[][2] = {{1e30f, 1e30f}, {big, big}, {big, 0.0f}, {-big, big}, {0.0f, -1e30f}};
  for (const auto& c : cases) {
    World world = WorldWithOne();
    ASSERT_EQ(world.SetInput(1, 1, c[0], c[1]), InputResult::kApplied) << c[0] << "," << c[1];
    const PlayerBody& body = *world.Find(1);
    EXPECT_TRUE(std::isfinite(body.dir_x) && std::isfinite(body.dir_y)) << c[0] << "," << c[1];
    EXPECT_NEAR(std::hypot(body.dir_x, body.dir_y), 1.0f, 1e-5f) << c[0] << "," << c[1];
    StepSeconds(world, 1.0f);
    EXPECT_TRUE(std::isfinite(body.x) && std::isfinite(body.y)) << c[0] << "," << c[1];
    EXPECT_GE(body.x, 40.0f);
    EXPECT_LE(body.x, 4960.0f);
    EXPECT_GE(body.y, 40.0f);
    EXPECT_LE(body.y, 4960.0f);
  }
}

TEST(WorldTest, ZeroDirectionStopsTheMovement) {
  World world = WorldWithOne();
  ASSERT_EQ(world.SetInput(1, 1, 1.0f, 0.0f), InputResult::kApplied);
  StepSeconds(world, 0.5f);
  const float stopped_at = world.Find(1)->x;
  ASSERT_EQ(world.SetInput(1, 2, 0.0f, 0.0f), InputResult::kApplied);
  StepSeconds(world, 1.0f);
  EXPECT_FLOAT_EQ(world.Find(1)->x, stopped_at);
}

TEST(WorldTest, LastInputStaysInEffectUntilTheNextOne) {
  World world = WorldWithOne();
  ASSERT_EQ(world.SetInput(1, 1, 0.0f, 1.0f), InputResult::kApplied);
  StepSeconds(world, 0.5f);
  StepSeconds(world, 0.5f);  // 입력을 다시 보내지 않아도 계속 움직인다
  EXPECT_NEAR(world.Find(1)->y, 2760.0f, 0.01f);
}

// 맵 경계: 몸 반지름(40 cm)만큼 안쪽까지만 갈 수 있다.
TEST(WorldTest, StaysInsideTheMapOnEveryEdge) {
  const PlayConfig config = Config();
  struct Case {
    float dx, dy;
    float expect_x, expect_y;
  };
  const Case cases[] = {
      {-1, 0, 40.0f, 2400.0f},
      {1, 0, 4960.0f, 2400.0f},
      {0, -1, 2400.0f, 40.0f},
      {0, 1, 2400.0f, 4960.0f},
      {-1, -1, 40.0f, 40.0f},
      {1, 1, 4960.0f, 4960.0f},
  };
  for (const auto& c : cases) {
    World world(config);
    world.AddPlayer(1, 0);
    ASSERT_EQ(world.SetInput(1, 1, c.dx, c.dy), InputResult::kApplied);
    StepSeconds(world, 60.0f);  // 충분히 오래 밀어도
    EXPECT_FLOAT_EQ(world.Find(1)->x, c.expect_x) << c.dx << "," << c.dy;
    EXPECT_FLOAT_EQ(world.Find(1)->y, c.expect_y) << c.dx << "," << c.dy;
  }
}

TEST(WorldTest, SlidesAlongAnEdgeWhenMovingDiagonally) {
  World world = WorldWithOne();
  ASSERT_EQ(world.SetInput(1, 1, -1.0f, 1.0f), InputResult::kApplied);
  StepSeconds(world, 60.0f);
  EXPECT_FLOAT_EQ(world.Find(1)->x, 40.0f);   // 왼쪽 벽에 닿아도
  EXPECT_FLOAT_EQ(world.Find(1)->y, 4960.0f);  // 아래쪽으로는 끝까지 간다
}

// NaN, 무한대는 위치 계산을 망가뜨리므로 입력 전체를 버리고, 이전 방향이 그대로 유지된다.
TEST(WorldTest, NonFiniteInputIsIgnoredAndKeepsThePreviousDirection) {
  World world = WorldWithOne();
  ASSERT_EQ(world.SetInput(1, 1, 1.0f, 0.0f), InputResult::kApplied);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  EXPECT_EQ(world.SetInput(1, 2, nan, 0.0f), InputResult::kInvalid);
  EXPECT_EQ(world.SetInput(1, 3, 0.0f, nan), InputResult::kInvalid);
  EXPECT_EQ(world.SetInput(1, 4, inf, 0.0f), InputResult::kInvalid);
  EXPECT_EQ(world.SetInput(1, 5, 0.0f, -inf), InputResult::kInvalid);
  StepSeconds(world, 1.0f);
  EXPECT_TRUE(std::isfinite(world.Find(1)->x));
  EXPECT_NEAR(world.Find(1)->x, 2760.0f, 0.01f);
  EXPECT_EQ(world.Find(1)->last_input_seq, 1u);  // 버린 입력의 번호는 기록하지 않는다
}

TEST(WorldTest, StaleSequenceNumbersAreIgnored) {
  World world = WorldWithOne();
  ASSERT_EQ(world.SetInput(1, 10, 1.0f, 0.0f), InputResult::kApplied);
  EXPECT_EQ(world.SetInput(1, 10, 0.0f, 1.0f), InputResult::kStale);  // 같은 번호(중복)
  EXPECT_EQ(world.SetInput(1, 9, 0.0f, 1.0f), InputResult::kStale);   // 더 작은 번호
  EXPECT_EQ(world.SetInput(1, 11, -1.0f, 0.0f), InputResult::kApplied);
  EXPECT_EQ(world.Find(1)->last_input_seq, 11u);
  EXPECT_FLOAT_EQ(world.Find(1)->dir_x, -1.0f);
}

TEST(WorldTest, TheFirstInputIsAcceptedEvenWithSequenceZero) {
  World world = WorldWithOne();
  EXPECT_EQ(world.SetInput(1, 0, 1.0f, 0.0f), InputResult::kApplied);
  EXPECT_EQ(world.SetInput(1, 0, 1.0f, 0.0f), InputResult::kStale);
}

TEST(WorldTest, SequenceNumbersMayWrapAround) {
  World world = WorldWithOne();
  ASSERT_EQ(world.SetInput(1, 0xFFFFFFFFu, 1.0f, 0.0f), InputResult::kApplied);
  EXPECT_EQ(world.SetInput(1, 0u, 0.0f, 1.0f), InputResult::kApplied);  // 한 바퀴 돌아 0이 더 새 번호
  EXPECT_EQ(world.SetInput(1, 0xFFFFFFFFu, 1.0f, 0.0f), InputResult::kStale);
}

TEST(WorldTest, InputForAnUnknownPlayerIsRejected) {
  World world = WorldWithOne();
  EXPECT_EQ(world.SetInput(99, 1, 1.0f, 0.0f), InputResult::kUnknownPlayer);
}

TEST(WorldTest, RemovedPlayersAreGone) {
  World world(Config());
  world.AddPlayer(1, 0);
  world.AddPlayer(2, 1);
  world.RemovePlayer(1);
  EXPECT_EQ(world.Find(1), nullptr);
  ASSERT_NE(world.Find(2), nullptr);
  EXPECT_EQ(world.SetInput(1, 1, 1.0f, 0.0f), InputResult::kUnknownPlayer);
  world.RemovePlayer(1);  // 없는 사람을 또 지워도 안전하다
  EXPECT_EQ(world.players().size(), 1u);
}

TEST(WorldTest, EachStepAdvancesTheTickNumber) {
  World world = WorldWithOne();
  EXPECT_EQ(world.tick(), 0u);
  world.Step(kDt);
  world.Step(kDt);
  EXPECT_EQ(world.tick(), 2u);
}

TEST(WorldTest, SamePlayersAreIndependent) {
  World world(Config());
  world.AddPlayer(1, 0);
  world.AddPlayer(2, 1);
  ASSERT_EQ(world.SetInput(1, 1, 1.0f, 0.0f), InputResult::kApplied);
  StepSeconds(world, 1.0f);
  EXPECT_NEAR(world.Find(1)->x, 2760.0f, 0.01f);
  EXPECT_FLOAT_EQ(world.Find(2)->x, 2600.0f);  // 입력이 없는 사람은 제자리
}

// 고정 시간 간격이라 같은 입력 열이면 비트까지 같은 결과가 나와야 한다(결정적).
TEST(WorldTest, IsDeterministicForTheSameInputSequence) {
  auto run = [] {
    World world(Config());
    world.AddPlayer(1, 0);
    world.AddPlayer(2, 3);
    std::uint32_t seq = 0;
    for (int i = 0; i < 400; ++i) {
      if (i % 7 == 0) world.SetInput(1, ++seq, std::sin(i * 0.3f), std::cos(i * 0.2f));
      if (i % 11 == 0) world.SetInput(2, seq + 1, std::cos(i * 0.1f) * 2.0f, -std::sin(i * 0.4f));
      world.Step(kDt);
    }
    return std::make_pair(*world.Find(1), *world.Find(2));
  };
  const auto a = run();
  const auto b = run();
  EXPECT_EQ(a.first.x, b.first.x);
  EXPECT_EQ(a.first.y, b.first.y);
  EXPECT_EQ(a.second.x, b.second.x);
  EXPECT_EQ(a.second.y, b.second.y);
}

TEST(WorldTest, PositionsNeverLeaveTheMapEvenWithWildInputs) {
  World world = WorldWithOne();
  std::uint32_t seq = 0;
  for (int i = 0; i < 20000; ++i) {
    world.SetInput(1, ++seq, std::sin(i * 1.7f) * 50.0f, std::cos(i * 2.3f) * 50.0f);
    world.Step(kDt);
    const PlayerBody& body = *world.Find(1);
    ASSERT_GE(body.x, 40.0f);
    ASSERT_LE(body.x, 4960.0f);
    ASSERT_GE(body.y, 40.0f);
    ASSERT_LE(body.y, 4960.0f);
  }
}

}  // namespace
}  // namespace game::play
