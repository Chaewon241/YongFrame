#include "server/server.h"

#include <chrono>
#include <utility>
#include <vector>

#include "account/in_memory_account_repository.h"
#include "farm/in_memory_farm_repository.h"
#include "chat/in_memory_chat_repository.h"
#include "common/error_text.h"
#include "server/log.h"

namespace game::server {

namespace {

// accept가 오류(예: 열 수 있는 파일/소켓 수 초과)로 실패했을 때 다시 시도하기 전에 기다릴 시간.
// 기다리지 않고 바로 재시도하면 같은 오류가 계속 나는 동안 CPU를 100% 쓰게 된다.
constexpr std::chrono::milliseconds kAcceptRetryDelay{100};

// 로그인 실패 초과로 연결을 닫기 전에 마지막 응답이 나갈 시간을 주는 지연.
constexpr std::chrono::milliseconds kAuthCloseFlushDelay{200};

}  // namespace

Server::Server(boost::asio::io_context& io, ServerConfig config,
               std::shared_ptr<chat::ChatRepository> repository,
               std::shared_ptr<account::AccountRepository> accounts,
               std::shared_ptr<account::PasswordHasher> hasher,
               std::shared_ptr<farm::FarmRepository> farms)
    : io_pool_(config.io_threads > 0
                   ? std::make_shared<boost::asio::io_context>(static_cast<int>(config.io_threads))
                   : nullptr),
      io_pool_guard_(io_pool_ ? std::make_unique<PoolGuard>(boost::asio::make_work_guard(*io_pool_)) : nullptr),
      logic_io_(io),
      socket_io_(io_pool_ ? *io_pool_ : io),
      config_(std::move(config)),
      acceptor_(io),
      accept_retry_timer_(io),
      stats_timer_(io),
      repository_(repository ? std::move(repository)
                             : std::make_shared<chat::InMemoryChatRepository>(io)),
      room_(std::make_unique<chat::ChatRoom>(io, repository_, config_.chat)),
      account_repository_(accounts ? std::move(accounts) : std::make_shared<account::InMemoryAccountRepository>(io)),
      account_(account::AccountService::Create(
          io, account_repository_,
          hasher ? std::move(hasher) : std::make_shared<account::SodiumPasswordHasher>(),
          config_.account)),
      game_(std::make_unique<play::GameService>(io, config_.play)) {
  // 게임 서비스가 계정 서비스와 이어지는 곳: 로그인한 연결은 방에서 계정의 ID가 닉네임이고, 방 만들기는 그 계정의 농장을
  // 연다. 채집/건설은 계정의 누적 기록에 더해지고, 저장할 때 그 최신 값을 읽는다.
  play::AccountHooks hooks;
  hooks.login_of = [account = account_](std::uint64_t client_id) { return account->LoginOf(client_id); };
  hooks.stats_of = [account = account_](std::uint64_t client_id) { return account->StatsOf(client_id); };
  hooks.add_progress = [account = account_](std::uint64_t client_id, const account::AccountStats& delta) {
    return account->AddProgress(client_id, delta);
  };
  game_->SetAccountHooks(std::move(hooks));
  // 농장 저장소: 넘기지 않으면 메모리 구현이다(계정 저장소도 메모리 구현이면 누적 기록을 함께 저장해 준다).
  if (!farms) {
    farms = std::make_shared<farm::InMemoryFarmRepository>(
        io, std::dynamic_pointer_cast<account::InMemoryAccountRepository>(account_repository_));
  }
  game_->SetFarmRepository(std::move(farms));
  // 로그인 실패가 한도를 넘으면 계정 서비스가 연결을 닫아 달라고 요청한다.
  account_->SetCloseRequester([this](std::uint64_t client_id, std::string detail) {
    const auto it = sessions_.find(client_id);
    if (it == sessions_.end()) {
      return;
    }
    // 마지막 응답(ACCOUNT_TOO_MANY_ATTEMPTS)이 소켓으로 나갈 시간을 잠깐 준 뒤에 닫는다. 바로 닫으면 응답이 도착하기
    // 전에 연결이 끊겨 클라이언트가 이유를 알 수 없다. 그 사이의 로그인 시도는 계정 서비스가 일 없이 거절한다.
    const auto session = it->second;  // Close가 sessions_에서 지우므로 먼저 복사해 둔다
    auto timer = std::make_shared<boost::asio::steady_timer>(logic_io_);
    timer->expires_after(kAuthCloseFlushDelay);
    timer->async_wait([timer, session, detail = std::move(detail)](const boost::system::error_code&) {
      session->Close(CloseReason::kAuthFailures, detail);  // 이미 닫혔다면 아무 일도 하지 않는다(멱등)
    });
  });
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

Server::~Server() {
  if (!io_pool_) {
    return;
  }
  // 입출력 스레드가 아직 돌고 있다면 일감이 끝나기를 기다리지 않고 멈춘다(정상 종료(Stop)를 거쳤다면 이미 모든
  // 소켓 닫기가 넘어가 있다).
  io_pool_guard_.reset();
  io_pool_->stop();
  for (auto& thread : io_threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  // 풀의 대기열에 남은 핸들러는 세션을 붙잡고 있고 세션은 풀을 붙잡고 있어서(Session의 io_keepalive), 그대로 두면
  // 서로 붙잡은 채 영원히 해제되지 않는다. 스레드가 모두 끝난 지금 남은 핸들러를 이 스레드에서 처리해 비운다.
  // (이미 닫힌 연결에 대한 정리뿐이고, 논리 스레드로 넘기는 일은 대기열에 쌓일 뿐 실행되지 않는다)
  io_pool_->restart();
  io_pool_->poll();
}

void Server::Start() {
  Log(LogLevel::kInfo, "[서버] 시작 주소=", config_.bind_address, " 포트=", port_,
      " 송신큐한도=", config_.session.max_pending_send_bytes, "B",
      " nodelay=", config_.session.tcp_nodelay ? "on" : "off", " 입출력스레드=", config_.io_threads,
      " 하루=", config_.play.DayLengthTicks(), "틱", config_.play.bench_day_ticks > 0 ? "(bench)" : "");
  for (std::uint32_t i = 0; i < config_.io_threads; ++i) {
    io_threads_.emplace_back([pool = io_pool_] { pool->run(); });
  }
  DoAccept();
  game_->LoadFarmCodes();  // 농장 코드를 예약해 게스트 방이 같은 코드를 쓰지 않게 한다
  game_->StartTicking();
  if (config_.stats_interval_seconds > 0) {
    ScheduleStatsReport();
  }
}

// 새 연결 하나를 받도록 요청한다. 연결이 오면 OnAccept가 호출된다.
// 핸들러는 (오류, 이미 연결된 소켓)을 받는다. OnAccept가 끝나면 다시 DoAccept를 불러
// 계속 새 연결을 받는다.
void Server::DoAccept() {
  // 새 소켓은 입출력 풀의 io_context 위에서 만든다(io_threads가 0이면 논리 io_context). 핸들러는 acceptor가
  // 속한 논리 스레드에서 실행된다.
  acceptor_.async_accept(socket_io_, [this](const boost::system::error_code& ec,
                                            boost::asio::basic_stream_socket<boost::asio::ip::tcp, boost::asio::io_context::executor_type> socket) {
    OnAccept(ec, boost::asio::ip::tcp::socket(std::move(socket)));
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
      std::move(socket), id, stats_, config_.session, *room_, *game_, *account_, logic_io_.get_executor(), io_pool_,
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
    Log(LogLevel::kInfo, "[통계] ", StatsReport());
    ScheduleStatsReport();
  });
}

std::string Server::FullSummary() const {
  return stats_.Summary() + ' ' + room_->stats().Summary() + " 게임{" + game_->stats().Summary() + "} 계정{" +
         account_->stats().Summary() + "} 농장{" + game_->FarmSummary() + '}';
}

std::string Server::StatsReport() const {
  if (config_.stats_format == StatsFormat::kBlock) {
    return '\n' + FormatStatsBlock(FullSummary());  // "[통계]" 다음 줄부터 묶음별로
  }
  return FullSummary();
}

void Server::Stop() {
  if (stopped_) {
    return;
  }
  stopped_ = true;

  // 채팅 방을 먼저 정리한다. 진행 중인 저장 대기를 취소하고, 이어서 연결을 닫을 때 "퇴장했다"는
  // 알림이 남은 연결들에게 쏟아지지 않게 한다.
  room_->Shutdown();
  game_->Shutdown();
  account_->Shutdown();
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

  // 소켓 닫기를 입출력 스레드에 모두 넘겼다. 풀이 일감을 다 처리하면 스레드가 끝나도록 대기 표시를 푼다.
  io_pool_guard_.reset();
}

}  // namespace game::server
