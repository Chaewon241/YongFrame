#include "common/db_credentials.h"

#include <fstream>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace game::common {

std::optional<std::string> ReadEnvironment(const char* name) {
#ifdef _WIN32
  // 먼저 필요한 크기를 묻고, 그 크기만큼 버퍼를 잡아 읽는다. (getenv는 deprecated 경고가 난다)
  const DWORD needed = GetEnvironmentVariableA(name, nullptr, 0);
  if (needed == 0) {
    return std::nullopt;  // 없음
  }
  std::vector<char> buffer(needed);
  const DWORD length = GetEnvironmentVariableA(name, buffer.data(), needed);
  if (length == 0 || length >= needed) {
    return std::nullopt;
  }
  return std::string(buffer.data(), length);
#else
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  return std::string(value);
#endif
}

std::optional<std::string> ReadDbPassword(std::string* source_description) {
  if (const auto direct = ReadEnvironment("GAME_DB_PASSWORD")) {
    if (source_description != nullptr) {
      *source_description = "환경변수 GAME_DB_PASSWORD";
    }
    return direct;
  }

  const auto path = ReadEnvironment("GAME_DB_PASSWORD_FILE");
  if (!path) {
    return std::nullopt;
  }
  std::ifstream file(*path, std::ios::binary);
  if (!file) {
    if (source_description != nullptr) {
      *source_description = "GAME_DB_PASSWORD_FILE이 가리키는 파일을 열 수 없음";
    }
    return std::nullopt;
  }
  std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  // PowerShell이 만든 UTF-8 파일에는 맨 앞에 BOM(EF BB BF)이 붙을 수 있다. 비밀번호의 일부가 아니므로 지운다.
  if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
      static_cast<unsigned char>(content[1]) == 0xBB && static_cast<unsigned char>(content[2]) == 0xBF) {
    content.erase(0, 3);
  }
  // 첫 줄만 쓰고, 끝의 줄바꿈은 지운다.
  const auto newline = content.find_first_of("\r\n");
  if (newline != std::string::npos) {
    content.erase(newline);
  }
  if (content.empty()) {
    return std::nullopt;
  }
  if (source_description != nullptr) {
    *source_description = "파일 (GAME_DB_PASSWORD_FILE)";
  }
  return content;
}

}  // namespace game::common
