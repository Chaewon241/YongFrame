#include "chat/chat_rules.h"

#include <vector>

namespace game::chat {

namespace {

// UTF-8 문자열을 유니코드 코드포인트 목록으로 바꾼다. 잘못된 UTF-8이면 false.
// 잘못된 경우: 이어지는 바이트 누락/초과, 더 짧게 쓸 수 있는 길이로 쓴 값(overlong),
// 서로게이트 영역(U+D800~U+DFFF), U+10FFFF 초과.
bool DecodeUtf8(std::string_view text, std::vector<char32_t>& out) {
  out.clear();
  std::size_t i = 0;
  while (i < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    char32_t code = 0;
    std::size_t extra = 0;     // 이어지는 바이트 수
    char32_t min_value = 0;    // 이 길이로 표현할 수 있는 최솟값 (overlong 검사용)

    if (lead < 0x80) {
      code = lead;
    } else if ((lead & 0xE0) == 0xC0) {
      code = lead & 0x1F;
      extra = 1;
      min_value = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
      code = lead & 0x0F;
      extra = 2;
      min_value = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
      code = lead & 0x07;
      extra = 3;
      min_value = 0x10000;
    } else {
      return false;  // 0x80~0xBF(이어지는 바이트가 맨 앞에 옴) 또는 0xF8 이상
    }

    if (extra > 0 && i + extra >= text.size()) {
      return false;  // 이어지는 바이트(i+1 ~ i+extra)가 문자열 끝을 넘어 모자람
    }
    for (std::size_t k = 1; k <= extra; ++k) {
      const unsigned char next = static_cast<unsigned char>(text[i + k]);
      if ((next & 0xC0) != 0x80) {
        return false;
      }
      code = (code << 6) | (next & 0x3F);
    }
    if (extra > 0 && code < min_value) {
      return false;  // overlong
    }
    if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
      return false;
    }
    out.push_back(code);
    i += extra + 1;
  }
  return true;
}

bool IsControl(char32_t c) { return c < 0x20 || (c >= 0x7F && c <= 0x9F); }

bool IsSpace(char32_t c) { return c == 0x20 || c == 0xA0 || c == 0x3000; }

}  // namespace

protocol::ResultCode ValidateNickname(std::string_view nickname) {
  if (nickname.size() > kMaxNicknameBytes) {
    return protocol::RESULT_INVALID_NICKNAME;
  }
  std::vector<char32_t> chars;
  if (!DecodeUtf8(nickname, chars)) {
    return protocol::RESULT_INVALID_NICKNAME;
  }
  if (chars.size() < kMinNicknameChars || chars.size() > kMaxNicknameChars) {
    return protocol::RESULT_INVALID_NICKNAME;
  }
  if (IsSpace(chars.front()) || IsSpace(chars.back())) {
    return protocol::RESULT_INVALID_NICKNAME;
  }
  for (const char32_t c : chars) {
    if (IsControl(c)) {
      return protocol::RESULT_INVALID_NICKNAME;
    }
  }
  return protocol::RESULT_OK;
}

protocol::ResultCode ValidateMessageText(std::string_view text) {
  // 가장 싼 검사(바이트 수)를 먼저 한다. 아주 긴 입력을 디코딩하느라 시간을 쓰지 않기 위해서다.
  if (text.size() > kMaxMessageBytes) {
    return protocol::RESULT_MESSAGE_TOO_LONG;
  }
  std::vector<char32_t> chars;
  if (!DecodeUtf8(text, chars)) {
    return protocol::RESULT_INVALID_ARGUMENT;
  }
  bool has_visible = false;
  for (const char32_t c : chars) {
    if (IsControl(c)) {
      return protocol::RESULT_INVALID_ARGUMENT;
    }
    if (!IsSpace(c)) {
      has_visible = true;
    }
  }
  return has_visible ? protocol::RESULT_OK : protocol::RESULT_EMPTY_MESSAGE;
}

}  // namespace game::chat
