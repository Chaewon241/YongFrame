#include "common/error_text.h"

#ifdef _WIN32
// windows.h는 이 파일에서만 쓴다. min/max 매크로가 std::min/max와 충돌하지 않게 한다.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace game::common {

std::string SystemMessageToUtf8(std::string_view message) {
#ifdef _WIN32
  if (message.empty()) {
    return {};
  }
  // 1단계: ANSI 코드페이지 -> UTF-16
  const int wide_length = MultiByteToWideChar(CP_ACP, 0, message.data(), static_cast<int>(message.size()), nullptr, 0);
  if (wide_length <= 0) {
    return std::string(message);  // 변환할 수 없으면 원문을 그대로 돌려준다.
  }
  std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
  MultiByteToWideChar(CP_ACP, 0, message.data(), static_cast<int>(message.size()), wide.data(), wide_length);

  // 2단계: UTF-16 -> UTF-8
  const int utf8_length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_length, nullptr, 0, nullptr, nullptr);
  if (utf8_length <= 0) {
    return std::string(message);
  }
  std::string utf8(static_cast<std::size_t>(utf8_length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_length, utf8.data(), utf8_length, nullptr, nullptr);
  return utf8;
#else
  return std::string(message);
#endif
}

std::string ErrorText(const boost::system::error_code& ec) {
  return SystemMessageToUtf8(ec.message());
}

}  // namespace game::common
