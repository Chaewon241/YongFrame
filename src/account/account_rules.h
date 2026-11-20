#pragma once

// 계정 입력 규칙 (docs/persistence.md). 소켓이나 저장소와 무관한 순수 함수라서 단위 테스트로 하나씩 확인한다.
//
//  - ID: 영문, 숫자, 밑줄만 4~16자. 입력은 대소문자를 구분하지 않고 **소문자로 바꿔 저장한다**(NormalizeLoginName).
//    ID가 곧 닉네임이므로 기존 닉네임 규칙(채팅)을 항상 만족한다.
//  - 비밀번호: 8~64바이트(UTF-8 바이트 수). 너무 짧은 것과 해시 비용을 키우는 긴 입력을 막는다.

#include <cstddef>
#include <string>
#include <string_view>

namespace game::account {

inline constexpr std::size_t kMinLoginNameChars = 4;
inline constexpr std::size_t kMaxLoginNameChars = 16;
inline constexpr std::size_t kMinPasswordBytes = 8;
inline constexpr std::size_t kMaxPasswordBytes = 64;

// ID 형식 검사. 대소문자는 모두 허용한다(저장 전에 NormalizeLoginName으로 소문자로 바꾼다).
bool IsValidLoginName(std::string_view login_name);

// 소문자로 바꾼 ID. IsValidLoginName을 통과한 입력에만 쓴다(ASCII만 다룬다).
std::string NormalizeLoginName(std::string_view login_name);

// 비밀번호 형식 검사(길이만 본다. protobuf string이라 올바른 UTF-8이어야 도착한다).
bool IsValidPassword(std::string_view password);

}  // namespace game::account
