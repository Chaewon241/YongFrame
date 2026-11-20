#pragma once

// Room: 게임 방 하나의 인원, 방장, 닉네임, 월드(위치)를 관리한다. 소켓과 시간을 모르는 순수 로직이다.
//
// 연결은 client_id(번호)로만 구분한다. 실제 연결 객체와 메시지 전송은 GameService가 맡는다.
// 방 안에서의 사람은 player_id로 구분한다. player_id는 방이 닫힐 때까지 재사용하지 않는다.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "play/play_config.h"
#include "play/resources.h"
#include "play/structures.h"
#include "play/world.h"

namespace game::play {

struct Member {
  std::uint64_t client_id = 0;
  std::uint32_t player_id = 0;
  std::string nickname;
  std::uint32_t slot = 0;  // 시작 위치를 정하는 칸 번호. 나가면 비워 다음 사람이 쓴다.

  // 자원 채집: 인벤토리와 쿨다운 기준. 방을 나가면 사라진다(저장하지 않는다).
  std::uint32_t wood = 0;
  std::uint32_t stone = 0;
  bool has_gathered = false;
  std::uint64_t last_gather_tick = 0;  // 받아들여진 마지막 채집이 있던 월드 틱
};

// 채집 시도의 결과. 프로토콜 코드(GameResult)로의 변환은 GameService가 한다.
enum class GatherStatus {
  kOk,
  kNodeNotFound,
  kNodeDepleted,
  kTooFar,
  kCooldown,
  kNotInRoom,  // 이 방에 없는 연결
};

struct GatherOutcome {
  GatherStatus status = GatherStatus::kNotInRoom;
  std::uint32_t wood = 0;  // 시도 뒤 내 인벤토리 합계(성공/실패 모두)
  std::uint32_t stone = 0;
};

// 건설 시도의 결과. 프로토콜 코드(GameResult)로의 변환은 GameService가 한다.
enum class BuildStatus {
  kOk,
  kNotInRoom,           // 이 방에 없는 연결
  kInvalidType,         // 모르는 건물 종류
  kLimit,               // 방의 구조물 수 상한
  kTooFar,              // 서버가 아는 내 위치에서 건설 허용 거리 밖
  kOutOfZone,           // 건물 발자국이 건설 구역(맵 중앙 정사각형) 밖으로 나감
  kBlocked,            // 맵 밖, 구조물/노드/플레이어와 겹침, 잘못된 좌표
  kNotEnoughResources,  // 비용 부족
};

struct BuildOutcome {
  BuildStatus status = BuildStatus::kNotInRoom;
  std::uint32_t structure_id = 0;  // 성공했을 때만
  std::uint32_t wood = 0;          // 시도 뒤 내 인벤토리 합계(성공/실패 모두)
  std::uint32_t stone = 0;
};

class Room {
 public:
  Room(std::string code, const PlayConfig& config);

  // 월드가 이 방의 노드와 구조물을 가리키는 포인터를 들고 있으므로 복사하거나 옮기지 않는다.
  Room(const Room&) = delete;
  Room& operator=(const Room&) = delete;

  const std::string& code() const { return code_; }
  bool Full() const { return members_.size() >= config_.max_players; }
  bool Empty() const { return members_.empty(); }

  bool HasNickname(std::string_view nickname) const;
  const Member* FindByClient(std::uint64_t client_id) const;
  const Member* FindByPlayer(std::uint32_t player_id) const;

  // 사람을 방에 넣고 새 player_id를 돌려준다. 가득 찼는지는 호출한 쪽이 먼저 확인한다(Full()).
  // 첫 사람이 방장이 된다.
  std::uint32_t AddMember(std::uint64_t client_id, std::string nickname);

  struct RemoveResult {
    bool removed = false;       // 방에 있던 사람이었는가
    Member member;              // 나간 사람 (removed가 true일 때만 유효)
    bool host_changed = false;  // 나간 사람이 방장이었고 남은 사람이 있어 방장이 바뀜
  };
  // 사람을 방에서 뺀다. 방장이 나갔고 남은 사람이 있으면, 먼저 들어온 순서로 다음 사람이 방장이 된다.
  RemoveResult RemoveMember(std::uint64_t client_id);

  std::uint32_t host_player_id() const { return host_player_id_; }
  const std::vector<Member>& members() const { return members_; }  // 입장한 순서

  World& world() { return world_; }
  const World& world() const { return world_; }

  ResourceField& resources() { return resources_; }
  const ResourceField& resources() const { return resources_; }

  // 연결이 자원 노드를 한 번 친다. 판정 순서: 방에 없음 -> 노드 없음 -> 고갈 -> 너무 멂 -> 쿨다운.
  // 거부되면 아무 상태도 바뀌지 않는다. 시간은 이 방 월드의 틱 번호다.
  GatherOutcome Gather(std::uint64_t client_id, std::uint32_t node_id);

  // 재생할 노드를 되살린다. 재생 자리는 구조물 발자국과 플레이어 몸을 피한다. 되살린 노드 수를 돌려준다.
  std::uint32_t AdvanceResources(std::uint64_t tick);

  // 연결의 인벤토리에 자원을 더한다(보상, 시험용). 이 방에 없는 연결이면 false.
  bool GrantResources(std::uint64_t client_id, std::uint32_t wood, std::uint32_t stone);

  StructureSet& structures() { return structures_; }
  const StructureSet& structures() const { return structures_; }

  // ---- 농장(영속하는 방) ----
  // 농장의 번호(저장소의 farm_id). 0이면 저장하지 않는 임시 방(게스트 방)이다.
  std::uint64_t farm_id() const { return farm_id_; }
  bool is_farm() const { return farm_id_ != 0; }
  void set_farm_id(std::uint64_t farm_id) { farm_id_ = farm_id; }

  // 저장되어 있던 구조물을 되살린다(농장을 열 때, 아직 사람이 없을 때). 저장된 번호는 1부터 이어져야 하며 새로 붙는 번호가
  // 같아야 성공이다. 건설 중이면 remaining_ticks 뒤에 완료된다.
  bool RestoreStructure(std::uint32_t structure_id, std::uint32_t type, float x, float y, bool complete,
                        std::uint64_t remaining_ticks);

  // 연결의 인벤토리를 정한다(저장된 값을 되살릴 때). 이 방에 없는 연결이면 false.
  bool SetInventory(std::uint64_t client_id, std::uint32_t wood, std::uint32_t stone);

  // 연결이 건물을 짓는다. (x, y)는 짓고 싶은 중심이며 build_grid_cm 격자의 중심으로 맞춘다.
  // 판정 순서: 방에 없음 -> 모르는 종류 -> 구조물 상한 -> 너무 멂 -> 건설 구역 밖 -> 겹침/맵 밖 -> 자원 부족.
  // 거부되면 아무 상태도 바뀌지 않는다. 성공하면 비용을 차감하고 건설 중 구조물을 만든다. 시간은 월드 틱이다.
  BuildOutcome Build(std::uint64_t client_id, std::uint32_t structure_type, float x, float y);

 private:
  std::uint32_t LowestFreeSlot() const;

  const std::string code_;
  const PlayConfig config_;
  std::vector<Member> members_;
  std::uint32_t next_player_id_ = 1;
  std::uint32_t host_player_id_ = 0;  // 0이면 방장 없음(빈 방)
  std::uint64_t farm_id_ = 0;         // 0이면 임시 방
  World world_;
  ResourceField resources_;
  StructureSet structures_;
};

}  // namespace game::play
