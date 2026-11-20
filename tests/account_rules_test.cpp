// 계정 입력 규칙 단위 시험: ID와 비밀번호 형식, 소문자 정규화.

#include <gtest/gtest.h>

#include <string>

#include "account/account_rules.h"
#include "chat/chat_rules.h"

namespace game::account {
namespace {

TEST(AccountRules, LoginNameAcceptsLettersDigitsAndUnderscoreInTheLengthRange) {
  EXPECT_TRUE(IsValidLoginName("abcd"));                  // 최소 4자
  EXPECT_TRUE(IsValidLoginName("abcdefghijklmnop"));      // 최대 16자
  EXPECT_TRUE(IsValidLoginName("Alice_01"));
  EXPECT_TRUE(IsValidLoginName("____"));
  EXPECT_TRUE(IsValidLoginName("1234"));
}

TEST(AccountRules, LoginNameLengthBoundaries) {
  EXPECT_FALSE(IsValidLoginName(""));
  EXPECT_FALSE(IsValidLoginName("abc"));                  // 3자
  EXPECT_FALSE(IsValidLoginName("abcdefghijklmnopq"));    // 17자
}

TEST(AccountRules, LoginNameRejectsAnythingOutsideTheAllowedCharacters) {
  EXPECT_FALSE(IsValidLoginName("ab cd"));      // 공백
  EXPECT_FALSE(IsValidLoginName("abcd "));      // 끝 공백
  EXPECT_FALSE(IsValidLoginName("ab-cd"));      // 하이픈
  EXPECT_FALSE(IsValidLoginName("ab.cd"));
  EXPECT_FALSE(IsValidLoginName("한글한글"));      // 한글 4글자(12바이트라 길이는 맞지만 문자 규칙 위반)
  EXPECT_FALSE(IsValidLoginName(std::string("ab\0cd", 5)));  // NUL
  EXPECT_FALSE(IsValidLoginName("ab\ncd"));
}

TEST(AccountRules, NormalizeLowercasesOnlyAscii) {
  EXPECT_EQ(NormalizeLoginName("Alice_01"), "alice_01");
  EXPECT_EQ(NormalizeLoginName("ABCD"), "abcd");
  EXPECT_EQ(NormalizeLoginName("abcd"), "abcd");
  EXPECT_EQ(NormalizeLoginName("A1_Z9"), "a1_z9");
}

TEST(AccountRules, PasswordLengthBoundariesAreInBytes) {
  EXPECT_FALSE(IsValidPassword(""));
  EXPECT_FALSE(IsValidPassword(std::string(7, 'a')));
  EXPECT_TRUE(IsValidPassword(std::string(8, 'a')));
  EXPECT_TRUE(IsValidPassword(std::string(64, 'a')));
  EXPECT_FALSE(IsValidPassword(std::string(65, 'a')));
  // 한글 3글자는 9바이트라 통과한다(문자 수가 아니라 바이트 수 기준).
  EXPECT_TRUE(IsValidPassword("비밀번"));
}

// ID는 방에서 닉네임으로 쓰이므로 항상 닉네임 규칙을 만족해야 한다.
TEST(AccountRules, EveryValidLoginNameIsAValidNickname) {
  for (const std::string name : {"abcd", "Alice_01", "abcdefghijklmnop", "____", "1234"}) {
    ASSERT_TRUE(IsValidLoginName(name));
    EXPECT_EQ(chat::ValidateNickname(NormalizeLoginName(name)), protocol::RESULT_OK) << name;
  }
}

}  // namespace
}  // namespace game::account
