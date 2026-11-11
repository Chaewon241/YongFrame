#include "play/world.h"

#include <algorithm>
#include <cmath>

namespace game::play {

namespace {

// 칸(slot)마다 맵 중앙에서 얼마나 떨어진 곳에서 시작하는가(cm). 4칸이 겹치지 않는 간격이다.
struct SpawnOffset {
  float x;
  float y;
};
constexpr SpawnOffset kSpawnOffsets[] = {{-100.0f, -100.0f}, {100.0f, -100.0f}, {-100.0f, 100.0f}, {100.0f, 100.0f}};

}  // namespace

World::World(const PlayConfig& config) : config_(config) {}

void World::AddPlayer(std::uint32_t id, std::uint32_t slot) {
  if (Find(id) != nullptr) {
    return;
  }
  PlayerBody body;
  body.id = id;
  // 맵 중앙을 기준으로, 칸 번호에 맞는 오프셋만큼 떨어뜨려 시작한다. 4칸을 넘는 번호는 중앙에서 시작한다.
  const std::size_t count = sizeof(kSpawnOffsets) / sizeof(kSpawnOffsets[0]);
  const SpawnOffset offset = slot < count ? kSpawnOffsets[slot] : SpawnOffset{0.0f, 0.0f};
  body.x = config_.map_width_cm * 0.5f + offset.x;
  body.y = config_.map_height_cm * 0.5f + offset.y;
  players_.push_back(body);
}

void World::RemovePlayer(std::uint32_t id) {
  players_.erase(std::remove_if(players_.begin(), players_.end(),
                                [id](const PlayerBody& body) { return body.id == id; }),
                 players_.end());
}

PlayerBody* World::FindMutable(std::uint32_t id) {
  for (auto& body : players_) {
    if (body.id == id) return &body;
  }
  return nullptr;
}

const PlayerBody* World::Find(std::uint32_t id) const {
  for (const auto& body : players_) {
    if (body.id == id) return &body;
  }
  return nullptr;
}

InputResult World::SetInput(std::uint32_t id, std::uint32_t sequence, float dir_x, float dir_y) {
  PlayerBody* body = FindMutable(id);
  if (body == nullptr) {
    return InputResult::kUnknownPlayer;
  }
  // NaN이나 무한대는 위치 계산을 망가뜨리므로 입력 전체를 버린다.
  if (!std::isfinite(dir_x) || !std::isfinite(dir_y)) {
    return InputResult::kInvalid;
  }
  // 번호가 이전 값 이하이면(순서가 거꾸로이거나 중복) 버린다. 32비트가 한 바퀴 돌아도 되도록 차이의 부호로 비교한다.
  if (body->has_input && static_cast<std::int32_t>(sequence - body->last_input_seq) <= 0) {
    return InputResult::kStale;
  }

  // 방향 벡터의 길이가 1을 넘으면 1로 줄인다. 클라이언트가 (10, 10)을 보내도 더 빠르게 움직일 수 없다.
  // 길이는 double로 hypot으로 구한다. float로 제곱하면 1e30 같은 값에서 넘쳐 무한대가 되고, 그러면 나눗셈
  // 결과가 0이나 NaN이 되어 이동이 멈추거나 위치가 망가진다.
  const double length = std::hypot(static_cast<double>(dir_x), static_cast<double>(dir_y));
  if (length > 1.0) {
    dir_x = static_cast<float>(dir_x / length);
    dir_y = static_cast<float>(dir_y / length);
  }

  body->dir_x = dir_x;
  body->dir_y = dir_y;
  body->last_input_seq = sequence;
  body->has_input = true;
  return InputResult::kApplied;
}

void World::Step(float dt_seconds) {
  const float step = config_.move_speed_cm_per_s * dt_seconds;
  // 몸 반지름만큼 안쪽까지만 갈 수 있다. 맵이 몸보다 작은 비정상 설정이면 중앙에 고정한다.
  const float min_x = config_.player_radius_cm;
  const float max_x = std::max(min_x, config_.map_width_cm - config_.player_radius_cm);
  const float min_y = config_.player_radius_cm;
  const float max_y = std::max(min_y, config_.map_height_cm - config_.player_radius_cm);

  for (auto& body : players_) {
    if (body.dir_x == 0.0f && body.dir_y == 0.0f) {
      continue;
    }
    body.x = std::clamp(body.x + body.dir_x * step, min_x, max_x);
    body.y = std::clamp(body.y + body.dir_y * step, min_y, max_y);
  }
  ++tick_;
}

}  // namespace game::play
