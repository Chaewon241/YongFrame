# CLAUDE.md

Cross-Platform Game Server Lab: Boost.Asio 기반 C++ 게임 서버를 단계별로 만들고, 각 단계의 정확성과 성능을 재현 가능한 기록으로 남기는 개인 프로젝트다. 문서와 대화는 한국어로 쓴다.

## 현재 상태

- 1단계(패킷 송수신)는 **Windows 기준으로 완료**했다. Linux 확인은 사용자가 보류했다. 다음은 2단계 계획 재검토. 빌드 환경(CMake + vcpkg)은 Windows에서 확인됐고, `src/hello_asio.cpp`는 툴체인 확인용이다.
- 구현됨: 프레임 코덱(단위 테스트 20개), Ping 서버(`game_server`), 동기식 테스트 클라이언트(`ping_client`, `BlockingClient`), 서버 통합 테스트 32개. 1단계 결과는 [docs/stage1.md](docs/stage1.md)에 정리됨. 보류: Linux 빌드 확인(VM), 확인 후 stage1.md 갱신.
- 확인된 명령만 아래 "명령" 절에 있다. 없는 명령을 지어내지 말고, 구현하면서 이 절에 추가한다.
- git 저장소다(`main` 브랜치). 커밋은 사용자가 요청할 때만 한다. `build/`, `vcpkg_installed/`, `CMakeUserPresets.json`은 gitignore 대상이다.

## 문서 지도 (먼저 읽을 것)

| 문서 | 역할 |
| --- | --- |
| [project.md](project.md) | 목표, 기술 선택, 1~4단계 로드맵과 완료 기준, 단계별 제외 범위 |
| [docs/protocol.md](docs/protocol.md) | 1단계 TCP 프레임 형식, Protobuf 메시지, 오류 처리 계약 |
| [docs/validation.md](docs/validation.md) | 기능 검증, 수용 인원 정의, 측정 절차, 실험 기록 템플릿 |
| [docs/stage1.md](docs/stage1.md) | 1단계 구성, 실행 방법, 검증 결과, 겪은 문제, 알려진 제한 |
| [README.md](README.md) | 진입점. 위 문서 링크와 바로 시작할 작업 |

`project.md`는 CLAUDE.md와 역할이 다르다(사람이 읽는 로드맵이며 README가 링크한다). 이름을 바꾸거나 합치지 않는다. 계획·계약이 바뀌면 CLAUDE.md가 아니라 해당 문서를 고치고, 여기에는 요약만 둔다.

## 기술 스택

- C++20, CMake, Windows와 Linux 모두 빌드 (OS별 소켓 코드 분기 없이 Boost.Asio 사용)
- Boost.Asio TCP + `io_context`, Protocol Buffers (`proto3`)
- MySQL은 **2단계부터**. 1단계에 DB를 넣지 않는다.
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
2. 채팅 + MySQL (방 단위 전파, 기록 저장·조회, DB 장애 시 동작)
3. 수용 규모와 병목 검증 (Idle/Active 워크로드 구분, p50/p95/p99, Windows·Linux 각각 측정)
4. 2인 게임 (서버가 상태와 판정을 소유. 우선 후보는 술래잡기/점령. 3~4인 확장은 이후 결정)

## 검증·기록 규칙

- 1단계 검증 입력은 정상 1건, 분할 전송, 한 번에 여러 프레임, 길이 0~1/65,536 초과, 알 수 없는 ID, 잘못된 Protobuf, 중간 종료다 (표는 protocol.md).
- 성능 결과는 기능 검증을 통과한 빌드에서만 채택한다. 부하 실험에서는 상세 패킷 로그를 끄고 계측 설정을 함께 기록한다.
- 실험마다 `results/YYYY-MM-DD-주제.md`와 원본 결과 파일을 남긴다. 템플릿은 validation.md 5절. 합격 기준은 **측정 전에** 쓴다.
- Windows와 Linux 결과는 따로 내고 합치지 않는다. 오류가 난 반복도 숨기지 않는다.
- 각 단계가 끝나면 실행 방법, 검증 결과, 알려진 제한을 문서로 남긴다.

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
build/windows-local/Debug/server_integration_test.exe   # 서버 통합 테스트 32개 (약 2초)
```

`ctest --test-dir build/windows-local -C Debug`로도 모두 실행된다 (gtest_discover_tests로 케이스별 등록). 특정 케이스만: `--gtest_filter=ServerIntegration.StopClosesAllOpenConnections`. 통합 테스트는 테스트 프로세스 안에서 진짜 `Server`를 포트 0(자동 배정)으로 띄우므로 서버를 따로 실행할 필요가 없다.

### 수동 확인 (서버 + ping_client)

```bash
build/windows-local/Debug/game_server.exe --port 7777                         # 터미널 1
build/windows-local/Debug/ping_client.exe --port 7777 --count 5 --payload-size 32   # 터미널 2
```

`ping_client` 옵션: `--host`, `--port`, `--count`, `--payload-size`(최대 65000), `--timeout-ms`. 종료 코드 0=전부 성공, 1=실패, 2=인자 오류. 요청 하나씩 왕복하는 단순 도구이며 부하 생성기가 아니다(부하 클라이언트는 3단계).

### 서버 실행 (Windows, 확인됨)

```bash
build/windows-local/Debug/game_server.exe --port 7777
```

옵션: `--bind`(기본 127.0.0.1, VM/외부 접속은 0.0.0.0), `--port`, `--log-level`(기본 debug, 부하 실험은 info 이상), `--stats-interval`(초, 기본 10), `--max-send-queue-bytes`(기본 4 MiB), `--help`. 종료는 Ctrl+C(열린 연결을 모두 닫고 최종 통계를 출력).

### 코드 구조

- `src/protocol/frame.h/.cpp`: 프레임 인코더와 `FrameDecoder` (소켓과 무관한 바이트 처리, `frame_codec` 라이브러리)
- `src/protocol/message_ids.h`: 메시지 ID 대응표 (한곳에서 관리)
- `src/server/session.h/.cpp`: 연결 1개 담당. 읽기 → 디코더 → Ping 처리 → 송신 큐 → 쓰기, 종료 처리 (`server_core` 라이브러리)
- `src/server/server.h/.cpp`: 리스닝 소켓, accept 반복, 세션 목록, 종료, 주기 통계 출력
- `src/server/stats.h`: 서버 전체 카운터(연결 수, 프레임 수, 오류 이유별 횟수, 최대 송신 큐)
- `src/server/log.h`: 레벨별 스레드 안전 로그
- `src/server/main.cpp`: 실행 인자 파싱, `io_context` 실행, 종료 신호 처리 (`game_server`)
- `src/client/blocking_client.h/.cpp`: 모든 대기에 타임아웃이 있는 동기식 클라이언트 (`client_core`). 테스트와 `ping_client`가 공유
- `src/client/ping_client.cpp`: 수동 확인용 실행 파일
- `src/common/error_text.h/.cpp`: 윈도우 ANSI 오류 메시지를 UTF-8로 변환 (`common_util`). 오류 메시지를 로그/출력에 넣을 때는 `ec.message()` 대신 `common::ErrorText(ec)`를 쓴다
- `tests/frame_codec_test.cpp`: 코덱 단위 테스트
- `tests/server_integration_test.cpp`: 서버 통합 테스트. 결과는 서버 `Stats` 카운터와 클라이언트가 받은 응답으로 확인
- `proto/ping.proto` → `game_proto` 라이브러리 (빌드 시 코드 생성)

### 서버 구현에서 정해 둔 것

- `io_context`는 스레드 1개로 돌린다. Session/Server는 스레드 안전하지 않고 단일 스레드 전제다. 스레드를 늘리는 실험(3단계)을 할 때는 소켓에 strand를 붙여 이 전제를 유지해야 한다.
- 소켓은 `TCP_NODELAY`를 켠다(즉시 송신). 배치 송신은 3단계 비교 후보다.
- 연결이 닫혀도 `Session::send_queue_`는 비우지 않는다. 진행 중인 `async_write`가 맨 앞 버퍼를 쓰는 동안 해제하면 안 되기 때문이다(IOCP는 취소 완료 전까지 버퍼를 쓸 수 있다).
- 윈도우에서는 `SO_REUSEADDR`를 켜지 않는다(중복 바인드 허용 의미라 위험). 리눅스에서만 켠다.

### Linux (보류)

`linux-debug`, `linux-release` 프리셋(Ninja)이 정의돼 있으나 아직 실행해 보지 않았다. 확인하면 이 절을 갱신한다.

### 추가 예정

부하 클라이언트 실행 명령은 3단계에서 구현한 뒤 채운다.
