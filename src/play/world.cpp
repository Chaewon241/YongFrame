#include "play/world.h"

#include <algorithm>
#include <cmath>

#include "play/resources.h"
#include "play/structures.h"

namespace game::play {

namespace {

// 칸(slot)마다 맵 중앙에서 얼마나 떨어진 곳에서 시작하는가(cm). 4칸이 겹치지 않는 간격이다.
struct SpawnOffset {
  float x;
  float y;
};
constexpr SpawnOffset kSpawnOffsets[] = {{-100.0f, -100.0f}, {100.0f, -100.0f}, {-100.0f, 100.0f}, {100.0f, 100.0f}};

// 시작 자리가 막혔을 때 빈 자리를 찾는 격자 간격과 최대 반복 횟수
constexpr double kSpawnSearchStep = 100.0;
constexpr int kSpawnSearchRings = 40;

// 장애물과 충돌할 때 한 틱 이동을 몇 번으로 나누어 접근하는가. 나눌수록 벽에 가깝게 멈춘다.
constexpr int kCollisionSubsteps = 3;

}  // namespace

World::World(const PlayConfig& config) : config_(config) {}

void World::CollectObstacles(double x, double y, double reach, std::vector<Obstacle>& out) const {
  const double r = config_.player_radius_cm;
  if (structures_ != nullptr) {
    for (const Structure& structure : structures_->structures()) {
      const Rect rect = structure.Footprint();
      if (std::abs(x - rect.cx) <= rect.hw + r + reach && std::abs(y - rect.cy) <= rect.hh + r + reach) {
        Obstacle obstacle;
        obstacle.is_rect = true;
        obstacle.rect = rect;
        out.push_back(obstacle);
      }
    }
  }
  if (resources_ != nullptr) {
    const double node_r = config_.node_collision_radius_cm;
    for (const ResourceNode& node : resources_->nodes()) {
      if (node.hp == 0) {
        continue;  // 고갈된 노드는 막지 않는다
      }
      if (std::abs(x - node.x) <= node_r + r + reach && std::abs(y - node.y) <= node_r + r + reach) {
        Obstacle obstacle;
        obstacle.x = node.x;
        obstacle.y = node.y;
        obstacle.radius = node_r;
        out.push_back(obstacle);
      }
    }
  }
}

bool World::Blocked(double x, double y, const std::vector<Obstacle>& obstacles) const {
  const double r = config_.player_radius_cm;
  for (const Obstacle& obstacle : obstacles) {
    if (obstacle.ignore) {
      continue;
    }
    const bool hit = obstacle.is_rect ? CircleOverlapsRect(x, y, r, obstacle.rect)
                                      : CirclesOverlap(x, y, r, obstacle.x, obstacle.y, obstacle.radius);
    if (hit) {
      return true;
    }
  }
  return false;
}

bool World::IsFree(float x, float y) const {
  const double r = config_.player_radius_cm;
  if (x < r || y < r || x > config_.map_width_cm - r || y > config_.map_height_cm - r) {
    return false;
  }
  std::vector<Obstacle> obstacles;
  CollectObstacles(x, y, 0.0, obstacles);
  return !Blocked(x, y, obstacles);
}

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

  // 그 자리에 구조물이나 노드가 있으면(늦게 입장했는데 중앙에 집을 지은 경우) 갇히지 않게 가까운 빈 자리로 옮긴다.
  if ((structures_ != nullptr || resources_ != nullptr) && !IsFree(body.x, body.y)) {
    bool found = false;
    for (int ring = 1; ring <= kSpawnSearchRings && !found; ++ring) {
      for (int dy = -ring; dy <= ring && !found; ++dy) {
        for (int dx = -ring; dx <= ring && !found; ++dx) {
          if (std::max(std::abs(dx), std::abs(dy)) != ring) {
            continue;  // 이 고리의 바깥 테두리만 본다
          }
          const float x = body.x + static_cast<float>(dx * kSpawnSearchStep);
          const float y = body.y + static_cast<float>(dy * kSpawnSearchStep);
          if (IsFree(x, y)) {
            body.x = x;
            body.y = y;
            found = true;
          }
        }
      }
    }
  }
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

    // 이번 이동으로 닿을 수 있는 장애물만 추린다. 대부분의 몸은 근처에 장애물이 없어 추가 비용이 없다.
    scratch_.clear();
    if (structures_ != nullptr || resources_ != nullptr) {
      CollectObstacles(body.x, body.y, static_cast<double>(step), scratch_);
    }
    if (scratch_.empty()) {
      body.x = std::clamp(body.x + body.dir_x * step, min_x, max_x);
      body.y = std::clamp(body.y + body.dir_y * step, min_y, max_y);
      continue;
    }

    // 이미 겹쳐 있는 장애물은 이번 이동에서 막지 않는다(빠져나올 수 있게).
    const double r = config_.player_radius_cm;
    for (Obstacle& obstacle : scratch_) {
      obstacle.ignore = obstacle.is_rect ? CircleOverlapsRect(body.x, body.y, r, obstacle.rect)
                                         : CirclesOverlap(body.x, body.y, r, obstacle.x, obstacle.y, obstacle.radius);
    }

    // 한 틱 이동을 여러 번으로 나누고, 각 번마다 x와 y를 따로 시도한다. 막히는 축만 멈추므로 벽을 따라 미끄러진다.
    const float sub_x = body.dir_x * step / static_cast<float>(kCollisionSubsteps);
    const float sub_y = body.dir_y * step / static_cast<float>(kCollisionSubsteps);
    for (int i = 0; i < kCollisionSubsteps; ++i) {
      const float try_x = std::clamp(body.x + sub_x, min_x, max_x);
      if (!Blocked(try_x, body.y, scratch_)) {
        body.x = try_x;
      }
      const float try_y = std::clamp(body.y + sub_y, min_y, max_y);
      if (!Blocked(body.x, try_y, scratch_)) {
        body.y = try_y;
      }
    }
  }
  ++tick_;
}

}  // namespace game::play
