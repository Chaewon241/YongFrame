#include "server/server.h"

#include <chrono>
#include <utility>
#include <vector>

#include "common/error_text.h"
#include "server/log.h"

namespace game::server {

namespace {

// accept가 오류(예: 열 수 있는 파일/소켓 수 초과)로 실패했을 때 다시 시도하기 전에 기다릴 시간.
// 기다리지 않고 바로 재시도하면 같은 오류가 계속 나는 동안 CPU를 100% 쓰게 된다.
constexpr std::chrono::milliseconds kAcceptRetryDelay{100};

}  // namespace

Server::Server(boost::asio::io_context& io, ServerConfig config)
    : config_(std::move(config)),
      acceptor_(io),
      accept_retry_timer_(io),
      stats_timer_(io) {
  // 주소 문자열("127.0.0.1" 등)을 해석한다. 형식이 틀리면 예외가 나간다.
  boost::system::error_code ec;
  const auto address = boost::asio::ip::make_address(config_.bind_address, ec);
  if (ec) {
    throw boost::system::system_error(ec, "bind 주소 해석 실패: " + config_.bind_address);
  }
  const boost::asio::ip::tcp::endpoint endpoint(address, config_.port);

  acceptor_.open(endpoint.protocol());
#ifndef _WIN32
  // 리눅스: 서버를 껐다 바로 다시 켤 때 이전 연결의 TIME_WAIT 때문에 bind가 실패하지 않게 한다.
  // 윈도우에서는 SO_REUSEADDR가 "이미 쓰는 포트에 중복 바인드를 허용"하는 뜻이라 오히려 위험하므로
  // 켜지 않는다. (Asio의 acceptor 생성자 기본값이 이 옵션을 켜기 때문에 직접 open/bind 한다)
  acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
#endif
  acceptor_.bind(endpoint);
  acceptor_.listen();
  port_ = acceptor_.local_endpoint().port();
}

void Server::Start() {
  Log(LogLevel::kInfo, "[서버] 시작 주소=", config_.bind_address, " 포트=", port_,
      " 송신큐한도=", config_.session.max_pending_send_bytes, "B");
  DoAccept();
  if (config_.stats_interval_seconds > 0) {
    ScheduleStatsReport();
  }
}

// 새 연결 하나를 받도록 요청한다. 연결이 오면 OnAccept가 호출된다.
// 핸들러는 (오류, 이미 연결된 소켓)을 받는다. OnAccept가 끝나면 다시 DoAccept를 불러
// 계속 새 연결을 받는다.
void Server::DoAccept() {
  acceptor_.async_accept([this](const boost::system::error_code& ec, boost::asio::ip::tcp::socket socket) {
    OnAccept(ec, std::move(socket));
  });
}

// accept가 끝났을 때 호출된다.
//  - 오류: 서버 종료 중이면 조용히 끝낸다. 그 외에는 로그를 남기고 잠시 뒤 재시도한다.
//  - 성공: Session을 만들어 목록에 넣고 시작시킨 뒤, 다음 연결을 받으러 간다.
void Server::OnAccept(const boost::system::error_code& ec, boost::asio::ip::tcp::socket socket) {
  if (ec) {
    if (stopped_ || ec == boost::asio::error::operation_aborted) {
      return;  // Stop()이 acceptor를 닫아서 취소된 것. 정상 종료 경로이다.
    }
    Log(LogLevel::kError, "[서버] accept 실패 코드=", ec.value(), " ", common::ErrorText(ec));
    ScheduleAcceptRetry();
    return;
  }

  const std::uint64_t id = next_session_id_++;
  auto session = std::make_shared<Session>(
      std::move(socket), id, stats_, config_.session,
      // 세션이 닫히면 이 콜백이 불려 목록에서 지운다. 지워도 핸들러가 shared_ptr을 쥐고 있어
      // 진행 중인 핸들러가 끝날 때까지 세션 객체는 살아 있다.
      [this](std::uint64_t closed_id) { sessions_.erase(closed_id); });
  sessions_.emplace(id, session);
  session->Start();

  DoAccept();
}

// accept 오류 뒤 일정 시간을 기다렸다가 다시 accept를 시도한다.
void Server::ScheduleAcceptRetry() {
  accept_retry_timer_.expires_after(kAcceptRetryDelay);
  accept_retry_timer_.async_wait([this](const boost::system::error_code& ec) {
    if (!ec && !stopped_) {
      DoAccept();
    }
  });
}

// 통계를 한 줄 출력하고, 같은 주기로 다음 출력을 예약한다.
void Server::ScheduleStatsReport() {
  stats_timer_.expires_after(std::chrono::seconds(config_.stats_interval_seconds));
  stats_timer_.async_wait([this](const boost::system::error_code& ec) {
    if (ec || stopped_) {
      return;  // 타이머가 취소됨(Stop)
    }
    Log(LogLevel::kInfo, "[통계] ", stats_.Summary());
    ScheduleStatsReport();
  });
}

void Server::Stop() {
  if (stopped_) {
    return;
  }
  stopped_ = true;
  Log(LogLevel::kInfo, "[서버] 종료 시작 (열린 연결 ", sessions_.size(), "개)");

  // 새 연결 받기를 멈춘다. 진행 중인 async_accept는 operation_aborted로 끝난다.
  boost::system::error_code ignored;
  acceptor_.close(ignored);
  accept_retry_timer_.cancel();
  stats_timer_.cancel();

  // Session::Close가 on_close 콜백으로 sessions_에서 자신을 지우므로, 순회 중에 컨테이너가
  // 바뀌지 않도록 먼저 복사본을 만든 뒤 그 복사본으로 닫는다.
  std::vector<std::shared_ptr<Session>> snapshot;
  snapshot.reserve(sessions_.size());
  for (const auto& entry : sessions_) {
    snapshot.push_back(entry.second);
  }
  for (const auto& session : snapshot) {
    session->Close(CloseReason::kServerShutdown);
  }
}

}  // namespace game::server
