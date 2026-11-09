# Cross-Platform Game Server Lab

Boost.Asio 기반 C++ 게임 서버를 작은 기능부터 만들고, 각 단계의 정확성과 성능을 재현 가능한 기록으로 남기는 개인 프로젝트입니다.

> 현재 단계: **1. 패킷 송수신**. Ping 서버, 테스트 클라이언트, 자동 테스트가 구현되어 Windows에서 통과했습니다. **Windows 기준으로 1단계를 마쳤고, Linux 확인은 보류 중입니다.** 자세한 결과는 [docs/stage1.md](docs/stage1.md)입니다.

> 진행 중: **2. 채팅과 MySQL**. 인메모리 채팅(입장/퇴장/전파/기록 조회)을 구현했고 DB가 필요 없는 테스트 110개가 통과합니다. MySQL 저장소(`--storage mysql`)와 마이그레이션 도구를 구현했고, 실제 MySQL 대상 테스트 13개가 통과했습니다(2단계는 Windows 기준 완료). 3단계는 기준 측정과 확장 측정을 마쳤습니다(Active 4,000연결, 초당 4만 건까지 합격 기준 통과). 자세한 내용은 [docs/stage2.md](docs/stage2.md), [docs/stage3.md](docs/stage3.md)입니다.

## 문서 안내

| 문서 | 내용 |
| --- | --- |
| [project.md](project.md) | 목표, 기술 선택, 단계별 로드맵, 범위 |
| [docs/protocol.md](docs/protocol.md) | 1단계 TCP 패킷 형식과 오류 처리 계약 |
| [docs/validation.md](docs/validation.md) | 기능 검증, 부하 측정, 비교 실험 기록 방식 |
| [docs/stage1.md](docs/stage1.md) | 1단계 구성, 실행 방법, 검증 결과, 겪은 문제, 알려진 제한 |
| [docs/stage2.md](docs/stage2.md) | 2단계(채팅과 MySQL) 결정 기록, 진행 상태, MySQL 준비 방법 |
| [docs/protocol-chat.md](docs/protocol-chat.md) | 2단계 채팅 프로토콜 (초안) |
| [docs/stage3.md](docs/stage3.md) | 3단계 측정 계획과 합격 기준, 진행 상태 |
| [docs/game-plan.md](docs/game-plan.md) | 4단계 게임 기획 초안과 서버 영향 |
| [CLAUDE.md](CLAUDE.md) | 개발 규칙과 확인된 빌드·실행 명령 (작업 도구용 안내) |

## 빠른 시작 (Windows)

환경 준비(Visual Studio 2022, vcpkg)와 빌드 명령은 [CLAUDE.md](CLAUDE.md)의 "명령" 절을 따릅니다. 빌드 후:

```bash
build\windows-local\Debug\game_server.exe --port 7777
build\windows-local\Debug\ping_client.exe --port 7777 --count 5
build\windows-local\Debug\server_integration_test.exe
```

채팅은 터미널을 여러 개 열어 직접 체험할 수 있습니다.

```bash
build\windows-local\Debug\game_server.exe --port 7777
build\windows-local\Debug\chat_client.exe --port 7777 --nick alice
build\windows-local\Debug\chat_client.exe --port 7777 --nick bob
```

## 남은 작업

1. 3단계: 비교 실험(로그 레벨, Nagle, 스레드 수)을 한다([docs/stage3.md](docs/stage3.md)).
2. (보류) VM에서 Linux 빌드와 테스트를 확인하고 [docs/stage1.md](docs/stage1.md)를 갱신한다.

단계의 순서와 범위는 실험 결과에 따라 바꿀 수 있습니다.
