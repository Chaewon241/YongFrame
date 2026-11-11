#pragma once

// World: 한 방의 플레이어 위치와 이동 계산. 소켓, 방 관리, 시간 흐름을 모르는 순수 계산이다.
//
// 서버가 위치를 소유한다(서버 판정). 클라이언트는 이동 "방향"만 보내고, 서버가 속도 상한과 맵 경계를
// 적용해 위치를 계산한다. 계산은 고정 시간 간격(Step)으로 하므로 같은 입력 열이면 항상 같은 결과다.
//
// 입력은 "마지막 입력이 계속 유효"하다. 정지하려면 방향 (0, 0)을 보내야 한다.

#include <cstdint>
#include <vector>

#include "play/play_config.h"

namespace game::play {

struct PlayerBody {
  std::uint32_t id = 0;
  float x = 0.0f;  // cm
  float y = 0.0f;  // cm
  float dir_x = 0.0f;  // 현재 이동 방향. 길이는 항상 1 이하(서버가 제한한 값)
  float dir_y = 0.0f;
  std::uint32_t last_input_seq = 0;  // 마지막으로 반영한 입력 번호
  bool has_input = false;            // 입력을 한 번이라도 받았는가 (첫 입력은 번호와 무관하게 받는다)
};

enum class InputResult {
  kApplied,        // 반영함
  kInvalid,        // NaN 또는 무한대라 무시함
  kStale,          // 번호가 이전 입력 이하라 무시함
  kUnknownPlayer,  // 이 월드에 없는 플레이어
};

class World {
 public:
  explicit World(const PlayConfig& config);

  // 플레이어를 칸(slot) 번호에 맞는 시작 위치에 둔다. 같은 id가 이미 있으면 아무것도 하지 않는다.
  void AddPlayer(std::uint32_t id, std::uint32_t slot);
  void RemovePlayer(std::uint32_t id);

  // 이동 방향 입력을 반영한다. 길이가 1을 넘으면 1로 줄인다(속도 조작 방지).
  InputResult SetInput(std::uint32_t id, std::uint32_t sequence, float dir_x, float dir_y);

  // 고정 시간 dt_seconds만큼 모든 플레이어를 이동시키고 틱 번호를 1 올린다.
  void Step(float dt_seconds);

  std::uint64_t tick() const { return tick_; }
  const std::vector<PlayerBody>& players() const { return players_; }
  const PlayerBody* Find(std::uint32_t id) const;

 private:
  PlayerBody* FindMutable(std::uint32_t id);

  PlayConfig config_;
  std::vector<PlayerBody> players_;  // 입장한 순서
  std::uint64_t tick_ = 0;
};

}  // namespace game::play
