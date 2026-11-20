#pragma once

// GameService: 게임 방들을 관리하고 틱을 돌린다.
//
// 하는 일
//  1. 방 만들기(방 코드 발급), 코드로 입장, 나가기, 이동 입력 처리 (Handle*)
//  2. 방장 지정과 변경, 빈 방 삭제, 입장/퇴장/방장 변경 알림
//  3. 고정 간격(기본 20Hz)으로 모든 방의 위치를 계산하고 방 전체 스냅샷을 보낸다 (Tick, StartTicking)
//
// 연결은 기존 채팅과 같은 인터페이스(ChatClient: client_id, Deliver)로만 본다. 그래서 소켓 없이 가짜
// 연결로 단위 테스트할 수 있다. 방 로직 자체는 Room/World가 가진다.
//
// 스레드 규칙: 모든 함수는 io_context를 돌리는 같은 스레드에서 부른다(서버는 스레드 1개).
// 메시지를 보내는 중(Deliver)에 연결이 닫혀 OnDisconnect가 다시 불릴 수 있으므로, 보내는 동안에는
// 방 컨테이너를 순회하지 않고 미리 확보해 둔 수신자 목록만 쓴다.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/asio.hpp>

#include "account/account_repository.h"
#include "account/login_info.h"
#include "chat/chat_client.h"
#include "farm/farm_repository.h"
#include "game.pb.h"
#include "play/farm_persistence.h"
#include "play/play_config.h"
#include "play/room.h"

namespace game::play {

// 방이 보는 연결 하나. 채팅과 같은 인터페이스를 그대로 쓴다.
using GameClient = chat::ChatClient;

// 방 코드를 만드는 함수. 숫자 6자리 문자열을 돌려줘야 한다(아니면 서버가 버리고 다시 뽑는다).
// 테스트에서 일부러 같은 코드를 반복해서 충돌 처리를 확인할 수 있게 바꿔 끼울 수 있다.
using RoomCodeGenerator = std::function<std::string()>;

// 게임 서비스가 계정 서비스에 묻고 알리는 함수들. 서버가 계정 서비스와 이어 준다(게스트만 있는 서버와 단위 시험에서는
// 비워 둔다. 비어 있으면 모든 연결이 로그인하지 않은 것으로 다룬다).
struct AccountHooks {
  // 이 연결이 로그인했다면 그 정보(계정 번호, 닉네임).
  std::function<std::optional<account::LoginInfo>(std::uint64_t client_id)> login_of;
  // 로그인한 연결의 누적 기록(메모리의 최신 절대값).
  std::function<std::optional<account::AccountStats>(std::uint64_t client_id)> stats_of;
  // 로그인한 연결의 누적 기록에 delta를 더하고 갱신된 절대값을 돌려준다.
  std::function<std::optional<account::AccountStats>(std::uint64_t client_id, const account::AccountStats& delta)>
      add_progress;
};

struct GameStats {
  using Counter = std::atomic<std::uint64_t>;

  Counter current_rooms{0};     // 지금 있는 방 수
  Counter current_players{0};   // 지금 방에 있는 사람 수
  Counter rooms_created{0};     // 지금까지 만든 방 수
  Counter rooms_closed{0};      // 비어서 지운 방 수
  Counter joins{0};             // 입장(방 만들기 포함) 성공 횟수
  Counter requests_rejected{0}; // 업무 규칙 위반으로 거부한 요청 수
  Counter inputs_applied{0};    // 반영한 이동 입력 수
  Counter inputs_ignored{0};    // 무시한 이동 입력 수 (방에 없음, 잘못된 값, 오래된 번호)
  Counter ticks{0};             // 처리한 틱 수 (틱 한 번에 방 전체를 계산)
  Counter ticks_skipped{0};     // 처리가 밀려 건너뛴 틱 수
  Counter snapshots_sent{0};    // 보낸 스냅샷 수 (수신자 수만큼 센다)
  Counter max_tick_micros{0};   // 한 틱(계산과 전송 요청)에 걸린 시간의 최댓값 (마이크로초)
  Counter gathers_ok{0};        // 받아들인 채집 수
  Counter gathers_rejected{0};  // 거부한 채집 수 (방에 없음, 없는 노드, 고갈, 너무 멂, 쿨다운)
  Counter nodes_depleted{0};    // 체력이 0이 된 노드 수
  Counter nodes_respawned{0};   // 재생한 노드 수
  Counter resource_updates_sent{0};  // 보낸 ResourceUpdate 수 (수신자 수만큼 센다)
  Counter builds_ok{0};              // 받아들인 건설 수
  Counter builds_rejected{0};        // 거부한 건설 수 (방에 없음, 종류, 상한, 너무 멂, 겹침, 자원 부족)
  Counter structures_completed{0};   // 건설이 끝난 구조물 수
  Counter structure_updates_sent{0}; // 보낸 StructureUpdate 수 (수신자 수만큼 센다)
  Counter farms_opened{0};           // 농장을 열어 방으로 만든 횟수(이미 열린 농장에 들어간 것은 제외)
  Counter farm_open_failures{0};     // 농장을 열지 못한 횟수(저장소 오류, 방 코드 충돌, 복원 실패)
  // 틱 시간의 단계별 누적(마이크로초). 병목을 가르려고 계측한다. 합 = 계산 + 갱신 + 구성 + 전송 + 기타(루프 오버헤드).
  Counter tick_us_step{0};     // 월드 이동 계산(World::Step)
  Counter tick_us_update{0};   // 재생/완공 갱신(노드 재생, 구조물 완공)
  Counter tick_us_build{0};    // 수신자 목록과 스냅샷/갱신 메시지 구성
  Counter tick_us_deliver{0};  // 전송(Deliver: 직렬화, 프레임, 송신 큐, 송신 시작)
  Counter tick_us_total{0};    // 한 틱 전체

  std::string Summary() const;
};

class GameService {
 public:
  GameService(boost::asio::io_context& io, PlayConfig config = {}, RoomCodeGenerator generator = nullptr);

  GameService(const GameService&) = delete;
  GameService& operator=(const GameService&) = delete;

  // 계정 서비스와 이어 주는 함수들. 로그인한 연결은 방에서 계정의 ID가 닉네임이 되고, 방 만들기는 그 계정의 농장을
  // 여는 것이 된다(농장 저장소가 있을 때). 시작하기 전에 한 번 정한다.
  void SetAccountHooks(AccountHooks hooks) { hooks_ = std::move(hooks); }

  // 농장 저장소를 정한다(시작하기 전에 한 번). 넘기면 로그인한 연결의 방 만들기는 농장을 열고, 변경은 저장소에 쓴다.
  // 넘기지 않으면 모든 방이 저장하지 않는 임시 방이다.
  void SetFarmRepository(std::shared_ptr<farm::FarmRepository> repository);

  // 서버가 시작할 때 부른다: 모든 농장 코드를 읽어 게스트 방 코드가 농장 코드와 겹치지 않게 예약한다. 읽지 못해도
  // 서버는 계속 동작한다(그 경우 게스트 방이 안 열린 농장의 코드를 쓸 수 있고, 그 농장은 열 때 충돌로 거절된다).
  void LoadFarmCodes();

  // 농장 저장 통계(저장소가 없으면 빈 문자열).
  std::string FarmSummary() const;
  const FarmPersistence* farm_persistence() const { return persistence_.get(); }

  // ---- 연결이 보낸 요청 처리. 응답은 client->Deliver로 돌려준다. ----
  void HandleCreateRoom(const std::shared_ptr<GameClient>& client, const protocol::CreateRoomRequest& request);
  void HandleJoinRoom(const std::shared_ptr<GameClient>& client, const protocol::JoinRoomRequest& request);
  void HandleLeaveRoom(const std::shared_ptr<GameClient>& client);
  // 이동 입력. 응답이 없고, 방에 없거나 값이 잘못된 입력은 조용히 무시한다.
  void HandleMoveInput(const std::shared_ptr<GameClient>& client, const protocol::MoveInput& input);
  // 자원 노드를 한 번 친다. 항상 GatherResponse(결과 코드와 내 인벤토리)를 돌려주고 연결은 유지한다.
  // 성공하면 바뀐 노드는 같은 틱 끝에 ResourceUpdate로 방 전원에게 간다.
  void HandleGather(const std::shared_ptr<GameClient>& client, const protocol::GatherRequest& request);
  // 건물을 짓는다. 항상 BuildResponse(결과 코드와 내 인벤토리)를 돌려주고 연결은 유지한다. 성공하면 새 구조물은
  // 같은 틱 끝에 StructureUpdate로 방 전원에게 간다.
  void HandleBuild(const std::shared_ptr<GameClient>& client, const protocol::BuildRequest& request);

  // 연결이 끊겼을 때 부른다. 방에 있었다면 나간 것으로 처리하고 남은 사람에게 알린다.
  void OnDisconnect(std::uint64_t client_id);

  // 고정 간격 타이머를 시작한다. 타이머는 절대 시각으로 예약해 시간이 밀려도 누적 오차가 생기지 않는다.
  void StartTicking();

  // 한 틱을 처리한다: steps번 위치를 계산하고, 방마다 마지막 상태의 스냅샷을 한 번 보낸다.
  // 타이머가 부르지만 테스트에서도 직접 부를 수 있다.
  void Tick(std::uint32_t steps = 1);

  // 서버 종료 시 부른다. 타이머를 멈추고, 이후로는 알림을 보내지 않는다.
  void Shutdown();

  const GameStats& stats() const { return stats_; }
  const PlayConfig& config() const { return config_; }

  // 시험과 관측용 조회.
  std::size_t room_count() const { return rooms_.size(); }
  const Room* FindRoom(const std::string& code) const;
  // 연결이 들어가 있는 방 코드. 방에 없으면 빈 문자열.
  std::string RoomCodeOf(std::uint64_t client_id) const;

 private:
  struct Connection {
    std::weak_ptr<GameClient> client;
    std::string room_code;
    std::uint64_t account_id = 0;  // 로그인한 연결이면 계정 번호(농장 방에서만 0이 아니다)
  };

  // 열려 있는 농장 하나의 메모리 상태. 방 코드로 찾는다. 방이 닫히면 지운다.
  struct FarmRuntime {
    std::uint64_t farm_id = 0;
    std::uint64_t owner_account_id = 0;
    std::map<std::uint64_t, farm::StoredPlayer> players;  // 지금 방에 없는 사람들의 마지막 인벤토리(방에 있는 사람은 Member가 진실)
    std::map<std::uint32_t, std::uint64_t> builders;      // 구조물 번호 -> 지은 계정
  };

  // ---- 농장 ----
  void OpenFarm(const std::shared_ptr<GameClient>& client, const account::LoginInfo& login);
  void OnFarmFound(const std::shared_ptr<GameClient>& client, const account::LoginInfo& login,
                   farm::FarmResult<std::optional<farm::FarmData>> result);
  void CreateNewFarm(const std::shared_ptr<GameClient>& client, const account::LoginInfo& login, int attempt);
  void OnFarmCreated(const std::shared_ptr<GameClient>& client, const account::LoginInfo& login, std::string code,
                     int attempt, farm::FarmResult<farm::FarmData> result);
  void FinishOpenFarm(const std::shared_ptr<GameClient>& client, const account::LoginInfo& login, farm::FarmData data);
  void RejectOpen(const std::shared_ptr<GameClient>& client, protocol::GameResult result);
  // 채집/건설로 계정의 누적 기록과 이 농장의 인벤토리가 바뀌었음을 저장 대기열에 알린다. immediate이면 바로 저장한다.
  void NoteProgress(Room& room, std::uint64_t client_id, const account::AccountStats& delta, bool immediate);
  std::vector<farm::StoredStructure> ExportStructures(const Room& room) const;
  // 방에서 나가는 사람(그리고 비게 되는 방)의 상태를 저장 대기열에 넣고 바로 저장한다.
  void PersistLeaver(Room& room, const Member& member, std::uint64_t account_id);

  // 방을 만들 새 코드를 뽑는다. 겹치는 코드가 계속 나오면 빈 문자열.
  std::string NewRoomCode();
  protocol::RoomState MakeRoomState(const Room& room, std::uint32_t your_player_id) const;
  // 사람 한 명을 방에서 빼고 남은 사람에게 알린다. 방이 비면 지운다. 방에 없던 연결이면 아무것도 하지 않는다.
  void RemoveFromRoom(std::uint64_t client_id);
  // room의 사람들 중 except_client_id를 뺀 연결들(살아 있는 것만). 메시지를 보내는 도중에 방이 바뀌어도
  // 안전하도록 강한 참조 목록을 미리 만들어 쓴다.
  std::vector<std::shared_ptr<GameClient>> RecipientsOf(const Room& room, std::uint64_t except_client_id) const;
  static void DeliverAll(const std::vector<std::shared_ptr<GameClient>>& targets, protocol::MessageId id,
                         const google::protobuf::MessageLite& message);
  void ScheduleTick();
  void OnTickTimer(const boost::system::error_code& ec);

  boost::asio::steady_timer timer_;
  const PlayConfig config_;
  RoomCodeGenerator generator_;
  AccountHooks hooks_;
  std::shared_ptr<farm::FarmRepository> farm_repo_;
  std::shared_ptr<FarmPersistence> persistence_;
  std::unordered_map<std::string, FarmRuntime> farms_;  // 열려 있는 농장. 키: 방 코드
  std::unordered_set<std::string> reserved_codes_;      // 농장 코드(DB에 있는 것). 게스트 방은 이 코드를 쓰지 않는다
  std::unordered_set<std::uint64_t> opening_;           // 농장을 여는 중인 연결(저장소 응답을 기다린다)
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);  // 저장소 핸들러가 서비스가 아직 있는지 확인하는 표지
  GameStats stats_;
  std::unordered_map<std::string, std::unique_ptr<Room>> rooms_;
  std::unordered_map<std::uint64_t, Connection> connections_;  // 방에 들어간 연결만
  std::chrono::steady_clock::time_point next_tick_;
  bool ticking_ = false;
  bool shutdown_ = false;
};

}  // namespace game::play
