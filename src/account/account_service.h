#pragma once

// AccountService: 가입, 로그인, 내 기록 조회를 처리하고 "이 연결이 누구인지"를 기억한다.
//
// 하는 일
//  1. 가입(HandleRegister): ID/비밀번호 형식 검사 -> 해시 계산(일꾼 스레드) -> 저장소에 계정 만들기
//  2. 로그인(HandleLogin): 저장소에서 계정 찾기 -> 비밀번호 확인(일꾼 스레드) -> 중복 접속 확인 -> 누적 기록 읽기
//  3. 내 기록(HandleProfile): 로그인한 연결의 누적 기록(메모리의 최신 값)을 돌려주기. 누적 기록은 로그인할 때
//     저장소에서 한 번 읽고, 이후 채집/건설이 AddProgress로 더한다(저장은 게임 서비스의 농장 저장이 맡는다)
//  4. 연결별 로그인 상태 보관(LoginOf), 같은 계정의 중복 접속 거부, 로그인 실패 횟수 제한
//
// 설계에서 지킨 것 (docs/persistence.md)
//  - **해시 계산은 로직 스레드에서 하지 않는다.** Argon2id는 수십 ms가 걸리고 메모리를 쓰므로, 작은 일꾼 스레드
//    풀(hash_workers)에서 계산하고 결과만 로직 스레드로 돌려준다. 대기 중인 해시 작업에 상한을 둔다(넘으면
//    ACCOUNT_SERVER_BUSY).
//  - 오류 응답은 "ID 또는 비밀번호가 틀림" 하나로 통일하고, 없는 ID도 가짜 해시를 한 번 확인해 응답 시간으로
//    ID 존재 여부를 알아내기 어렵게 한다.
//  - 로그인 실패가 max_login_failures번 쌓이면 ACCOUNT_TOO_MANY_ATTEMPTS를 응답하고 연결을 닫게 요청한다.
//  - 비밀번호는 로그에 남기지 않는다. 저장소에는 해시만 간다.
//  - 한 연결에서 인증 요청은 한 번에 하나만 처리한다(이전 요청이 끝나기 전의 요청은 ACCOUNT_SERVER_BUSY).
//
// 스레드 규칙: 모든 Handle*/OnDisconnect/LoginOf는 서버의 로직 스레드(io_context 스레드 1개)에서만 부른다.
// 저장소 핸들러와 해시 결과도 로직 스레드에서 실행된다. 해시 계산만 일꾼 스레드에서 돈다.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include <boost/asio.hpp>
#include <boost/asio/thread_pool.hpp>

#include "account.pb.h"
#include "account/account_repository.h"
#include "account/login_info.h"
#include "account/password_hasher.h"
#include "chat/chat_client.h"

namespace game::account {

using AccountClient = chat::ChatClient;

struct AccountServiceOptions {
  unsigned hash_workers = 2;                 // 해시를 계산하는 일꾼 스레드 수(0이면 1)
  std::size_t max_pending_hash_jobs = 64;    // 대기 중인 해시 작업 상한. 넘으면 ACCOUNT_SERVER_BUSY
  unsigned max_login_failures = 5;           // 한 연결에서 로그인에 이만큼 실패하면 연결을 닫는다
};

struct AccountStatsCounters {
  using Counter = std::atomic<std::uint64_t>;

  Counter registers_ok{0};          // 가입 성공
  Counter registers_rejected{0};    // 가입 거부(형식, 중복, 저장소 오류, 바쁨)
  Counter logins_ok{0};             // 로그인 성공
  Counter login_bad_credentials{0}; // ID 또는 비밀번호가 틀림
  Counter logins_rejected{0};       // 그 밖의 로그인 거부(이미 접속 중, 저장소 오류, 바쁨)
  Counter profile_requests{0};      // 기록 조회 성공
  Counter repo_failures{0};         // 저장소 호출 실패
  Counter closes_for_failures{0};   // 로그인 실패 초과로 닫기를 요청한 횟수
  Counter max_pending_hash_jobs{0}; // 대기 중인 해시 작업의 관측 최댓값
  Counter current_logged_in{0};     // 지금 로그인한 연결 수

  std::string Summary() const;
};

class AccountService : public std::enable_shared_from_this<AccountService> {
 public:
  // 연결을 닫아 달라는 요청. 서버가 연결 번호로 세션을 찾아 닫는다(로직 스레드에서 호출된다).
  using CloseRequester = std::function<void(std::uint64_t client_id, std::string detail)>;

  // 해시 계산에 쓰는 가짜 해시(없는 ID도 같은 일을 하게 하려는 것)를 만들기 위해 hasher를 한 번 부른다.
  static std::shared_ptr<AccountService> Create(boost::asio::io_context& io,
                                                std::shared_ptr<AccountRepository> repository,
                                                std::shared_ptr<PasswordHasher> hasher,
                                                AccountServiceOptions options = {});

  // 일꾼 스레드를 모두 정리한다(진행 중인 해시 계산이 끝나기를 기다린다).
  ~AccountService();

  AccountService(const AccountService&) = delete;
  AccountService& operator=(const AccountService&) = delete;

  void SetCloseRequester(CloseRequester requester) { close_requester_ = std::move(requester); }

  // ---- 연결이 보낸 요청 처리. 응답은 client->Deliver로 돌려준다(연결이 그 사이 끊겼다면 보내지 않는다). ----
  void HandleRegister(const std::shared_ptr<AccountClient>& client, const protocol::RegisterRequest& request);
  void HandleLogin(const std::shared_ptr<AccountClient>& client, const protocol::LoginRequest& request);
  void HandleProfile(const std::shared_ptr<AccountClient>& client);

  // 연결이 끊겼을 때 부른다. 로그인 상태와 접속 중 표시를 지운다. 진행 중이던 요청의 결과는 버려진다.
  void OnDisconnect(std::uint64_t client_id);

  // 서버 종료 시 부른다. 이후 요청은 ACCOUNT_SERVER_BUSY로 거절하고, 진행 중인 요청의 응답은 보내지 않는다.
  void Shutdown();

  // 이 연결이 로그인했다면 그 정보. 게임 서비스가 방에서 쓸 닉네임을 정할 때 쓴다.
  std::optional<LoginInfo> LoginOf(std::uint64_t client_id) const;

  // 로그인한 연결의 누적 기록(메모리의 최신 값. 로그인할 때 저장소에서 읽은 값에 채집/건설이 더한 것). 로그인하지
  // 않았으면 nullopt. 게임 서비스가 저장할 절대값을 얻을 때와 "내 기록" 응답이 쓴다.
  std::optional<AccountStats> StatsOf(std::uint64_t client_id) const;

  // 로그인한 연결의 누적 기록에 delta를 더하고 갱신된 값을 돌려준다. 로그인하지 않았으면 nullopt.
  std::optional<AccountStats> AddProgress(std::uint64_t client_id, const AccountStats& delta);

  const AccountStatsCounters& stats() const { return stats_; }

 private:
  struct ConnState {
    bool busy = false;       // 이 연결의 인증 요청이 처리 중
    unsigned failures = 0;   // 로그인 실패 횟수
    std::optional<LoginInfo> login;
    AccountStats stats;      // 로그인한 동안 메모리에서 갱신하는 누적 기록
  };

  AccountService(boost::asio::io_context& io, std::shared_ptr<AccountRepository> repository,
                 std::shared_ptr<PasswordHasher> hasher, AccountServiceOptions options);

  // 일꾼 스레드에서 fn을 실행하고, 결과를 로직 스레드에서 done에 넘긴다. 대기 작업이 상한이면 false(아무것도 하지
  // 않는다). done은 서비스가 살아 있을 때만 호출된다.
  template <typename Result, typename Fn, typename Done>
  bool SubmitHashJob(Fn fn, Done done);

  void ReplyRegister(const std::shared_ptr<AccountClient>& client, protocol::AccountResult result);
  void ReplyLogin(const std::shared_ptr<AccountClient>& client, protocol::AccountResult result,
                  const AccountStats* stats = nullptr, const std::string& login_name = {});
  void ReplyProfile(const std::shared_ptr<AccountClient>& client, protocol::AccountResult result,
                    const AccountStats* stats = nullptr, const std::string& login_name = {});

  // 로그인 자격 증명이 틀렸을 때의 처리(실패 횟수 증가, 응답, 초과 시 닫기 요청).
  void FailCredentials(const std::shared_ptr<AccountClient>& client);

  void OnRegisterHashed(const std::shared_ptr<AccountClient>& client, std::string login_name, std::string hash);
  void OnRegisterStored(const std::shared_ptr<AccountClient>& client, RepoResult<AccountRecord> result);
  void OnLoginFound(const std::shared_ptr<AccountClient>& client, std::string password,
                    RepoResult<std::optional<AccountRecord>> result);
  void OnLoginVerified(const std::shared_ptr<AccountClient>& client, AccountRecord record, bool matched);
  void OnLoginStats(const std::shared_ptr<AccountClient>& client, RepoResult<AccountStats> result);

  static protocol::AccountProfile MakeProfile(const std::string& login_name, const AccountStats& stats);

  boost::asio::io_context& io_;
  std::shared_ptr<AccountRepository> repository_;
  std::shared_ptr<PasswordHasher> hasher_;
  const AccountServiceOptions options_;
  std::string dummy_hash_;  // 없는 ID의 비밀번호 확인에 쓰는 해시
  AccountStatsCounters stats_;
  CloseRequester close_requester_;

  std::unordered_map<std::uint64_t, ConnState> connections_;
  std::unordered_map<std::uint64_t, std::uint64_t> online_;  // 계정 번호 -> 접속 중인 연결 번호
  std::size_t pending_hash_jobs_ = 0;
  bool shutdown_ = false;

  // 마지막에 선언해 가장 먼저 소멸시킨다(일꾼이 아직 위 멤버를 쓰고 있을 수 있으므로 멤버 소멸 전에 합류한다).
  boost::asio::thread_pool hash_pool_;
};

}  // namespace game::account
