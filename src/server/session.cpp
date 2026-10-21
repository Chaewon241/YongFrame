#include "server/session.h"

#include <utility>

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

// 소켓 오류를 "코드=값 메시지" 형태로 만든다. 윈도우는 메시지가 시스템 언어로 나오므로 코드도 함께 남긴다.
std::string DescribeError(const boost::system::error_code& ec) {
  return "코드=" + std::to_string(ec.value()) + " " + ec.message();
}

}  // namespace

Session::Session(boost::asio::ip::tcp::socket socket, std::uint64_t id, Stats& stats,
                 const SessionOptions& options, CloseCallback on_close)
    : socket_(std::move(socket)),
      id_(id),
      stats_(stats),
      options_(options),
      on_close_(std::move(on_close)) {}

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
  socket_.set_option(boost::asio::ip::tcp::no_delay(true), ec);

  ++stats_.total_accepted;
  const std::uint64_t current = ++stats_.current_connections;
  Stats::UpdateMax(stats_.peak_connections, current);
  Log(LogLevel::kInfo, "[연결] id=", id_, " 원격=", remote_, " 현재연결=", current);

  DoRead();
}

// 소켓에서 데이터를 비동기로 읽도록 요청한다. 완료되면 OnRead가 호출된다.
// 읽은 크기는 정해져 있지 않다(1바이트일 수도, 버퍼 크기만큼일 수도 있다). 그래서 read_some을 쓰고,
// 프레임으로 자르는 일은 디코더가 한다.
void Session::DoRead() {
  // 핸들러가 이 Session을 붙잡고 있게 한다. 이게 없으면 읽기가 끝나기 전에 서버 목록에서
  // 세션이 지워질 때 객체가 사라져 핸들러가 해제된 메모리를 쓰게 된다.
  auto self = shared_from_this();
  socket_.async_read_some(boost::asio::buffer(read_buffer_),
                          [this, self](const boost::system::error_code& ec, std::size_t bytes) {
                            OnRead(ec, bytes);
                          });
}

// 읽기가 끝났을 때 호출된다.
//  - 오류/상대 종료: 이유를 가려 Close
//  - 정상: 읽은 바이트를 디코더에 넣고 완성된 프레임을 처리한 뒤, 다음 읽기를 요청
void Session::OnRead(const boost::system::error_code& ec, std::size_t bytes_transferred) {
  // 우리가 Close()를 부른 뒤에 취소된 읽기가 완료 통지로 오는 경우다. 이미 정리됐으니 무시한다.
  if (closed_) {
    return;
  }

  if (ec) {
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
    return;
  }

  decoder_.Append(std::span<const std::uint8_t>(read_buffer_.data(), bytes_transferred));
  ProcessFrames();

  // ProcessFrames 안에서 연결이 닫혔다면 더 읽지 않는다.
  if (!closed_) {
    DoRead();
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

// 프레임의 메시지 ID를 보고 처리 함수로 보낸다. 1단계에서 서버가 받는 요청은 PingRequest뿐이다.
// 알 수 없는 ID뿐 아니라 PingResponse(서버가 보내는 쪽 메시지)가 들어와도 규약 위반으로 본다.
void Session::HandleFrame(const protocol::Frame& frame) {
  if (frame.message_id == protocol::ToUint16(protocol::MessageId::kPingRequest)) {
    HandlePing(frame);
    return;
  }
  Close(CloseReason::kUnknownMessageId, "message_id=" + std::to_string(frame.message_id));
}

// PingRequest를 처리한다. 받은 request_id와 payload를 그대로 PingResponse에 담아 되돌려준다.
void Session::HandlePing(const protocol::Frame& frame) {
  game::protocol::PingRequest request;
  if (!request.ParseFromArray(frame.payload.data(), static_cast<int>(frame.payload.size()))) {
    Close(CloseReason::kParseError,
          "PingRequest 파싱 실패 (payload " + std::to_string(frame.payload.size()) + "바이트)");
    return;
  }

  game::protocol::PingResponse response;
  response.set_request_id(request.request_id());
  response.set_payload(request.payload());

  std::vector<std::uint8_t> body(response.ByteSizeLong());
  response.SerializeToArray(body.data(), static_cast<int>(body.size()));

  // 응답 본문에 길이 필드와 메시지 ID를 붙여 완성된 프레임으로 만든다.
  auto wire = protocol::EncodeFrame(protocol::ToUint16(protocol::MessageId::kPingResponse), body);
  if (!wire) {
    // 응답은 요청과 같은 크기라 최대 크기를 넘을 수 없다. 여기 온다면 서버 버그다.
    Close(CloseReason::kInternalError, "응답 프레임이 최대 크기를 초과");
    return;
  }

  // 본문(payload)은 기본 로그에 남기지 않고, 요청 ID와 크기만 남긴다. (project.md 1단계 4번)
  Log(LogLevel::kDebug, "[요청] id=", id_, " request_id=", request.request_id(),
      " payload=", request.payload().size(), "B -> 응답 큐 등록");
  Send(std::move(*wire));
}

// 완성된 프레임을 송신 큐에 넣는다. 이미 쓰기가 진행 중이면 큐에 쌓아 두기만 하고,
// 진행 중이 아니면 쓰기를 시작한다. 한 연결에서 async_write를 동시에 두 번 걸면 바이트가 섞일 수
// 있으므로, 항상 "앞의 쓰기가 끝난 뒤 다음 쓰기" 순서를 지킨다. (docs/protocol.md)
void Session::Send(std::vector<std::uint8_t> wire_bytes) {
  if (closed_) {
    return;
  }

  // 한도 검사: 상대가 응답을 읽지 않으면 큐가 계속 쌓이므로 한도를 넘으면 연결을 끊는다.
  if (pending_send_bytes_ + wire_bytes.size() > options_.max_pending_send_bytes) {
    Close(CloseReason::kSendQueueOverflow,
          "대기 중 " + std::to_string(pending_send_bytes_) + "바이트, 한도 " +
              std::to_string(options_.max_pending_send_bytes) + "바이트");
    return;
  }

  pending_send_bytes_ += wire_bytes.size();
  send_queue_.push_back(std::move(wire_bytes));

  // 송신 큐 길이 관찰: 이 연결의 최대치와 서버 전체 최대치를 갱신한다.
  if (send_queue_.size() > max_send_queue_) {
    max_send_queue_ = send_queue_.size();
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
                           [this, self](const boost::system::error_code& ec, std::size_t bytes) {
                             OnWrite(ec, bytes);
                           });
}

// 쓰기가 끝났을 때 호출된다. 성공하면 맨 앞 프레임을 큐에서 빼고, 남은 프레임이 있으면 다음 쓰기를 시작한다.
void Session::OnWrite(const boost::system::error_code& ec, std::size_t /*bytes_transferred*/) {
  if (ec) {
    // Close()가 이미 불린 뒤에 취소된 쓰기라면 무시한다. 그렇지 않으면 쓰기 오류로 정리한다.
    if (!closed_) {
      Close(CloseReason::kWriteError, DescribeError(ec));
    }
    return;
  }

  ++frames_sent_;
  ++stats_.frames_sent;
  pending_send_bytes_ -= send_queue_.front().size();
  send_queue_.pop_front();

  if (closed_) {
    return;  // 쓰기가 진행되는 동안 연결이 닫혔다면 더 보내지 않는다.
  }
  if (send_queue_.empty()) {
    writing_ = false;
  } else {
    DoWrite();
  }
}

// 연결을 정리한다. 처음 한 번만 동작한다.
// 순서: 상태 표시 -> 로그/통계 -> 소켓 닫기 -> 서버에 알림
void Session::Close(CloseReason reason, std::string_view detail) {
  if (closed_) {
    return;
  }
  closed_ = true;

  CountClose(stats_, reason);
  const std::uint64_t remaining = --stats_.current_connections;
  Log(LevelFor(reason), "[종료] id=", id_, " 원격=", remote_, " 이유=", ToString(reason),
      detail.empty() ? "" : " (", detail, detail.empty() ? "" : ")", " 수신프레임=", frames_received_,
      " 송신프레임=", frames_sent_, " 최대송신큐=", max_send_queue_, " 현재연결=", remaining);

  // 소켓을 닫으면 진행 중이던 읽기/쓰기가 operation_aborted로 완료된다. 그 핸들러들은 closed_를 보고 바로 반환한다.
  // 주의: send_queue_는 여기서 비우지 않는다. 진행 중인 쓰기가 큐 맨 앞 버퍼를 아직 쓰고 있을 수 있고,
  // 윈도우(IOCP)는 취소가 완료되기 전까지 그 메모리를 계속 사용할 수 있다. 큐는 쓰기 핸들러가 끝나 이
  // 객체가 소멸할 때 함께 해제된다.
  boost::system::error_code ignored;
  socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
  socket_.close(ignored);

  if (on_close_) {
    on_close_(id_);
  }
}

}  // namespace game::server
