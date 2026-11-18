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
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/asio.hpp>

#include "chat/chat_client.h"
#include "game.pb.h"
#include "play/play_config.h"
#include "play/room.h"

namespace game::play {

// 방이 보는 연결 하나. 채팅과 같은 인터페이스를 그대로 쓴다.
using GameClient = chat::ChatClient;

// 방 코드를 만드는 함수. 숫자 6자리 문자열을 돌려줘야 한다(아니면 서버가 버리고 다시 뽑는다).
// 테스트에서 일부러 같은 코드를 반복해서 충돌 처리를 확인할 수 있게 바꿔 끼울 수 있다.
using RoomCodeGenerator = std::function<std::string()>;

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

  std::string Summary() const;
};

class GameService {
 public:
  GameService(boost::asio::io_context& io, PlayConfig config = {}, RoomCodeGenerator generator = nullptr);

  GameService(const GameService&) = delete;
  GameService& operator=(const GameService&) = delete;

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
  };

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
  GameStats stats_;
  std::unordered_map<std::string, std::unique_ptr<Room>> rooms_;
  std::unordered_map<std::uint64_t, Connection> connections_;  // 방에 들어간 연결만
  std::chrono::steady_clock::time_point next_tick_;
  bool ticking_ = false;
  bool shutdown_ = false;
};

}  // namespace game::play
