#include "account/password_hasher.h"

#include <sodium.h>

#include <mutex>
#include <stdexcept>

namespace game::account {

namespace {

void EnsureSodiumInitialized() {
  static std::once_flag once;
  static bool ok = false;
  std::call_once(once, [] { ok = (sodium_init() >= 0); });
  if (!ok) {
    throw std::runtime_error("libsodium 초기화 실패");
  }
}

}  // namespace

SodiumPasswordHasher::SodiumPasswordHasher(HashCost cost) : cost_(cost) { EnsureSodiumInitialized(); }

std::string SodiumPasswordHasher::Hash(std::string_view password) const {
  const unsigned long long ops =
      cost_ == HashCost::kMinimum ? crypto_pwhash_OPSLIMIT_MIN : crypto_pwhash_OPSLIMIT_INTERACTIVE;
  const std::size_t mem =
      cost_ == HashCost::kMinimum ? crypto_pwhash_MEMLIMIT_MIN : crypto_pwhash_MEMLIMIT_INTERACTIVE;

  char out[crypto_pwhash_STRBYTES];
  if (crypto_pwhash_str(out, password.data(), password.size(), ops, mem) != 0) {
    return {};  // 메모리 부족 등
  }
  return std::string(out);
}

bool SodiumPasswordHasher::Verify(std::string_view hash, std::string_view password) const {
  // crypto_pwhash_str_verify는 NUL로 끝나는 문자열을 요구하므로 복사해서 넘긴다(길이도 확인한다).
  if (hash.empty() || hash.size() >= crypto_pwhash_STRBYTES) {
    return false;
  }
  const std::string terminated(hash);
  return crypto_pwhash_str_verify(terminated.c_str(), password.data(), password.size()) == 0;
}

}  // namespace game::account
