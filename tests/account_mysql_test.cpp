// MySQL 계정 저장소 시험: 실제 MySQL에 연결해 MySqlAccountRepository와, 그것을 쓰는 서버 전체를 검증한다.
//
// 실행 조건 (chat_mysql_test와 같다)
//  - DB 비밀번호가 환경변수 GAME_DB_PASSWORD 또는 GAME_DB_PASSWORD_FILE로 있어야 한다. 없으면 이 파일의 DB 시험은
//    "건너뜀(SKIPPED)"으로 표시된다. 비밀번호는 채팅 창에 쓰지 말고 환경변수나 파일로만 준다.
//  - 접속 정보는 기본 127.0.0.1:3306, 계정 game, 데이터베이스 game_chat이고, GAME_DB_HOST, GAME_DB_PORT,
//    GAME_DB_USER, GAME_DB_NAME 환경변수로 바꿀 수 있다.
//
// 운영 데이터 보호
//  - 이 시험은 accounts와 account_stats가 아니라 별도 테이블 accounts_test, account_stats_test만 쓴다. 매 시험 시작 때
//    그 테이블을 지우고 다시 만든다. 테이블은 db/migrations/002의 실제 SQL에서 이름만 바꿔 만들기 때문에, 마이그레이션
//    파일이 올바른지도 함께 검증된다.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <boost/mysql.hpp>

#include "account/mysql_account_repository.h"
#include "account/password_hasher.h"
#include "account_test_client.h"
#include "farm/mysql_farm_repository.h"
#include "common/db_credentials.h"
#include "test_support.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace game {
namespace {

namespace fs = std::filesystem;
using namespace testing_support;
using account::AccountRecord;
using account::AccountTables;
using account::MySqlAccountRepository;
using account::RepoError;
using account::RepoResult;
using chat::MySqlOptions;

constexpr const char* kAccountsTable = "accounts_test";
constexpr const char* kStatsTable = "account_stats_test";
constexpr const char* kFarmsTable = "farms_test";
constexpr const char* kFarmStructuresTable = "farm_structures_test";
constexpr const char* kFarmPlayersTable = "farm_players_test";

[[maybe_unused]] ::testing::Environment* const kQuietLog = ::testing::AddGlobalTestEnvironment(new QuietLogEnvironment);

AccountTables TestTables() { return AccountTables{kAccountsTable, kStatsTable}; }

// ============================================================ 환경 읽기

std::optional<MySqlOptions> OptionsFromEnvironment(std::string& skip_reason) {
  const auto password = common::ReadDbPassword();
  if (!password) {
    skip_reason = "DB 비밀번호(GAME_DB_PASSWORD 또는 GAME_DB_PASSWORD_FILE)가 없어 건너뜀";
    return std::nullopt;
  }
  MySqlOptions options;
  options.password = *password;
  if (const auto v = common::ReadEnvironment("GAME_DB_HOST")) options.host = *v;
  if (const auto v = common::ReadEnvironment("GAME_DB_PORT")) options.port = static_cast<std::uint16_t>(std::stoi(*v));
  if (const auto v = common::ReadEnvironment("GAME_DB_USER")) options.user = *v;
  if (const auto v = common::ReadEnvironment("GAME_DB_NAME")) options.database = *v;
  return options;
}

std::optional<fs::path> FindMigrationFile() {
  const std::vector<fs::path> starts = [] {
    std::vector<fs::path> v{fs::current_path()};
#ifdef _WIN32
    wchar_t buffer[MAX_PATH * 4];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
    if (length > 0) v.push_back(fs::path(std::wstring(buffer, length)).parent_path());
#endif
    return v;
  }();
  for (fs::path dir : starts) {
    for (int depth = 0; depth < 8 && !dir.empty(); ++depth) {
      const fs::path candidate = dir / "db" / "migrations" / "002_create_accounts.sql";
      if (fs::exists(candidate)) return candidate;
      dir = dir.parent_path();
    }
  }
  return std::nullopt;
}

std::uint64_t FieldToUint64(boost::mysql::field_view field) {
  if (field.is_uint64()) return field.as_uint64();
  if (field.is_int64()) return static_cast<std::uint64_t>(field.as_int64());
  if (field.is_string()) return std::stoull(std::string(field.as_string()));
  throw std::runtime_error("숫자로 읽을 수 없는 필드 (field_kind=" + std::to_string(static_cast<int>(field.kind())) + ")");
}

// 동기식 연결로 SQL을 한 번 실행한다. 시험 준비와 정리, 값 확인, 장애 만들기에 쓴다.
class AdminConnection {
 public:
  explicit AdminConnection(const MySqlOptions& options) : connection_(ctx_) {
    boost::mysql::connect_params params;
    params.server_address.emplace_host_and_port(options.host, options.port);
    params.username = options.user;
    params.password = options.password;
    params.database = options.database;
    params.multi_queries = true;
    boost::mysql::error_code ec;
    boost::mysql::diagnostics diag;
    connection_.connect(params, ec, diag);
    if (ec) {
      error_ = ec.message() + " / " + std::string(diag.server_message());
    }
  }

  bool ok() const { return error_.empty(); }
  const std::string& error() const { return error_; }

  bool Execute(const std::string& sql, boost::mysql::results& result) {
    boost::mysql::error_code ec;
    boost::mysql::diagnostics diag;
    connection_.execute(sql, result, ec, diag);
    if (ec) {
      error_ = ec.message() + " / " + std::string(diag.server_message()) + " / SQL: " + sql.substr(0, 80);
      return false;
    }
    return true;
  }

  // 이 계정의 다른 연결을 모두 끊는다. DB가 연결을 끊어 버린 상황을 흉내 낸다.
  int KillOtherConnections() {
    boost::mysql::results mine;
    if (!Execute("SELECT CONNECTION_ID()", mine)) return -1;
    const auto my_id = FieldToUint64(mine.rows().at(0).at(0));

    boost::mysql::results list;
    if (!Execute("SHOW PROCESSLIST", list)) return -1;
    std::vector<std::uint64_t> victims;
    for (const auto row : list.rows()) {
      const std::uint64_t id = FieldToUint64(row.at(0));
      if (id != my_id) victims.push_back(id);
    }
    int killed = 0;
    for (const auto id : victims) {
      boost::mysql::results ignored;
      if (Execute("KILL CONNECTION " + std::to_string(id), ignored)) ++killed;
    }
    return killed;
  }

 private:
  boost::asio::io_context ctx_;
  boost::mysql::any_connection connection_;
  std::string error_;
};

// 마이그레이션 파일(번호로 찾는다)의 내용을 읽는다.
std::optional<std::string> ReadMigration(const std::string& file_name) {
  const auto base = FindMigrationFile();  // 002를 기준으로 같은 폴더를 찾는다
  if (!base) return std::nullopt;
  std::ifstream file(base->parent_path() / file_name, std::ios::binary);
  if (!file) return std::nullopt;
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// 시험 전용 테이블을 실제 마이그레이션 SQL(002 계정, 003 농장)에서 이름만 바꿔 새로 만든다.
// 바꾸는 순서: 긴 이름을 먼저(farm_structures, farm_players, farms, account_stats), 그다음 accounts. 어느 이름도 다른
// 이름의 일부가 아니라서 안전하다.
bool ResetTestTables(const MySqlOptions& options, std::string& error) {
  const auto accounts_sql = ReadMigration("002_create_accounts.sql");
  const auto farms_sql = ReadMigration("003_create_farms.sql");
  if (!accounts_sql || !farms_sql) {
    error = "db/migrations/002_create_accounts.sql 또는 003_create_farms.sql을 찾을 수 없음";
    return false;
  }
  const auto rename = [](std::string ddl) {
    const auto replace_all = [&ddl](const std::string& from, const std::string& to) {
      for (std::size_t pos = ddl.find(from); pos != std::string::npos; pos = ddl.find(from, pos + to.size())) {
        ddl.replace(pos, from.size(), to);
      }
    };
    replace_all("farm_structures", kFarmStructuresTable);
    replace_all("farm_players", kFarmPlayersTable);
    replace_all("farms", kFarmsTable);
    replace_all("account_stats", kStatsTable);
    replace_all("accounts", kAccountsTable);
    return ddl;
  };

  AdminConnection admin(options);
  if (!admin.ok()) {
    error = "DB에 연결할 수 없음: " + admin.error();
    return false;
  }
  boost::mysql::results result;
  // 외래 키 때문에 참조하는 쪽(농장 인벤토리, 구조물, 농장, 누적 기록)을 먼저 지우고 accounts를 마지막에 지운다.
  for (const char* table : {kFarmPlayersTable, kFarmStructuresTable, kFarmsTable, kStatsTable, kAccountsTable}) {
    if (!admin.Execute(std::string("DROP TABLE IF EXISTS `") + table + "`", result)) {
      error = admin.error();
      return false;
    }
  }
  if (!admin.Execute(rename(*accounts_sql), result) || !admin.Execute(rename(*farms_sql), result)) {
    error = admin.error();
    return false;
  }
  return true;
}

// ============================================================ 저장소 단독 시험

class MySqlAccountRepositoryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string reason;
    const auto options = OptionsFromEnvironment(reason);
    if (!options) GTEST_SKIP() << reason;
    options_ = *options;
    std::string error;
    ASSERT_TRUE(ResetTestTables(options_, error)) << error;
    repo_ = MySqlAccountRepository::Create(io_, options_, TestTables());
  }

  // 저장소 작업을 시작하고 결과가 올 때까지(최대 15초) io_context를 돌린다.
  template <typename T, typename StartFn>
  T Await(StartFn start) {
    std::optional<T> out;
    start([&out](T r) { out = std::move(r); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!out && std::chrono::steady_clock::now() < deadline) {
      if (io_.stopped()) io_.restart();
      io_.run_one_for(std::chrono::milliseconds(100));
    }
    if (!out) {
      ADD_FAILURE() << "저장소 응답이 15초 안에 오지 않음";
      return {};
    }
    return std::move(*out);
  }

  RepoResult<AccountRecord> Create(MySqlAccountRepository& repo, const std::string& name, const std::string& hash) {
    return Await<RepoResult<AccountRecord>>([&](auto handler) { repo.CreateAccount(name, hash, handler); });
  }
  RepoResult<AccountRecord> Create(const std::string& name, const std::string& hash) { return Create(*repo_, name, hash); }

  RepoResult<std::optional<AccountRecord>> Find(MySqlAccountRepository& repo, const std::string& name) {
    return Await<RepoResult<std::optional<AccountRecord>>>([&](auto handler) { repo.FindAccount(name, handler); });
  }
  RepoResult<std::optional<AccountRecord>> Find(const std::string& name) { return Find(*repo_, name); }

  RepoResult<account::AccountStats> Stats(std::uint64_t id) {
    return Await<RepoResult<account::AccountStats>>([&](auto handler) { repo_->LoadStats(id, handler); });
  }

  boost::asio::io_context io_;
  MySqlOptions options_;
  std::shared_ptr<MySqlAccountRepository> repo_;
};

TEST_F(MySqlAccountRepositoryTest, CreatedAccountsComeBackExactly) {
  const auto created = Create("alice_01", "$argon2id$v=19$example-hash");
  ASSERT_TRUE(created.ok) << created.message;
  EXPECT_GT(created.value.id, 0u);
  EXPECT_EQ(created.value.login_name, "alice_01");

  const auto found = Find("alice_01");
  ASSERT_TRUE(found.ok) << found.message;
  ASSERT_TRUE(found.value.has_value());
  EXPECT_EQ(found.value->id, created.value.id);
  EXPECT_EQ(found.value->login_name, "alice_01");
  EXPECT_EQ(found.value->password_hash, "$argon2id$v=19$example-hash");

  const auto second = Create("bobby_02", "hash-2");
  ASSERT_TRUE(second.ok);
  EXPECT_EQ(second.value.id, created.value.id + 1) << "번호는 만든 순서대로 증가한다";
}

TEST_F(MySqlAccountRepositoryTest, AnUnknownAccountIsNotAnError) {
  const auto found = Find("nobody_99");
  EXPECT_TRUE(found.ok) << found.message;
  EXPECT_FALSE(found.value.has_value());
}

TEST_F(MySqlAccountRepositoryTest, DuplicateNamesAreReportedAndTheConnectionStaysUsable) {
  ASSERT_TRUE(Create("alice_01", "hash-1").ok);
  const auto duplicate = Create("alice_01", "hash-2");
  EXPECT_FALSE(duplicate.ok);
  EXPECT_EQ(duplicate.error, RepoError::kDuplicate) << duplicate.message;

  // 대소문자만 다른 ID도 DB의 UNIQUE와 정렬 규칙(_ai_ci)이 같은 ID로 본다.
  const auto cased = Create("ALICE_01", "hash-3");
  EXPECT_FALSE(cased.ok);
  EXPECT_EQ(cased.error, RepoError::kDuplicate) << cased.message;

  // 중복은 서버 오류가 아니므로 연결을 버리지 않고 그대로 쓴다.
  const auto found = Find("alice_01");
  ASSERT_TRUE(found.ok) << found.message;
  ASSERT_TRUE(found.value.has_value());
  EXPECT_EQ(found.value->password_hash, "hash-1") << "중복 시도가 기존 계정을 바꾸면 안 된다";
}

TEST_F(MySqlAccountRepositoryTest, StatsDefaultToZeroAndReadStoredValues) {
  const auto created = Create("alice_01", "hash-1");
  ASSERT_TRUE(created.ok);
  auto stats = Stats(created.value.id);
  ASSERT_TRUE(stats.ok) << stats.message;
  EXPECT_EQ(stats.value.wood_gathered, 0u);
  EXPECT_EQ(stats.value.stone_gathered, 0u);
  EXPECT_EQ(stats.value.houses_built, 0u);

  AdminConnection admin(options_);
  ASSERT_TRUE(admin.ok()) << admin.error();
  boost::mysql::results ignored;
  ASSERT_TRUE(admin.Execute(std::string("INSERT INTO `") + kStatsTable +
                                "` (account_id, wood_gathered, stone_gathered, houses_built, updated_at_unix_ms) VALUES (" +
                                std::to_string(created.value.id) + ", 15, 8, 2, 0)",
                            ignored))
      << admin.error();
  stats = Stats(created.value.id);
  ASSERT_TRUE(stats.ok) << stats.message;
  EXPECT_EQ(stats.value.wood_gathered, 15u);
  EXPECT_EQ(stats.value.stone_gathered, 8u);
  EXPECT_EQ(stats.value.houses_built, 2u);
}

TEST_F(MySqlAccountRepositoryTest, ConcurrentCreatesOfOneNameLeaveExactlyOneAccount) {
  constexpr int kRequests = 10;
  std::vector<std::optional<RepoResult<AccountRecord>>> results(kRequests);
  for (int i = 0; i < kRequests; ++i) {
    repo_->CreateAccount("alice_01", "hash-" + std::to_string(i),
                         [&results, i](RepoResult<AccountRecord> r) { results[i] = std::move(r); });
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  const auto all_done = [&] {
    for (const auto& r : results) {
      if (!r) return false;
    }
    return true;
  };
  while (!all_done() && std::chrono::steady_clock::now() < deadline) {
    if (io_.stopped()) io_.restart();
    io_.run_one_for(std::chrono::milliseconds(100));
  }
  ASSERT_TRUE(all_done());
  int ok = 0;
  int duplicate = 0;
  for (const auto& r : results) {
    ok += r->ok;
    duplicate += r->error == RepoError::kDuplicate;
  }
  EXPECT_EQ(ok, 1);
  EXPECT_EQ(duplicate, kRequests - 1);
}

TEST_F(MySqlAccountRepositoryTest, FailsOnceWhenTheConnectionIsKilledThenRecovers) {
  ASSERT_TRUE(Create("alice_01", "hash-1").ok);  // 연결을 만들어 둔다

  AdminConnection admin(options_);
  ASSERT_TRUE(admin.ok()) << admin.error();
  ASSERT_GE(admin.KillOtherConnections(), 1) << admin.error();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  const auto failed = Find("alice_01");
  EXPECT_FALSE(failed.ok);
  EXPECT_EQ(failed.error, RepoError::kFailure);
  const auto recovered = Find("alice_01");
  ASSERT_TRUE(recovered.ok) << recovered.message;
  EXPECT_TRUE(recovered.value.has_value());
}

TEST_F(MySqlAccountRepositoryTest, DroppedConnectionIsReestablishedOnTheNextRequest) {
  ASSERT_TRUE(Create("alice_01", "hash-1").ok);
  repo_->DropConnectionForTest();
  const auto found = Find("alice_01");
  ASSERT_TRUE(found.ok) << found.message;
  EXPECT_TRUE(found.value.has_value());
}

TEST_F(MySqlAccountRepositoryTest, WrongPasswordIsRejectedWithoutLeakingIt) {
  MySqlOptions wrong = options_;
  wrong.password = "definitely-not-the-password-123";
  auto repo = MySqlAccountRepository::Create(io_, wrong, TestTables());
  const auto found = Find(*repo, "alice_01");
  EXPECT_FALSE(found.ok);
  EXPECT_EQ(found.message.find(wrong.password), std::string::npos) << "오류 메시지에 비밀번호가 들어 있다";
  EXPECT_EQ(found.message.find(options_.password), std::string::npos);
}

TEST_F(MySqlAccountRepositoryTest, AMissingTableGivesAReadableError) {
  auto repo = MySqlAccountRepository::Create(io_, options_, AccountTables{"no_such_table_a", "no_such_table_b"});
  const auto found = Find(*repo, "alice_01");
  EXPECT_FALSE(found.ok);
  EXPECT_NE(found.message.find("db_migrate"), std::string::npos) << found.message;
}

TEST(MySqlAccountRepository, UnsafeTableNamesAreRejected) {
  boost::asio::io_context io;
  MySqlOptions options;
  EXPECT_THROW(MySqlAccountRepository::Create(io, options, AccountTables{"a b", "account_stats"}), std::invalid_argument);
  EXPECT_THROW(MySqlAccountRepository::Create(io, options, AccountTables{"accounts", "x`; DROP TABLE y; --"}),
               std::invalid_argument);
  EXPECT_THROW(MySqlAccountRepository::Create(io, options, AccountTables{"", "account_stats"}), std::invalid_argument);
  EXPECT_NO_THROW(MySqlAccountRepository::Create(io, options, AccountTables{"accounts_test", "account_stats_test"}));
}

TEST(MySqlAccountRepository, UnreachableDatabaseFailsQuicklyWithAReadableError) {
  boost::asio::io_context io;
  MySqlOptions options;
  options.port = 1;  // 아무도 듣지 않는 포트
  options.password = "irrelevant-password";
  auto repo = MySqlAccountRepository::Create(io, options);
  std::optional<RepoResult<std::optional<AccountRecord>>> out;
  const auto started = std::chrono::steady_clock::now();
  repo->FindAccount("alice_01", [&out](auto r) { out = std::move(r); });
  while (!out && std::chrono::steady_clock::now() - started < std::chrono::seconds(10)) {
    if (io.stopped()) io.restart();
    io.run_one_for(std::chrono::milliseconds(100));
  }
  ASSERT_TRUE(out.has_value());
  EXPECT_FALSE(out->ok);
  EXPECT_EQ(out->error, RepoError::kFailure);
  EXPECT_EQ(out->message.find("irrelevant-password"), std::string::npos);
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(5)) << "DB가 없을 때 오래 붙들면 안 된다";
}

// ============================================================ 서버 전체 + MySQL

class MySqlAccountServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string reason;
    const auto options = OptionsFromEnvironment(reason);
    if (!options) GTEST_SKIP() << reason;
    options_ = *options;
    std::string error;
    ASSERT_TRUE(ResetTestTables(options_, error)) << error;
  }

  AccountRepositoryFactory Factory(MySqlOptions options) const {
    return [options](boost::asio::io_context& io) -> std::shared_ptr<account::AccountRepository> {
      return MySqlAccountRepository::Create(io, options, TestTables());
    };
  }

  // 시험은 진짜 Argon2id를 가장 작은 비용으로 쓴다(저장된 값이 실제 해시 형식인지도 확인한다).
  static std::shared_ptr<account::PasswordHasher> RealHasher() {
    return std::make_shared<account::SodiumPasswordHasher>(account::HashCost::kMinimum);
  }

  MySqlOptions options_;
};

// P1 완료 기준: 서버를 껐다 켜도 계정이 남고, 같은 비밀번호로 다시 로그인할 수 있다.
TEST_F(MySqlAccountServerTest, AccountsSurviveAServerRestart) {
  {
    TestServer server(DefaultServerConfig(), nullptr, RealHasher(), Factory(options_));
    auto c = Connect(server);
    ASSERT_EQ(RegisterAccount(*c, "Alice_01", "restart-password-1"), protocol::ACCOUNT_OK);
    ASSERT_EQ(LoginAccount(*c, "alice_01", "restart-password-1").result(), protocol::ACCOUNT_OK);
  }  // 서버 종료

  TestServer restarted(DefaultServerConfig(), nullptr, RealHasher(), Factory(options_));
  auto d = Connect(restarted);
  EXPECT_EQ(LoginAccount(*d, "alice_01", "wrong-password-9").result(), protocol::ACCOUNT_BAD_CREDENTIALS);
  const auto login = LoginAccount(*d, "ALICE_01", "restart-password-1");
  EXPECT_EQ(login.result(), protocol::ACCOUNT_OK);
  EXPECT_EQ(login.profile().login_name(), "alice_01");
  EXPECT_EQ(RegisterAccount(*d, "alice_01", "restart-password-1"), protocol::ACCOUNT_ALREADY_LOGGED_IN);
  auto e = Connect(restarted);
  EXPECT_EQ(RegisterAccount(*e, "alice_01", "another-pass-12"), protocol::ACCOUNT_LOGIN_NAME_TAKEN) << "재시작 뒤에도 ID 중복이 막혀야 한다";
}

TEST_F(MySqlAccountServerTest, ThePasswordIsStoredOnlyAsAnArgon2Hash) {
  TestServer server(DefaultServerConfig(), nullptr, RealHasher(), Factory(options_));
  auto c = Connect(server);
  ASSERT_EQ(RegisterAccount(*c, "alice_01", "plain-secret-12345"), protocol::ACCOUNT_OK);

  AdminConnection admin(options_);
  ASSERT_TRUE(admin.ok()) << admin.error();
  boost::mysql::results rows;
  ASSERT_TRUE(admin.Execute(std::string("SELECT password_hash FROM `") + kAccountsTable + "` WHERE login_name = 'alice_01'", rows))
      << admin.error();
  ASSERT_EQ(rows.rows().size(), 1u);
  const std::string stored(rows.rows().at(0).at(0).as_string());
  EXPECT_EQ(stored.rfind("$argon2id$", 0), 0u) << stored;
  EXPECT_EQ(stored.find("plain-secret-12345"), std::string::npos) << "평문 비밀번호가 DB에 저장되었다";
}

TEST_F(MySqlAccountServerTest, DatabaseOutageIsReportedAndTheServerRecovers) {
  TestServer server(DefaultServerConfig(), nullptr, RealHasher(), Factory(options_));
  auto c = Connect(server);
  ASSERT_EQ(RegisterAccount(*c, "alice_01", "outage-password-1"), protocol::ACCOUNT_OK);

  AdminConnection admin(options_);
  ASSERT_TRUE(admin.ok()) << admin.error();
  ASSERT_GE(admin.KillOtherConnections(), 1) << admin.error();  // 서버의 DB 연결이 끊긴다
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  auto d = Connect(server);
  EXPECT_EQ(LoginAccount(*d, "alice_01", "outage-password-1").result(), protocol::ACCOUNT_STORAGE_UNAVAILABLE);
  EXPECT_TRUE(PingWorksInGame(*d)) << "DB가 끊겨도 서버와 연결은 그대로다";
  EXPECT_EQ(LoginAccount(*d, "alice_01", "outage-password-1").result(), protocol::ACCOUNT_OK) << "다음 요청은 새로 연결해 정상 처리된다";
}

TEST_F(MySqlAccountServerTest, ConcurrentRegistrationsOfOneNameCreateExactlyOneAccount) {
  TestServer server(DefaultServerConfig(), nullptr, RealHasher(), Factory(options_));
  constexpr int kClients = 8;
  std::vector<std::unique_ptr<BlockingClient>> clients;
  for (int i = 0; i < kClients; ++i) {
    clients.push_back(Connect(server));
    protocol::RegisterRequest request;
    request.set_login_name("alice_01");
    request.set_password("concurrent-pw-1");
    ASSERT_TRUE(SendProto(*clients.back(), MessageId::kRegisterRequest, request));
  }
  int ok = 0;
  int taken = 0;
  for (auto& c : clients) {
    protocol::RegisterResponse response;
    ASSERT_TRUE(Receive(*c, MessageId::kRegisterResponse, response, kGameSkip, Millis(10000)));
    ok += response.result() == protocol::ACCOUNT_OK;
    taken += response.result() == protocol::ACCOUNT_LOGIN_NAME_TAKEN;
  }
  EXPECT_EQ(ok, 1);
  EXPECT_EQ(taken, kClients - 1);
}

// ============================================================ 농장 저장소 단독 시험

using farm::FarmError;
using farm::FarmTables;
using farm::MySqlFarmRepository;

FarmTables TestFarmTables() { return FarmTables{kFarmsTable, kFarmStructuresTable, kFarmPlayersTable, kStatsTable}; }

// 외래 키가 있어서 농장을 만들기 전에 계정 행이 있어야 한다. 번호는 1, 2, 3(새 테이블이라 1부터).
void InsertAccounts(const MySqlOptions& options, int count) {
  AdminConnection admin(options);
  ASSERT_TRUE(admin.ok()) << admin.error();
  for (int i = 1; i <= count; ++i) {
    boost::mysql::results ignored;
    ASSERT_TRUE(admin.Execute(std::string("INSERT INTO `") + kAccountsTable +
                                  "` (login_name, password_hash, created_at_unix_ms) VALUES ('player_" + std::to_string(i) +
                                  "', 'hash', 0)",
                              ignored))
        << admin.error();
  }
}

class MySqlFarmRepositoryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string reason;
    const auto options = OptionsFromEnvironment(reason);
    if (!options) GTEST_SKIP() << reason;
    options_ = *options;
    std::string error;
    ASSERT_TRUE(ResetTestTables(options_, error)) << error;
    InsertAccounts(options_, 3);
    repo_ = MySqlFarmRepository::Create(io_, options_, TestFarmTables());
  }

  template <typename T, typename StartFn>
  T Await(StartFn start) {
    std::optional<T> out;
    start([&out](T r) { out = std::move(r); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!out && std::chrono::steady_clock::now() < deadline) {
      if (io_.stopped()) io_.restart();
      io_.run_one_for(std::chrono::milliseconds(100));
    }
    if (!out) {
      ADD_FAILURE() << "저장소 응답이 15초 안에 오지 않음";
      return {};
    }
    return std::move(*out);
  }

  farm::FarmResult<farm::FarmData> Create(std::uint64_t owner, const std::string& code) {
    return Await<farm::FarmResult<farm::FarmData>>([&](auto h) { repo_->CreateFarm(owner, code, h); });
  }
  farm::FarmResult<std::optional<farm::FarmData>> Find(std::uint64_t owner) {
    return Await<farm::FarmResult<std::optional<farm::FarmData>>>([&](auto h) { repo_->FindFarmByOwner(owner, h); });
  }
  farm::FarmResult<bool> Save(farm::FarmSave save) {
    return Await<farm::FarmResult<bool>>([&](auto h) { repo_->SaveFarm(std::move(save), h); });
  }

  static farm::StoredStructure House(std::uint32_t id, float x, float y, std::uint32_t state = 1,
                                     std::uint64_t remaining = 0, std::uint64_t builder = 1) {
    farm::StoredStructure s;
    s.structure_id = id;
    s.type = 0;
    s.x = x;
    s.y = y;
    s.state = state;
    s.remaining_ticks = remaining;
    s.built_by_account_id = builder;
    return s;
  }

  boost::asio::io_context io_;
  MySqlOptions options_;
  std::shared_ptr<MySqlFarmRepository> repo_;
};

TEST_F(MySqlFarmRepositoryTest, ACreatedFarmCanBeFoundByItsOwnerOnly) {
  const auto created = Create(1, "123456");
  ASSERT_TRUE(created.ok) << created.message;
  EXPECT_GT(created.value.farm_id, 0u);
  EXPECT_EQ(created.value.code, "123456");

  const auto found = Find(1);
  ASSERT_TRUE(found.ok) << found.message;
  ASSERT_TRUE(found.value.has_value());
  EXPECT_EQ(found.value->farm_id, created.value.farm_id);
  EXPECT_EQ(found.value->code, "123456");
  EXPECT_EQ(found.value->owner_account_id, 1u);
  EXPECT_TRUE(found.value->structures.empty());
  EXPECT_TRUE(found.value->players.empty());

  const auto other = Find(2);
  EXPECT_TRUE(other.ok);
  EXPECT_FALSE(other.value.has_value());
}

TEST_F(MySqlFarmRepositoryTest, DuplicateCodeAndDuplicateOwnerAreToldApart) {
  ASSERT_TRUE(Create(1, "123456").ok);
  const auto same_code = Create(2, "123456");
  EXPECT_FALSE(same_code.ok);
  EXPECT_EQ(same_code.error, FarmError::kDuplicateCode) << same_code.message;
  const auto same_owner = Create(1, "654321");
  EXPECT_FALSE(same_owner.ok);
  EXPECT_EQ(same_owner.error, FarmError::kDuplicateOwner) << same_owner.message;

  // 중복은 서버 오류가 아니라서 연결을 그대로 쓴다.
  const auto third = Create(3, "000042");
  ASSERT_TRUE(third.ok) << third.message;
  EXPECT_EQ(third.value.code, "000042") << "앞자리 0이 있는 코드가 그대로 저장되어야 한다";
}

TEST_F(MySqlFarmRepositoryTest, LoadFarmCodesListsEveryCode) {
  ASSERT_TRUE(Create(1, "123456").ok);
  ASSERT_TRUE(Create(2, "000042").ok);
  const auto codes = Await<farm::FarmResult<std::vector<std::string>>>([&](auto h) { repo_->LoadFarmCodes(h); });
  ASSERT_TRUE(codes.ok) << codes.message;
  std::set<std::string> set(codes.value.begin(), codes.value.end());
  EXPECT_EQ(set, (std::set<std::string>{"123456", "000042"}));
}

TEST_F(MySqlFarmRepositoryTest, ASavedFarmComesBackExactly) {
  const auto created = Create(1, "123456");
  ASSERT_TRUE(created.ok);
  farm::FarmSave save;
  save.farm_id = created.value.farm_id;
  save.structures = {House(1, 2400.5f, 2800.25f, 1, 0, 1), House(2, 2600.0f, 2400.0f, 0, 77, 2)};
  save.players = {farm::StoredPlayer{1, 5, 3}, farm::StoredPlayer{2, 9, 0}};
  save.stats = {farm::StoredAccountStats{1, account::AccountStats{15, 8, 2}}};
  const auto saved = Save(save);
  ASSERT_TRUE(saved.ok) << saved.message;

  const auto found = Find(1);
  ASSERT_TRUE(found.ok) << found.message;
  ASSERT_TRUE(found.value.has_value());
  ASSERT_EQ(found.value->structures.size(), 2u);
  EXPECT_EQ(found.value->structures[0].structure_id, 1u);
  EXPECT_EQ(found.value->structures[0].x, 2400.5f);
  EXPECT_EQ(found.value->structures[0].y, 2800.25f);
  EXPECT_EQ(found.value->structures[0].state, 1u);
  EXPECT_EQ(found.value->structures[1].remaining_ticks, 77u);
  EXPECT_EQ(found.value->structures[1].built_by_account_id, 2u);
  ASSERT_EQ(found.value->players.size(), 2u);
  std::map<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>> inventory;
  for (const auto& p : found.value->players) inventory[p.account_id] = {p.wood, p.stone};
  EXPECT_EQ(inventory[1], (std::pair<std::uint32_t, std::uint32_t>{5, 3}));
  EXPECT_EQ(inventory[2], (std::pair<std::uint32_t, std::uint32_t>{9, 0}));

  // 누적 기록은 계정 저장소가 읽는 같은 테이블에 들어간다.
  auto accounts = account::MySqlAccountRepository::Create(io_, options_, TestTables());
  std::optional<account::RepoResult<account::AccountStats>> stats;
  accounts->LoadStats(1, [&](auto r) { stats = std::move(r); });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!stats && std::chrono::steady_clock::now() < deadline) {
    if (io_.stopped()) io_.restart();
    io_.run_one_for(std::chrono::milliseconds(100));
  }
  ASSERT_TRUE(stats.has_value());
  ASSERT_TRUE(stats->ok) << stats->message;
  EXPECT_EQ(stats->value.wood_gathered, 15u);
  EXPECT_EQ(stats->value.stone_gathered, 8u);
  EXPECT_EQ(stats->value.houses_built, 2u);
}

TEST_F(MySqlFarmRepositoryTest, SavingAgainOverwritesWithTheLatestAbsoluteValues) {
  const auto created = Create(1, "123456");
  ASSERT_TRUE(created.ok);
  farm::FarmSave first;
  first.farm_id = created.value.farm_id;
  first.structures = {House(1, 2400, 2800, /*state=*/0, /*remaining=*/50)};
  first.players = {farm::StoredPlayer{1, 5, 1}};
  ASSERT_TRUE(Save(first).ok);
  farm::FarmSave second = first;
  second.structures = {House(1, 2400, 2800, /*state=*/1, /*remaining=*/0)};
  second.players = {farm::StoredPlayer{1, 2, 7}};
  ASSERT_TRUE(Save(second).ok);
  ASSERT_TRUE(Save(second).ok) << "같은 값을 두 번 써도 같은 결과여야 한다(멱등)";

  const auto found = Find(1);
  ASSERT_TRUE(found.value.has_value());
  ASSERT_EQ(found.value->structures.size(), 1u);
  EXPECT_EQ(found.value->structures[0].state, 1u);
  EXPECT_EQ(found.value->structures[0].remaining_ticks, 0u);
  ASSERT_EQ(found.value->players.size(), 1u);
  EXPECT_EQ(found.value->players[0].wood, 2u);
  EXPECT_EQ(found.value->players[0].stone, 7u);
}

TEST_F(MySqlFarmRepositoryTest, ASaveThatFailsHalfwayChangesNothing) {
  const auto created = Create(1, "123456");
  ASSERT_TRUE(created.ok);
  farm::FarmSave bad;
  bad.farm_id = created.value.farm_id;
  bad.structures = {House(1, 2400, 2800)};
  bad.players = {farm::StoredPlayer{1, 5, 1}, farm::StoredPlayer{999, 1, 1}};  // 없는 계정(외래 키 위반)
  const auto failed = Save(bad);
  EXPECT_FALSE(failed.ok);
  EXPECT_EQ(failed.error, FarmError::kFailure);

  const auto found = Find(1);
  ASSERT_TRUE(found.ok) << found.message;
  ASSERT_TRUE(found.value.has_value());
  EXPECT_TRUE(found.value->structures.empty()) << "트랜잭션이 취소되어 구조물도 저장되지 않아야 한다";
  EXPECT_TRUE(found.value->players.empty());

  // 연결이 정리되어 다음 저장은 정상으로 처리된다.
  farm::FarmSave good = bad;
  good.players = {farm::StoredPlayer{1, 5, 1}};
  EXPECT_TRUE(Save(good).ok);
}

TEST_F(MySqlFarmRepositoryTest, AKilledConnectionFailsOneSaveAndTheSameSaveWorksAgain) {
  const auto created = Create(1, "123456");
  ASSERT_TRUE(created.ok);
  farm::FarmSave save;
  save.farm_id = created.value.farm_id;
  save.players = {farm::StoredPlayer{1, 4, 2}};
  ASSERT_TRUE(Save(save).ok);

  AdminConnection admin(options_);
  ASSERT_TRUE(admin.ok()) << admin.error();
  ASSERT_GE(admin.KillOtherConnections(), 1) << admin.error();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  save.players = {farm::StoredPlayer{1, 6, 2}};
  const auto failed = Save(save);
  EXPECT_FALSE(failed.ok);
  EXPECT_EQ(failed.error, FarmError::kFailure);
  EXPECT_TRUE(Save(save).ok) << "실패한 저장을 그대로 다시 보내면 성공해야 한다";
  EXPECT_EQ(Find(1).value->players[0].wood, 6u);
}

TEST_F(MySqlFarmRepositoryTest, ConcurrentSavesAreAppliedInTheOrderTheyWereSent) {
  const auto created = Create(1, "123456");
  ASSERT_TRUE(created.ok);
  constexpr int kSaves = 20;
  std::vector<std::optional<farm::FarmResult<bool>>> results(kSaves);
  for (int i = 0; i < kSaves; ++i) {
    farm::FarmSave save;
    save.farm_id = created.value.farm_id;
    save.players = {farm::StoredPlayer{1, static_cast<std::uint32_t>(i + 1), 0}};
    repo_->SaveFarm(save, [&results, i](farm::FarmResult<bool> r) { results[i] = std::move(r); });
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  const auto done = [&] {
    for (const auto& r : results) {
      if (!r) return false;
    }
    return true;
  };
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    if (io_.stopped()) io_.restart();
    io_.run_one_for(std::chrono::milliseconds(100));
  }
  ASSERT_TRUE(done());
  for (const auto& r : results) EXPECT_TRUE(r->ok) << r->message;
  EXPECT_EQ(Find(1).value->players[0].wood, static_cast<std::uint32_t>(kSaves)) << "마지막에 보낸 값이 최종 값이어야 한다";
}

TEST_F(MySqlFarmRepositoryTest, AMissingTableGivesAReadableError) {
  auto repo = MySqlFarmRepository::Create(io_, options_, FarmTables{"no_farms_x", "no_structs_x", "no_players_x", kStatsTable});
  const auto found = Await<farm::FarmResult<std::optional<farm::FarmData>>>([&](auto h) { repo->FindFarmByOwner(1, h); });
  EXPECT_FALSE(found.ok);
  EXPECT_NE(found.message.find("db_migrate"), std::string::npos) << found.message;
}

TEST(MySqlFarmRepository, UnsafeTableNamesAreRejected) {
  boost::asio::io_context io;
  MySqlOptions options;
  EXPECT_THROW(MySqlFarmRepository::Create(io, options, FarmTables{"a b", "s", "p", "a"}), std::invalid_argument);
  EXPECT_THROW(MySqlFarmRepository::Create(io, options, FarmTables{"farms", "x`; DROP TABLE y; --", "p", "a"}), std::invalid_argument);
  EXPECT_NO_THROW(MySqlFarmRepository::Create(io, options, FarmTables{"farms_test", "farm_structures_test", "farm_players_test", "account_stats_test"}));
}

// ============================================================ 서버 전체 + MySQL 농장

server::ServerConfig FarmServerConfig(std::uint64_t flush_ticks = 100000) {
  server::ServerConfig config = DefaultServerConfig();
  config.play.gather_range_cm = 100000.0f;
  config.play.gather_cooldown_ticks = 0;
  config.play.tree_hp = 1000;
  config.play.rock_hp = 1000;
  config.play.bench_day_ticks = 10;  // 0.5초마다 밤 정산(완공)
  config.play.farm_flush_interval_ticks = flush_ticks;
  return config;
}

class MySqlFarmServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string reason;
    const auto options = OptionsFromEnvironment(reason);
    if (!options) GTEST_SKIP() << reason;
    options_ = *options;
    std::string error;
    ASSERT_TRUE(ResetTestTables(options_, error)) << error;
  }

  AccountRepositoryFactory Accounts() const {
    return [options = options_](boost::asio::io_context& io) -> std::shared_ptr<account::AccountRepository> {
      return MySqlAccountRepository::Create(io, options, TestTables());
    };
  }
  FarmRepositoryFactory Farms() const {
    return [options = options_](boost::asio::io_context& io) -> std::shared_ptr<farm::FarmRepository> {
      return MySqlFarmRepository::Create(io, options, TestFarmTables());
    };
  }
  static std::shared_ptr<account::PasswordHasher> RealHasher() {
    return std::make_shared<account::SodiumPasswordHasher>(account::HashCost::kMinimum);
  }

  MySqlOptions options_;
};

// P2 완료 기준: 서버를 완전히 껐다 켜도 농장(집)과 인벤토리, 누적 기록이 그대로 복원된다.
TEST_F(MySqlFarmServerTest, TheFarmSurvivesAServerRestart) {
  std::string code;
  {
    TestServer server(FarmServerConfig(), nullptr, RealHasher(), Accounts(), Farms());
    auto alice = Connect(server);
    ASSERT_EQ(RegisterAccount(*alice, "alice_01", "restart-password-1"), protocol::ACCOUNT_OK);
    ASSERT_EQ(LoginAccount(*alice, "alice_01", "restart-password-1").result(), protocol::ACCOUNT_OK);
    const auto created = CreateRoom(*alice, "x");
    ASSERT_EQ(created.result(), protocol::GAME_OK);
    code = created.state().room_code();
    for (int i = 0; i < 26; ++i) ASSERT_EQ(Gather(*alice, 1).result(), protocol::GAME_OK);   // 목재 26
    for (int i = 0; i < 11; ++i) ASSERT_EQ(Gather(*alice, 31).result(), protocol::GAME_OK);  // 석재 11
    ASSERT_EQ(Build(*alice, 2400.0f, 2000.0f).result(), protocol::GAME_OK);                  // 집(목재 20, 석재 10)
    protocol::StructureUpdate update;
    ASSERT_TRUE(WaitStructureUpdate(*alice, update, [](const protocol::StructureUpdate& u) {
      return u.structures_size() == 1 && u.structures(0).state() == protocol::STRUCTURE_COMPLETE;
    }));
  }  // 서버를 끈다(연결이 끊기고 종료하면서 저장한다)

  TestServer restarted(FarmServerConfig(), nullptr, RealHasher(), Accounts(), Farms());
  auto again = Connect(restarted);
  const auto login = LoginAccount(*again, "alice_01", "restart-password-1");
  ASSERT_EQ(login.result(), protocol::ACCOUNT_OK);
  EXPECT_EQ(login.profile().wood_gathered(), 26u) << "누적 기록이 복원되어야 한다";
  EXPECT_EQ(login.profile().stone_gathered(), 11u);
  EXPECT_EQ(login.profile().houses_built(), 1u);

  const auto reopened = CreateRoom(*again, "x");
  ASSERT_EQ(reopened.result(), protocol::GAME_OK);
  EXPECT_EQ(reopened.state().room_code(), code) << "농장 코드가 같아야 한다";
  ASSERT_EQ(reopened.state().structures_size(), 1);
  EXPECT_EQ(reopened.state().structures(0).x(), 2400.0f);
  EXPECT_EQ(reopened.state().structures(0).state(), protocol::STRUCTURE_COMPLETE);
  const auto hit = Gather(*again, 1);
  EXPECT_EQ(hit.wood(), 7u) << "인벤토리(목재 26 - 20 = 6)가 복원되어 1이 더해져야 한다";
  EXPECT_EQ(hit.stone(), 1u) << "인벤토리(석재 11 - 10 = 1)";
}

TEST_F(MySqlFarmServerTest, ASaveThatFailsDuringADatabaseOutageIsRetriedAndLands) {
  TestServer server(FarmServerConfig(/*flush_ticks=*/5), nullptr, RealHasher(), Accounts(), Farms());
  auto alice = Connect(server);
  ASSERT_EQ(RegisterAccount(*alice, "alice_01", "outage-password-1"), protocol::ACCOUNT_OK);
  ASSERT_EQ(LoginAccount(*alice, "alice_01", "outage-password-1").result(), protocol::ACCOUNT_OK);
  ASSERT_EQ(CreateRoom(*alice, "x").result(), protocol::GAME_OK);

  AdminConnection admin(options_);
  ASSERT_TRUE(admin.ok()) << admin.error();
  ASSERT_GE(admin.KillOtherConnections(), 1) << admin.error();  // 서버의 DB 연결이 모두 끊긴다
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  for (int i = 0; i < 4; ++i) ASSERT_EQ(Gather(*alice, 1).result(), protocol::GAME_OK);  // 저장이 한 번 실패해도 게임은 계속된다
  EXPECT_TRUE(PingWorksInGame(*alice));

  // 다음 주기(0.25초)의 재시도가 새 연결로 성공해 DB에 들어간다.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  bool saved = false;
  while (!saved && std::chrono::steady_clock::now() < deadline) {
    boost::mysql::results rows;
    if (admin.Execute(std::string("SELECT wood FROM `") + kFarmPlayersTable + "` WHERE account_id = 1", rows) &&
        rows.rows().size() == 1 && FieldToUint64(rows.rows().at(0).at(0)) == 4) {
      saved = true;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  EXPECT_TRUE(saved) << "장애 뒤의 재시도로 채집량이 저장되어야 한다";
}

}  // namespace
}  // namespace game
