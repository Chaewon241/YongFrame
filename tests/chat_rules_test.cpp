// 채팅 입력 규칙(닉네임, 메시지 본문) 단위 테스트.
// 규칙의 숫자(16문자, 64바이트, 1,000바이트)는 상수를 가져다 쓰지 않고 직접 적어, 상수가 실수로
// 바뀌면 테스트가 실패하게 한다.

#include <gtest/gtest.h>

#include <string>

#include "chat/chat_rules.h"

namespace game::chat {
namespace {

using protocol::ResultCode;

// "\xFF" 뒤에 영문자가 오면 C++이 그 글자를 16진수로 읽어 버리므로, 깨진 바이트를 만들 때는 길이를
// 직접 지정해 문자열을 만든다.
std::string Bytes(const char* data, std::size_t length) { return std::string(data, length); }

std::string Repeat(const std::string& unit, std::size_t count) {
  std::string out;
  for (std::size_t i = 0; i < count; ++i) out += unit;
  return out;
}

// ============================================================ 규칙 상수

TEST(ChatRuleConstants, MatchTheContract) {
  EXPECT_EQ(kMinNicknameChars, 1u);
  EXPECT_EQ(kMaxNicknameChars, 16u);
  EXPECT_EQ(kMaxNicknameBytes, 64u);
  EXPECT_EQ(kMaxMessageBytes, 1000u);
}

// ============================================================ 닉네임

TEST(ValidateNickname, AcceptsOrdinaryNames) {
  EXPECT_EQ(ValidateNickname("a"), protocol::RESULT_OK);
  EXPECT_EQ(ValidateNickname("alice"), protocol::RESULT_OK);
  EXPECT_EQ(ValidateNickname("홍길동"), protocol::RESULT_OK);
  EXPECT_EQ(ValidateNickname("Mix 한글 123"), protocol::RESULT_OK);  // 가운데 공백은 허용
}

TEST(ValidateNickname, LengthLimitIsCountedInCharactersNotBytes) {
  EXPECT_EQ(ValidateNickname(Repeat("a", 16)), protocol::RESULT_OK);
  EXPECT_EQ(ValidateNickname(Repeat("a", 17)), protocol::RESULT_INVALID_NICKNAME);

  // 한글은 글자당 3바이트다. 16글자(48바이트)는 허용, 17글자(51바이트)는 글자 수 초과.
  EXPECT_EQ(ValidateNickname(Repeat("가", 16)), protocol::RESULT_OK);
  EXPECT_EQ(ValidateNickname(Repeat("가", 17)), protocol::RESULT_INVALID_NICKNAME);

  // 이모지는 글자당 4바이트다. 16글자는 정확히 64바이트라 허용된다.
  const std::string emoji = "\xF0\x9F\x98\x80";  // U+1F600
  EXPECT_EQ(Repeat(emoji, 16).size(), 64u);
  EXPECT_EQ(ValidateNickname(Repeat(emoji, 16)), protocol::RESULT_OK);
  EXPECT_EQ(ValidateNickname(Repeat(emoji, 17)), protocol::RESULT_INVALID_NICKNAME);
}

TEST(ValidateNickname, RejectsEmpty) { EXPECT_EQ(ValidateNickname(""), protocol::RESULT_INVALID_NICKNAME); }

TEST(ValidateNickname, RejectsLeadingOrTrailingSpace) {
  EXPECT_EQ(ValidateNickname(" alice"), protocol::RESULT_INVALID_NICKNAME);
  EXPECT_EQ(ValidateNickname("alice "), protocol::RESULT_INVALID_NICKNAME);
  EXPECT_EQ(ValidateNickname(" "), protocol::RESULT_INVALID_NICKNAME);
  EXPECT_EQ(ValidateNickname("\xE3\x80\x80" "a"), protocol::RESULT_INVALID_NICKNAME);  // 앞의 전각 공백 U+3000
  EXPECT_EQ(ValidateNickname("a\xC2\xA0"), protocol::RESULT_INVALID_NICKNAME);         // 뒤의 U+00A0
}

TEST(ValidateNickname, RejectsControlCharacters) {
  EXPECT_EQ(ValidateNickname("a\tb"), protocol::RESULT_INVALID_NICKNAME);
  EXPECT_EQ(ValidateNickname("a\nb"), protocol::RESULT_INVALID_NICKNAME);
  EXPECT_EQ(ValidateNickname(Bytes("a\x01" "b", 3)), protocol::RESULT_INVALID_NICKNAME);
  EXPECT_EQ(ValidateNickname(Bytes("a\0b", 3)), protocol::RESULT_INVALID_NICKNAME);
  EXPECT_EQ(ValidateNickname("a\x7F" "b"), protocol::RESULT_INVALID_NICKNAME);          // DEL
  EXPECT_EQ(ValidateNickname("a\xC2\x85" "b"), protocol::RESULT_INVALID_NICKNAME);       // U+0085
}

TEST(ValidateNickname, RejectsMalformedUtf8) {
  EXPECT_EQ(ValidateNickname(Bytes("\xFF", 1)), protocol::RESULT_INVALID_NICKNAME);              // 시작 바이트가 될 수 없는 값
  EXPECT_EQ(ValidateNickname(Bytes("\xC3", 1)), protocol::RESULT_INVALID_NICKNAME);              // 이어지는 바이트 누락
  EXPECT_EQ(ValidateNickname(Bytes("a\xE3\x81", 3)), protocol::RESULT_INVALID_NICKNAME);         // 3바이트 문자가 중간에 끊김
  EXPECT_EQ(ValidateNickname(Bytes("\x80", 1)), protocol::RESULT_INVALID_NICKNAME);              // 이어지는 바이트가 맨 앞
  EXPECT_EQ(ValidateNickname(Bytes("\xC0\x80", 2)), protocol::RESULT_INVALID_NICKNAME);          // overlong NUL
  EXPECT_EQ(ValidateNickname(Bytes("\xED\xA0\x80", 3)), protocol::RESULT_INVALID_NICKNAME);      // 서로게이트 U+D800
  EXPECT_EQ(ValidateNickname(Bytes("\xF4\x90\x80\x80", 4)), protocol::RESULT_INVALID_NICKNAME);  // U+110000 (범위 초과)
}

// ============================================================ 메시지 본문

TEST(ValidateMessageText, AcceptsOrdinaryText) {
  EXPECT_EQ(ValidateMessageText("hello"), protocol::RESULT_OK);
  EXPECT_EQ(ValidateMessageText("안녕하세요, 반갑습니다!"), protocol::RESULT_OK);
  EXPECT_EQ(ValidateMessageText("  앞뒤 공백은 본문에서는 허용  "), protocol::RESULT_OK);
  EXPECT_EQ(ValidateMessageText("\xF0\x9F\x98\x80"), protocol::RESULT_OK);  // 이모지
}

TEST(ValidateMessageText, LengthLimitIsInBytes) {
  EXPECT_EQ(ValidateMessageText(Repeat("a", 1000)), protocol::RESULT_OK);
  EXPECT_EQ(ValidateMessageText(Repeat("a", 1001)), protocol::RESULT_MESSAGE_TOO_LONG);

  // 한글 333글자 = 999바이트는 허용, 334글자 = 1002바이트는 초과. 글자 수가 아니라 바이트 기준이다.
  EXPECT_EQ(ValidateMessageText(Repeat("가", 333)), protocol::RESULT_OK);
  EXPECT_EQ(ValidateMessageText(Repeat("가", 334)), protocol::RESULT_MESSAGE_TOO_LONG);
}

TEST(ValidateMessageText, RejectsEmptyAndWhitespaceOnly) {
  EXPECT_EQ(ValidateMessageText(""), protocol::RESULT_EMPTY_MESSAGE);
  EXPECT_EQ(ValidateMessageText("   "), protocol::RESULT_EMPTY_MESSAGE);
  EXPECT_EQ(ValidateMessageText("\xE3\x80\x80\xE3\x80\x80"), protocol::RESULT_EMPTY_MESSAGE);  // 전각 공백만
}

TEST(ValidateMessageText, RejectsControlCharactersIncludingNewlineAndTab) {
  EXPECT_EQ(ValidateMessageText("line1\nline2"), protocol::RESULT_INVALID_ARGUMENT);
  EXPECT_EQ(ValidateMessageText("a\tb"), protocol::RESULT_INVALID_ARGUMENT);
  EXPECT_EQ(ValidateMessageText("a\rb"), protocol::RESULT_INVALID_ARGUMENT);
  EXPECT_EQ(ValidateMessageText(Bytes("a\0b", 3)), protocol::RESULT_INVALID_ARGUMENT);
  EXPECT_EQ(ValidateMessageText("\n"), protocol::RESULT_INVALID_ARGUMENT);  // 공백뿐이어도 제어 문자가 먼저 걸린다
}

TEST(ValidateMessageText, RejectsMalformedUtf8) {
  EXPECT_EQ(ValidateMessageText(Bytes("ok\xFF", 3)), protocol::RESULT_INVALID_ARGUMENT);
  EXPECT_EQ(ValidateMessageText(Bytes("\xC0\x80", 2)), protocol::RESULT_INVALID_ARGUMENT);
}

TEST(ValidateMessageText, TooLongIsCheckedBeforeContent) {
  // 길이 초과가 가장 싼 검사라 먼저 한다. 제어 문자가 섞여 있어도 결과는 TOO_LONG이다.
  EXPECT_EQ(ValidateMessageText(Repeat("a", 1000) + "\n"), protocol::RESULT_MESSAGE_TOO_LONG);
}

}  // namespace
}  // namespace game::chat
