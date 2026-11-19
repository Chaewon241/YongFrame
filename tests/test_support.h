#pragma once

// 서버를 실제로 띄우는 통합 테스트들이 공유하는 도우미.
//
//  - TestServer: 테스트 프로세스 안에서 진짜 Server를 다른 스레드로 띄운다. 포트는 OS가 자동 배정한다.
//    저장소는 기본적으로 장애/지연을 주입할 수 있는 InMemoryChatRepository를 쓰고, 다른 저장소(MySQL 등)를
//    쓰려면 만드는 함수(RepositoryFactory)를 넘긴다.
//  - WaitUntil:  서버 스레드가 처리하는 시각과 테스트 스레드가 확인하는 시각이 달라서, 통계처럼
//    "곧 반영될" 값은 고정 sleep 대신 제한 시간 동안 조건을 기다리며 확인한다.
//  - QuietLogEnvironment: 의도적으로 오류를 만드는 테스트가 많아 서버 경고 로그가 쏟아지므로,
//    테스트 출력이 묻히지 않도록 오류 레벨 이상만 출력하게 한다. 각 테스트 파일에서
//    AddGlobalTestEnvironment로 등록한다.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#ifdef _WIN32
#include <crtdbg.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>

#include "chat/in_memory_chat_repository.h"
#include "server/log.h"
#include "server/server.h"

namespace game::testing_support {

class QuietLogEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    server::SetMinLogLevel(server::LogLevel::kError);
    // 파일로 리디렉션해도 실패 메시지가 즉시 나오게 한다. 테스트가 비정상 종료(abort)해도 그 직전까지의 출력이
    // 남는다. 출력량이 적어 성능 영향은 없다.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
    // Debug CRT의 assert/abort가 대화상자로 멈추지 않고 stderr로 메시지를 내고 종료하게 한다.
    _set_abort_behavior(_WRITE_ABORT_MSG, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
    for (const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
      _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
      _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
#endif
    // 잡히지 않은 예외로 프로세스가 죽을 때(다른 스레드 포함) 원인을 출력하고 종료한다.
    std::set_terminate([] {
      if (const auto exception = std::current_exception()) {
        try {
          std::rethrow_exception(exception);
        } catch (const std::exception& e) {
          std::fprintf(stderr, "[terminate] 잡히지 않은 예외: %s\n", e.what());
        } catch (...) {
          std::fprintf(stderr, "[terminate] 잡히지 않은 알 수 없는 예외\n");
        }
      } else {
        std::fprintf(stderr, "[terminate] 예외 없이 terminate 호출됨 (joinable 스레드 소멸 등)\n");
      }
      std::abort();
    });
  }
};

inline server::ServerConfig DefaultServerConfig() {
  server::ServerConfig config;
  config.bind_address = "127.0.0.1";
  config.port = 0;  // 빈 포트를 OS가 고른다. 테스트끼리 포트가 겹치지 않는다.
  config.stats_interval_seconds = 0;
  // 같은 테스트를 입출력 스레드를 둔 서버로도 돌리기 위한 스위치. ctest가 환경변수로 켠다(CMakeLists.txt의
  // *_io4 테스트). 값이 없거나 숫자가 아니면 0(단일 스레드)이다.
#ifdef _WIN32
  char* env = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&env, &length, "GAME_TEST_IO_THREADS") == 0 && env != nullptr) {
    config.io_threads = static_cast<std::uint32_t>(std::strtoul(env, nullptr, 10));
    std::free(env);
  }
#else
  if (const char* env = std::getenv("GAME_TEST_IO_THREADS")) {
    config.io_threads = static_cast<std::uint32_t>(std::strtoul(env, nullptr, 10));
  }
#endif
  return config;
}

// io_context를 받아 저장소를 만드는 함수. 저장소는 서버의 io_context 위에서 돌아야 하므로, TestServer가
// 자신의 io_context를 넘겨 호출한다.
using RepositoryFactory = std::function<std::shared_ptr<chat::ChatRepository>(boost::asio::io_context&)>;

// 테스트 한 개가 쓰는 서버. 만들면 서버가 시작되고, 소멸하면 정상 종료한다.
class TestServer {
 public:
  explicit TestServer(server::ServerConfig config = DefaultServerConfig(), RepositoryFactory factory = nullptr)
      : repository_(factory ? factory(io_) : std::make_shared<chat::InMemoryChatRepository>(io_)),
        memory_repository_(dynamic_cast<chat::InMemoryChatRepository*>(repository_.get())),
        server_(io_, std::move(config), repository_) {
    server_.Start();
    finished_ = finished_promise_.get_future();
    thread_ = std::thread([this] {
      io_.run();
      finished_promise_.set_value();
    });
  }

  ~TestServer() { Stop(); }

  // Server::Stop은 io_context 스레드에서 불러야 하므로 post로 넘기고, 스레드가 끝나기를 기다린다.
  // 서버가 제한 시간 안에 끝나지 않으면(예: 연결을 닫지 않는 버그) 테스트가 영원히 멈추지 않도록
  // io_context를 강제로 멈추고 테스트를 실패시킨다.
  void Stop() {
    if (!thread_.joinable()) {
      return;
    }
    boost::asio::post(io_, [this] { server_.Stop(); });
    if (finished_.wait_for(kStopTimeout) != std::future_status::ready) {
      ADD_FAILURE() << "서버가 " << kStopTimeout.count() << "ms 안에 종료되지 않음 (강제 중단)";
      io_.stop();
    }
    thread_.join();
  }

  std::uint16_t port() const { return server_.port(); }
  const server::Stats& stats() const { return server_.stats(); }
  const chat::ChatStats& chat_stats() const { return server_.chat_stats(); }
  const play::GameStats& game_stats() const { return server_.game_stats(); }

  // 인메모리 저장소에 장애(SetFailing)나 지연(SetDelay)을 주입할 때 쓴다. 다른 저장소를 넘겼다면 쓸 수 없다.
  chat::InMemoryChatRepository& repository() {
    if (memory_repository_ == nullptr) {
      throw std::logic_error("이 TestServer는 인메모리 저장소를 쓰지 않는다");
    }
    return *memory_repository_;
  }

 private:
  static constexpr std::chrono::milliseconds kStopTimeout{5000};

  boost::asio::io_context io_;
  std::shared_ptr<chat::ChatRepository> repository_;
  chat::InMemoryChatRepository* memory_repository_;  // repository_가 인메모리일 때만 가리킨다
  server::Server server_;
  std::thread thread_;
  std::promise<void> finished_promise_;  // io_context 스레드가 끝났음을 알린다.
  std::future<void> finished_;
};

// 조건이 참이 될 때까지 제한 시간 동안 기다린다.
template <typename Predicate>
bool WaitUntil(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

inline std::uint64_t Load(const std::atomic<std::uint64_t>& counter) { return counter.load(); }

}  // namespace game::testing_support
