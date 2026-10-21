#pragma once

#include <cstdint>

namespace game::protocol {

// 메시지 ID와 .proto 메시지의 대응표는 이 파일에서만 관리한다. (docs/protocol.md)
enum class MessageId : std::uint16_t {
  kPingRequest = 1,
  kPingResponse = 2,
};

// enum class는 정수로 자동 변환되지 않으므로, 프레임의 message_id(uint16_t)와 비교할 때 쓴다.
inline constexpr std::uint16_t ToUint16(MessageId id) { return static_cast<std::uint16_t>(id); }

inline constexpr bool IsKnownMessageId(std::uint16_t id) {
  switch (static_cast<MessageId>(id)) {
    case MessageId::kPingRequest:
    case MessageId::kPingResponse:
      return true;
  }
  return false;
}

}  // namespace game::protocol
