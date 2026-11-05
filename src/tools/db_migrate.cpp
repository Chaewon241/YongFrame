// db_migrate: db/migrations 폴더의 SQL 파일을 번호 순서대로 MySQL에 적용하는 도구.
//
// 동작
//  1. schema_migrations 테이블(적용한 마이그레이션 번호를 기록)이 없으면 만든다.
//  2. 폴더의 "번호_설명.sql" 파일을 번호 순으로 훑으면서, 아직 기록되지 않은 번호만 실행하고 기록한다.
//  3. 이미 적용한 번호는 건너뛴다. 그래서 몇 번을 실행해도 안전하다.
//
// 규칙: 한 번 적용한 파일은 고치지 않는다. 스키마를 바꾸려면 다음 번호의 새 파일을 추가한다.
// (파일 내용이 바뀌었는지는 검사하지 않는다. 적용한 파일을 몰래 고치면 DB와 파일이 어긋난다)
//
// 비밀번호는 환경변수 GAME_DB_PASSWORD 또는 GAME_DB_PASSWORD_FILE로 전달한다.
// 종료 코드: 0=성공(적용할 것이 없어도 0), 1=실패, 2=인자 오류

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/mysql.hpp>

#include "common/db_credentials.h"
#include "common/error_text.h"

namespace {

namespace fs = std::filesystem;

struct Options {
  std::string host = "127.0.0.1";
  std::uint16_t port = 3306;
  std::string user = "game";
  std::string database = "game_chat";
  std::string directory = "db/migrations";
};

struct MigrationFile {
  long long version = 0;
  std::string name;  // 확장자를 뺀 이름. 기록용
  fs::path path;
};

void PrintUsage(const char* program) {
  std::cout << "사용법: " << program << " [옵션]\n"
            << "  --host <주소>   MySQL 서버 주소 (기본 127.0.0.1)\n"
            << "  --port <번호>   포트 (기본 3306)\n"
            << "  --user <이름>   계정 (기본 game)\n"
            << "  --db <이름>     데이터베이스 (기본 game_chat)\n"
            << "  --dir <폴더>    마이그레이션 폴더 (기본 db/migrations, 프로젝트 폴더에서 실행)\n"
            << "  --help          이 도움말\n"
            << "비밀번호는 환경변수 GAME_DB_PASSWORD 또는 GAME_DB_PASSWORD_FILE로 전달합니다.\n";
}

int ParseArgs(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      return 1;
    }
    if (i + 1 >= argc) {
      std::cerr << "옵션 값이 없습니다: " << arg << "\n";
      return 2;
    }
    const std::string value = argv[++i];
    if (arg == "--host") {
      options.host = value;
    } else if (arg == "--port") {
      const long port = std::strtol(value.c_str(), nullptr, 10);
      if (port < 1 || port > 65535) {
        std::cerr << "--port는 1~65535 사이 숫자여야 합니다: " << value << "\n";
        return 2;
      }
      options.port = static_cast<std::uint16_t>(port);
    } else if (arg == "--user") {
      options.user = value;
    } else if (arg == "--db") {
      options.database = value;
    } else if (arg == "--dir") {
      options.directory = value;
    } else {
      std::cerr << "알 수 없는 옵션: " << arg << "\n";
      PrintUsage(argv[0]);
      return 2;
    }
  }
  return 0;
}

// "001_create_chat_messages" 같은 이름에서 맨 앞의 숫자를 번호로 읽는다. 숫자로 시작하지 않으면 -1.
long long ParseVersion(const std::string& stem) {
  std::size_t digits = 0;
  while (digits < stem.size() && stem[digits] >= '0' && stem[digits] <= '9') ++digits;
  if (digits == 0 || digits > 9) return -1;
  return std::stoll(stem.substr(0, digits));
}

std::string ReadFile(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
      static_cast<unsigned char>(content[1]) == 0xBB && static_cast<unsigned char>(content[2]) == 0xBF) {
    content.erase(0, 3);  // UTF-8 BOM
  }
  return content;
}

// 실패 설명: 오류 코드와 서버가 알려준 메시지.
std::string Describe(const boost::mysql::error_code& ec, const boost::mysql::diagnostics& diag) {
  std::string text = game::common::ErrorText(ec);
  if (!diag.server_message().empty()) {
    text += " / 서버 메시지: " + std::string(diag.server_message());
  }
  return text;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
#endif

  Options options;
  if (const int parsed = ParseArgs(argc, argv, options); parsed != 0) {
    return parsed == 1 ? 0 : 2;
  }

  std::string password_source;
  const auto password = game::common::ReadDbPassword(&password_source);
  if (!password) {
    std::cerr << "DB 비밀번호가 없습니다. 환경변수 GAME_DB_PASSWORD 또는 GAME_DB_PASSWORD_FILE을 설정하세요.\n";
    return 2;
  }

  // 마이그레이션 파일 목록을 번호 순으로 만든다.
  std::vector<MigrationFile> files;
  std::error_code fs_error;
  if (!fs::is_directory(options.directory, fs_error)) {
    std::cerr << "마이그레이션 폴더를 찾을 수 없습니다: " << options.directory
              << " (프로젝트 폴더에서 실행하거나 --dir로 지정하세요)\n";
    return 2;
  }
  for (const auto& entry : fs::directory_iterator(options.directory)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".sql") continue;
    const std::string stem = entry.path().stem().string();
    const long long version = ParseVersion(stem);
    if (version < 0) {
      std::cerr << "[건너뜀] 번호로 시작하지 않는 파일: " << entry.path().filename().string() << "\n";
      continue;
    }
    files.push_back(MigrationFile{version, stem, entry.path()});
  }
  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.version < b.version; });
  for (std::size_t i = 1; i < files.size(); ++i) {
    if (files[i].version == files[i - 1].version) {
      std::cerr << "같은 번호의 파일이 둘 이상 있습니다: " << files[i].version << "\n";
      return 2;
    }
  }

  boost::asio::io_context ctx;
  boost::mysql::any_connection conn(ctx);
  boost::mysql::connect_params params;
  params.server_address.emplace_host_and_port(options.host, options.port);
  params.username = options.user;
  params.password = *password;
  params.database = options.database;
  params.multi_queries = true;  // 한 파일에 문장이 여러 개 있어도 실행할 수 있게 한다

  boost::mysql::error_code ec;
  boost::mysql::diagnostics diag;
  std::cout << "접속: " << options.user << "@" << options.host << ":" << options.port << "/" << options.database
            << " (비밀번호: " << password_source << ")\n";
  conn.connect(params, ec, diag);
  if (ec) {
    std::cerr << "[실패] 연결할 수 없습니다: " << Describe(ec, diag) << "\n";
    return 1;
  }

  boost::mysql::results result;
  conn.execute(
      "CREATE TABLE IF NOT EXISTS schema_migrations ("
      " version INT NOT NULL PRIMARY KEY,"
      " name VARCHAR(255) NOT NULL,"
      " applied_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP"
      ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
      result, ec, diag);
  if (ec) {
    std::cerr << "[실패] schema_migrations 테이블을 만들 수 없습니다: " << Describe(ec, diag) << "\n";
    return 1;
  }

  conn.execute("SELECT version FROM schema_migrations", result, ec, diag);
  if (ec) {
    std::cerr << "[실패] 적용 기록을 읽을 수 없습니다: " << Describe(ec, diag) << "\n";
    return 1;
  }
  std::set<long long> applied;
  for (const auto row : result.rows()) applied.insert(row.at(0).as_int64());

  // 적용 기록은 준비된 문장으로 값만 바인딩해서 저장한다.
  auto record = conn.prepare_statement("INSERT INTO schema_migrations (version, name) VALUES (?, ?)", ec, diag);
  if (ec) {
    std::cerr << "[실패] 기록 문장을 준비할 수 없습니다: " << Describe(ec, diag) << "\n";
    return 1;
  }

  int newly_applied = 0;
  for (const MigrationFile& file : files) {
    if (applied.count(file.version) != 0) {
      std::cout << "[이미 적용] " << file.name << "\n";
      continue;
    }
    const std::string sql = ReadFile(file.path);
    if (sql.empty()) {
      std::cerr << "[실패] 파일을 읽을 수 없거나 비어 있습니다: " << file.path.filename().string() << "\n";
      return 1;
    }
    conn.execute(sql, result, ec, diag);
    if (ec) {
      // DDL은 MySQL에서 자동 커밋되어 중간까지 적용됐을 수 있다. 기록은 하지 않으므로 파일을 고친 뒤
      // 다시 실행하면 처음부터 다시 시도한다. (그래서 파일은 IF NOT EXISTS로 쓴다)
      std::cerr << "[실패] " << file.name << ": " << Describe(ec, diag) << "\n";
      return 1;
    }
    conn.execute(record.bind(static_cast<std::int64_t>(file.version), file.name), result, ec, diag);
    if (ec) {
      std::cerr << "[실패] 적용 기록 저장 실패: " << Describe(ec, diag) << "\n";
      return 1;
    }
    std::cout << "[적용] " << file.name << "\n";
    ++newly_applied;
  }

  std::cout << "완료: 새로 적용 " << newly_applied << "건, 전체 파일 " << files.size() << "건\n";
  conn.close(ec, diag);
  return 0;
}
