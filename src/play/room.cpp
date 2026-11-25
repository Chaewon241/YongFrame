#include "play/room.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace game::play {

Room::Room(std::string code, const PlayConfig& config)
    : code_(std::move(code)),
      config_(config),
      landmarks_(MakeLandmarks(config)),
      world_(config),
      resources_(config),
      structures_(config) {
  if (!config_.bench_no_collision) {
    world_.AttachObstacles(&resources_, &structures_);
    std::vector<Rect> fixed;
    for (const Landmark& landmark : landmarks_) {
      fixed.push_back(landmark.Footprint());
    }
    world_.SetFixedObstacles(std::move(fixed));
  }

  // 부하 측정 전용: 완공된 집을 미리 놓는다(건설 구역 안, 서로 겹치지 않는 격자, 시작 위치를 피함).
  // 줄 y = 1450, 1850, 3150, 3550 (시작 위치와 사람이 돌아다니는 중앙 줄을 비운다), 칸 x = 1450 + 400 * i (i < 5).
  if (config_.bench_prebuilt_structures > 0) {
    const float rows[] = {1450.0f, 1850.0f, 3150.0f, 3550.0f};
    std::uint32_t placed = 0;
    for (const float y : rows) {
      for (int i = 0; i < 5 && placed < config_.bench_prebuilt_structures && placed < 20; ++i, ++placed) {
        structures_.Add(StructureType::kHouse, 1450.0f + 400.0f * static_cast<float>(i), y);
      }
    }
    structures_.CompleteAll(0);  // 모두 완공 상태로
    structures_.TakeChanges();                 // 입장 알림에는 방 상태로 실리므로 틱 알림은 필요 없다
  }
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

bool Room::RestoreStructure(std::uint32_t structure_id, std::uint32_t type, float x, float y, bool complete) {
  if (type != static_cast<std::uint32_t>(StructureType::kHouse)) {
    return false;
  }
  const std::uint32_t id = structures_.Restore(static_cast<StructureType>(type), x, y,
                                               complete ? StructureState::kComplete : StructureState::kBuilding,
                                               world_.tick());
  return id != 0 && id == structure_id;
}

bool Room::SetInventory(std::uint64_t client_id, std::uint32_t wood, std::uint32_t stone) {
  for (Member& member : members_) {
    if (member.client_id == client_id) {
      member.wood = wood;
      member.stone = stone;
      return true;
    }
  }
  return false;
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
  if (member.sleeping) {
    outcome.status = GatherStatus::kSleeping;
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

  const ResourceKind kind = resources_.Hit(node_id);
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

SpotBlockers Room::MakeSpotBlockers() const {
  // 재생 자리를 피할 것: 구조물 발자국(건설 중인 것도 포함), 고정 물체(시작 집), 플레이어 몸(노드 반지름을 더해 서 있는
  // 자리에 나타나지 않게).
  SpotBlockers blockers;
  blockers.rects.reserve(structures_.structures().size() + landmarks_.size());
  for (const Structure& structure : structures_.structures()) {
    blockers.rects.push_back(structure.Footprint());
  }
  for (const Landmark& landmark : landmarks_) {
    blockers.rects.push_back(landmark.Footprint());
  }
  const double player_reach = static_cast<double>(config_.player_radius_cm) + config_.node_collision_radius_cm;
  blockers.circles.reserve(world_.players().size());
  for (const PlayerBody& body : world_.players()) {
    blockers.circles.push_back(SpotBlockers::Circle{static_cast<double>(body.x), static_cast<double>(body.y), player_reach});
  }
  return blockers;
}

std::uint32_t Room::AdvanceResources(std::uint64_t tick) {
  if (resources_.depleted_count() == 0) {
    return 0;  // 고갈된 노드가 없으면 막을 것을 모을 필요도 없다(대부분의 틱)
  }
  return resources_.Advance(tick, MakeSpotBlockers());
}

Member* Room::FindMutableByClient(std::uint64_t client_id) {
  for (auto& member : members_) {
    if (member.client_id == client_id) return &member;
  }
  return nullptr;
}

InputResult Room::ApplyInput(std::uint64_t client_id, std::uint32_t sequence, float dir_x, float dir_y) {
  const Member* member = FindByClient(client_id);
  if (member == nullptr) {
    return InputResult::kUnknownPlayer;
  }
  if (member->sleeping) {
    return InputResult::kSleeping;  // 잠든 동안의 입력은 통째로 무시한다(입력 번호도 올리지 않는다)
  }
  return world_.SetInput(member->player_id, sequence, dir_x, dir_y);
}

// ---------------------------------------------------------------- 하루와 잠 (docs/content-day.md)

bool Room::CanSleepAt(float x, float y) const {
  const double range = static_cast<double>(config_.sleep_range_cm);
  const auto near = [&](const Rect& rect) {
    // 점에서 직사각형까지의 최단 거리(안에 있으면 0). 경계 포함.
    const double dx = std::max(std::abs(static_cast<double>(x) - rect.cx) - rect.hw, 0.0);
    const double dy = std::max(std::abs(static_cast<double>(y) - rect.cy) - rect.hh, 0.0);
    return dx * dx + dy * dy <= range * range;
  };
  for (const Landmark& landmark : landmarks_) {
    if (landmark.kind == LandmarkKind::kFarmhouse && near(landmark.Footprint())) {
      return true;
    }
  }
  for (const Structure& structure : structures_.structures()) {
    if (structure.state == StructureState::kComplete && near(structure.Footprint())) {
      return true;  // 건설 중인 집 앞에서는 잘 수 없다
    }
  }
  return false;
}

SleepOutcome Room::Sleep(std::uint64_t client_id, bool sleep) {
  SleepOutcome outcome;
  Member* member = FindMutableByClient(client_id);
  if (member == nullptr) {
    outcome.status = SleepStatus::kNotInRoom;
    return outcome;
  }
  outcome.sleeping = member->sleeping;
  if (member->sleeping == sleep) {
    outcome.status = SleepStatus::kOk;  // 이미 그 상태다. 성공으로 답하고 아무것도 바꾸지 않는다
    return outcome;
  }
  if (sleep) {
    const PlayerBody* body = world_.Find(member->player_id);
    if (body == nullptr || !CanSleepAt(body->x, body->y)) {
      outcome.status = SleepStatus::kTooFar;
      return outcome;
    }
    world_.Stop(member->player_id);
  }
  member->sleeping = sleep;
  outcome.status = SleepStatus::kOk;
  outcome.sleeping = sleep;
  outcome.changed = true;
  return outcome;
}

bool Room::IsSleeping(std::uint64_t client_id) const {
  const Member* member = FindByClient(client_id);
  return member != nullptr && member->sleeping;
}

std::vector<std::uint32_t> Room::SleepingPlayerIds() const {
  std::vector<std::uint32_t> ids;
  for (const Member& member : members_) {
    if (member.sleeping) ids.push_back(member.player_id);
  }
  return ids;
}

bool Room::TakeSleepChange(std::vector<std::uint32_t>& out) {
  std::vector<std::uint32_t> now = SleepingPlayerIds();
  if (now == reported_sleepers_) {
    return false;
  }
  reported_sleepers_ = now;
  out = std::move(now);
  return true;
}

std::uint64_t Room::DayElapsed() const {
  const std::int64_t elapsed = static_cast<std::int64_t>(world_.tick()) - day_start_tick_;
  return elapsed < 0 ? 0 : static_cast<std::uint64_t>(elapsed);
}

std::uint32_t Room::GameMinute() const {
  const std::uint64_t per_minute = config_.ticks_per_game_minute == 0 ? 1 : config_.ticks_per_game_minute;
  return config_.day_start_minute + static_cast<std::uint32_t>(DayElapsed() / per_minute);
}

NightTrigger Room::ShouldEndDay(std::uint64_t tick) const {
  if (!members_.empty() &&
      std::all_of(members_.begin(), members_.end(), [](const Member& member) { return member.sleeping; })) {
    return NightTrigger::kAllAsleep;
  }
  const std::int64_t elapsed = static_cast<std::int64_t>(tick) - day_start_tick_;
  if (elapsed >= 0 && static_cast<std::uint64_t>(elapsed) >= config_.DayLengthTicks()) {
    return NightTrigger::kForced;
  }
  return NightTrigger::kNone;
}

NightReport Room::RunNight(std::uint64_t tick, bool forced) {
  NightReport report;
  report.forced = forced;
  if (forced) {
    // 02:00: 깨어 있는 사람도 그 자리에서 강제로 잠든다(벌칙 없음). 아래 3)에서 모두 깨어나므로 밖에서 보이는 차이는 없다.
    for (Member& member : members_) member.sleeping = true;
  }

  // 1) 완공: 건설 중인 모든 집
  report.completed = structures_.CompleteAll(tick);

  // 2) (조각 3~5 자리) 작물 성장, 스프링클러 물 주기, 물 마름, 출하 판매. docs/content-day.md 5.4.
  //    이 조각에서는 비워 둔다.

  // 3) 깨우기와 아침 위치: 재생(4)보다 먼저 한다. 재생 자리가 플레이어 몸을 피하는데 그 기준이 아침 위치여야 한다.
  //    입력 번호(last_input_seq)는 그대로 둔다.
  for (Member& member : members_) {
    member.sleeping = false;
    world_.MoveToSpawn(member.player_id, member.slot);
  }

  // 4) 재생: 고갈된 모든 노드. 자리를 못 찾은 노드는 낮 동안 respawn_retry_ticks마다 다시 찾는다.
  report.respawned = resources_.RespawnAllDepleted(tick, MakeSpotBlockers());

  // 5) 날 넘김
  ++day_;
  day_start_tick_ = static_cast<std::int64_t>(tick);
  report.day = day_;
  // 6) 알림과 저장은 GameService가 한다.
  return report;
}

bool Room::RestoreClock(std::uint32_t day, std::uint64_t elapsed_ticks) {
  const std::uint64_t length = config_.DayLengthTicks();
  bool clamped = false;
  if (elapsed_ticks > length) {
    elapsed_ticks = length;  // 설정이 바뀌었거나 값이 잘못되었다. 다음 틱에 강제 정산된다
    clamped = true;
  }
  day_ = day == 0 ? 1 : day;
  day_start_tick_ = static_cast<std::int64_t>(world_.tick()) - static_cast<std::int64_t>(elapsed_ticks);
  return clamped;
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
  if (member.sleeping) {
    outcome.status = BuildStatus::kSleeping;
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
  for (const Landmark& landmark : landmarks_) {
    blocked = blocked || RectsOverlap(footprint, landmark.Footprint());  // 시작 집 위에는 지을 수 없다
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
  outcome.structure_id = structures_.Add(type, static_cast<float>(cx), static_cast<float>(cy));
  outcome.status = BuildStatus::kOk;
  outcome.wood = member.wood;
  outcome.stone = member.stone;
  return outcome;
}

}  // namespace game::play
