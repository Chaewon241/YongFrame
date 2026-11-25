#pragma once

// 게임 통합 테스트가 공유하는 클라이언트 쪽 도우미.
//
// 같은 방에 있는 연결에는 응답 사이에 스냅샷(WorldSnapshot, 초당 20개)과 방 알림(RoomEvent)이 끼어든다.
// 그래서 Receive()가 기다리는 종류가 아닌 것은 "건너뛸 종류"로 지정해 읽고 버린다.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "chat_test_client.h"
#include "game.pb.h"

namespace game::testing_support {

// 응답을 기다리는 동안 건너뛰는 종류.
// 채팅에도 입장한 연결이면 채팅 전파(ChatBroadcast)와 입장 알림(PresenceNotice)도 끼어든다.
inline const std::vector<MessageId> kGameSkip = {MessageId::kWorldSnapshot, MessageId::kRoomEvent,
                                                  MessageId::kResourceUpdate, MessageId::kStructureUpdate,
                                                  MessageId::kSleepUpdate,    MessageId::kDayStarted,
                                                  MessageId::kPresenceNotice, MessageId::kChatBroadcast};

inline protocol::CreateRoomResponse CreateRoom(BlockingClient& c, const std::string& nickname) {
  protocol::CreateRoomRequest request;
  request.set_nickname(nickname);
  protocol::CreateRoomResponse response;
  response.set_result(static_cast<protocol::GameResult>(-1));
  if (!SendProto(c, MessageId::kCreateRoomRequest, request) ||
      !Receive(c, MessageId::kCreateRoomResponse, response, kGameSkip)) {
    ADD_FAILURE() << "방 만들기 요청/응답 실패: " << c.last_error();
  }
  return response;
}

inline protocol::JoinRoomResponse JoinRoom(BlockingClient& c, const std::string& code, const std::string& nickname) {
  protocol::JoinRoomRequest request;
  request.set_room_code(code);
  request.set_nickname(nickname);
  protocol::JoinRoomResponse response;
  response.set_result(static_cast<protocol::GameResult>(-1));
  if (!SendProto(c, MessageId::kJoinRoomRequest, request) ||
      !Receive(c, MessageId::kJoinRoomResponse, response, kGameSkip)) {
    ADD_FAILURE() << "방 입장 요청/응답 실패: " << c.last_error();
  }
  return response;
}

inline protocol::LeaveRoomResponse LeaveRoom(BlockingClient& c) {
  protocol::LeaveRoomRequest request;
  protocol::LeaveRoomResponse response;
  response.set_result(static_cast<protocol::GameResult>(-1));
  if (!SendProto(c, MessageId::kLeaveRoomRequest, request) ||
      !Receive(c, MessageId::kLeaveRoomResponse, response, kGameSkip)) {
    ADD_FAILURE() << "방 나가기 요청/응답 실패: " << c.last_error();
  }
  return response;
}

// 방에 들어가 있는 연결에서도 쓸 수 있는 Ping 확인(스냅샷과 방 알림은 건너뛴다).
inline ::testing::AssertionResult PingWorksInGame(BlockingClient& c, std::uint64_t request_id = 4343) {
  if (!c.SendPing(request_id, "ping")) {
    return ::testing::AssertionFailure() << "Ping 전송 실패: " << c.last_error();
  }
  protocol::PingResponse response;
  const auto received = Receive(c, MessageId::kPingResponse, response, kGameSkip);
  if (!received) return received;
  if (response.request_id() != request_id || response.payload() != "ping") {
    return ::testing::AssertionFailure() << "Ping 응답 내용이 다름";
  }
  return ::testing::AssertionSuccess();
}

inline bool SendMove(BlockingClient& c, std::uint32_t sequence, float dir_x, float dir_y) {
  protocol::MoveInput input;
  input.set_sequence(sequence);
  input.set_dir_x(dir_x);
  input.set_dir_y(dir_y);
  return SendProto(c, MessageId::kMoveInput, input);
}

// 스냅샷을 계속 받다가 조건(predicate)을 만족하는 것이 나오면 out에 담고 true. 제한 시간이 지나면 false.
// 조건에 맞지 않는 스냅샷, 방 알림은 버린다.
inline bool WaitSnapshot(BlockingClient& c, protocol::WorldSnapshot& out,
                         const std::function<bool(const protocol::WorldSnapshot&)>& predicate,
                         Millis timeout = Millis(3000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    protocol::Frame frame;
    const Status status = c.ReceiveFrame(frame, Millis(500));
    if (status == Status::kTimeout) continue;
    if (status != Status::kFrame) return false;
    if (frame.message_id != protocol::ToUint16(MessageId::kWorldSnapshot)) continue;
    protocol::WorldSnapshot snapshot;
    if (!snapshot.ParseFromArray(frame.payload.data(), static_cast<int>(frame.payload.size()))) return false;
    if (predicate(snapshot)) {
      out = snapshot;
      return true;
    }
  }
  return false;
}

// 방 알림(RoomEvent) 중 kind가 같은 것이 올 때까지 기다린다. 스냅샷은 버린다.
inline bool WaitEvent(BlockingClient& c, protocol::RoomEvent::Kind kind, protocol::RoomEvent& out,
                      Millis timeout = Millis(3000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    protocol::Frame frame;
    const Status status = c.ReceiveFrame(frame, Millis(500));
    if (status == Status::kTimeout) continue;
    if (status != Status::kFrame) return false;
    if (frame.message_id != protocol::ToUint16(MessageId::kRoomEvent)) continue;
    protocol::RoomEvent event;
    if (!event.ParseFromArray(frame.payload.data(), static_cast<int>(frame.payload.size()))) return false;
    if (event.kind() == kind) {
      out = event;
      return true;
    }
  }
  return false;
}

// 자원 노드를 한 번 친다. 응답(결과 코드와 내 인벤토리)을 돌려준다.
inline protocol::GatherResponse Gather(BlockingClient& c, std::uint32_t node_id) {
  protocol::GatherRequest request;
  request.set_node_id(node_id);
  protocol::GatherResponse response;
  response.set_result(static_cast<protocol::GameResult>(-1));
  if (!SendProto(c, MessageId::kGatherRequest, request) ||
      !Receive(c, MessageId::kGatherResponse, response, kGameSkip)) {
    ADD_FAILURE() << "채집 요청/응답 실패: " << c.last_error();
  }
  return response;
}

// ResourceUpdate 중 조건을 만족하는 것이 올 때까지 기다린다. 다른 메시지는 버린다.
inline bool WaitResourceUpdate(BlockingClient& c, protocol::ResourceUpdate& out,
                               const std::function<bool(const protocol::ResourceUpdate&)>& predicate,
                               Millis timeout = Millis(3000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    protocol::Frame frame;
    const Status status = c.ReceiveFrame(frame, Millis(500));
    if (status == Status::kTimeout) continue;
    if (status != Status::kFrame) return false;
    if (frame.message_id != protocol::ToUint16(MessageId::kResourceUpdate)) continue;
    protocol::ResourceUpdate update;
    if (!update.ParseFromArray(frame.payload.data(), static_cast<int>(frame.payload.size()))) return false;
    if (predicate(update)) {
      out = update;
      return true;
    }
  }
  return false;
}

// 건물을 짓는다. 응답(결과 코드와 내 인벤토리)을 돌려준다.
inline protocol::BuildResponse Build(BlockingClient& c, float x, float y, std::uint32_t type = 0) {
  protocol::BuildRequest request;
  request.set_structure_type(static_cast<protocol::StructureType>(type));
  request.set_x(x);
  request.set_y(y);
  protocol::BuildResponse response;
  response.set_result(static_cast<protocol::GameResult>(-1));
  if (!SendProto(c, MessageId::kBuildRequest, request) ||
      !Receive(c, MessageId::kBuildResponse, response, kGameSkip)) {
    ADD_FAILURE() << "건설 요청/응답 실패: " << c.last_error();
  }
  return response;
}

// StructureUpdate 중 조건을 만족하는 것이 올 때까지 기다린다. 다른 메시지는 버린다.
inline bool WaitStructureUpdate(BlockingClient& c, protocol::StructureUpdate& out,
                                const std::function<bool(const protocol::StructureUpdate&)>& predicate,
                                Millis timeout = Millis(3000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    protocol::Frame frame;
    const Status status = c.ReceiveFrame(frame, Millis(500));
    if (status == Status::kTimeout) continue;
    if (status != Status::kFrame) return false;
    if (frame.message_id != protocol::ToUint16(MessageId::kStructureUpdate)) continue;
    protocol::StructureUpdate update;
    if (!update.ParseFromArray(frame.payload.data(), static_cast<int>(frame.payload.size()))) return false;
    if (predicate(update)) {
      out = update;
      return true;
    }
  }
  return false;
}

// 잔다(true) 또는 깬다(false). 응답(결과 코드와 내 잠 상태)을 돌려준다. docs/content-day.md
inline protocol::SleepResponse Sleep(BlockingClient& c, bool sleep) {
  protocol::SleepRequest request;
  request.set_sleep(sleep);
  protocol::SleepResponse response;
  response.set_result(static_cast<protocol::GameResult>(-1));
  if (!SendProto(c, MessageId::kSleepRequest, request) || !Receive(c, MessageId::kSleepResponse, response, kGameSkip)) {
    ADD_FAILURE() << "잠 요청/응답 실패: " << c.last_error();
  }
  return response;
}

// id 종류의 메시지 중 조건을 만족하는 것이 올 때까지 기다린다. 다른 메시지는 버린다. seen이 있으면 받은 모든 ID를
// 순서대로 적는다(알림 순서 시험용).
template <typename T>
bool WaitMessage(BlockingClient& c, MessageId id, T& out, const std::function<bool(const T&)>& predicate,
                 Millis timeout = Millis(3000), std::vector<std::uint16_t>* seen = nullptr) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    protocol::Frame frame;
    const Status status = c.ReceiveFrame(frame, Millis(500));
    if (status == Status::kTimeout) continue;
    if (status != Status::kFrame) return false;
    if (seen != nullptr) seen->push_back(frame.message_id);
    if (frame.message_id != protocol::ToUint16(id)) continue;
    T message;
    if (!message.ParseFromArray(frame.payload.data(), static_cast<int>(frame.payload.size()))) return false;
    if (predicate(message)) {
      out = message;
      return true;
    }
  }
  return false;
}

// 스냅샷에서 player_id의 상태를 찾는다. 없으면 nullptr.
inline const protocol::PlayerState* FindPlayer(const protocol::WorldSnapshot& snapshot, std::uint32_t player_id) {
  for (const auto& player : snapshot.players()) {
    if (player.player_id() == player_id) return &player;
  }
  return nullptr;
}

}  // namespace game::testing_support
