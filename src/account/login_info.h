#pragma once

// 로그인한 연결이 가진 정보. 계정 서비스가 연결별로 보관하고, 게임 서비스가 닉네임과 농장 주인을 정할 때 읽는다.
// 게임 서비스가 계정 서비스의 무거운 헤더(비동기, 해시)를 끌어오지 않도록 따로 둔 작은 헤더다.

#include <cstdint>
#include <string>

namespace game::account {

struct LoginInfo {
  std::uint64_t account_id = 0;
  std::string login_name;  // 소문자. 방에서 닉네임으로도 쓰인다
};

}  // namespace game::account
