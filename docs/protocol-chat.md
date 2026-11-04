# 2단계 채팅 프로토콜 (초안)

> 상태: **구현 전 초안.** 2-2(인메모리 채팅) 구현을 시작하면서 확정하고, 호환되지 않는 변경은 기록한다.
> 1단계 계약([protocol.md](protocol.md))의 프레임 형식과 연결 종료 규칙은 **그대로** 쓴다. 이 문서는 메시지와 상태 규칙을 더한다.

## 1단계에서 달라지는 점

1. **오류를 두 종류로 나눈다.**
   - **프로토콜 위반**: 1단계와 같다. 잘못된 프레임 길이, 서버가 받지 않는 메시지 ID, 파싱 실패(잘못된 UTF-8 포함)는 기록 후 **연결을 종료**한다.
   - **업무 규칙 위반**: 닉네임 중복, 너무 긴 메시지, 입장하지 않고 채팅 같은 경우는 `ResultCode`가 담긴 응답을 돌려주고 **연결을 유지**한다.
2. **연결이 상태를 가진다.** `미입장`과 `입장함` 두 상태가 있고, 요청마다 허용되는 상태가 다르다.
3. **연결 사이에 상호작용이 생긴다.** 한 사람의 메시지가 방의 모든 입장자에게 전달된다.

## 메시지 ID

| ID | 메시지 | 방향 | 설명 |
| --- | --- | --- | --- |
| 1 | `PingRequest` | 클라이언트 → 서버 | 1단계와 같음 |
| 2 | `PingResponse` | 서버 → 클라이언트 | 1단계와 같음 |
| 10 | `JoinRequest` | 클라이언트 → 서버 | 닉네임을 정해 방에 입장 |
| 11 | `JoinResponse` | 서버 → 클라이언트 | 입장 결과와 현재 접속자 |
| 12 | `LeaveRequest` | 클라이언트 → 서버 | 방에서 퇴장 (연결은 유지) |
| 13 | `LeaveResponse` | 서버 → 클라이언트 | 퇴장 결과 |
| 20 | `ChatRequest` | 클라이언트 → 서버 | 채팅 메시지 전송 |
| 21 | `ChatResponse` | 서버 → 클라이언트 | 보낸 사람에게 전송 결과 |
| 22 | `ChatBroadcast` | 서버 → 클라이언트 | 방의 모든 입장자에게 전파 |
| 30 | `HistoryRequest` | 클라이언트 → 서버 | 최근 기록 조회 |
| 31 | `HistoryResponse` | 서버 → 클라이언트 | 기록 목록 |
| 40 | `PresenceNotice` | 서버 → 클라이언트 | 다른 사람의 입장/퇴장 알림 |

서버가 요청으로 받는 ID는 **1, 10, 12, 20, 30**뿐이다. 나머지 ID를 클라이언트가 보내면 알 수 없는 ID와 똑같이 기록 후 연결을 종료한다. ID 대응표는 `src/protocol/message_ids.h` 한곳에서 관리한다.

## Protobuf 초안

```proto
syntax = "proto3";
package game.protocol;

enum ResultCode {
  RESULT_OK = 0;
  RESULT_INVALID_NICKNAME = 1;   // 닉네임 규칙 위반
  RESULT_NICKNAME_TAKEN = 2;     // 방에 같은 닉네임이 이미 있음
  RESULT_ALREADY_JOINED = 3;     // 이미 입장한 연결이 Join
  RESULT_NOT_JOINED = 4;         // 입장하지 않은 연결이 Leave/Chat/History
  RESULT_EMPTY_MESSAGE = 5;      // 메시지가 비었거나 공백뿐
  RESULT_MESSAGE_TOO_LONG = 6;   // 메시지가 1,000바이트 초과
  RESULT_INVALID_ARGUMENT = 7;   // 제어 문자 포함, limit 범위 초과 등
  RESULT_STORAGE_FAILURE = 8;    // DB 저장/조회 실패 또는 시간 초과
  RESULT_SERVER_BUSY = 9;        // 방의 저장 대기열이 가득 참
}

message JoinRequest   { string nickname = 1; }
message JoinResponse  { ResultCode result = 1; repeated string members = 2; }  // members: 성공 시 현재 접속자(자신 포함)
message LeaveRequest  {}
message LeaveResponse { ResultCode result = 1; }

message ChatEntry {
  uint64 message_id = 1;        // DB가 부여한 번호. 방에서 항상 증가한다.
  string nickname = 2;
  string text = 3;
  int64  sent_at_unix_ms = 4;   // 저장된 시각 (UTC, 밀리초)
}

message ChatRequest   { string text = 1; }
message ChatResponse  { ResultCode result = 1; uint64 message_id = 2; }  // message_id는 성공 시에만 의미 있음
message ChatBroadcast { ChatEntry entry = 1; }

message HistoryRequest  { uint32 limit = 1; }                              // 0이면 기본 20, 최대 50
message HistoryResponse { ResultCode result = 1; repeated ChatEntry entries = 2; }  // 오래된 것부터

message PresenceNotice {
  enum Kind { KIND_JOINED = 0; KIND_LEFT = 1; }
  Kind kind = 1;
  string nickname = 2;
}
```

`HistoryResponse`가 가장 크다. 항목 하나가 최대 약 1.1 KB(본문 1,000바이트 + 닉네임 최대 64바이트 + 필드 오버헤드)이므로 50개면 약 55 KB로, 프레임 상한(payload 65,534바이트) 안에 들어온다. 서버는 인코딩한 크기를 확인하고, 넘으면 항목 수를 줄여 보낸다.

## 입력 규칙

| 항목 | 규칙 | 위반 시 |
| --- | --- | --- |
| 닉네임 | 1~16 문자(유니코드 코드포인트), 64바이트 이하, 앞뒤 공백 금지, 제어 문자 금지, 대소문자 구분 | `INVALID_NICKNAME` |
| 닉네임 중복 | 방에 같은 닉네임이 접속 중이면 거부 (퇴장하면 바로 재사용 가능) | `NICKNAME_TAKEN` |
| 메시지 본문 | 1~1,000바이트(UTF-8). 공백만 있으면 빈 메시지로 취급 | `EMPTY_MESSAGE` / `MESSAGE_TOO_LONG` |
| 메시지 제어 문자 | 개행과 탭을 포함한 모든 제어 문자 금지 | `INVALID_ARGUMENT` |
| 기록 `limit` | 0(기본 20) 또는 1~50 | `INVALID_ARGUMENT` |
| 잘못된 UTF-8 | Protobuf `string` 파싱이 실패하므로 프로토콜 위반 | 연결 종료 |

모든 검증은 서버에서 한다. 클라이언트가 보낸 값은 신뢰하지 않는다. 닉네임 중복 검사는 서버 메모리의 방 상태로 하며 DB에 저장하지 않는다(단일 서버, 단일 방).

## 연결 상태와 허용 요청

| 요청 | 미입장 | 입장함 |
| --- | --- | --- |
| `PingRequest` | 응답 | 응답 |
| `JoinRequest` | 검증 후 입장 → 입장함 | `ALREADY_JOINED` |
| `LeaveRequest` | `NOT_JOINED` | 퇴장 → 미입장 |
| `ChatRequest` | `NOT_JOINED` | 아래 "채팅 처리" |
| `HistoryRequest` | `NOT_JOINED` | 아래 "기록 조회" |

- 입장하면 방의 다른 입장자에게 `PresenceNotice(KIND_JOINED)`가 간다. 퇴장도 같다(`KIND_LEFT`). 알림은 본인에게는 가지 않는다.
- 입장한 채로 연결이 끊기면 자동으로 퇴장 처리하고 `KIND_LEFT`를 보낸다.
- 입장 이전에 오간 메시지는 실시간으로 받지 못한다. 필요하면 `HistoryRequest`로 조회한다.

## 채팅 처리: 저장에 성공한 뒤 전파

결정: **기록에 없는 메시지는 존재하지 않는다.** 저장이 성공한 메시지만 전파한다. (근거와 대안은 [stage2.md](stage2.md))

1. 입력을 검증한다. 실패하면 `ChatResponse(오류)`만 보내고 끝낸다. 저장도 전파도 없다.
2. 방의 **저장 대기열**에 넣는다. 방마다 저장은 **한 번에 하나씩, 도착한 순서대로** 한다. 대기열이 가득 차면(기본 256개) `SERVER_BUSY`.
3. 저장에 성공하면 DB가 부여한 `message_id`와 시각으로 `ChatResponse(OK, message_id)`를 보낸 사람에게 보내고, 이어서 `ChatBroadcast`를 방의 모든 입장자(보낸 사람 포함)에게 보낸다.
4. 저장에 실패하거나 제한 시간(기본 3초)이 지나면 `ChatResponse(STORAGE_FAILURE)`만 보낸다. 전파도 없다. 연결은 유지된다.

**순서 보장:** 같은 방의 입장자는 `ChatBroadcast`를 `message_id`가 증가하는 순서로 받는다. 방 단위로 저장을 직렬화하기 때문에 성립한다. 서로 다른 방은 병렬로 저장할 수 있다(지금은 방이 하나).

**보낸 사람이 저장 도중 연결을 끊으면** 저장과 전파는 그대로 완료되고 응답만 보내지 않는다.

**알려진 불확실성:** 저장 요청이 제한 시간을 넘겨 `STORAGE_FAILURE`로 응답했는데 실제로는 DB에 반영됐을 수 있다. 이 경우 그 메시지는 이후 `HistoryRequest`에 나타난다(전파는 되지 않았음). 이 불확실성은 제거하지 않고 문서화만 한다.

## 기록 조회

`HistoryRequest`는 DB에서 최근 `limit`개를 읽어 **오래된 것부터** 돌려준다. 저장 대기열을 거치지 않는다. 실패하면 `STORAGE_FAILURE`와 빈 목록을 보낸다. 재접속한 사용자가 입장 후 이 요청으로 지난 대화를 복구하는 것이 "재접속 후 기록 조회" 완료 기준이다.

## DB 장애 중 서버 동작

| 기능 | DB가 내려갔을 때 |
| --- | --- |
| 서버 프로세스, 연결, `Ping` | 계속 동작 |
| 입장/퇴장/방 상태/알림 | 계속 동작 (DB가 필요 없음) |
| `ChatRequest`, `HistoryRequest` | 제한 시간 안에 `STORAGE_FAILURE`로 응답, 연결 유지 |
| 복구 | 서버 재시작 없이, 다음 요청에서 재연결을 시도하고 성공하면 정상 동작 |

DB가 내려가 있는 동안 요청이 쌓여 서버 메모리가 늘지 않도록, 저장 대기열 한도와 제한 시간으로 빠르게 실패시킨다.

## 느린 수신자

`ChatBroadcast`는 각 수신자의 송신 큐에 들어간다. 큐 한도를 넘는 수신자는 1단계 규칙대로 **그 사람의 연결만** 종료한다. 한 사람이 느려도 다른 입장자의 전파는 영향을 받지 않아야 한다.

## 2단계 검증 입력

| 입력 | 기대 결과 |
| --- | --- |
| 두 명이 입장 후 한 명이 채팅 | 둘 다 같은 `ChatBroadcast`를 받음, 보낸 사람은 `ChatResponse(OK)`도 받음 |
| 입장 알림 | 먼저 입장해 있던 사람만 `KIND_JOINED`를 받음 |
| 닉네임 중복 / 규칙 위반 | `NICKNAME_TAKEN` / `INVALID_NICKNAME`, 연결 유지 |
| 입장 전 채팅, 이중 입장 | `NOT_JOINED` / `ALREADY_JOINED`, 연결 유지 |
| 빈 메시지, 1,001바이트, 제어 문자 | 각각 `EMPTY_MESSAGE` / `MESSAGE_TOO_LONG` / `INVALID_ARGUMENT` |
| 같은 방 여러 명이 동시에 채팅 | 모든 입장자가 같은 순서(`message_id` 증가)로 수신 |
| 퇴장 / 연결 끊김 | 남은 사람에게 `KIND_LEFT`, 닉네임 재사용 가능 |
| 재접속 후 `HistoryRequest` | 이전 메시지가 오래된 순으로 복구됨 |
| 응답을 읽지 않는 입장자 | 그 연결만 종료, 다른 입장자는 정상 수신 |
| DB 중단 중 채팅/기록 조회 | `STORAGE_FAILURE`, 연결 유지, `Ping` 정상 |
| DB 복구 후 채팅 | 서버 재시작 없이 정상 저장·전파 |
| 잘못된 UTF-8 닉네임/본문 | 연결 종료 (프로토콜 위반) |

## 범위 밖 (이후 필요할 때)

인증, 다중 방, 개인 메시지, 메시지 수정·삭제, 기록 보존 정책, 요청 빈도 제한, 닉네임 영속화.
