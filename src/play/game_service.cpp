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
      << " 완공=" << structures_completed.load()
      << " 농장열림=" << farms_opened.load() << " 농장열기실패=" << farm_open_failures.load()
      << " 틱시간{이동=" << tick_us_step.load() << " 갱신=" << tick_us_update.load() << " 구성=" << tick_us_build.load()
      << " 전송=" << tick_us_deliver.load() << " 합=" << tick_us_total.load() << "}";
  return out.str();
}

GameService::GameService(boost::asio::io_context& io, PlayConfig config, RoomCodeGenerator generator)
    : timer_(io),
      config_(config),
      generator_(generator ? std::move(generator) : DefaultGenerator()) {}

void GameService::SetFarmRepository(std::shared_ptr<farm::FarmRepository> repository) {
  farm_repo_ = std::move(repository);
  persistence_ = farm_repo_ ? FarmPersistence::Create(farm_repo_, config_.farm_flush_interval_ticks) : nullptr;
}

void GameService::LoadFarmCodes() {
  if (!farm_repo_) {
    return;
  }
  farm_repo_->LoadFarmCodes([this, alive = std::weak_ptr<bool>(alive_)](farm::FarmResult<std::vector<std::string>> result) {
    if (alive.expired()) {
      return;
    }
    if (!result.ok) {
      Log(LogLevel::kError, "[농장] 농장 코드를 읽지 못함(게스트 방 코드가 농장과 겹칠 수 있음): ", result.message);
      return;
    }
    for (auto& code : result.value) {
      reserved_codes_.insert(std::move(code));
    }
    Log(LogLevel::kInfo, "[농장] 농장 코드 ", reserved_codes_.size(), "개를 예약");
  });
}

std::string GameService::FarmSummary() const { return persistence_ ? persistence_->stats().Summary() : std::string(); }

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
    if (IsValidRoomCode(code) && rooms_.find(code) == rooms_.end() && reserved_codes_.count(code) == 0) {
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
  // 로그인한 연결의 방 만들기는 "내 농장을 연다"이다(요청의 닉네임은 무시하고 계정의 ID를 닉네임으로 쓴다).
  // 게스트(로그인하지 않은 연결)는 지금까지처럼 저장하지 않는 임시 방을 만든다.
  if (farm_repo_ && hooks_.login_of) {
    if (auto login = hooks_.login_of(client->client_id())) {
      OpenFarm(client, *login);
      return;
    }
  }
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
                                 const protocol::JoinRoomRequest& original_request) {
  // 로그인한 연결은 요청의 닉네임을 무시하고 계정의 ID를 닉네임으로 쓴다. 농장 저장소가 없는 서버에서는 로그인 여부를
  // 보지 않는다(모든 방이 임시 방이다).
  std::optional<account::LoginInfo> login;
  if (farm_repo_ && hooks_.login_of) {
    login = hooks_.login_of(client->client_id());
  }
  protocol::JoinRoomRequest request = original_request;
  if (login) {
    request.set_nickname(login->login_name);
  }
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
  // 농장 방에는 로그인한 사람만, 임시 방에는 로그인하지 않은 사람만 들어간다(두 종류가 섞이지 않는다).
  if (room.is_farm() && !login) return reject(protocol::GAME_LOGIN_REQUIRED);
  if (!room.is_farm() && login) return reject(protocol::GAME_ROOM_NOT_FOUND);
  if (room.Full()) return reject(protocol::GAME_ROOM_FULL);
  if (room.HasNickname(request.nickname())) return reject(protocol::GAME_NICKNAME_TAKEN);

  const std::uint32_t player_id = room.AddMember(client->client_id(), request.nickname());
  if (room.is_farm()) {
    // 이 농장에서 예전에 모은 인벤토리를 되살린다(농장을 열 때 읽어 둔 값).
    const auto runtime = farms_.find(room.code());
    if (runtime != farms_.end()) {
      const auto saved = runtime->second.players.find(login->account_id);
      if (saved != runtime->second.players.end()) {
        room.SetInventory(client->client_id(), saved->second.wood, saved->second.stone);
      }
    }
  }
  connections_[client->client_id()] = Connection{client, room.code(), login ? login->account_id : 0};
  ++stats_.joins;
  ++stats_.current_players;

  // 보내기 전에 필요한 것을 모두 만들어 둔다. 보내는 도중에 연결이 닫혀 방이 바뀔 수 있기 때문이다.
  response.set_result(protocol::GAME_OK);
  *response.mutable_state() = MakeRoomState(room, player_id);
  if (const Member* me = room.FindByPlayer(player_id)) {  // 복원한 인벤토리를 알려 준다(HUD가 0으로 보이지 않게)
    response.set_wood(me->wood);
    response.set_stone(me->stone);
  }
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

void GameService::OnDisconnect(std::uint64_t client_id) {
  opening_.erase(client_id);  // 농장을 여는 중에 끊겼다면 저장소 응답이 와도 방을 만들지 않는다
  RemoveFromRoom(client_id);
}

void GameService::RemoveFromRoom(std::uint64_t client_id) {
  const auto connection = connections_.find(client_id);
  if (connection == connections_.end()) {
    return;
  }
  const std::string code = connection->second.room_code;
  const std::uint64_t account_id = connection->second.account_id;
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

  // 농장이면 나가는 사람의 인벤토리와 누적 기록을 저장한다(방이 비면 구조물 상태도). 이 호출은 연결의 계정 정보가 아직
  // 살아 있을 때(Session::Close에서 계정 서비스보다 먼저) 이루어진다.
  if (room.is_farm()) {
    PersistLeaver(room, result.member, account_id);
    if (room.Empty()) {
      farms_.erase(code);  // 농장이 닫혔다. 코드는 예약된 채로 남는다(reserved_codes_)
    }
  }

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
      const Member* before = room.FindByClient(client->client_id());
      const std::uint32_t wood_before = before != nullptr ? before->wood : 0;
      const std::uint32_t stone_before = before != nullptr ? before->stone : 0;
      outcome = room.Gather(client->client_id(), request.node_id());
      if (outcome.status == GatherStatus::kOk) {
        const ResourceNode* node = room.resources().Find(request.node_id());
        if (!was_depleted && node != nullptr && node->hp == 0) {
          ++stats_.nodes_depleted;
        }
        // 농장이면 얻은 만큼을 누적 기록에 더하고 저장 대기열에 알린다(채집은 자주 일어나므로 모아서 저장한다).
        account::AccountStats gained;
        gained.wood_gathered = outcome.wood - wood_before;
        gained.stone_gathered = outcome.stone - stone_before;
        NoteProgress(room, client->client_id(), gained, /*immediate=*/false);
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
      Room& room = *it->second;
      outcome = room.Build(client->client_id(), static_cast<std::uint32_t>(request.structure_type()), request.x(),
                           request.y());
      if (outcome.status == BuildStatus::kOk && room.is_farm() && persistence_) {
        // 건설은 드문 사건이라 바로 저장한다: 새 구조물, 줄어든 인벤토리, 늘어난 집 수를 한 번에.
        const auto runtime = farms_.find(room.code());
        if (runtime != farms_.end()) {
          runtime->second.builders[outcome.structure_id] = connection->second.account_id;
        }
        persistence_->UpdateStructures(room.farm_id(), ExportStructures(room));
        account::AccountStats built;
        built.houses_built = 1;
        NoteProgress(room, client->client_id(), built, /*immediate=*/true);
      }
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
    case BuildStatus::kOutOfZone: response.set_result(protocol::GAME_BUILD_OUT_OF_ZONE); break;
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
  using Clock = std::chrono::steady_clock;
  std::chrono::nanoseconds step_ns{0};
  std::chrono::nanoseconds update_ns{0};
  std::chrono::nanoseconds build_ns{0};
  for (auto& entry : rooms_) {
    Room& room = *entry.second;
    const auto t0 = Clock::now();
    for (std::uint32_t i = 0; i < steps; ++i) {
      room.world().Step(dt);
    }
    const auto t1 = Clock::now();
    // 재생할 노드를 되살린다(따라잡기 틱이 합쳐져도 한 번에 현재 틱 기준으로 처리한다).
    stats_.nodes_respawned += room.AdvanceResources(room.world().tick());
    const std::uint32_t completed = room.structures().Advance(room.world().tick());
    stats_.structures_completed += completed;
    if (completed > 0 && room.is_farm() && persistence_) {
      // 완공은 드문 사건이라 바로 저장한다(건설 중이던 구조물이 완료 상태가 되었다).
      persistence_->UpdateStructures(room.farm_id(), ExportStructures(room));
      persistence_->Flush(room.farm_id());
    }
    const auto t2 = Clock::now();
    step_ns += t1 - t0;
    update_ns += t2 - t1;
    Outgoing out;
    out.targets = RecipientsOf(room, 0);
    if (out.targets.empty()) {
      build_ns += Clock::now() - t2;
      continue;
    }
    for (const auto& change : room.resources().TakeChanges()) {
      protocol::ResourceNodeState* node = out.resource_update.add_nodes();
      node->set_node_id(change.node_id);
      node->set_hp(change.hp);
      node->set_x(change.x);
      node->set_y(change.y);
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
    build_ns += Clock::now() - t2;
  }
  stats_.ticks += steps;
  if (persistence_) {
    persistence_->OnTick(steps);  // 주기가 되면 모아 둔 채집 변경을 저장한다(저장소 호출은 비동기라 틱을 막지 않는다)
  }
  const auto deliver_start = Clock::now();

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

  const auto finished = Clock::now();
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(finished - started);
  AtomicMax(stats_.max_tick_micros, static_cast<std::uint64_t>(elapsed.count()));
  stats_.tick_us_step += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(step_ns).count());
  stats_.tick_us_update += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(update_ns).count());
  stats_.tick_us_build += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(build_ns).count());
  stats_.tick_us_deliver += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(finished - deliver_start).count());
  stats_.tick_us_total += static_cast<std::uint64_t>(elapsed.count());
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
  // 서버가 끝나기 전에 열려 있는 농장을 모두 저장한다: 구조물(건설 중이면 남은 틱)과 방에 있는 사람들의 인벤토리,
  // 누적 기록. 저장소 호출은 비동기라 io_context가 끝나기 전에 마무리된다(저장소의 제한 시간 안에서).
  if (persistence_) {
    for (auto& entry : rooms_) {
      Room& room = *entry.second;
      if (!room.is_farm()) continue;
      persistence_->UpdateStructures(room.farm_id(), ExportStructures(room));
      for (const Member& member : room.members()) {
        const auto connection = connections_.find(member.client_id);
        if (connection == connections_.end() || connection->second.account_id == 0) continue;
        const farm::StoredPlayer player{connection->second.account_id, member.wood, member.stone};
        persistence_->UpdatePlayer(room.farm_id(), player,
                                   hooks_.stats_of ? hooks_.stats_of(member.client_id) : std::nullopt);
      }
    }
    persistence_->FlushAll();
  }
  shutdown_ = true;
  timer_.cancel();
}

// ---------------------------------------------------------------- 농장 (docs/persistence.md)

void GameService::RejectOpen(const std::shared_ptr<GameClient>& client, protocol::GameResult result) {
  ++stats_.requests_rejected;
  protocol::CreateRoomResponse response;
  response.set_result(result);
  client->Deliver(protocol::MessageId::kCreateRoomResponse, response);
}

void GameService::OpenFarm(const std::shared_ptr<GameClient>& client, const account::LoginInfo& login) {
  const std::uint64_t client_id = client->client_id();
  if (shutdown_) return RejectOpen(client, protocol::GAME_SERVER_BUSY);
  if (connections_.count(client_id) != 0) return RejectOpen(client, protocol::GAME_ALREADY_IN_ROOM);
  if (opening_.count(client_id) != 0) return RejectOpen(client, protocol::GAME_SERVER_BUSY);  // 이전 요청이 처리 중

  opening_.insert(client_id);
  // 저장소 응답은 나중에 로직 스레드에서 온다. 그 사이 연결이 끊겼거나 서비스가 사라졌을 수 있으므로 핸들러가 확인한다.
  farm_repo_->FindFarmByOwner(login.account_id, [this, alive = std::weak_ptr<bool>(alive_), client,
                                                 login](farm::FarmResult<std::optional<farm::FarmData>> result) {
    if (!alive.expired()) {
      OnFarmFound(client, login, std::move(result));
    }
  });
}

void GameService::OnFarmFound(const std::shared_ptr<GameClient>& client, const account::LoginInfo& login,
                              farm::FarmResult<std::optional<farm::FarmData>> result) {
  const std::uint64_t client_id = client->client_id();
  if (opening_.count(client_id) == 0 || shutdown_) {
    return;  // 그 사이 연결이 끊겼다(OnDisconnect가 지웠다)
  }
  if (!result.ok) {
    opening_.erase(client_id);
    ++stats_.farm_open_failures;
    Log(LogLevel::kError, "[농장] 농장을 불러오지 못함 계정=", login.login_name, ": ", result.message);
    return RejectOpen(client, protocol::GAME_STORAGE_UNAVAILABLE);
  }
  if (result.value) {
    return FinishOpenFarm(client, login, std::move(*result.value));
  }
  CreateNewFarm(client, login, 0);
}

void GameService::CreateNewFarm(const std::shared_ptr<GameClient>& client, const account::LoginInfo& login,
                                int attempt) {
  const std::uint64_t client_id = client->client_id();
  std::string code = attempt < 5 ? NewRoomCode() : std::string();
  if (code.empty()) {
    opening_.erase(client_id);
    ++stats_.farm_open_failures;
    Log(LogLevel::kError, "[농장] 새 농장 코드를 정하지 못함 계정=", login.login_name);
    return RejectOpen(client, protocol::GAME_SERVER_BUSY);
  }
  reserved_codes_.insert(code);  // 만드는 동안 게스트 방이 같은 코드를 쓰지 못하게 먼저 예약한다
  farm_repo_->CreateFarm(login.account_id, code,
                         [this, alive = std::weak_ptr<bool>(alive_), client, login, code,
                          attempt](farm::FarmResult<farm::FarmData> result) {
                           if (!alive.expired()) {
                             OnFarmCreated(client, login, code, attempt, std::move(result));
                           }
                         });
}

void GameService::OnFarmCreated(const std::shared_ptr<GameClient>& client, const account::LoginInfo& login,
                                std::string code, int attempt, farm::FarmResult<farm::FarmData> result) {
  const std::uint64_t client_id = client->client_id();
  if (!result.ok && result.error != farm::FarmError::kDuplicateCode) {
    reserved_codes_.erase(code);  // 만들지 못했다. 코드가 DB에 없으니 예약을 푼다(중복 코드는 DB에 있으므로 유지)
  }
  if (opening_.count(client_id) == 0 || shutdown_) {
    return;  // 연결이 끊겼다. 농장이 만들어졌다면 DB에 남아 다음에 열린다
  }
  if (result.ok) {
    return FinishOpenFarm(client, login, std::move(result.value));
  }
  if (result.error == farm::FarmError::kDuplicateCode) {
    return CreateNewFarm(client, login, attempt + 1);  // 코드가 이미 있다. 다른 코드로 다시 만든다
  }
  if (result.error == farm::FarmError::kDuplicateOwner) {
    // 같은 계정의 농장이 이미 있다(드문 경쟁). 그 농장을 불러서 연다.
    farm_repo_->FindFarmByOwner(login.account_id, [this, alive = std::weak_ptr<bool>(alive_), client,
                                                   login](farm::FarmResult<std::optional<farm::FarmData>> found) {
      if (alive.expired()) return;
      if (found.ok && found.value) {
        FinishOpenFarm(client, login, std::move(*found.value));
        return;
      }
      opening_.erase(client->client_id());
      ++stats_.farm_open_failures;
      RejectOpen(client, protocol::GAME_STORAGE_UNAVAILABLE);
    });
    return;
  }
  opening_.erase(client_id);
  ++stats_.farm_open_failures;
  Log(LogLevel::kError, "[농장] 농장을 만들지 못함 계정=", login.login_name, ": ", result.message);
  RejectOpen(client, protocol::GAME_STORAGE_UNAVAILABLE);
}

void GameService::FinishOpenFarm(const std::shared_ptr<GameClient>& client, const account::LoginInfo& login,
                                 farm::FarmData data) {
  const std::uint64_t client_id = client->client_id();
  if (opening_.erase(client_id) == 0 || shutdown_) {
    return;  // 그 사이 연결이 끊겼다
  }
  const auto fail = [&](protocol::GameResult result) {
    ++stats_.farm_open_failures;
    RejectOpen(client, result);
  };
  if (connections_.count(client_id) != 0) return fail(protocol::GAME_ALREADY_IN_ROOM);

  Room* room = nullptr;
  bool newly_opened = false;
  const auto live = rooms_.find(data.code);
  if (live != rooms_.end()) {
    // 이미 열려 있다(다른 사람이 있는 동안 주인이 다시 접속했다). 같은 농장이면 그 방에 들어간다.
    if (live->second->farm_id() != data.farm_id) {
      Log(LogLevel::kError, "[농장] 코드가 같은 다른 방이 열려 있음 코드=", data.code);
      return fail(protocol::GAME_SERVER_BUSY);
    }
    room = live->second.get();
    if (room->Full()) return fail(protocol::GAME_ROOM_FULL);
    if (room->HasNickname(login.login_name)) return fail(protocol::GAME_NICKNAME_TAKEN);
  } else {
    if (rooms_.size() >= config_.max_rooms) return fail(protocol::GAME_SERVER_BUSY);
    auto opened = std::make_unique<Room>(data.code, config_);
    opened->set_farm_id(data.farm_id);
    for (const farm::StoredStructure& stored : data.structures) {
      if (!opened->RestoreStructure(stored.structure_id, stored.type, stored.x, stored.y,
                                    stored.state == static_cast<std::uint32_t>(StructureState::kComplete),
                                    stored.remaining_ticks)) {
        Log(LogLevel::kError, "[농장] 저장된 구조물을 복원하지 못함 farm=", data.farm_id, " 구조물=", stored.structure_id);
        return fail(protocol::GAME_STORAGE_UNAVAILABLE);
      }
    }
    FarmRuntime runtime;
    runtime.farm_id = data.farm_id;
    runtime.owner_account_id = data.owner_account_id;
    for (const farm::StoredPlayer& stored : data.players) runtime.players[stored.account_id] = stored;
    for (const farm::StoredStructure& stored : data.structures) runtime.builders[stored.structure_id] = stored.built_by_account_id;
    farms_[data.code] = std::move(runtime);
    reserved_codes_.insert(data.code);
    room = opened.get();
    rooms_.emplace(data.code, std::move(opened));
    newly_opened = true;
    ++stats_.rooms_created;
    ++stats_.farms_opened;
    ++stats_.current_rooms;
    Log(LogLevel::kInfo, "[농장] 열림 코드=", data.code, " 계정=", login.login_name, " 집=", data.structures.size(),
        " 현재방=", rooms_.size());
  }

  const std::uint32_t player_id = room->AddMember(client_id, login.login_name);
  const auto runtime = farms_.find(data.code);
  if (runtime != farms_.end()) {
    const auto saved = runtime->second.players.find(login.account_id);
    if (saved != runtime->second.players.end()) {
      room->SetInventory(client_id, saved->second.wood, saved->second.stone);
    }
  }
  connections_[client_id] = Connection{client, data.code, login.account_id};
  ++stats_.joins;
  ++stats_.current_players;

  protocol::CreateRoomResponse response;
  response.set_result(protocol::GAME_OK);
  *response.mutable_state() = MakeRoomState(*room, player_id);
  if (const Member* me = room->FindByPlayer(player_id)) {  // 저장된 인벤토리를 알려 준다(HUD가 0으로 보이지 않게)
    response.set_wood(me->wood);
    response.set_stone(me->stone);
  }
  std::vector<std::shared_ptr<GameClient>> others;
  protocol::RoomEvent event;
  if (!newly_opened) {
    event.set_kind(protocol::RoomEvent::PLAYER_JOINED);
    *event.mutable_player() = MakePlayerInfo(*room->FindByPlayer(player_id));
    event.set_host_player_id(room->host_player_id());
    others = RecipientsOf(*room, client_id);
  }
  client->Deliver(protocol::MessageId::kCreateRoomResponse, response);
  DeliverAll(others, protocol::MessageId::kRoomEvent, event);
}

std::vector<farm::StoredStructure> GameService::ExportStructures(const Room& room) const {
  const auto runtime = farms_.find(room.code());
  const std::uint64_t tick = room.world().tick();
  std::vector<farm::StoredStructure> out;
  out.reserve(room.structures().size());
  for (const Structure& structure : room.structures().structures()) {
    farm::StoredStructure stored;
    stored.structure_id = structure.id;
    stored.type = static_cast<std::uint32_t>(structure.type);
    stored.x = structure.x;
    stored.y = structure.y;
    stored.state = static_cast<std::uint32_t>(structure.state);
    // 건설 중이면 완공까지 남은 틱을 저장한다(농장이 닫혀 있는 동안 시간은 흐르지 않는다).
    if (structure.state == StructureState::kBuilding && structure.ready_tick > tick) {
      stored.remaining_ticks = structure.ready_tick - tick;
    }
    if (runtime != farms_.end()) {
      const auto builder = runtime->second.builders.find(structure.id);
      if (builder != runtime->second.builders.end()) {
        stored.built_by_account_id = builder->second;
      }
    }
    out.push_back(stored);
  }
  return out;
}

void GameService::NoteProgress(Room& room, std::uint64_t client_id, const account::AccountStats& delta,
                               bool immediate) {
  if (!persistence_ || !room.is_farm()) {
    return;
  }
  const auto connection = connections_.find(client_id);
  const Member* member = room.FindByClient(client_id);
  if (connection == connections_.end() || connection->second.account_id == 0 || member == nullptr) {
    return;
  }
  // 누적 기록은 계정 서비스가 메모리에 들고 있는 값에 더하고, 그 절대값을 저장 대기열에 넣는다.
  std::optional<account::AccountStats> stats;
  if (hooks_.add_progress) {
    stats = hooks_.add_progress(client_id, delta);
  }
  persistence_->UpdatePlayer(room.farm_id(), farm::StoredPlayer{connection->second.account_id, member->wood, member->stone},
                             stats);
  if (immediate) {
    persistence_->Flush(room.farm_id());
  }
}

void GameService::PersistLeaver(Room& room, const Member& member, std::uint64_t account_id) {
  if (!persistence_ || account_id == 0) {
    return;
  }
  const farm::StoredPlayer player{account_id, member.wood, member.stone};
  const auto runtime = farms_.find(room.code());
  if (runtime != farms_.end()) {
    runtime->second.players[account_id] = player;  // 다시 입장하면 이 값으로 되살린다(저장이 늦어도 메모리가 맞다)
  }
  persistence_->UpdatePlayer(room.farm_id(), player, hooks_.stats_of ? hooks_.stats_of(member.client_id) : std::nullopt);
  if (room.Empty()) {
    // 농장이 닫힌다: 구조물의 마지막 상태(건설 중이면 남은 틱)를 함께 저장한다.
    persistence_->UpdateStructures(room.farm_id(), ExportStructures(room));
  }
  persistence_->Flush(room.farm_id());
}

}  // namespace game::play
