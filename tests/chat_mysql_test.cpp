// MySQL 저장소 테스트: 실제 MySQL에 연결해 MySqlChatRepository와, 그것을 쓰는 서버 전체를 검증한다.
//
// 실행 조건
//  - DB 비밀번호가 환경변수 GAME_DB_PASSWORD 또는 GAME_DB_PASSWORD_FILE로 있어야 한다. 없으면 이 파일의
//    DB 테스트는 "건너뜀(SKIPPED)"으로 표시된다. (DB가 없는 환경에서도 전체 테스트가 통과하게 하기 위해)
//  - 접속 정보는 기본 127.0.0.1:3306, 계정 game, 데이터베이스 game_chat이고, GAME_DB_HOST, GAME_DB_PORT,
//    GAME_DB_USER, GAME_DB_NAME 환경변수로 바꿀 수 있다.
//
// 운영 데이터 보호
//  - 이 테스트는 chat_messages가 아니라 별도 테이블 chat_messages_test만 쓴다. 매 테스트 시작 때 그 테이블을
//    지우고 다시 만든다. 테이블은 db/migrations의 실제 SQL에서 이름만 바꿔 만들기 때문에, 마이그레이션 파일이
//    올바른지도 함께 검증된다.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <boost/mysql.hpp>

#include "chat/mysql_chat_repository.h"
#include "chat_test_client.h"
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
using chat::MySqlChatRepository;
using chat::MySqlOptions;
using chat::StorageResult;
using chat::StoredMessage;
using testing_support::BlockingClient;
using testing_support::Connect;
using testing_support::DefaultServerConfig;
using testing_support::JoinOk;
using testing_support::kTimeout;
using testing_support::MessageId;
using testing_support::PingWorks;
using testing_support::RequestHistory;
using testing_support::SendChat;
using testing_support::TestServer;

constexpr const char* kTestTable = "chat_messages_test";

[[maybe_unused]] ::testing::Environment* const kQuietLog =
    ::testing::AddGlobalTestEnvironment(new testing_support::QuietLogEnvironment);

// ============================================================ 환경 읽기

// 환경변수로 접속 정보를 만든다. 비밀번호가 없으면 nullopt와 건너뛴 이유를 돌려준다.
std::optional<MySqlOptions> OptionsFromEnvironment(std::string& skip_reason) {
  const auto password = common::ReadDbPassword();
  if (!password) {
    skip_reason = "DB 비밀번호(GAME_DB_PASSWORD 또는 GAME_DB_PASSWORD_FILE)가 없어 건너뜀";
    return std::nullopt;
  }
  MySqlOptions options;
  options.password = *password;
  options.table = kTestTable;
  if (const auto v = common::ReadEnvironment("GAME_DB_HOST")) options.host = *v;
  if (const auto v = common::ReadEnvironment("GAME_DB_PORT")) options.port = static_cast<std::uint16_t>(std::stoi(*v));
  if (const auto v = common::ReadEnvironment("GAME_DB_USER")) options.user = *v;
  if (const auto v = common::ReadEnvironment("GAME_DB_NAME")) options.database = *v;
  return options;
}

// 현재 폴더에서 위로 올라가며 db/migrations 폴더를 찾는다. ctest는 build 폴더에서 실행하고, 직접 실행하면
// 어디서든 실행할 수 있으므로 위로 올라가며 찾는다. 윈도우에서는 실행 파일 위치에서도 찾는다.
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
      const fs::path candidate = dir / "db" / "migrations" / "001_create_chat_messages.sql";
      if (fs::exists(candidate)) return candidate;
      dir = dir.parent_path();
    }
  }
  return std::nullopt;
}

// 숫자 컬럼을 부호 있음/없음/문자열 어느 타입으로 와도 읽는다. MySQL 버전에 따라 같은 컬럼(연결 번호 등)의
// 타입이 다르게 나올 수 있어서, 타입을 하나로 단정하면 bad_value_access(= bad_field_access)로 터진다.
// 읽을 수 없는 타입이면 어떤 종류였는지 메시지에 남긴다.
std::uint64_t FieldToUint64(boost::mysql::field_view field) {
  if (field.is_uint64()) return field.as_uint64();
  if (field.is_int64()) return static_cast<std::uint64_t>(field.as_int64());
  if (field.is_string()) return std::stoull(std::string(field.as_string()));
  throw std::runtime_error("숫자로 읽을 수 없는 필드 (field_kind=" + std::to_string(static_cast<int>(field.kind())) + ")");
}

// 동기식 연결로 SQL을 한 번 실행한다. 테스트 준비와 정리, 장애 만들기에 쓴다.
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

  // 실행에 성공하면 true. 결과 행이 필요하면 result를 읽는다.
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

  // 이 계정의 다른 연결을 모두 끊는다. DB가 연결을 끊어 버린 상황(재시작, 네트워크 단절)을 흉내 낸다.
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

// 테스트 전용 테이블을 실제 마이그레이션 SQL에서 이름만 바꿔 새로 만든다.
bool ResetTestTable(const MySqlOptions& options, std::string& error) {
  const auto migration = FindMigrationFile();
  if (!migration) {
    error = "db/migrations/001_create_chat_messages.sql을 찾을 수 없음";
    return false;
  }
  std::ifstream file(*migration, std::ios::binary);
  std::string ddl((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  const std::string from = "chat_messages";
  for (std::size_t pos = ddl.find(from); pos != std::string::npos; pos = ddl.find(from, pos + std::string(kTestTable).size())) {
    ddl.replace(pos, from.size(), kTestTable);
  }

  AdminConnection admin(options);
  if (!admin.ok()) {
    error = "DB에 연결할 수 없음: " + admin.error();
    return false;
  }
  boost::mysql::results result;
  if (!admin.Execute(std::string("DROP TABLE IF EXISTS `") + kTestTable + "`", result) || !admin.Execute(ddl, result)) {
    error = admin.error();
    return false;
  }
  return true;
}

// ============================================================ 저장소 단독 테스트

// 비밀번호가 없으면 건너뛰고, 있으면 테스트 테이블을 새로 만든 저장소를 준비한다.
class MySqlRepositoryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string reason;
    const auto options = OptionsFromEnvironment(reason);
    if (!options) GTEST_SKIP() << reason;
    options_ = *options;
    std::string error;
    ASSERT_TRUE(ResetTestTable(options_, error)) << error;
    repo_ = MySqlChatRepository::Create(io_, options_);
  }

  // 저장소 작업을 시작하고 결과가 올 때까지(최대 15초) io_context를 돌린다.
  template <typename T, typename StartFn>
  StorageResult<T> Await(StartFn start) {
    std::optional<StorageResult<T>> out;
    start([&out](StorageResult<T> r) { out = std::move(r); });
    RunUntil([&out] { return out.has_value(); });
    if (!out) {
      ADD_FAILURE() << "저장소 응답이 15초 안에 오지 않음";
      return {};
    }
    return std::move(*out);
  }

  template <typename Done>
  void RunUntil(Done done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
      if (io_.stopped()) io_.restart();
      io_.run_one_for(std::chrono::milliseconds(100));
    }
  }

  StorageResult<StoredMessage> Save(const std::string& nickname, const std::string& text) {
    return Await<StoredMessage>([&](auto handler) { repo_->SaveMessage({nickname, text}, handler); });
  }

  StorageResult<std::vector<StoredMessage>> Load(std::size_t limit) {
    return Await<std::vector<StoredMessage>>([&](auto handler) { repo_->LoadRecent(limit, handler); });
  }

  boost::asio::io_context io_;
  MySqlOptions options_;
  std::shared_ptr<MySqlChatRepository> repo_;
};

TEST_F(MySqlRepositoryTest, CheckConnectionSucceeds) {
  std::optional<std::pair<bool, std::string>> out;
  repo_->CheckConnection([&out](bool ok, std::string detail) { out = {ok, std::move(detail)}; });
  RunUntil([&out] { return out.has_value(); });
  ASSERT_TRUE(out.has_value());
  EXPECT_TRUE(out->first) << out->second;
}

TEST_F(MySqlRepositoryTest, SavedMessagesComeBackExactlyAndInOrder) {
  // 한글, 이모지, 따옴표, 백슬래시, SQL처럼 보이는 문장도 값 그대로 저장되고 조회되어야 한다.
  const std::vector<std::pair<std::string, std::string>> messages = {
      {"alice", "안녕하세요"},
      {"밥", "emoji \xF0\x9F\x98\x80 and 한글"},
      {"o'brien", "it's a \"quoted\" back\\slash"},
      {"mallory", "'; DROP TABLE chat_messages_test; --"},
      {"carol", std::string(1000, 'x')},
  };
  std::vector<std::uint64_t> ids;
  for (const auto& [nickname, text] : messages) {
    const auto saved = Save(nickname, text);
    ASSERT_TRUE(saved.ok) << saved.error;
    EXPECT_GT(saved.value.sent_at_unix_ms, 0);
    ids.push_back(saved.value.id);
  }
  EXPECT_TRUE(std::is_sorted(ids.begin(), ids.end()));
  EXPECT_EQ(std::set<std::uint64_t>(ids.begin(), ids.end()).size(), ids.size());  // 번호는 모두 다르다

  const auto loaded = Load(50);
  ASSERT_TRUE(loaded.ok) << loaded.error;
  ASSERT_EQ(loaded.value.size(), messages.size());  // 위의 "DROP TABLE" 문장이 실행됐다면 테이블이 없어 실패했을 것이다
  for (std::size_t i = 0; i < messages.size(); ++i) {
    EXPECT_EQ(loaded.value[i].id, ids[i]);
    EXPECT_EQ(loaded.value[i].nickname, messages[i].first);
    EXPECT_EQ(loaded.value[i].text, messages[i].second) << "메시지 " << i;
  }
}

TEST_F(MySqlRepositoryTest, LoadRecentReturnsTheLastNOldestFirst) {
  for (int i = 1; i <= 5; ++i) ASSERT_TRUE(Save("u", "m" + std::to_string(i)).ok);

  const auto last3 = Load(3);
  ASSERT_TRUE(last3.ok) << last3.error;
  ASSERT_EQ(last3.value.size(), 3u);
  EXPECT_EQ(last3.value[0].text, "m3");
  EXPECT_EQ(last3.value[1].text, "m4");
  EXPECT_EQ(last3.value[2].text, "m5");

  EXPECT_EQ(Load(100).value.size(), 5u);  // 요청한 것보다 적으면 있는 만큼만
}

TEST_F(MySqlRepositoryTest, EmptyTableLoadsAnEmptyList) {
  const auto loaded = Load(10);
  ASSERT_TRUE(loaded.ok) << loaded.error;
  EXPECT_TRUE(loaded.value.empty());
}

// 방은 저장과 기록 조회를 동시에 요청할 수 있다. 연결 하나로 안전하게 처리되어야 한다.
TEST_F(MySqlRepositoryTest, ConcurrentRequestsAreSerializedOnOneConnection) {
  constexpr int kSaves = 30;
  constexpr int kLoads = 10;
  std::vector<std::uint64_t> ids;
  int loads_done = 0;
  int failures = 0;

  for (int i = 0; i < kSaves; ++i) {
    repo_->SaveMessage({"u", "m" + std::to_string(i)}, [&](StorageResult<StoredMessage> r) {
      if (r.ok) ids.push_back(r.value.id); else ++failures;
    });
    if (i % 3 == 0) {
      repo_->LoadRecent(5, [&](StorageResult<std::vector<StoredMessage>> r) {
        if (r.ok) ++loads_done; else ++failures;
      });
    }
  }
  RunUntil([&] { return static_cast<int>(ids.size()) + loads_done + failures >= kSaves + kLoads; });

  EXPECT_EQ(failures, 0);
  EXPECT_EQ(loads_done, kLoads);
  ASSERT_EQ(ids.size(), static_cast<std::size_t>(kSaves));
  EXPECT_TRUE(std::is_sorted(ids.begin(), ids.end())) << "요청한 순서대로 처리되어야 한다";
  EXPECT_EQ(std::set<std::uint64_t>(ids.begin(), ids.end()).size(), ids.size());
}

// DB가 연결을 끊어 버린 상황: 그 순간의 요청은 실패하지만, 서버를 다시 시작하지 않아도 다음 요청부터 복구된다.
TEST_F(MySqlRepositoryTest, FailsOnceWhenTheConnectionIsKilledThenRecovers) {
  ASSERT_TRUE(Save("u", "before").ok);

  AdminConnection admin(options_);
  ASSERT_TRUE(admin.ok()) << admin.error();
  const int killed = admin.KillOtherConnections();
  ASSERT_GE(killed, 1) << "끊을 연결이 없음: " << admin.error();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  const auto failed = Save("u", "during outage");
  EXPECT_FALSE(failed.ok);
  EXPECT_FALSE(failed.error.empty());

  const auto recovered = Save("u", "after recovery");
  EXPECT_TRUE(recovered.ok) << recovered.error;

  const auto loaded = Load(10);
  ASSERT_TRUE(loaded.ok) << loaded.error;
  ASSERT_EQ(loaded.value.size(), 2u);  // 끊긴 사이의 메시지는 저장되지 않았다
  EXPECT_EQ(loaded.value[0].text, "before");
  EXPECT_EQ(loaded.value[1].text, "after recovery");
}

TEST_F(MySqlRepositoryTest, DroppedConnectionIsReestablishedOnTheNextRequest) {
  ASSERT_TRUE(Save("u", "one").ok);
  repo_->DropConnectionForTest();
  EXPECT_TRUE(Save("u", "two").ok);
}

// ============================================================ 연결 실패 / 잘못된 설정 (DB가 없어도 되는 것은 게이트 없음)

TEST(MySqlRepository, UnsafeTableNamesAreRejected) {
  boost::asio::io_context io;
  for (const std::string name : {std::string("a b"), std::string("x`y"), std::string("t;DROP TABLE z"), std::string(""),
                                 std::string(65, 'a'), std::string("한글"), std::string("a-b")}) {
    MySqlOptions options;
    options.table = name;
    EXPECT_THROW(MySqlChatRepository::Create(io, options), std::invalid_argument) << "테이블 이름 \"" << name << "\"";
  }
  MySqlOptions ok_options;
  ok_options.table = "chat_messages_2";
  EXPECT_NO_THROW(MySqlChatRepository::Create(io, ok_options));
}

TEST(MySqlRepository, UnreachableDatabaseFailsQuicklyWithAReadableError) {
  boost::asio::io_context io;
  MySqlOptions options;
  options.port = 1;  // 아무것도 듣고 있지 않은 포트
  options.password = "unused";
  auto repo = MySqlChatRepository::Create(io, options);

  std::optional<StorageResult<StoredMessage>> out;
  const auto started = std::chrono::steady_clock::now();
  repo->SaveMessage({"u", "hello"}, [&out](StorageResult<StoredMessage> r) { out = std::move(r); });
  const auto deadline = started + std::chrono::seconds(10);
  while (!out && std::chrono::steady_clock::now() < deadline) {
    if (io.stopped()) io.restart();
    io.run_one_for(std::chrono::milliseconds(100));
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

  ASSERT_TRUE(out.has_value()) << "10초 안에 실패 응답이 오지 않음";
  EXPECT_FALSE(out->ok);
  EXPECT_NE(out->error.find("DB 연결 실패"), std::string::npos) << out->error;
  EXPECT_LT(elapsed.count(), 5000) << "DB가 없을 때 오래 붙들면 안 된다";
}

TEST_F(MySqlRepositoryTest, WrongPasswordIsRejectedWithoutLeakingIt) {
  MySqlOptions wrong = options_;
  wrong.password = "definitely-not-the-password-123";
  auto repo = MySqlChatRepository::Create(io_, wrong);

  const auto saved = Await<StoredMessage>([&](auto handler) { repo->SaveMessage({"u", "x"}, handler); });
  EXPECT_FALSE(saved.ok);
  EXPECT_EQ(saved.error.find(wrong.password), std::string::npos) << "오류 메시지에 비밀번호가 들어 있다";
  EXPECT_EQ(saved.error.find(options_.password), std::string::npos);
}

// ============================================================ 서버 전체 + MySQL

class MySqlServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string reason;
    const auto options = OptionsFromEnvironment(reason);
    if (!options) GTEST_SKIP() << reason;
    options_ = *options;
    std::string error;
    ASSERT_TRUE(ResetTestTable(options_, error)) << error;
  }

  // 서버가 쓸 저장소를 만드는 함수. 서버를 껐다 켜도 같은 테이블을 쓴다.
  testing_support::RepositoryFactory Factory(MySqlOptions options) const {
    return [options](boost::asio::io_context& io) -> std::shared_ptr<chat::ChatRepository> {
      return MySqlChatRepository::Create(io, options);
    };
  }

  MySqlOptions options_;
};

// 2단계 완료 기준: 서버를 껐다 켜도 기록이 남고, 재접속한 사용자가 기록을 조회할 수 있다.
TEST_F(MySqlServerTest, HistorySurvivesAServerRestart) {
  {
    TestServer server(DefaultServerConfig(), Factory(options_));
    auto alice = Connect(server);
    ASSERT_TRUE(JoinOk(*alice, "alice"));
    ASSERT_EQ(SendChat(*alice, "서버를 끄기 전의 첫 메시지").result(), protocol::RESULT_OK);
    ASSERT_EQ(SendChat(*alice, "둘째 메시지").result(), protocol::RESULT_OK);
  }  // 서버 종료

  TestServer restarted(DefaultServerConfig(), Factory(options_));
  auto bob = Connect(restarted);
  ASSERT_TRUE(JoinOk(*bob, "bob"));

  const auto history = RequestHistory(*bob, 10);
  ASSERT_EQ(history.result(), protocol::RESULT_OK);
  ASSERT_EQ(history.entries_size(), 2);
  EXPECT_EQ(history.entries(0).nickname(), "alice");
  EXPECT_EQ(history.entries(0).text(), "서버를 끄기 전의 첫 메시지");
  EXPECT_EQ(history.entries(1).text(), "둘째 메시지");

  // 번호는 이어서 증가한다.
  const auto next = SendChat(*bob, "재시작 뒤의 메시지");
  EXPECT_EQ(next.result(), protocol::RESULT_OK);
  EXPECT_EQ(next.message_id(), history.entries(1).message_id() + 1);
}

// DB 장애 시나리오: 채팅과 기록 조회는 STORAGE_FAILURE로 응답하고 연결과 Ping은 정상이며, DB가 돌아오면 서버 재시작 없이 복구된다.
TEST_F(MySqlServerTest, DatabaseOutageIsReportedAndServerRecovers) {
  TestServer server(DefaultServerConfig(), Factory(options_));
  auto alice = Connect(server);
  auto bob = Connect(server);
  ASSERT_TRUE(JoinOk(*alice, "alice"));
  ASSERT_TRUE(JoinOk(*bob, "bob"));
  ASSERT_EQ(SendChat(*alice, "before outage").result(), protocol::RESULT_OK);

  AdminConnection admin(options_);
  ASSERT_TRUE(admin.ok()) << admin.error();
  ASSERT_GE(admin.KillOtherConnections(), 1) << admin.error();  // 서버의 DB 연결이 끊긴다
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  EXPECT_EQ(SendChat(*alice, "during outage").result(), protocol::RESULT_STORAGE_FAILURE);
  EXPECT_TRUE(PingWorks(*alice));  // 서버와 채팅 연결은 그대로다
  EXPECT_TRUE(PingWorks(*bob));

  // 다음 요청은 새로 연결해 정상 처리된다.
  EXPECT_EQ(SendChat(*alice, "after recovery").result(), protocol::RESULT_OK);
  const auto history = RequestHistory(*bob, 10);
  ASSERT_EQ(history.result(), protocol::RESULT_OK);
  ASSERT_EQ(history.entries_size(), 2);
  EXPECT_EQ(history.entries(0).text(), "before outage");
  EXPECT_EQ(history.entries(1).text(), "after recovery");
}

// 서버를 시작할 때 DB가 아직 없어도 서버는 동작하고, 채팅만 실패하며, 나중에 DB가 올라오면 복구된다.
TEST_F(MySqlServerTest, ServerStartsWithoutDatabaseAndFailsFast) {
  MySqlOptions unreachable = options_;
  unreachable.port = 1;
  TestServer server(DefaultServerConfig(), Factory(unreachable));
  auto alice = Connect(server);
  ASSERT_TRUE(JoinOk(*alice, "alice"));  // 입장은 DB가 필요 없다

  const auto started = std::chrono::steady_clock::now();
  EXPECT_EQ(SendChat(*alice, "no database").result(), protocol::RESULT_STORAGE_FAILURE);
  EXPECT_EQ(RequestHistory(*alice, 5).result(), protocol::RESULT_STORAGE_FAILURE);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
  EXPECT_LT(elapsed.count(), 4000);
  EXPECT_TRUE(PingWorks(*alice));
}

}  // namespace
}  // namespace game
