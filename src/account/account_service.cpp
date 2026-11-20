#include "account/account_service.h"

#include <algorithm>
#include <sstream>
#include <utility>

#include "account/account_rules.h"
#include "server/log.h"

namespace game::account {

namespace {

using server::Log;
using server::LogLevel;

void UpdateMax(std::atomic<std::uint64_t>& target, std::uint64_t value) {
  std::uint64_t current = target.load(std::memory_order_relaxed);
  while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
  }
}

}  // namespace

std::string AccountStatsCounters::Summary() const {
  const auto get = [](const Counter& c) { return c.load(std::memory_order_relaxed); };
  std::ostringstream out;
  out << "가입(성공/거부)=" << get(registers_ok) << '/' << get(registers_rejected)
      << " 로그인(성공/틀림/거부)=" << get(logins_ok) << '/' << get(login_bad_credentials) << '/'
      << get(logins_rejected) << " 접속중=" << get(current_logged_in) << " 기록조회=" << get(profile_requests)
      << " 저장소실패=" << get(repo_failures) << " 실패초과종료=" << get(closes_for_failures)
      << " 해시대기최대=" << get(max_pending_hash_jobs);
  return out.str();
}

std::shared_ptr<AccountService> AccountService::Create(boost::asio::io_context& io,
                                                       std::shared_ptr<AccountRepository> repository,
                                                       std::shared_ptr<PasswordHasher> hasher,
                                                       AccountServiceOptions options) {
  return std::shared_ptr<AccountService>(
      new AccountService(io, std::move(repository), std::move(hasher), options));
}

AccountService::AccountService(boost::asio::io_context& io, std::shared_ptr<AccountRepository> repository,
                               std::shared_ptr<PasswordHasher> hasher, AccountServiceOptions options)
    : io_(io),
      repository_(std::move(repository)),
      hasher_(std::move(hasher)),
      options_(options),
      hash_pool_(std::max(1u, options.hash_workers)) {
  // 없는 ID에도 실제 계정과 같은 해시 확인을 하려고 가짜 해시를 하나 만들어 둔다(시작할 때 한 번).
  dummy_hash_ = hasher_->Hash("dummy-password-for-timing-equalization");
}

AccountService::~AccountService() {
  hash_pool_.stop();
  hash_pool_.join();
}

// ---------------------------------------------------------------- 도우미

template <typename Result, typename Fn, typename Done>
bool AccountService::SubmitHashJob(Fn fn, Done done) {
  if (pending_hash_jobs_ >= options_.max_pending_hash_jobs) {
    return false;
  }
  ++pending_hash_jobs_;
  UpdateMax(stats_.max_pending_hash_jobs, pending_hash_jobs_);
  // 일꾼 스레드에서 계산하고, 결과는 로직 스레드로 되돌린다. 서비스가 그 사이 소멸했다면(약한 참조가 비었다면)
  // 결과를 버린다.
  boost::asio::post(hash_pool_, [weak = weak_from_this(), &io = io_, fn = std::move(fn),
                                 done = std::move(done)]() mutable {
    Result result = fn();
    boost::asio::post(io, [weak, result = std::move(result), done = std::move(done)]() mutable {
      const auto self = weak.lock();
      if (!self) {
        return;
      }
      --self->pending_hash_jobs_;
      done(std::move(result));
    });
  });
  return true;
}

protocol::AccountProfile AccountService::MakeProfile(const std::string& login_name, const AccountStats& stats) {
  protocol::AccountProfile profile;
  profile.set_login_name(login_name);
  profile.set_wood_gathered(stats.wood_gathered);
  profile.set_stone_gathered(stats.stone_gathered);
  profile.set_houses_built(stats.houses_built);
  return profile;
}

void AccountService::ReplyRegister(const std::shared_ptr<AccountClient>& client, protocol::AccountResult result) {
  if (result == protocol::ACCOUNT_OK) {
    ++stats_.registers_ok;
  } else {
    ++stats_.registers_rejected;
  }
  protocol::RegisterResponse response;
  response.set_result(result);
  client->Deliver(protocol::MessageId::kRegisterResponse, response);
}

void AccountService::ReplyLogin(const std::shared_ptr<AccountClient>& client, protocol::AccountResult result,
                                const AccountStats* stats, const std::string& login_name) {
  if (result == protocol::ACCOUNT_OK) {
    ++stats_.logins_ok;
  } else if (result != protocol::ACCOUNT_BAD_CREDENTIALS && result != protocol::ACCOUNT_TOO_MANY_ATTEMPTS) {
    ++stats_.logins_rejected;  // 틀린 자격 증명은 FailCredentials가 따로 센다
  }
  protocol::LoginResponse response;
  response.set_result(result);
  if (result == protocol::ACCOUNT_OK && stats != nullptr) {
    *response.mutable_profile() = MakeProfile(login_name, *stats);
  }
  client->Deliver(protocol::MessageId::kLoginResponse, response);
}

void AccountService::ReplyProfile(const std::shared_ptr<AccountClient>& client, protocol::AccountResult result,
                                  const AccountStats* stats, const std::string& login_name) {
  protocol::ProfileResponse response;
  response.set_result(result);
  if (result == protocol::ACCOUNT_OK && stats != nullptr) {
    ++stats_.profile_requests;
    *response.mutable_profile() = MakeProfile(login_name, *stats);
  }
  client->Deliver(protocol::MessageId::kProfileResponse, response);
}

void AccountService::FailCredentials(const std::shared_ptr<AccountClient>& client) {
  const std::uint64_t client_id = client->client_id();
  const auto it = connections_.find(client_id);
  if (it == connections_.end()) {
    return;
  }
  it->second.busy = false;
  ++it->second.failures;
  ++stats_.login_bad_credentials;
  const bool too_many = it->second.failures >= options_.max_login_failures;
  Log(LogLevel::kWarn, "[계정] 로그인 실패 id=", client_id, " 누적=", it->second.failures);

  protocol::LoginResponse response;
  response.set_result(too_many ? protocol::ACCOUNT_TOO_MANY_ATTEMPTS : protocol::ACCOUNT_BAD_CREDENTIALS);
  client->Deliver(protocol::MessageId::kLoginResponse, response);

  if (too_many) {
    ++stats_.closes_for_failures;
    if (close_requester_) {
      close_requester_(client_id, "로그인 실패 " + std::to_string(options_.max_login_failures) + "회");
    }
  }
}

// ---------------------------------------------------------------- 가입

void AccountService::HandleRegister(const std::shared_ptr<AccountClient>& client,
                                    const protocol::RegisterRequest& request) {
  if (shutdown_) return ReplyRegister(client, protocol::ACCOUNT_SERVER_BUSY);
  ConnState& state = connections_[client->client_id()];
  if (state.busy) return ReplyRegister(client, protocol::ACCOUNT_SERVER_BUSY);
  if (state.login) return ReplyRegister(client, protocol::ACCOUNT_ALREADY_LOGGED_IN);
  if (!IsValidLoginName(request.login_name())) return ReplyRegister(client, protocol::ACCOUNT_INVALID_LOGIN_NAME);
  if (!IsValidPassword(request.password())) return ReplyRegister(client, protocol::ACCOUNT_INVALID_PASSWORD);

  std::string login_name = NormalizeLoginName(request.login_name());
  const auto hasher = hasher_;
  const bool submitted = SubmitHashJob<std::string>(
      [hasher, password = request.password()] { return hasher->Hash(password); },
      [this, client, login_name](std::string hash) mutable { OnRegisterHashed(client, login_name, std::move(hash)); });
  if (!submitted) {
    return ReplyRegister(client, protocol::ACCOUNT_SERVER_BUSY);
  }
  state.busy = true;
}

void AccountService::OnRegisterHashed(const std::shared_ptr<AccountClient>& client, std::string login_name,
                                      std::string hash) {
  const auto it = connections_.find(client->client_id());
  if (it == connections_.end() || shutdown_) {
    return;  // 그 사이 연결이 끊겼다
  }
  if (hash.empty()) {
    it->second.busy = false;
    Log(LogLevel::kError, "[계정] 비밀번호 해시 계산 실패 id=", client->client_id());
    return ReplyRegister(client, protocol::ACCOUNT_STORAGE_UNAVAILABLE);
  }
  repository_->CreateAccount(std::move(login_name), std::move(hash),
                             [weak = weak_from_this(), client](RepoResult<AccountRecord> result) {
                               if (const auto self = weak.lock()) {
                                 self->OnRegisterStored(client, std::move(result));
                               }
                             });
}

void AccountService::OnRegisterStored(const std::shared_ptr<AccountClient>& client,
                                      RepoResult<AccountRecord> result) {
  const auto it = connections_.find(client->client_id());
  if (it == connections_.end() || shutdown_) {
    return;
  }
  it->second.busy = false;
  if (result.ok) {
    Log(LogLevel::kInfo, "[계정] 가입 id=", client->client_id(), " 계정=", result.value.login_name);
    return ReplyRegister(client, protocol::ACCOUNT_OK);
  }
  if (result.error == RepoError::kDuplicate) {
    return ReplyRegister(client, protocol::ACCOUNT_LOGIN_NAME_TAKEN);
  }
  ++stats_.repo_failures;
  Log(LogLevel::kError, "[계정] 가입 저장 실패: ", result.message);
  ReplyRegister(client, protocol::ACCOUNT_STORAGE_UNAVAILABLE);
}

// ---------------------------------------------------------------- 로그인

void AccountService::HandleLogin(const std::shared_ptr<AccountClient>& client,
                                 const protocol::LoginRequest& request) {
  if (shutdown_) return ReplyLogin(client, protocol::ACCOUNT_SERVER_BUSY);
  ConnState& state = connections_[client->client_id()];
  if (state.busy) return ReplyLogin(client, protocol::ACCOUNT_SERVER_BUSY);
  if (state.login) return ReplyLogin(client, protocol::ACCOUNT_ALREADY_LOGGED_IN);
  // 한도를 넘긴 연결은 곧 닫힐 것이다(닫기까지의 짧은 시간 동안 시도를 더 받지 않는다. 해시도 계산하지 않는다).
  if (state.failures >= options_.max_login_failures) {
    return ReplyLogin(client, protocol::ACCOUNT_TOO_MANY_ATTEMPTS);
  }
  // 형식이 틀린 입력은 해시를 계산하지 않고 바로 틀린 것으로 처리하되, 실패 횟수에는 센다.
  if (!IsValidLoginName(request.login_name()) || !IsValidPassword(request.password())) {
    return FailCredentials(client);
  }

  state.busy = true;
  repository_->FindAccount(NormalizeLoginName(request.login_name()),
                           [weak = weak_from_this(), client, password = request.password()](
                               RepoResult<std::optional<AccountRecord>> result) {
                             if (const auto self = weak.lock()) {
                               self->OnLoginFound(client, password, std::move(result));
                             }
                           });
}

void AccountService::OnLoginFound(const std::shared_ptr<AccountClient>& client, std::string password,
                                  RepoResult<std::optional<AccountRecord>> result) {
  const auto it = connections_.find(client->client_id());
  if (it == connections_.end() || shutdown_) {
    return;
  }
  if (!result.ok) {
    it->second.busy = false;
    ++stats_.repo_failures;
    Log(LogLevel::kError, "[계정] 계정 조회 실패: ", result.message);
    return ReplyLogin(client, protocol::ACCOUNT_STORAGE_UNAVAILABLE);
  }

  // 없는 ID도 가짜 해시를 한 번 확인해서, 응답 시간으로 ID의 존재를 구분하기 어렵게 한다.
  const bool exists = result.value.has_value();
  AccountRecord record = exists ? *result.value : AccountRecord{};
  const std::string hash = exists ? record.password_hash : dummy_hash_;
  const auto hasher = hasher_;
  const bool submitted = SubmitHashJob<bool>(
      [hasher, hash, password = std::move(password)] { return hasher->Verify(hash, password); },
      [this, client, record = std::move(record), exists](bool matched) mutable {
        OnLoginVerified(client, std::move(record), exists && matched);
      });
  if (!submitted) {
    it->second.busy = false;
    ReplyLogin(client, protocol::ACCOUNT_SERVER_BUSY);
  }
}

void AccountService::OnLoginVerified(const std::shared_ptr<AccountClient>& client, AccountRecord record,
                                     bool matched) {
  const std::uint64_t client_id = client->client_id();
  const auto it = connections_.find(client_id);
  if (it == connections_.end() || shutdown_) {
    return;
  }
  if (!matched) {
    return FailCredentials(client);
  }
  // 같은 계정이 다른 연결에서 접속 중이면 새 접속을 거부한다(한 계정의 값을 두 곳에서 바꾸는 경합을 막는다).
  if (online_.count(record.id) != 0) {
    it->second.busy = false;
    return ReplyLogin(client, protocol::ACCOUNT_ALREADY_LOGGED_IN);
  }
  // 누적 기록을 읽는 동안 다른 연결이 같은 계정으로 끼어들지 못하게 지금 접속 중으로 표시한다.
  it->second.login = LoginInfo{record.id, record.login_name};
  online_[record.id] = client_id;
  ++stats_.current_logged_in;
  repository_->LoadStats(record.id, [weak = weak_from_this(), client](RepoResult<AccountStats> stats) {
    if (const auto self = weak.lock()) {
      self->OnLoginStats(client, std::move(stats));
    }
  });
}

void AccountService::OnLoginStats(const std::shared_ptr<AccountClient>& client, RepoResult<AccountStats> result) {
  const auto it = connections_.find(client->client_id());
  if (it == connections_.end() || shutdown_ || !it->second.login) {
    return;
  }
  it->second.busy = false;
  if (!result.ok) {
    // 기록을 못 읽었으니 로그인을 되돌린다(0으로 보여 주면 나중에 저장할 때 기록을 덮어쓸 수 있다).
    online_.erase(it->second.login->account_id);
    it->second.login.reset();
    --stats_.current_logged_in;
    ++stats_.repo_failures;
    Log(LogLevel::kError, "[계정] 누적 기록 조회 실패: ", result.message);
    return ReplyLogin(client, protocol::ACCOUNT_STORAGE_UNAVAILABLE);
  }
  it->second.failures = 0;
  it->second.stats = result.value;  // 이후 채집/건설이 이 값에 더한다
  Log(LogLevel::kInfo, "[계정] 로그인 id=", client->client_id(), " 계정=", it->second.login->login_name);
  ReplyLogin(client, protocol::ACCOUNT_OK, &result.value, it->second.login->login_name);
}

// ---------------------------------------------------------------- 내 기록

void AccountService::HandleProfile(const std::shared_ptr<AccountClient>& client) {
  if (shutdown_) return ReplyProfile(client, protocol::ACCOUNT_SERVER_BUSY);
  const auto it = connections_.find(client->client_id());
  if (it == connections_.end() || !it->second.login) {
    return ReplyProfile(client, protocol::ACCOUNT_NOT_LOGGED_IN);
  }
  // 누적 기록은 메모리의 최신 값이다(저장소를 다시 읽지 않는다). 그래서 저장소가 느리거나 죽어도 응답하고, 방금
  // 한 채집이 저장되기 전에도 바로 보인다.
  ReplyProfile(client, protocol::ACCOUNT_OK, &it->second.stats, it->second.login->login_name);
}

// ---------------------------------------------------------------- 연결 상태

void AccountService::OnDisconnect(std::uint64_t client_id) {
  const auto it = connections_.find(client_id);
  if (it == connections_.end()) {
    return;
  }
  if (it->second.login) {
    online_.erase(it->second.login->account_id);
    --stats_.current_logged_in;
  }
  connections_.erase(it);
}

void AccountService::Shutdown() { shutdown_ = true; }

std::optional<AccountStats> AccountService::StatsOf(std::uint64_t client_id) const {
  const auto it = connections_.find(client_id);
  if (it == connections_.end() || !it->second.login) {
    return std::nullopt;
  }
  return it->second.stats;
}

std::optional<AccountStats> AccountService::AddProgress(std::uint64_t client_id, const AccountStats& delta) {
  const auto it = connections_.find(client_id);
  if (it == connections_.end() || !it->second.login) {
    return std::nullopt;
  }
  AccountStats& stats = it->second.stats;
  stats.wood_gathered += delta.wood_gathered;
  stats.stone_gathered += delta.stone_gathered;
  stats.houses_built += delta.houses_built;
  return stats;
}

std::optional<LoginInfo> AccountService::LoginOf(std::uint64_t client_id) const {
  const auto it = connections_.find(client_id);
  if (it == connections_.end()) {
    return std::nullopt;
  }
  return it->second.login;
}

}  // namespace game::account
