# Cross-Platform Game Server Lab

Boost.Asio 기반 C++ 게임 서버를 작은 기능부터 만들고, 각 단계의 정확성과 성능을 재현 가능한 기록으로 남기는 개인 프로젝트입니다.

> 현재 단계: **1. 패킷 송수신**. Ping 서버, 테스트 클라이언트, 자동 테스트(52개)가 구현되어 Windows에서 통과했습니다. **Windows 기준으로 1단계를 마쳤고, Linux 확인은 보류 중입니다.** 자세한 결과는 [docs/stage1.md](docs/stage1.md)입니다.

## 문서 안내

| 문서 | 내용 |
| --- | --- |
| [project.md](project.md) | 목표, 기술 선택, 단계별 로드맵, 범위 |
| [docs/protocol.md](docs/protocol.md) | 1단계 TCP 패킷 형식과 오류 처리 계약 |
| [docs/validation.md](docs/validation.md) | 기능 검증, 부하 측정, 비교 실험 기록 방식 |
| [docs/stage1.md](docs/stage1.md) | 1단계 구성, 실행 방법, 검증 결과, 겪은 문제, 알려진 제한 |
| [CLAUDE.md](CLAUDE.md) | 개발 규칙과 확인된 빌드·실행 명령 (작업 도구용 안내) |

## 빠른 시작 (Windows)

환경 준비(Visual Studio 2022, vcpkg)와 빌드 명령은 [CLAUDE.md](CLAUDE.md)의 "명령" 절을 따릅니다. 빌드 후:

```bash
build\windows-local\Debug\game_server.exe --port 7777
build\windows-local\Debug\ping_client.exe --port 7777 --count 5
build\windows-local\Debug\server_integration_test.exe
```

## 남은 작업

1. 2단계(채팅과 MySQL)에 들어가기 전에 목표와 완료 기준을 재검토한다.
2. (보류) VM에서 Linux 빌드와 테스트를 확인하고 [docs/stage1.md](docs/stage1.md)를 갱신한다.

단계의 순서와 범위는 실험 결과에 따라 바꿀 수 있습니다.
