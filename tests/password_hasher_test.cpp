// 진짜 해시(libsodium Argon2id) 시험. 비용은 가장 작게(kMinimum) 해서 빨리 돈다.
// 운영 비용(kInteractive)은 한 번만 시험해서 해시가 그 비용 매개변수를 문자열에 담는지 본다.

#include <gtest/gtest.h>

#include <set>
#include <string>

#include "account/password_hasher.h"

namespace game::account {
namespace {

TEST(SodiumPasswordHasher, HashVerifiesWithTheRightPasswordOnly) {
  SodiumPasswordHasher hasher(HashCost::kMinimum);
  const std::string hash = hasher.Hash("correct horse battery");
  ASSERT_FALSE(hash.empty());
  EXPECT_TRUE(hasher.Verify(hash, "correct horse battery"));
  EXPECT_FALSE(hasher.Verify(hash, "correct horse batterY"));
  EXPECT_FALSE(hasher.Verify(hash, ""));
  EXPECT_FALSE(hasher.Verify(hash, "correct horse battery "));
}

TEST(SodiumPasswordHasher, TheHashIsNotThePasswordAndContainsNoPlaintext) {
  SodiumPasswordHasher hasher(HashCost::kMinimum);
  const std::string password = "my-secret-password";
  const std::string hash = hasher.Hash(password);
  EXPECT_NE(hash, password);
  EXPECT_EQ(hash.find(password), std::string::npos);
  EXPECT_EQ(hash.rfind("$argon2id$", 0), 0u) << hash;  // Argon2id 형식(솔트와 비용 매개변수 포함)
}

TEST(SodiumPasswordHasher, SamePasswordGivesDifferentHashesBecauseOfTheSalt) {
  SodiumPasswordHasher hasher(HashCost::kMinimum);
  std::set<std::string> hashes;
  for (int i = 0; i < 5; ++i) {
    hashes.insert(hasher.Hash("same-password-123"));
  }
  EXPECT_EQ(hashes.size(), 5u);
}

TEST(SodiumPasswordHasher, MalformedHashesAreRejectedWithoutThrowing) {
  SodiumPasswordHasher hasher(HashCost::kMinimum);
  EXPECT_FALSE(hasher.Verify("", "password123"));
  EXPECT_FALSE(hasher.Verify("not a hash", "password123"));
  EXPECT_FALSE(hasher.Verify("$argon2id$garbage", "password123"));
  EXPECT_FALSE(hasher.Verify(std::string(500, 'x'), "password123"));  // 너무 긴 값
}

TEST(SodiumPasswordHasher, AHashMadeWithOneCostVerifiesUnderAnotherHasher) {
  // 비용 매개변수는 해시 문자열에 들어 있어서, 비용 설정이 다른 구현으로도 확인할 수 있어야 한다.
  SodiumPasswordHasher cheap(HashCost::kMinimum);
  SodiumPasswordHasher interactive(HashCost::kInteractive);
  const std::string cheap_hash = cheap.Hash("password-for-cost-test");
  EXPECT_TRUE(interactive.Verify(cheap_hash, "password-for-cost-test"));
  const std::string strong_hash = interactive.Hash("password-for-cost-test");  // 운영 비용으로 한 번
  ASSERT_FALSE(strong_hash.empty());
  EXPECT_TRUE(cheap.Verify(strong_hash, "password-for-cost-test"));
  EXPECT_FALSE(cheap.Verify(strong_hash, "another-password-x"));
}

TEST(SodiumPasswordHasher, HandlesPasswordsContainingNulAndNonAscii) {
  SodiumPasswordHasher hasher(HashCost::kMinimum);
  const std::string password = std::string("pass\0word-", 10) + "한글";
  const std::string hash = hasher.Hash(password);
  ASSERT_FALSE(hash.empty());
  EXPECT_TRUE(hasher.Verify(hash, password));
  EXPECT_FALSE(hasher.Verify(hash, "pass"));  // NUL 앞부분만으로는 통과하면 안 된다
}

}  // namespace
}  // namespace game::account
