#include "account/mysql_account_repository.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

#include <boost/asio/cancel_after.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>

#include "common/error_text.h"

namespace game::account {

namespace {

namespace asio = boost::asio;

// 테이블 이름 검사: 영문, 숫자, 밑줄만 1~64자. 테이블 이름은 값처럼 바인딩할 수 없어 SQL 문자열에 직접 넣어야 하므로,
// 이상한 문자가 들어가 SQL이 깨지거나 변조되는 것을 막는다.
bool IsSafeIdentifier(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;
  return std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  });
}

std::int64_t NowUnixMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 오류 코드와 서버가 알려준 진단 메시지를 하나의 설명 문자열로 합친다. (비밀번호와 해시는 들어 있지 않다)
std::string Describe(const std::string& what, const boost::system::error_code& ec,
                     const boost::mysql::diagnostics& diag) {
  std::string text = what + ": " + common::ErrorText(ec);
  if (!diag.server_message().empty()) {
    text += " / 서버 메시지: " + std::string(diag.server_message());
  }
  return text;
}

}  // namespace

std::shared_ptr<MySqlAccountRepository> MySqlAccountRepository::Create(boost::asio::io_context& io,
                                                                       chat::MySqlOptions options,
                                                                       AccountTables tables) {
  if (!IsSafeIdentifier(tables.accounts) || !IsSafeIdentifier(tables.stats)) {
    throw std::invalid_argument("테이블 이름은 영문/숫자/밑줄만 쓸 수 있습니다: " + tables.accounts + ", " +
                                tables.stats);
  }
  // 생성자가 private라서 make_shared를 쓸 수 없다.
  return std::shared_ptr<MySqlAccountRepository>(new MySqlAccountRepository(io, std::move(options), std::move(tables)));
}

MySqlAccountRepository::MySqlAccountRepository(boost::asio::io_context& io, chat::MySqlOptions options,
                                               AccountTables tables)
    : io_(io),
      options_(std::move(options)),
      tables_(std::move(tables)),
      // 값은 모두 ?로 비워 두고 바인딩한다. 테이블 이름만 검증을 통과한 문자열을 백틱으로 감싸 넣는다.
      insert_sql_("INSERT INTO `" + tables_.accounts + "` (login_name, password_hash, created_at_unix_ms) VALUES (?, ?, ?)"),
      find_sql_("SELECT id, login_name, password_hash FROM `" + tables_.accounts + "` WHERE login_name = ?"),
      stats_sql_("SELECT wood_gathered, stone_gathered, houses_built FROM `" + tables_.stats +
                 "` WHERE account_id = ?") {}

// ============================================================ 작업 큐

void MySqlAccountRepository::Enqueue(Operation operation) {
  queue_.push_back(std::move(operation));
  if (running_) {
    return;  // 이미 큐를 처리하는 코루틴이 돌고 있다.
  }
  running_ = true;
  // 호출한 쪽이 반환한 "뒤에" 핸들러가 불리도록 post로 시작한다(AccountRepository의 규칙).
  asio::post(io_, [self = shared_from_this()] { asio::co_spawn(self->io_, Pump(self), asio::detached); });
}

asio::awaitable<void> MySqlAccountRepository::Pump(std::shared_ptr<MySqlAccountRepository> self) {
  while (!self->queue_.empty()) {
    Operation operation = std::move(self->queue_.front());
    self->queue_.pop_front();
    co_await operation();
  }
  self->running_ = false;
}

void MySqlAccountRepository::CreateAccount(std::string login_name, std::string password_hash,
                                           CreateHandler handler) {
  Enqueue([self = shared_from_this(), login_name = std::move(login_name), password_hash = std::move(password_hash),
           handler = std::move(handler)]() mutable {
    return DoCreate(self, std::move(login_name), std::move(password_hash), std::move(handler));
  });
}

void MySqlAccountRepository::FindAccount(std::string login_name, FindHandler handler) {
  Enqueue([self = shared_from_this(), login_name = std::move(login_name), handler = std::move(handler)]() mutable {
    return DoFind(self, std::move(login_name), std::move(handler));
  });
}

void MySqlAccountRepository::LoadStats(std::uint64_t account_id, StatsHandler handler) {
  Enqueue([self = shared_from_this(), account_id, handler = std::move(handler)]() mutable {
    return DoLoadStats(self, account_id, std::move(handler));
  });
}

// ============================================================ 연결

asio::awaitable<bool> MySqlAccountRepository::EnsureReady(std::string& error) {
  if (connection_) {
    co_return true;
  }

  auto connection = std::make_unique<boost::mysql::any_connection>(io_.get_executor());
  boost::mysql::connect_params params;
  params.server_address.emplace_host_and_port(options_.host, options_.port);
  params.username = options_.user;
  params.password = options_.password;
  params.database = options_.database;

  boost::system::error_code ec;
  boost::mysql::diagnostics diag;
  co_await connection->async_connect(
      params, diag, asio::cancel_after(options_.step_timeout, asio::redirect_error(asio::use_awaitable, ec)));
  if (ec) {
    error = Describe("DB 연결 실패 (" + options_.host + ":" + std::to_string(options_.port) + ")", ec, diag);
    co_return false;
  }

  // 세 문장을 준비한다. 한 문장이라도 실패하면 연결을 버린다(accounts/account_stats 테이블이 없으면 여기서 실패).
  const std::string* const sqls[3] = {&insert_sql_, &find_sql_, &stats_sql_};
  boost::mysql::statement statements[3];
  for (int i = 0; i < 3; ++i) {
    diag.clear();
    statements[i] = co_await connection->async_prepare_statement(
        *sqls[i], diag, asio::cancel_after(options_.step_timeout, asio::redirect_error(asio::use_awaitable, ec)));
    if (ec) {
      error = Describe("문장 준비 실패 (accounts, account_stats 테이블이 있는지 db_migrate로 확인)", ec, diag);
      co_return false;
    }
  }

  connection_ = std::move(connection);
  insert_statement_ = statements[0];
  find_statement_ = statements[1];
  stats_statement_ = statements[2];
  co_return true;
}

// ============================================================ 작업

asio::awaitable<void> MySqlAccountRepository::DoCreate(std::shared_ptr<MySqlAccountRepository> self,
                                                       std::string login_name, std::string password_hash,
                                                       CreateHandler handler) {
  RepoResult<AccountRecord> result;
  result.error = RepoError::kFailure;
  if (!co_await self->EnsureReady(result.message)) {
    handler(std::move(result));
    co_return;
  }

  boost::mysql::results rows;
  boost::mysql::diagnostics diag;
  boost::system::error_code ec;
  co_await self->connection_->async_execute(
      self->insert_statement_.bind(login_name, password_hash, NowUnixMillis()), rows, diag,
      asio::cancel_after(self->options_.step_timeout, asio::redirect_error(asio::use_awaitable, ec)));
  if (ec == boost::mysql::common_server_errc::er_dup_entry) {
    // UNIQUE 제약 위반: 이미 있는 ID. 연결은 정상이므로 그대로 쓴다.
    result.error = RepoError::kDuplicate;
    result.message = "이미 있는 ID";
    handler(std::move(result));
    co_return;
  }
  if (ec) {
    // 응답을 못 받았다. 서버가 이미 반영했는지 알 수 없으므로 재시도하지 않고 연결만 버린다.
    result.message = Describe("계정 저장 실패", ec, diag);
    self->connection_.reset();
    handler(std::move(result));
    co_return;
  }

  result.ok = true;
  result.error = RepoError::kNone;
  result.message.clear();
  result.value.id = rows.last_insert_id();
  result.value.login_name = std::move(login_name);
  result.value.password_hash = std::move(password_hash);
  handler(std::move(result));
}

asio::awaitable<void> MySqlAccountRepository::DoFind(std::shared_ptr<MySqlAccountRepository> self,
                                                     std::string login_name, FindHandler handler) {
  RepoResult<std::optional<AccountRecord>> result;
  result.error = RepoError::kFailure;
  if (!co_await self->EnsureReady(result.message)) {
    handler(std::move(result));
    co_return;
  }

  boost::mysql::results rows;
  boost::mysql::diagnostics diag;
  boost::system::error_code ec;
  co_await self->connection_->async_execute(
      self->find_statement_.bind(login_name), rows, diag,
      asio::cancel_after(self->options_.step_timeout, asio::redirect_error(asio::use_awaitable, ec)));
  if (ec) {
    result.message = Describe("계정 조회 실패", ec, diag);
    self->connection_.reset();
    handler(std::move(result));
    co_return;
  }

  result.ok = true;
  result.error = RepoError::kNone;
  result.message.clear();
  if (!rows.rows().empty()) {
    const auto row = rows.rows().at(0);
    AccountRecord record;
    record.id = row.at(0).as_uint64();
    record.login_name = std::string(row.at(1).as_string());
    record.password_hash = std::string(row.at(2).as_string());
    result.value = std::move(record);
  }
  handler(std::move(result));
}

asio::awaitable<void> MySqlAccountRepository::DoLoadStats(std::shared_ptr<MySqlAccountRepository> self,
                                                          std::uint64_t account_id, StatsHandler handler) {
  RepoResult<AccountStats> result;
  result.error = RepoError::kFailure;
  if (!co_await self->EnsureReady(result.message)) {
    handler(std::move(result));
    co_return;
  }

  boost::mysql::results rows;
  boost::mysql::diagnostics diag;
  boost::system::error_code ec;
  co_await self->connection_->async_execute(
      self->stats_statement_.bind(account_id), rows, diag,
      asio::cancel_after(self->options_.step_timeout, asio::redirect_error(asio::use_awaitable, ec)));
  if (ec) {
    result.message = Describe("누적 기록 조회 실패", ec, diag);
    self->connection_.reset();
    handler(std::move(result));
    co_return;
  }

  result.ok = true;
  result.error = RepoError::kNone;
  result.message.clear();
  if (!rows.rows().empty()) {  // 기록 행이 아직 없으면 0 그대로
    const auto row = rows.rows().at(0);
    result.value.wood_gathered = row.at(0).as_uint64();
    result.value.stone_gathered = row.at(1).as_uint64();
    result.value.houses_built = row.at(2).as_uint64();
  }
  handler(std::move(result));
}

}  // namespace game::account
