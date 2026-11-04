#pragma once

// 채팅 입력 검증 규칙. (docs/protocol-chat.md "입력 규칙")
//
// 소켓이나 방 상태와 무관한 순수 함수라서 단위 테스트로 규칙을 하나씩 확인할 수 있다.
// 모든 검증은 서버에서 한다. 클라이언트가 보낸 값은 신뢰하지 않는다.

#include <cstddef>
#include <string_view>

#include "chat.pb.h"  // game::protocol::ResultCode

namespace game::chat {

inline constexpr std::size_t kMinNicknameChars = 1;
inline constexpr std::size_t kMaxNicknameChars = 16;  // 유니코드 코드포인트 기준
inline constexpr std::size_t kMaxNicknameBytes = 64;  // UTF-8 최대 4바이트 x 16
inline constexpr std::size_t kMaxMessageBytes = 1000;

// 닉네임 규칙 검사. 통과하면 RESULT_OK, 위반하면 RESULT_INVALID_NICKNAME.
//  - 1~16 문자(유니코드 코드포인트), 64바이트 이하
//  - 앞뒤에 공백 금지 (공백: 스페이스, 줄바꿈 없는 공백 U+00A0, 전각 공백 U+3000)
//  - 제어 문자 금지 (U+0000~U+001F, U+007F~U+009F)
//  - 올바른 UTF-8이어야 함
// 닉네임 중복은 방 상태가 필요하므로 여기서 검사하지 않는다.
protocol::ResultCode ValidateNickname(std::string_view nickname);

// 메시지 본문 규칙 검사. 위반 종류에 따라 다음 중 하나를 돌려준다.
//  - RESULT_MESSAGE_TOO_LONG: 1,000바이트 초과
//  - RESULT_INVALID_ARGUMENT: 제어 문자(개행, 탭 포함) 또는 잘못된 UTF-8
//  - RESULT_EMPTY_MESSAGE: 비었거나 공백뿐
protocol::ResultCode ValidateMessageText(std::string_view text);

}  // namespace game::chat
