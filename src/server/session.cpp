#include "server/session.h"

#include <utility>

#include "common/error_text.h"
#include "ping.pb.h"  // proto/ping.proto에서 빌드 때 생성되는 PingRequest / PingResponse
#include "protocol/message_ids.h"
#include "server/log.h"

namespace game::server {

namespace {

// CloseReason을 로그에 찍을 한글 문자열로 바꾼다.
const char* ToString(CloseReason reason) {
  switch (reason) {
    case CloseReason::kPeerClosed: return "상대가 연결을 닫음";
    case CloseReason::kTruncated: return "프레임 중간에 연결이 끊김";
    case CloseReason::kReadError: return "읽기 오류";
    case CloseReason::kWriteError: return "쓰기 오류";
    case CloseReason::kDecodeError: return "잘못된 프레임 길이";
    case CloseReason::kUnknownMessageId: return "알 수 없는 메시지 ID";
    case CloseReason::kParseError: return "Protobuf 파싱 실패";
    case CloseReason::kSendQueueOverflow: return "송신 큐 한도 초과";
    case CloseReason::kAuthFailures: return "로그인 실패 횟수 초과";
    case CloseReason::kInternalError: return "서버 내부 오류";
    case CloseReason::kServerShutdown: return "서버 종료";
  }
  return "알 수 없음";
}

// 종료 이유에 맞는 로그 레벨. 정상 종료는 INFO, 상대의 규약 위반은 WARN, 서버 문제는 ERROR.
LogLevel LevelFor(CloseReason reason) {
  switch (reason) {
    case CloseReason::kPeerClosed:
    case CloseReason::kServerShutdown:
      return LogLevel::kInfo;
    case CloseReason::kInternalError:
      return LogLevel::kError;
    default:
      return LogLevel::kWarn;
  }
}

// 종료 이유에 맞는 통계 카운터를 1 올린다. 정상 종료(kPeerClosed, kServerShutdown)는 올리지 않는다.
void CountClose(Stats& stats, CloseReason reason) {
  switch (reason) {
    case CloseReason::kTruncated: ++stats.truncated_connections; break;
    case CloseReason::kReadError:
    case CloseReason::kWriteError: ++stats.network_errors; break;
    case CloseReason::kDecodeError: ++stats.decode_errors; break;
    case CloseReason::kUnknownMessageId: ++stats.unknown_message_ids; break;
    case CloseReason::kParseError: ++stats.parse_errors; break;
    case CloseReason::kSendQueueOverflow: ++stats.send_queue_overflows; break;
    case CloseReason::kAuthFailures: ++stats.auth_failure_closes; break;
    case CloseReason::kInternalError: ++stats.internal_errors; break;
    case CloseReason::kPeerClosed:
    case CloseReason::kServerShutdown: break;
  }
}

// FrameDecoder가 알려준 오류 종류를 설명 문자열로 바꾼다.
std::string DescribeDecodeError(protocol::DecodeError error) {
  switch (error) {
    case protocol::DecodeError::kLengthTooSmall: return "body_length가 2 미만";
    case protocol::DecodeError::kLengthTooLarge: return "body_length가 65,536 초과";
    case protocol::DecodeError::kNone: break;
  }
  return "알 수 없는 디코드 오류";
}

// 소켓 오류를 "코드=값 메시지" 형태로 만든다. 윈도우 메시지는 ANSI 코드페이지라 UTF-8로 바꿔 쓰고,
// 메시지가 환경마다 달라지므로 코드 값도 함께 남긴다.
std::string DescribeError(const boost::system::error_code& ec) {
  return "코드=" + std::to_string(ec.value()) + " " + common::ErrorText(ec);
}

}  // namespace

Session::Session(boost::asio::ip::tcp::socket socket, std::uint64_t id, Stats& stats,
                 const SessionOptions& options, chat::ChatRoom& room, play::GameService& game,
                 account::AccountService& account, boost::asio::any_io_executor logic, std::shared_ptr<void> io_keepalive,
                 CloseCallback on_close)
    : io_keepalive_(std::move(io_keepalive)),
      id_(id),
      stats_(stats),
      options_(options),
      room_(room),
      game_(game),
      account_(account),
      logic_(std::move(logic)),
      // 이 연결의 strand는 소켓이 속한 입출력 실행자 위에 만든다(멤버 선언 순서상 socket_보다 먼저 만들어지므로
      // 아직 옮기지 않은 인자 socket에서 실행자를 얻는다).
      strand_(boost::asio::make_strand(socket.get_executor())),
      on_close_(std::move(on_close)),
      socket_(std::move(socket)) {}

// 연결이 시작될 때 한 번 호출된다. 상대 주소를 기록하고, 소켓 옵션을 설정하고, 통계를 올린 뒤
// 첫 읽기를 시작한다.
void Session::Start() {
  boost::system::error_code ec;

  // 상대 주소 문자열을 만든다. 이미 끊긴 연결이면 실패할 수 있으므로 실패해도 계속 진행한다.
  const auto endpoint = socket_.remote_endpoint(ec);
  remote_ = ec ? "(알 수 없음)" : endpoint.address().to_string() + ":" + std::to_string(endpoint.port());

  // TCP_NODELAY: 작은 패킷(Ping 응답)을 모아 보내려고 기다리는 Nagle 알고리즘을 끈다.
  // 켜 두면 응답 지연이 수십 ms 늘어 왕복 지연 측정이 왜곡된다. 모아 보내기는 3단계에서
  // "즉시 송신 vs 제한된 배치 송신"으로 따로 비교할 후보이므로 지금은 즉시 송신으로 둔다.
  socket_.set_option(boost::asio::ip::tcp::no_delay(options_.tcp_nodelay), ec);

  ++stats_.total_accepted;
  const std::uint64_t current = ++stats_.current_connections;
  Stats::UpdateMax(stats_.peak_connections, current);
  Log(LogLevel::kInfo, "[연결] id=", id_, " 원격=", remote_, " 현재연결=", current);

  auto self = shared_from_this();
  boost::asio::dispatch(strand_, [self] { self->DoRead(); });
}

// 입출력 구역에서 연결을 닫아야 할 때(읽기/쓰기 오류, 송신 큐 초과) 논리 실행자에서 Close를 실행하게 넘긴다.
void Session::RequestClose(CloseReason reason, std::string detail) {
  auto self = shared_from_this();
  boost::asio::dispatch(logic_, [self, reason, detail = std::move(detail)] { self->Close(reason, detail); });
}

// [strand] 소켓에서 데이터를 비동기로 읽도록 요청한다. 완료되면 OnRead가 호출된다.
// 읽은 크기는 정해져 있지 않다(1바이트일 수도, 버퍼 크기만큼일 수도 있다). 그래서 read_some을 쓰고,
// 프레임으로 자르는 일은 디코더가 한다.
void Session::DoRead() {
  if (io_closed_) {
    return;
  }
  // 핸들러가 이 Session을 붙잡고 있게 한다. 이게 없으면 읽기가 끝나기 전에 서버 목록에서
  // 세션이 지워질 때 객체가 사라져 핸들러가 해제된 메모리를 쓰게 된다.
  auto self = shared_from_this();
  socket_.async_read_some(boost::asio::buffer(read_buffer_),
                          boost::asio::bind_executor(strand_, [this, self](const boost::system::error_code& ec,
                                                                           std::size_t bytes) { OnRead(ec, bytes); }));
}

// [strand] 읽기가 끝났을 때 호출된다. 소켓을 직접 만지는 일은 여기까지고, 나머지는 논리 구역에 넘긴다.
//  - 오류/상대 종료: 논리 구역에서 이유를 가려 Close
//  - 정상: 읽은 바이트를 논리 구역에서 처리. 다음 읽기는 논리 구역이 처리를 마친 뒤에 요청한다.
void Session::OnRead(const boost::system::error_code& ec, std::size_t bytes_transferred) {
  // 우리가 소켓을 닫은 뒤에 취소된 읽기가 완료 통지로 오는 경우다. 이미 정리됐으니 무시한다.
  if (io_closed_) {
    return;
  }
  auto self = shared_from_this();
  if (ec) {
    boost::asio::dispatch(logic_, [self, ec] { self->OnReadError(ec); });
    return;
  }
  boost::asio::dispatch(logic_, [self, bytes_transferred] { self->OnData(bytes_transferred); });
}

// [논리] 읽기 오류/상대 종료를 닫는 이유로 가려 Close한다.
void Session::OnReadError(const boost::system::error_code& ec) {
  if (closed_) {
    return;
  }
  if (ec == boost::asio::error::eof) {
    // 상대가 연결을 닫았다. 디코더에 처리 못 한 바이트가 남아 있으면 프레임 도중에 끊긴 것이다.
    if (decoder_.buffered_bytes() > 0) {
      Close(CloseReason::kTruncated,
            "미완성 " + std::to_string(decoder_.buffered_bytes()) + "바이트가 남은 채 종료");
    } else {
      Close(CloseReason::kPeerClosed);
    }
  } else {
    Close(CloseReason::kReadError, DescribeError(ec));
  }
}

// [논리] 읽은 바이트를 디코더에 넣고 완성된 프레임을 처리한 뒤, 다음 읽기를 요청한다.
void Session::OnData(std::size_t bytes_transferred) {
  if (closed_) {
    return;
  }
  decoder_.Append(std::span<const std::uint8_t>(read_buffer_.data(), bytes_transferred));
  ProcessFrames();

  // ProcessFrames 안에서 연결이 닫혔다면 더 읽지 않는다.
  if (!closed_) {
    auto self = shared_from_this();
    boost::asio::dispatch(strand_, [self] { self->DoRead(); });
  }
}

// 디코더에 쌓인 바이트에서 완성된 프레임을 모두 꺼내 처리한다.
// 한 번의 읽기에 프레임이 여러 개 들어 있을 수 있으므로 더 나올 게 없을 때까지 반복한다.
// 이 반복 덕분에 "여러 요청이 한 번에 도착" 해도 요청 순서대로 응답이 큐에 들어간다.
void Session::ProcessFrames() {
  protocol::Frame frame;
  while (!closed_) {
    switch (decoder_.Next(frame)) {
      case protocol::DecodeStatus::kNeedMoreData:
        return;  // 다음 읽기에서 나머지가 오면 이어서 처리한다.
      case protocol::DecodeStatus::kError:
        Close(CloseReason::kDecodeError, DescribeDecodeError(decoder_.error()));
        return;
      case protocol::DecodeStatus::kFrame:
        ++frames_received_;
        ++stats_.frames_received;
        HandleFrame(frame);
        break;
    }
  }
}

// 프레임의 메시지 ID를 보고 처리 함수로 보낸다. 서버가 요청으로 받는 ID는 Ping, 채팅(입장, 퇴장, 채팅, 기록
// 조회), 게임(방 만들기, 방 입장, 방 나가기, 이동 입력, 채집)뿐이다. 알 수 없는 ID뿐 아니라 서버가 보내는 쪽
// 메시지(PingResponse, ChatBroadcast, WorldSnapshot 등)가 들어와도 규약 위반으로 보고 연결을 닫는다.
void Session::HandleFrame(const protocol::Frame& frame) {
  if (!protocol::IsClientRequestId(frame.message_id)) {
    Close(CloseReason::kUnknownMessageId, "message_id=" + std::to_string(frame.message_id));
    return;
  }
  switch (static_cast<protocol::MessageId>(frame.message_id)) {
    case protocol::MessageId::kPingRequest:
      HandlePing(frame);
      return;
    case protocol::MessageId::kCreateRoomRequest:
    case protocol::MessageId::kJoinRoomRequest:
    case protocol::MessageId::kLeaveRoomRequest:
    case protocol::MessageId::kMoveInput:
    case protocol::MessageId::kGatherRequest:
    case protocol::MessageId::kBuildRequest:
    case protocol::MessageId::kSleepRequest:
      HandleGameFrame(frame);
      return;
    case protocol::MessageId::kRegisterRequest:
    case protocol::MessageId::kLoginRequest:
    case protocol::MessageId::kProfileRequest:
      HandleAccountFrame(frame);
      return;
    default:
      HandleChatFrame(frame);
      return;
  }
}

// 입장/퇴장/채팅/기록 요청을 파싱해 채팅 방에 넘긴다. 파싱에 실패하면 프로토콜 위반이라 연결을 닫는다.
// (닉네임이 잘못된 UTF-8인 경우도 proto3 string 파싱이 실패하므로 여기서 걸린다)
// 입력 규칙 위반이나 상태 위반은 연결을 닫지 않고, 방이 오류 코드가 담긴 응답을 돌려준다.
void Session::HandleChatFrame(const protocol::Frame& frame) {
  // 방은 이 세션을 약한 참조로만 들고 있으므로, 호출하는 동안 살아 있는 강한 참조를 넘긴다.
  const std::shared_ptr<chat::ChatClient> self = shared_from_this();

  switch (static_cast<protocol::MessageId>(frame.message_id)) {
    case protocol::MessageId::kJoinRequest: {
      game::protocol::JoinRequest request;
      if (ParsePayload(frame, request, "JoinRequest")) {
        Log(LogLevel::kDebug, "[입장요청] id=", id_, " 닉네임 ", request.nickname().size(), "B");
        room_.HandleJoin(self, request);
      }
      return;
    }
    case protocol::MessageId::kLeaveRequest: {
      game::protocol::LeaveRequest request;
      if (ParsePayload(frame, request, "LeaveRequest")) {
        Log(LogLevel::kDebug, "[퇴장요청] id=", id_);
        room_.HandleLeave(self);
      }
      return;
    }
    case protocol::MessageId::kChatRequest: {
      game::protocol::ChatRequest request;
      if (ParsePayload(frame, request, "ChatRequest")) {
        // 채팅 본문은 기본 로그에 남기지 않고 크기만 남긴다.
        Log(LogLevel::kDebug, "[채팅요청] id=", id_, " 본문 ", request.text().size(), "B");
        room_.HandleChat(self, request);
      }
      return;
    }
    case protocol::MessageId::kHistoryRequest: {
      game::protocol::HistoryRequest request;
      if (ParsePayload(frame, request, "HistoryRequest")) {
        Log(LogLevel::kDebug, "[기록요청] id=", id_, " limit=", request.limit());
        room_.HandleHistory(self, request);
      }
      return;
    }
    default:
      break;  // IsClientRequestId가 걸러 주므로 여기 오지 않는다.
  }
  Close(CloseReason::kInternalError, "처리되지 않는 요청 ID " + std::to_string(frame.message_id));
}

// 방 만들기/입장/나가기/이동 입력/채집 요청을 파싱해 게임 서비스에 넘긴다. 파싱에 실패하면 프로토콜 위반이라
// 연결을 닫는다. 규칙 위반(잘못된 코드, 가득 참 등)은 서비스가 결과 코드가 담긴 응답을 돌려주고 연결은
// 유지한다. 이동 입력은 초당 수십 번 오므로 로그를 남기지 않는다.
void Session::HandleGameFrame(const protocol::Frame& frame) {
  const std::shared_ptr<chat::ChatClient> self = shared_from_this();

  switch (static_cast<protocol::MessageId>(frame.message_id)) {
    case protocol::MessageId::kCreateRoomRequest: {
      game::protocol::CreateRoomRequest request;
      if (ParsePayload(frame, request, "CreateRoomRequest")) {
        Log(LogLevel::kDebug, "[방만들기요청] id=", id_);
        game_.HandleCreateRoom(self, request);
      }
      return;
    }
    case protocol::MessageId::kJoinRoomRequest: {
      game::protocol::JoinRoomRequest request;
      if (ParsePayload(frame, request, "JoinRoomRequest")) {
        Log(LogLevel::kDebug, "[방입장요청] id=", id_);
        game_.HandleJoinRoom(self, request);
      }
      return;
    }
    case protocol::MessageId::kLeaveRoomRequest: {
      game::protocol::LeaveRoomRequest request;
      if (ParsePayload(frame, request, "LeaveRoomRequest")) {
        Log(LogLevel::kDebug, "[방나가기요청] id=", id_);
        game_.HandleLeaveRoom(self);
      }
      return;
    }
    case protocol::MessageId::kMoveInput: {
      game::protocol::MoveInput input;
      if (ParsePayload(frame, input, "MoveInput")) {
        game_.HandleMoveInput(self, input);
      }
      return;
    }
    case protocol::MessageId::kGatherRequest: {
      game::protocol::GatherRequest request;
      if (ParsePayload(frame, request, "GatherRequest")) {
        Log(LogLevel::kDebug, "[채집요청] id=", id_, " node=", request.node_id());
        game_.HandleGather(self, request);
      }
      return;
    }
    case protocol::MessageId::kBuildRequest: {
      game::protocol::BuildRequest request;
      if (ParsePayload(frame, request, "BuildRequest")) {
        Log(LogLevel::kDebug, "[건설요청] id=", id_);
        game_.HandleBuild(self, request);
      }
      return;
    }
    case protocol::MessageId::kSleepRequest: {
      game::protocol::SleepRequest request;
      if (ParsePayload(frame, request, "SleepRequest")) {
        Log(LogLevel::kDebug, "[잠요청] id=", id_, " sleep=", request.sleep() ? 1 : 0);
        game_.HandleSleep(self, request);
      }
      return;
    }
    default:
      break;  // HandleFrame이 걸러 주므로 여기 오지 않는다.
  }
  Close(CloseReason::kInternalError, "처리되지 않는 게임 요청 ID " + std::to_string(frame.message_id));
}

// 가입/로그인/내 기록 요청을 파싱해 계정 서비스에 넘긴다. 파싱에 실패하면 프로토콜 위반이라 연결을 닫는다.
// 비밀번호가 든 메시지이므로 본문은 로그에 남기지 않는다(요청이 왔다는 사실만 남긴다).
void Session::HandleAccountFrame(const protocol::Frame& frame) {
  const std::shared_ptr<chat::ChatClient> self = shared_from_this();

  switch (static_cast<protocol::MessageId>(frame.message_id)) {
    case protocol::MessageId::kRegisterRequest: {
      game::protocol::RegisterRequest request;
      if (ParsePayload(frame, request, "RegisterRequest")) {
        Log(LogLevel::kDebug, "[가입요청] id=", id_);
        account_.HandleRegister(self, request);
      }
      return;
    }
    case protocol::MessageId::kLoginRequest: {
      game::protocol::LoginRequest request;
      if (ParsePayload(frame, request, "LoginRequest")) {
        Log(LogLevel::kDebug, "[로그인요청] id=", id_);
        account_.HandleLogin(self, request);
      }
      return;
    }
    case protocol::MessageId::kProfileRequest: {
      game::protocol::ProfileRequest request;
      if (ParsePayload(frame, request, "ProfileRequest")) {
        account_.HandleProfile(self);
      }
      return;
    }
    default:
      break;  // HandleFrame이 걸러 주므로 여기 오지 않는다.
  }
  Close(CloseReason::kInternalError, "처리되지 않는 계정 요청 ID " + std::to_string(frame.message_id));
}

// 프레임 본문(payload)을 message로 파싱한다. 실패하면 연결을 닫고 false.
bool Session::ParsePayload(const protocol::Frame& frame, google::protobuf::MessageLite& message,
                           const char* name) {
  if (message.ParseFromArray(frame.payload.data(), static_cast<int>(frame.payload.size()))) {
    return true;
  }
  Close(CloseReason::kParseError,
        std::string(name) + " 파싱 실패 (payload " + std::to_string(frame.payload.size()) + "바이트)");
  return false;
}

// message를 [길이][ID][본문] 프레임으로 만들어 송신 큐에 넣는다.
void Session::SendMessage(protocol::MessageId id, const google::protobuf::MessageLite& message) {
  if (closed_) {
    return;
  }
  std::vector<std::uint8_t> body(message.ByteSizeLong());
  message.SerializeToArray(body.data(), static_cast<int>(body.size()));

  auto wire = protocol::EncodeFrame(protocol::ToUint16(id), body);
  if (!wire) {
    // 서버가 만든 메시지가 최대 크기를 넘었다. 서버 버그이므로 이 연결을 닫는다.
    Close(CloseReason::kInternalError, "전송할 프레임이 최대 크기를 초과 (message_id=" +
                                           std::to_string(protocol::ToUint16(id)) + ")");
    return;
  }
  // 송신 큐와 소켓은 strand가 맡는다. 입출력 스레드가 따로 있으면 여기서는 넘기기만 하고 돌아간다.
  auto self = shared_from_this();
  boost::asio::dispatch(strand_, [self, bytes = std::move(*wire)]() mutable { self->Send(std::move(bytes)); });
}

// ChatClient 인터페이스 구현. 채팅 방이 이 연결에 메시지를 보낸다.
void Session::Deliver(protocol::MessageId id, const google::protobuf::MessageLite& message) {
  SendMessage(id, message);
}

// PingRequest를 처리한다. 받은 request_id와 payload를 그대로 PingResponse에 담아 되돌려준다.
void Session::HandlePing(const protocol::Frame& frame) {
  game::protocol::PingRequest request;
  if (!ParsePayload(frame, request, "PingRequest")) {
    return;
  }

  game::protocol::PingResponse response;
  response.set_request_id(request.request_id());
  response.set_payload(request.payload());

  // 본문(payload)은 기본 로그에 남기지 않고, 요청 ID와 크기만 남긴다. (project.md 1단계 4번)
  Log(LogLevel::kDebug, "[요청] id=", id_, " request_id=", request.request_id(),
      " payload=", request.payload().size(), "B -> 응답 큐 등록");
  // 응답을 직렬화해 길이 필드와 메시지 ID를 붙이고 송신 큐에 넣는다.
  SendMessage(protocol::MessageId::kPingResponse, response);
}

// 완성된 프레임을 송신 큐에 넣는다. 이미 쓰기가 진행 중이면 큐에 쌓아 두기만 하고,
// 진행 중이 아니면 쓰기를 시작한다. 한 연결에서 async_write를 동시에 두 번 걸면 바이트가 섞일 수
// 있으므로, 항상 "앞의 쓰기가 끝난 뒤 다음 쓰기" 순서를 지킨다. (docs/protocol.md)
void Session::Send(std::vector<std::uint8_t> wire_bytes) {
  if (io_closed_) {
    return;
  }

  // 한도 검사: 상대가 응답을 읽지 않으면 큐가 계속 쌓이므로 한도를 넘으면 연결을 끊는다.
  if (pending_send_bytes_ + wire_bytes.size() > options_.max_pending_send_bytes) {
    // 더 쌓지 않는다. 실제 닫기(방/서비스 정리, 통계, 소켓 닫기)는 논리 구역이 한다.
    io_closed_ = true;
    RequestClose(CloseReason::kSendQueueOverflow,
                 "대기 중 " + std::to_string(pending_send_bytes_) + "바이트, 한도 " +
                     std::to_string(options_.max_pending_send_bytes) + "바이트");
    return;
  }

  pending_send_bytes_ += wire_bytes.size();
  send_queue_.push_back(std::move(wire_bytes));

  // 송신 큐 길이 관찰: 이 연결의 최대치와 서버 전체 최대치를 갱신한다.
  send_queue_depth_.store(send_queue_.size(), std::memory_order_relaxed);
  if (send_queue_.size() > max_send_queue_.load(std::memory_order_relaxed)) {
    max_send_queue_.store(send_queue_.size(), std::memory_order_relaxed);
  }
  Stats::UpdateMax(stats_.max_send_queue_depth, send_queue_.size());

  if (!writing_) {
    DoWrite();
  }
}

// 큐의 맨 앞 프레임 하나를 소켓에 쓰도록 요청한다. 완료되면 OnWrite가 호출된다.
// async_write는 요청한 바이트를 전부 쓸 때까지 내부에서 반복해 주므로(짧은 쓰기 처리),
// OnWrite가 불리면 이 프레임 전체가 전송된 것이다.
void Session::DoWrite() {
  writing_ = true;
  auto self = shared_from_this();
  // 큐의 맨 앞 요소를 가리키는 버퍼를 넘긴다. deque의 push_back은 기존 요소의 위치를 바꾸지
  // 않으므로, 쓰기가 끝날 때까지 새 프레임이 큐 뒤에 추가되어도 이 버퍼는 안전하다.
  boost::asio::async_write(socket_, boost::asio::buffer(send_queue_.front()),
                           boost::asio::bind_executor(strand_, [this, self](const boost::system::error_code& ec,
                                                                            std::size_t bytes) { OnWrite(ec, bytes); }));
}

// [strand] 쓰기가 끝났을 때 호출된다. 성공하면 맨 앞 프레임을 큐에서 빼고, 남은 프레임이 있으면 다음 쓰기를 시작한다.
void Session::OnWrite(const boost::system::error_code& ec, std::size_t /*bytes_transferred*/) {
  if (ec) {
    // 우리가 소켓을 닫은 뒤에 취소된 쓰기라면 무시한다. 그렇지 않으면 쓰기 오류로 정리한다.
    if (!io_closed_) {
      io_closed_ = true;
      RequestClose(CloseReason::kWriteError, DescribeError(ec));
    }
    return;
  }

  frames_sent_.fetch_add(1, std::memory_order_relaxed);
  ++stats_.frames_sent;
  pending_send_bytes_ -= send_queue_.front().size();
  send_queue_.pop_front();
  send_queue_depth_.store(send_queue_.size(), std::memory_order_relaxed);

  if (io_closed_) {
    return;  // 쓰기가 진행되는 동안 연결이 닫혔다면 더 보내지 않는다.
  }
  if (send_queue_.empty()) {
    writing_ = false;
  } else {
    DoWrite();
  }
}

// [strand] 소켓을 닫는다. 진행 중이던 읽기/쓰기가 operation_aborted로 완료되고, 그 핸들러들은 io_closed_를
// 보고 바로 반환한다. 주의: send_queue_는 여기서 비우지 않는다. 진행 중인 쓰기가 큐 맨 앞 버퍼를 아직 쓰고
// 있을 수 있고, 윈도우(IOCP)는 취소가 완료되기 전까지 그 메모리를 계속 사용할 수 있다. 큐는 쓰기 핸들러가
// 끝나 이 객체가 소멸할 때 함께 해제된다.
void Session::CloseSocket() {
  io_closed_ = true;
  boost::system::error_code ignored;
  socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
  socket_.close(ignored);
}

// 연결을 정리한다. 처음 한 번만 동작한다.
// 순서: 상태 표시 -> 로그/통계 -> 소켓 닫기 -> 서버에 알림
void Session::Close(CloseReason reason, std::string_view detail) {
  if (closed_.load()) {
    return;
  }
  closed_.store(true);

  // 입장해 있었다면 방에서 빼고 남은 사람에게 알린다. (입장하지 않았다면 방이 아무것도 하지 않는다)
  // 채팅 방과 게임 방 모두 같은 규칙이다.
  room_.OnDisconnect(id_);
  game_.OnDisconnect(id_);
  account_.OnDisconnect(id_);

  CountClose(stats_, reason);
  const std::uint64_t remaining = --stats_.current_connections;
  Log(LevelFor(reason), "[종료] id=", id_, " 원격=", remote_, " 이유=", ToString(reason),
      detail.empty() ? "" : " (", detail, detail.empty() ? "" : ")", " 수신프레임=", frames_received_,
      " 송신프레임=", frames_sent_.load(std::memory_order_relaxed),
      " 최대송신큐=", max_send_queue_.load(std::memory_order_relaxed), " 현재연결=", remaining);

  // 소켓은 입출력 구역의 것이므로 strand에서 닫는다(입출력 스레드가 따로 없으면 이 자리에서 바로 닫힌다).
  auto self = shared_from_this();
  boost::asio::dispatch(strand_, [self] { self->CloseSocket(); });

  if (on_close_) {
    on_close_(id_);
  }
}

}  // namespace game::server
