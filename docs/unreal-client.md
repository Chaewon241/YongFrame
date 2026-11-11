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

기본 도구는 `list_toolsets`, `describe_toolset`, `call_tool` 세 개이고 나머지는 툴셋 이름으로 호출한다(`bEnableToolSearch`). `list_toolsets`로 확인한 툴셋은 에디터 상태(콘솔 변수, 선택, 카메라, 플레이 세션, 로그), 액터, 에셋, 블루프린트, 레벨, 데이터 테이블, 머티리얼, 메시, 텍스처, 오브젝트 프로퍼티, 그리고 호출을 묶는 샌드박스 파이썬 도구다. **쓰기 도구는 호출해 보지 않았다.**

- **인증이 없다.** 플러그인은 DNS 리바인딩을 막는 Origin 검사만 한다. 로컬 주소로만 열어 두는 것이 유일한 방어이므로, 이 PC의 다른 프로그램은 이 포트로 에디터를 조작할 수 있다. 쓰지 않을 때는 에디터를 닫는다.
- Epic의 실험 기능이라 도구 범위와 안정성은 써 보며 확인해야 한다.

## 한계

- Unreal 에디터 타깃(`Development Editor`)만 5.8에서 빌드하고 시험했다. 게임 타깃(`Game`), Debug/Shipping 구성, 다른 플랫폼은 확인하지 않았다.
- 5.8과 MSVC 14.44 조합은 선호 구성이라 추가 우회가 필요 없었다. 5.4로 되돌려 쓸 경우 위 "5.4만" 우회가 다시 필요하다(백업 폴더에 그 버전이 있다).
- 접속은 동기식 소켓으로 한 번에 끝내는 시험이다. 게임 틱 안에서 쓰려면 비동기(별도 스레드 또는 비차단 소켓)로 바꿔야 한다.
- 채팅 메시지(`chat.pb.*`)는 컴파일만 되며 Unreal에서 보내 본 적은 없다.
- `ThirdParty/Protobuf`(약 300 MB)는 이 PC에서만 복사해 둔 것이다. 다른 PC에서는 위 절차를 반복해야 한다.

## 다음

1. (선택) 에디터에서 프로젝트를 열어 보고 VS 솔루션을 생성한다.
2. 4단계 서버 작업(방 관리자, 숫자 방 코드, 20Hz 틱, 서버 판정 이동)을 만들고, 그에 맞춰 `.proto`와 Unreal 쪽 비동기 접속 계층을 만든다.
3. 맵 크기, 이동 속도, 충돌 크기를 정한다([game-plan.md](game-plan.md)의 미정 사항).
