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

  float move_speed_cm_per_s = 360.0f;  // 방향 벡터 길이 1일 때의 속도(이전 300, 조정함)
  float player_radius_cm = 40.0f;      // 맵 경계 처리에만 쓴다(플레이어끼리는 충돌하지 않는다)

  std::uint32_t max_players = 4;    // 한 방의 최대 인원
  std::uint32_t tick_rate_hz = 20;  // 초당 틱 수. 1 이상. 틱은 고정 시간 간격으로 계산한다.
  std::size_t max_rooms = 10000;    // 방 수 상한. 넘으면 만들 수 없다(메모리 보호)

  // 방 코드를 뽑아 이미 있는 코드와 겹치면 다시 뽑는 최대 횟수. 다 겹치면 서버가 바쁘다고 답한다.
  int room_code_attempts = 32;

  // ---- 자원 채집 (docs/stage4-gather.md). 임의 기본값 ----
  std::uint32_t tree_count = 30;
  std::uint32_t tree_hp = 5;  // 한 번 칠 때마다 1 감소
  std::uint32_t rock_count = 20;
  std::uint32_t rock_hp = 8;
  float gather_range_cm = 200.0f;            // 서버가 아는 플레이어 위치와 노드 중심의 거리(경계 포함)
  std::uint32_t gather_cooldown_ticks = 10;  // 플레이어별. 받아들여진 마지막 채집으로부터
  std::uint32_t respawn_ticks = 600;         // 고갈된 틱으로부터 이만큼 지나면 체력이 가득 차 돌아온다
  float node_min_gap_cm = 150.0f;            // 노드 사이 최소 거리
  float node_edge_margin_cm = 200.0f;        // 맵 가장자리에서 이만큼 안쪽에만 둔다
  float spawn_clear_radius_cm = 500.0f;      // 맵 중앙(시작 위치) 이 반지름 안에는 두지 않는다
  std::uint64_t layout_seed = 20240607;      // 배치 생성 시드. 같은 설정이면 모든 방이 같은 배치

  // ---- 집 짓기와 이동 충돌 (docs/content-roadmap.md). 임의 기본값 ----
  float house_width_cm = 300.0f;             // 집 크기(축에 정렬된 직사각형)
  float house_height_cm = 300.0f;
  std::uint32_t house_cost_wood = 20;        // 건설을 요청한 사람의 인벤토리에서 즉시 차감
  std::uint32_t house_cost_stone = 10;
  float build_grid_cm = 100.0f;              // 요청 좌표를 이 격자의 중심에 맞춘다
  float build_range_cm = 500.0f;             // 서버가 아는 내 위치에서 격자에 맞춘 중심까지(경계 포함)
  std::uint32_t build_ticks = 100;           // 건설 중 상태가 이어지는 틱 수(5초)
  std::uint32_t max_structures = 20;         // 방당 구조물 상한
  float node_collision_radius_cm = 60.0f;    // 나무/돌이 이동을 막는 원의 반지름(고갈되면 막지 않는다)

  // ---- 구역 분리 (docs/content-roadmap.md "구역 분리"). 임의 기본값 ----
  // 맵 중앙의 정사각형(한 변 = 이 값의 2배)이 건설 구역이다. 건물은 발자국 전체가 이 안에만 들어갈 수 있고, 자원 노드는
  // 이 구역(노드 충돌 반지름만큼 넓힌 것)의 밖에만 놓이고 재생한다. 그래서 건물이 아무리 늘어도 노드가 설 자리가 줄지 않는다.
  // 0이면 구역 제한이 없다(건물은 맵 어디든, 노드는 시작 영역만 피해 어디든).
  float build_zone_half_size_cm = 1200.0f;
  // 재생할 자리를 못 찾았을 때 다시 찾기까지 기다리는 틱 수(매 틱 찾지 않는다)
  std::uint32_t respawn_retry_ticks = 20;

  float TickSeconds() const { return 1.0f / static_cast<float>(tick_rate_hz == 0 ? 1 : tick_rate_hz); }
};

inline constexpr std::size_t kRoomCodeLength = 6;

}  // namespace game::play
