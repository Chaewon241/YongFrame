#include "account/in_memory_account_repository.h"

#include <memory>
#include <utility>

namespace game::account {

// delay가 0이면 post로, 아니면 타이머로 fn을 io_context 스레드에서 나중에 실행한다.
template <typename Fn>
void InMemoryAccountRepository::RunLater(Fn fn) {
  const auto delay = std::chrono::milliseconds(delay_ms_.load());
  if (delay.count() <= 0) {
    boost::asio::post(io_, std::move(fn));
    return;
  }
  auto timer = std::make_shared<boost::asio::steady_timer>(io_);
  timer->expires_after(delay);
  timer->async_wait([timer, fn = std::move(fn)](const boost::system::error_code&) mutable { fn(); });
}

void InMemoryAccountRepository::CreateAccount(std::string login_name, std::string password_hash,
                                              CreateHandler handler) {
  RunLater([this, login_name = std::move(login_name), password_hash = std::move(password_hash),
            handler = std::move(handler)]() mutable {
    RepoResult<AccountRecord> result;
    if (failing_.load()) {
      result.error = RepoError::kFailure;
      result.message = "인메모리 저장소에 주입된 장애";
      handler(std::move(result));
      return;
    }
    {
      std::lock_guard lock(mutex_);
      if (accounts_.count(login_name) != 0) {
        result.error = RepoError::kDuplicate;
        result.message = "이미 있는 ID";
      } else {
        AccountRecord record;
        record.id = next_id_++;
        record.login_name = login_name;
        record.password_hash = std::move(password_hash);
        accounts_.emplace(login_name, record);
        result.ok = true;
        result.value = std::move(record);
      }
    }
    handler(std::move(result));
  });
}

void InMemoryAccountRepository::FindAccount(std::string login_name, FindHandler handler) {
  RunLater([this, login_name = std::move(login_name), handler = std::move(handler)]() mutable {
    RepoResult<std::optional<AccountRecord>> result;
    if (failing_.load()) {
      result.error = RepoError::kFailure;
      result.message = "인메모리 저장소에 주입된 장애";
      handler(std::move(result));
      return;
    }
    result.ok = true;
    {
      std::lock_guard lock(mutex_);
      const auto it = accounts_.find(login_name);
      if (it != accounts_.end()) {
        result.value = it->second;
      }
    }
    handler(std::move(result));
  });
}

void InMemoryAccountRepository::LoadStats(std::uint64_t account_id, StatsHandler handler) {
  RunLater([this, account_id, handler = std::move(handler)]() mutable {
    RepoResult<AccountStats> result;
    if (failing_.load()) {
      result.error = RepoError::kFailure;
      result.message = "인메모리 저장소에 주입된 장애";
      handler(std::move(result));
      return;
    }
    result.ok = true;
    {
      std::lock_guard lock(mutex_);
      const auto it = stats_.find(account_id);
      if (it != stats_.end()) {
        result.value = it->second;
      }
    }
    handler(std::move(result));
  });
}

void InMemoryAccountRepository::SetStats(std::uint64_t account_id, AccountStats stats) {
  std::lock_guard lock(mutex_);
  stats_[account_id] = stats;
}

std::optional<AccountStats> InMemoryAccountRepository::StatsOf(std::uint64_t account_id) const {
  std::lock_guard lock(mutex_);
  const auto it = stats_.find(account_id);
  if (it == stats_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::size_t InMemoryAccountRepository::account_count() const {
  std::lock_guard lock(mutex_);
  return accounts_.size();
}

std::string InMemoryAccountRepository::StoredHash(const std::string& login_name) const {
  std::lock_guard lock(mutex_);
  const auto it = accounts_.find(login_name);
  return it == accounts_.end() ? std::string() : it->second.password_hash;
}

}  // namespace game::account
