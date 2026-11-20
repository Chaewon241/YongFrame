#pragma once

// 계정 통합 시험이 공유하는 클라이언트 쪽 도우미. 가입/로그인/내 기록 요청을 보내고 응답을 받는다.
// 같은 연결에 방 스냅샷 등이 끼어들 수 있으므로 게임 시험의 건너뛰기 목록(kGameSkip)을 쓴다.

#include <string>

#include "account.pb.h"
#include "game_test_client.h"

namespace game::testing_support {

inline protocol::AccountResult RegisterAccount(BlockingClient& c, const std::string& name,
                                               const std::string& password) {
  protocol::RegisterRequest request;
  request.set_login_name(name);
  request.set_password(password);
  protocol::RegisterResponse response;
  response.set_result(static_cast<protocol::AccountResult>(-1));
  if (!SendProto(c, MessageId::kRegisterRequest, request) ||
      !Receive(c, MessageId::kRegisterResponse, response, kGameSkip)) {
    ADD_FAILURE() << "가입 요청/응답 실패: " << c.last_error();
  }
  return response.result();
}

inline protocol::LoginResponse LoginAccount(BlockingClient& c, const std::string& name, const std::string& password) {
  protocol::LoginRequest request;
  request.set_login_name(name);
  request.set_password(password);
  protocol::LoginResponse response;
  response.set_result(static_cast<protocol::AccountResult>(-1));
  if (!SendProto(c, MessageId::kLoginRequest, request) ||
      !Receive(c, MessageId::kLoginResponse, response, kGameSkip)) {
    ADD_FAILURE() << "로그인 요청/응답 실패: " << c.last_error();
  }
  return response;
}

inline protocol::ProfileResponse GetProfile(BlockingClient& c) {
  protocol::ProfileRequest request;
  protocol::ProfileResponse response;
  response.set_result(static_cast<protocol::AccountResult>(-1));
  if (!SendProto(c, MessageId::kProfileRequest, request) ||
      !Receive(c, MessageId::kProfileResponse, response, kGameSkip)) {
    ADD_FAILURE() << "기록 요청/응답 실패: " << c.last_error();
  }
  return response;
}

// 접속한 뒤 가입하고 로그인까지 마친 연결을 만든다.
inline std::unique_ptr<BlockingClient> ConnectLoggedIn(const TestServer& server, const std::string& name,
                                                       const std::string& password = "password123") {
  auto c = Connect(server);
  EXPECT_EQ(RegisterAccount(*c, name, password), protocol::ACCOUNT_OK) << name;
  EXPECT_EQ(LoginAccount(*c, name, password).result(), protocol::ACCOUNT_OK) << name;
  return c;
}

}  // namespace game::testing_support
