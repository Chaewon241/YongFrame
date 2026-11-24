# CLAUDE.md

Cross-Platform Game Server Lab: Boost.Asio 기반 C++ 게임 서버를 단계별로 만들고, 각 단계의 정확성과 성능을 재현 가능한 기록으로 남기는 개인 프로젝트다. 문서와 대화는 한국어로 쓴다.

## 현재 상태

- 1단계(패킷 송수신)는 **Windows 기준으로 완료**했다. Linux 확인은 사용자가 보류했다. **2단계(채팅 + MySQL)는 Windows 기준 완료**: 인메모리 채팅과 MySQL 저장소 모두 구현했고, 실제 MySQL 대상 `chat_mysql_test` 13개가 통과했다. **3단계(측정) 진행 중**: 기준·확장 측정을 마쳤고(Active 4,000연결까지 합격, 서버 스레드 1개의 처리량 상한 관측 약 4.5만/s) 비교 실험 1(로그 레벨)도 완료(`debug`는 응답당 CPU 5~6배, 부하 측정은 `info`), 실험 2(Nagle)도 완료(끄는 이득 없음, 켬 유지)이고 다음은 실험 3(스레드 수)(docs/stage3.md, results/stage3/). 진행 상태는 [docs/stage2.md](docs/stage2.md). 빌드 환경(CMake + vcpkg)은 Windows에서 확인됐고, `src/hello_asio.cpp`는 툴체인 확인용이다.
- 구현됨: 프레임 코덱, Ping 서버(`game_server`), 동기식 테스트 클라이언트(`ping_client`, `BlockingClient`), 채팅(입장/퇴장/전파/기록 조회, 인메모리 저장소), 대화형 `chat_client`. `ctest` 121개 중 110개 통과, MySQL 대상 11개는 비밀번호가 없는 환경에서는 건너뛰며 비밀번호를 주고 사용자가 `chat_mysql_test`를 실행해 13개 통과를 확인했다. 1단계 결과는 [docs/stage1.md](docs/stage1.md)에 정리됨. 보류: Linux 빌드 확인(VM), 확인 후 stage1.md 갱신.
- **4단계(게임) 서버 최소 구현 완료(Windows)**: 방 만들기, 6자리 숫자 코드로 입장(최대 4명), 방장 변경, 빈 방 삭제, 서버 판정 이동(속도 360 cm/s, 처음에는 300이었고 이 값으로 부하를 쟀다. 맵 5,000 x 5,000 cm), 20Hz 방 전체 스냅샷. `ctest` 218개 통과(MySQL 11개 건너뜀. 채집 확장 뒤에는 251개). 값은 사용자가 정하지 않아 **임의 기본값**이며 `PlayConfig`에서 바꾼다. **부하 측정 완료: 방 500개(플레이어 2,000명)까지 합격, 600개부터 불합격**(results/stage4/game-load.md, Windows 루프백 한 대, 충돌과 속도 360 이전 서버. 이후 재측정은 450개까지 합격이었으나 확정 아님). Unreal 쪽 연동은 다음 작업이다. 설계와 검증은 [docs/stage4.md](docs/stage4.md), Unreal용 프로토콜과 기준 바이트는 [docs/unreal-client.md](docs/unreal-client.md).
- **Unreal 연동 스파이크 성공**: `D:\UnrealProjects\YongClient`(이 저장소와 별개)에서 서버에 접속해 Ping 5건 왕복을 확인했다(docs/unreal-client.md). 5.4.4에서 먼저 확인하고 **UE 5.8.4(`D:\UE_5.8`)로 올려 다시 통과**했다(5.4 시점 프로젝트는 `YongClient_UE54_backup`). 5.8은 MSVC 14.44가 선호 버전이라 5.4 때의 우회가 필요 없고, 타깃은 `BuildSettingsVersion.Latest`를 쓴다. 이 PC의 MSVC 14.38은 x64 lib가 없는 불완전 설치다. Unreal 쪽 Protobuf는 서버와 같은 6.33.4를 복사해 쓴다. Unreal 프로젝트에는 접속 계층(`FGameNetworkConnection`)과 게임 메시지 계층(`FGameClient`)이 구현되어 실제 서버로 시험을 통과했다(docs/unreal-client.md). 그 프로젝트는 이 저장소와 별개인 로컬 git 저장소다(원격 없음). **게임 클라이언트 최소판 완료**: 서브시스템, 스냅샷 보간, 탑뷰 카메라, WASD 이동, 메뉴(방 만들기/코드 입장)와 HUD(UMG, 로직은 C++)를 만들었고, 에디터에서 창 2개(Play Standalone)로 서버에 접속해 한 방에 여러 명이 보이는 것을 사용자가 확인했다(docs/unreal-client.md "메뉴와 HUD"). 에셋은 Git LFS. **4단계 확장 1(자원 채집) 서버와 Unreal C++ 구현 완료(Windows, 에디터 쪽 에셋과 PIE 확인은 남음)**: 서버가 나무 30개/돌 20개 노드의 배치, 체력, 재생(30초)과 거리(200 cm)·쿨다운(0.5초) 판정을 소유하고, `E`로 쳐서 목재/석재를 얻는다. 서버 `ctest` 251개 통과(MySQL 11개 건너뜀), 변형 검증 서버 20가지를 모두 잡았다. 설계와 검증은 [docs/stage4-gather.md](docs/stage4-gather.md). **콘텐츠 조각 1(집 짓기와 이동 충돌) 서버와 Unreal C++ 구현 완료(Windows, 에디터 에셋과 PIE 확인, 부하 재측정은 남음)**: B로 건설 모드, 마우스 클릭으로 300x300 cm 집을 지음(목재 20, 석재 10, 5초 건설 중), 구조물과 살아 있는 나무/돌이 서버 판정 이동을 막음. `ctest` 324개(MySQL 11개 건너뜀) 모두 통과, 서버 변형 검증 30가지 중 29가지를 잡고 1가지는 동등 변형, Unreal 변형 검증 10가지를 모두 잡았다. **구역 분리**: 건물은 맵 중앙 정사각형(건설 구역, 사방 1,200 cm) 안에만, 자원 노드는 그 밖에만 놓이고 재생하며(`GAME_BUILD_OUT_OF_ZONE` 18), 재생 자리를 못 찾으면 1초마다 다시 찾는다(변형 검증 10가지를 모두 잡았다). **충돌 이후 부하 재측정에서는 450개까지 합격, 500개는 불합격이었으나 이것은 확정이 아니다**: 이후 실험에서 속도와 충돌은 CPU에 영향이 없었고, 같은 코드의 방당 CPU가 측정한 날의 PC 상태(배경 프로그램)에 따라 20% 넘게 달라졌으며, 이전 서버와의 같은 세션 비교는 배경 프로그램 때문에 무효가 되었다. 틱 시간의 약 98%는 전송(Deliver)이고 그 비용은 소켓 송신 시스템 호출이다(직렬화는 1% 미만, results/stage4/deliver-breakdown.md). 설계와 검증은 [docs/content-roadmap.md](docs/content-roadmap.md). PowerShell에서 `-key=1.2.3.4` 같은 인자는 따옴표로 감싸야 한다(점에서 쪼개짐). **입출력 스레드 분리 구현과 측정 완료(개선 후보 A)**: 소켓 읽기/쓰기를 `--io-threads N`개 스레드가 나눠 맡고 게임 로직은 스레드 1개 그대로다. 같은 세션 비교에서 분리 이전은 방 500개 불합격(누락 4.8~7.8%)이었고 `--io-threads 2`는 600개, `4`는 700개까지 3회 합격했다(틱 스레드 전송 시간 92% 감소, 서버 전체 CPU 약 25% 증가, 일부 실행은 PC 부하가 섞였을 수 있음). `ctest` 427개 통과(MySQL 11개 건너뜀). 기본값은 0 유지(results/stage4/io-threads.md, docs/stage4.md).
- **계정과 농장 저장(MySQL)은 진행 중이다(설계와 결과: [docs/persistence.md](docs/persistence.md)). P1(계정)과 P2(농장 저장)는 서버에 구현했고 인메모리 저장소로 시험했다(Windows)**: ID/비밀번호 가입과 로그인(비밀번호는 libsodium Argon2id 해시만 저장하고 해시 계산은 일꾼 스레드 `--hash-workers`), 내 기록 조회, 같은 계정의 중복 접속 거부, 연결당 로그인 실패 5회 제한, 게스트(로그인 없이 닉네임만)는 그대로 유지. **로그인한 사람의 방 만들기는 "내 농장 열기"**이고(계정당 농장 1개, 방 코드 고정), 집(건설 중이면 남은 틱까지)과 (농장, 계정)별 인벤토리와 계정별 누적 기록(채집량, 지은 집 수)이 MySQL에 저장된다. 저장은 쓰기 대기열(`FarmPersistence`)이 맡는다: 건설, 완공, 방을 나갈 때, 농장이 닫힐 때, 서버 종료 때는 바로, 채집은 10초마다 모아서, 저장이 실패해도 게임은 계속되고 다음 주기에 다시 저장한다(절대값 덮어쓰기라 멱등). 농장 방에는 로그인한 사람만, 임시 방에는 게스트만 들어간다. 메시지 ID 120~125(`proto/account.proto`), 결과 코드 19/20, 마이그레이션 `002`, `003`, `--storage mysql`이면 채팅, 계정, 농장이 모두 MySQL에 저장된다. 실제 MySQL 시험 `account_mysql_test`는 비밀번호를 환경변수로 주고 **사용자가** 실행해야 한다(만든 환경에서는 실행하지 못했다). **비밀번호가 평문으로 전송된다(TLS 없음).** **P3(Unreal 로그인 화면)은 C++와 커맨들릿 시험(5모드 111건, 기존 모드 회귀 통과, 변형 검증 21가지)과 위젯 에셋 추가(MCP)까지 끝났고, PIE 확인은 진행 중이다**(docs/unreal-client.md "계정과 로그인 (Unreal)"). PIE에서 "농장을 다시 열면 목재/석재가 0으로 보임"을 발견해 고쳤다(방 만들기/입장 응답에 인벤토리 `wood`, `stone` 추가). P4(저장 켬/끔 부하 측정)는 아직 안 했다.
- 확인된 명령만 아래 "명령" 절에 있다. 없는 명령을 지어내지 말고, 구현하면서 이 절에 추가한다.
- git 저장소다(`main` 브랜치). 커밋은 사용자가 요청할 때만 한다. `build/`, `vcpkg_installed/`, `CMakeUserPresets.json`은 gitignore 대상이다.

## 문서 지도 (먼저 읽을 것)

| 문서 | 역할 |
| --- | --- |
| [project.md](project.md) | 목표, 기술 선택, 1~4단계 로드맵과 완료 기준, 단계별 제외 범위 |
| [docs/protocol.md](docs/protocol.md) | 1단계 TCP 프레임 형식, Protobuf 메시지, 오류 처리 계약 |
| [docs/validation.md](docs/validation.md) | 기능 검증, 수용 인원 정의, 측정 절차, 실험 기록 템플릿 |
| [docs/stage1.md](docs/stage1.md) | 1단계 구성, 실행 방법, 검증 결과, 겪은 문제, 알려진 제한 |
| [docs/protocol-chat.md](docs/protocol-chat.md) | 2단계 채팅 메시지, 입력 규칙, 상태 규칙, 저장 정책 (초안) |
| [docs/stage2.md](docs/stage2.md) | 2단계 결정 기록, 작업 순서와 진행 상태, MySQL 준비 방법 |
| [docs/stage3.md](docs/stage3.md) | 3단계 측정 계획과 합격 기준(측정 전 확정), 진행 상태, 측정 도구에서 겪은 문제 |
| [docs/game-plan.md](docs/game-plan.md) | 게임 기획(스타듀밸리 풍 1~4인 협동 농장): 하루 루프와 밤 정산, 농사, 출하/상점, 제작/도구 강화, 기본값, 넣지 않는 것, 열린 결정 |
| [docs/stage4.md](docs/stage4.md) | 4단계 게임 서버: 범위, 프로토콜(ID 100~111), 기본값, 완료 기준, 검증 결과, 겪은 문제, 제한 |
| [docs/stage4-gather.md](docs/stage4-gather.md) | 4단계 확장 1(자원 채집) 설계: 범위, 프로토콜(ID 107/108/112), 기본값, 완료 기준, 서버·Unreal 설계, MCP 작업 목록 (구현 전) |
| [docs/content-roadmap.md](docs/content-roadmap.md) | 채집 이후 콘텐츠 로드맵(집 짓기 → 하루 → 농사 → 경제 → 저장)과 첫 조각(집 짓기, 이동 충돌 포함) 설계. 구현 전 계획 |
| [docs/content-day.md](docs/content-day.md) | 콘텐츠 조각 2(하루와 잠) 설계: 시계, 잠, 밤 정산 순서, 시작 집, 프로토콜(ID 116~119, 결과 코드 21~22), 시계 저장(마이그레이션 004), 완료 기준과 밤 정산 비용 측정 계획. 구현 전 |
| [docs/unreal-client.md](docs/unreal-client.md) | Unreal 연동 스파이크(5.4.4와 5.8.4에서 Ping 성공), 프로토콜 기준 바이트, Protobuf 연동 방법, 겪은 문제, 확인 방법 |
| [docs/persistence.md](docs/persistence.md) | 계정(ID/비밀번호)과 농장 저장(MySQL) 설계: 결정, 기본값, 스키마, 저장 규칙, 프로토콜(ID 120~), 검증과 성능 측정 계획, 작업 순서. 구현 전 |
| [docs/server-journey.md](docs/server-journey.md) | 서버 시행착오 기록(포트폴리오용): 측정 도구 포화, 결론 철회 두 번, 병목 분해, 입출력 스레드 분리, 버그와 시험의 허점 |
| [docs/unreal-client-structure.md](docs/unreal-client-structure.md) | Unreal 클라이언트 계층 구조, C++와 MCP의 책임 분담과 작업 순서, 검증 방법 (포트폴리오용 정리) |
| [README.md](README.md) | 진입점(포트폴리오용). 결과 한눈에 보기, 구조, 단계 요약, 문서 지도, 빠른 시작, 한계 |

`project.md`는 CLAUDE.md와 역할이 다르다(사람이 읽는 로드맵이며 README가 링크한다). 이름을 바꾸거나 합치지 않는다. 계획·계약이 바뀌면 CLAUDE.md가 아니라 해당 문서를 고치고, 여기에는 요약만 둔다.

## 기술 스택

- C++20, CMake, Windows와 Linux 모두 빌드 (OS별 소켓 코드 분기 없이 Boost.Asio 사용)
- Boost.Asio TCP + `io_context`, Protocol Buffers (`proto3`)
- MySQL은 2단계부터. 드라이버는 **Boost.MySQL**(`boost-mysql`, Asio 비동기)로 시작하고 Windows 빌드와 실제 접속(MySQL 26.10.0)을 확인했다. 결정 근거는 docs/stage2.md. DB 호출은 `io_context`를 막지 않아야 하며, 채팅 로직(`ChatRoom`)은 `ChatRepository` 인터페이스에만 의존한다(2-3에서 MySQL 구현만 추가). MySQL 계정의 인증 플러그인은 `caching_sha2_password`여야 한다(Boost.MySQL이 `sha256_password`를 지원하지 않음). "알 수 없는 인증 플러그인" 오류는 계정이 아예 없을 때도 나온다(MySQL이 없는 계정에 가짜 플러그인을 알려 줌). `mysql.user`를 조회해 계정 유무부터 확인한다.
- 의존성 버전과 설치 방식은 첫 구현 때 정해 고정한다. 임의로 올리지 않는다.
- 문서의 `Boost.io`는 **Boost.Asio**를 뜻한다.

## 1단계 프로토콜 요약

상세는 [docs/protocol.md](docs/protocol.md)가 기준이다.

- 프레임: `body_length: uint32` + `message_id: uint16` (모두 network byte order) + Protobuf 본문
- `body_length`는 message_id 2바이트와 본문의 합이다 (헤더 4바이트 제외). 허용 범위 **2~65,536**이며, 벗어나면 연결을 종료한다. 메모리 할당 전에 검사한다.
- 메시지 ID: `1 = PingRequest`, `2 = PingResponse`. 알 수 없는 ID, 파싱 실패는 기록 후 해당 연결만 종료한다. ID와 메시지의 대응표는 한곳에서 관리한다.
- 서버는 `request_id`와 `payload`를 그대로 되돌려준다. 연결별 요청 순서를 보존한다.
- 비동기 쓰기에 넘긴 버퍼는 쓰기 완료까지 살아 있어야 한다. 연결마다 송신 큐를 두고 앞선 쓰기가 끝난 뒤 다음 쓰기를 시작한다.
- 호환되지 않는 프로토콜 변경은 반드시 문서에 기록한다.

## 개발 원칙

- **작은 완성 단위:** 단계의 완료 기준을 만족한 뒤 다음 기능으로 간다. 단계 밖 기능(로그인, 채팅, MySQL, 암호화, 게임 로직, ObjectPool, 분산 서버)을 미리 구현하지 않는다.
- **프로토콜 먼저, 실패도 설계:** TCP는 메시지 경계가 없다. 분할 수신, 합쳐진 프레임, 비정상 길이, 연결 중단에서도 서버 프로세스는 살아 있어야 한다.
- **로그와 관측:** 연결/해제, 요청 ID, 처리 결과, 오류 유형을 남긴다. 본문 전체는 기본 로그에 남기지 않는다. 연결 수, 송수신 프레임 수, 오류 수, 송신 큐 크기를 볼 수 있게 한다.
- **측정 없이 주장하지 않는다:** 성능 목표 수치, 최대 접속자 수, "N% 향상"을 측정 전에 적지 않는다. 최적화(MemoryPool, 배치 송신, 스레드 수 등)는 병목 증거가 있을 때만, **한 번에 한 변수씩** 비교한다. 비슷하면 단순한 방식을 유지한다.
- **로드맵은 바뀔 수 있다.** 단계에 들어가기 전에 목표와 완료 기준을 재검토한다.

## 로드맵 요약

1. 패킷 송수신 (Ping 왕복, 프레이밍, 오류 처리) ← Windows 기준 완료, Linux 확인 보류
2. 채팅 + MySQL (방 단위 전파, 기록 저장·조회, DB 장애 시 동작) ← Windows 기준 완료
3. 수용 규모와 병목 검증 (Idle/Active 워크로드 구분, p50/p95/p99, Windows·Linux 각각 측정) ← 진행 중 (기준 측정 완료)
4. 게임 (서버가 상태와 판정을 소유. 사용자가 정한 기획은 1~4인 협동 농장 게임, 방 코드 입장, Unreal 클라이언트: docs/game-plan.md) ← 서버 최소 구현 완료(Windows), Unreal 연동은 다음 (docs/stage4.md)

## 검증·기록 규칙

- 1단계 검증 입력은 정상 1건, 분할 전송, 한 번에 여러 프레임, 길이 0~1/65,536 초과, 알 수 없는 ID, 잘못된 Protobuf, 중간 종료다 (표는 protocol.md).
- 성능 결과는 기능 검증을 통과한 빌드에서만 채택한다. 부하 실험에서는 상세 패킷 로그를 끄고 계측 설정을 함께 기록한다.
- 실험마다 `results/YYYY-MM-DD-주제.md`와 원본 결과 파일을 남긴다. 템플릿은 validation.md 5절. 합격 기준은 **측정 전에** 쓴다.
- Windows와 Linux 결과는 따로 내고 합치지 않는다. 오류가 난 반복도 숨기지 않는다.
- 각 단계가 끝나면 실행 방법, 검증 결과, 알려진 제한을 문서로 남긴다.
- 문서(md)에는 날짜를 쓰지 않는다. 이력은 git 커밋으로 남긴다.

## 명령

### 의존성 고정

vcpkg manifest 모드. [vcpkg.json](vcpkg.json)의 `builtin-baseline`(vcpkg 커밋)이 Boost와 Protobuf 버전을 고정한다. 현재 확인된 버전은 Boost 1.92.0, Protobuf 6.33.4다. baseline을 바꾸면 버전이 바뀌므로 결과 기록에 함께 남긴다.

### Windows (확인됨)

- 도구: Visual Studio 2022 (MSVC v143), vcpkg `C:\dev\vcpkg`, 환경변수 `VCPKG_ROOT=C:\dev\vcpkg`. CMake는 PATH에 없고 VS 2022에 포함된 것을 쓴다 (`...\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`).
- 프리셋: 공용 `windows-x64`는 [CMakePresets.json](CMakePresets.json), 이 PC 전용 `windows-local`은 gitignore된 `CMakeUserPresets.json`에 있다. 전용 프리셋은 두 가지를 해결한다.
  - 프로젝트 경로에 한글(`문서`)이 있어 vcpkg 설치 폴더를 `C:/dev/vcpkg_installed/game-server-lab`으로 뺐다 (안 그러면 `utf8_validity.lib` 링크 실패).
  - vcpkg와 프로젝트가 같은 MSVC 14.44를 쓰게 고정했다 (안 그러면 `__std_find_first_of_trivial_pos_1` 링크 오류).

```bash
cmake --preset windows-local          # 첫 실행은 Boost/Protobuf 소스 빌드로 약 9분
cmake --build --preset local-debug    # 또는 local-release
build/windows-local/Debug/hello_asio.exe
```

### 테스트 (GoogleTest, Windows 확인됨)

```bash
cmake --build --preset local-debug
build/windows-local/Debug/frame_codec_test.exe          # 코덱 단위 테스트 20개
build/windows-local/Debug/server_integration_test.exe   # 서버(Ping) 통합 테스트 32개 (약 2초)
build/windows-local/Debug/chat_rules_test.exe           # 채팅 입력 규칙 13개
build/windows-local/Debug/chat_room_test.exe            # 채팅 방 로직 23개 (소켓 없이 가짜 연결 사용)
build/windows-local/Debug/chat_integration_test.exe     # 채팅 통합 20개 (실제 TCP)
```

`ctest --test-dir build/windows-local -C Debug`로 전체 593개가 한 번에 실행된다(통합 테스트의 `_io4` 반복 포함)(MySQL 대상 35개는 비밀번호가 없으면 건너뜀) (gtest_discover_tests로 케이스별 등록). 특정 케이스만: `--gtest_filter=ServerIntegration.StopClosesAllOpenConnections`. 통합 테스트는 테스트 프로세스 안에서 진짜 `Server`를 포트 0(자동 배정)으로 띄우므로 서버를 따로 실행할 필요가 없다.

### 수동 확인 (서버 + ping_client)

```bash
build/windows-local/Debug/game_server.exe --port 7777                         # 터미널 1
build/windows-local/Debug/ping_client.exe --port 7777 --count 5 --payload-size 32   # 터미널 2
```

`ping_client` 옵션: `--host`, `--port`, `--count`, `--payload-size`(최대 65000), `--timeout-ms`. 종료 코드 0=전부 성공, 1=실패, 2=인자 오류. 요청 하나씩 왕복하는 단순 도구이며 부하 생성기가 아니다(부하 클라이언트는 3단계).

### MySQL 연결 확인 (Windows 확인됨)

```bash
$env:GAME_DB_PASSWORD = '비밀번호'
build/windows-local/Debug/mysql_check.exe --user game --db game_chat
```

옵션: `--host`, `--port`, `--user`, `--db`. MySQL 서버가 없으면 "연결을 거부"로 실패(종료 코드 1)하는 것이 정상이다. 서버 설치와 계정 만들기는 docs/stage2.md. 이 PC에는 MySQL 26.10.0이 설치되어 있다(Docker는 없음).

### MySQL 저장소 (Windows 확인됨)

비밀번호는 명령줄로 받지 않는다. 환경변수 `GAME_DB_PASSWORD` 또는 `GAME_DB_PASSWORD_FILE`(비밀번호가 든 파일의 경로)로만 전달하고, 로그에 출력하지 않는다. 비밀번호 파일은 저장소 밖에 둔다.

```bash
build/windows-local/Debug/db_migrate.exe                                  # 프로젝트 폴더에서: db/migrations 적용
build/windows-local/Debug/game_server.exe --port 7777 --storage mysql     # 기록을 MySQL에 저장
build/windows-local/Debug/chat_mysql_test.exe                             # MySQL 대상 테스트 (비밀번호 없으면 건너뜀)
```

접속 정보 기본값은 127.0.0.1:3306, 계정 `game`, 데이터베이스 `game_chat`이며 서버는 `--db-host/--db-port/--db-user/--db-name`, 테스트는 `GAME_DB_HOST/PORT/USER/NAME`으로 바꾼다. 테스트는 `chat_messages_test` 테이블만 쓴다.

#### 계정 MySQL 시험 (사용자가 실행)

```bash
$env:GAME_DB_PASSWORD = '비밀번호'                                    # 비밀번호를 채팅 창에 쓰지 않는다
build/windows-local/Debug/db_migrate.exe                              # 운영 테이블 만들기(002 계정, 003 농장 적용)
build/windows-local/Debug/account_mysql_test.exe                      # 시험 전용 테이블(accounts_test, account_stats_test, farms_test, farm_structures_test, farm_players_test)만 쓴다
```

`game_server.exe --port 7777 --storage mysql`이면 채팅 기록, 계정, 농장이 모두 MySQL에 저장된다(서버를 다시 켜도 같은 ID로 로그인되고 농장의 집, 인벤토리, 누적 기록이 복원된다). `db_migrate`를 먼저 실행해야 한다.

### 대화형 채팅 체험 (chat_client)

```bash
build/windows-local/Debug/game_server.exe --port 7777                         # 터미널 1
build/windows-local/Debug/chat_client.exe --port 7777 --nick alice            # 터미널 2
build/windows-local/Debug/chat_client.exe --port 7777 --nick bob              # 터미널 3
```

한 줄을 입력하면 채팅 메시지로 전송된다. 명령: `/join <닉네임>`, `/leave`, `/history [개수]`, `/ping`, `/wait <밀리초>`(스크립트용), `/quit`, `/help`. 같은 닉네임으로 입장하거나 입장 전에 채팅하면 거부 메시지가 나오는 것도 확인할 수 있다. `--nick`을 주면 접속 직후 자동 입장한다.

### 게임 체험 (4단계, game_client)

```bash
build/windows-local/Release/game_server.exe --port 7777 --log-level info                  # 터미널 1
build/windows-local/Release/game_client.exe --port 7777 --create --nick alice             # 터미널 2 (방 코드가 출력됨)
build/windows-local/Release/game_client.exe --port 7777 --join <방코드> --nick bob        # 터미널 3
```

명령: `/create <닉네임>`, `/join <방코드> <닉네임>`, `/leave`, `/move <dx> <dy>`(길이 1 이하 방향), `/stop`, `/snap <N>`(N틱마다 위치 출력, 0이면 끔), `/nodes`(자원 노드 목록과 내 위치에서의 거리), `/gather [번호]`(노드를 한 번 침, 번호를 생략하면 가장 가까운 살아 있는 노드. 서버 허용 거리 200 cm 안으로 `/move`로 먼저 가야 한다), `/ping`, `/wait <밀리초>`, `/quit`, `/help`. 위치는 서버가 계산해 20Hz 스냅샷으로 보내며 클라이언트는 방향만 보낸다. 서버 로그는 `info`로 두면 방 생성/입장/퇴장/삭제만 남는다. `/build <x> <y>`(그 자리에 집을 지음, 내 위치 500 cm 이내), `/structures`(건물 목록)도 있다. 계정: `/register <ID> <비밀번호>`, `/login <ID> <비밀번호>`, `/profile`(내 누적 기록). 로그인하면 방에서 ID가 닉네임이 된다(비밀번호가 터미널에 보이고 평문으로 전송되는 개발용 도구다). 단위·통합 테스트는 `play_world_test`, `play_resources_test`, `play_structures_test`, `play_collision_test`, `play_room_test`, `game_service_test`, `game_integration_test`다.

### 게임 부하 측정 (4단계, game_load_client)

```bash
build/windows-local/Release/game_server.exe --port 7777 --log-level info --stats-interval 1                   # 터미널 1
build/windows-local/Release/game_load_client.exe --port 7777 --rooms 100 --server-pid <서버PID> --label 이름 --csv results/stage4/raw/파일.csv   # 터미널 2
```

옵션: `--rooms`(방 수, 방마다 4명), `--input-rate`(플레이어당 초당 입력, 기본 10), `--warmup`, `--duration`, `--threads`(기본 4), `--server-pid`, `--csv`. 종료 코드 0=합격, 1=불합격, 3=생성기 포화. 서버의 틱 건너뜀은 서버 로그의 통계 줄(`건너뜀=`)을 측정 구간 시각(출력의 "측정 구간")으로 잘라 따로 본다. 합격 기준은 docs/stage4.md, 결과는 results/stage4/.

### 부하 측정 (3단계, Windows)

```bash
build/windows-local/Release/game_server.exe --port 7777 --log-level info --stats-interval 0   # 터미널 1 (Release)
build/windows-local/Release/load_client.exe --port 7777 --connections 1000 --mode active --server-pid <서버PID> --label 이름 --csv results/stage3/raw/파일.csv   # 터미널 2
```

옵션: `--mode idle|active`, `--rate`(연결당 초당 건수), `--payload`, `--warmup`, `--duration`, `--threads`(생성기 스레드 수, 연결 5,000 이상은 4 권장), `--server-pid`(서버 CPU/메모리 측정), `--csv`. 종료 코드 0=합격, 1=불합격, 3=생성기 포화로 판정 불가. 합격 기준과 절차는 docs/stage3.md, 결과는 results/stage3/. 측정할 때는 **Release 빌드**로, 다른 무거운 작업을 하지 않는 상태에서 한다. 서버는 `--tcp-nodelay on|off`도 받는다.

### 서버 실행 (Windows, 확인됨)

```bash
build/windows-local/Debug/game_server.exe --port 7777
```

옵션: `--bind`(기본 127.0.0.1, VM/외부 접속은 0.0.0.0), `--io-threads`(소켓 입출력 스레드 수, 기본 0), `--hash-workers`(비밀번호 해시 일꾼 스레드 수, 기본 2), `--port`, `--log-level`(기본 debug, 부하 실험은 info 이상), `--stats-interval`(초, 기본 10), `--stats-format line|block`(기본 line. block은 통계를 묶음마다 줄을 바꿔 보여 주는 사람용 모양이고, 부하 측정은 통계 줄을 시각으로 잘라 읽으므로 line으로 한다), `--max-send-queue-bytes`(기본 4 MiB), `--help`. 종료는 Ctrl+C(열린 연결을 모두 닫고 최종 통계를 출력).

### 코드 구조

- `src/protocol/frame.h/.cpp`: 프레임 인코더와 `FrameDecoder` (소켓과 무관한 바이트 처리, `frame_codec` 라이브러리)
- `src/protocol/message_ids.h`: 메시지 ID 대응표 (한곳에서 관리)
- `src/server/session.h/.cpp`: 연결 1개 담당(논리 구역과 strand 입출력 구역으로 나뉜다. 머리말 참고). 읽기 → 디코더 → Ping 처리 또는 채팅 요청 파싱 후 `ChatRoom`에 위임 → 송신 큐 → 쓰기, 종료 처리 (`server_core` 라이브러리). `chat::ChatClient`를 구현해 방이 이 연결에 메시지를 보낼 수 있게 한다
- `src/chat/chat_rules.h/.cpp`: 닉네임/메시지 검증 규칙 (순수 함수, `chat_core` 라이브러리)
- `src/chat/chat_repository.h`: 저장소 인터페이스(비동기, 핸들러는 항상 `io_context` 스레드에서 호출). `in_memory_chat_repository.*`는 메모리 구현이며 테스트용 장애·지연 주입을 지원
- `src/chat/mysql_chat_repository.h/.cpp`: MySQL 저장소(`chat_mysql` 라이브러리). 연결 1개 + 작업 큐 직렬화, 모든 DB 호출에 제한 시간, 오류 시 연결 폐기 후 재연결, 쓰기는 재시도 없음, 준비된 문장 바인딩
- `db/migrations/*.sql`: 번호 순서의 스키마 파일. 적용한 파일은 고치지 않고 새 번호를 추가한다. `src/tools/db_migrate.cpp`가 적용
- `src/common/db_credentials.h/.cpp`: DB 비밀번호 읽기
- `src/chat/chat_room.h/.cpp`: 채팅 방. 접속자, 입장/퇴장, 방 단위 직렬 저장 대기열, 저장 제한 시간, 전파, 기록 조회. 소켓·Session을 모르고 `ChatClient`/`ChatRepository` 인터페이스만 안다
- `src/chat/chat_client.h`: 방이 보는 연결 인터페이스
- `src/server/server.h/.cpp`: 리스닝 소켓, accept 반복, 세션 목록, 종료, 주기 통계 출력
- `src/server/stats.h`: 서버 전체 카운터(연결 수, 프레임 수, 오류 이유별 횟수, 최대 송신 큐)
- `src/server/stats_format.h/.cpp`: 통계 한 줄을 묶음별 여러 줄로 바꾸는 순수 함수(`--stats-format block`). 모양이 예상과 다르면 원문을 그대로 돌려준다. 시험은 `stats_format_test`
- `src/server/log.h`: 레벨별 스레드 안전 로그
- `src/server/main.cpp`: 실행 인자 파싱, `io_context` 실행, 종료 신호 처리 (`game_server`)
- `src/client/blocking_client.h/.cpp`: 모든 대기에 타임아웃이 있는 동기식 클라이언트 (`client_core`). 테스트와 `ping_client`가 공유
- `src/client/ping_client.cpp`: 수동 확인용 실행 파일
- `src/client/chat_client.cpp`: 대화형 채팅 클라이언트(입력 스레드 + `io_context`)
- `src/tools/load_client.cpp`: 3단계 부하 생성기(`--threads N` 다중 스레드, 기본 1, 열린 루프, 요청별 응답 대조, 서버 프로세스 CPU/메모리 샘플링, 생성기 포화 감지, 합격 판정)
- `src/tools/mysql_check.cpp`: MySQL 접속·문자 집합 진단 도구 (`mysql_check`). 비밀번호는 환경변수 `GAME_DB_PASSWORD`로만 받는다
- `src/common/error_text.h/.cpp`: 윈도우 ANSI 오류 메시지를 UTF-8로 변환 (`common_util`). 오류 메시지를 로그/출력에 넣을 때는 `ec.message()` 대신 `common::ErrorText(ec)`를 쓴다
- `tests/frame_codec_test.cpp`: 코덱 단위 테스트
- `tests/server_integration_test.cpp`: 서버(Ping) 통합 테스트. 결과는 서버 `Stats` 카운터와 클라이언트가 받은 응답으로 확인
- `tests/chat_rules_test.cpp`, `chat_room_test.cpp`, `chat_integration_test.cpp`: 채팅 규칙/방 로직/통합 테스트. `tests/test_support.h`에 서버를 띄우는 `TestServer`(저장소 장애·지연 주입 가능, 다른 저장소도 주입 가능)와 `WaitUntil`이, `tests/chat_test_client.h`에 클라이언트 쪽 도우미(`Connect`, `JoinOk`, `SendChat` 등)가 있다. `tests/chat_mysql_test.cpp`는 MySQL 대상 테스트
- `src/account/account_rules.h/.cpp`: ID/비밀번호 규칙(ID는 소문자로 정규화, 순수 함수, `account_core` 라이브러리)
- `src/account/password_hasher.h/.cpp`: 비밀번호 해시 인터페이스와 libsodium Argon2id 구현(`crypto_pwhash_str`). **해시는 비싸서 로직/입출력 스레드에서 직접 부르지 않는다**
- `src/account/account_repository.h`, `in_memory_account_repository.*`, `mysql_account_repository.*`(`account_mysql` 라이브러리): 계정 저장소(비동기, `ChatRepository`와 같은 규칙). MySQL 구현은 테이블 이름을 옵션으로 받아 시험이 운영 테이블을 건드리지 않는다
- `src/account/account_service.h/.cpp`: 가입/로그인/내 기록, 연결별 로그인 상태, 중복 접속 거부, 로그인 실패 제한, 해시 일꾼 스레드 풀. 로직 스레드에서만 부른다(해시 계산만 일꾼에서 돈다). `GameService`는 `SetAccountLookup`으로 로그인한 연결의 닉네임을 얻는다
- `src/farm/farm_repository.h`, `in_memory_farm_repository.*`, `mysql_farm_repository.*`(`farm_core`, `farm_mysql` 라이브러리): 농장 저장소(비동기, 같은 저장소의 요청은 순서대로). `SaveFarm` 한 번이 하나의 트랜잭션(구조물/인벤토리/누적 기록의 절대값 upsert). 인메모리 구현은 장애/지연 주입을 지원
- `src/play/farm_persistence.h/.cpp`: 농장 변경의 쓰기 대기열. 사건이 알리는 절대값을 모아 즉시 또는 주기(`PlayConfig::farm_flush_interval_ticks`)로 저장하고, 실패한 값은 다음 주기에 다시 저장한다(저장 중이면 겹쳐 보내지 않고 끝난 뒤 이어서). `GameService`가 소유한다
- `src/play/play_config.h`: 게임 규칙 기본값(맵, 속도, 인원, 틱, 방 수 상한). 한 곳에서만 바꾼다
- `src/play/world.h/.cpp`: 한 방의 위치와 이동 계산(순수 계산, 서버 판정). 방향 길이는 `double` `hypot`으로 구해 1로 제한한다(`play_core` 라이브러리)
- `src/play/room.h/.cpp`: 방 한 개의 인원, 방장, 닉네임, 시작 칸(slot). 소켓과 시간을 모른다
- `src/play/resources.h/.cpp`: 한 방의 자원 노드(고정 시드 배치, 체력, 고갈, 틱 기준 재생, 바뀐 노드 모으기). 소켓과 시계를 모르는 순수 계산이고 시간은 틱 번호로만 안다. `Room::Gather`가 거리·쿨다운·인벤토리를 합쳐 판정하고, 판정 순서는 방에 없음 → 노드 없음 → 고갈 → 너무 멂 → 쿨다운이다. 규칙은 docs/stage4-gather.md
- `src/play/structures.h/.cpp`, `src/play/geometry.h`: 한 방의 구조물(건설 진행, 틱 기준 완공, 바뀐 구조물 모으기)과 겹침 판정 함수(닿기만 하는 것은 겹침이 아니다, double 계산). `Room::Build`가 거리·겹침(구조물, 살아 있는 노드, **모든 플레이어**)·비용을 판정하며 순서는 방에 없음 → 종류 → 상한 → 너무 멂 → 겹침/맵 밖 → 자원 부족이다. `World`는 구조물과 노드를 읽기 전용 포인터로 받아(`AttachObstacles`, 그래서 `Room`은 복사/이동 금지) 이동을 축별로 나눠 막고 벽을 따라 미끄러지게 한다. 규칙은 docs/content-roadmap.md
- `src/play/game_service.h/.cpp`: 여러 방 관리, 6자리 숫자 코드 발급(충돌 재발급, 생성기 교체 가능), 입장/퇴장/방장 변경/알림, 고정 간격 틱(밀리면 최대 3틱 따라잡고 나머지는 건너뜀). 전송 중 연결이 닫혀도 안전하도록 틱은 "계산 -> 수신자 확보 -> 전송" 순서다. 연결은 기존 `chat::ChatClient` 인터페이스를 그대로 쓴다
- `src/client/game_client.cpp`: 대화형 게임 클라이언트(수동 실험용)
- `src/tools/game_load_client.cpp`: 4단계 게임 부하 생성기(방 R개 x 4명, 열린 루프 입력, 스냅샷 도착 간격/입력 반영 지연/누락/정합성 검사, 생성기 포화 감지)
- `proto/ping.proto`, `proto/chat.proto`, `proto/game.proto` → `game_proto` 라이브러리 (빌드 시 코드 생성)

### 서버 구현에서 정해 둔 것

- **논리 스레드는 1개다.** Server, ChatRoom, GameService, Session의 논리 쪽(디코더, Close)은 스레드 안전하지 않고, `Server`에 넘긴 `io_context`(스레드 1개)에서만 실행된다는 전제다. **소켓 입출력만** `--io-threads N`(기본 0)일 때 별도 풀의 스레드 N개가 연결별 strand로 나눠 맡는다. 두 구역은 `dispatch`로만 넘나들고(논리->입출력: 송신, 읽기 재개, 소켓 닫기. 입출력->논리: 읽은 바이트 처리, 오류로 인한 Close), 세션은 입출력 풀을 공유 소유한다(소켓이 속한 `io_context`보다 먼저 소멸해야 해서, 안 그러면 서버가 먼저 소멸할 때 프로세스가 가끔 죽는다). 로직 스레드를 늘리려면 이 전제를 다시 설계해야 한다. 측정 결과: `--io-threads 2`는 방 600개, `4`는 700개까지 합격(분리 이전은 500개에서 불합격), `1`은 입력 반영 지연이 쌓여 쓰면 안 되고 `0`은 이전보다 약간 느리다(results/stage4/io-threads.md). 통합 테스트는 `*_io4` ctest 항목이 같은 시나리오를 입출력 스레드 4개로 반복한다(환경변수 `GAME_TEST_IO_THREADS`).
- 소켓은 `TCP_NODELAY`를 켠다(즉시 송신). 배치 송신은 3단계 비교 후보다.
- 연결이 닫혀도 `Session::send_queue_`는 비우지 않는다. 진행 중인 `async_write`가 맨 앞 버퍼를 쓰는 동안 해제하면 안 되기 때문이다(IOCP는 취소 완료 전까지 버퍼를 쓸 수 있다).
- 게임 틱(20Hz)은 `GameService`가 절대 시각 격자로 예약한다. 윈도우의 기본 타이머 해상도(약 15.6 ms)에서는 틱이 늦게 깨어나 몰아서 처리될 수 있어, 서버 `main`이 3단계 부하 생성기와 같은 방식으로 해상도를 1 ms로 올린다.
- 사용자가 `game_server.exe`를 켜 둔 채면 같은 구성의 빌드가 `LNK1104`로 막힌다. 프로세스를 끄지 말고 실행 중인 파일의 이름만 바꾼 뒤(`game_server.old-running-2.exe` 등) 빌드한다.
- 윈도우에서는 `SO_REUSEADDR`를 켜지 않는다(중복 바인드 허용 의미라 위험). 리눅스에서만 켠다.

### Linux (보류)

`linux-debug`, `linux-release` 프리셋(Ninja)이 정의돼 있으나 아직 실행해 보지 않았다. 확인하면 이 절을 갱신한다.

### 추가 예정

부하 클라이언트 실행 명령은 3단계에서 구현한 뒤 채운다.
