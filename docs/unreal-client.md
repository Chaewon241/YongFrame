# Unreal 클라이언트 연동 (스파이크)

> 상태: **스파이크 성공.** Unreal Engine 5.4.4에서 먼저 확인했고, **5.8.4로 올린 뒤에도 같은 시험을 통과했다.** 프로젝트가 이 서버에 TCP로 접속해 `PingRequest` 5건을 보내고 `PingResponse`를 받아 요청ID와 payload를 바이트 단위로 대조했다(분할 전송 1건, 한글 payload 1건 포함). 서버 로그도 프레임 5개 수신, 5개 송신으로 일치했다. 아직 게임 기능(방, 이동)은 없고, 에디터 화면(UI)에서의 확인도 하지 않았다. 이 문서는 프로토콜을 다른 클라이언트에서 구현하는 사람(그리고 이후의 나)을 위한 안내다.

## 환경과 위치

| 항목 | 값 |
| --- | --- |
| 엔진 | **Unreal Engine 5.8.4** (`D:\UE_5.8`, 런처 설치본). 5.4.4(`D:\UE_5.4`)에서 먼저 시험했고, 그 시점의 프로젝트는 `D:\UnrealProjects\YongClient_UE54_backup`에 백업해 두었다 |
| Unreal 프로젝트 | `D:\UnrealProjects\YongClient` (이 저장소와 별개. 경로에 한글이 있으면 Unreal 빌드가 깨질 수 있어 영문 경로에 둠) |
| 컴파일러 | MSVC 14.44 (VS 2022). 5.8의 선호 버전이라 별도 우회 없이 빌드된다. (5.4는 14.38을 선호하는데 이 PC의 14.38은 x64 라이브러리가 없는 불완전 설치라서 14.44를 강제로 써야 했고 우회 코드가 필요했다) |
| Protobuf | 서버와 **같은 6.33.4** (서버용 vcpkg 빌드의 헤더와 정적 라이브러리를 복사해 사용. 정적 라이브러리 + 동적 CRT(`/MD`)) |
| 프로젝트 구성 | 모듈 `YongClient`(런타임), 커맨들릿 `PingSpikeCommandlet`(화면 없이 Ping 확인), 생성된 `ping.pb.*`, `chat.pb.*`. 타깃은 `DefaultBuildSettings = BuildSettingsVersion.Latest`를 써야 한다(`V5`로 두면 5.8의 공유 빌드 환경과 경고 수준이 달라 "UnrealEditor와 공통 빌드 산출물을 가진다" 오류로 빌드가 실패함) |

## 프로토콜 (docs/protocol.md가 기준)

프레임: `[body_length: uint32][message_id: uint16][Protobuf 본문]`, 모두 **big-endian**. `body_length`는 `message_id`(2바이트)와 본문의 합이며 허용 범위는 2~65,536이다. 요청 `PingRequest`는 ID 1, 응답 `PingResponse`는 ID 2.

### 기준 바이트 (golden bytes, 구현 확인용)

Protobuf 본문은 `protoc --encode`로, 프레임은 위 형식대로 만든 값이다.

| 내용 | 프레임 전체 (16진수) |
| --- | --- |
| `PingRequest{request_id=1, payload="hi"}` | `00 00 00 08` `00 01` `08 01 12 02 68 69` |
| `PingRequest{request_id=1000, payload="unreal-ping-0"}` | `00 00 00 14` `00 01` `08 E8 07 12 0D 75 6E 72 65 61 6C 2D 70 69 6E 67 2D 30` |
| 위 요청의 응답 (`PingResponse`, ID 2) | 요청과 같되 `message_id`만 `00 02` |

첫 줄에서 `00 00 00 08`은 `body_length` = 2(ID) + 6(본문)이고, `08 01`은 `request_id=1`, `12 02 68 69`는 `payload="hi"`다. 두 번째 줄의 `08 E8 07`은 `request_id=1000`(varint)이다. 이 값으로 클라이언트 구현의 인코더를 단위 시험할 수 있다. 두 번째 줄은 Unreal 스파이크가 실제로 서버에 보낸 프레임이다.

## Unreal에 Protobuf 넣기 (핵심만)

1. 서버 저장소의 `proto/*.proto`를 서버가 쓰는 `protoc`(`C:\dev\vcpkg_installed\game-server-lab\x64-windows-static-md\tools\protobuf\protoc.exe`, libprotoc 33.4)로 생성해 **프로젝트의 `ThirdParty/Generated/`** 에 둔다. 생성 코드는 `Source/` 밖에 둬야 UBT가 따로 컴파일하지 않는다(경고 억제 래퍼로만 컴파일하기 위해).

   ```bash
   protoc.exe -I <저장소>/proto --cpp_out=D:/UnrealProjects/YongClient/ThirdParty/Generated <저장소>/proto/ping.proto <저장소>/proto/chat.proto
   ```

2. `ThirdParty/Protobuf/{include,lib,bin}`에 서버 vcpkg 설치본의 `google/`, `absl/`, `utf8_*.h`(include), `libprotobuf.lib`, `utf8_range.lib`, `utf8_validity.lib`, `absl_*.lib`(lib), `protoc.exe`(bin)를 복사한다(약 300 MB, 저장소에 올리지 않는다).
3. `YongClient.Build.cs`가 위 두 폴더를 include 경로와 라이브러리로 등록한다. `bEnableExceptions = true`, `bUseRTTI = true`가 필요하다.
4. 생성된 `.pb.cc`는 파일마다 하나씩 래퍼(`Private/Proto_ping.cpp`, `Proto_chat.cpp`)에서 `THIRD_PARTY_INCLUDES_START/END`와 `Windows/AllowWindowsPlatformTypes.h`로 감싸 포함한다. 두 개를 한 파일에 합치면 익명 이름공간과 `init_seg` 때문에 컴파일이 깨진다.
5. Protobuf 헤더를 포함하는 구간에서는 Unreal의 `verify`, `check` 매크로를 잠시 해제한다(`#pragma push_macro` / `#undef` / `pop_macro`). 안 하면 Abseil의 `btree::verify()`와 충돌한다.

`.proto`를 바꾸면 1번을 다시 실행하고 에디터 타깃을 다시 빌드한다. 서버와 클라이언트의 Protobuf 버전이 달라지면 생성 코드가 맞지 않을 수 있다.

## 겪은 문제

5.8로 올리면서 **5.4 시절의 우회 중 세 가지(ASAN 정의, 공유 PCH 끄기, 컴파일러 버전 고정)를 뺐고 빌드가 통과했다.** 아래 표에서 "5.4만"이라고 적은 줄이 그것이다. 나머지(매크로 해제, `.pb.cc` 래퍼 분리, PowerShell 따옴표)는 5.8에서도 그대로 유지했고 필요 여부는 따로 시험하지 않았다.

| 문제 | 원인 | 해결 |
| --- | --- | --- |
| **(5.4만)** vcpkg로 Unreal용 Protobuf를 MSVC 14.38/14.40으로 빌드하려 했더니 "Visual Studio 인스턴스를 찾을 수 없음" | 14.38은 `lib\x64`가 없는 불완전 설치. 14.40은 설치돼 있으나 vcpkg가 해당 툴셋 버전을 인식하지 못함(원인은 확인하지 못함) | 서버용으로 이미 빌드한 Protobuf(14.44)를 복사해 쓰고, Unreal도 14.44로 고정. 이후 링크까지 성공했다 |
| **(5.4만)** Unreal 5.4 헤더가 컴파일 오류(`__has_feature`, C4668/C4067) | MSVC 14.44에 asan 헤더가 있어 UE의 `PLATFORM_HAS_ASAN_INCLUDE`가 켜지는데 MSVC에는 `__has_feature`가 없음 | 모듈 규칙에서 `PLATFORM_HAS_ASAN_INCLUDE=0`을 정의하고 공유 PCH를 쓰지 않음(`PCHUsage = NoPCHs`). 타깃 전체 정의는 `UnrealEditor`와 공유되어 바꿀 수 없다. 5.8에서는 제거 |
| Abseil `btree.h`의 `verify()` 선언이 컴파일 오류 | Unreal이 `verify` 매크로를 정의 | Protobuf 포함 구간에서 `verify`, `check` 매크로 해제 |
| 두 `.pb.cc`를 한 파일로 합치자 오류(`schemas` 재정의, init 세그먼트) | 생성 코드의 익명 이름공간과 `init_seg` | 파일마다 래퍼를 따로 둠 |
| 커맨들릿이 `Assertion failed: GEngine && GEditor`로 종료 | 커맨들릿의 `IsEditor = false` 설정으로 엔진이 에디터 없이 초기화됨 | 기본값(`IsEditor = true`) 사용 |
| `-host=127.0.0.1`이 `127`로 잘려 "잘못된 주소" | **PowerShell**이 따옴표 없는 `-host=127.0.0.1` 인자를 점에서 쪼개 넘김 | 인자를 작은따옴표로 감싸 `'-host=127.0.0.1'`로 전달 (Unreal 파서 문제가 아니었다. 처음엔 `FParse::Value` 탓으로 짐작하고 파서를 바꿨는데 효과가 없었고, 원인은 셸이었다) |

## 확인 방법

### 터미널 (화면 없이)

서버를 띄운 뒤 Unreal 커맨들릿으로 Ping을 보낸다. **PowerShell에서는 인자를 따옴표로 감싸야 한다.**

```bash
build\windows-local\Release\game_server.exe --port 17777 --log-level debug --stats-interval 0
```

```bash
& 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' 'D:\UnrealProjects\YongClient\YongClient.uproject' '-run=PingSpike' '-host=127.0.0.1' '-port=17777' '-count=5' '-unattended' '-nullrhi' '-nosplash' '-stdout' '-FullStdOutLogOutput'
```

`LogPingSpike`가 붙은 줄에 각 요청의 `OK`와 마지막 줄 "결과: 성공"이 나오고 종료 코드는 0이다. 한글은 콘솔 코드 페이지 때문에 `?`로 보일 수 있다(비교는 바이트로 하므로 결과에는 영향 없음). 서버 로그에는 `[요청] id=1 request_id=1000 …`이 찍힌다.

빌드(코드를 고친 뒤):

```bash
& 'D:\UE_5.8\Engine\Build\BatchFiles\Build.bat' YongClientEditor Win64 Development '-Project=D:\UnrealProjects\YongClient\YongClient.uproject' -WaitMutex
```

### Unreal 에디터 / Visual Studio

아직 에디터 화면에서의 확인은 하지 않았다. `YongClient.uproject`를 더블클릭하면 에디터가 열리고(처음에는 모듈을 빌드하겠냐고 물을 수 있음), 커맨들릿은 터미널에서만 실행한다. Visual Studio에서 열려면 `.uproject`를 우클릭해 "Generate Visual Studio project files"를 실행한다(이 PC에서는 아직 시도하지 않았다).

## 4단계 게임 프로토콜 (Unreal 구현용)

서버가 지원하는 게임 메시지다(자세한 규칙은 [stage4.md](stage4.md), 정의는 `proto/game.proto`). 프레임 형식은 위와 같다. 같은 연결에서 응답, 스냅샷(초당 20개), 방 알림이 섞여 오므로 `message_id`로 구분해야 한다.

| ID | 메시지 | 방향 |
| ---: | --- | --- |
| 100 / 101 | `CreateRoomRequest{nickname}` / `CreateRoomResponse{result, state}` | C→S / S→C |
| 102 / 103 | `JoinRoomRequest{room_code, nickname}` / `JoinRoomResponse{result, state}` | C→S / S→C |
| 104 / 105 | `LeaveRoomRequest{}` / `LeaveRoomResponse{result}` | C→S / S→C |
| 106 | `MoveInput{sequence, dir_x, dir_y}` (응답 없음) | C→S |
| 110 | `RoomEvent{kind, player, host_player_id}` | S→C |
| 111 | `WorldSnapshot{tick, players[{player_id, x, y, last_input_seq}]}` | S→C |
| 107 / 108 | `GatherRequest{node_id}` / `GatherResponse{result, node_id, wood, stone}` | C→S / S→C |
| 112 | `ResourceUpdate{tick, nodes[{node_id, hp}]}` (바뀐 노드가 있는 틱에만) | S→C |
| 113 / 114 | `BuildRequest{structure_type, x, y}` / `BuildResponse{result, structure_id, wood, stone}` | C→S / S→C |
| 115 | `StructureUpdate{tick, structures[{structure_id, type, x, y, width, height, state, ready_tick}]}` (새로 생기거나 바뀐 구조물이 있는 틱에만) | S→C |

자원 채집(107, 108, 112, `RoomState.nodes`, 결과 코드 9~12)의 규칙은 [stage4-gather.md](stage4-gather.md)가 기준이고, Unreal 쪽 구현은 아래 "자원 채집 (Unreal)" 절이다. 집 짓기(113~115, `RoomState.structures`, 결과 코드 13~17)와 이동 충돌은 [content-roadmap.md](content-roadmap.md)가 기준이고 Unreal 쪽은 "집 짓기 (Unreal)" 절이다.

- 방 코드는 숫자 6자리 **문자열**이다(`"000042"`처럼 앞자리가 0일 수 있다). 정수로 바꾸지 않는다.
- 위치 단위는 센티미터(Unreal과 같음), 맵은 5,000 x 5,000 cm이고 원점은 한쪽 모서리다. 이동 속도 360 cm/s(처음에는 300), 최대 4명, 틱 20 Hz는 **임의로 정한 기본값**이다(`PlayConfig`).
- 클라이언트는 이동 **방향**(길이 1 이하)과 증가하는 `sequence`만 보낸다. 위치는 서버가 계산한 스냅샷을 그린다. 정지는 방향 (0, 0)을 보낸다. 서버는 다음 입력이 올 때까지 마지막 방향으로 계속 움직인다.
- `last_input_seq`는 서버가 마지막으로 반영한 이 플레이어의 입력 번호다. 나중에 클라이언트 예측과 보정에 쓸 수 있다.
- 규칙 위반(잘못된 코드, 가득 참 등)은 응답의 `result`로 알리고 연결은 유지한다.

### 기준 바이트 (Unreal 인코더 시험용, `protoc --encode`로 만든 값)

| 내용 | 프레임 전체 (16진수) |
| --- | --- |
| `CreateRoomRequest{nickname="alice"}` | `00 00 00 09` `00 64` `0A 05 61 6C 69 63 65` |
| `JoinRoomRequest{room_code="000042", nickname="bob"}` | `00 00 00 0F` `00 66` `0A 06 30 30 30 30 34 32 12 03 62 6F 62` |
| `LeaveRoomRequest{}` | `00 00 00 02` `00 68` (본문 없음) |
| `MoveInput{sequence=1, dir_x=1.0, dir_y=0}` | `00 00 00 09` `00 6A` `08 01 15 00 00 80 3F` (`dir_y`가 0이면 필드를 생략) |
| `MoveInput{sequence=2, dir_x=0.5, dir_y=-1.0}` | `00 00 00 0E` `00 6A` `08 02 15 00 00 00 3F 1D 00 00 80 BF` |
| `CreateRoomResponse{OK, state{room_code="000042", your=1, host=1, players=[1:"alice"]}}` | `00 00 00 1B` `00 65` `12 17 0A 06 30 30 30 30 34 32 10 01 18 01 22 09 08 01 12 05 61 6C 69 63 65` (`result`가 OK(0)이면 필드를 생략) |

| `GatherRequest{node_id=7}` | `00 00 00 04` `00 6B` `08 07` |
| `GatherResponse{OK, node_id=7, wood=1, stone=0}` | `00 00 00 06` `00 6C` `10 07 18 01` (`result`가 OK(0)이면 생략, `stone`이 0이면 생략) |
| `GatherResponse{TOO_FAR(11), node_id=7, wood=3, stone=2}` | `00 00 00 0A` `00 6C` `08 0B 10 07 18 03 20 02` |
| `ResourceUpdate{tick=3, nodes=[{node_id=1, hp=0}]}` | `00 00 00 08` `00 70` `08 03 12 02 08 01` (`hp`가 0이면 필드를 생략) |

| `BuildRequest{HOUSE, x=2600, y=2400}` | `00 00 00 0C` `00 71` `15 00 80 22 45 1D 00 00 16 45` (`structure_type`이 HOUSE(0)이면 생략) |
| `BuildResponse{OK, structure_id=1}` | `00 00 00 04` `00 72` `10 01` |
| `BuildResponse{NOT_ENOUGH_RESOURCES(15), wood=19, stone=9}` | `00 00 00 08` `00 72` `08 0F 18 13 20 09` |
| `StructureUpdate{tick=3, [{id=1, HOUSE, x=2600, y=2400, 300x300, BUILDING, ready_tick=100}]}` | `00 00 00 1E` `00 73` `08 03 12 18 08 01 1D 00 80 22 45 25 00 00 16 45 2D 00 00 96 43 35 00 00 96 43 40 64` (`type`과 `state`가 0이면 생략) |

`float`은 Protobuf가 4바이트 little-endian으로 인코딩한다(`1.0` = `00 00 80 3F`). 프레임 헤더(길이, ID)는 big-endian이다.

### 터미널로 같은 것을 먼저 해 보기

Unreal 없이 서버를 확인하는 터미널 클라이언트가 있다(`game_client`).

```bash
build\windows-local\Release\game_server.exe --port 7777 --log-level info
```

```bash
build\windows-local\Release\game_client.exe --port 7777 --create --nick alice
```

방 코드가 출력되면 다른 터미널에서 `game_client.exe --port 7777 --join <코드> --nick bob`으로 입장하고, `/move 1 0`, `/stop`, `/leave`, `/help`를 써 본다.

## 접속 계층 (`FGameNetworkConnection`, Unreal 프로젝트)

Unreal 프로젝트의 `Source/YongClient/{Public,Private}/Network/GameNetworkConnection.*`가 서버와의 TCP 연결 하나를 맡는다. **프레임 단위 송수신만** 하며 게임 메시지(방 만들기, 입장, 이동, 스냅샷)는 아직 모른다(다음 단계). 수신 전용 스레드가 프레임을 큐에 넣고 게임 스레드가 `ReceiveMessage`로 꺼낸다.

처음 작성된 버전에서 코드 검토로 찾아 고친 4가지:

| 문제 | 수정 |
| --- | --- |
| 헤더(6바이트)나 본문이 쪼개져 오면 연결을 끊음(`Recv`가 요청보다 적게 줄 수 있음) | 정확히 N바이트가 모일 때까지 반복해서 읽음(`RecvExact`, 0.1초 대기 단위) |
| 서버가 아무것도 보내지 않으면 `Disconnect`가 수신 스레드의 무한 대기 때문에 멈춤 | 0.1초마다 중지 요청을 확인하므로 끝나는 데 길어야 0.1초 |
| 본문 버퍼가 65,530바이트라 최대 크기(65,534바이트) 프레임에서 넘침 | 길이(2~65,536)를 검사한 뒤에만 그 크기만큼 동적 할당 |
| 헤더와 본문을 따로 `Send`하고 `NoDelay`를 켜지 않아 지연 ACK와 겹치면 수십 ms 튈 수 있음 | 한 버퍼로 합쳐 한 번에 전송, `NoDelay` 켬, 부분 전송 반복 |

덤으로: 연결 제한 시간(기본 5초), 큐를 `TQueue`(SPSC)로, 오류 문자열의 스레드 안전성, 연결이 끊기면 `Tick`에서 자원 정리.

### 시험 (`GameConnectSpike` 커맨들릿, 에디터를 닫고 실행)

| 모드 | 상대 | 확인하는 것 |
| --- | --- | --- |
| `real` | 진짜 `game_server` | Ping 50회 응답 일치와 지연(p50 약 1.6~2.0 ms, 최대 약 2.5~3.3 ms), 유휴 상태 `Disconnect` 시간(수 ms), 서버가 없을 때 제한 시간 안에 실패, 너무 큰 본문 거부 |
| `fake` | `Tools/fake_server.py <포트>` | 프레임을 1바이트씩 쪼개 보내기, 두 프레임 붙여 보내기, 최대 크기(65,534바이트) 본문, 규약에 어긋난 길이(65,537)에 연결 끊기 |
| `send` | `Tools/fake_server.py <포트> send` | 프레임 20개가 한 번의 수신으로 통째로 도착하는지(송신을 한 번에 하는지). 판정은 가짜 서버의 종료 코드 |

모두 통과했다. **변형 검증:** 코드를 예전 버그 방식으로 되돌려 시험이 잡는지 확인했다. 분할 수신, `Disconnect` 지연, 본문 크기 제한, 송신 분리 4가지를 모두 잡았다. 다만 송신 분리는 처음 만든 "왕복 지연" 시험으로는 **못 잡았다**(루프백은 ACK가 즉시 와서 지연이 생기지 않음). 그래서 `send` 모드를 추가했고, 수정한 코드는 20/20(첫 수신 1,006바이트), 되돌린 코드는 0/20(첫 수신 6바이트)이었다.

시험은 로그의 `[통과]/[실패]`와 마지막 `결과:` 줄로 판정한다. **종료 코드로는 판정하지 못한다.** 프로젝트에 `AllToolsets` 플러그인이 켜져 있으면 엔진이 시작할 때 `GameFeatureData` 규칙 오류 2건을 내서 종료 코드가 항상 1이다(시험 결과와 무관).

```bash
build\windows-local\Release\game_server.exe --port 18300 --log-level info --stats-interval 0
```

```bash
& 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' 'D:\UnrealProjects\YongClient\YongClient.uproject' '-run=GameConnectSpike' '-mode=real' '-host=127.0.0.1' '-port=18300' '-unattended' '-nullrhi' '-nosplash' '-stdout'
```

## 게임 메시지 계층 (`FGameClient`, Unreal 프로젝트)

`Source/YongClient/{Public,Private}/Network/GameClient.*`가 접속 계층 위에서 서버의 게임 프로토콜([stage4.md](stage4.md))을 말한다. 메시지 ID 표는 `GameMessageIds.h`(서버 `message_ids.h`와 같은 값, 서버가 바뀌면 함께 고친다)다. Protobuf는 `GameClient.cpp` 안에만 있고 헤더는 Unreal 타입만 쓴다.

| 하는 일 | 내용 |
| --- | --- |
| 요청 | `CreateRoom(닉네임)`, `JoinRoom(코드, 닉네임)`, `LeaveRoom()`, `SendMove(dx, dy)`. 응답은 델리게이트(`OnCreateRoomResult`, `OnJoinRoomResult`, `OnLeaveRoomResult`)로 온다 |
| 이동 입력 | 길이 1 초과 방향은 1로 줄이고(`double`로 계산), NaN/무한대는 보내지 않는다. 입력 번호는 **보낸 것만** 1씩 올린다 |
| 방 상태 | 접속자 목록과 방장을 보관한다. 입장/퇴장/방장 변경 알림(`OnRoomEvent`)을 받으면 **상태를 먼저 갱신한 뒤** 알린다 |
| 스냅샷 | 서버가 계산한 위치(cm)를 보관하고 `OnSnapshot`으로 알린다. 위치를 클라이언트가 계산하지 않는다 |
| 끊김 | 서버/네트워크/프로토콜 오류로 끊기면 이유와 함께 `OnDisconnected`를 **한 번만** 부른다. 사용자가 `Disconnect()`로 끊으면 부르지 않는다 |
| 모르는 입력 | Ping 응답과 모르는 메시지 ID, 모르는 이벤트 종류는 무시한다(서버가 새 종류를 추가해도 깨지지 않게). 알려진 메시지를 해석하지 못하면 프로토콜 오류로 연결을 끊는다 |

`UObject`가 아닌 일반 클래스다(에디터 없이 커맨들릿에서 시험하려고). 게임 안에서는 `GameInstance` 서브시스템이나 액터가 매 프레임 `Tick()`을 부르는 얇은 래퍼가 필요하다(아직 없다).

### 시험 (`GameClientSpike` 커맨들릿, 에디터를 닫고 실행)

| 모드 | 상대 | 내용 |
| --- | --- | --- |
| `real` (53건) | 진짜 `game_server` | 클라이언트 셋(alice, bob, carol)으로 방 만들기, 코드 입장, 입장 알림(본인은 못 받음), 오류 응답 4종(잘못된 코드, 없는 방, 중복 닉네임, 규칙 위반 닉네임)과 연결 유지, 이동 입력과 스냅샷(틱 증가, 틱당 이동 거리 15 cm 이하, 입력 번호 반영), 나가기와 퇴장 알림, 방에 없을 때 `NotInRoom`, 다시 입장하면 새 플레이어 번호, 방장이 나가면 퇴장 -> 방장 변경 알림과 방장 교체, 사용자가 끊으면 `OnDisconnected` 없음 |
| `fake` (7건) | `Tools/fake_server.py <포트>` | 규약에 어긋난 길이를 받으면 `OnDisconnected`(이유에 `body_length`), 한 번만 알림, 끊긴 뒤 요청은 실패하고 죽지 않음 |
| `garbage` (7건) | `Tools/fake_server.py <포트> garbage` | 해석할 수 없는 `WorldSnapshot`을 받으면 프로토콜 오류로 끊고 알림 |

전부 통과했다. **변형 검증(9가지):** 코드를 일부러 망가뜨려 시험이 잡는지 확인했다. 나가기 후 방 상태 안 비움, 퇴장 알림 미반영, 방장 변경 미반영, 입장 알림 미반영, NaN 입력 전송, 해석 불가 스냅샷 무시, 끊김 미통지, 방 만들기 상태 미저장 8가지는 잡혔다. **1가지는 잡지 못했다:** "전송에 실패한 입력의 번호도 올림"이다. `SendMove`가 연결이 끊긴 상태에서는 먼저 거절하므로, 연결은 있는데 전송만 실패하는 경우를 시험으로 만들 수 없다(방어 코드로만 남아 있다).

시험은 로그의 `[통과]/[실패]`와 마지막 `결과:` 줄로 판정한다(종료 코드는 `AllToolsets` 플러그인 때문에 항상 1). 에디터가 열려 있으면 Live Coding이 빌드를 막으므로 닫고 실행한다.

```bash
build\windows-local\Release\game_server.exe --port 18320 --log-level info --stats-interval 0
```

```bash
& 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' 'D:\UnrealProjects\YongClient\YongClient.uproject' '-run=GameClientSpike' '-mode=real' '-host=127.0.0.1' '-port=18320' '-unattended' '-nullrhi' '-nosplash' '-stdout'
```

## Unreal 프로젝트 저장소

`D:\UnrealProjects\YongClient`는 이 저장소와 별개인 **로컬 git 저장소**다(원격 없음). 제외 항목은 `.gitignore`: 빌드 산출물, 서버에서 복사한 `ThirdParty/Protobuf`(약 300 MB), 로그, `.claude/`(로컬 설정). `Content/`의 애셋(`.uasset`, `.umap` 등)은 `.gitattributes`로 **Git LFS**에 올린다(`git lfs ls-files`로 확인). `Config/DefaultEngine.ini`에 엔진이 만든 `SecurityToken`(Android 파일 서버용)이 있으니 원격에 올릴 때는 비공개 저장소로 하거나 그 줄을 뺀다.

## 게임 클라이언트 기반 (Unreal 프로젝트의 `Yong/` 클래스)

게임 메시지 계층 위에 화면을 만드는 C++ 기반이다. 위치는 **서버가 계산한 값**이고 클라이언트는 방향만 보낸다.

| 클래스 | 역할 |
| --- | --- |
| `FGameSnapshotInterpolator` (`Network/`) | 20Hz 스냅샷 사이를 보간해 화면 프레임마다 부드러운 위치를 만든다. 순수 계산이다. 시간축은 **서버 틱 번호**이고, 도착 시각의 지터는 최근 (도착 시각 - 틱 x 틱 간격)의 **최솟값**으로 흡수한다. 0.1초(2틱) 과거를 그린다. 다음 스냅샷이 없으면 외삽하지 않고 마지막 위치에 머문다 |
| `UYongNetSubsystem` | `GameInstance` 수명의 블루프린트용 서버 연결(연결, 방 만들기/입장/나가기, 이동 입력, 방 상태, 보간된 위치, 끊김 이벤트). 레벨을 옮겨도 연결과 방 상태가 유지된다. PIE에서 플레이어를 여러 명으로 실행하면 플레이어마다 서브시스템이 따로 생긴다 |
| `YongTypes.h` | 블루프린트 타입(`EYongGameResult`, `FYongRoomState`, `FYongRoomEvent`, `FYongPlayerState`). 번호는 `int32` |
| `AYongPlayerActor` | 플레이어 한 명의 화면 표현. 서버 위치를 반영하고 움직인 방향으로 돌아본다. 모양/색/닉네임 위젯은 블루프린트 자식(`BP_YongPlayer`)에서 만들고 `OnPlayerInitialized`, `OnInfoUpdated`, `OnMovementStateChanged` 이벤트를 구현한다 |
| `AYongCameraRig` | 탑뷰 카메라(기본 경사 -55도, 거리 1,800 cm, 회전 고정). 화면 위가 +X, 오른쪽이 +Y |
| `AYongPlayerController` | WASD/방향키를 읽어 방향이 **바뀔 때만** 서버에 보낸다(대각선은 길이 1). Enhanced Input 애셋이 없어도 동작한다 |
| `AYongRoomGameMode` | 보간된 서버 위치로 플레이어 액터를 만들고, 위치를 갱신하고, 나간 사람은 지운다. 카메라가 내 플레이어를 따라간다. 연결이 끊기면 메뉴 레벨(`MenuLevelName`, 기본 `L_Menu`)로 돌아간다 |

좌표는 변환 없이 쓴다. 서버 좌표(cm)가 곧 월드 좌표이므로 바닥을 (0,0)~(5000,5000)에 두면 된다.

**시험(`GameClientSpike`):** `interp`(21건, 서버 불필요), `subsystem`(18건, 진짜 서버에 `GameInstance` 둘), `subsystem-fake`(4건). 전부 통과했다. 서브시스템 시험에서 서버 위치를 약 4 ms 간격으로 읽었을 때 샘플 사이 최대 이동이 1.94 cm였다(보간이 없으면 스냅샷마다 15 cm씩 점프). **변형 검증 10가지(보간기 6, 서브시스템 4)를 모두 잡았다.** 지터 시험의 핵심은 Offset을 최솟값 대신 마지막 값으로 바꾸면 프레임당 이동 편차가 7.4 cm로 커져 실패하는 것이다.

**월드가 필요한 부분은 PIE에서 확인했다**(아래 "메뉴와 HUD" 절의 시험). `Connect`는 서버가 없으면 제한 시간(기본 3초)만큼 게임 스레드를 막는다(비동기 연결은 아직 아니다). 블루프린트 이벤트(동적 델리게이트)는 같은 이벤트의 C++ 판(`NativeOn...`)으로만 자동 시험했다.

## 메뉴와 HUD (UMG)

화면 로직은 C++, 배치와 모양은 에디터(UMG)가 맡는다. 위젯 로직 클래스는 `UCLASS(Abstract)`이고, 블루프린트 자식이 `BindWidget` 이름과 **같은 이름과 타입**의 위젯을 트리에 두어야 컴파일된다.

| C++ 클래스 | 블루프린트 자식 | 필요한 위젯 (이름: 타입) |
| --- | --- | --- |
| `UYongMenuWidget` | `WBP_MainMenu` | `NicknameBox`, `HostBox`, `PortBox`, `CodeBox`: EditableTextBox / `CreateButton`, `JoinButton`: Button / `StatusText`: TextBlock |
| `UYongHudWidget` | `WBP_HUD` | `RoomCodeText`, `StatusText`: TextBlock / `PlayerListBox`: VerticalBox / `LeaveButton`: Button |

- `AYongMenuGameMode`(블루프린트 `BP_MenuGameMode`)는 `MenuWidgetClass`로 메뉴를 띄우고, `AYongRoomGameMode`(`BP_YongRoomGameMode`)는 `HudWidgetClass`로 HUD를 띄운다. 레벨은 메뉴 `L_Menu`, 플레이 `L_Farm`이다.
- 입력 검증은 순수 함수(`YongUi::` 네임스페이스, `YongUiHelpers.*`)로 분리했다: 방 코드는 숫자 6자리(입력 필터가 숫자 외 글자를 걸러 내고 6자리로 자른다), 닉네임은 앞뒤 공백을 지우고 1~16 코드포인트(이모지 같은 서로게이트 쌍은 한 글자), 결과 열거형(`EYongGameResult`)별 한국어 문구는 서로 다르다. 서버가 거부하는 전각 숫자를 걸러 내는 것도 여기서 한다.
- 컨트롤러는 `FInputModeGameAndUI`와 마우스 커서를 쓴다. WASD는 게임이 받고 클릭은 HUD 버튼이 받는다.
- `UYongNetSubsystem::LastDisconnectReason`에 끊김 사유를 남겨 메뉴 레벨로 돌아온 뒤 `StatusText`에 보여 준다.

**자동 시험:** `-run=GameClientSpike -mode=ui`(21건, 서버·월드 불필요)가 `YongUi::` 함수를 확인한다. **변형 검증 5가지를 모두 잡았다**(6자리 자르기 제거, 길이 검사 제거, 닉네임 공백 제거 제거, 서로게이트 처리 제거, `RoomFull` 문구를 `RoomNotFound`와 같게 변경).

**PIE 시험(사람이 확인):** 서버를 켜고 에디터의 플레이 설정을 Number of Players 2, Net Mode `Play Standalone`, "Run Under One Process" 끔으로 둔 뒤 `L_Menu`에서 플레이했다. 사용자가 방 만들기, 코드로 입장, 한 방에 여러 플레이어가 보이는 흐름이 잘 동작한다고 확인했다. 창별 세부 항목(닉네임 라벨, 나가기, 서버 종료 시 메뉴 복귀, 창 3~4개)은 항목별 기록이 없으므로 확인된 것으로 적지 않는다. 패키징한 빌드는 시험하지 않았다.

**에디터 쪽 메모**
- `Config/*.ini`에 기본 맵(`GameDefaultMap`)이 없다. 에디터 플레이에는 영향이 없고 패키징할 때 정한다.
- 환경 소품은 Kenney 메시 8개(`Content/YongClient/Environment/Kenney/`)다. 출처와 라이선스는 아직 확인하지 않았다(Kenney 에셋은 보통 CC0이지만 받은 파일의 라이선스를 확인해 이 문서에 적는다).
- 이 에셋들은 MCP 세션이 만들었다. 이 작업에서 MCP는 레이아웃과 에셋만 맡고 로직은 C++에 둔다.

**작업 순서 규칙:** C++ 빌드는 에디터가 꺼져 있어야 하고(Live Coding이 막는다), MCP는 에디터가 켜져 있어야 한다. C++ 단계와 에디터 단계를 번갈아 한다. 애셋(`Content/`)을 만들기 전에 Unreal 저장소의 `.gitattributes`(Git LFS)를 커밋해 둔다.

## 자원 채집 (Unreal)

서버 규칙과 완료 기준은 [stage4-gather.md](stage4-gather.md)다. Unreal 쪽 구현:

| 클래스 | 역할 |
| --- | --- |
| `FGameClient` | `Gather(NodeId)`, 채집 응답(`OnGatherResult`: 결과, 노드 번호, 내 인벤토리), 체력 변화(`OnResourcesChanged`), 방 상태의 `Nodes`(체력은 `ResourceUpdate`로 갱신), `GetInventory()`. 방을 나가거나 연결이 끊기면 인벤토리와 노드가 비워지고, 방 밖에서 도착한 응답/알림은 버린다. 모르는 노드 번호나 종류는 건너뛴다 |
| `UYongNetSubsystem` | 블루프린트용 `Gather`, `GetResourceNodes`, `GetInventory`, `FindNearestAliveNode(위치, 최대 거리)`, `GetMyPosition()`(보간된 내 위치), 이벤트 `OnGatherResult`, `OnResourcesChanged`(바뀐 노드 번호들) |
| `AYongResourceNodeActor` | 노드 하나의 화면 표현. 노드 번호로 메시 후보(`MeshVariants`) 중 하나를 고르고, 체력이 줄면 눌리는 연출, 0이면 숨김, 재생하면 다시 보이게 한다(C++가 처리). 목표로 고르면 약간 커진다. 충돌 없음. `OnNodeHit/Depleted/Respawned`는 블루프린트 이벤트 |
| `AYongRoomGameMode` | `TreeNodeClass`, `RockNodeClass`로 서버 노드 목록에서 액터를 만들고 체력 변화를 반영한다. `SetTargetedNode`로 목표 표시 |
| `AYongPlayerController` | 내 위치에서 150 cm 안의 가장 가까운 살아 있는 노드를 목표로 고르고, `E`를 누르고 있으면 0.55초마다 채집을 요청한다(서버 허용 거리는 200 cm, 쿨다운 0.5초) |
| `UYongHudWidget` | 선택 위젯 `WoodText`, `StoneText`, `HintText`(`BindWidgetOptional`, 없어도 컴파일된다). 거부 문구(너무 멂, 고갈)는 2초만 `StatusText`에 보인다. 쿨다운 거부는 띄우지 않는다 |

**시험(`GameClientSpike`):** `gather`(40건, 진짜 서버에 `FGameClient` 셋: 방 만들기 응답의 노드 50개와 종류, 시작 위치에서 `TooFar`와 `NodeNotFound`, 가까운 나무까지 **실제로 걸어가** 치기, 목재 증가, 방 사람 모두의 체력 변화, 쿨다운 거부, 쿨다운을 지킨 연속 채집과 고갈, `NodeDepleted`, 돌의 석재, 늦은 입장자가 고갈 상태를 받음, 나가면 인벤토리와 노드가 비워짐), `subsystem-gather`(25건: 같은 흐름을 서브시스템으로, 가장 가까운 노드 찾기의 경계와 "번호가 낮은 쪽이 아니라 가까운 쪽" 선택 포함). 서버 재생(30초)은 서버 쪽 시험이 확인하므로 여기서는 기다리지 않는다. 서버는 기본 설정이면 되고(`game_server.exe --port N`) 특별한 옵션이 없다.

**아직 시험하지 못한 것:** 노드 액터, 게임 모드의 노드 동기화, 컨트롤러의 `E` 입력과 목표 선택, HUD 표시는 컴파일만 확인했다(월드가 필요하다). 에디터 쪽 에셋(`BP_ResourceTree`, `BP_ResourceRock`, `WBP_HUD`의 새 위젯)은 MCP 세션이 만들고, PIE에서 사람이 확인한다.

## 집 짓기 (Unreal)

서버 규칙, 완료 기준, 기본값은 [content-roadmap.md](content-roadmap.md)다. **서버 판정 이동에 충돌이 생겼다**: 건설 중이든 완공이든 구조물, 그리고 고갈되지 않은 나무/돌(반지름 60 cm 원)이 플레이어(반지름 40 cm)를 막는다. 노드 가까이에서는 중심에서 약 100 cm까지만 다가갈 수 있으므로(채집 허용 거리 200 cm 안), 노드를 향해 걸어가는 시험은 100 cm보다 먼 곳(125 cm)에서 멈춘다.

| 클래스 | 역할 |
| --- | --- |
| `FGameClient` | `BuildHouse(X, Y)`, 건설 응답(`OnBuildResult`: 결과, 구조물 번호, 내 인벤토리), 구조물 변화(`OnStructuresChanged`), 방 상태의 `Structures`(번호가 같으면 갱신, 없으면 추가). 나가거나 끊기면 비워지고, 방 밖에서 도착한 응답/알림은 버린다 |
| `YongBuild`(`YongBuildRules.*`) | 서버 규칙을 그대로 따라 한 순수 함수: `SnapToGrid`, `Evaluate`(너무 멂 → 맵 밖/겹침 → 자원 부족, 서버와 같은 순서), `Describe`. 값(집 300x300 cm, 격자 100, 거리 500, 목재 20/석재 10, 건설 100틱)은 서버 `PlayConfig`의 복사이며 어긋나면 서버가 기준이다. 보간된(0.1초 늦은) 위치로 계산하므로 경계에서는 서버와 다를 수 있다 |
| `UYongNetSubsystem` | `BuildHouse`, `GetStructures`, `EvaluatePlacement(마우스 땅 좌표)`(격자로 맞춘 자리와 판정을 돌려줌), 이벤트 `OnBuildResult`, `OnStructuresChanged` |
| `AYongStructureActor` | 구조물 하나의 표현. 서버 틱 기준 진행률만큼 높이가 자라다가 완료되면 완성 모양. 메시는 발자국에 맞춰 크기가 조정되고 바닥이 지면에 닿는다. 기본 모양은 엔진 큐브이고 `CompleteMesh`/`BuildingMesh`로 바꾼다. 충돌 없음 |
| `AYongBuildGhost` | 건설 미리보기. 지을 수 있으면 `ValidMaterial`(초록), 없으면 `InvalidMaterial`(빨강). 비워 두면 엔진 기본 머티리얼에 색 값을 넣어 본다 |
| `AYongRoomGameMode` | `HouseClass`, `BuildGhostClass`로 구조물 액터를 만들고 서버 틱과 완료를 반영한다. `ShowBuildGhost`로 미리보기를 켜고 끈다 |
| `AYongPlayerController` | `B`로 건설 모드를 켜고 끈다. 켜면 마우스가 가리키는 땅(Z = 0 평면)의 격자 중심에 미리보기가 따라가고, **왼쪽 클릭**으로 짓고(클라이언트가 지을 수 있다고 볼 때만 요청), **오른쪽 클릭이나 B**로 취소한다. 건설이 성공하면 자동으로 꺼진다 |
| `UYongHudWidget` | 선택 위젯 `BuildCostText`(평소 비용, 건설 모드에서는 지금 자리의 판정 문구), `BuildHintText`(건설 모드 안내). 건설 결과 문구를 `StatusText`에 2초 보인다 |

**시험(`GameClientSpike`):** `buildrules`(25건, 서버 불필요: 격자, 거리 경계 500 cm, 맵 경계, 구조물/노드/플레이어 겹침, 닿기만 하는 경계, 비용, 판정 순서), `build`(35건, 진짜 서버에 서브시스템 셋: 자원 없을 때 거부, **가까운 나무와 돌까지 실제로 걸어가 목재 20, 석재 10을 모으고**, 클라이언트가 지을 수 있다고 본 자리에 서버도 짓게 하는 판정 일치, 거부 코드(자원 부족, 겹침, 너무 멂), 정확한 비용 차감, 건설 중 → 약 5초 뒤 완공 알림, 집 한가운데로 걸어도 **몸이 발자국 안으로 들어가지 못함**, 늦은 입장자가 완공된 집을 받음, 나가면 비워지고 지은 사람이 나가도 집은 남음). 이 시험은 약 1분 걸린다. `ui`는 새 결과 문구 5개가 서로 다른지도 확인한다.

**아직 시험하지 못한 것:** 구조물 액터와 미리보기의 모양, 건설 모드의 마우스 입력(땅 좌표 계산, 클릭), HUD 표시는 컴파일만 확인했다(월드와 창이 필요). 에디터 에셋과 PIE 확인은 MCP 작업 뒤에 사람이 한다.

## Unreal MCP (Epic 제공, 5.8)

UE 5.8에는 Epic의 실험 기능 플러그인 `ModelContextProtocol`("Unreal MCP")과 `EditorToolset`이 들어 있다(`Engine\Plugins\Experimental`). 기본으로 꺼져 있고, 켜면 에디터가 HTTP로 MCP 서버를 연다. 이 프로젝트에서 켜서 서버 응답까지 확인했다.

| 항목 | 값 |
| --- | --- |
| 켜는 곳 | `YongClient.uproject`의 `Plugins`에 `ModelContextProtocol`, `EditorToolset` |
| 주소 | `http://127.0.0.1:8000/mcp` (`Config/DefaultEngine.ini`의 `[HTTPServer.Listeners] DefaultBindAddress=127.0.0.1`로 로컬에만 연다) |
| 자동 시작 | `Config/DefaultEditorPerProjectUserSettings.ini`의 `bAutoStartServer=True`. 끄려면 `False` |
| 설정 화면 | **편집 > 에디터 환경설정(Editor Preferences)** 에서 "Model Context Protocol"을 검색한다. 프로젝트 설정에는 없다(이 설정 클래스가 `EditorPerProjectUserSettings`라서 에디터 쪽에 표시됨) |
| Claude Code 연결 | 프로젝트 폴더의 `.mcp.json`(`mcpServers.unreal-mcp`, `type: http`)을 Claude Code가 읽는다. 또는 `claude mcp add --transport http unreal-mcp http://127.0.0.1:8000/mcp` |
| 설정 파일 생성 | 에디터 콘솔 명령 `ModelContextProtocol.GenerateClientConfig ClaudeCode`가 `.mcp.json`을 만든다(직접 쓴 것과 같은 형식) |

기본 도구는 `list_toolsets`, `describe_toolset`, `call_tool` 세 개이고 나머지는 툴셋 이름으로 호출한다(`bEnableToolSearch`). `list_toolsets`로 확인한 툴셋은 에디터 상태(콘솔 변수, 선택, 카메라, 플레이 세션, 로그), 액터, 에셋, 블루프린트, 레벨, 데이터 테이블, 머티리얼, 메시, 텍스처, 오브젝트 프로퍼티, 그리고 호출을 묶는 샌드박스 파이썬 도구다. 이 절을 처음 쓸 때는 쓰기 도구를 호출해 보지 않았고, 이후 메뉴/채집/집 짓기 에셋 작업(블루프린트, 머티리얼, 위젯, 레벨)을 MCP 세션으로 했다(정리는 [unreal-client-structure.md](unreal-client-structure.md)).

- **인증이 없다.** 플러그인은 DNS 리바인딩을 막는 Origin 검사만 한다. 로컬 주소로만 열어 두는 것이 유일한 방어이므로, 이 PC의 다른 프로그램은 이 포트로 에디터를 조작할 수 있다. 쓰지 않을 때는 에디터를 닫는다.
- Epic의 실험 기능이라 도구 범위와 안정성은 써 보며 확인해야 한다.

## 한계

- Unreal 에디터 타깃(`Development Editor`)만 5.8에서 빌드하고 시험했다. 게임 타깃(`Game`), Debug/Shipping 구성, 다른 플랫폼은 확인하지 않았다.
- 5.8과 MSVC 14.44 조합은 선호 구성이라 추가 우회가 필요 없었다. 5.4로 되돌려 쓸 경우 위 "5.4만" 우회가 다시 필요하다(백업 폴더에 그 버전이 있다).
- 접속은 동기식 소켓으로 한 번에 끝내는 시험이다. 게임 틱 안에서 쓰려면 비동기(별도 스레드 또는 비차단 소켓)로 바꿔야 한다.
- 채팅 메시지(`chat.pb.*`)는 컴파일만 되며 Unreal에서 보내 본 적은 없다.
- `ThirdParty/Protobuf`(약 300 MB)는 이 PC에서만 복사해 둔 것이다. 다른 PC에서는 위 절차를 반복해야 한다.

## 다음

1. 메뉴 → 방 → 이동까지 되는 최소 클라이언트는 완성됐다. 남은 확인: 3~4명, 방장 변경, 서버 종료 시 복귀, 잘못된 입력 문구.
2. Unreal 쪽 비동기 접속(지금은 `Connect`가 게임 스레드를 막는다).
3. 맵 크기, 이동 속도, 충돌 크기를 정한다([game-plan.md](game-plan.md)의 미정 사항). 지금 값은 서버의 임의 기본값이다.
4. 게임 기능(자원 채집, 집 짓기, 농사)은 game-plan.md의 확장 순서를 따른다.
