#include "play/room.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace game::play {

Room::Room(std::string code, const PlayConfig& config)
    : code_(std::move(code)), config_(config), world_(config), resources_(config), structures_(config) {
  world_.AttachObstacles(&resources_, &structures_);
}

bool Room::HasNickname(std::string_view nickname) const {
  return std::any_of(members_.begin(), members_.end(),
                     [nickname](const Member& member) { return member.nickname == nickname; });
}

const Member* Room::FindByClient(std::uint64_t client_id) const {
  for (const auto& member : members_) {
    if (member.client_id == client_id) return &member;
  }
  return nullptr;
}

const Member* Room::FindByPlayer(std::uint32_t player_id) const {
  for (const auto& member : members_) {
    if (member.player_id == player_id) return &member;
  }
  return nullptr;
}

// 비어 있는 가장 작은 칸 번호. 누가 나가면 그 칸이 비므로 새로 들어온 사람이 이어서 쓴다.
std::uint32_t Room::LowestFreeSlot() const {
  for (std::uint32_t slot = 0;; ++slot) {
    const bool used = std::any_of(members_.begin(), members_.end(),
                                  [slot](const Member& member) { return member.slot == slot; });
    if (!used) return slot;
  }
}

std::uint32_t Room::AddMember(std::uint64_t client_id, std::string nickname) {
  Member member;
  member.client_id = client_id;
  member.player_id = next_player_id_++;
  member.nickname = std::move(nickname);
  member.slot = LowestFreeSlot();

  world_.AddPlayer(member.player_id, member.slot);
  if (members_.empty()) {
    host_player_id_ = member.player_id;  // 첫 사람이 방장
  }
  const std::uint32_t player_id = member.player_id;
  members_.push_back(std::move(member));
  return player_id;
}

Room::RemoveResult Room::RemoveMember(std::uint64_t client_id) {
  RemoveResult result;
  const auto it = std::find_if(members_.begin(), members_.end(),
                               [client_id](const Member& member) { return member.client_id == client_id; });
  if (it == members_.end()) {
    return result;
  }

  result.removed = true;
  result.member = *it;
  world_.RemovePlayer(it->player_id);
  members_.erase(it);

  if (result.member.player_id == host_player_id_) {
    if (members_.empty()) {
      host_player_id_ = 0;
    } else {
      host_player_id_ = members_.front().player_id;  // 먼저 들어온 사람
      result.host_changed = true;
    }
  }
  return result;
}

GatherOutcome Room::Gather(std::uint64_t client_id, std::uint32_t node_id) {
  GatherOutcome outcome;
  const auto it = std::find_if(members_.begin(), members_.end(),
                               [client_id](const Member& member) { return member.client_id == client_id; });
  if (it == members_.end()) {
    outcome.status = GatherStatus::kNotInRoom;
    return outcome;
  }
  Member& member = *it;
  outcome.wood = member.wood;
  outcome.stone = member.stone;

  const PlayerBody* body = world_.Find(member.player_id);
  if (body == nullptr) {
    outcome.status = GatherStatus::kNotInRoom;  // 일어나지 않아야 하는 불일치
    return outcome;
  }
  switch (resources_.Check(node_id, body->x, body->y)) {
    case NodeCheck::kNotFound:
      outcome.status = GatherStatus::kNodeNotFound;
      return outcome;
    case NodeCheck::kDepleted:
      outcome.status = GatherStatus::kNodeDepleted;
      return outcome;
    case NodeCheck::kTooFar:
      outcome.status = GatherStatus::kTooFar;
      return outcome;
    case NodeCheck::kOk:
      break;
  }

  // 쿨다운: 받아들여진 마지막 채집으로부터 gather_cooldown_ticks 틱이 지나야 한다(차이가 같으면 허용).
  const std::uint64_t tick = world_.tick();
  if (member.has_gathered && tick - member.last_gather_tick < config_.gather_cooldown_ticks) {
    outcome.status = GatherStatus::kCooldown;
    return outcome;
  }

  const ResourceKind kind = resources_.Hit(node_id, tick);
  member.has_gathered = true;
  member.last_gather_tick = tick;
  if (kind == ResourceKind::kTree) {
    ++member.wood;
  } else {
    ++member.stone;
  }
  outcome.status = GatherStatus::kOk;
  outcome.wood = member.wood;
  outcome.stone = member.stone;
  return outcome;
}

std::uint32_t Room::AdvanceResources(std::uint64_t tick) {
  // 재생 자리를 피할 것: 구조물 발자국(건설 중인 것도 포함), 플레이어 몸(노드 반지름을 더해 서 있는 자리에 나타나지 않게).
  SpotBlockers blockers;
  blockers.rects.reserve(structures_.structures().size());
  for (const Structure& structure : structures_.structures()) {
    blockers.rects.push_back(structure.Footprint());
  }
  const double player_reach = static_cast<double>(config_.player_radius_cm) + config_.node_collision_radius_cm;
  blockers.circles.reserve(world_.players().size());
  for (const PlayerBody& body : world_.players()) {
    blockers.circles.push_back(SpotBlockers::Circle{static_cast<double>(body.x), static_cast<double>(body.y), player_reach});
  }
  return resources_.Advance(tick, blockers);
}

bool Room::GrantResources(std::uint64_t client_id, std::uint32_t wood, std::uint32_t stone) {
  const auto it = std::find_if(members_.begin(), members_.end(),
                               [client_id](const Member& member) { return member.client_id == client_id; });
  if (it == members_.end()) {
    return false;
  }
  it->wood += wood;
  it->stone += stone;
  return true;
}

BuildOutcome Room::Build(std::uint64_t client_id, std::uint32_t structure_type, float x, float y) {
  BuildOutcome outcome;
  const auto it = std::find_if(members_.begin(), members_.end(),
                               [client_id](const Member& member) { return member.client_id == client_id; });
  if (it == members_.end()) {
    outcome.status = BuildStatus::kNotInRoom;
    return outcome;
  }
  Member& member = *it;
  outcome.wood = member.wood;
  outcome.stone = member.stone;

  const PlayerBody* body = world_.Find(member.player_id);
  if (body == nullptr) {
    outcome.status = BuildStatus::kNotInRoom;  // 일어나지 않아야 하는 불일치
    return outcome;
  }

  // 종류와 상한
  const StructureType type = StructureType::kHouse;
  if (structure_type != static_cast<std::uint32_t>(type)) {
    outcome.status = BuildStatus::kInvalidType;
    return outcome;
  }
  if (structures_.size() >= config_.max_structures) {
    outcome.status = BuildStatus::kLimit;
    return outcome;
  }

  // 좌표가 유한하지 않거나 맵보다 훨씬 크면 격자로 맞추기 전에 막는다(맵 밖이라 어차피 지을 수 없다).
  const double limit = 1.0e9;
  if (!std::isfinite(x) || !std::isfinite(y) || std::abs(static_cast<double>(x)) > limit ||
      std::abs(static_cast<double>(y)) > limit) {
    outcome.status = BuildStatus::kBlocked;
    return outcome;
  }

  // 격자 중심으로 맞춘다.
  float width = 0.0f;
  float height = 0.0f;
  structures_.FootprintOf(type, width, height);
  const double grid = config_.build_grid_cm > 0.0f ? static_cast<double>(config_.build_grid_cm) : 1.0;
  const double cx = std::round(static_cast<double>(x) / grid) * grid;
  const double cy = std::round(static_cast<double>(y) / grid) * grid;

  // 거리: 서버가 아는 내 위치에서 격자에 맞춘 중심까지(경계 포함)
  const double range = static_cast<double>(config_.build_range_cm);
  const double dx = static_cast<double>(body->x) - cx;
  const double dy = static_cast<double>(body->y) - cy;
  if (dx * dx + dy * dy > range * range) {
    outcome.status = BuildStatus::kTooFar;
    return outcome;
  }

  // 겹침: 맵 밖, 구조물, 고갈되지 않은 노드, 방 안의 모든 플레이어(몸 위에 지으면 갇힌다)
  const Rect footprint{cx, cy, static_cast<double>(width) * 0.5, static_cast<double>(height) * 0.5};
  // 건설 구역: 발자국 전체가 맵 중앙의 구역 안에 있어야 한다(경계에 닿는 것은 허용). 0이면 제한 없음.
  if (config_.build_zone_half_size_cm > 0.0f) {
    const double zone = static_cast<double>(config_.build_zone_half_size_cm);
    const double zone_x = static_cast<double>(config_.map_width_cm) * 0.5;
    const double zone_y = static_cast<double>(config_.map_height_cm) * 0.5;
    if (footprint.cx - footprint.hw < zone_x - zone || footprint.cx + footprint.hw > zone_x + zone ||
        footprint.cy - footprint.hh < zone_y - zone || footprint.cy + footprint.hh > zone_y + zone) {
      outcome.status = BuildStatus::kOutOfZone;
      return outcome;
    }
  }
  bool blocked = footprint.cx - footprint.hw < 0.0 || footprint.cy - footprint.hh < 0.0 ||
                 footprint.cx + footprint.hw > static_cast<double>(config_.map_width_cm) ||
                 footprint.cy + footprint.hh > static_cast<double>(config_.map_height_cm);
  for (const Structure& other : structures_.structures()) {
    blocked = blocked || RectsOverlap(footprint, other.Footprint());
  }
  for (const ResourceNode& node : resources_.nodes()) {
    blocked = blocked || (node.hp > 0 && CircleOverlapsRect(node.x, node.y, config_.node_collision_radius_cm, footprint));
  }
  for (const PlayerBody& player : world_.players()) {
    blocked = blocked || CircleOverlapsRect(player.x, player.y, config_.player_radius_cm, footprint);
  }
  if (blocked) {
    outcome.status = BuildStatus::kBlocked;
    return outcome;
  }

  // 비용
  if (member.wood < config_.house_cost_wood || member.stone < config_.house_cost_stone) {
    outcome.status = BuildStatus::kNotEnoughResources;
    return outcome;
  }

  member.wood -= config_.house_cost_wood;
  member.stone -= config_.house_cost_stone;
  outcome.structure_id = structures_.Add(type, static_cast<float>(cx), static_cast<float>(cy), world_.tick());
  outcome.status = BuildStatus::kOk;
  outcome.wood = member.wood;
  outcome.stone = member.stone;
  return outcome;
}

}  // namespace game::play
