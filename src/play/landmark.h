#pragma once

// Landmark: 모든 방의 같은 자리에 있는 고정 물체(지금은 시작 집 하나). 설정(PlayConfig)에서 만들고 저장하지 않는다.
// 구조물(StructureSet)과 달리 번호가 없고, 구조물 수 상한에 세지 않는다. 이동을 막고, 그 위에 지을 수 없고,
// 시작 집 앞에서는 잘 수 있다. docs/content-day.md 5.6.

#include <cstdint>
#include <vector>

#include "play/geometry.h"
#include "play/play_config.h"

namespace game::play {

enum class LandmarkKind : std::uint32_t {
  kFarmhouse = 0,  // 시작 집. 조각 4에서 출하 상자, 상점이 더해진다
};

struct Landmark {
  LandmarkKind kind = LandmarkKind::kFarmhouse;
  float x = 0.0f;  // 중심, cm
  float y = 0.0f;
  float width = 0.0f;   // x 방향, cm
  float height = 0.0f;  // y 방향, cm

  Rect Footprint() const {
    return Rect{static_cast<double>(x), static_cast<double>(y), static_cast<double>(width) * 0.5,
                static_cast<double>(height) * 0.5};
  }
};

// 설정에서 고정 물체 목록을 만든다. 시작 집 너비나 높이가 0 이하이면 시작 집이 없다.
inline std::vector<Landmark> MakeLandmarks(const PlayConfig& config) {
  std::vector<Landmark> out;
  if (config.farmhouse_width_cm > 0.0f && config.farmhouse_height_cm > 0.0f) {
    out.push_back(Landmark{LandmarkKind::kFarmhouse, config.farmhouse_x_cm, config.farmhouse_y_cm,
                           config.farmhouse_width_cm, config.farmhouse_height_cm});
  }
  return out;
}

}  // namespace game::play
