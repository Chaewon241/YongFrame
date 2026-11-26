// game_server 실행 파일의 진입점.
//
// 하는 일: 실행 인자를 읽고, Server를 만들고, io_context를 돌린다.
// Ctrl+C(SIGINT) 또는 SIGTERM을 받으면 Server::Stop()을 불러 깔끔하게 종료한다.

#ifdef _WIN32
// windows.h가 min/max 매크로를 정의해 std::min/max와 충돌하지 않도록 한다.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>  // timeBeginPeriod: 윈도우 타이머 해상도 조정
#endif

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

#include <boost/asio.hpp>

#include "account/mysql_account_repository.h"
#include "chat/mysql_chat_repository.h"
#include "farm/mysql_farm_repository.h"
#include "common/db_credentials.h"
#include "common/error_text.h"
#include "server/log.h"
#include "server/server.h"

namespace {

void PrintUsage(const char* program) {
  std::cout << "사용법: " << program << " [옵션]\n"
            << "  --bind <주소>               바인드할 주소 (기본 127.0.0.1, 외부 접속은 0.0.0.0)\n"
            << "  --port <번호>               포트 (기본 7777, 0이면 자동)\n"
            << "  --log-level <debug|info|warn|error>\n"
            << "                              로그 레벨 (기본 debug: 요청 단위 로그 포함.\n"
            << "                              부하 실험에서는 info 이상으로 올려 요청 로그를 끈다)\n"
            << "  --stats-interval <초>       통계 출력 주기 (기본 10, 0이면 끔)\n"
            << "  --stats-format <line|block> 통계 모양 (기본 line: 한 줄. block: 묶음마다 줄을 바꿔 읽기 쉽게.\n"
            << "                              부하 측정은 line으로 한다)\n"
            << "  --max-send-queue-bytes <N>  연결당 송신 큐 한도 바이트 (기본 4194304)\n"
            << "  --tcp-nodelay <on|off>      TCP_NODELAY (기본 on: 작은 패킷도 바로 보냄)\n"
            << "  --io-threads <N>            소켓 입출력 전용 스레드 수 (기본 0: 게임 로직 스레드가 입출력도 함께 함.\n"
            << "                              N이면 게임 로직은 스레드 1개 그대로, 소켓 읽기/쓰기만 N개 스레드가 나눠 맡음)\n"
            << "  --bench-move-speed <cm/s>   부하 측정 전용: 이동 속도를 바꿈 (기본 360)\n"
            << "  --bench-no-collision <on|off>\n"
            << "                              부하 측정 전용: on이면 구조물/노드가 이동을 막지 않음 (기본 off)\n"
            << "  --bench-prebuilt-structures <N>\n"
            << "                              부하 측정 전용: 방마다 완공된 집을 N개(최대 20) 미리 놓음 (기본 0)\n"
            << "  --bench-day-ticks <N>       부하 측정과 시험 전용: 하루를 N틱으로 (기본 0: 12,000틱 = 10분)\n"
            << "  --hash-workers <N>          비밀번호 해시를 계산하는 일꾼 스레드 수 (기본 2, 1~16)\n"
            << "  --storage <memory|mysql>    채팅 기록, 계정, 농장 저장소 (기본 memory: 서버를 끄면 사라짐)\n"
            << "  --db-host <주소>            MySQL 주소 (기본 127.0.0.1)\n"
            << "  --db-port <번호>            MySQL 포트 (기본 3306)\n"
            << "  --db-user <이름>            MySQL 계정 (기본 game)\n"
            << "  --db-name <이름>            MySQL 데이터베이스 (기본 game_chat)\n"
            << "                              비밀번호는 환경변수 GAME_DB_PASSWORD 또는 GAME_DB_PASSWORD_FILE\n"
            << "  --help                      이 도움말\n";
}

// 문자열을 양의 정수로 바꾼다. 숫자가 아니거나 max_value를 넘으면 false.
bool ParseUnsigned(std::string_view text, unsigned long long max_value, unsigned long long& out) {
  if (text.empty()) {
    return false;
  }
  unsigned long long value = 0;
  for (const char ch : text) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    value = value * 10 + static_cast<unsigned long long>(ch - '0');
    if (value > max_value) {
      return false;
    }
  }
  out = value;
  return true;
}

enum class ParseResult { kRun, kExitOk, kExitError };

// 채팅 기록을 어디에 저장할지. mysql이면 db 옵션을 쓰고, 비밀번호는 환경변수/파일에서 읽는다.
struct StorageOptions {
  bool use_mysql = false;
  game::chat::MySqlOptions mysql;
};

// 실행 인자를 읽어 config를 채운다. 잘못된 인자가 있으면 이유를 출력하고 kExitError.
ParseResult ParseArgs(int argc, char** argv, game::server::ServerConfig& config, StorageOptions& storage) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];

    if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      return ParseResult::kExitOk;
    }

    // 나머지 옵션은 모두 "옵션 값" 쌍이다.
    if (i + 1 >= argc) {
      std::cerr << "옵션 값이 없습니다: " << arg << "\n";
      return ParseResult::kExitError;
    }
    const std::string_view value = argv[++i];
    unsigned long long number = 0;

    if (arg == "--bind") {
      config.bind_address = std::string(value);
    } else if (arg == "--port") {
      if (!ParseUnsigned(value, 65535, number)) {
        std::cerr << "--port는 0~65535 사이 숫자여야 합니다: " << value << "\n";
        return ParseResult::kExitError;
      }
      config.port = static_cast<std::uint16_t>(number);
    } else if (arg == "--log-level") {
      game::server::LogLevel level;
      if (!game::server::ParseLogLevel(value, level)) {
        std::cerr << "--log-level은 debug/info/warn/error 중 하나여야 합니다: " << value << "\n";
        return ParseResult::kExitError;
      }
      game::server::SetMinLogLevel(level);
    } else if (arg == "--stats-interval") {
      if (!ParseUnsigned(value, 86400, number)) {
        std::cerr << "--stats-interval은 0~86400 사이 숫자(초)여야 합니다: " << value << "\n";
        return ParseResult::kExitError;
      }
      config.stats_interval_seconds = static_cast<std::uint32_t>(number);
    } else if (arg == "--stats-format") {
      if (!game::server::ParseStatsFormat(value, config.stats_format)) {
        std::cerr << "--stats-format은 line 또는 block이어야 합니다: " << value << "\n";
        return ParseResult::kExitError;
      }
    } else if (arg == "--max-send-queue-bytes") {
      if (!ParseUnsigned(value, 1ull << 40, number) || number == 0) {
        std::cerr << "--max-send-queue-bytes는 1 이상의 숫자여야 합니다: " << value << "\n";
        return ParseResult::kExitError;
      }
      config.session.max_pending_send_bytes = static_cast<std::size_t>(number);
    } else if (arg == "--bench-move-speed") {
      if (!ParseUnsigned(value, 100000, number) || number == 0) {
        std::cerr << "--bench-move-speed는 1~100000 사이 숫자(cm/s)여야 합니다: " << value << "\n";
        return ParseResult::kExitError;
      }
      config.play.move_speed_cm_per_s = static_cast<float>(number);
    } else if (arg == "--bench-no-collision") {
      if (value == "on") {
        config.play.bench_no_collision = true;
      } else if (value == "off") {
        config.play.bench_no_collision = false;
      } else {
        std::cerr << "--bench-no-collision은 on 또는 off여야 합니다: " << value << "\n";
        return ParseResult::kExitError;
      }
    } else if (arg == "--bench-prebuilt-structures") {
      if (!ParseUnsigned(value, 20, number)) {
        std::cerr << "--bench-prebuilt-structures는 0~20 사이 숫자여야 합니다: " << value << "\n";
        return ParseResult::kExitError;
      }
      config.play.bench_prebuilt_structures = static_cast<std::uint32_t>(number);
    } else if (arg == "--bench-day-ticks") {
      if (!ParseUnsigned(value, 1000000000, number)) {
        std::cerr << "--bench-day-ticks는 0~1000000000 사이 숫자(틱)여야 합니다: " << value << "\n";
        return ParseResult::kExitError;
      }
      config.play.bench_day_ticks = static_cast<std::uint32_t>(number);
    } else if (arg == "--io-threads") {
      if (!ParseUnsigned(value, 64, number)) {
        std::cerr << "--io-threads는 0~64 사이 숫자여야 합니다: " << value << "\n";
        return ParseResult::kExitError;
      }
      config.io_threads = static_cast<std::uint32_t>(number);
    } else if (arg == "--hash-workers") {
      if (!ParseUnsigned(value, 16, number) || number == 0) {
        std::cerr << "--hash-workers는 1~16 사이 숫자여야 합니다: " << value << "\n";
        return ParseResult::kExitError;
      }
      config.account.hash_workers = static_cast<unsigned>(number);
    } else if (arg == "--tcp-nodelay") {
      if (value == "on") {
        config.session.tcp_nodelay = true;
      } else if (value == "off") {
        config.session.tcp_nodelay = false;
      } else {
        std::cerr << "--tcp-nodelay는 on 또는 off여야 합니다: " << value << "\n";
        return ParseResult::kExitError;
      }
    } else if (arg == "--storage") {
      if (value == "memory") {
        storage.use_mysql = false;
      } else if (value == "mysql") {
        storage.use_mysql = true;
      } else {
        std::cerr << "--storage는 memory 또는 mysql이어야 합니다: " << value << "\n";
        return ParseResult::kExitError;
      }
    } else if (arg == "--db-host") {
      storage.mysql.host = std::string(value);
    } else if (arg == "--db-port") {
      if (!ParseUnsigned(value, 65535, number) || number == 0) {
        std::cerr << "--db-port는 1~65535 사이 숫자여야 합니다: " << value << "\n";
        return ParseResult::kExitError;
      }
      storage.mysql.port = static_cast<std::uint16_t>(number);
    } else if (arg == "--db-user") {
      storage.mysql.user = std::string(value);
    } else if (arg == "--db-name") {
      storage.mysql.database = std::string(value);
    } else {
      std::cerr << "알 수 없는 옵션: " << arg << "\n";
      PrintUsage(argv[0]);
      return ParseResult::kExitError;
    }
  }
  return ParseResult::kRun;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  // 윈도우 콘솔은 기본 코드페이지가 UTF-8이 아니라서 한글 로그가 깨진다. 출력만 UTF-8로 맞춘다.
  SetConsoleOutputCP(CP_UTF8);

  // 게임 틱(기본 20Hz = 50 ms 간격)이 윈도우의 기본 타이머 해상도(약 15.6 ms) 때문에 흔들리지 않도록 해상도를
  // 1 ms로 올린다. 3단계 부하 생성기에서 같은 문제를 겪어 고친 방식이다(docs/stage3.md). 윈도우 11이 창 없는
  // 프로세스의 해상도 요청을 무시하지 않도록 프로세스를 명시적으로 제외한다. (Linux는 필요 없다)
  timeBeginPeriod(1);
  PROCESS_POWER_THROTTLING_STATE throttling{};
  throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  throttling.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  throttling.StateMask = 0;
  SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
#endif

  game::server::ServerConfig config;
  StorageOptions storage;
  switch (ParseArgs(argc, argv, config, storage)) {
    case ParseResult::kExitOk: return EXIT_SUCCESS;
    case ParseResult::kExitError: return 2;
    case ParseResult::kRun: break;
  }

  try {
    boost::asio::io_context io;

    // 저장소를 고른다. 넘기지 않으면(memory) Server가 메모리 저장소를 만들어 쓴다.
    std::shared_ptr<game::chat::MySqlChatRepository> mysql;
    std::shared_ptr<game::account::MySqlAccountRepository> mysql_accounts;
    std::shared_ptr<game::farm::MySqlFarmRepository> mysql_farms;
    if (storage.use_mysql) {
      std::string password_source;
      const auto password = game::common::ReadDbPassword(&password_source);
      if (!password) {
        std::cerr << "--storage mysql에는 DB 비밀번호가 필요합니다. 환경변수 GAME_DB_PASSWORD 또는 "
                     "GAME_DB_PASSWORD_FILE을 설정하세요.\n";
        return 2;
      }
      storage.mysql.password = *password;
      mysql = game::chat::MySqlChatRepository::Create(io, storage.mysql);
      mysql_accounts = game::account::MySqlAccountRepository::Create(io, storage.mysql);
      mysql_farms = game::farm::MySqlFarmRepository::Create(io, storage.mysql);
      game::server::Log(game::server::LogLevel::kInfo, "[서버] 저장소: MySQL ", storage.mysql.user, "@",
                        storage.mysql.host, ":", storage.mysql.port, "/", storage.mysql.database, " (비밀번호: ",
                        password_source, ")");
    } else {
      game::server::Log(game::server::LogLevel::kInfo, "[서버] 저장소: 메모리 (서버를 끄면 채팅 기록과 계정이 사라집니다)");
    }

    game::server::Server server(io, config, mysql, mysql_accounts, nullptr, mysql_farms);  // 여기서 bind에 실패하면 예외가 난다.

    // Ctrl+C / 종료 신호를 받으면 서버를 멈춘다. 핸들러는 io.run()을 돌리는 스레드에서 실행되므로
    // Server::Stop()을 안전하게 부를 수 있다.
    boost::asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&server](const boost::system::error_code& ec, int signal_number) {
      if (!ec) {
        game::server::Log(game::server::LogLevel::kInfo, "[서버] 종료 신호 수신 (", signal_number, ")");
        server.Stop();
      }
    });

    server.Start();

    // DB 연결 상태를 시작 로그로 알려 준다. 실패해도 서버는 계속 동작하고, DB가 나중에 올라오면
    // 다음 요청에서 자동으로 연결한다.
    if (mysql) {
      mysql->CheckConnection([](bool ok, std::string detail) {
        game::server::Log(ok ? game::server::LogLevel::kInfo : game::server::LogLevel::kError,
                          "[DB] ", ok ? "연결 확인: " : "연결 확인 실패 (채팅은 STORAGE_FAILURE로 응답합니다): ", detail);
      });
    }

    // 처리할 비동기 작업이 남아 있는 동안 계속 돌고, Stop() 이후 모든 연결이 정리되면 반환한다.
    // 현재는 스레드 1개로 돌린다. 스레드 수 비교는 3단계에서 한 번에 한 변수씩 실험한다.
    io.run();

    game::server::Log(game::server::LogLevel::kInfo, "[통계] 최종 ", server.StatsReport());
    game::server::Log(game::server::LogLevel::kInfo, "[서버] 종료 완료");
  } catch (const std::exception& e) {
    // 주로 시작 단계의 실패 (포트 사용 중, 잘못된 주소 등)
    game::server::Log(game::server::LogLevel::kError, "[서버] 시작 실패: ", game::common::SystemMessageToUtf8(e.what()));
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
