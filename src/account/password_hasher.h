#pragma once

// PasswordHasher: 비밀번호를 해시하고 확인하는 인터페이스.
//
// 해시 계산은 일부러 비싸다(Argon2id는 메모리를 쓰고 수십 ms가 걸린다). 그래서 이 함수들은 **로직 스레드나 입출력
// 스레드에서 직접 부르지 않는다.** AccountService가 별도 일꾼 스레드에서 부르고 결과만 로직 스레드로 돌려준다.
// 따라서 구현은 여러 스레드에서 동시에 불러도 안전해야 한다.
//
// 실제 구현(SodiumPasswordHasher)은 libsodium의 crypto_pwhash_str(Argon2id)을 쓴다. 해시 문자열에 솔트와 비용
// 매개변수가 들어 있어서, 나중에 비용을 올려도 이미 저장된 해시를 계속 확인할 수 있다. 시험에서는 빠른 가짜
// 구현을 끼워 시간을 줄인다.

#include <string>
#include <string_view>

namespace game::account {

class PasswordHasher {
 public:
  virtual ~PasswordHasher() = default;

  // password의 해시 문자열을 돌려준다. 솔트가 매번 달라서 같은 비밀번호도 결과가 다르다. 실패하면 빈 문자열.
  virtual std::string Hash(std::string_view password) const = 0;

  // hash가 password의 해시이면 true. 형식이 깨진 hash는 false(예외를 던지지 않는다).
  virtual bool Verify(std::string_view hash, std::string_view password) const = 0;
};

// 해시 비용. 운영은 kInteractive(메모리 64 MiB, 반복 2), 시험은 kMinimum(가장 작게)으로 빨리 돈다.
enum class HashCost { kInteractive, kMinimum };

class SodiumPasswordHasher : public PasswordHasher {
 public:
  // libsodium을 초기화한다(여러 번 만들어도 안전). 초기화에 실패하면 std::runtime_error.
  explicit SodiumPasswordHasher(HashCost cost = HashCost::kInteractive);

  std::string Hash(std::string_view password) const override;
  bool Verify(std::string_view hash, std::string_view password) const override;

 private:
  HashCost cost_;
};

}  // namespace game::account
