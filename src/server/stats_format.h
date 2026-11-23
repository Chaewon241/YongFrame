#pragma once

// 통계 한 줄을 사람이 읽기 좋은 여러 줄로 바꾼다(--stats-format block).
//
// 입력은 Server::FullSummary()가 만드는 한 줄이다:
//   "연결(현재/최대/누적)=1/1/1 프레임(수신/송신)=3/180 최대송신큐=1 오류{길이=0 ...} 채팅{...} 게임{... 틱시간{...}} ..."
// 출력은 묶음마다 한 줄이고, 항목이 많으면 몇 개씩 끊어 다음 줄로 접는다. 묶음 밖의 맨 앞 항목은 "서버" 묶음이 된다.
//   "  서버  연결(현재/최대/누적)=1/1/1  프레임(수신/송신)=3/180  최대송신큐=1\n"
//   "  오류  길이=0  알수없는ID=0  ...\n"
//   "  게임  방=1  플레이어=1  ...\n"
//   "        틱시간: 이동=181  갱신=664  ...\n"
//
// 기본 형식(한 줄)은 바꾸지 않는다. 부하 측정 기록이 그 형식의 통계 줄을 시각으로 잘라 읽기 때문이다.
// 중괄호가 맞지 않는 등 예상과 다른 입력이면 바꾸지 않고 그대로 돌려준다(통계 출력이 사라지면 안 된다).

#include <cstddef>
#include <string>
#include <string_view>

namespace game::server {

enum class StatsFormat {
  kLine,   // 한 줄(기본). 부하 측정과 기존 기록이 이 형식이다
  kBlock,  // 묶음마다 줄을 바꾼다(사람이 읽거나 화면을 찍을 때)
};

// "line" / "block"을 StatsFormat으로 바꾼다. 알 수 없는 문자열이면 false.
bool ParseStatsFormat(std::string_view text, StatsFormat& out);

// 한 줄 통계를 여러 줄로 바꾼다. 각 줄은 두 칸 들여쓰고 줄 끝마다 '\n'이 붙는다(마지막 줄 제외).
// items_per_line: 한 줄에 둘 항목 수(0이면 끊지 않음).
std::string FormatStatsBlock(std::string_view one_line, std::size_t items_per_line = 6);

// 터미널에서 보이는 폭(한글 등 넓은 글자는 2칸)으로 센다. 들여쓰기를 맞추는 데 쓴다.
std::size_t DisplayWidth(std::string_view utf8);

}  // namespace game::server
