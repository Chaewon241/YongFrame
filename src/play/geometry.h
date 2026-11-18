#pragma once

// 충돌과 배치 판정에 쓰는 작은 기하 함수. 모두 "겹침은 엄격한 부등호"다: 모서리나 면이 정확히 닿기만 하는 것은
// 겹침이 아니다. 이렇게 해야 "닿을 때까지 다가간 플레이어"와 "그 옆에 바로 붙여 지은 건물"이 서로 허용된다.
// 계산은 double로 한다(float 제곱의 반올림으로 경계 판정이 흔들리지 않게).

#include <algorithm>

namespace game::play {

// 축에 정렬된 직사각형. (cx, cy)는 중심, hw/hh는 반폭/반높이(cm).
struct Rect {
  double cx = 0.0;
  double cy = 0.0;
  double hw = 0.0;
  double hh = 0.0;
};

// 중심 (x, y), 반지름 r인 원이 직사각형과 겹치는가.
inline bool CircleOverlapsRect(double x, double y, double r, const Rect& rect) {
  const double nearest_x = std::clamp(x, rect.cx - rect.hw, rect.cx + rect.hw);
  const double nearest_y = std::clamp(y, rect.cy - rect.hh, rect.cy + rect.hh);
  const double dx = x - nearest_x;
  const double dy = y - nearest_y;
  return dx * dx + dy * dy < r * r;
}

// 두 원이 겹치는가.
inline bool CirclesOverlap(double x1, double y1, double r1, double x2, double y2, double r2) {
  const double dx = x1 - x2;
  const double dy = y1 - y2;
  const double sum = r1 + r2;
  return dx * dx + dy * dy < sum * sum;
}

// 두 직사각형이 겹치는가(면이 닿기만 하면 겹침이 아니다).
inline bool RectsOverlap(const Rect& a, const Rect& b) {
  const double dx = a.cx > b.cx ? a.cx - b.cx : b.cx - a.cx;
  const double dy = a.cy > b.cy ? a.cy - b.cy : b.cy - a.cy;
  return dx < a.hw + b.hw && dy < a.hh + b.hh;
}

}  // namespace game::play
