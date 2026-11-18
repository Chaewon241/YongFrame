#pragma once

#include <cstdint>

namespace game::protocol {

// 메시지 ID와 .proto 메시지의 대응표는 이 파일에서만 관리한다. (docs/protocol.md, protocol-chat.md)
enum class MessageId : std::uint16_t {
  // 1단계
  kPingRequest = 1,
  kPingResponse = 2,

  // 2단계 채팅
  kJoinRequest = 10,
  kJoinResponse = 11,
  kLeaveRequest = 12,
  kLeaveResponse = 13,
  kChatRequest = 20,
  kChatResponse = 21,
  kChatBroadcast = 22,
  kHistoryRequest = 30,
  kHistoryResponse = 31,
  kPresenceNotice = 40,

  // 4단계 게임 (docs/stage4.md)
  kCreateRoomRequest = 100,
  kCreateRoomResponse = 101,
  kJoinRoomRequest = 102,
  kJoinRoomResponse = 103,
  kLeaveRoomRequest = 104,
  kLeaveRoomResponse = 105,
  kMoveInput = 106,  // 응답 없음
  kRoomEvent = 110,
  kWorldSnapshot = 111,

  // 4단계 확장: 자원 채집 (docs/stage4-gather.md)
  kGatherRequest = 107,
  kGatherResponse = 108,
  kResourceUpdate = 112,

  // 4단계 콘텐츠: 집 짓기 (docs/content-roadmap.md)
  kBuildRequest = 113,
  kBuildResponse = 114,
  kStructureUpdate = 115,
};

// enum class는 정수로 자동 변환되지 않으므로, 프레임의 message_id(uint16_t)와 비교할 때 쓴다.
inline constexpr std::uint16_t ToUint16(MessageId id) { return static_cast<std::uint16_t>(id); }

// 정의된 ID인가. (클라이언트가 보내는 것인지 서버가 보내는 것인지는 구분하지 않는다)
inline constexpr bool IsKnownMessageId(std::uint16_t id) {
  switch (static_cast<MessageId>(id)) {
    case MessageId::kPingRequest:
    case MessageId::kPingResponse:
    case MessageId::kJoinRequest:
    case MessageId::kJoinResponse:
    case MessageId::kLeaveRequest:
    case MessageId::kLeaveResponse:
    case MessageId::kChatRequest:
    case MessageId::kChatResponse:
    case MessageId::kChatBroadcast:
    case MessageId::kHistoryRequest:
    case MessageId::kHistoryResponse:
    case MessageId::kPresenceNotice:
    case MessageId::kCreateRoomRequest:
    case MessageId::kCreateRoomResponse:
    case MessageId::kJoinRoomRequest:
    case MessageId::kJoinRoomResponse:
    case MessageId::kLeaveRoomRequest:
    case MessageId::kLeaveRoomResponse:
    case MessageId::kMoveInput:
    case MessageId::kRoomEvent:
    case MessageId::kWorldSnapshot:
    case MessageId::kGatherRequest:
    case MessageId::kGatherResponse:
    case MessageId::kResourceUpdate:
    case MessageId::kBuildRequest:
    case MessageId::kBuildResponse:
    case MessageId::kStructureUpdate:
      return true;
  }
  return false;
}

// 서버가 클라이언트에게서 "요청으로" 받아들이는 ID인가. 이 밖의 ID를 클라이언트가 보내면
// (서버가 보내는 쪽 메시지 포함) 알 수 없는 ID와 똑같이 연결을 종료한다.
inline constexpr bool IsClientRequestId(std::uint16_t id) {
  switch (static_cast<MessageId>(id)) {
    case MessageId::kPingRequest:
    case MessageId::kJoinRequest:
    case MessageId::kLeaveRequest:
    case MessageId::kChatRequest:
    case MessageId::kHistoryRequest:
    case MessageId::kCreateRoomRequest:
    case MessageId::kJoinRoomRequest:
    case MessageId::kLeaveRoomRequest:
    case MessageId::kMoveInput:
    case MessageId::kGatherRequest:
    case MessageId::kBuildRequest:
      return true;
    default:
      return false;
  }
}

}  // namespace game::protocol
