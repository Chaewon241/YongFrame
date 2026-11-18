#include "play/game_service.h"

#include <algorithm>
#include <cstdio>
#include <random>
#include <sstream>
#include <utility>

#include "chat/chat_rules.h"
#include "server/log.h"

namespace game::play {

namespace {

using server::Log;
using server::LogLevel;

// 한 틱에서 밀린 틱을 한 번에 따라잡는 최대 횟수. 이보다 많이 밀리면 나머지는 건너뛴다.
constexpr std::uint32_t kMaxCatchUpSteps = 3;

// 숫자 6자리인가. 앞자리 0을 허용하므로 문자열로 검사한다.
bool IsValidRoomCode(const std::string& code) {
  return code.size() == kRoomCodeLength &&
         std::all_of(code.begin(), code.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// 기본 방 코드 생성기: 000000~999999 중 무작위 하나를 6자리 문자열로 만든다.
RoomCodeGenerator DefaultGenerator() {
  auto engine = std::make_shared<std::mt19937_64>(std::random_device{}());
  return [engine]() {
    std::uniform_int_distribution<std::uint32_t> distribution(0, 999999);
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%06u", distribution(*engine));
    return std::string(buffer);
  };
}

void AtomicMax(std::atomic<std::uint64_t>& target, std::uint64_t value) {
  std::uint64_t seen = target.load(std::memory_order_relaxed);
  while (value > seen && !target.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
  }
}

protocol::Structure MakeStructure(const Structure& structure) {
  protocol::Structure out;
  out.set_structure_id(structure.id);
  out.set_type(protocol::STRUCTURE_HOUSE);
  out.set_x(structure.x);
  out.set_y(structure.y);
  out.set_width(structure.width);
  out.set_height(structure.height);
  out.set_state(structure.state == StructureState::kComplete ? protocol::STRUCTURE_COMPLETE : protocol::STRUCTURE_BUILDING);
  out.set_ready_tick(structure.ready_tick);
  return out;
}

protocol::PlayerInfo MakePlayerInfo(const Member& member) {
  protocol::PlayerInfo info;
  info.set_player_id(member.player_id);
  info.set_nickname(member.nickname);
  return info;
}

}  // namespace

std::string GameStats::Summary() const {
  std::ostringstream out;
  out << "방=" << current_rooms.load() << " 플레이어=" << current_players.load()
      << " 방생성=" << rooms_created.load() << " 방삭제=" << rooms_closed.load()
      << " 입장=" << joins.load() << " 거부=" << requests_rejected.load()
      << " 입력반영=" << inputs_applied.load() << " 입력무시=" << inputs_ignored.load()
      << " 틱=" << ticks.load() << " 건너뜀=" << ticks_skipped.load()
      << " 스냅샷=" << snapshots_sent.load() << " 최대틱처리=" << max_tick_micros.load() << "us"
      << " 채집=" << gathers_ok.load() << " 채집거부=" << gathers_rejected.load()
      << " 고갈=" << nodes_depleted.load() << " 재생=" << nodes_respawned.load()
      << " 건설=" << builds_ok.load() << " 건설거부=" << builds_rejected.load()
      << " 완공=" << structures_completed.load();
  return out.str();
}

GameService::GameService(boost::asio::io_context& io, PlayConfig config, RoomCodeGenerator generator)
    : timer_(io),
      config_(config),
      generator_(generator ? std::move(generator) : DefaultGenerator()) {}

const Room* GameService::FindRoom(const std::string& code) const {
  const auto it = rooms_.find(code);
  return it == rooms_.end() ? nullptr : it->second.get();
}

std::string GameService::RoomCodeOf(std::uint64_t client_id) const {
  const auto it = connections_.find(client_id);
  return it == connections_.end() ? std::string() : it->second.room_code;
}

std::string GameService::NewRoomCode() {
  for (int attempt = 0; attempt < config_.room_code_attempts; ++attempt) {
    std::string code = generator_();
    // 생성기가 형식이 틀린 값을 주거나 이미 있는 코드를 주면 버리고 다시 뽑는다.
    if (IsValidRoomCode(code) && rooms_.find(code) == rooms_.end()) {
      return code;
    }
  }
  return {};
}

protocol::RoomState GameService::MakeRoomState(const Room& room, std::uint32_t your_player_id) const {
  protocol::RoomState state;
  state.set_room_code(room.code());
  state.set_your_player_id(your_player_id);
  state.set_host_player_id(room.host_player_id());
  for (const auto& member : room.members()) {
    *state.add_players() = MakePlayerInfo(member);
  }
  for (const auto& node : room.resources().nodes()) {
    protocol::ResourceNode* out = state.add_nodes();
    out->set_node_id(node.id);
    out->set_kind(node.kind == ResourceKind::kTree ? protocol::RESOURCE_TREE : protocol::RESOURCE_ROCK);
    out->set_x(node.x);
    out->set_y(node.y);
    out->set_hp(node.hp);
    out->set_max_hp(node.max_hp);
  }
  for (const Structure& structure : room.structures().structures()) {
    *state.add_structures() = MakeStructure(structure);
  }
  return state;
}

std::vector<std::shared_ptr<GameClient>> GameService::RecipientsOf(const Room& room,
                                                                    std::uint64_t except_client_id) const {
  std::vector<std::shared_ptr<GameClient>> targets;
  targets.reserve(room.members().size());
  for (const auto& member : room.members()) {
    if (member.client_id == except_client_id) continue;
    const auto it = connections_.find(member.client_id);
    if (it == connections_.end()) continue;
    if (auto client = it->second.client.lock()) {
      targets.push_back(std::move(client));
    }
  }
  return targets;
}

void GameService::DeliverAll(const std::vector<std::shared_ptr<GameClient>>& targets, protocol::MessageId id,
                             const google::protobuf::MessageLite& message) {
  for (const auto& target : targets) {
    target->Deliver(id, message);
  }
}

// ---------------------------------------------------------------- 방 만들기

void GameService::HandleCreateRoom(const std::shared_ptr<GameClient>& client,
                                   const protocol::CreateRoomRequest& request) {
  protocol::CreateRoomResponse response;
  const auto reject = [&](protocol::GameResult result) {
    ++stats_.requests_rejected;
    response.set_result(result);
    client->Deliver(protocol::MessageId::kCreateRoomResponse, response);
  };

  if (shutdown_) return reject(protocol::GAME_SERVER_BUSY);
  if (connections_.count(client->client_id()) != 0) return reject(protocol::GAME_ALREADY_IN_ROOM);
  if (chat::ValidateNickname(request.nickname()) != protocol::RESULT_OK) {
    return reject(protocol::GAME_INVALID_NICKNAME);
  }
  if (rooms_.size() >= config_.max_rooms) return reject(protocol::GAME_SERVER_BUSY);
  std::string code = NewRoomCode();
  if (code.empty()) return reject(protocol::GAME_SERVER_BUSY);

  auto room = std::make_unique<Room>(code, config_);
  const std::uint32_t player_id = room->AddMember(client->client_id(), request.nickname());
  response.set_result(protocol::GAME_OK);
  *response.mutable_state() = MakeRoomState(*room, player_id);

  rooms_.emplace(code, std::move(room));
  connections_[client->client_id()] = Connection{client, code};
  ++stats_.rooms_created;
  ++stats_.joins;
  ++stats_.current_rooms;
  ++stats_.current_players;
  Log(LogLevel::kInfo, "[방] 생성 코드=", code, " 방장=", client->client_id(), " 현재방=", rooms_.size());

  client->Deliver(protocol::MessageId::kCreateRoomResponse, response);
}

// ---------------------------------------------------------------- 입장

void GameService::HandleJoinRoom(const std::shared_ptr<GameClient>& client,
                                 const protocol::JoinRoomRequest& request) {
  protocol::JoinRoomResponse response;
  const auto reject = [&](protocol::GameResult result) {
    ++stats_.requests_rejected;
    response.set_result(result);
    client->Deliver(protocol::MessageId::kJoinRoomResponse, response);
  };

  // 확인 순서: 이미 방에 있음 -> 닉네임 규칙 -> 코드 형식 -> 방 존재 -> 가득 참 -> 닉네임 중복
  if (shutdown_) return reject(protocol::GAME_SERVER_BUSY);
  if (connections_.count(client->client_id()) != 0) return reject(protocol::GAME_ALREADY_IN_ROOM);
  if (chat::ValidateNickname(request.nickname()) != protocol::RESULT_OK) {
    return reject(protocol::GAME_INVALID_NICKNAME);
  }
  if (!IsValidRoomCode(request.room_code())) return reject(protocol::GAME_INVALID_ROOM_CODE);
  const auto it = rooms_.find(request.room_code());
  if (it == rooms_.end()) return reject(protocol::GAME_ROOM_NOT_FOUND);
  Room& room = *it->second;
  if (room.Full()) return reject(protocol::GAME_ROOM_FULL);
  if (room.HasNickname(request.nickname())) return reject(protocol::GAME_NICKNAME_TAKEN);

  const std::uint32_t player_id = room.AddMember(client->client_id(), request.nickname());
  connections_[client->client_id()] = Connection{client, room.code()};
  ++stats_.joins;
  ++stats_.current_players;

  // 보내기 전에 필요한 것을 모두 만들어 둔다. 보내는 도중에 연결이 닫혀 방이 바뀔 수 있기 때문이다.
  response.set_result(protocol::GAME_OK);
  *response.mutable_state() = MakeRoomState(room, player_id);
  protocol::RoomEvent event;
  event.set_kind(protocol::RoomEvent::PLAYER_JOINED);
  *event.mutable_player() = MakePlayerInfo(*room.FindByPlayer(player_id));
  event.set_host_player_id(room.host_player_id());
  const auto others = RecipientsOf(room, client->client_id());
  Log(LogLevel::kInfo, "[방] 입장 코드=", room.code(), " 인원=", room.members().size());

  client->Deliver(protocol::MessageId::kJoinRoomResponse, response);
  DeliverAll(others, protocol::MessageId::kRoomEvent, event);
}

// ---------------------------------------------------------------- 나가기

void GameService::HandleLeaveRoom(const std::shared_ptr<GameClient>& client) {
  protocol::LeaveRoomResponse response;
  if (connections_.count(client->client_id()) == 0) {
    ++stats_.requests_rejected;
    response.set_result(protocol::GAME_NOT_IN_ROOM);
    client->Deliver(protocol::MessageId::kLeaveRoomResponse, response);
    return;
  }
  RemoveFromRoom(client->client_id());
  response.set_result(protocol::GAME_OK);
  client->Deliver(protocol::MessageId::kLeaveRoomResponse, response);
}

void GameService::OnDisconnect(std::uint64_t client_id) { RemoveFromRoom(client_id); }

void GameService::RemoveFromRoom(std::uint64_t client_id) {
  const auto connection = connections_.find(client_id);
  if (connection == connections_.end()) {
    return;
  }
  const std::string code = connection->second.room_code;
  connections_.erase(connection);

  const auto it = rooms_.find(code);
  if (it == rooms_.end()) {
    return;  // 일어나지 않아야 하는 불일치. 연결 기록만 지우고 끝낸다.
  }
  Room& room = *it->second;
  const Room::RemoveResult result = room.RemoveMember(client_id);
  if (!result.removed) {
    return;
  }
  --stats_.current_players;

  if (room.Empty()) {
    rooms_.erase(it);
    ++stats_.rooms_closed;
    --stats_.current_rooms;
    Log(LogLevel::kInfo, "[방] 삭제(빈 방) 코드=", code, " 현재방=", rooms_.size());
    return;
  }
  if (shutdown_) {
    return;
  }

  // 남은 사람에게 알린다. 방 상태를 읽는 일은 여기서 끝내고, 보내는 도중에는 방을 만지지 않는다.
  const auto remaining = RecipientsOf(room, 0);
  protocol::RoomEvent left;
  left.set_kind(protocol::RoomEvent::PLAYER_LEFT);
  *left.mutable_player() = MakePlayerInfo(result.member);
  left.set_host_player_id(room.host_player_id());
  protocol::RoomEvent host;
  host.set_kind(protocol::RoomEvent::HOST_CHANGED);
  host.set_host_player_id(room.host_player_id());
  Log(LogLevel::kInfo, "[방] 퇴장 코드=", code, " 인원=", room.members().size(),
      result.host_changed ? " 방장변경" : "");

  DeliverAll(remaining, protocol::MessageId::kRoomEvent, left);
  if (result.host_changed) {
    DeliverAll(remaining, protocol::MessageId::kRoomEvent, host);
  }
}

// ---------------------------------------------------------------- 이동 입력

void GameService::HandleMoveInput(const std::shared_ptr<GameClient>& client, const protocol::MoveInput& input) {
  const auto connection = connections_.find(client->client_id());
  if (connection == connections_.end()) {
    ++stats_.inputs_ignored;  // 방에 없다. 방을 막 나간 직후에 도착한 입력일 수 있으므로 연결을 닫지 않는다.
    return;
  }
  const auto it = rooms_.find(connection->second.room_code);
  if (it == rooms_.end()) {
    ++stats_.inputs_ignored;
    return;
  }
  Room& room = *it->second;
  const Member* member = room.FindByClient(client->client_id());
  if (member == nullptr) {
    ++stats_.inputs_ignored;
    return;
  }

  const InputResult result = room.world().SetInput(member->player_id, input.sequence(), input.dir_x(), input.dir_y());
  if (result == InputResult::kApplied) {
    ++stats_.inputs_applied;
  } else {
    ++stats_.inputs_ignored;
  }
}

// ---------------------------------------------------------------- 자원 채집

void GameService::HandleGather(const std::shared_ptr<GameClient>& client, const protocol::GatherRequest& request) {
  protocol::GatherResponse response;
  response.set_node_id(request.node_id());

  GatherOutcome outcome;  // 기본은 방에 없음
  const auto connection = connections_.find(client->client_id());
  if (connection != connections_.end()) {
    const auto it = rooms_.find(connection->second.room_code);
    if (it != rooms_.end()) {
      Room& room = *it->second;
      const bool was_depleted = [&] {
        const ResourceNode* node = room.resources().Find(request.node_id());
        return node != nullptr && node->hp == 0;
      }();
      outcome = room.Gather(client->client_id(), request.node_id());
      if (outcome.status == GatherStatus::kOk) {
        const ResourceNode* node = room.resources().Find(request.node_id());
        if (!was_depleted && node != nullptr && node->hp == 0) {
          ++stats_.nodes_depleted;
        }
      }
    }
  }

  switch (outcome.status) {
    case GatherStatus::kOk:
      response.set_result(protocol::GAME_OK);
      ++stats_.gathers_ok;
      break;
    case GatherStatus::kNodeNotFound:
      response.set_result(protocol::GAME_NODE_NOT_FOUND);
      break;
    case GatherStatus::kNodeDepleted:
      response.set_result(protocol::GAME_NODE_DEPLETED);
      break;
    case GatherStatus::kTooFar:
      response.set_result(protocol::GAME_TOO_FAR);
      break;
    case GatherStatus::kCooldown:
      response.set_result(protocol::GAME_GATHER_COOLDOWN);
      break;
    case GatherStatus::kNotInRoom:
      response.set_result(protocol::GAME_NOT_IN_ROOM);
      break;
  }
  if (outcome.status != GatherStatus::kOk) {
    ++stats_.gathers_rejected;
  }
  response.set_wood(outcome.wood);
  response.set_stone(outcome.stone);
  client->Deliver(protocol::MessageId::kGatherResponse, response);
}

// ---------------------------------------------------------------- 건설

void GameService::HandleBuild(const std::shared_ptr<GameClient>& client, const protocol::BuildRequest& request) {
  protocol::BuildResponse response;

  BuildOutcome outcome;  // 기본은 방에 없음
  const auto connection = connections_.find(client->client_id());
  if (connection != connections_.end()) {
    const auto it = rooms_.find(connection->second.room_code);
    if (it != rooms_.end()) {
      outcome = it->second->Build(client->client_id(), static_cast<std::uint32_t>(request.structure_type()), request.x(),
                                  request.y());
    }
  }

  switch (outcome.status) {
    case BuildStatus::kOk:
      response.set_result(protocol::GAME_OK);
      response.set_structure_id(outcome.structure_id);
      ++stats_.builds_ok;
      break;
    case BuildStatus::kNotInRoom: response.set_result(protocol::GAME_NOT_IN_ROOM); break;
    case BuildStatus::kInvalidType: response.set_result(protocol::GAME_BUILD_INVALID_TYPE); break;
    case BuildStatus::kLimit: response.set_result(protocol::GAME_STRUCTURE_LIMIT); break;
    case BuildStatus::kTooFar: response.set_result(protocol::GAME_BUILD_TOO_FAR); break;
    case BuildStatus::kBlocked: response.set_result(protocol::GAME_BUILD_BLOCKED); break;
    case BuildStatus::kNotEnoughResources: response.set_result(protocol::GAME_NOT_ENOUGH_RESOURCES); break;
  }
  if (outcome.status != BuildStatus::kOk) {
    ++stats_.builds_rejected;
  }
  response.set_wood(outcome.wood);
  response.set_stone(outcome.stone);
  client->Deliver(protocol::MessageId::kBuildResponse, response);
}

// ---------------------------------------------------------------- 틱

void GameService::Tick(std::uint32_t steps) {
  if (shutdown_ || steps == 0) {
    return;
  }
  const auto started = std::chrono::steady_clock::now();
  const float dt = config_.TickSeconds();

  // 1) 계산: 모든 방의 위치를 steps번 계산하고, 방마다 마지막 상태의 스냅샷과 수신자 목록을 확보한다.
  struct Outgoing {
    std::vector<std::shared_ptr<GameClient>> targets;
    protocol::WorldSnapshot snapshot;
    protocol::ResourceUpdate resource_update;  // 바뀐 노드가 없으면 nodes가 비어 있고 보내지 않는다
    protocol::StructureUpdate structure_update;  // 바뀐 구조물이 없으면 structures가 비어 있고 보내지 않는다
  };
  std::vector<Outgoing> outgoing;
  outgoing.reserve(rooms_.size());
  for (auto& entry : rooms_) {
    Room& room = *entry.second;
    for (std::uint32_t i = 0; i < steps; ++i) {
      room.world().Step(dt);
    }
    // 재생할 노드를 되살린다(따라잡기 틱이 합쳐져도 한 번에 현재 틱 기준으로 처리한다).
    stats_.nodes_respawned += room.resources().Advance(room.world().tick());
    stats_.structures_completed += room.structures().Advance(room.world().tick());
    Outgoing out;
    out.targets = RecipientsOf(room, 0);
    if (out.targets.empty()) {
      continue;
    }
    for (const auto& change : room.resources().TakeChanges()) {
      protocol::ResourceNodeState* node = out.resource_update.add_nodes();
      node->set_node_id(change.node_id);
      node->set_hp(change.hp);
    }
    out.resource_update.set_tick(room.world().tick());
    for (const Structure& changed : room.structures().TakeChanges()) {
      *out.structure_update.add_structures() = MakeStructure(changed);
    }
    out.structure_update.set_tick(room.world().tick());
    out.snapshot.set_tick(room.world().tick());
    for (const auto& body : room.world().players()) {
      protocol::PlayerState* state = out.snapshot.add_players();
      state->set_player_id(body.id);
      state->set_x(body.x);
      state->set_y(body.y);
      state->set_last_input_seq(body.last_input_seq);
    }
    outgoing.push_back(std::move(out));
  }
  stats_.ticks += steps;

  // 2) 전송: 여기서부터는 방 컨테이너를 순회하지 않는다. 느린 수신자 때문에 연결이 닫혀 OnDisconnect가
  //    불려도(방이 지워질 수 있음) 위 단계에서 확보한 강한 참조만 쓰므로 안전하다.
  for (const auto& out : outgoing) {
    for (const auto& target : out.targets) {
      target->Deliver(protocol::MessageId::kWorldSnapshot, out.snapshot);
      ++stats_.snapshots_sent;
      if (out.resource_update.nodes_size() > 0) {
        target->Deliver(protocol::MessageId::kResourceUpdate, out.resource_update);
        ++stats_.resource_updates_sent;
      }
      if (out.structure_update.structures_size() > 0) {
        target->Deliver(protocol::MessageId::kStructureUpdate, out.structure_update);
        ++stats_.structure_updates_sent;
      }
    }
  }

  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started);
  AtomicMax(stats_.max_tick_micros, static_cast<std::uint64_t>(elapsed.count()));
}

void GameService::StartTicking() {
  if (ticking_ || shutdown_) {
    return;
  }
  ticking_ = true;
  const auto interval = std::chrono::microseconds(1'000'000 / std::max<std::uint32_t>(1, config_.tick_rate_hz));
  next_tick_ = std::chrono::steady_clock::now() + interval;
  ScheduleTick();
}

void GameService::ScheduleTick() {
  timer_.expires_at(next_tick_);
  timer_.async_wait([this](const boost::system::error_code& ec) { OnTickTimer(ec); });
}

void GameService::OnTickTimer(const boost::system::error_code& ec) {
  if (ec || shutdown_) {
    return;  // 타이머가 취소됨
  }
  const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::microseconds(1'000'000 / std::max<std::uint32_t>(1, config_.tick_rate_hz)));

  // 예약한 시각보다 얼마나 늦었는가에 따라 이번에 처리해야 할 틱 수를 정한다. 정상이면 1이다.
  // 많이 밀렸을 때 따라잡는 것은 kMaxCatchUpSteps까지만 하고 나머지는 건너뛴다(한꺼번에 몰려 더 밀리는 것을 막는다).
  const auto now = std::chrono::steady_clock::now();
  std::uint64_t due = 1;
  if (now > next_tick_) {
    due += static_cast<std::uint64_t>((now - next_tick_) / interval);
  }
  const std::uint32_t steps = static_cast<std::uint32_t>(std::min<std::uint64_t>(due, kMaxCatchUpSteps));
  if (due > steps) {
    stats_.ticks_skipped += due - steps;
  }

  Tick(steps);

  // 다음 시각은 건너뛴 틱까지 포함해 원래 격자에 맞춘다. 이렇게 하면 지연이 누적되지 않는다.
  next_tick_ += interval * static_cast<std::int64_t>(due);
  if (!shutdown_) {
    ScheduleTick();
  }
}

void GameService::Shutdown() {
  shutdown_ = true;
  timer_.cancel();
}

}  // namespace game::play
