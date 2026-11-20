#include "account/account_rules.h"

namespace game::account {

namespace {

bool IsAllowedChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

}  // namespace

bool IsValidLoginName(std::string_view login_name) {
  if (login_name.size() < kMinLoginNameChars || login_name.size() > kMaxLoginNameChars) {
    return false;
  }
  for (const char c : login_name) {
    if (!IsAllowedChar(c)) {
      return false;
    }
  }
  return true;
}

std::string NormalizeLoginName(std::string_view login_name) {
  std::string result(login_name);
  for (char& c : result) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return result;
}

bool IsValidPassword(std::string_view password) {
  return password.size() >= kMinPasswordBytes && password.size() <= kMaxPasswordBytes;
}

}  // namespace game::account
