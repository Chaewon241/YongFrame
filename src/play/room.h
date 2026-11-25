#pragma once

// Room: 게임 방 하나의 인원, 방장, 닉네임, 월드(위치)를 관리한다. 소켓과 시간을 모르는 순수 로직이다.
//
// 연결은 client_id(번호)로만 구분한다. 실제 연결 객체와 메시지 전송은 GameService가 맡는다.
// 방 안에서의 사람은 player_id로 구분한다. player_id는 방이 닫힐 때까지 재사용하지 않는다.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "play/landmark.h"
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

  // 하루와 잠(docs/content-day.md 5.2). 들어올 때는 깨어 있고, 나가면 사라진다.
  bool sleeping = false;
};

// 잠자기/깨어나기 시도의 결과. 프로토콜 코드(GameResult)로의 변환은 GameService가 한다.
enum class SleepStatus {
  kOk,
  kNotInRoom,  // 이 방에 없는 연결
  kTooFar,     // 시작 집이나 완공된 집 앞(sleep_range_cm 안)이 아니다
};

struct SleepOutcome {
  SleepStatus status = SleepStatus::kNotInRoom;
  bool sleeping = false;  // 시도 뒤의 내 잠 상태(성공/실패 모두 현재 값)
  bool changed = false;   // 잠 상태가 실제로 바뀌었는가
};

// 밤 정산을 해야 하는가(틱마다 이동 계산 뒤에 판정).
enum class NightTrigger {
  kNone,
  kAllAsleep,  // 방에 사람이 있고 모두 잠들었다(forced = false)
  kForced,     // 하루 끝(02:00)이 되었다(forced = true)
};

// 밤 정산 결과.
struct NightReport {
  std::uint32_t completed = 0;  // 완공된 구조물 수
  std::uint32_t respawned = 0;  // 되살아난 노드 수
  std::uint32_t day = 0;        // 새 날 번호
  bool forced = false;
};

// 방 시계의 저장용 값: 날 번호와 그날의 경과 틱.
struct ClockState {
  std::uint32_t day = 1;
  std::uint64_t elapsed_ticks = 0;
};

// 채집 시도의 결과. 프로토콜 코드(GameResult)로의 변환은 GameService가 한다.
enum class GatherStatus {
  kOk,
  kNodeNotFound,
  kNodeDepleted,
  kTooFar,
  kCooldown,
  kNotInRoom,  // 이 방에 없는 연결
  kSleeping,   // 잠든 상태에서는 채집할 수 없다
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
  kSleeping,            // 잠든 상태에서는 지을 수 없다
  kInvalidType,         // 모르는 건물 종류
  kLimit,               // 방의 구조물 수 상한
  kTooFar,              // 서버가 아는 내 위치에서 건설 허용 거리 밖
  kOutOfZone,           // 건물 발자국이 건설 구역(맵 중앙 정사각형) 밖으로 나감
  kBlocked,             // 맵 밖, 구조물/고정 물체/노드/플레이어와 겹침, 잘못된 좌표
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

  // 연결이 자원 노드를 한 번 친다. 판정 순서: 방에 없음 -> 잠듦 -> 노드 없음 -> 고갈 -> 너무 멂 -> 쿨다운.
  // 거부되면 아무 상태도 바뀌지 않는다. 시간은 이 방 월드의 틱 번호다.
  GatherOutcome Gather(std::uint64_t client_id, std::uint32_t node_id);

  // 낮 동안의 재생 재시도: 밤 정산에서 자리를 못 찾은 노드만 예약 시각에 다시 찾는다. 재생 자리는 구조물 발자국,
  // 고정 물체, 플레이어 몸을 피한다. 되살린 노드 수를 돌려준다.
  std::uint32_t AdvanceResources(std::uint64_t tick);

  // 이동 입력. 방에 없으면 kUnknownPlayer, 잠들어 있으면 kSleeping(통째로 무시, 입력 번호도 올리지 않는다).
  InputResult ApplyInput(std::uint64_t client_id, std::uint32_t sequence, float dir_x, float dir_y);

  // ---- 하루와 잠 (docs/content-day.md) ----
  // 잔다(sleep = true): 방에 없음 -> 이미 잠듦(성공, 변화 없음) -> 잘 수 있는 집이 근처에 없음 순서. 잠들면 이동 방향이
  // (0, 0)이 된다. 깬다(sleep = false): 방에 없음 -> 이미 깨어 있음(성공, 변화 없음) -> 깨운다(위치 그대로).
  // 거부되면 상태가 바뀌지 않는다.
  SleepOutcome Sleep(std::uint64_t client_id, bool sleep);
  // (x, y)에서 잘 수 있는가: 시작 집이나 완공된 집의 발자국까지 최단 거리가 sleep_range_cm 이하(경계 포함).
  bool CanSleepAt(float x, float y) const;
  bool IsSleeping(std::uint64_t client_id) const;
  // 지금 잠든 사람의 player_id(입장한 순서).
  std::vector<std::uint32_t> SleepingPlayerIds() const;
  // 잠든 사람 목록이 마지막으로 꺼내 간 목록과 다르면 true와 현재 목록을 돌려주고 그 목록을 기억한다.
  bool TakeSleepChange(std::vector<std::uint32_t>& out);

  // 이 틱(tick, 보통 world().tick())에 밤 정산을 해야 하는가. 모두 잠듦이 강제보다 먼저다.
  NightTrigger ShouldEndDay(std::uint64_t tick) const;
  // 밤 정산(5.4절 순서): 1) 건설 중인 집 완공 2) (조각 3~5 자리) 3) 모두 깨우고 자기 칸의 시작 위치에 방향 (0, 0)으로
  // 4) 고갈된 노드 재생 5) day += 1, day_start_tick = tick. 알림과 저장은 GameService가 한다.
  NightReport RunNight(std::uint64_t tick, bool forced);

  std::uint32_t day() const { return day_; }
  std::int64_t day_start_tick() const { return day_start_tick_; }
  // 그날의 경과 틱(월드 틱 - 그날 시작 틱). 음수가 되지 않게 0으로 자른다.
  std::uint64_t DayElapsed() const;
  // 게임 시각(자정부터 센 분): day_start_minute + 경과 / ticks_per_game_minute.
  std::uint32_t GameMinute() const;
  // 저장된 시계로 되살린다(농장을 열 때). day_start_tick = 월드 틱 - elapsed(음수일 수 있다). elapsed가 하루 길이보다
  // 크면 하루 길이로 자르고 true를 돌려준다(다음 틱에 강제 정산된다). day가 0이면 1로 둔다.
  bool RestoreClock(std::uint32_t day, std::uint64_t elapsed_ticks);
  ClockState ExportClock() const { return ClockState{day_, DayElapsed()}; }

  const std::vector<Landmark>& landmarks() const { return landmarks_; }
  // 노드 재생 자리가 피해야 할 것: 구조물(건설 중 포함), 고정 물체, 플레이어 몸(지금 위치).
  SpotBlockers MakeSpotBlockers() const;

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
  // 같아야 성공이다. 건설 중이면 저장된 남은 틱과 상관없이 건설 중으로 되살리고 다음 밤 정산에 완료된다.
  bool RestoreStructure(std::uint32_t structure_id, std::uint32_t type, float x, float y, bool complete);

  // 연결의 인벤토리를 정한다(저장된 값을 되살릴 때). 이 방에 없는 연결이면 false.
  bool SetInventory(std::uint64_t client_id, std::uint32_t wood, std::uint32_t stone);

  // 연결이 건물을 짓는다. (x, y)는 짓고 싶은 중심이며 build_grid_cm 격자의 중심으로 맞춘다.
  // 판정 순서: 방에 없음 -> 잠듦 -> 모르는 종류 -> 구조물 상한 -> 너무 멂 -> 건설 구역 밖 -> 겹침/맵 밖 -> 자원 부족.
  // 거부되면 아무 상태도 바뀌지 않는다. 성공하면 비용을 차감하고 건설 중 구조물을 만든다. 시간은 월드 틱이다.
  BuildOutcome Build(std::uint64_t client_id, std::uint32_t structure_type, float x, float y);

 private:
  std::uint32_t LowestFreeSlot() const;
  Member* FindMutableByClient(std::uint64_t client_id);

  const std::string code_;
  const PlayConfig config_;
  std::vector<Member> members_;
  std::uint32_t next_player_id_ = 1;
  std::uint32_t host_player_id_ = 0;  // 0이면 방장 없음(빈 방)
  std::uint64_t farm_id_ = 0;         // 0이면 임시 방
  std::vector<Landmark> landmarks_;  // 고정 물체(설정에서 만든다)
  World world_;
  ResourceField resources_;
  StructureSet structures_;
  std::uint32_t day_ = 1;
  std::int64_t day_start_tick_ = 0;               // 그날이 시작된 월드 틱(복원하면 음수일 수 있다)
  std::vector<std::uint32_t> reported_sleepers_;  // TakeSleepChange가 마지막으로 꺼내 간 목록
};

}  // namespace game::play
