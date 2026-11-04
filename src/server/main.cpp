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
#endif

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

#include <boost/asio.hpp>

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
            << "  --max-send-queue-bytes <N>  연결당 송신 큐 한도 바이트 (기본 4194304)\n"
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

// 실행 인자를 읽어 config를 채운다. 잘못된 인자가 있으면 이유를 출력하고 kExitError.
ParseResult ParseArgs(int argc, char** argv, game::server::ServerConfig& config) {
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
    } else if (arg == "--max-send-queue-bytes") {
      if (!ParseUnsigned(value, 1ull << 40, number) || number == 0) {
        std::cerr << "--max-send-queue-bytes는 1 이상의 숫자여야 합니다: " << value << "\n";
        return ParseResult::kExitError;
      }
      config.session.max_pending_send_bytes = static_cast<std::size_t>(number);
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
#endif

  game::server::ServerConfig config;
  switch (ParseArgs(argc, argv, config)) {
    case ParseResult::kExitOk: return EXIT_SUCCESS;
    case ParseResult::kExitError: return 2;
    case ParseResult::kRun: break;
  }

  try {
    boost::asio::io_context io;
    game::server::Server server(io, config);  // 여기서 bind에 실패하면 예외가 난다.

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

    // 처리할 비동기 작업이 남아 있는 동안 계속 돌고, Stop() 이후 모든 연결이 정리되면 반환한다.
    // 현재는 스레드 1개로 돌린다. 스레드 수 비교는 3단계에서 한 번에 한 변수씩 실험한다.
    io.run();

    game::server::Log(game::server::LogLevel::kInfo, "[통계] 최종 ", server.FullSummary());
    game::server::Log(game::server::LogLevel::kInfo, "[서버] 종료 완료");
  } catch (const std::exception& e) {
    // 주로 시작 단계의 실패 (포트 사용 중, 잘못된 주소 등)
    game::server::Log(game::server::LogLevel::kError, "[서버] 시작 실패: ", game::common::SystemMessageToUtf8(e.what()));
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
