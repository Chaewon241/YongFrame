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
  std::uint32_t gather_cooldown_ticks = 10;  // 플레이어별. 받아들여진 마지막 채집으로부터(틱)
  // 고갈된 노드는 밤 정산에 되살아난다(docs/content-day.md 5.4). 예전의 틱 기준 재생(respawn_ticks)은 없앴다.
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
  // 지은 집은 다음 밤 정산에 완공된다(docs/content-day.md 5.4). 예전의 틱 기준 완공(build_ticks)은 없앴다.
  std::uint32_t max_structures = 20;         // 방당 구조물 상한

  // ---- 농장 저장 (docs/persistence.md). 게임 규칙이 아니라 저장 방식의 기본값이다 ----
  // 채집처럼 자주 일어나는 변경을 모아서 DB에 쓰는 주기(게임 틱 수). 20Hz에서 200틱 = 10초. 0이면 주기 저장을 하지 않는다.
  std::uint64_t farm_flush_interval_ticks = 200;
  float node_collision_radius_cm = 60.0f;    // 나무/돌이 이동을 막는 원의 반지름(고갈되면 막지 않는다)

  // ---- 구역 분리 (docs/content-roadmap.md "구역 분리"). 임의 기본값 ----
  // 맵 중앙의 정사각형(한 변 = 이 값의 2배)이 건설 구역이다. 건물은 발자국 전체가 이 안에만 들어갈 수 있고, 자원 노드는
  // 이 구역(노드 충돌 반지름만큼 넓힌 것)의 밖에만 놓이고 재생한다. 그래서 건물이 아무리 늘어도 노드가 설 자리가 줄지 않는다.
  // 0이면 구역 제한이 없다(건물은 맵 어디든, 노드는 시작 영역만 피해 어디든).
  float build_zone_half_size_cm = 1200.0f;
  // 재생할 자리를 못 찾았을 때 다시 찾기까지 기다리는 틱 수(매 틱 찾지 않는다)
  std::uint32_t respawn_retry_ticks = 20;

  // **부하 측정 전용(게임 규칙이 아니다).** 방을 만들 때 완공된 집을 이 개수만큼 건설 구역 안에 미리 놓는다(최대 20).
  // 구조물이 방당 상한까지 찬 상태에서 이동 계산 비용을 재려고 만들었다. 기본은 0이다.
  std::uint32_t bench_prebuilt_structures = 0;
  // **부하 측정 전용.** true이면 구조물과 노드가 이동을 막지 않는다(월드에 장애물을 연결하지 않는다). 충돌 비용을 가르는 실험용이다.
  bool bench_no_collision = false;

  // ---- 하루와 잠 (docs/content-day.md 4절). 임의 기본값 ----
  // 시간 단위 구분: "하루" 단위로 바뀌는 것은 노드 재생과 건설 완공(밤 정산)이다. 채집 쿨다운, 재생 자리 재시도,
  // 농장 주기 저장은 그대로 "틱" 단위다.
  std::uint32_t day_start_minute = 360;     // 하루 시작 시각(06:00). 자정부터 센 게임 분
  std::uint32_t day_end_minute = 1560;      // 하루 끝 시각(다음 날 02:00). 24시간을 넘겨 센다
  std::uint32_t ticks_per_game_minute = 10; // 게임 1분의 틱 수(0.5초). 하루 = (1560 - 360) x 10 = 12,000틱 = 10분
  // **부하 측정과 시험 전용.** 0보다 크면 하루가 이 틱 수다(서버 옵션 --bench-day-ticks). 0이면 위 값으로 계산한다.
  std::uint32_t bench_day_ticks = 0;
  float sleep_range_cm = 150.0f;            // 내 중심에서 잘 수 있는 집 발자국까지의 최단 거리(경계 포함)
  // 시작 집(고정 물체). 모든 방의 같은 자리에 있고, 이동을 막고, 그 위에 지을 수 없고, 그 앞에서 잘 수 있다.
  // 너비가 0이면 시작 집이 없다(시작 집과 무관한 시험용).
  float farmhouse_x_cm = 2500.0f;           // 중심. 발자국 x 2300~2700, y 2700~3000
  float farmhouse_y_cm = 2850.0f;
  float farmhouse_width_cm = 400.0f;
  float farmhouse_height_cm = 300.0f;

  // 하루의 틱 수. bench_day_ticks가 있으면 그 값, 아니면 (끝 - 시작) x 분당 틱. 최소 1.
  std::uint64_t DayLengthTicks() const {
    if (bench_day_ticks > 0) return bench_day_ticks;
    const std::uint64_t minutes = day_end_minute > day_start_minute ? day_end_minute - day_start_minute : 0;
    const std::uint64_t length = minutes * ticks_per_game_minute;
    return length == 0 ? 1 : length;
  }

  float TickSeconds() const { return 1.0f / static_cast<float>(tick_rate_hz == 0 ? 1 : tick_rate_hz); }
};

inline constexpr std::size_t kRoomCodeLength = 6;

}  // namespace game::play
