#pragma once

// Session: 클라이언트 연결 "하나"를 담당하는 클래스.
//
// 한 연결에서 일어나는 일의 흐름:
//
//   소켓에서 바이트 읽기 (DoRead / OnRead)
//     -> FrameDecoder에 넣어 프레임으로 자르기 (ProcessFrames)
//       -> 메시지 ID에 따라 처리 (HandleFrame)
//            Ping은 직접 처리하고, 입장/퇴장/채팅/기록 요청은 파싱한 뒤 채팅 방(ChatRoom)에 넘긴다
//         -> 응답을 송신 큐에 넣기 (SendMessage -> Send)
//           -> 큐에서 하나씩 꺼내 소켓에 쓰기 (DoWrite / OnWrite)
//
// 채팅 방은 이 클래스를 ChatClient 인터페이스로만 본다. 방이 이 연결에 메시지를 보내려면 Deliver를
// 부르고, Deliver는 SendMessage로 이어져 같은 송신 큐를 쓴다. 따라서 방이 보낸 메시지도 응답과 같은
// 순서 규칙과 송신 큐 한도를 따른다.
//
// 오류나 종료가 생기면 Close()가 연결을 정리하고, 이유를 로그와 통계에 남긴다.
//
// 스레드 규칙: 이 클래스는 스레드 안전하지 않다. 모든 핸들러가 같은 스레드(또는 같은 strand)에서
// 실행된다는 전제로 동작한다. 지금은 io_context를 스레드 1개로 돌리므로 항상 성립한다.
// 3단계에서 스레드를 늘릴 때는 소켓에 strand를 붙여 이 전제를 유지해야 한다.

#include <array>
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
};

class Session : public std::enable_shared_from_this<Session>, public chat::ChatClient {
 public:
  // 세션이 닫힐 때 서버에게 알리는 콜백. 서버는 이걸로 자신의 세션 목록에서 이 세션을 지운다.
  using CloseCallback = std::function<void(std::uint64_t session_id)>;

  // socket: accept로 얻은 연결된 소켓(소유권을 넘겨받는다).
  // id:     서버가 부여한 연결 번호. 로그에서 연결을 구분하는 용도.
  // stats:  서버 전체 통계. 서버보다 오래 살지 않는다는 전제로 참조만 저장한다.
  // room:   이 연결이 속할 수 있는 채팅 방. 서버가 소유하며 세션보다 오래 산다.
  Session(boost::asio::ip::tcp::socket socket, std::uint64_t id, Stats& stats,
          const SessionOptions& options, chat::ChatRoom& room, CloseCallback on_close);

  // 통계와 로그를 갱신하고 첫 읽기를 시작한다. shared_ptr로 만든 뒤에 호출해야 한다.
  void Start();

  // 연결을 닫는다. 여러 번 호출해도 처음 한 번만 동작한다(멱등).
  void Close(CloseReason reason, std::string_view detail = {});

  std::uint64_t id() const { return id_; }

  // ---- ChatClient 인터페이스: 채팅 방이 이 연결에 메시지를 보낼 때 쓴다 ----
  std::uint64_t client_id() const override { return id_; }
  void Deliver(protocol::MessageId id, const google::protobuf::MessageLite& message) override;

  // 아직 소켓에 쓰지 못한 프레임 수. 송신 큐 크기 관찰용.
  std::size_t send_queue_size() const { return send_queue_.size(); }

 private:
  // ---- 수신 ----
  void DoRead();
  void OnRead(const boost::system::error_code& ec, std::size_t bytes_transferred);
  void ProcessFrames();
  void HandleFrame(const protocol::Frame& frame);
  void HandlePing(const protocol::Frame& frame);
  void HandleChatFrame(const protocol::Frame& frame);

  // 프레임 본문을 message로 파싱한다. 실패하면 연결을 닫고(kParseError) false를 돌려준다.
  bool ParsePayload(const protocol::Frame& frame, google::protobuf::MessageLite& message, const char* name);

  // message를 직렬화해 프레임으로 만들어 송신 큐에 넣는다. 응답(Ping)과 방의 전송(Deliver)이 함께 쓴다.
  void SendMessage(protocol::MessageId id, const google::protobuf::MessageLite& message);

  // ---- 송신 ----
  void Send(std::vector<std::uint8_t> wire_bytes);
  void DoWrite();
  void OnWrite(const boost::system::error_code& ec, std::size_t bytes_transferred);

  static constexpr std::size_t kReadBufferSize = 8192;

  boost::asio::ip::tcp::socket socket_;
  const std::uint64_t id_;
  Stats& stats_;
  const SessionOptions options_;
  chat::ChatRoom& room_;
  CloseCallback on_close_;
  std::string remote_;  // "ip:port" 형태의 상대 주소. 로그용.

  // 수신: 소켓에서 읽은 바이트를 담는 고정 버퍼와, 바이트를 프레임으로 자르는 디코더.
  std::array<std::uint8_t, kReadBufferSize> read_buffer_{};
  protocol::FrameDecoder decoder_;

  // 송신: 보낼 프레임(완성된 바이트열)을 쌓아 두는 큐. 앞에서부터 하나씩 쓴다.
  // 큐의 맨 앞 요소는 async_write가 끝날 때까지 메모리에 그대로 있어야 한다.
  std::deque<std::vector<std::uint8_t>> send_queue_;
  std::size_t pending_send_bytes_ = 0;  // send_queue_에 쌓인 총 바이트
  bool writing_ = false;                // async_write가 진행 중인지

  bool closed_ = false;

  // 이 연결만의 카운터. 연결 종료 로그에 남긴다.
  std::uint64_t frames_received_ = 0;
  std::uint64_t frames_sent_ = 0;
  std::size_t max_send_queue_ = 0;
};

}  // namespace game::server
