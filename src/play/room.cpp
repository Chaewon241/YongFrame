#include "play/room.h"

#include <algorithm>
#include <utility>

namespace game::play {

Room::Room(std::string code, const PlayConfig& config)
    : code_(std::move(code)), config_(config), world_(config), resources_(config) {}

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

}  // namespace game::play
