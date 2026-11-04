#pragma once

// ChatClient: 채팅 방(ChatRoom)이 보는 "연결 하나"의 모습.
//
// 방은 소켓이나 Session을 직접 알 필요가 없고, 이 두 가지만 있으면 된다.
//  1. 연결을 구분하는 번호
//  2. 그 연결에 메시지를 보내는 방법
// Session이 이 인터페이스를 구현한다. 덕분에 방 로직을 소켓 없이 가짜 연결로 단위 테스트할 수 있다.

#include <cstdint>

#include <google/protobuf/message_lite.h>

#include "protocol/message_ids.h"

namespace game::chat {

class ChatClient {
 public:
  virtual ~ChatClient() = default;

  // 서버 안에서 연결을 구분하는 번호 (Session의 id와 같다).
  virtual std::uint64_t client_id() const = 0;

  // message를 직렬화해 프레임(길이 + ID + 본문)으로 만들어 이 연결의 송신 큐에 넣는다.
  // 연결이 이미 닫혔다면 아무 일도 하지 않는다. 호출한 쪽은 전송 성공 여부를 알 수 없다.
  virtual void Deliver(protocol::MessageId id, const google::protobuf::MessageLite& message) = 0;
};

}  // namespace game::chat
