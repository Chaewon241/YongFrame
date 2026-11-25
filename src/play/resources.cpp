#include "play/resources.h"

#include "play/landmark.h"

#include <algorithm>
#include <cmath>

namespace game::play {

namespace {

// 배치용 난수 생성기(splitmix64). 표준 라이브러리의 분포(uniform_real_distribution 등)는 구현마다 같은
// 시드에서 다른 값을 내므로 쓰지 않는다. 서버만 배치를 만들고 클라이언트는 받기만 하지만, 같은 설정이면
// 어느 빌드에서나 같은 배치가 나와야 시험과 재현이 쉽다.
class LayoutRandom {
 public:
  explicit LayoutRandom(std::uint64_t seed) : state_(seed) {}

  // [0, 1) 사이 값. 24비트만 써서 float로 정확히 표현된다.
  float Next01() {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z = z ^ (z >> 31);
    return static_cast<float>(z >> 40) / 16777216.0f;
  }

 private:
  std::uint64_t state_;
};

constexpr int kPlacementAttempts = 1000;  // 노드 하나를 놓을 자리를 찾는 최대 시도 횟수

// 노드 중심 (x, y)가 건설 구역(노드 충돌 반지름만큼 넓힌 정사각형) 안인가. 노드는 건설 구역 밖에만 둔다.
// 구역 제한이 없으면(0) 항상 false다. 정확히 경계에 닿는 것은 안이 아니다.
bool InsideBuildZone(const PlayConfig& config, float x, float y) {
  if (config.build_zone_half_size_cm <= 0.0f) {
    return false;
  }
  const double reach = static_cast<double>(config.build_zone_half_size_cm) + config.node_collision_radius_cm;
  return std::abs(static_cast<double>(x) - config.map_width_cm * 0.5) < reach &&
         std::abs(static_cast<double>(y) - config.map_height_cm * 0.5) < reach;
}

}  // namespace

ResourceField::ResourceField(const PlayConfig& config) : config_(config) {
  LayoutRandom random(config.layout_seed);
  const float margin = config.node_edge_margin_cm;
  const float span_x = config.map_width_cm - 2.0f * margin;
  const float span_y = config.map_height_cm - 2.0f * margin;
  const float center_x = config.map_width_cm * 0.5f;
  const float center_y = config.map_height_cm * 0.5f;
  if (span_x <= 0.0f || span_y <= 0.0f) {
    return;  // 맵이 너무 작아 둘 곳이 없다(노드 0개)
  }
  const std::vector<Landmark> landmarks = MakeLandmarks(config);

  const auto place = [&](ResourceKind kind, std::uint32_t count, std::uint32_t hp) {
    if (hp == 0) {
      return;  // 체력 0짜리 노드는 의미가 없다(고갈 상태로 시작하면 재생 계산이 어긋난다)
    }
    for (std::uint32_t i = 0; i < count; ++i) {
      for (int attempt = 0; attempt < kPlacementAttempts; ++attempt) {
        const float x = margin + random.Next01() * span_x;
        const float y = margin + random.Next01() * span_y;
        // 시작 위치 근처는 비운다.
        if (std::hypot(static_cast<double>(x - center_x), static_cast<double>(y - center_y)) <
            static_cast<double>(config.spawn_clear_radius_cm)) {
          continue;
        }
        // 건설 구역 안에는 두지 않는다(건물이 차지할 자리를 비워 둔다).
        if (InsideBuildZone(config, x, y)) {
          continue;
        }
        // 고정 물체(시작 집) 위에는 두지 않는다(기본값에서는 건설 구역 안이라 위 규칙으로 이미 걸러진다).
        const bool on_landmark = std::any_of(landmarks.begin(), landmarks.end(), [&](const Landmark& landmark) {
          return CircleOverlapsRect(x, y, config.node_collision_radius_cm, landmark.Footprint());
        });
        if (on_landmark) {
          continue;
        }
        // 다른 노드와 너무 가까우면 안 된다.
        const bool too_close = std::any_of(nodes_.begin(), nodes_.end(), [&](const ResourceNode& other) {
          return std::hypot(static_cast<double>(x - other.x), static_cast<double>(y - other.y)) <
                 static_cast<double>(config.node_min_gap_cm);
        });
        if (too_close) {
          continue;
        }
        ResourceNode node;
        node.id = static_cast<std::uint32_t>(nodes_.size()) + 1;
        node.kind = kind;
        node.x = x;
        node.y = y;
        node.hp = hp;
        node.max_hp = hp;
        nodes_.push_back(node);
        break;  // 자리를 못 찾으면 이 노드는 건너뛴다(노드 수가 설정보다 적어질 수 있다)
      }
    }
  };
  nodes_.reserve(static_cast<std::size_t>(config.tree_count) + config.rock_count);
  place(ResourceKind::kTree, config.tree_count, config.tree_hp);
  place(ResourceKind::kRock, config.rock_count, config.rock_hp);
}

const ResourceNode* ResourceField::Find(std::uint32_t node_id) const {
  if (node_id == 0 || node_id > nodes_.size()) {
    return nullptr;
  }
  return &nodes_[node_id - 1];
}

ResourceNode* ResourceField::FindMutable(std::uint32_t node_id) {
  if (node_id == 0 || node_id > nodes_.size()) {
    return nullptr;
  }
  return &nodes_[node_id - 1];
}

NodeCheck ResourceField::Check(std::uint32_t node_id, float x, float y) const {
  const ResourceNode* node = Find(node_id);
  if (node == nullptr) {
    return NodeCheck::kNotFound;
  }
  if (node->hp == 0) {
    return NodeCheck::kDepleted;
  }
  // 제곱 거리로 비교한다(double). 허용 거리와 정확히 같으면 허용한다.
  const double dx = static_cast<double>(x) - static_cast<double>(node->x);
  const double dy = static_cast<double>(y) - static_cast<double>(node->y);
  const double range = static_cast<double>(config_.gather_range_cm);
  if (dx * dx + dy * dy > range * range) {
    return NodeCheck::kTooFar;
  }
  return NodeCheck::kOk;
}

ResourceKind ResourceField::Hit(std::uint32_t node_id) {
  ResourceNode* node = FindMutable(node_id);
  if (node == nullptr) {
    return ResourceKind::kTree;
  }
  if (node->hp == 0) {
    return node->kind;  // Check를 거치지 않고 불렸다. 체력이 0 아래로 내려가지 않게 아무것도 하지 않는다.
  }
  --node->hp;
  if (node->hp == 0) {
    node->respawn_tick = kNoRetry;  // 밤 정산까지 고갈 상태로 남는다
    ++depleted_count_;
  }
  MarkChanged(node_id);
  return node->kind;
}

bool ResourceField::FindRespawnSpot(std::uint32_t node_id, std::uint64_t tick, const SpotBlockers& blockers, float& out_x,
                                    float& out_y) const {
  // 노드 번호와 틱으로 시드를 정한다. 같은 설정과 같은 틱이면 같은 자리가 나온다(시험이 결정적이다).
  LayoutRandom random(config_.layout_seed ^ (static_cast<std::uint64_t>(node_id) << 32) ^ tick);
  const float margin = config_.node_edge_margin_cm;
  const float span_x = config_.map_width_cm - 2.0f * margin;
  const float span_y = config_.map_height_cm - 2.0f * margin;
  const float center_x = config_.map_width_cm * 0.5f;
  const float center_y = config_.map_height_cm * 0.5f;
  if (span_x <= 0.0f || span_y <= 0.0f) {
    return false;
  }
  const double node_radius = static_cast<double>(config_.node_collision_radius_cm);

  for (int attempt = 0; attempt < kPlacementAttempts; ++attempt) {
    const float x = margin + random.Next01() * span_x;
    const float y = margin + random.Next01() * span_y;
    // 시작 위치 근처는 비운다(배치 때와 같은 규칙).
    if (std::hypot(static_cast<double>(x - center_x), static_cast<double>(y - center_y)) <
        static_cast<double>(config_.spawn_clear_radius_cm)) {
      continue;
    }
    // 건설 구역 안에는 나타나지 않는다(처음 배치와 같은 규칙).
    if (InsideBuildZone(config_, x, y)) {
      continue;
    }
    // 다른 노드와 너무 가까우면 안 된다(고갈된 노드의 옛 자리도 포함한다).
    const bool too_close = std::any_of(nodes_.begin(), nodes_.end(), [&](const ResourceNode& other) {
      return other.id != node_id &&
             std::hypot(static_cast<double>(x - other.x), static_cast<double>(y - other.y)) <
                 static_cast<double>(config_.node_min_gap_cm);
    });
    if (too_close) {
      continue;
    }
    // 구조물 발자국과 겹치면 안 된다(노드가 집 안에 들어가면 안 된다).
    const bool in_structure = std::any_of(blockers.rects.begin(), blockers.rects.end(), [&](const Rect& rect) {
      return CircleOverlapsRect(x, y, node_radius, rect);
    });
    if (in_structure) {
      continue;
    }
    // 플레이어 몸 위에 나타나면 안 된다(방금 서 있던 사람이 갇힌다).
    const bool on_player = std::any_of(blockers.circles.begin(), blockers.circles.end(), [&](const SpotBlockers::Circle& c) {
      const double dx = static_cast<double>(x) - c.x;
      const double dy = static_cast<double>(y) - c.y;
      return dx * dx + dy * dy < c.r * c.r;
    });
    if (on_player) {
      continue;
    }
    out_x = x;
    out_y = y;
    return true;
  }
  return false;
}

bool ResourceField::TryRespawn(ResourceNode& node, std::uint64_t tick, const SpotBlockers& blockers) {
  float x = 0.0f;
  float y = 0.0f;
  if (!FindRespawnSpot(node.id, tick, blockers, x, y)) {
    // 지금은 자리가 없다. 고갈 상태로 남고 respawn_retry_ticks 뒤에 다시 찾는다(매 틱 찾지 않는다).
    node.respawn_tick = tick + std::max<std::uint32_t>(1, config_.respawn_retry_ticks);
    return false;
  }
  node.x = x;
  node.y = y;
  node.hp = node.max_hp;
  node.respawn_tick = 0;
  --depleted_count_;
  MarkChanged(node.id);
  return true;
}

std::uint32_t ResourceField::RespawnAllDepleted(std::uint64_t tick, const SpotBlockers& blockers) {
  if (depleted_count_ == 0) {
    return 0;
  }
  std::uint32_t respawned = 0;
  retry_count_ = 0;
  for (auto& node : nodes_) {
    if (node.hp != 0) {
      continue;
    }
    if (TryRespawn(node, tick, blockers)) {
      ++respawned;
    } else {
      ++retry_count_;
    }
  }
  return respawned;
}

std::uint32_t ResourceField::Advance(std::uint64_t tick, const SpotBlockers& blockers) {
  if (retry_count_ == 0) {
    return 0;
  }
  std::uint32_t respawned = 0;
  for (auto& node : nodes_) {
    if (node.hp == 0 && node.respawn_tick != kNoRetry && tick >= node.respawn_tick) {
      if (TryRespawn(node, tick, blockers)) {
        --retry_count_;
        ++respawned;
      }
    }
  }
  return respawned;
}

void ResourceField::MarkChanged(std::uint32_t node_id) {
  if (std::find(changed_ids_.begin(), changed_ids_.end(), node_id) == changed_ids_.end()) {
    changed_ids_.push_back(node_id);
  }
}

std::vector<ResourceField::Change> ResourceField::TakeChanges() {
  std::sort(changed_ids_.begin(), changed_ids_.end());
  std::vector<Change> changes;
  changes.reserve(changed_ids_.size());
  for (const std::uint32_t id : changed_ids_) {
    changes.push_back(Change{id, nodes_[id - 1].hp, nodes_[id - 1].x, nodes_[id - 1].y});
  }
  changed_ids_.clear();
  return changes;
}

}  // namespace game::play
