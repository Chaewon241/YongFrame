#include "play/resources.h"

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

ResourceKind ResourceField::Hit(std::uint32_t node_id, std::uint64_t tick) {
  ResourceNode* node = FindMutable(node_id);
  if (node == nullptr) {
    return ResourceKind::kTree;
  }
  if (node->hp == 0) {
    return node->kind;  // Check를 거치지 않고 불렸다. 체력이 0 아래로 내려가지 않게 아무것도 하지 않는다.
  }
  --node->hp;
  if (node->hp == 0) {
    node->respawn_tick = tick + config_.respawn_ticks;
    ++depleted_count_;
  }
  MarkChanged(node_id);
  return node->kind;
}

std::uint32_t ResourceField::Advance(std::uint64_t tick) {
  if (depleted_count_ == 0) {
    return 0;
  }
  std::uint32_t respawned = 0;
  for (auto& node : nodes_) {
    if (node.hp == 0 && tick >= node.respawn_tick) {
      node.hp = node.max_hp;
      --depleted_count_;
      ++respawned;
      MarkChanged(node.id);
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
    changes.push_back(Change{id, nodes_[id - 1].hp});
  }
  changed_ids_.clear();
  return changes;
}

}  // namespace game::play
