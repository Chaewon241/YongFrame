# 1단계 결과: 패킷 송수신

> 상태: **Windows 기준 완료. Linux 확인은 보류**(사용자 결정으로 뒤로 미룸). 1단계 완료 기준 중 "Windows와 Linux 모두 통과"는 Linux 쪽이 아직 충족되지 않았다.
> 검증한 코드: 커밋 `495679f` (`main`)

테스트 클라이언트가 TCP로 Protobuf `PingRequest`를 보내면 서버가 같은 `request_id`와 `payload`의 `PingResponse`를 돌려준다. 이 단계의 성공은 "네트워크, 프레이밍, 직렬화 경로가 끝까지 작동한다"는 뜻이며, 성능은 측정하지 않았다(7절).

## 1. 완료 기준 점검

기준은 [project.md](../project.md#1단계-패킷-송수신)의 1단계 완료 기준이다.

| 완료 기준 | 상태 | 근거 |
| --- | --- | --- |
| Windows와 Linux의 깨끗한 빌드 환경에서 같은 테스트 시나리오 통과 | **부분 충족 (Linux 보류)** | Windows는 이 PC에서 빌드와 테스트 통과. 다만 저장소를 새로 clone해서 처음부터 빌드하는 절차는 확인하지 않았다. Linux는 아직 실행하지 않음 |
| 요청 1건, 연속 여러 건, 여러 요청이 한 번에 도착, 한 요청이 여러 조각으로 도착 | 충족 | 4.2절 |
| 응답의 요청 ID와 payload가 원본과 일치, 연결별 요청 순서 보존 | 충족 | 4.2절 |
| 길이 0~1/최대 초과, 알 수 없는 메시지 ID, 잘못된 Protobuf, 전송 중 연결 종료를 정의된 방식으로 처리하고 서버가 살아 있음 | 충족 | 4.2절 |
| 실행 명령, 검증 결과, 알려진 제한을 문서로 남김 | 충족 | 이 문서 |
| 성능 목표 수치나 최대 접속자 수를 주장하지 않음 | 준수 | 7절 |

## 2. 구성

| 위치 | 역할 |
| --- | --- |
| `proto/ping.proto` | `PingRequest` / `PingResponse` 정의. 빌드할 때 C++ 코드가 생성되어 서버와 클라이언트가 공유 |
| `src/protocol/frame.h/.cpp` | 프레임 인코더와 `FrameDecoder`. 소켓과 무관한 바이트 처리. 길이 필드 4바이트가 모이면 즉시 검증 |
| `src/protocol/message_ids.h` | 메시지 ID 대응표(한곳에서 관리) |
| `src/server/session.*` | 연결 1개 담당. 읽기, 디코더, Ping 처리, 송신 큐, 쓰기, 종료 |
| `src/server/server.*` | 리스닝 소켓, accept 반복, 세션 목록, 종료, 주기 통계 |
| `src/server/stats.h`, `log.h`, `main.cpp` | 통계 카운터, 로그, 실행 인자와 종료 신호 |
| `src/client/blocking_client.*` | 모든 대기에 타임아웃이 있는 동기식 클라이언트 |
| `src/client/ping_client.cpp` | 수동 확인용 실행 파일 |
| `src/common/error_text.*` | 윈도우 ANSI 오류 메시지를 UTF-8로 변환 |
| `tests/frame_codec_test.cpp` | 코덱 단위 테스트 20개 |
| `tests/server_integration_test.cpp` | 서버 통합 테스트 32개 |

프레임 형식과 오류 처리 규칙은 [protocol.md](protocol.md)가 기준이다.

## 3. 실행 방법 (Windows)

환경 준비와 빌드 명령은 [CLAUDE.md](../CLAUDE.md)의 "명령" 절에 있다. 프로젝트 폴더에서 다음을 실행한다.

```bash
cmake --preset windows-local
cmake --build --preset local-debug
```

서버와 클라이언트 (터미널 2개):

```bash
build\windows-local\Debug\game_server.exe --port 7777
build\windows-local\Debug\ping_client.exe --port 7777 --count 5 --payload-size 32
```

자동 테스트 (서버를 따로 띄울 필요 없음):

```bash
build\windows-local\Debug\frame_codec_test.exe
build\windows-local\Debug\server_integration_test.exe
```

`ctest --test-dir build/windows-local -C Debug`로 52개를 한 번에 돌릴 수도 있다. 서버 옵션은 `game_server.exe --help`, 클라이언트 옵션은 `ping_client.exe --help`로 확인한다.

## 4. 검증 결과

### 4.1 환경

| 항목 | 값 |
| --- | --- |
| OS | Windows 11 Home 10.0.26200 |
| CPU / RAM | AMD Ryzen 5 5600 (6코어 12스레드) / 16 GB |
| 컴파일러 | Visual Studio 2022, MSVC 14.44.35207 |
| CMake | 3.31.6 (VS 2022 번들) |
| 의존성 | Boost 1.92.0, Protobuf 6.33.4, GoogleTest 1.18.0 (vcpkg `x64-windows-static-md`) |
| vcpkg baseline | `e456309491fd875487bf02b8a0dca76c1b1b00cc` |
| 서버 설정 | `io_context` 스레드 1개, 바인드 127.0.0.1, 송신 큐 한도 기본 4 MiB (일부 테스트는 200 KB) |

모든 시험은 한 PC 안의 루프백(127.0.0.1)에서 했다. 서버와 클라이언트가 같은 머신이다.

### 4.2 기능 검증

[protocol.md의 검증 입력표](protocol.md#1단계-검증-입력)를 `tests/server_integration_test.cpp`로 자동화했다. 오류 시나리오는 (1) 서버가 연결을 닫는지, (2) 서버 통계에 맞는 이유가 기록되는지, (3) 그 뒤 새 연결로 Ping이 되는지(서버 생존)를 모두 확인한다.

| 입력 | 기대 결과 | 테스트 | 결과 |
| --- | --- | --- | --- |
| 정상 Ping 1건 | 동일한 ID·payload 응답 | `SinglePingEchoesIdAndPayload`, `EmptyPayloadIsEchoed` | 통과 |
| 같은 연결에서 연속 50건 | 모두 동일 응답 | `ConsecutiveRequestsOnOneConnection` | 통과 |
| 최대 길이 프레임 (body_length 65,536) | 정상 응답 | `MaxSizeFrameRoundTrips` | 통과 |
| 한 프레임을 쪼개 전송 | 정상 응답 1건 | `FrameSentOneByteAtATime`(1바이트씩), `FrameSplitInsideHeaderAndInsideBody`(헤더/본문 중간) | 통과 |
| 여러 프레임을 한 번에 전송 | 요청 순서대로 응답 | `ManyFramesInOneSendAreAnsweredInOrder`(10개), `ThreeFramesPlusPartialFourthThenTheRest` | 통과 |
| 길이 0, 1, 65,537, 1,000,000, 0x7FFFFFFF, 0xFFFFFFFF | 연결 종료, 서버 계속 실행 | `InvalidLengthTest` (6가지 값) | 통과 |
| 알 수 없는 ID 0, 3, 99, 65535 / 서버가 보내는 쪽인 2 | 연결 종료, 서버 계속 실행 | `UnknownMessageIdTest` (5가지 값) | 통과 |
| 잘못된 Protobuf 4종 (끝나지 않고 잘린 varint, 선언한 길이보다 짧은 데이터, 필드 번호 0, 너무 긴 varint) | 연결 종료, 서버 계속 실행 | `InvalidProtobufTest` (4가지) | 통과 |
| 헤더 중간 / 본문 중간에 연결 종료 | 연결 정리, `truncated_connections` 증가, 서버 계속 실행 | `ClosingInTheMiddleOfHeaderIsCleanedUp`, `...OfBody...` | 통과 |
| 프레임 경계에서 정상 종료 | 중간 종료로 세지 않음 | `ClosingBetweenFramesIsNotCountedAsTruncated` | 통과 |
| 동시 100개 연결 | 각자 정상 응답 | `ManyConcurrentConnections` | 통과 |
| 한 연결의 오류 | 다른 연결에 영향 없음 | `BadConnectionDoesNotAffectGoodConnection`, `IdleConnectionDoesNotBlockOthers` | 통과 |
| 응답을 읽지 않는 클라이언트 | 송신 큐 한도 초과로 연결 종료, 서버 계속 실행 | `ClientThatNeverReadsIsDisconnectedWhenSendQueueOverflows` | 통과 |
| 서버 종료 | 열린 연결을 모두 닫음, 새 연결 거부 | `StopClosesAllOpenConnections`, `StoppedServerRefusesNewConnections` | 통과 |

코덱 자체는 `frame_codec_test`의 20개로 따로 검증했다(빅엔디언 바이트 비교, 경계값 2와 65,536, 고정 시드 난수로 200개 프레임을 1~50바이트로 쪼개 넣어도 동일 복원, 오류 후 입력 무시 등).

전체 결과: ctest **52개 중 52개 통과**, Debug와 Release 모두.

### 4.3 반복 실행

`server_integration_test`를 30번 반복했다.

| 구성 | 30회 중 통과 | 1회 소요 |
| --- | --- | --- |
| Debug | 30 | 1.7~1.8초 |
| Release | 30 | 1.6~1.7초 |

간헐 실패는 관측되지 않았다. 다만 30회로 간헐 실패가 없음을 증명할 수는 없다.

### 4.4 테스트가 버그를 잡는지 확인

테스트가 항상 통과하도록 만들어진 것이 아닌지 보기 위해, 코드를 일부러 망가뜨리고 테스트가 실패하는지 확인했다(원복 후 소스는 해시로 동일함을 확인).

| 망가뜨린 곳 | 실패한 테스트 수 |
| --- | --- |
| (코덱) 최대 길이를 65,535로 변경 | 3 (한계값을 상수가 아닌 숫자로 고정한 뒤) |
| (코덱) 최소 길이 검사 `<`를 `<=`로 | 2 |
| (코덱) 길이 필드의 바이트 순서 뒤집기 | 9 |
| (코덱) 최대 길이 초과 검사 삭제 | 2 |
| (서버) 응답 `request_id`에 1 더하기 | 30 |
| (서버) 송신 큐 한도 검사 무력화 | 1 |
| (서버) 프레임 중간 종료를 정상 종료로 처리 | 2 |
| (서버) `Stop()`이 연결을 닫지 않음 | 1 |
| (서버) 알 수 없는 ID도 Ping으로 처리 | 5 |
| (서버) 쓰기 완료 후 다음 프레임을 보내지 않음 | 2 |

이 검증은 사람이 한 번 수행한 것이고 자동화되어 있지 않다(6절).

### 4.5 수동으로만 확인한 항목

- **Ctrl+C 종료:** 열린 연결 3개를 닫고 최종 통계를 출력한 뒤 종료 코드 0으로 끝나는 것을 확인했다. `main.cpp`의 신호 처리는 자동 테스트가 없다(자동 테스트는 `Server::Stop()`까지).
- **포트 충돌:** 이미 쓰는 포트로 서버를 띄우면 `bind` 오류와 함께 시작에 실패하고 종료 코드 1이다. 윈도우에서 `SO_REUSEADDR`를 켜지 않은 결정이 맞게 동작함을 확인했다.

## 5. 겪은 문제와 수정

| 문제 | 원인 | 수정 |
| --- | --- | --- |
| vcpkg의 protobuf 빌드가 링크 단계에서 실패 (`utf8_validity.lib`를 열 수 없음) | 프로젝트 경로에 한글(`문서`)이 있고 vcpkg 설치 폴더가 그 안에 있었음 | 설치 폴더를 `C:/dev/vcpkg_installed/...`로 이동 (이 PC 전용 프리셋) |
| `hello_asio` 링크 오류 `__std_find_first_of_trivial_pos_1` | vcpkg는 VS 2026(MSVC 14.51), 프로젝트는 VS 2022의 구버전(14.42)으로 빌드해 STL 버전이 어긋남 | vcpkg와 프로젝트 모두 VS 2022의 MSVC 14.44로 고정 |
| VS에서 실행이 안 됨 | CMake가 만든 솔루션의 시작 프로젝트가 실행 파일이 아닌 `ALL_BUILD` | `VS_STARTUP_PROJECT` 지정 |
| 한계값 변경을 디코더 테스트가 못 잡음 | 테스트가 헤더의 상수를 그대로 가져다 써서 상수가 바뀌면 기대값도 같이 바뀜 | 테스트에서 한계값을 숫자로 고정하고 상수 자체를 검사하는 테스트 추가 |
| 서버가 종료되지 않으면 통합 테스트가 영원히 멈춤 | 서버 스레드 `join()`에 제한 시간이 없었음 | 5초 뒤 강제 중단하고 실패로 처리 |
| 윈도우 오류 메시지가 깨져 보임 | 시스템 오류 메시지는 ANSI 코드페이지(CP949), 출력은 UTF-8 | 변환 함수 `common::ErrorText` |

## 6. 알려진 제한

**검증하지 못한 것**
- Linux 빌드와 테스트. 특히 느린 수신자 테스트는 OS 소켓 버퍼 크기에 영향을 받으므로 Linux에서 동작이 달라질 수 있다.
- 서버와 클라이언트가 다른 머신인 환경. 모든 시험이 루프백이다.
- IPv6, `--bind 0.0.0.0` 외부 접속, 방화벽 동작.
- accept 오류 재시도 경로(파일/소켓 수 고갈 등). 코드는 있으나 오류를 일으켜 보지 않았다.
- 공용 `windows-x64` 프리셋만으로 다른 PC에서 빌드되는지. 이 PC에서는 전용 프리셋(`windows-local`)이 필요했다.

**설계상 제한**
- `io_context`는 스레드 1개다. `Session`과 `Server`는 스레드 안전하지 않다. 스레드를 늘릴 때는 소켓에 strand를 붙여야 한다(3단계).
- 서버는 오류를 에러 응답으로 알리지 않고 연결을 끊는다(에러 응답 프로토콜은 추후).
- 인증, 암호화, 요청 ID 중복 제거는 없다(1단계 범위 밖).
- 송신 큐 한도(기본 4 MiB), 읽기 버퍼(8 KiB), `TCP_NODELAY` 켬은 **측정 없이 정한 기본값**이다. 이후 단계에서 비교 실험으로 검증한다.
- `FrameDecoder::Append`는 넘긴 크기만큼 내부 버퍼를 키운다. 서버는 8 KiB 조각만 넣으므로 문제가 없지만, 큰 덩어리를 한 번에 넣는 호출자에게는 보호 장치가 없다.
- 로그 기본 레벨이 `debug`(요청 단위 로그 포함)이고 한 줄마다 flush한다. 부하 측정 때는 `--log-level info` 이상으로 올려야 한다.
- 로그에 payload 본문은 남기지 않고 요청 ID와 크기만 남긴다.

**검증 방법의 제한**
- 4.4의 변형 검증은 수동 일회성이다. 자동화하거나 CI에 넣지 않았다.
- 포트가 이미 점유된 경우의 거동은 수동 시험 1건뿐이다.
- 4.3의 반복 횟수는 30회다.

## 7. 성능에 대해

이 단계에서는 성능 수치를 주장하지 않는다. `ping_client`를 실행하면 왕복 시간이 출력되고(예: 루프백에서 평균 0.1ms 안팎) 개발 중에 눈으로 본 값은 있지만, **Debug 빌드, 단일 연결, 같은 머신, 반복 횟수 1회**로 잰 값이고 합격 기준도 정하지 않았으므로 결과로 채택하지 않았다. 지연, 처리량, 수용 인원은 [validation.md](validation.md)의 절차(합격 기준을 측정 전에 작성, Release 빌드, 여러 번 반복, p50/p95/p99)로 3단계에서 측정한다.

## 8. 남은 일

1. **Linux 빌드와 테스트 확인** (VM). 보류 중이며, 확인하면 이 문서의 1절 첫 행과 6절 목록을 갱신한다.
2. 4.4의 변형 검증을 자동화할지 결정한다.
3. 2단계(채팅과 MySQL)에 들어가기 전에 목표와 완료 기준을 재검토한다([project.md](../project.md)).
