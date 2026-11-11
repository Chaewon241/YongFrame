#pragma once

// 게임 규칙의 기본값. docs/stage4.md "기본값" 표와 같은 값이며 한 곳에서만 바꾼다.
// 사용자가 정하지 않아 임의로 정한 값이다. 서버의 이동 검증과 클라이언트 연출이 같은 값을 써야 한다.

#include <cstddef>
#include <cstdint>

namespace game::play {

struct PlayConfig {
  // 맵 크기와 이동 단위는 센티미터(Unreal과 같음). 원점은 한쪽 모서리이고 장애물은 없다.
  float map_width_cm = 5000.0f;
  float map_height_cm = 5000.0f;

  float move_speed_cm_per_s = 300.0f;  // 방향 벡터 길이 1일 때의 속도
  float player_radius_cm = 40.0f;      // 맵 경계 처리에만 쓴다(플레이어끼리는 충돌하지 않는다)

  std::uint32_t max_players = 4;    // 한 방의 최대 인원
  std::uint32_t tick_rate_hz = 20;  // 초당 틱 수. 1 이상. 틱은 고정 시간 간격으로 계산한다.
  std::size_t max_rooms = 10000;    // 방 수 상한. 넘으면 만들 수 없다(메모리 보호)

  // 방 코드를 뽑아 이미 있는 코드와 겹치면 다시 뽑는 최대 횟수. 다 겹치면 서버가 바쁘다고 답한다.
  int room_code_attempts = 32;

  float TickSeconds() const { return 1.0f / static_cast<float>(tick_rate_hz == 0 ? 1 : tick_rate_hz); }
};

inline constexpr std::size_t kRoomCodeLength = 6;

}  // namespace game::play
