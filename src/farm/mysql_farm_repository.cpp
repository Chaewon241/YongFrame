#include "farm/mysql_farm_repository.h"

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

namespace game::farm {

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

std::string Describe(const std::string& what, const boost::system::error_code& ec,
                     const boost::mysql::diagnostics& diag) {
  std::string text = what + ": " + common::ErrorText(ec);
  if (!diag.server_message().empty()) {
    text += " / 서버 메시지: " + std::string(diag.server_message());
  }
  return text;
}

// 숫자 컬럼을 부호 있음/없음 어느 타입으로 와도 읽는다(MySQL 버전과 컬럼 정의에 따라 다르게 올 수 있다).
std::uint64_t FieldU64(boost::mysql::field_view field) {
  if (field.is_uint64()) return field.as_uint64();
  if (field.is_int64()) return static_cast<std::uint64_t>(field.as_int64());
  throw std::runtime_error("숫자로 읽을 수 없는 필드 (field_kind=" + std::to_string(static_cast<int>(field.kind())) + ")");
}

float FieldFloat(boost::mysql::field_view field) {
  if (field.is_float()) return field.as_float();
  if (field.is_double()) return static_cast<float>(field.as_double());
  throw std::runtime_error("실수로 읽을 수 없는 필드 (field_kind=" + std::to_string(static_cast<int>(field.kind())) + ")");
}

}  // namespace

std::shared_ptr<MySqlFarmRepository> MySqlFarmRepository::Create(boost::asio::io_context& io,
                                                                 chat::MySqlOptions options, FarmTables tables) {
  for (const std::string* name : {&tables.farms, &tables.structures, &tables.players, &tables.account_stats, &tables.clock}) {
    if (!IsSafeIdentifier(*name)) {
      throw std::invalid_argument("테이블 이름은 영문/숫자/밑줄만 쓸 수 있습니다: " + *name);
    }
  }
  // 생성자가 private라서 make_shared를 쓸 수 없다.
  return std::shared_ptr<MySqlFarmRepository>(new MySqlFarmRepository(io, std::move(options), std::move(tables)));
}

// 값은 모두 ?로 비워 두고 바인딩한다. 테이블 이름만 검증을 통과한 문자열을 백틱으로 감싸 넣는다.
// upsert는 별칭(AS new_row) 문법을 쓴다(MySQL 8.0.19 이상). 같은 키가 있으면 새 값으로 덮어쓴다(절대값).
MySqlFarmRepository::MySqlFarmRepository(boost::asio::io_context& io, chat::MySqlOptions options, FarmTables tables)
    : io_(io),
      options_(std::move(options)),
      tables_(std::move(tables)),
      codes_sql_("SELECT code FROM `" + tables_.farms + "`"),
      find_sql_("SELECT id, code, owner_account_id FROM `" + tables_.farms + "` WHERE owner_account_id = ?"),
      structures_sql_("SELECT structure_id, type, x, y, state, remaining_ticks, built_by_account_id FROM `" +
                      tables_.structures + "` WHERE farm_id = ? ORDER BY structure_id"),
      players_sql_("SELECT account_id, wood, stone FROM `" + tables_.players + "` WHERE farm_id = ?"),
      insert_farm_sql_("INSERT INTO `" + tables_.farms + "` (code, owner_account_id, created_at_unix_ms) VALUES (?, ?, ?)"),
      upsert_structure_sql_(
          "INSERT INTO `" + tables_.structures +
          "` (farm_id, structure_id, type, x, y, state, remaining_ticks, built_by_account_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
          "AS new_row ON DUPLICATE KEY UPDATE type = new_row.type, x = new_row.x, y = new_row.y, state = new_row.state, "
          "remaining_ticks = new_row.remaining_ticks, built_by_account_id = new_row.built_by_account_id"),
      upsert_player_sql_("INSERT INTO `" + tables_.players +
                         "` (farm_id, account_id, wood, stone, updated_at_unix_ms) VALUES (?, ?, ?, ?, ?) "
                         "AS new_row ON DUPLICATE KEY UPDATE wood = new_row.wood, stone = new_row.stone, "
                         "updated_at_unix_ms = new_row.updated_at_unix_ms"),
      upsert_stats_sql_("INSERT INTO `" + tables_.account_stats +
                        "` (account_id, wood_gathered, stone_gathered, houses_built, updated_at_unix_ms) "
                        "VALUES (?, ?, ?, ?, ?) AS new_row ON DUPLICATE KEY UPDATE wood_gathered = new_row.wood_gathered, "
                        "stone_gathered = new_row.stone_gathered, houses_built = new_row.houses_built, "
                        "updated_at_unix_ms = new_row.updated_at_unix_ms"),
      clock_sql_("SELECT day, day_elapsed_ticks FROM `" + tables_.clock + "` WHERE farm_id = ?"),
      upsert_clock_sql_("INSERT INTO `" + tables_.clock +
                        "` (farm_id, day, day_elapsed_ticks, updated_at_unix_ms) VALUES (?, ?, ?, ?) "
                        "AS new_row ON DUPLICATE KEY UPDATE day = new_row.day, day_elapsed_ticks = new_row.day_elapsed_ticks, "
                        "updated_at_unix_ms = new_row.updated_at_unix_ms") {}

// ============================================================ 작업 큐

void MySqlFarmRepository::Enqueue(Operation operation) {
  queue_.push_back(std::move(operation));
  if (running_) {
    return;  // 이미 큐를 처리하는 코루틴이 돌고 있다.
  }
  running_ = true;
  // 호출한 쪽이 반환한 "뒤에" 핸들러가 불리도록 post로 시작한다(FarmRepository의 규칙).
  asio::post(io_, [self = shared_from_this()] { asio::co_spawn(self->io_, Pump(self), asio::detached); });
}

asio::awaitable<void> MySqlFarmRepository::Pump(std::shared_ptr<MySqlFarmRepository> self) {
  while (!self->queue_.empty()) {
    Operation operation = std::move(self->queue_.front());
    self->queue_.pop_front();
    co_await operation();
  }
  self->running_ = false;
}

void MySqlFarmRepository::LoadFarmCodes(CodesHandler handler) {
  Enqueue([self = shared_from_this(), handler = std::move(handler)]() mutable { return DoLoadCodes(self, std::move(handler)); });
}

void MySqlFarmRepository::FindFarmByOwner(std::uint64_t owner_account_id, FindHandler handler) {
  Enqueue([self = shared_from_this(), owner_account_id, handler = std::move(handler)]() mutable {
    return DoFind(self, owner_account_id, std::move(handler));
  });
}

void MySqlFarmRepository::CreateFarm(std::uint64_t owner_account_id, std::string code, CreateHandler handler) {
  Enqueue([self = shared_from_this(), owner_account_id, code = std::move(code), handler = std::move(handler)]() mutable {
    return DoCreate(self, owner_account_id, std::move(code), std::move(handler));
  });
}

void MySqlFarmRepository::SaveFarm(FarmSave save, SaveHandler handler) {
  Enqueue([self = shared_from_this(), save = std::move(save), handler = std::move(handler)]() mutable {
    return DoSave(self, std::move(save), std::move(handler));
  });
}

// ============================================================ 연결과 실행

asio::awaitable<bool> MySqlFarmRepository::EnsureReady(std::string& error) {
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

  // 문장을 모두 준비한다. 하나라도 실패하면 연결을 버린다(농장 테이블이 없으면 여기서 실패한다).
  constexpr int kStatements = 10;
  const std::string* const sqls[kStatements] = {&codes_sql_,         &find_sql_,           &structures_sql_,
                                                &players_sql_,       &insert_farm_sql_,    &upsert_structure_sql_,
                                                &upsert_player_sql_, &upsert_stats_sql_,   &clock_sql_,
                                                &upsert_clock_sql_};
  boost::mysql::statement statements[kStatements];
  for (int i = 0; i < kStatements; ++i) {
    diag.clear();
    statements[i] = co_await connection->async_prepare_statement(
        *sqls[i], diag, asio::cancel_after(options_.step_timeout, asio::redirect_error(asio::use_awaitable, ec)));
    if (ec) {
      error = Describe("문장 준비 실패 (farms, farm_structures, farm_players, farm_clock 테이블이 있는지 db_migrate로 확인)", ec,
                       diag);
      co_return false;
    }
  }

  connection_ = std::move(connection);
  codes_stmt_ = statements[0];
  find_stmt_ = statements[1];
  structures_stmt_ = statements[2];
  players_stmt_ = statements[3];
  insert_farm_stmt_ = statements[4];
  upsert_structure_stmt_ = statements[5];
  upsert_player_stmt_ = statements[6];
  upsert_stats_stmt_ = statements[7];
  clock_stmt_ = statements[8];
  upsert_clock_stmt_ = statements[9];
  co_return true;
}

template <typename Bound>
asio::awaitable<bool> MySqlFarmRepository::Run(Bound bound, boost::mysql::results& rows, std::string& error,
                                               const char* what, boost::system::error_code* ec_out) {
  boost::mysql::diagnostics diag;
  boost::system::error_code ec;
  co_await connection_->async_execute(
      std::move(bound), rows, diag,
      asio::cancel_after(options_.step_timeout, asio::redirect_error(asio::use_awaitable, ec)));
  if (ec_out != nullptr) {
    *ec_out = ec;
  }
  if (ec) {
    error = Describe(what, ec, diag);
    co_return false;
  }
  co_return true;
}

asio::awaitable<bool> MySqlFarmRepository::RunSql(const char* sql, std::string& error) {
  boost::mysql::results rows;
  boost::mysql::diagnostics diag;
  boost::system::error_code ec;
  co_await connection_->async_execute(
      sql, rows, diag, asio::cancel_after(options_.step_timeout, asio::redirect_error(asio::use_awaitable, ec)));
  if (ec) {
    error = Describe(sql, ec, diag);
    co_return false;
  }
  co_return true;
}

// ============================================================ 작업

asio::awaitable<void> MySqlFarmRepository::DoLoadCodes(std::shared_ptr<MySqlFarmRepository> self, CodesHandler handler) {
  FarmResult<std::vector<std::string>> result;
  result.error = FarmError::kFailure;
  if (!co_await self->EnsureReady(result.message)) {
    handler(std::move(result));
    co_return;
  }
  boost::mysql::results rows;
  if (!co_await self->Run(self->codes_stmt_.bind(), rows, result.message, "농장 코드 조회 실패")) {
    self->connection_.reset();
    handler(std::move(result));
    co_return;
  }
  result.ok = true;
  result.error = FarmError::kNone;
  for (const auto row : rows.rows()) {
    result.value.emplace_back(row.at(0).as_string());
  }
  handler(std::move(result));
}

// 연결이 준비된 상태에서 이 계정의 농장과 구조물, 인벤토리를 읽는다.
asio::awaitable<bool> MySqlFarmRepository::ReadFarm(std::uint64_t owner, std::optional<FarmData>& out,
                                                    std::string& error) {
  out.reset();
  boost::mysql::results farm_rows;
  if (!co_await Run(find_stmt_.bind(owner), farm_rows, error, "농장 조회 실패")) {
    co_return false;
  }
  if (farm_rows.rows().empty()) {
    co_return true;  // 없다(오류가 아니다)
  }
  FarmData data;
  const auto farm_row = farm_rows.rows().at(0);
  data.farm_id = FieldU64(farm_row.at(0));
  data.code = std::string(farm_row.at(1).as_string());
  data.owner_account_id = FieldU64(farm_row.at(2));

  boost::mysql::results structure_rows;
  if (!co_await Run(structures_stmt_.bind(data.farm_id), structure_rows, error, "구조물 조회 실패")) {
    co_return false;
  }
  for (const auto row : structure_rows.rows()) {
    StoredStructure structure;
    structure.structure_id = static_cast<std::uint32_t>(FieldU64(row.at(0)));
    structure.type = static_cast<std::uint32_t>(FieldU64(row.at(1)));
    structure.x = FieldFloat(row.at(2));
    structure.y = FieldFloat(row.at(3));
    structure.state = static_cast<std::uint32_t>(FieldU64(row.at(4)));
    structure.remaining_ticks = FieldU64(row.at(5));
    structure.built_by_account_id = FieldU64(row.at(6));
    data.structures.push_back(structure);
  }

  boost::mysql::results player_rows;
  if (!co_await Run(players_stmt_.bind(data.farm_id), player_rows, error, "인벤토리 조회 실패")) {
    co_return false;
  }
  for (const auto row : player_rows.rows()) {
    StoredPlayer player;
    player.account_id = FieldU64(row.at(0));
    player.wood = static_cast<std::uint32_t>(FieldU64(row.at(1)));
    player.stone = static_cast<std::uint32_t>(FieldU64(row.at(2)));
    data.players.push_back(player);
  }

  boost::mysql::results clock_rows;
  if (!co_await Run(clock_stmt_.bind(data.farm_id), clock_rows, error, "시계 조회 실패")) {
    co_return false;
  }
  if (!clock_rows.rows().empty()) {  // 행이 없으면 이 조각 전에 만든 농장이다(비워 둔다)
    const auto row = clock_rows.rows().at(0);
    StoredClock clock;
    clock.day = static_cast<std::uint32_t>(FieldU64(row.at(0)));
    clock.day_elapsed_ticks = FieldU64(row.at(1));
    data.clock = clock;
  }
  out = std::move(data);
  co_return true;
}

asio::awaitable<void> MySqlFarmRepository::DoFind(std::shared_ptr<MySqlFarmRepository> self, std::uint64_t owner,
                                                  FindHandler handler) {
  FarmResult<std::optional<FarmData>> result;
  result.error = FarmError::kFailure;
  if (!co_await self->EnsureReady(result.message)) {
    handler(std::move(result));
    co_return;
  }
  if (!co_await self->ReadFarm(owner, result.value, result.message)) {
    self->connection_.reset();
    result.value.reset();
    handler(std::move(result));
    co_return;
  }
  result.ok = true;
  result.error = FarmError::kNone;
  result.message.clear();
  handler(std::move(result));
}

asio::awaitable<void> MySqlFarmRepository::DoCreate(std::shared_ptr<MySqlFarmRepository> self, std::uint64_t owner,
                                                    std::string code, CreateHandler handler) {
  FarmResult<FarmData> result;
  result.error = FarmError::kFailure;
  if (!co_await self->EnsureReady(result.message)) {
    handler(std::move(result));
    co_return;
  }

  boost::mysql::results rows;
  boost::system::error_code ec;
  std::string error;
  const bool ok = co_await self->Run(self->insert_farm_stmt_.bind(code, owner, NowUnixMillis()), rows, error,
                                     "농장 만들기 실패", &ec);
  if (!ok && ec == boost::mysql::common_server_errc::er_dup_entry) {
    // UNIQUE 위반이다. 코드가 겹친 것인지 이 계정의 농장이 이미 있는 것인지 가린다(연결은 정상이라 그대로 쓴다).
    std::optional<FarmData> existing;
    std::string read_error;
    if (!co_await self->ReadFarm(owner, existing, read_error)) {
      self->connection_.reset();
      result.message = read_error;
      handler(std::move(result));
      co_return;
    }
    result.error = existing ? FarmError::kDuplicateOwner : FarmError::kDuplicateCode;
    result.message = existing ? "이 계정의 농장이 이미 있음" : "이미 있는 농장 코드";
    handler(std::move(result));
    co_return;
  }
  if (!ok) {
    // 응답을 못 받았다. 서버가 이미 반영했는지 알 수 없으므로 재시도하지 않고 연결만 버린다. 반영되었다면 다음 열기에서
    // 그 농장을 찾는다.
    self->connection_.reset();
    result.message = error;
    handler(std::move(result));
    co_return;
  }

  result.ok = true;
  result.error = FarmError::kNone;
  result.message.clear();
  result.value.farm_id = rows.last_insert_id();
  result.value.code = std::move(code);
  result.value.owner_account_id = owner;
  handler(std::move(result));
}

asio::awaitable<void> MySqlFarmRepository::DoSave(std::shared_ptr<MySqlFarmRepository> self, FarmSave save,
                                                  SaveHandler handler) {
  FarmResult<bool> result;
  result.error = FarmError::kFailure;
  if (!co_await self->EnsureReady(result.message)) {
    handler(std::move(result));
    co_return;
  }

  // 하나의 트랜잭션: 하나라도 실패하면 연결을 버려 전부 취소한다(호출한 쪽이 같은 묶음을 다시 보내도 된다).
  const auto abort_save = [&](const std::string& error) {
    self->connection_.reset();
    result.message = error;
  };
  std::string error;
  if (!co_await self->RunSql("START TRANSACTION", error)) {
    abort_save(error);
    handler(std::move(result));
    co_return;
  }
  boost::mysql::results ignored;
  const std::int64_t now = NowUnixMillis();
  // 시계를 먼저 쓴다. 뒤의 문장이 실패하면 시계도 함께 취소되는지 시험이 확인할 수 있게 하는 순서다.
  if (save.clock) {
    if (!co_await self->Run(self->upsert_clock_stmt_.bind(save.farm_id, save.clock->day, save.clock->day_elapsed_ticks, now),
                            ignored, error, "시계 저장 실패")) {
      abort_save(error);
      handler(std::move(result));
      co_return;
    }
  }
  for (const StoredStructure& s : save.structures) {
    if (!co_await self->Run(self->upsert_structure_stmt_.bind(save.farm_id, s.structure_id, s.type, s.x, s.y, s.state,
                                                              s.remaining_ticks, s.built_by_account_id),
                            ignored, error, "구조물 저장 실패")) {
      abort_save(error);
      handler(std::move(result));
      co_return;
    }
  }
  for (const StoredPlayer& p : save.players) {
    if (!co_await self->Run(self->upsert_player_stmt_.bind(save.farm_id, p.account_id, p.wood, p.stone, now), ignored,
                            error, "인벤토리 저장 실패")) {
      abort_save(error);
      handler(std::move(result));
      co_return;
    }
  }
  for (const StoredAccountStats& a : save.stats) {
    if (!co_await self->Run(self->upsert_stats_stmt_.bind(a.account_id, a.stats.wood_gathered, a.stats.stone_gathered,
                                                          a.stats.houses_built, now),
                            ignored, error, "누적 기록 저장 실패")) {
      abort_save(error);
      handler(std::move(result));
      co_return;
    }
  }
  if (!co_await self->RunSql("COMMIT", error)) {
    abort_save(error);
    handler(std::move(result));
    co_return;
  }

  result.ok = true;
  result.value = true;
  result.error = FarmError::kNone;
  result.message.clear();
  handler(std::move(result));
}

}  // namespace game::farm
