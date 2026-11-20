#pragma once

// InMemoryAccountRepository: 계정을 프로세스 메모리에 저장하는 AccountRepository 구현.
// 서버를 다시 시작하면 사라진다. 기본 저장소이고, 시험에서 저장소의 실패와 지연을 흉내 내는 데도 쓴다.
// 결과는 실제 DB처럼 "나중에"(post 또는 타이머) 돌려준다.

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>

#include <boost/asio.hpp>

#include "account/account_repository.h"

namespace game::account {

class InMemoryAccountRepository : public AccountRepository {
 public:
  explicit InMemoryAccountRepository(boost::asio::io_context& io) : io_(io) {}

  void CreateAccount(std::string login_name, std::string password_hash, CreateHandler handler) override;
  void FindAccount(std::string login_name, FindHandler handler) override;
  void LoadStats(std::uint64_t account_id, StatsHandler handler) override;

  // ---- 테스트용. 다른 스레드(테스트 스레드)에서 불러도 안전하다. ----
  // true이면 이후의 모든 호출이 실패한다(DB 중단 흉내).
  void SetFailing(bool failing) { failing_.store(failing); }
  // 이후의 호출 결과를 이 시간만큼 늦게 돌려준다.
  void SetDelay(std::chrono::milliseconds delay) { delay_ms_.store(delay.count()); }
  // 누적 기록을 직접 정한다(시험 준비용).
  void SetStats(std::uint64_t account_id, AccountStats stats);
  // 저장된 누적 기록(없으면 빈 값). 농장 저장이 누적 기록을 올바르게 썼는지 확인하는 용도.
  std::optional<AccountStats> StatsOf(std::uint64_t account_id) const;
  std::size_t account_count() const;
  // 저장된 해시 문자열(없으면 빈 문자열). 평문이 저장되지 않았는지 확인하는 용도.
  std::string StoredHash(const std::string& login_name) const;

 private:
  template <typename Fn>
  void RunLater(Fn fn);

  boost::asio::io_context& io_;
  std::atomic<bool> failing_{false};
  std::atomic<long long> delay_ms_{0};

  mutable std::mutex mutex_;
  std::map<std::string, AccountRecord> accounts_;  // 키: login_name
  std::map<std::uint64_t, AccountStats> stats_;
  std::uint64_t next_id_ = 1;
};

}  // namespace game::account
