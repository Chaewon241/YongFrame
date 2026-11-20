// 계정 통합 시험: 테스트 프로세스 안에서 진짜 서버를 띄우고 실제 TCP로 가입, 로그인, 중복 접속, 실패 횟수 제한,
// 방에서의 닉네임, 저장소 장애, 비정상 입력, 비밀번호가 로그에 남지 않는지를 확인한다.
// 해시는 빠른 가짜를 쓴다(진짜 Argon2id는 password_hasher_test와 마지막의 실제 해시 시험이 확인한다).

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "account/password_hasher.h"
#include "account_test_client.h"

namespace game {
namespace {

using namespace testing_support;

const ::testing::Environment* const kQuiet = ::testing::AddGlobalTestEnvironment(new QuietLogEnvironment);

// ============================================================ 가입, 로그인, 내 기록

TEST(AccountIntegration, RegisterLoginAndProfileOverTcp) {
  TestServer server;
  auto c = Connect(server);
  EXPECT_EQ(RegisterAccount(*c, "Alice_01", "password123"), protocol::ACCOUNT_OK);
  EXPECT_EQ(server.accounts().account_count(), 1u);

  // 가입만으로는 로그인되지 않는다.
  EXPECT_EQ(GetProfile(*c).result(), protocol::ACCOUNT_NOT_LOGGED_IN);

  server.accounts().SetStats(1, account::AccountStats{12, 3, 4});
  const auto login = LoginAccount(*c, "ALICE_01", "password123");
  ASSERT_EQ(login.result(), protocol::ACCOUNT_OK);
  EXPECT_EQ(login.profile().login_name(), "alice_01");
  EXPECT_EQ(login.profile().wood_gathered(), 12u);

  const auto profile = GetProfile(*c);
  EXPECT_EQ(profile.result(), protocol::ACCOUNT_OK);
  EXPECT_EQ(profile.profile().stone_gathered(), 3u);
  EXPECT_EQ(profile.profile().houses_built(), 4u);
  EXPECT_EQ(server.account_stats().current_logged_in.load(), 1u);
}

TEST(AccountIntegration, ValidationAndDuplicateErrorsKeepTheConnectionOpen) {
  TestServer server;
  auto c = Connect(server);
  EXPECT_EQ(RegisterAccount(*c, "ab", "password123"), protocol::ACCOUNT_INVALID_LOGIN_NAME);
  EXPECT_EQ(RegisterAccount(*c, "alice_01", "short"), protocol::ACCOUNT_INVALID_PASSWORD);
  EXPECT_EQ(RegisterAccount(*c, "alice_01", "password123"), protocol::ACCOUNT_OK);
  EXPECT_EQ(RegisterAccount(*c, "ALICE_01", "password123"), protocol::ACCOUNT_LOGIN_NAME_TAKEN);
  EXPECT_TRUE(PingWorksInGame(*c)) << "규칙 위반 응답 뒤에도 연결이 살아 있어야 한다";
}

TEST(AccountIntegration, WrongPasswordAndUnknownAccountGiveTheSameAnswer) {
  TestServer server;
  auto c = Connect(server);
  ASSERT_EQ(RegisterAccount(*c, "alice_01", "password123"), protocol::ACCOUNT_OK);
  auto d = Connect(server);
  EXPECT_EQ(LoginAccount(*d, "alice_01", "wrong-password").result(), protocol::ACCOUNT_BAD_CREDENTIALS);
  EXPECT_EQ(LoginAccount(*d, "nobody_99", "password123").result(), protocol::ACCOUNT_BAD_CREDENTIALS);
}

// ============================================================ 실패 횟수 제한

TEST(AccountIntegration, FiveWrongPasswordsGetTheFinalAnswerAndThenTheConnectionIsClosed) {
  TestServer server;
  auto c = Connect(server);
  ASSERT_EQ(RegisterAccount(*c, "alice_01", "password123"), protocol::ACCOUNT_OK);

  auto attacker = Connect(server);
  for (int i = 1; i <= 4; ++i) {
    EXPECT_EQ(LoginAccount(*attacker, "alice_01", "guess-" + std::to_string(i) + "-xx").result(),
              protocol::ACCOUNT_BAD_CREDENTIALS)
        << i;
  }
  // 5번째 실패 응답은 연결이 닫히기 전에 도착한다(닫기 전에 마지막 응답이 나갈 시간을 준다).
  EXPECT_EQ(LoginAccount(*attacker, "alice_01", "guess-5-xx").result(), protocol::ACCOUNT_TOO_MANY_ATTEMPTS);
  EXPECT_TRUE(attacker->WaitForServerClose(Millis(3000))) << "실패 한도를 넘긴 연결은 서버가 닫아야 한다";
  EXPECT_TRUE(WaitUntil([&] { return server.stats().auth_failure_closes.load() == 1; }));

  // 다른 연결의 정상 로그인은 영향이 없다.
  auto good = Connect(server);
  EXPECT_EQ(LoginAccount(*good, "alice_01", "password123").result(), protocol::ACCOUNT_OK);
}

// ============================================================ 중복 접속

TEST(AccountIntegration, TheSameAccountCannotLogInTwiceUntilTheFirstConnectionLeaves) {
  TestServer server;
  auto first = ConnectLoggedIn(server, "alice_01");

  auto second = Connect(server);
  EXPECT_EQ(LoginAccount(*second, "alice_01", "password123").result(), protocol::ACCOUNT_ALREADY_LOGGED_IN);

  first->Close();
  ASSERT_TRUE(WaitUntil([&] { return server.account_stats().current_logged_in.load() == 0; }));
  EXPECT_EQ(LoginAccount(*second, "alice_01", "password123").result(), protocol::ACCOUNT_OK);
}

TEST(AccountIntegration, ADisconnectedLoginFreesTheAccountEvenWhenTheClientVanishesMidLogin) {
  TestServer server;
  {
    auto c = Connect(server);
    ASSERT_EQ(RegisterAccount(*c, "alice_01", "password123"), protocol::ACCOUNT_OK);
  }
  // 로그인 요청을 보내자마자 끊는 클라이언트를 여러 번 반복해도 계정이 붙잡혀 있으면 안 된다.
  for (int i = 0; i < 5; ++i) {
    auto c = Connect(server);
    protocol::LoginRequest request;
    request.set_login_name("alice_01");
    request.set_password("password123");
    ASSERT_TRUE(SendProto(*c, MessageId::kLoginRequest, request));
    c->Close();
  }
  ASSERT_TRUE(WaitUntil([&] { return server.account_stats().current_logged_in.load() == 0; }));
  auto d = Connect(server);
  EXPECT_EQ(LoginAccount(*d, "alice_01", "password123").result(), protocol::ACCOUNT_OK);
}

// ============================================================ 방과 이어지는 부분

TEST(AccountIntegration, ALoggedInPlayerUsesTheAccountNameAsTheNicknameInTheFarm) {
  TestServer server;
  auto alice = ConnectLoggedIn(server, "Alice_01");
  const auto created = CreateRoom(*alice, "ignored-nickname");  // 로그인한 연결은 요청의 닉네임을 무시한다
  ASSERT_EQ(created.result(), protocol::GAME_OK);
  ASSERT_EQ(created.state().players_size(), 1);
  EXPECT_EQ(created.state().players(0).nickname(), "alice_01");

  auto bob = ConnectLoggedIn(server, "bobby_02");
  const auto joined = JoinRoom(*bob, created.state().room_code(), "someone-else");
  ASSERT_EQ(joined.result(), protocol::GAME_OK);
  std::vector<std::string> names;
  for (const auto& player : joined.state().players()) names.push_back(player.nickname());
  EXPECT_EQ(names, (std::vector<std::string>{"alice_01", "bobby_02"}));

  // 로그인한 사람의 방은 농장이라 게스트는 들어갈 수 없다(닉네임을 가로채는 것도 불가능하다).
  auto guest = Connect(server);
  EXPECT_EQ(JoinRoom(*guest, created.state().room_code(), "alice_01").result(), protocol::GAME_LOGIN_REQUIRED);
}

TEST(AccountIntegration, ALoggedInPlayerCannotJoinAGuestRoom) {
  TestServer server;
  auto guest = Connect(server);
  const auto created = CreateRoom(*guest, "guest_one");
  ASSERT_EQ(created.result(), protocol::GAME_OK);

  auto alice = ConnectLoggedIn(server, "alice_01");
  EXPECT_EQ(JoinRoom(*alice, created.state().room_code(), "alice_01").result(), protocol::GAME_ROOM_NOT_FOUND)
      << "로그인한 사람은 임시 방(게스트 방)에 들어가지 않는다";
}

TEST(AccountIntegration, TheGuestFlowStillWorksWithoutALogin) {
  TestServer server;
  auto guest = Connect(server);
  const auto created = CreateRoom(*guest, "guest_one");
  ASSERT_EQ(created.result(), protocol::GAME_OK);
  EXPECT_EQ(created.state().players(0).nickname(), "guest_one");
  EXPECT_EQ(server.account_stats().current_logged_in.load(), 0u);
}

TEST(AccountIntegration, TheAccountNameIsFreeAgainAfterTheLoggedInPlayerLeavesTheRoom) {
  TestServer server;
  auto alice = ConnectLoggedIn(server, "alice_01");
  const auto created = CreateRoom(*alice, "x");
  ASSERT_EQ(created.result(), protocol::GAME_OK);
  auto bob = ConnectLoggedIn(server, "bobby_02");
  ASSERT_EQ(JoinRoom(*bob, created.state().room_code(), "bobby").result(), protocol::GAME_OK);

  LeaveRoom(*alice);
  const auto again = JoinRoom(*alice, created.state().room_code(), "ignored");
  EXPECT_EQ(again.result(), protocol::GAME_OK);
}

// ============================================================ 저장소 장애와 비정상 입력

TEST(AccountIntegration, StorageFailureIsAnsweredWithoutBreakingTheServer) {
  TestServer server;
  auto c = Connect(server);
  ASSERT_EQ(RegisterAccount(*c, "alice_01", "password123"), protocol::ACCOUNT_OK);

  server.accounts().SetFailing(true);
  auto d = Connect(server);
  EXPECT_EQ(RegisterAccount(*d, "bobby_02", "password123"), protocol::ACCOUNT_STORAGE_UNAVAILABLE);
  EXPECT_EQ(LoginAccount(*d, "alice_01", "password123").result(), protocol::ACCOUNT_STORAGE_UNAVAILABLE);
  EXPECT_TRUE(PingWorksInGame(*d)) << "저장소가 죽어도 서버와 연결은 살아 있어야 한다";
  // 게스트 게임은 저장소와 무관하게 계속 된다.
  EXPECT_EQ(CreateRoom(*d, "guest_one").result(), protocol::GAME_OK);

  server.accounts().SetFailing(false);
  auto e = Connect(server);
  EXPECT_EQ(LoginAccount(*e, "alice_01", "password123").result(), protocol::ACCOUNT_OK);
}

TEST(AccountIntegration, ASlowStorageDoesNotBlockOtherConnections) {
  TestServer server;
  auto c = Connect(server);
  ASSERT_EQ(RegisterAccount(*c, "alice_01", "password123"), protocol::ACCOUNT_OK);

  server.accounts().SetDelay(Millis(300));
  auto slow = Connect(server);
  protocol::LoginRequest request;
  request.set_login_name("alice_01");
  request.set_password("password123");
  ASSERT_TRUE(SendProto(*slow, MessageId::kLoginRequest, request));  // 저장소 응답을 300 ms 기다리는 중

  auto other = Connect(server);
  const auto start = std::chrono::steady_clock::now();
  EXPECT_TRUE(PingWorksInGame(*other));
  EXPECT_LT(std::chrono::steady_clock::now() - start, Millis(250)) << "느린 저장소가 다른 연결의 응답을 막았다";

  protocol::LoginResponse response;
  ASSERT_TRUE(Receive(*slow, MessageId::kLoginResponse, response, kGameSkip, Millis(3000)));
  EXPECT_EQ(response.result(), protocol::ACCOUNT_OK);
}

TEST(AccountIntegration, AMalformedRegisterRequestClosesOnlyThatConnection) {
  TestServer server;
  auto good = Connect(server);
  auto bad = Connect(server);
  const std::vector<std::uint8_t> garbage = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // 올바른 protobuf가 아니다
  ASSERT_TRUE(bad->SendFrame(protocol::ToUint16(MessageId::kRegisterRequest), garbage));
  EXPECT_TRUE(bad->WaitForServerClose(Millis(3000)));
  EXPECT_TRUE(WaitUntil([&] { return server.stats().parse_errors.load() == 1; }));
  EXPECT_TRUE(PingWorksInGame(*good));
}

TEST(AccountIntegration, ServerSideAccountMessagesSentByAClientAreAProtocolViolation) {
  TestServer server;
  auto c = Connect(server);
  protocol::RegisterResponse response;  // 서버가 보내는 쪽 메시지
  response.set_result(protocol::ACCOUNT_OK);
  ASSERT_TRUE(SendProto(*c, MessageId::kRegisterResponse, response));
  EXPECT_TRUE(c->WaitForServerClose(Millis(3000)));
  EXPECT_TRUE(WaitUntil([&] { return server.stats().unknown_message_ids.load() == 1; }));
}

// ============================================================ 동시성

TEST(AccountIntegration, ManyClientsRegisterAndLogInAtTheSameTime) {
  TestServer server;
  constexpr int kClients = 24;
  std::vector<std::unique_ptr<BlockingClient>> clients;
  for (int i = 0; i < kClients; ++i) {
    clients.push_back(Connect(server));
  }
  // 모두에게 가입 요청을 먼저 보낸 뒤 응답을 모아 서버에 동시에 몰리게 한다.
  for (int i = 0; i < kClients; ++i) {
    protocol::RegisterRequest request;
    request.set_login_name("player_" + std::to_string(100 + i));
    request.set_password("password123");
    ASSERT_TRUE(SendProto(*clients[i], MessageId::kRegisterRequest, request));
  }
  for (int i = 0; i < kClients; ++i) {
    protocol::RegisterResponse response;
    ASSERT_TRUE(Receive(*clients[i], MessageId::kRegisterResponse, response, kGameSkip));
    EXPECT_EQ(response.result(), protocol::ACCOUNT_OK) << i;
  }
  for (int i = 0; i < kClients; ++i) {
    protocol::LoginRequest request;
    request.set_login_name("player_" + std::to_string(100 + i));
    request.set_password("password123");
    ASSERT_TRUE(SendProto(*clients[i], MessageId::kLoginRequest, request));
  }
  for (int i = 0; i < kClients; ++i) {
    protocol::LoginResponse response;
    ASSERT_TRUE(Receive(*clients[i], MessageId::kLoginResponse, response, kGameSkip));
    EXPECT_EQ(response.result(), protocol::ACCOUNT_OK) << i;
  }
  EXPECT_EQ(server.account_stats().current_logged_in.load(), static_cast<std::uint64_t>(kClients));
  EXPECT_EQ(server.accounts().account_count(), static_cast<std::size_t>(kClients));
}

TEST(AccountIntegration, StoppingTheServerWhileLoginsAreRunningEndsCleanly) {
  TestServer server;
  std::vector<std::unique_ptr<BlockingClient>> clients;
  for (int i = 0; i < 6; ++i) {
    clients.push_back(Connect(server));
    protocol::RegisterRequest request;
    request.set_login_name("player_" + std::to_string(200 + i));
    request.set_password("password123");
    ASSERT_TRUE(SendProto(*clients.back(), MessageId::kRegisterRequest, request));
  }
  server.Stop();  // 응답을 기다리지 않고 종료한다. 정상 종료되고(시간 초과 없음) 크래시가 없어야 한다.
}

// ============================================================ 비밀번호는 로그에 남지 않는다

TEST(AccountIntegration, ThePasswordNeverAppearsInTheServerLog) {
  const std::string password = "UniquePw-7Zq9-Marker";
  std::string log_text;
  {
    server::SetMinLogLevel(server::LogLevel::kDebug);  // 가장 자세한 로그에서도 남으면 안 된다
    ::testing::internal::CaptureStdout();
    {
      TestServer server;
      auto c = Connect(server);
      ASSERT_EQ(RegisterAccount(*c, "alice_01", password), protocol::ACCOUNT_OK);
      EXPECT_EQ(LoginAccount(*c, "alice_01", password).result(), protocol::ACCOUNT_OK);
      auto d = Connect(server);
      EXPECT_EQ(LoginAccount(*d, "alice_01", password + "x").result(), protocol::ACCOUNT_BAD_CREDENTIALS);
    }  // 서버가 정상 종료할 때까지(모든 로그가 나올 때까지) 기다린다
    log_text = ::testing::internal::GetCapturedStdout();
    server::SetMinLogLevel(server::LogLevel::kError);
  }
  EXPECT_NE(log_text.find("alice_01"), std::string::npos) << "로그 캡처가 동작하지 않았다(가입/로그인 로그가 없음)";
  EXPECT_EQ(log_text.find(password), std::string::npos) << "비밀번호가 로그에 남았다";
}

// ============================================================ 진짜 해시로 한 번

TEST(AccountIntegration, TheRealArgon2HasherWorksEndToEnd) {
  auto hasher = std::make_shared<account::SodiumPasswordHasher>(account::HashCost::kMinimum);
  TestServer server(DefaultServerConfig(), nullptr, hasher);
  auto c = Connect(server);
  ASSERT_EQ(RegisterAccount(*c, "alice_01", "real-password-1"), protocol::ACCOUNT_OK);
  const std::string stored = server.accounts().StoredHash("alice_01");
  EXPECT_EQ(stored.rfind("$argon2id$", 0), 0u) << stored;
  EXPECT_EQ(stored.find("real-password-1"), std::string::npos);

  auto d = Connect(server);
  EXPECT_EQ(LoginAccount(*d, "alice_01", "real-password-2").result(), protocol::ACCOUNT_BAD_CREDENTIALS);
  EXPECT_EQ(LoginAccount(*d, "alice_01", "real-password-1").result(), protocol::ACCOUNT_OK);
}

}  // namespace
}  // namespace game
