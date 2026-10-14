#pragma once

#include <cstdint>

namespace game::protocol {

// 메시지 ID와 .proto 메시지의 대응표는 이 파일에서만 관리한다. (docs/protocol.md)
enum class MessageId : std::uint16_t {
  kPingRequest = 1,
  kPingResponse = 2,
};

inline constexpr bool IsKnownMessageId(std::uint16_t id) {
  switch (static_cast<MessageId>(id)) {
    case MessageId::kPingRequest:
    case MessageId::kPingResponse:
      return true;
  }
  return false;
}

}  // namespace game::protocol
