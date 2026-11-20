// AccountService 단위 시험: 소켓 없이 가짜 연결과 인메모리 저장소, 빠른 가짜 해시로 가입, 로그인, 중복 접속,
// 실패 횟수 제한, 저장소 장애, 해시가 로직 스레드를 막지 않는지를 확인한다.

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "account/account_service.h"
#include "account/in_memory_account_repository.h"
#include "account_test_support.h"
#include "server/log.h"

namespace game::account {
namespace {

using namespace testing_support;
using protocol::AccountResult;
using protocol::MessageId;
using Millis = std::chrono::milliseconds;

class QuietLog : public ::testing::Environment {
 public:
  void SetUp() override { server::SetMinLogLevel(server::LogLevel::kError); }
};
const ::testing::Environment* const kQuiet = ::testing::AddGlobalTestEnvironment(new QuietLog);

// 누적 기록 조회만 실패시키는 저장소(로그인 도중의 부분 실패를 재현한다). 나머지는 인메모리 저장소에 맡긴다.
class StatsFailingRepository : public AccountRepository {
 public:
  explicit StatsFailingRepository(std::shared_ptr<InMemoryAccountRepository> inner) : inner_(std::move(inner)) {}

  void CreateAccount(std::string name, std::string hash, CreateHandler handler) override {
    inner_->CreateAccount(std::move(name), std::move(hash), std::move(handler));
  }
  void FindAccount(std::string name, FindHandler handler) override { inner_->FindAccount(std::move(name), std::move(handler)); }
  void LoadStats(std::uint64_t account_id, StatsHandler handler) override {
    if (fail_stats) {
      RepoResult<AccountStats> result;
      result.error = RepoError::kFailure;
      result.message = "주입된 기록 조회 실패";
      inner_->FindAccount("", [handler = std::move(handler), result](auto) mutable { handler(std::move(result)); });
      return;
    }
    inner_->LoadStats(account_id, std::move(handler));
  }

  bool fail_stats = false;

 private:
  std::shared_ptr<InMemoryAccountRepository> inner_;
};

struct Fixture {
  explicit Fixture(AccountServiceOptions options = {}, Millis hash_delay = Millis(0))
      : repo(std::make_shared<InMemoryAccountRepository>(io)),
        hasher(std::make_shared<FastPasswordHasher>(hash_delay)) {
    service = AccountService::Create(io, repo, hasher, options);
    service->SetCloseRequester([this](std::uint64_t id, std::string detail) { closes.emplace_back(id, std::move(detail)); });
    hashes_at_start = hasher->hash_calls();
  }

  std::shared_ptr<FakeClient> NewClient(std::uint64_t id) { return std::make_shared<FakeClient>(id); }

  AccountResult Register(const std::shared_ptr<FakeClient>& c, const std::string& name, const std::string& password) {
    const auto before = c->count();
    protocol::RegisterRequest request;
    request.set_login_name(name);
    request.set_password(password);
    service->HandleRegister(c, request);
    EXPECT_TRUE(PumpUntil(io, [&] { return c->count() > before; })) << "가입 응답이 오지 않음";
    return c->Last<protocol::RegisterResponse>(MessageId::kRegisterResponse).result();
  }

  protocol::LoginResponse Login(const std::shared_ptr<FakeClient>& c, const std::string& name,
                                const std::string& password) {
    const auto before = c->count();
    protocol::LoginRequest request;
    request.set_login_name(name);
    request.set_password(password);
    service->HandleLogin(c, request);
    EXPECT_TRUE(PumpUntil(io, [&] { return c->count() > before; })) << "로그인 응답이 오지 않음";
    return c->Last<protocol::LoginResponse>(MessageId::kLoginResponse);
  }

  protocol::ProfileResponse Profile(const std::shared_ptr<FakeClient>& c) {
    const auto before = c->count();
    service->HandleProfile(c);
    EXPECT_TRUE(PumpUntil(io, [&] { return c->count() > before; })) << "기록 응답이 오지 않음";
    return c->Last<protocol::ProfileResponse>(MessageId::kProfileResponse);
  }

  // 응답이 오지 않는다는 것을 확인할 때 쓴다(제한 시간 동안 돌려 본다).
  void PumpFor(Millis duration) {
    const auto end = std::chrono::steady_clock::now() + duration;
    PumpUntil(io, [&] { return std::chrono::steady_clock::now() >= end; }, duration + Millis(500));
  }

  boost::asio::io_context io;
  std::shared_ptr<InMemoryAccountRepository> repo;
  std::shared_ptr<FastPasswordHasher> hasher;
  std::shared_ptr<AccountService> service;
  std::vector<std::pair<std::uint64_t, std::string>> closes;
  std::uint64_t hashes_at_start = 0;
};

// ============================================================ 가입

TEST(AccountService, RegisterThenLoginWorksAndReturnsTheProfile) {
  Fixture f;
  auto c = f.NewClient(1);
  EXPECT_EQ(f.Register(c, "alice_01", "password123"), protocol::ACCOUNT_OK);
  EXPECT_EQ(f.repo->account_count(), 1u);

  f.repo->SetStats(1, AccountStats{5, 6, 7});
  const auto login = f.Login(c, "alice_01", "password123");
  EXPECT_EQ(login.result(), protocol::ACCOUNT_OK);
  EXPECT_EQ(login.profile().login_name(), "alice_01");
  EXPECT_EQ(login.profile().wood_gathered(), 5u);
  EXPECT_EQ(login.profile().stone_gathered(), 6u);
  EXPECT_EQ(login.profile().houses_built(), 7u);
  ASSERT_TRUE(f.service->LoginOf(1).has_value());
  EXPECT_EQ(f.service->LoginOf(1)->login_name, "alice_01");
}

TEST(AccountService, LoginNamesAreCaseInsensitiveAndStoredInLowercase) {
  Fixture f;
  auto a = f.NewClient(1);
  auto b = f.NewClient(2);
  EXPECT_EQ(f.Register(a, "Alice_01", "password123"), protocol::ACCOUNT_OK);
  EXPECT_EQ(f.Register(b, "ALICE_01", "otherpass123"), protocol::ACCOUNT_LOGIN_NAME_TAKEN);
  EXPECT_EQ(f.repo->account_count(), 1u);

  const auto login = f.Login(a, "aLiCe_01", "password123");
  EXPECT_EQ(login.result(), protocol::ACCOUNT_OK);
  EXPECT_EQ(login.profile().login_name(), "alice_01");  // 방에서 닉네임으로 쓰이는 값도 소문자
}

TEST(AccountService, InvalidLoginNameAndPasswordAreRejectedWithoutHashing) {
  Fixture f;
  auto c = f.NewClient(1);
  const auto hashes_before = f.hasher->hash_calls();
  EXPECT_EQ(f.Register(c, "ab", "password123"), protocol::ACCOUNT_INVALID_LOGIN_NAME);
  EXPECT_EQ(f.Register(c, "has space", "password123"), protocol::ACCOUNT_INVALID_LOGIN_NAME);
  EXPECT_EQ(f.Register(c, "alice_01", "short"), protocol::ACCOUNT_INVALID_PASSWORD);
  EXPECT_EQ(f.Register(c, "alice_01", std::string(65, 'x')), protocol::ACCOUNT_INVALID_PASSWORD);
  EXPECT_EQ(f.repo->account_count(), 0u);
  EXPECT_EQ(f.hasher->hash_calls(), hashes_before) << "형식이 틀린 요청에 비싼 해시를 계산하면 안 된다";
}

TEST(AccountService, TheStoredValueIsTheHashNotThePassword) {
  Fixture f;
  auto c = f.NewClient(1);
  ASSERT_EQ(f.Register(c, "alice_01", "plain-password-1"), protocol::ACCOUNT_OK);
  const std::string stored = f.repo->StoredHash("alice_01");
  EXPECT_NE(stored, "plain-password-1");
  EXPECT_EQ(stored.rfind("fast$", 0), 0u) << "저장된 값은 해시 구현이 만든 문자열이어야 한다: " << stored;
}

TEST(AccountService, RegisteringWhileLoggedInIsRejected) {
  Fixture f;
  auto c = f.NewClient(1);
  ASSERT_EQ(f.Register(c, "alice_01", "password123"), protocol::ACCOUNT_OK);
  ASSERT_EQ(f.Login(c, "alice_01", "password123").result(), protocol::ACCOUNT_OK);
  EXPECT_EQ(f.Register(c, "bobby_02", "password123"), protocol::ACCOUNT_ALREADY_LOGGED_IN);
  EXPECT_EQ(f.repo->account_count(), 1u);
}

// ============================================================ 로그인

TEST(AccountService, WrongPasswordAndUnknownAccountLookTheSameAndBothRunAVerification) {
  Fixture f;
  auto a = f.NewClient(1);
  ASSERT_EQ(f.Register(a, "alice_01", "password123"), protocol::ACCOUNT_OK);

  auto b = f.NewClient(2);
  const auto verify_before = f.hasher->verify_calls();
  EXPECT_EQ(f.Login(b, "alice_01", "wrong-password").result(), protocol::ACCOUNT_BAD_CREDENTIALS);
  EXPECT_EQ(f.hasher->verify_calls(), verify_before + 1);
  EXPECT_EQ(f.Login(b, "nobody_99", "password123").result(), protocol::ACCOUNT_BAD_CREDENTIALS);
  EXPECT_EQ(f.hasher->verify_calls(), verify_before + 2) << "없는 ID도 가짜 해시를 확인해 응답 시간을 맞춰야 한다";
  EXPECT_FALSE(f.service->LoginOf(2).has_value());
}

TEST(AccountService, TooManyFailuresAnswersTooManyAttemptsAndAsksToCloseTheConnection) {
  Fixture f;  // 기본 한도 5회
  auto a = f.NewClient(1);
  ASSERT_EQ(f.Register(a, "alice_01", "password123"), protocol::ACCOUNT_OK);

  auto b = f.NewClient(7);
  for (int i = 1; i <= 4; ++i) {
    EXPECT_EQ(f.Login(b, "alice_01", "wrong-password").result(), protocol::ACCOUNT_BAD_CREDENTIALS) << i;
    EXPECT_TRUE(f.closes.empty()) << i << "번째 실패에서 닫으면 안 된다";
  }
  EXPECT_EQ(f.Login(b, "alice_01", "wrong-password").result(), protocol::ACCOUNT_TOO_MANY_ATTEMPTS);
  ASSERT_EQ(f.closes.size(), 1u);
  EXPECT_EQ(f.closes[0].first, 7u);
  EXPECT_EQ(f.service->stats().closes_for_failures.load(), 1u);

  // 닫히기 전의 짧은 시간에도 시도를 더 받지 않는다: 올바른 비밀번호여도 거절하고 해시도 계산하지 않는다.
  const auto verify_before = f.hasher->verify_calls();
  EXPECT_EQ(f.Login(b, "alice_01", "password123").result(), protocol::ACCOUNT_TOO_MANY_ATTEMPTS);
  EXPECT_EQ(f.hasher->verify_calls(), verify_before);
  EXPECT_EQ(f.closes.size(), 1u) << "닫기 요청을 반복하면 안 된다";
}

TEST(AccountService, FailureLimitIsConfigurable) {
  AccountServiceOptions options;
  options.max_login_failures = 2;
  Fixture f(options);
  auto b = f.NewClient(3);
  EXPECT_EQ(f.Login(b, "nobody_99", "password123").result(), protocol::ACCOUNT_BAD_CREDENTIALS);
  EXPECT_EQ(f.Login(b, "nobody_99", "password123").result(), protocol::ACCOUNT_TOO_MANY_ATTEMPTS);
  EXPECT_EQ(f.closes.size(), 1u);
}

TEST(AccountService, MalformedLoginsCountAsFailuresButNeverReachTheHasher) {
  Fixture f;
  auto b = f.NewClient(3);
  const auto verify_before = f.hasher->verify_calls();
  for (int i = 1; i <= 4; ++i) {
    EXPECT_EQ(f.Login(b, "x", "p").result(), protocol::ACCOUNT_BAD_CREDENTIALS) << i;
  }
  EXPECT_EQ(f.Login(b, "x", "p").result(), protocol::ACCOUNT_TOO_MANY_ATTEMPTS);
  EXPECT_EQ(f.closes.size(), 1u);
  EXPECT_EQ(f.hasher->verify_calls(), verify_before);
}

TEST(AccountService, StorageFailuresDoNotCountAsWrongPasswords) {
  Fixture f;
  auto a = f.NewClient(1);
  ASSERT_EQ(f.Register(a, "alice_01", "password123"), protocol::ACCOUNT_OK);

  f.repo->SetFailing(true);
  auto b = f.NewClient(2);
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(f.Login(b, "alice_01", "password123").result(), protocol::ACCOUNT_STORAGE_UNAVAILABLE);
  }
  EXPECT_TRUE(f.closes.empty());
  f.repo->SetFailing(false);
  EXPECT_EQ(f.Login(b, "alice_01", "password123").result(), protocol::ACCOUNT_OK);
}

TEST(AccountService, LoggingInTwiceOnTheSameConnectionIsRejected) {
  Fixture f;
  auto c = f.NewClient(1);
  ASSERT_EQ(f.Register(c, "alice_01", "password123"), protocol::ACCOUNT_OK);
  ASSERT_EQ(f.Login(c, "alice_01", "password123").result(), protocol::ACCOUNT_OK);
  EXPECT_EQ(f.Login(c, "alice_01", "password123").result(), protocol::ACCOUNT_ALREADY_LOGGED_IN);
}

TEST(AccountService, TheSameAccountCannotBeLoggedInFromTwoConnectionsUntilTheFirstLeaves) {
  Fixture f;
  auto a = f.NewClient(1);
  auto b = f.NewClient(2);
  ASSERT_EQ(f.Register(a, "alice_01", "password123"), protocol::ACCOUNT_OK);
  ASSERT_EQ(f.Login(a, "alice_01", "password123").result(), protocol::ACCOUNT_OK);

  EXPECT_EQ(f.Login(b, "alice_01", "password123").result(), protocol::ACCOUNT_ALREADY_LOGGED_IN);
  EXPECT_FALSE(f.service->LoginOf(2).has_value());
  EXPECT_TRUE(f.closes.empty()) << "중복 접속 거부는 실패 횟수에 세지 않는다";

  f.service->OnDisconnect(1);
  EXPECT_FALSE(f.service->LoginOf(1).has_value());
  EXPECT_EQ(f.Login(b, "alice_01", "password123").result(), protocol::ACCOUNT_OK);
  EXPECT_EQ(f.service->stats().current_logged_in.load(), 1u);
}

TEST(AccountService, ALoginThatCannotReadTheStatsIsRolledBack) {
  boost::asio::io_context io;  // Fixture를 쓰지 않고 부분 실패 저장소를 직접 끼운다
  auto inner = std::make_shared<InMemoryAccountRepository>(io);
  auto repo = std::make_shared<StatsFailingRepository>(inner);
  auto hasher = std::make_shared<FastPasswordHasher>();
  auto service = AccountService::Create(io, repo, hasher);

  auto c = std::make_shared<FakeClient>(1);
  protocol::RegisterRequest reg;
  reg.set_login_name("alice_01");
  reg.set_password("password123");
  service->HandleRegister(c, reg);
  ASSERT_TRUE(PumpUntil(io, [&] { return c->count() == 1; }));

  protocol::LoginRequest login;
  login.set_login_name("alice_01");
  login.set_password("password123");
  repo->fail_stats = true;
  service->HandleLogin(c, login);
  ASSERT_TRUE(PumpUntil(io, [&] { return c->count() == 2; }));
  EXPECT_EQ(c->Last<protocol::LoginResponse>(MessageId::kLoginResponse).result(), protocol::ACCOUNT_STORAGE_UNAVAILABLE);
  EXPECT_FALSE(service->LoginOf(1).has_value()) << "기록을 못 읽었으면 로그인 상태가 남으면 안 된다";
  EXPECT_EQ(service->stats().current_logged_in.load(), 0u);

  // 되돌려졌으므로 같은 계정이 "이미 접속 중"으로 막히지 않고 다시 로그인할 수 있다.
  repo->fail_stats = false;
  service->HandleLogin(c, login);
  ASSERT_TRUE(PumpUntil(io, [&] { return c->count() == 3; }));
  EXPECT_EQ(c->Last<protocol::LoginResponse>(MessageId::kLoginResponse).result(), protocol::ACCOUNT_OK);
}

// ============================================================ 내 기록

TEST(AccountService, ProfileNeedsALoginAndReturnsTheStatsReadAtLogin) {
  Fixture f;
  auto c = f.NewClient(1);
  EXPECT_EQ(f.Profile(c).result(), protocol::ACCOUNT_NOT_LOGGED_IN);

  ASSERT_EQ(f.Register(c, "alice_01", "password123"), protocol::ACCOUNT_OK);
  f.repo->SetStats(1, AccountStats{11, 22, 3});  // 로그인할 때 저장소에서 읽는 값
  ASSERT_EQ(f.Login(c, "alice_01", "password123").result(), protocol::ACCOUNT_OK);
  const auto profile = f.Profile(c);
  EXPECT_EQ(profile.result(), protocol::ACCOUNT_OK);
  EXPECT_EQ(profile.profile().login_name(), "alice_01");
  EXPECT_EQ(profile.profile().wood_gathered(), 11u);
  EXPECT_EQ(profile.profile().stone_gathered(), 22u);
  EXPECT_EQ(profile.profile().houses_built(), 3u);

  // 기록은 메모리의 값이라 저장소가 죽어도 응답한다.
  f.repo->SetFailing(true);
  EXPECT_EQ(f.Profile(c).result(), protocol::ACCOUNT_OK);
}

TEST(AccountService, ProgressIsAddedInMemoryAndShownInTheProfileAndStatsOf) {
  Fixture f;
  auto c = f.NewClient(1);
  EXPECT_FALSE(f.service->StatsOf(1).has_value());
  EXPECT_FALSE(f.service->AddProgress(1, AccountStats{1, 1, 1}).has_value()) << "로그인하지 않은 연결에는 더하지 않는다";

  ASSERT_EQ(f.Register(c, "alice_01", "password123"), protocol::ACCOUNT_OK);
  f.repo->SetStats(1, AccountStats{10, 20, 1});
  ASSERT_EQ(f.Login(c, "alice_01", "password123").result(), protocol::ACCOUNT_OK);

  const auto after = f.service->AddProgress(1, AccountStats{3, 4, 1});
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->wood_gathered, 13u);
  EXPECT_EQ(after->stone_gathered, 24u);
  EXPECT_EQ(after->houses_built, 2u);
  EXPECT_EQ(f.service->StatsOf(1)->wood_gathered, 13u);

  const auto profile = f.Profile(c);
  EXPECT_EQ(profile.profile().wood_gathered(), 13u);
  EXPECT_EQ(profile.profile().houses_built(), 2u);

  // 로그아웃(접속 종료)하면 더 이상 더할 수 없다.
  f.service->OnDisconnect(1);
  EXPECT_FALSE(f.service->AddProgress(1, AccountStats{1, 1, 1}).has_value());
}

// ============================================================ 저장소 장애

TEST(AccountService, RegisterFailsCleanlyWhenTheStorageIsDown) {
  Fixture f;
  auto c = f.NewClient(1);
  f.repo->SetFailing(true);
  EXPECT_EQ(f.Register(c, "alice_01", "password123"), protocol::ACCOUNT_STORAGE_UNAVAILABLE);
  EXPECT_EQ(f.repo->account_count(), 0u);
  f.repo->SetFailing(false);
  EXPECT_EQ(f.Register(c, "alice_01", "password123"), protocol::ACCOUNT_OK) << "복구되면 같은 연결이 다시 가입할 수 있어야 한다";
}

// ============================================================ 연결 끊김과 동시성

TEST(AccountService, DisconnectingWhileTheHashIsRunningDropsTheResultAndFreesTheAccount) {
  Fixture f({}, Millis(150));
  auto a = f.NewClient(1);
  ASSERT_EQ(f.Register(a, "alice_01", "password123"), protocol::ACCOUNT_OK);

  auto b = f.NewClient(2);
  protocol::LoginRequest request;
  request.set_login_name("alice_01");
  request.set_password("password123");
  const auto verify_before = f.hasher->verify_calls();
  f.service->HandleLogin(b, request);
  ASSERT_TRUE(PumpUntil(f.io, [&] { return f.hasher->verify_calls() > verify_before; })) << "비밀번호 확인이 시작되지 않음";

  f.service->OnDisconnect(2);  // 확인 중에 연결이 끊긴다
  f.PumpFor(Millis(400));
  EXPECT_EQ(b->count(), 0u) << "끊긴 연결에 응답을 보내면 안 된다";
  EXPECT_FALSE(f.service->LoginOf(2).has_value());

  auto c = f.NewClient(3);
  EXPECT_EQ(f.Login(c, "alice_01", "password123").result(), protocol::ACCOUNT_OK) << "끊긴 연결이 계정을 붙잡고 있으면 안 된다";
}

TEST(AccountService, HashingRunsOffTheLogicThread) {
  Fixture f({}, Millis(200));
  auto c = f.NewClient(1);
  protocol::RegisterRequest request;
  request.set_login_name("alice_01");
  request.set_password("password123");
  f.service->HandleRegister(c, request);

  // 해시가 도는 200 ms 동안에도 로직 스레드(이 스레드)가 다른 일을 계속 처리해야 한다. 10 ms 타이머가 몇 번
  // 울렸는지로 확인한다. 해시를 로직 스레드에서 직접 계산하면 그동안 타이머가 한 번도 울리지 못한다.
  int ticks = 0;
  boost::asio::steady_timer timer(f.io);
  std::function<void()> tick = [&] {
    ++ticks;
    timer.expires_after(Millis(10));
    timer.async_wait([&](const boost::system::error_code& ec) {
      if (!ec) tick();
    });
  };
  tick();
  ASSERT_TRUE(PumpUntil(f.io, [&] { return c->count() > 0; }, Millis(3000)));
  timer.cancel();
  f.io.restart();
  f.io.poll();
  EXPECT_GE(ticks, 5) << "해시 계산 중에 로직 스레드가 막혔다";
}

TEST(AccountService, ASecondRequestOnTheSameConnectionWhileOneIsRunningIsBusy) {
  Fixture f({}, Millis(150));
  auto c = f.NewClient(1);
  protocol::RegisterRequest first;
  first.set_login_name("alice_01");
  first.set_password("password123");
  f.service->HandleRegister(c, first);

  protocol::RegisterRequest second;
  second.set_login_name("bobby_02");
  second.set_password("password123");
  f.service->HandleRegister(c, second);  // 첫 요청이 아직 처리 중
  ASSERT_GE(c->count(), 1u);
  EXPECT_EQ(c->Last<protocol::RegisterResponse>(MessageId::kRegisterResponse).result(), protocol::ACCOUNT_SERVER_BUSY);

  ASSERT_TRUE(PumpUntil(f.io, [&] { return c->count() >= 2; }));
  EXPECT_EQ(f.repo->account_count(), 1u) << "바쁨으로 거절된 두 번째 요청은 계정을 만들면 안 된다";
}

TEST(AccountService, TooManyPendingHashJobsAreAnsweredWithBusy) {
  AccountServiceOptions options;
  options.hash_workers = 1;
  options.max_pending_hash_jobs = 1;
  Fixture f(options, Millis(150));
  auto a = f.NewClient(1);
  auto b = f.NewClient(2);

  protocol::RegisterRequest ra;
  ra.set_login_name("alice_01");
  ra.set_password("password123");
  f.service->HandleRegister(a, ra);  // 해시 작업 1개가 대기

  protocol::RegisterRequest rb;
  rb.set_login_name("bobby_02");
  rb.set_password("password123");
  f.service->HandleRegister(b, rb);  // 상한(1)이라 바로 거절
  ASSERT_EQ(b->count(), 1u);
  EXPECT_EQ(b->Last<protocol::RegisterResponse>(MessageId::kRegisterResponse).result(), protocol::ACCOUNT_SERVER_BUSY);

  ASSERT_TRUE(PumpUntil(f.io, [&] { return a->count() == 1; }));
  EXPECT_EQ(a->Last<protocol::RegisterResponse>(MessageId::kRegisterResponse).result(), protocol::ACCOUNT_OK);
  // 대기가 풀리면 다시 받는다.
  EXPECT_EQ(f.Register(b, "bobby_02", "password123"), protocol::ACCOUNT_OK);
}

TEST(AccountService, ConcurrentRegistrationsOfTheSameNameCreateExactlyOneAccount) {
  Fixture f;
  std::vector<std::shared_ptr<FakeClient>> clients;
  for (std::uint64_t id = 1; id <= 4; ++id) {
    clients.push_back(f.NewClient(id));
    protocol::RegisterRequest request;
    request.set_login_name("alice_01");
    request.set_password("password123");
    f.service->HandleRegister(clients.back(), request);
  }
  ASSERT_TRUE(PumpUntil(f.io, [&] {
    for (const auto& c : clients) {
      if (c->count() == 0) return false;
    }
    return true;
  }));
  int ok = 0;
  int taken = 0;
  for (const auto& c : clients) {
    const auto result = c->Last<protocol::RegisterResponse>(MessageId::kRegisterResponse).result();
    ok += result == protocol::ACCOUNT_OK;
    taken += result == protocol::ACCOUNT_LOGIN_NAME_TAKEN;
  }
  EXPECT_EQ(ok, 1);
  EXPECT_EQ(taken, 3);
  EXPECT_EQ(f.repo->account_count(), 1u);
}

// ============================================================ 종료와 상태

TEST(AccountService, AfterShutdownRequestsAreRefusedAsBusy) {
  Fixture f;
  auto c = f.NewClient(1);
  f.service->Shutdown();
  protocol::RegisterRequest request;
  request.set_login_name("alice_01");
  request.set_password("password123");
  f.service->HandleRegister(c, request);
  ASSERT_EQ(c->count(), 1u);
  EXPECT_EQ(c->Last<protocol::RegisterResponse>(MessageId::kRegisterResponse).result(), protocol::ACCOUNT_SERVER_BUSY);
}

TEST(AccountService, LoginOfIsEmptyForUnknownAndDisconnectedConnections) {
  Fixture f;
  EXPECT_FALSE(f.service->LoginOf(99).has_value());
  auto c = f.NewClient(1);
  ASSERT_EQ(f.Register(c, "alice_01", "password123"), protocol::ACCOUNT_OK);
  ASSERT_EQ(f.Login(c, "alice_01", "password123").result(), protocol::ACCOUNT_OK);
  EXPECT_TRUE(f.service->LoginOf(1).has_value());
  f.service->OnDisconnect(1);
  EXPECT_FALSE(f.service->LoginOf(1).has_value());
  f.service->OnDisconnect(1);  // 두 번 불러도 안전하다
}

TEST(AccountService, CountersFollowTheOutcomes) {
  Fixture f;
  auto a = f.NewClient(1);
  auto b = f.NewClient(2);
  ASSERT_EQ(f.Register(a, "alice_01", "password123"), protocol::ACCOUNT_OK);
  EXPECT_EQ(f.Register(b, "alice_01", "password123"), protocol::ACCOUNT_LOGIN_NAME_TAKEN);
  EXPECT_EQ(f.Login(b, "alice_01", "wrong-password").result(), protocol::ACCOUNT_BAD_CREDENTIALS);
  ASSERT_EQ(f.Login(a, "alice_01", "password123").result(), protocol::ACCOUNT_OK);
  EXPECT_EQ(f.Login(b, "alice_01", "password123").result(), protocol::ACCOUNT_ALREADY_LOGGED_IN);

  const auto& s = f.service->stats();
  EXPECT_EQ(s.registers_ok.load(), 1u);
  EXPECT_EQ(s.registers_rejected.load(), 1u);
  EXPECT_EQ(s.logins_ok.load(), 1u);
  EXPECT_EQ(s.login_bad_credentials.load(), 1u);
  EXPECT_EQ(s.logins_rejected.load(), 1u);
  EXPECT_EQ(s.current_logged_in.load(), 1u);
}

}  // namespace
}  // namespace game::account
