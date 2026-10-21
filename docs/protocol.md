# 1단계 패킷 계약 (초안)

> 상태: 구현 전 초안. 처음 구현에서 클라이언트와 서버가 함께 사용하는 계약으로 확정하고, 호환되지 않는 변경은 기록한다.

## 전송 형식

TCP는 메시지 경계를 보장하지 않으며 Protobuf 데이터도 자체적으로 여러 메시지의 끝을 구분하지 않는다. 따라서 다음 프레임을 사용한다.

```text
+-------------------------+--------------------+--------------------------+
| body_length: uint32 (4B) | message_id: uint16 | protobuf bytes           |
| network byte order       | network byte order | body_length에 포함       |
+-------------------------+--------------------+--------------------------+
```

- `body_length`는 뒤따르는 `message_id` 2바이트와 Protobuf 본문의 합계다. 헤더 4바이트는 제외한다.
- 허용 범위는 **2~65,536바이트**다. 범위를 벗어나면 서버가 해당 연결을 종료한다. 메모리 할당 전에 길이를 검사한다.
- 1단계 메시지 ID: `1 = PingRequest`, `2 = PingResponse`. 알 수 없는 ID는 기록 후 해당 연결을 종료한다.
- 서버가 요청으로 받아들이는 ID는 `1`뿐이다. 서버가 보내는 쪽인 `2 = PingResponse`를 클라이언트가 보내도 알 수 없는 ID와 똑같이 기록 후 연결을 종료한다.
- 메시지 ID와 `.proto` 메시지의 대응표는 한곳(`src/protocol/message_ids.h`)에서 관리한다.
- 송수신은 각 연결에서 순서대로 프레임을 처리한다. 비동기 쓰기에 전달한 버퍼는 쓰기 완료까지 살아 있어야 한다.

## Protobuf 메시지 초안

```proto
syntax = "proto3";
package game.protocol;

message PingRequest {
  uint64 request_id = 1;
  bytes payload = 2;
}

message PingResponse {
  uint64 request_id = 1;
  bytes payload = 2;
}
```

서버는 `request_id`와 `payload`를 그대로 응답한다. 클라이언트는 둘 다 확인한다. 첫 단계는 왕복 검증에 집중하며, `request_id`의 중복 제거·재전송 의미는 부여하지 않는다.

## 읽기·쓰기 처리

1. 헤더 4바이트를 모두 읽는다.
2. `body_length`를 검증한 후 그 크기만큼 본문을 모두 읽는다.
3. 메시지 ID를 확인하고 나머지를 해당 Protobuf 타입으로 파싱한다.
4. 응답을 직렬화해 연결별 송신 큐에 넣고, 앞선 쓰기가 끝난 뒤 다음 쓰기를 시작한다.
5. 읽기 오류, 파싱 실패, 쓰기 오류, 연결 종료는 연결 단위로 정리하고 이유를 기록한다.

송신 큐에 쌓인(아직 소켓에 쓰지 못한) 바이트가 연결당 한도(기본 4 MiB, `--max-send-queue-bytes`)를 넘으면 그 연결을 종료한다. 응답을 읽지 않는 클라이언트 때문에 서버 메모리가 계속 늘지 않게 하는 1단계의 안전장치이며, 제한 큐와 무제한 큐의 비교는 3단계 실험 후보다.

메시지 본문이 분할되거나 여러 프레임이 합쳐져 도착해도 같은 결과가 나와야 한다. 비정상 Protobuf는 해당 연결을 종료한다. 에러 응답 프로토콜은 추후 필요할 때 추가한다.

## 1단계 검증 입력

| 입력 | 기대 결과 |
| --- | --- |
| 정상 Ping 1건 | 동일한 ID·payload의 `PingResponse` 1건 |
| 한 프레임을 여러 번 나눠 전송 | 정상 응답 1건 |
| 여러 프레임을 한 번에 전송 | 각 요청에 대한 순서 있는 응답 |
| 길이 0~1 또는 65,536 초과 | 연결 종료, 서버 계속 실행 |
| 알 수 없는 ID | 연결 종료, 오류 로그 |
| 잘못된 Protobuf | 연결 종료, 오류 로그 |
| 헤더/본문 중간에 연결 종료 | 연결 정리, 서버 계속 실행 |

참고: [Boost.Asio 스트림 처리](https://www.boost.org/doc/libs/latest/doc/html/boost_asio/overview/core/streams.html), [Protobuf 메시지 스트리밍](https://protobuf.dev/programming-guides/techniques/#streaming-multiple-messages).
