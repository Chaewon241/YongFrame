#pragma once

// Session: 클라이언트 연결 "하나"를 담당하는 클래스.
//
// 한 연결에서 일어나는 일의 흐름:
//
//   소켓에서 바이트 읽기 (DoRead / OnRead)
//     -> FrameDecoder에 넣어 프레임으로 자르기 (ProcessFrames)
//       -> 메시지 ID에 따라 처리 (HandleFrame)
//            Ping은 직접 처리하고, 입장/퇴장/채팅/기록 요청은 파싱한 뒤 채팅 방(ChatRoom)에,
//            방 만들기/입장/나가기/이동 입력은 파싱한 뒤 게임 서비스(GameService)에 넘긴다
//         -> 응답을 송신 큐에 넣기 (SendMessage -> Send)
//           -> 큐에서 하나씩 꺼내 소켓에 쓰기 (DoWrite / OnWrite)
//
// 채팅 방은 이 클래스를 ChatClient 인터페이스로만 본다. 방이 이 연결에 메시지를 보내려면 Deliver를
// 부르고, Deliver는 SendMessage로 이어져 같은 송신 큐를 쓴다. 따라서 방이 보낸 메시지도 응답과 같은
// 순서 규칙과 송신 큐 한도를 따른다.
//
// 오류나 종료가 생기면 Close()가 연결을 정리하고, 이유를 로그와 통계에 남긴다.
//
// 스레드 규칙 (두 구역으로 나뉜다):
//
//  - 논리 구역: 프레임 디코딩 이후의 모든 일(디코더, 채팅 방/게임 서비스 호출, Close, 서버 세션 목록)은
//    "논리 실행자"(Server에 넘긴 io_context, 스레드 1개) 위에서만 실행된다. ChatRoom/GameService/Server는
//    스레드 안전하지 않고 이 전제로 동작한다. 이 전제는 바뀌지 않는다.
//  - 입출력 구역: 소켓, 읽기 버퍼, 송신 큐는 이 연결 전용 strand 위에서만 만진다. 소켓 읽기/쓰기 시스템
//    호출은 입출력 스레드 풀(Server의 io_threads, 0이면 논리 실행자와 같은 스레드)이 처리한다.
//
// 두 구역은 dispatch로만 넘나든다. 논리 -> 입출력: 송신(Deliver), 읽기 재개, 소켓 닫기. 입출력 -> 논리: 읽은
// 바이트 처리, 읽기/쓰기 오류와 송신 큐 초과로 인한 Close. 논리 실행자와 입출력 풀이 같으면(io_threads=0)
// dispatch가 그 자리에서 바로 실행되므로 예전의 단일 스레드 동작과 같다.
//
// 읽기는 한 번에 하나만 걸고, 읽은 바이트를 논리 구역이 다 처리한 뒤에 다음 읽기를 건다. 그래서 읽기 버퍼는
// 복사 없이 넘겨도 안전하고, 처리가 느린 연결은 TCP 흐름 제어로 자연스럽게 늦춰진다(예전과 같다).

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio.hpp>

#include "chat/chat_client.h"
#include "chat/chat_room.h"
#include "play/game_service.h"
#include "protocol/frame.h"
#include "protocol/message_ids.h"
#include "server/stats.h"

namespace game::server {

// 연결이 닫힌 이유. 로그와 통계(Stats)를 구분하는 데 쓴다.
enum class CloseReason {
  kPeerClosed,         // 상대가 정상적으로 연결을 닫음 (프레임 경계에서)
  kTruncated,          // 상대가 프레임 중간에 연결을 닫음
  kReadError,          // 읽기 중 소켓 오류
  kWriteError,         // 쓰기 중 소켓 오류
  kDecodeError,        // 길이 필드가 허용 범위(2~65,536) 밖
  kUnknownMessageId,   // 서버가 처리하지 않는 메시지 ID
  kParseError,         // Protobuf 파싱 실패
  kSendQueueOverflow,  // 송신 큐에 쌓인 바이트가 한도를 넘음 (느린 수신자)
  kInternalError,      // 서버 내부 문제 (정상이라면 발생하지 않음)
  kServerShutdown,     // 서버가 종료되며 연결을 닫음
};

struct SessionOptions {
  // 연결 하나의 송신 큐에 아직 소켓에 쓰지 못한 바이트가 이 값을 넘으면 연결을 끊는다.
  // 응답을 읽지 않는 클라이언트 때문에 서버 메모리가 무한히 늘어나는 것을 막는 안전장치다.
  // (무제한 큐와 제한 큐 비교는 3단계 실험 후보이므로, 지금은 기본 안전값만 둔다)
  std::size_t max_pending_send_bytes = 4 * 1024 * 1024;

  // true이면 TCP_NODELAY를 켜서 작은 패킷도 모으지 않고 바로 보낸다(기본). false이면 운영체제가 Nagle
  // 알고리즘으로 작은 패킷을 모아 보낸다. 3단계 실험 2(즉시 송신 vs 모아 보내기)를 위한 스위치다.
  bool tcp_nodelay = true;
};

class Session : public std::enable_shared_from_this<Session>, public chat::ChatClient {
 public:
  // 세션이 닫힐 때 서버에게 알리는 콜백. 서버는 이걸로 자신의 세션 목록에서 이 세션을 지운다.
  using CloseCallback = std::function<void(std::uint64_t session_id)>;

  // socket: accept로 얻은 연결된 소켓(소유권을 넘겨받는다). 입출력 스레드 풀의 io_context 위에서 만든 것이다.
  // id:     서버가 부여한 연결 번호. 로그에서 연결을 구분하는 용도.
  // stats:  서버 전체 통계. 서버보다 오래 살지 않는다는 전제로 참조만 저장한다.
  // room:   이 연결이 속할 수 있는 채팅 방. 서버가 소유하며 세션보다 오래 산다.
  // game:   이 연결이 들어갈 수 있는 게임 방들을 관리하는 서비스. 서버가 소유하며 세션보다 오래 산다.
  // logic:  논리 실행자. 방/서비스/서버를 호출하는 일은 모두 이 위에서 한다(스레드 1개 전제).
  // io_keepalive: 소켓이 속한 입출력 풀(io_context)을 세션이 사라질 때까지 살려 두는 공유 소유권. 소켓은 자신이
  //         속한 io_context보다 먼저 소멸해야 하기 때문이다. 입출력 풀이 따로 없으면 nullptr.
  Session(boost::asio::ip::tcp::socket socket, std::uint64_t id, Stats& stats,
          const SessionOptions& options, chat::ChatRoom& room, play::GameService& game,
          boost::asio::any_io_executor logic, std::shared_ptr<void> io_keepalive, CloseCallback on_close);

  // 통계와 로그를 갱신하고 첫 읽기를 시작한다. shared_ptr로 만든 뒤에 논리 실행자에서 호출해야 한다.
  void Start();

  // 연결을 닫는다. 논리 실행자에서만 부른다. 여러 번 호출해도 처음 한 번만 동작한다(멱등).
  // (입출력 구역에서 닫아야 할 일이 생기면 RequestClose가 논리 실행자로 넘겨서 부른다)
  void Close(CloseReason reason, std::string_view detail = {});

  std::uint64_t id() const { return id_; }

  // ---- ChatClient 인터페이스: 채팅 방이 이 연결에 메시지를 보낼 때 쓴다 (논리 실행자에서 호출) ----
  std::uint64_t client_id() const override { return id_; }
  void Deliver(protocol::MessageId id, const google::protobuf::MessageLite& message) override;

  // 아직 소켓에 쓰지 못한 프레임 수. 송신 큐 크기 관찰용(어느 스레드에서 읽어도 된다. 값은 약간 늦을 수 있다).
  std::size_t send_queue_size() const { return send_queue_depth_.load(std::memory_order_relaxed); }

 private:
  // ---- 입출력 구역(strand) -> 논리 구역으로 넘기는 일 ----
  // 입출력 구역에서 연결을 닫아야 할 때 부른다. 논리 실행자에서 Close를 실행하게 넘긴다.
  void RequestClose(CloseReason reason, std::string detail);

  // ---- 수신 ----
  // [strand] 읽기 한 번을 건다. 이전에 읽은 바이트를 논리 구역이 다 처리한 뒤에만 부른다.
  void DoRead();
  // [strand] 읽기가 끝났다. 오류면 논리 구역에 넘기고, 아니면 읽은 바이트를 논리 구역에서 처리하게 넘긴다.
  void OnRead(const boost::system::error_code& ec, std::size_t bytes_transferred);
  // [논리] 읽기 오류/상대 종료를 닫는 이유로 가려 Close한다.
  void OnReadError(const boost::system::error_code& ec);
  // [논리] 읽은 바이트를 디코더에 넣고 프레임을 처리한 뒤, 다음 읽기를 strand에 요청한다.
  void OnData(std::size_t bytes_transferred);
  void ProcessFrames();
  void HandleFrame(const protocol::Frame& frame);
  void HandlePing(const protocol::Frame& frame);
  void HandleChatFrame(const protocol::Frame& frame);
  void HandleGameFrame(const protocol::Frame& frame);

  // 프레임 본문을 message로 파싱한다. 실패하면 연결을 닫고(kParseError) false를 돌려준다.
  bool ParsePayload(const protocol::Frame& frame, google::protobuf::MessageLite& message, const char* name);

  // [논리] message를 직렬화해 프레임으로 만들고, 송신 큐에 넣는 일을 strand에 넘긴다. 직렬화는 호출한
  // 스레드(논리 스레드)에서 한 번만 한다. 응답(Ping)과 방의 전송(Deliver)이 함께 쓴다.
  void SendMessage(protocol::MessageId id, const google::protobuf::MessageLite& message);

  // ---- 송신 [strand] ----
  void Send(std::vector<std::uint8_t> wire_bytes);
  void DoWrite();
  void OnWrite(const boost::system::error_code& ec, std::size_t bytes_transferred);
  // 소켓을 닫고 이후 새 입출력을 시작하지 않게 표시한다. Close가 넘겨 준다.
  void CloseSocket();

  static constexpr std::size_t kReadBufferSize = 8192;

  // 가장 먼저 선언해 가장 나중에 소멸시킨다(소켓보다 오래 살아야 한다).
  const std::shared_ptr<void> io_keepalive_;

  // 읽기 오류 등으로 닫을 때 논리 구역에서 상태를 읽으므로, 아래 상수 멤버는 어디서나 읽어도 된다.
  const std::uint64_t id_;
  Stats& stats_;
  const SessionOptions options_;
  chat::ChatRoom& room_;
  play::GameService& game_;
  boost::asio::any_io_executor logic_;                      // 논리 실행자
  boost::asio::strand<boost::asio::any_io_executor> strand_;  // 이 연결의 입출력 직렬화
  CloseCallback on_close_;

  // ---- 입출력 구역: strand에서만 만진다 ----
  boost::asio::ip::tcp::socket socket_;
  // 수신: 소켓에서 읽은 바이트를 담는 고정 버퍼. 읽기가 완료된 뒤 논리 구역이 처리를 마칠 때까지는
  // 다음 읽기를 걸지 않으므로, 그동안 논리 구역이 이 버퍼를 읽어도 안전하다.
  std::array<std::uint8_t, kReadBufferSize> read_buffer_{};
  // 송신: 보낼 프레임(완성된 바이트열)을 쌓아 두는 큐. 앞에서부터 하나씩 쓴다.
  // 큐의 맨 앞 요소는 async_write가 끝날 때까지 메모리에 그대로 있어야 한다.
  std::deque<std::vector<std::uint8_t>> send_queue_;
  std::size_t pending_send_bytes_ = 0;  // send_queue_에 쌓인 총 바이트
  bool writing_ = false;                // async_write가 진행 중인지
  bool io_closed_ = false;              // 소켓을 닫았거나 닫기로 한 뒤: 새 입출력을 시작하지 않는다

  // ---- 논리 구역: 논리 실행자에서만 만진다 ----
  std::string remote_;  // "ip:port" 형태의 상대 주소. 로그용.
  protocol::FrameDecoder decoder_;
  std::uint64_t frames_received_ = 0;

  // 논리 구역이 쓰고 입출력 구역이 읽는다(닫힌 연결에 대한 쓸데없는 일을 빨리 그만두기 위한 용도).
  std::atomic<bool> closed_{false};

  // 입출력 구역이 쓰고 다른 구역이 읽는 카운터 (종료 로그, 관찰용)
  std::atomic<std::uint64_t> frames_sent_{0};
  std::atomic<std::size_t> max_send_queue_{0};
  std::atomic<std::size_t> send_queue_depth_{0};
};

}  // namespace game::server
