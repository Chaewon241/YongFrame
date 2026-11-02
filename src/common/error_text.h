#pragma once

// 시스템 오류 메시지를 UTF-8 문자열로 바꾸는 도우미.
//
// 윈도우는 오류 메시지(error_code::message(), std::exception::what())를 시스템 언어의 "ANSI 코드페이지"
// (한국어 윈도우는 CP949)로 만든다. 이 프로젝트의 로그와 콘솔 출력은 UTF-8이라서 그대로 찍으면 깨진다.
// 이 함수로 UTF-8로 바꿔 쓴다. 리눅스는 이미 UTF-8이라 그대로 돌려준다.

#include <string>
#include <string_view>

#include <boost/system/error_code.hpp>

namespace game::common {

// 운영체제가 만든 메시지 문자열(윈도우: ANSI 코드페이지)을 UTF-8로 바꾼다.
std::string SystemMessageToUtf8(std::string_view message);

// error_code의 설명 문장을 UTF-8로 돌려준다.
std::string ErrorText(const boost::system::error_code& ec);

}  // namespace game::common
