# 2단계: 채팅과 MySQL (진행 기록)

> 상태: **진행 중.** 2-0(드라이버 빌드 확인)과 2-1(프로토콜 초안)을 마쳤고, MySQL 서버 설치를 기다린다. 목표와 완료 기준은 [project.md](../project.md#2단계-채팅과-mysql), 프로토콜은 [protocol-chat.md](protocol-chat.md).

## 목표와 완료 기준

여러 클라이언트가 같은 방에서 메시지를 주고받고, 필요한 대화 기록을 MySQL에 저장·조회한다.

완료 기준: 다중 사용자 전파, 재접속 후 기록 조회, DB 중단·복구 시나리오를 확인한다. 처음에는 단일 서버·단일 방이다.

## 결정 기록

| 결정 | 이유 | 검토한 대안 |
| --- | --- | --- |
| MySQL 드라이버: **Boost.MySQL** (`boost-mysql`) 먼저 시도 | Asio 비동기를 그대로 써서 DB 호출이 `io_context` 안에서 논블로킹이다. 스레드를 새로 만들지 않아 1단계의 단일 스레드 전제가 유지된다. | MySQL Connector/C++ (동기식이라 DB 전용 워커 스레드와 결과 전달 설계가 필요, 이 단계에서 처음 스레드가 등장) |
| 저장 실패 정책: **저장 성공 후 전파**, 실패 시 보낸 사람에게 오류 응답 | 기록에 없는 메시지는 존재하지 않는다는 일관성. 구현이 단순하다. | 먼저 전파하고 비동기 저장 (빠르지만 실시간으로 본 메시지가 기록에 없을 수 있음) |
| 방 단위로 저장을 직렬화 | 같은 방의 전파 순서를 `message_id` 순서와 일치시킨다. | 병렬 저장 (처리량은 높지만 순서가 뒤바뀔 수 있음) |
| 업무 규칙 위반은 오류 코드 응답 + 연결 유지, 프로토콜 위반은 연결 종료 | 닉네임 중복 같은 일상적 오류로 연결을 끊을 이유가 없다. | 모든 오류에서 연결 종료 (1단계 방식) |
| MySQL 서버는 Windows에 직접 설치 | 이 PC에 Docker가 없다. | Docker Desktop 설치 |

드라이버 결정은 **접속 확인 후 확정**한다. Windows에서 Boost.MySQL의 빌드는 확인했지만(OpenSSL 3.6.5 포함), 서버에 실제로 접속해 질의하는 것은 MySQL 설치 후에야 확인할 수 있다. 접속이 안 되면 Connector/C++로 바꾸고 이 표를 고친다. 두 방식의 비교(DB 호출 중 다른 연결의 지연)는 3단계 실험 후보다.

## 작업 순서와 상태

| 순서 | 내용 | 상태 |
| --- | --- | --- |
| 2-0 | MySQL 준비, 드라이버 빌드 확인 도구(`mysql_check`) | 드라이버 빌드는 확인됨(Windows, Debug/Release, 경고 없음). **실제 접속은 미확인: MySQL 서버 설치가 필요(사용자 작업)** |
| 2-1 | 채팅 프로토콜 문서 | [초안 작성됨](protocol-chat.md) |
| 2-2 | DB 없이 인메모리로 방, 입장/퇴장, 전파 구현 | 대기 |
| 2-3 | `ChatRepository` 인터페이스 뒤에 MySQL 연결, 스키마와 마이그레이션 | 대기 |
| 2-4 | 기록 조회, DB 중단·복구 처리 | 대기 |
| 2-5 | 통합 테스트와 결과 문서화 | 대기 |

설계 원칙: 채팅 로직은 `ChatRepository`(저장소 인터페이스)에만 의존한다. 자동 테스트는 가짜 저장소로 대부분 검증하고, 실제 MySQL을 쓰는 테스트는 별도로 분리해 서버가 없으면 건너뛴다. 그래야 DB 없이도 테스트가 돌고, "DB 중단" 시나리오도 가짜 저장소에 실패를 주입해 재현할 수 있다.

## MySQL 서버 준비 (직접 설치)

이 PC에는 MySQL과 Docker가 없다. 설치 프로그램 실행, 라이선스 동의, 비밀번호 설정이 필요해서 직접 진행해야 한다.

1. https://dev.mysql.com/downloads/mysql/ 에서 **MySQL Community Server**(LTS 계열 권장)의 Windows MSI 설치 파일을 받아 설치한다.
2. 설치 중 포트는 3306, Windows 서비스로 실행, root 비밀번호를 정한다. (개발 전용 PC의 개발용 서버임을 전제한다.)
3. 서비스가 켜져 있는지 확인한 뒤, 이 프로젝트 전용 데이터베이스와 계정을 만든다. root로 채팅 서버를 돌리지 않는다.

```sql
CREATE DATABASE game_chat CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci;
CREATE USER 'game'@'localhost' IDENTIFIED BY '여기에_비밀번호';
GRANT ALL PRIVILEGES ON game_chat.* TO 'game'@'localhost';
```

4. 접속을 확인한다. 비밀번호는 환경변수로 전달하며 명령줄이나 파일에 적지 않는다.

```bash
$env:GAME_DB_PASSWORD = '여기에_비밀번호'
.\build\windows-local\Debug\mysql_check.exe --user game --db game_chat
```

성공하면 서버 버전, 문자 집합, 정렬 규칙이 출력된다.

## 열린 문제

- 저장 요청이 시간 초과로 실패했지만 실제로는 DB에 반영된 경우의 처리([protocol-chat.md](protocol-chat.md)의 "알려진 불확실성").
- DB 연결을 1개로 쓸지 풀을 둘지. 처음에는 연결 1개로 시작하고, 병목이 관측되면 3단계에서 비교한다.
