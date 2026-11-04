#pragma once

// ChatRoom: 채팅 방 하나. 입장/퇴장, 채팅 처리, 전파, 기록 조회를 담당한다.
//
// 이 클래스가 하는 일 (docs/protocol-chat.md)
//  1. 접속자 목록과 닉네임 중복을 관리한다. (HandleJoin / HandleLeave / OnDisconnect)
//  2. 채팅 요청을 검증하고 저장 대기열에 넣는다. 방마다 저장은 "한 번에 하나, 도착 순서대로"
//     처리하고, 저장에 성공한 메시지만 방의 모든 입장자에게 전파한다. (HandleChat)
//  3. 저장소에서 최근 기록을 읽어 돌려준다. (HandleHistory)
//  4. 저장소가 느리거나 실패해도 서버가 멈추지 않도록 제한 시간을 둔다.
//
// 이 클래스는 소켓과 Session을 모른다. 연결은 ChatClient 인터페이스로, 저장소는 ChatRepository
// 인터페이스로만 만난다. 그래서 가짜 연결과 인메모리 저장소로 단위 테스트할 수 있다.
//
// 스레드 규칙: 모든 함수는 서버의 io_context 스레드에서 호출해야 한다. 내부 상태에 잠금이 없다.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/asio.hpp>

#include "chat/chat_client.h"
#include "chat/chat_repository.h"
#include "chat.pb.h"

namespace game::chat {

struct ChatRoomOptions {
  // 저장 대기열(아직 저장을 시작하지 못한 채팅)의 최대 길이. 넘으면 RESULT_SERVER_BUSY.
  // 저장소가 느려질 때 요청이 무한히 쌓여 메모리가 늘어나는 것을 막는다.
  std::size_t max_pending_chats = 256;

  // 저장/조회 한 건을 기다리는 최대 시간. 넘으면 RESULT_STORAGE_FAILURE로 응답하고 다음으로 넘어간다.
  std::chrono::milliseconds storage_timeout{3000};

  std::uint32_t default_history_limit = 20;  // HistoryRequest.limit이 0일 때
  std::uint32_t max_history_limit = 50;
};

// 채팅 방의 통계. 서버 통계(Stats)와 따로 두어 채팅 모듈이 서버 모듈에 의존하지 않게 한다.
// 값은 atomic이라 다른 스레드(통계 출력, 테스트)에서 읽어도 안전하다.
struct ChatStats {
  using Counter = std::atomic<std::uint64_t>;

  Counter current_members{0};       // 지금 입장해 있는 접속자 수
  Counter peak_members{0};          // 동시에 입장했던 최대 접속자 수
  Counter messages_saved{0};        // 저장에 성공해 전파한 채팅 수
  Counter requests_rejected{0};     // 업무 규칙 위반으로 거부한 요청 수 (입력 규칙, 상태 위반 등)
  Counter storage_failures{0};      // 저장/조회가 실패한 횟수 (시간 초과 포함)
  Counter storage_timeouts{0};      // 그중 시간 초과로 실패 처리한 횟수
  Counter busy_rejections{0};       // 저장 대기열이 가득 차 거부한 횟수
  Counter max_pending_depth{0};     // 저장 대기열 길이의 최댓값

  std::string Summary() const;
};

class ChatRoom {
 public:
  ChatRoom(boost::asio::io_context& io, std::shared_ptr<ChatRepository> repository,
           ChatRoomOptions options = {});

  ChatRoom(const ChatRoom&) = delete;
  ChatRoom& operator=(const ChatRoom&) = delete;

  // ---- 연결이 보낸 요청을 처리한다. 응답은 client->Deliver로 돌려준다. ----
  void HandleJoin(const std::shared_ptr<ChatClient>& client, const protocol::JoinRequest& request);
  void HandleLeave(const std::shared_ptr<ChatClient>& client);
  void HandleChat(const std::shared_ptr<ChatClient>& client, const protocol::ChatRequest& request);
  void HandleHistory(const std::shared_ptr<ChatClient>& client, const protocol::HistoryRequest& request);

  // 연결이 끊겼을 때 부른다. 입장해 있었다면 퇴장 처리하고 남은 사람에게 알린다.
  void OnDisconnect(std::uint64_t client_id);

  // 서버 종료 시 부른다. 진행 중인 저장 대기를 모두 취소해서 타이머가 서버 종료를 붙잡지 않게 하고,
  // 이후로는 알림을 보내지 않는다.
  void Shutdown();

  const ChatStats& stats() const { return stats_; }

 private:
  // 방의 접속자 한 명.
  struct Member {
    std::uint64_t client_id = 0;
    std::weak_ptr<ChatClient> client;  // 약한 참조: 방이 연결의 수명을 붙잡지 않는다
    std::string nickname;
  };

  // 저장을 기다리는 채팅 한 건. 닉네임은 요청한 시점의 것을 저장해 둔다.
  struct PendingChat {
    std::uint64_t sender_id = 0;
    std::weak_ptr<ChatClient> sender;
    std::string nickname;
    std::string text;
  };

  // 저장소 작업 하나를 감시하는 상태. 결과 도착과 시간 초과 중 먼저 오는 쪽이 이기고,
  // 나중에 오는 쪽은 finished를 보고 무시한다.
  struct Guard {
    explicit Guard(boost::asio::io_context& io) : timer(io) {}
    boost::asio::steady_timer timer;
    std::uint64_t id = 0;
    bool finished = false;
  };

  // ---- 접속자 관리 ----
  Member* FindMember(std::uint64_t client_id);
  bool NicknameInUse(const std::string& nickname) const;
  void RemoveMember(std::uint64_t client_id);

  // ---- 전송 도우미 ----
  static void DeliverTo(const std::weak_ptr<ChatClient>& client, protocol::MessageId id,
                        const google::protobuf::MessageLite& message);
  // 현재 접속자 모두에게 보낸다. except_id인 연결은 제외한다. (0이면 제외 없음)
  void Broadcast(protocol::MessageId id, const google::protobuf::MessageLite& message,
                 std::uint64_t except_id = 0);
  void NotifyPresence(protocol::PresenceNotice::Kind kind, const std::string& nickname,
                      std::uint64_t except_id);

  // ---- 저장 ----
  void SaveNext();
  void OnSaveFinished(const PendingChat& chat, StorageResult<StoredMessage> result);
  void OnSaveTimedOut(const PendingChat& chat);

  // 저장소 작업을 시작하고 결과 또는 시간 초과 중 먼저 오는 쪽만 처리한다.
  //   start:      결과를 받을 핸들러를 넘겨받아 저장소 작업을 시작하는 함수
  //   on_result:  결과가 제한 시간 안에 도착했을 때
  //   on_timeout: 제한 시간이 지났을 때 (이후 도착하는 결과는 버려진다)
  template <typename Result, typename StartFn, typename OnResult, typename OnTimeout>
  void RunWithTimeout(StartFn start, OnResult on_result, OnTimeout on_timeout);

  void RecordPendingDepth();

  boost::asio::io_context& io_;
  std::shared_ptr<ChatRepository> repository_;
  const ChatRoomOptions options_;
  ChatStats stats_;

  std::vector<Member> members_;  // 입장한 순서대로
  std::deque<PendingChat> pending_;
  bool saving_ = false;  // 저장이 진행 중인지 (방 단위 직렬화)

  std::unordered_map<std::uint64_t, std::shared_ptr<Guard>> guards_;
  std::uint64_t next_guard_id_ = 0;
  bool shutting_down_ = false;
};

}  // namespace game::chat
