// mysql_check: MySQL 서버에 접속해 연결과 기본 설정을 확인하는 진단 도구.
//
// 2단계(채팅 + MySQL)의 첫 확인용이다. 다음을 한 번에 점검한다.
//  1. Boost.MySQL 드라이버가 이 환경에서 빌드되고 실행되는가
//  2. MySQL 서버가 켜져 있고, 계정/비밀번호/데이터베이스 이름이 맞는가
//  3. 서버의 버전, 문자 집합, 정렬 규칙(한글 닉네임/채팅을 위해 utf8mb4여야 한다)
//
// 비밀번호는 명령행 인자로 받지 않고 환경변수 GAME_DB_PASSWORD에서 읽는다.
// (명령행은 셸 기록과 프로세스 목록에 남기 때문이다) 비밀번호는 어디에도 출력하지 않는다.
//
// 종료 코드: 0=성공, 1=연결 또는 질의 실패, 2=인자 오류

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include <boost/asio/io_context.hpp>
#include <boost/mysql.hpp>

#include "common/db_credentials.h"
#include "common/error_text.h"

namespace {

struct Options {
  std::string host = "127.0.0.1";
  std::uint16_t port = 3306;
  std::string user = "root";
  std::string database;  // 비어 있으면 특정 데이터베이스를 선택하지 않고 접속한다.
};

void PrintUsage(const char* program) {
  std::cout << "사용법: " << program << " [옵션]\n"
            << "  --host <주소>       MySQL 서버 주소 (기본 127.0.0.1)\n"
            << "  --port <번호>       포트 (기본 3306)\n"
            << "  --user <이름>       계정 (기본 root)\n"
            << "  --db <이름>         데이터베이스 이름 (기본: 선택 안 함)\n"
            << "  --help              이 도움말\n"
            << "비밀번호는 환경변수 GAME_DB_PASSWORD로 전달합니다. 예 (PowerShell):\n"
            << "  $env:GAME_DB_PASSWORD = '비밀번호'\n";
}

// 0: 실행, 1: 정상 종료(--help), 2: 인자 오류
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
    } else {
      std::cerr << "알 수 없는 옵션: " << arg << "\n";
      PrintUsage(argv[0]);
      return 2;
    }
  }
  return 0;
}

// 서버가 돌려준 진단 메시지(비어 있을 수 있음)를 사람이 읽을 문장 뒤에 붙인다.
std::string WithServerMessage(const std::string& text, const boost::mysql::diagnostics& diag) {
  if (diag.server_message().empty()) {
    return text;
  }
  return text + " / 서버 메시지: " + std::string(diag.server_message());
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);  // 한글 출력이 깨지지 않게 한다.
#endif

  Options options;
  if (const int parsed = ParseArgs(argc, argv, options); parsed != 0) {
    return parsed == 1 ? 0 : 2;
  }

  std::string password_source;
  const std::string password = game::common::ReadDbPassword(&password_source).value_or("");
  if (password_source.empty()) {
    std::cout << "[안내] GAME_DB_PASSWORD / GAME_DB_PASSWORD_FILE이 없어 빈 비밀번호로 접속합니다.\n";
  } else {
    std::cout << "[안내] 비밀번호 출처: " << password_source << "\n";
  }

  boost::asio::io_context ctx;
  boost::mysql::any_connection conn(ctx);

  boost::mysql::connect_params params;
  params.server_address.emplace_host_and_port(options.host, options.port);
  params.username = options.user;
  params.password = password;
  params.database = options.database;

  std::cout << "접속 시도: " << options.user << "@" << options.host << ":" << options.port
            << (options.database.empty() ? "" : "/" + options.database) << "\n";

  boost::mysql::error_code ec;
  boost::mysql::diagnostics diag;
  conn.connect(params, ec, diag);
  if (ec) {
    std::cerr << "[실패] 연결할 수 없습니다: "
              << WithServerMessage(game::common::ErrorText(ec), diag) << "\n";
    return 1;
  }
  std::cout << "[성공] 연결됨\n";

  // 서버 버전과, 한글을 안전하게 저장하기 위한 문자 집합/정렬 규칙을 확인한다.
  boost::mysql::results result;
  conn.execute("SELECT VERSION(), @@character_set_server, @@collation_server, @@time_zone", result, ec, diag);
  if (ec) {
    std::cerr << "[실패] 질의 오류: " << WithServerMessage(game::common::ErrorText(ec), diag) << "\n";
    return 1;
  }

  const auto row = result.rows().at(0);
  const std::string charset(row.at(1).as_string());
  std::cout << "서버 버전      : " << row.at(0).as_string() << "\n"
            << "문자 집합      : " << charset << "\n"
            << "정렬 규칙      : " << row.at(2).as_string() << "\n"
            << "서버 시간대    : " << row.at(3).as_string() << "\n";
  if (charset != "utf8mb4") {
    std::cout << "[주의] 서버 기본 문자 집합이 utf8mb4가 아닙니다. 채팅 테이블은 스키마에서 utf8mb4를 명시해야 "
                 "이모지와 일부 한글이 깨지지 않습니다.\n";
  }

  conn.close(ec, diag);
  return 0;
}
