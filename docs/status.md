# 진행 상태

> **이 문서가 프로젝트 진행 상태의 기준이다.** 무엇이 끝났고, 무엇이 진행 중이며, 누가 무엇을 해야 하고, 무엇을 미뤘는지를 한곳에 둔다. 시험 개수, 사용 중인 메시지 ID 같은 "지금의 숫자"도 여기에만 둔다(다른 문서는 그 시점의 기록이다). 상태가 바뀌면 이 문서를 고치고, CLAUDE.md "현재 상태"는 요약 한 줄만 고친다. 설계, 검증 결과, 겪은 문제는 각 단계 문서에 둔다. 플랫폼은 모두 **Windows 기준**이다(Linux는 보류).

## 1. 한눈에 보기

| 영역 | 상태 | 남은 것 | 문서 |
| --- | --- | --- | --- |
| 1단계 패킷 송수신 | **완료** | Linux 확인(보류) | [stage1.md](stage1.md), [protocol.md](protocol.md) |
| 2단계 채팅 + MySQL | **완료**(실제 MySQL로 `chat_mysql_test` 13개 통과) | Linux 확인(보류) | [stage2.md](stage2.md), [protocol-chat.md](protocol-chat.md) |
| 3단계 수용 규모 측정 | **완료**: 기준, 확장(Active 4,000연결 합격), 실험 1(로그 레벨), 실험 2(Nagle). 실험 3(스레드 수)은 하지 않았고 같은 방향의 개선을 4단계 입출력 분리로 했다 | Linux 측정(보류) | [stage3.md](stage3.md), results/stage3/ |
| 4단계 게임 서버 최소 구현 | **완료**: 방 코드 입장, 서버 판정 이동, 20Hz 스냅샷 | | [stage4.md](stage4.md) |
| 4단계 확장 1: 자원 채집 | **서버, Unreal C++ 완료.** 재생 규칙은 조각 2에서 밤 정산으로 바뀌었다 | 에디터 에셋과 PIE 확인: 문서에는 "남음"이지만 계정 PIE 중에 채집을 했으므로 **끝났는지 사용자 확인 필요** | [stage4-gather.md](stage4-gather.md) |
| 콘텐츠 조각 1: 집 짓기, 이동 충돌, 구역 분리 | **서버, Unreal C++ 완료.** 부하 재측정도 했다(450개 합격, 확정 아님). 완공 규칙은 조각 2에서 밤 정산으로 바뀌었다 | 에디터 에셋과 PIE 확인(위와 같이 **확인 필요**) | [content-roadmap.md](content-roadmap.md) |
| 부하 측정과 입출력 스레드 분리 | **완료**: 분리 이전 500개 불합격, `--io-threads 2` 600개, `4` 700개까지 합격 | `IO4`의 상한, 기본값(지금 0) 결정 | [stage4.md](stage4.md), [server-journey.md](server-journey.md), results/stage4/ |
| 계정과 농장 저장 P1, P2(서버) | **구현과 인메모리 시험 완료** | 실제 MySQL 시험 `account_mysql_test`(**사용자가 실행**), P4 측정 | [persistence.md](persistence.md) |
| 계정 P3(Unreal 로그인 화면) | C++, 커맨들릿 시험, 위젯 에셋 완료. **PIE 확인 진행 중** | PIE 확인 마무리(사용자) | [unreal-client.md](unreal-client.md) "계정과 로그인 (Unreal)" |
| 계정 P4(저장 켬/끔 부하, 로그인 폭주) | 계획만 있음 | 측정 전 수치 확정과 측정 | [persistence.md](persistence.md) 9절 |
| 콘텐츠 조각 2: 하루와 잠 | **서버 S0-1~S0-5 완료**: 시계, 잠, 밤 정산, 시작 집, 프로토콜 116~119, 농장 시계 저장(마이그레이션 004, 인메모리로 시험), `--bench-day-ticks`, `game_client`의 `/sleep`·`/wake`·`/clock`, 부하 생성기의 밤 정산 대응, 변형 검증 37가지(35 잡음, 1 시험 보강 뒤 잡음, 1 동등 변형) | S0-6(문서: stage4.md 프로토콜 표, persistence.md 스키마, unreal-client.md 기준 바이트, CLAUDE.md 코드 구조), 밤 정산 비용 측정(사용자), Unreal S1~S3 | [content-day.md](content-day.md) 13절 |
| 콘텐츠 조각 2.5: 맵 여러 개 | **설계 완료**(구현 전) | 조각 2가 끝난 뒤 구현 | [content-maps.md](content-maps.md) |
| 콘텐츠 조각 3~5(농사, 경제, 제작과 도구 강화) | 기획만 있음 | 조각마다 구현 전 상세 설계 | [game-plan.md](game-plan.md), [content-roadmap.md](content-roadmap.md) |

## 2. 지금의 숫자

이 절의 숫자가 기준이다. 다른 문서의 숫자는 그 문서를 쓴 시점의 값이다.

| 항목 | 값 | 기준 시점 |
| --- | --- | --- |
| 서버 `ctest`(Debug) | **685개 등록, 648개 통과, 37개 건너뜀(MySQL 대상, 비밀번호 없음), 실패 0** | 조각 2 S0-5 뒤 ([content-day.md](content-day.md) 13절) |
| 실제 MySQL 시험 | `chat_mysql_test` 13개 통과(사용자 확인). `account_mysql_test`(계정, 농장, 농장 시계)는 **아직 실행하지 않음** | |
| Unreal `GameClientSpike` | 계정 5모드 111건(`ui` 42, `account` 46, `subsystem-account` 11, `garbage-login` 7, `login-fake` 5)과 기존 10모드 통과 | P3 S1 뒤 ([unreal-client.md](unreal-client.md)) |
| 게임 부하 최대 합격 방 수 | 분리 이전 서버 450~500개(측정일에 따라 다름, 확정 아님), `--io-threads 2` 600개, `4` 700개(상한 미측정) | [stage4.md](stage4.md), results/stage4/io-threads.md |
| 저장을 켠 구성의 성능 | **측정한 적 없음**(P4) | |

**사용 중인 번호** (새 메시지나 결과 코드를 더할 때 여기를 먼저 본다. 기준은 `src/protocol/message_ids.h`, `proto/game.proto`)

| 종류 | 사용 중 | 다음 |
| --- | --- | --- |
| 메시지 ID | 1~2(Ping), 10~13, 20~22, 30~31, 40(채팅), 100~108, 110~119(게임), 120~125(계정). 109는 비어 있음 | **126**은 조각 2.5의 `MapEntered`로 예약 |
| 게임 결과 코드 `GameResult` | 0~22 | 조각 2.5는 새 코드를 만들지 않는다 |
| `RoomState` 필드 | 1~9 | 10, 11은 조각 2.5에서 쓸 예정 |
| 마이그레이션 | 001~004(004 `farm_clock`은 조각 2 S0-3). 사용자 PC의 운영 DB에는 `db_migrate`로 004를 적용해야 `--storage mysql`의 농장 열기가 된다 | 005 |

## 3. 지금 진행 중인 일

| 일 | 누가 | 다음 단계 |
| --- | --- | --- |
| 조각 2 서버 S0-6(문서) | 구현 세션(Windows) | stage4.md 프로토콜 표, persistence.md 스키마, unreal-client.md 기준 바이트, CLAUDE.md 코드 구조. 그다음 Unreal S1 |
| P3 PIE 확인 | 사용자 | 로그인, 가입, 농장 열기, 복원된 인벤토리 표시 |

## 4. 사용자가 해야 하는 일

에이전트가 할 수 없는 일이다(비밀번호, 에디터 화면, 측정 PC).

1. `db_migrate`(004 적용)와 `account_mysql_test` 실행(비밀번호는 환경변수로, 채팅 창에 쓰지 않는다). 계정, 농장, 농장 시계(`farm_clock_test`, 서버를 껐다 켜도 날과 시각 복원) 시험이 들어 있다.
2. P3 PIE 확인 마무리.
3. 채집과 집 짓기의 에디터 에셋과 PIE 확인이 실제로 끝났는지 확인하고, 끝났으면 [stage4-gather.md](stage4-gather.md)와 [content-roadmap.md](content-roadmap.md)의 상태 줄을 고치게 한다.
4. 측정 실행(Windows, Release): 조각 2 완료 기준 12(밤 정산 비용, `powershell -ExecutionPolicy Bypass -File results\stage4\run-night-cost.ps1`로 준비됨), P4(저장 켬/끔, 아직 계획만), 조각 2.5 완료 기준 13(맵, 구현 뒤).
5. GitHub에 남은 세션 브랜치 `claude/peaceful-mccarthy-lsm5ju` 삭제(`main`과 같은 내용, 이 환경에서는 원격 브랜치 삭제가 막혀 있었다).

## 5. 보류 (사용자가 미룬 것)

| 항목 | 비고 |
| --- | --- |
| Linux 빌드, 시험, 측정 | 1단계부터. `linux-debug`, `linux-release` 프리셋은 있으나 실행해 본 적 없음 |
| TLS | 비밀번호가 평문으로 전송된다 |

## 6. 후속 후보 (설계 부채와 알려진 한계)

| 항목 | 비고 | 근거 |
| --- | --- | --- |
| 느린 연결에 낡은 스냅샷이 쌓이는 문제 | 송신 큐가 깊으면 새 스냅샷으로 대체하거나 건너뛰기. `IO1`에서 드러남 | [server-journey.md](server-journey.md) 8절 |
| `--io-threads 4`의 상한, 기본값 | 사전 규칙이 700개까지만 허용. 더 재려면 새 계획 | 같음 |
| 충돌 이후 수용량 확정 | 450개(합격)/500개(불합격)가 측정일의 PC 상태로 흔들림 | [stage4.md](stage4.md) |
| 인벤토리 일반화 | `wood`/`stone` 두 칸. 조각 3의 전제 | [content-roadmap.md](content-roadmap.md) |
| 로그인 실패 제한이 연결 단위 | IP별 제한 없음 | [persistence.md](persistence.md) 11절 |
| 스냅샷을 받는 사람마다 따로 직렬화 | 병목 증거 없음(직렬화 1% 미만) | [stage4.md](stage4.md), results/stage4/deliver-breakdown.md |
| Unreal: 3~4명 동시, 패키징 빌드, 비동기 접속 | `Connect`가 게임 스레드를 막는다 | [unreal-client.md](unreal-client.md) "다음" |
| Unreal: 미니맵이 Unreal 저장소에 커밋되지 않음 | | README "알려진 한계" |
| Kenney 에셋 라이선스 확인 | | [unreal-client.md](unreal-client.md) |
| 집이 탑뷰 카메라 시야를 가림 | 조각 6(다듬기) | [content-roadmap.md](content-roadmap.md) |

## 7. 열린 결정 (기본값으로 진행 중)

- 게임 규칙: [game-plan.md](game-plan.md) "열린 결정"(공동 목표, 잠드는 곳, 다시 열 때의 시각, 가격, 도구 소유, 광산 맵, 맵 배치)
- 건설: [content-roadmap.md](content-roadmap.md) "열린 결정"
- 채집: [stage4-gather.md](stage4-gather.md) "열린 결정"
- 작업 순서: P4(저장 부하 측정)를 언제 할지 정하지 않았다

## 8. 다음 순서 (사용자가 정한 것)

조각 2(하루와 잠) 서버 마무리 → 조각 2 Unreal S1~S3 → **조각 2.5(맵 여러 개)** → 조각 3(아이템 일반화와 농사) → 조각 4(경제) → 조각 5(제작과 도구 강화) → 조각 6(다듬기). 조각마다 S0 서버 → S1 Unreal C++ → S2 MCP 에셋 → S3 사람의 PIE 확인 순서다.
