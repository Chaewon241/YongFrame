// ping_client: 서버에 PingRequest를 보내고 PingResponse가 똑같이 돌아오는지 확인하는 실행 파일.
//
// 1단계 완료 기준(project.md)의 "클라이언트는 요청 ID와 payload를 보내고, 응답이 동일한지 검사한다"를
// 수동으로 확인하는 도구이다. 요청을 하나 보내고 응답을 받은 뒤 다음 요청을 보내는 단순한 방식이라
// 부하 생성기가 아니다. 부하 측정용 클라이언트는 3단계에서 따로 만든다.
//
// 종료 코드: 모든 요청이 성공하면 0, 하나라도 실패하면 1, 인자 오류면 2.

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
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

#include "client/blocking_client.h"

namespace {

struct Options {
  std::string host = "127.0.0.1";
  std::uint16_t port = 7777;
  std::uint64_t count = 1;           // 보낼 요청 수
  std::size_t payload_size = 16;     // 요청마다 보낼 payload 바이트 수
  std::uint32_t timeout_ms = 3000;   // 응답을 기다리는 제한 시간
};

void PrintUsage(const char* program) {
  std::cout << "사용법: " << program << " [옵션]\n"
            << "  --host <주소>          서버 주소 (기본 127.0.0.1)\n"
            << "  --port <번호>          서버 포트 (기본 7777)\n"
            << "  --count <N>            보낼 요청 수 (기본 1)\n"
            << "  --payload-size <바이트> 요청마다 보낼 payload 크기 (기본 16, 최대 65000)\n"
            << "  --timeout-ms <밀리초>  응답 대기 제한 시간 (기본 3000)\n"
            << "  --help                 이 도움말\n";
}

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
    const std::string_view value = argv[++i];
    unsigned long long number = 0;

    if (arg == "--host") {
      options.host = std::string(value);
    } else if (arg == "--port" && ParseUnsigned(value, 65535, number)) {
      options.port = static_cast<std::uint16_t>(number);
    } else if (arg == "--count" && ParseUnsigned(value, 100000000, number) && number > 0) {
      options.count = number;
    } else if (arg == "--payload-size" && ParseUnsigned(value, 65000, number)) {
      options.payload_size = static_cast<std::size_t>(number);
    } else if (arg == "--timeout-ms" && ParseUnsigned(value, 600000, number) && number > 0) {
      options.timeout_ms = static_cast<std::uint32_t>(number);
    } else {
      std::cerr << "잘못된 옵션이거나 값입니다: " << arg << " " << value << "\n";
      PrintUsage(argv[0]);
      return 2;
    }
  }
  return 0;
}

// 요청 번호마다 내용이 다른 payload를 만든다. 응답이 엉뚱한 요청의 것이면 내용이 달라 바로 드러난다.
std::string MakePayload(std::uint64_t request_id, std::size_t size) {
  std::string payload(size, '\0');
  for (std::size_t i = 0; i < size; ++i) {
    payload[i] = static_cast<char>('a' + (request_id + i) % 26);
  }
  return payload;
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

  using game::client::BlockingClient;
  BlockingClient client;
  if (!client.Connect(options.host, options.port)) {
    std::cerr << "서버에 연결하지 못했습니다 (" << options.host << ":" << options.port
              << "): " << client.last_error() << "\n";
    return 1;
  }
  std::cout << "연결됨: " << options.host << ":" << options.port << "  요청 " << options.count
            << "건, payload " << options.payload_size << "바이트\n";

  std::uint64_t ok = 0;
  std::uint64_t failed = 0;
  double total_ms = 0.0;
  double min_ms = 1e18;
  double max_ms = 0.0;
  const BlockingClient::Millis timeout(options.timeout_ms);

  for (std::uint64_t request_id = 1; request_id <= options.count; ++request_id) {
    const std::string payload = MakePayload(request_id, options.payload_size);

    const auto started = std::chrono::steady_clock::now();
    if (!client.SendPing(request_id, payload, timeout)) {
      std::cerr << "[실패] request_id=" << request_id << " 전송 실패: " << client.last_error() << "\n";
      ++failed;
      break;  // 연결이 깨졌을 가능성이 높아 더 보내지 않는다.
    }

    game::protocol::PingResponse response;
    const auto status = client.ReceivePingResponse(response, timeout);
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

    if (status != BlockingClient::ReceiveStatus::kFrame) {
      std::cerr << "[실패] request_id=" << request_id << " 응답을 받지 못함 (";
      switch (status) {
        case BlockingClient::ReceiveStatus::kTimeout: std::cerr << "시간 초과"; break;
        case BlockingClient::ReceiveStatus::kClosed: std::cerr << "서버가 연결을 닫음"; break;
        default: std::cerr << client.last_error(); break;
      }
      std::cerr << ")\n";
      ++failed;
      break;
    }

    // 요청 ID와 payload가 둘 다 같아야 성공이다. (docs/protocol.md)
    if (response.request_id() != request_id || response.payload() != payload) {
      std::cerr << "[불일치] 보낸 request_id=" << request_id << ", 받은 request_id=" << response.request_id()
                << ", payload 일치=" << (response.payload() == payload ? "예" : "아니오") << "\n";
      ++failed;
      continue;
    }

    ++ok;
    total_ms += elapsed_ms;
    min_ms = std::min(min_ms, elapsed_ms);
    max_ms = std::max(max_ms, elapsed_ms);

    // 요청이 적을 때는 하나씩 보여 주고, 많을 때는 로그가 넘치지 않게 생략한다.
    if (options.count <= 10) {
      std::cout << "[성공] request_id=" << request_id << " payload=" << payload.size() << "B 왕복 "
                << elapsed_ms << " ms\n";
    }
  }

  std::cout << "\n결과: 성공 " << ok << " / 실패 " << failed << " / 요청 " << options.count << "\n";
  if (ok > 0) {
    std::cout << "왕복 시간(ms): 최소 " << min_ms << "  평균 " << total_ms / static_cast<double>(ok)
              << "  최대 " << max_ms << "\n";
  }
  return failed == 0 && ok == options.count ? 0 : 1;
}
