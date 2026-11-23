// 통계 출력 모양(--stats-format block) 단위 시험.
//
// FormatStatsBlock은 Server::FullSummary()의 한 줄을 묶음별 여러 줄로 바꾸는 순수 함수다. 사람이 보는 용도라
// 값이 빠지거나 바뀌면 안 되고, 모양이 예상과 다르면 원문을 그대로 돌려줘야 한다(통계가 사라지면 안 된다).

#include <gtest/gtest.h>

#include <string>

#include "server/stats_format.h"

namespace {

using game::server::DisplayWidth;
using game::server::FormatStatsBlock;
using game::server::ParseStatsFormat;
using game::server::StatsFormat;

TEST(StatsFormat, ParsesOnlyLineAndBlock) {
  StatsFormat format = StatsFormat::kBlock;
  EXPECT_TRUE(ParseStatsFormat("line", format));
  EXPECT_EQ(format, StatsFormat::kLine);
  EXPECT_TRUE(ParseStatsFormat("block", format));
  EXPECT_EQ(format, StatsFormat::kBlock);
  EXPECT_FALSE(ParseStatsFormat("pretty", format));
  EXPECT_FALSE(ParseStatsFormat("", format));
  EXPECT_EQ(format, StatsFormat::kBlock) << "실패하면 값을 바꾸지 않는다";
}

TEST(StatsFormat, KoreanLettersAreTwoColumnsWide) {
  EXPECT_EQ(DisplayWidth("abc"), 3u);
  EXPECT_EQ(DisplayWidth("게임"), 4u);
  EXPECT_EQ(DisplayWidth("틱시간: "), 8u);
  EXPECT_EQ(DisplayWidth(""), 0u);
}

TEST(StatsFormat, EachGroupGetsItsOwnLineAndNestedGroupsGoUnderTheirParent) {
  const std::string line =
      "연결(현재/최대/누적)=1/1/1 프레임(수신/송신)=3/180 최대송신큐=1 오류{길이=0 파싱=0} "
      "게임{방=1 플레이어=1 틱시간{이동=181 합=22610}} 농장{저장(성공/실패)=0/0}";
  const std::string expected =
      "  서버  연결(현재/최대/누적)=1/1/1  프레임(수신/송신)=3/180  최대송신큐=1\n"
      "  오류  길이=0  파싱=0\n"
      "  게임  방=1  플레이어=1\n"
      "        틱시간: 이동=181  합=22610\n"
      "  농장  저장(성공/실패)=0/0";
  EXPECT_EQ(FormatStatsBlock(line), expected);
}

TEST(StatsFormat, LongGroupsWrapAndContinuationLinesLineUpWithTheFirstItem) {
  const std::string expected =
      "  게임  a=1  b=2  c=3\n"
      "        d=4  e=5";
  EXPECT_EQ(FormatStatsBlock("게임{a=1 b=2 c=3 d=4 e=5}", 3), expected);
  EXPECT_EQ(FormatStatsBlock("게임{a=1 b=2 c=3 d=4 e=5}", 0), "  게임  a=1  b=2  c=3  d=4  e=5") << "0이면 끊지 않는다";
}

TEST(StatsFormat, NoValueIsLostOrChanged) {
  // 실제 서버 한 줄의 모양(값은 일부러 서로 다르게)으로 항목 수가 그대로인지 본다.
  const std::string line =
      "연결(현재/최대/누적)=2/2/2 프레임(수신/송신)=4/381 최대송신큐=1 오류{길이=0 알수없는ID=0 파싱=0 네트워크=0 "
      "송신큐초과=0 중간종료=0 로그인실패종료=0 내부=0} 채팅{접속(현재/최대)=0/0 저장=0 거부=0 저장실패=0 시간초과=0 "
      "혼잡거부=0 최대대기=0} 게임{방=1 플레이어=1 방생성=1 방삭제=0 입장=1 거부=0 입력반영=0 입력무시=0 틱=1000 "
      "건너뜀=0 스냅샷=377 최대틱처리=614us 채집=0 채집거부=0 고갈=0 재생=0 건설=0 건설거부=0 완공=0 농장열림=1 "
      "농장열기실패=0 틱시간{이동=315 갱신=1304 구성=1675 전송=41515 합=47348}} 계정{가입(성공/거부)=0/0 "
      "로그인(성공/틀림/거부)=2/0/0 접속중=2 기록조회=1 저장소실패=0 실패초과종료=0 해시대기최대=1} "
      "농장{저장(성공/실패)=0/0 요청=0 연속실패=0 대기농장=0}";
  const std::string block = FormatStatsBlock(line);

  const auto count_items = [](const std::string& text) {
    std::size_t n = 0;
    for (const char ch : text) n += (ch == '=') ? 1 : 0;
    return n;
  };
  EXPECT_EQ(count_items(block), count_items(line)) << block;
  EXPECT_NE(block.find("건너뜀=0"), std::string::npos);
  EXPECT_NE(block.find("전송=41515"), std::string::npos);
  EXPECT_NE(block.find("로그인(성공/틀림/거부)=2/0/0"), std::string::npos);
  EXPECT_EQ(block.find('{'), std::string::npos) << "중괄호는 줄바꿈으로 바뀐다";
  for (std::size_t start = 0; start < block.size();) {
    const std::size_t end = block.find('\n', start);
    const std::string row = block.substr(start, end == std::string::npos ? std::string::npos : end - start);
    EXPECT_LE(DisplayWidth(row), 110u) << "한 줄이 화면을 넘지 않을 만큼 짧아야 한다: " << row;
    if (end == std::string::npos) break;
    start = end + 1;
  }
}

TEST(StatsFormat, MalformedInputIsReturnedUnchanged) {
  EXPECT_EQ(FormatStatsBlock("게임{방=1"), "게임{방=1") << "닫히지 않은 묶음";
  EXPECT_EQ(FormatStatsBlock("방=1}"), "방=1}") << "여는 괄호 없는 닫는 괄호";
  EXPECT_EQ(FormatStatsBlock("{방=1}"), "{방=1}") << "이름 없는 묶음";
}

}  // namespace
