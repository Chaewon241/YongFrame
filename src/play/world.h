#pragma once

// World: 한 방의 플레이어 위치와 이동 계산. 소켓, 방 관리, 시간 흐름을 모르는 순수 계산이다.
//
// 서버가 위치를 소유한다(서버 판정). 클라이언트는 이동 "방향"만 보내고, 서버가 속도 상한과 맵 경계를
// 적용해 위치를 계산한다. 계산은 고정 시간 간격(Step)으로 하므로 같은 입력 열이면 항상 같은 결과다.
//
// 입력은 "마지막 입력이 계속 유효"하다. 정지하려면 방향 (0, 0)을 보내야 한다.

#include <cstdint>
#include <utility>
#include <vector>

#include "play/geometry.h"
#include "play/play_config.h"

namespace game::play {

class ResourceField;
class StructureSet;

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
  kSleeping,       // 잠든 사람의 입력이라 통째로 무시함(Room이 판정한다)
};

class World {
 public:
  explicit World(const PlayConfig& config);

  // 이동을 막는 것들을 연결한다: 구조물(직사각형)과 고갈되지 않은 자원 노드(원). 매번 현재 상태를 직접 읽으므로
  // 노드가 고갈/재생하거나 구조물이 늘어도 따로 알릴 필요가 없다. 두 객체는 이 World보다 오래 살아야 한다.
  // 연결하지 않으면(nullptr) 맵 경계만 처리한다.
  void AttachObstacles(const ResourceField* resources, const StructureSet* structures) {
    resources_ = resources;
    structures_ = structures;
  }

  // 움직이지 않는 직사각형 장애물(시작 집 같은 고정 물체)을 정한다. 구조물처럼 이동을 막는다.
  void SetFixedObstacles(std::vector<Rect> rects) { fixed_ = std::move(rects); }
  const std::vector<Rect>& fixed_obstacles() const { return fixed_; }

  // 반지름 r인 몸이 (x, y)에 있어도 맵 안이고 장애물과 겹치지 않는가.
  bool IsFree(float x, float y) const;

  // 플레이어를 칸(slot) 번호에 맞는 시작 위치에 둔다. 같은 id가 이미 있으면 아무것도 하지 않는다.
  // 그 자리가 장애물(구조물, 노드)로 막혀 있으면 가장 가까운 빈 자리(격자 100 cm 단위로 바깥으로 찾음)에 둔다.
  void AddPlayer(std::uint32_t id, std::uint32_t slot);
  void RemovePlayer(std::uint32_t id);

  // 플레이어의 이동 방향을 (0, 0)으로 한다(잠들 때). 입력 번호는 그대로다. 없는 플레이어면 false.
  bool Stop(std::uint32_t id);
  // 플레이어를 칸 번호의 시작 위치로 옮기고 방향을 (0, 0)으로 한다(밤 정산의 아침 위치). 막혀 있으면 AddPlayer와 같은
  // "가장 가까운 빈 자리" 규칙을 쓴다. 입력 번호(last_input_seq)는 그대로다. 없는 플레이어면 false.
  bool MoveToSpawn(std::uint32_t id, std::uint32_t slot);

  // 이동 방향 입력을 반영한다. 길이가 1을 넘으면 1로 줄인다(속도 조작 방지).
  InputResult SetInput(std::uint32_t id, std::uint32_t sequence, float dir_x, float dir_y);

  // 고정 시간 dt_seconds만큼 모든 플레이어를 이동시키고 틱 번호를 1 올린다.
  // 장애물과는 축을 나눠 움직여 막힌 축만 멈춘다(벽을 따라 미끄러진다). 이미 겹친 채로 시작한 장애물과는 충돌하지
  // 않아 빠져나올 수 있다. 한 틱 이동량(15 cm)은 가장 얇은 장애물의 지름보다 훨씬 작아 뚫고 지나가지 않는다.
  void Step(float dt_seconds);

  std::uint64_t tick() const { return tick_; }
  const std::vector<PlayerBody>& players() const { return players_; }
  const PlayerBody* Find(std::uint32_t id) const;

 private:
  // 이동 계산 한 몸에 대해 가까운 장애물만 모은 것.
  struct Obstacle {
    bool is_rect = false;
    Rect rect;       // is_rect일 때
    double x = 0.0;  // 원일 때 중심
    double y = 0.0;
    double radius = 0.0;  // 원일 때 (노드 충돌 반지름)
    bool ignore = false;  // 이동을 시작할 때 이미 겹쳐 있어서 이번 이동에서는 막지 않는다
  };

  PlayerBody* FindMutable(std::uint32_t id);
  // 칸 번호의 시작 위치(막혀 있으면 가장 가까운 빈 자리)를 x, y에 넣는다.
  void SpawnPosition(std::uint32_t slot, float& x, float& y) const;
  bool HasObstacles() const { return structures_ != nullptr || resources_ != nullptr || !fixed_.empty(); }
  // (x, y)의 몸이 겹치는 obstacle이 있는가(ignore인 것은 건너뛴다).
  bool Blocked(double x, double y, const std::vector<Obstacle>& obstacles) const;
  // 중심 (x, y)에서 reach 안에 닿을 수 있는 장애물을 모은다.
  void CollectObstacles(double x, double y, double reach, std::vector<Obstacle>& out) const;

  PlayConfig config_;
  std::vector<PlayerBody> players_;  // 입장한 순서
  std::uint64_t tick_ = 0;
  const ResourceField* resources_ = nullptr;
  const StructureSet* structures_ = nullptr;
  std::vector<Rect> fixed_;        // 고정 물체(시작 집)
  std::vector<Obstacle> scratch_;  // Step이 재사용하는 임시 목록(할당을 줄인다)
};

}  // namespace game::play
