#pragma once

// DB 비밀번호를 읽는 도우미. 비밀번호는 명령행 인자로 받지 않는다. (명령행은 셸 기록과 프로세스 목록에 남는다)
//
// 읽는 순서
//  1. 환경변수 GAME_DB_PASSWORD
//  2. 환경변수 GAME_DB_PASSWORD_FILE이 가리키는 파일의 내용 (첫 줄)
// 2번은 비밀번호를 저장소 밖의 파일에 두고 프로그램만 읽게 하고 싶을 때 쓴다. 파일 안의 UTF-8 BOM과
// 끝의 줄바꿈은 지운다. 비밀번호는 어디에도 출력하지 않는다.

#include <optional>
#include <string>

namespace game::common {

// 설정되어 있고 읽을 수 있으면 비밀번호를, 아니면 nullopt를 돌려준다.
// source_description이 null이 아니면 어디서 읽었는지(환경변수/파일) 설명을 채운다. 비밀번호 자체는 넣지 않는다.
std::optional<std::string> ReadDbPassword(std::string* source_description = nullptr);

// 환경변수 하나를 읽는다. 없거나 비어 있으면 nullopt.
std::optional<std::string> ReadEnvironment(const char* name);

}  // namespace game::common
