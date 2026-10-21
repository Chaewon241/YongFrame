#pragma once

// 서버 공용 로그 출력.
//
// - 여러 스레드가 동시에 호출해도 한 줄이 섞이지 않도록 뮤텍스로 보호한다.
// - 로그 레벨보다 낮은 호출은 출력하지 않는다. (부하 실험에서는 --log-level info 이상으로 올려
//   요청 단위 로그를 끈다. docs/validation.md 1절)
// - 한 줄 형식: "시:분:초.밀리초 [레벨] 메시지"

#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

namespace game::server {

enum class LogLevel { kDebug = 0, kInfo, kWarn, kError };

// 현재 출력 기준 레벨. 기본은 kDebug(요청 단위 로그까지 출력)이다.
inline std::atomic<LogLevel>& MinLogLevel() {
  static std::atomic<LogLevel> level{LogLevel::kDebug};
  return level;
}

inline void SetMinLogLevel(LogLevel level) { MinLogLevel().store(level, std::memory_order_relaxed); }

// "debug" / "info" / "warn" / "error" 문자열을 LogLevel로 바꾼다. 알 수 없는 문자열이면 false.
inline bool ParseLogLevel(std::string_view text, LogLevel& out) {
  if (text == "debug") out = LogLevel::kDebug;
  else if (text == "info") out = LogLevel::kInfo;
  else if (text == "warn") out = LogLevel::kWarn;
  else if (text == "error") out = LogLevel::kError;
  else return false;
  return true;
}

namespace detail {

inline const char* LevelName(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug: return "DEBUG";
    case LogLevel::kInfo: return "INFO ";
    case LogLevel::kWarn: return "WARN ";
    case LogLevel::kError: return "ERROR";
  }
  return "?????";
}

// 현재 시각을 "HH:MM:SS.mmm" 형태로 만든다. localtime은 스레드 안전하지 않아
// 플랫폼별 재진입 가능 버전(localtime_s / localtime_r)을 쓴다.
inline std::string Timestamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;

  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &seconds);
#else
  localtime_r(&seconds, &local);
#endif

  std::ostringstream out;
  out << std::put_time(&local, "%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << millis;
  return out.str();
}

inline std::mutex& LogMutex() {
  static std::mutex mutex;
  return mutex;
}

}  // namespace detail

// 인자들을 순서대로 이어 붙여 한 줄로 출력한다.
//   Log(LogLevel::kInfo, "[연결] id=", id, " 원격=", remote);
template <typename... Args>
void Log(LogLevel level, const Args&... args) {
  if (level < MinLogLevel().load(std::memory_order_relaxed)) {
    return;
  }
  std::ostringstream line;
  line << detail::Timestamp() << " [" << detail::LevelName(level) << "] ";
  (line << ... << args);
  line << '\n';

  // 문자열을 먼저 완성한 뒤 잠금 안에서는 출력만 한다. (잠금 시간을 줄이기 위해)
  std::lock_guard<std::mutex> lock(detail::LogMutex());
  std::cout << line.str() << std::flush;
}

}  // namespace game::server
