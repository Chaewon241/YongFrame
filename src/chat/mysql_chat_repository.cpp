#include "chat/mysql_chat_repository.h"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

#include <boost/asio/cancel_after.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>

#include "common/error_text.h"

namespace game::chat {

namespace {

namespace asio = boost::asio;

// 테이블 이름 검사: 영문, 숫자, 밑줄만 1~64자. 테이블 이름은 값처럼 바인딩할 수 없어 SQL 문자열에 직접
// 넣어야 하므로, 이상한 문자가 들어가 SQL이 깨지거나 변조되는 것을 막는다.
bool IsSafeIdentifier(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;
  return std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  });
}

std::int64_t NowUnixMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 오류 코드와 서버가 알려준 진단 메시지를 하나의 설명 문자열로 합친다. (비밀번호는 들어 있지 않다)
std::string Describe(const std::string& what, const boost::system::error_code& ec,
                     const boost::mysql::diagnostics& diag) {
  std::string text = what + ": " + common::ErrorText(ec);
  if (!diag.server_message().empty()) {
    text += " / 서버 메시지: " + std::string(diag.server_message());
  }
  return text;
}

}  // namespace

std::shared_ptr<MySqlChatRepository> MySqlChatRepository::Create(boost::asio::io_context& io,
                                                                 MySqlOptions options) {
  if (!IsSafeIdentifier(options.table)) {
    throw std::invalid_argument("테이블 이름은 영문/숫자/밑줄만 쓸 수 있습니다: " + options.table);
  }
  // 생성자가 private라서 make_shared를 쓸 수 없다.
  return std::shared_ptr<MySqlChatRepository>(new MySqlChatRepository(io, std::move(options)));
}

MySqlChatRepository::MySqlChatRepository(boost::asio::io_context& io, MySqlOptions options)
    : io_(io),
      options_(std::move(options)),
      // 값은 모두 ?로 비워 두고 바인딩한다. 테이블 이름만 검증을 통과한 문자열을 백틱으로 감싸 넣는다.
      insert_sql_("INSERT INTO `" + options_.table + "` (nickname, text, sent_at_unix_ms) VALUES (?, ?, ?)"),
      select_sql_("SELECT id, nickname, text, sent_at_unix_ms FROM `" + options_.table +
                  "` ORDER BY id DESC LIMIT ?") {}

// ============================================================ 작업 큐

void MySqlChatRepository::Enqueue(Operation operation) {
  queue_.push_back(std::move(operation));
  if (running_) {
    return;  // 이미 큐를 처리하는 코루틴이 돌고 있다. 그 코루틴이 이어서 처리한다.
  }
  running_ = true;
  // 호출한 쪽이 반환한 "뒤에" 핸들러가 불리도록 post로 시작한다. (ChatRepository의 규칙)
  asio::post(io_, [self = shared_from_this()] {
    asio::co_spawn(self->io_, Pump(self), asio::detached);
  });
}

// 큐가 빌 때까지 작업을 하나씩 순서대로 처리한다. 한 번에 하나라서 연결을 안전하게 공유한다.
asio::awaitable<void> MySqlChatRepository::Pump(std::shared_ptr<MySqlChatRepository> self) {
  while (!self->queue_.empty()) {
    Operation operation = std::move(self->queue_.front());
    self->queue_.pop_front();
    co_await operation();
  }
  self->running_ = false;
}

void MySqlChatRepository::SaveMessage(NewMessage message, SaveHandler handler) {
  Enqueue([self = shared_from_this(), message = std::move(message), handler = std::move(handler)]() mutable {
    return DoSave(self, std::move(message), std::move(handler));
  });
}

void MySqlChatRepository::LoadRecent(std::size_t limit, LoadHandler handler) {
  Enqueue([self = shared_from_this(), limit, handler = std::move(handler)]() mutable {
    return DoLoad(self, limit, std::move(handler));
  });
}

void MySqlChatRepository::CheckConnection(std::function<void(bool, std::string)> handler) {
  Enqueue([self = shared_from_this(), handler = std::move(handler)]() mutable {
    return DoCheck(self, std::move(handler));
  });
}

void MySqlChatRepository::DropConnectionForTest() { ResetConnection(); }

// ============================================================ 연결

// 연결이 없으면 새로 연결하고 두 개의 준비된 문장을 만든다.
// 각 단계(연결, 준비)에 제한 시간을 건다. 실패하면 연결 객체를 버려서 다음 작업이 처음부터 다시 시도한다.
asio::awaitable<bool> MySqlChatRepository::EnsureReady(std::string& error) {
  if (connection_) {
    co_return true;
  }

  auto connection = std::make_unique<boost::mysql::any_connection>(io_.get_executor());
  boost::mysql::connect_params params;
  params.server_address.emplace_host_and_port(options_.host, options_.port);
  params.username = options_.user;
  params.password = options_.password;
  params.database = options_.database;
  // 연결 문자 집합은 기본값(utf8mb4)을 쓴다. 한글과 이모지를 안전하게 주고받기 위해서다.

  boost::system::error_code ec;
  boost::mysql::diagnostics diag;
  co_await connection->async_connect(
      params, diag, asio::cancel_after(options_.step_timeout, asio::redirect_error(asio::use_awaitable, ec)));
  if (ec) {
    error = Describe("DB 연결 실패 (" + options_.host + ":" + std::to_string(options_.port) + ")", ec, diag);
    co_return false;
  }

  diag.clear();
  auto insert = co_await connection->async_prepare_statement(
      insert_sql_, diag, asio::cancel_after(options_.step_timeout, asio::redirect_error(asio::use_awaitable, ec)));
  if (ec) {
    error = Describe("INSERT 문장 준비 실패 (테이블 " + options_.table + "이 있는지 db_migrate로 확인)", ec, diag);
    co_return false;
  }

  diag.clear();
  auto select = co_await connection->async_prepare_statement(
      select_sql_, diag, asio::cancel_after(options_.step_timeout, asio::redirect_error(asio::use_awaitable, ec)));
  if (ec) {
    error = Describe("SELECT 문장 준비 실패", ec, diag);
    co_return false;
  }

  connection_ = std::move(connection);
  insert_statement_ = insert;
  select_statement_ = select;
  co_return true;
}

// ============================================================ 작업

asio::awaitable<void> MySqlChatRepository::DoCheck(std::shared_ptr<MySqlChatRepository> self,
                                                   std::function<void(bool, std::string)> handler) {
  std::string error;
  const bool ok = co_await self->EnsureReady(error);
  handler(ok, ok ? "연결과 문장 준비 성공" : error);
}

asio::awaitable<void> MySqlChatRepository::DoSave(std::shared_ptr<MySqlChatRepository> self, NewMessage message,
                                                  SaveHandler handler) {
  StorageResult<StoredMessage> result;
  if (!co_await self->EnsureReady(result.error)) {
    handler(std::move(result));
    co_return;
  }

  const std::int64_t sent_at = NowUnixMillis();
  boost::mysql::results rows;
  boost::mysql::diagnostics diag;
  boost::system::error_code ec;
  // 닉네임, 본문, 시각을 ?에 바인딩한다. 값은 SQL 문자열에 섞이지 않는다.
  co_await self->connection_->async_execute(
      self->insert_statement_.bind(message.nickname, message.text, sent_at), rows, diag,
      asio::cancel_after(self->options_.step_timeout, asio::redirect_error(asio::use_awaitable, ec)));
  if (ec) {
    // 응답을 못 받았다. 서버가 이미 반영했는지 알 수 없지만, 같은 메시지를 두 번 저장할 수 있으므로
    // 재시도하지 않는다. 연결만 버려서 다음 작업이 새로 연결하게 한다.
    result.error = Describe("메시지 저장 실패", ec, diag);
    self->ResetConnection();
    handler(std::move(result));
    co_return;
  }

  result.ok = true;
  result.value.id = rows.last_insert_id();
  result.value.nickname = std::move(message.nickname);
  result.value.text = std::move(message.text);
  result.value.sent_at_unix_ms = sent_at;
  handler(std::move(result));
}

asio::awaitable<void> MySqlChatRepository::DoLoad(std::shared_ptr<MySqlChatRepository> self, std::size_t limit,
                                                  LoadHandler handler) {
  StorageResult<std::vector<StoredMessage>> result;
  if (!co_await self->EnsureReady(result.error)) {
    handler(std::move(result));
    co_return;
  }

  boost::mysql::results rows;
  boost::mysql::diagnostics diag;
  boost::system::error_code ec;
  co_await self->connection_->async_execute(
      self->select_statement_.bind(static_cast<std::uint64_t>(limit)), rows, diag,
      asio::cancel_after(self->options_.step_timeout, asio::redirect_error(asio::use_awaitable, ec)));
  if (ec) {
    result.error = Describe("기록 조회 실패", ec, diag);
    self->ResetConnection();
    handler(std::move(result));
    co_return;
  }

  // 쿼리는 최신 것부터 가져오므로 뒤집어서 "오래된 것부터" 순서로 돌려준다.
  for (const auto row : rows.rows()) {
    StoredMessage stored;
    stored.id = row.at(0).as_uint64();
    stored.nickname = std::string(row.at(1).as_string());
    stored.text = std::string(row.at(2).as_string());
    stored.sent_at_unix_ms = row.at(3).as_int64();
    result.value.push_back(std::move(stored));
  }
  std::reverse(result.value.begin(), result.value.end());
  result.ok = true;
  handler(std::move(result));
}

}  // namespace game::chat
