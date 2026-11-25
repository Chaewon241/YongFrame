#pragma once

// ResourceField: 한 방의 자원 노드(나무, 돌)의 배치와 체력, 고갈과 재생. 소켓과 시계를 모르는 순수 계산이며
// 시간은 호출하는 쪽이 넘기는 틱 번호로만 안다(그래서 시험이 결정적이다). docs/stage4-gather.md.
//
// 고갈된 노드는 밤 정산(RespawnAllDepleted)에 되살아난다. 낮 동안에는 밤에 자리를 못 찾은 노드만 재시도(Advance)한다
// (docs/content-day.md 5.4).
//
// 노드의 종류와 처음 위치는 방이 만들어질 때 고정 시드로 생성된다. 재생할 때는 위치가 새로 정해질 수 있다
// (구조물, 플레이어를 피해서). 체력이 바뀌거나 위치가 바뀐 노드는 "바뀜" 목록에 모아 두었다가 TakeChanges로 꺼내 간다.

#include <cstdint>
#include <vector>

#include "play/geometry.h"
#include "play/play_config.h"

namespace game::play {

enum class ResourceKind : std::uint32_t {
  kTree = 0,  // 목재
  kRock = 1,  // 석재
};

// 재생할 자리를 고를 때 피해야 할 것들(방이 채운다). 구조물 발자국과 플레이어 몸(반지름에 노드 반지름을 이미 더했다).
struct SpotBlockers {
  struct Circle {
    double x = 0.0;
    double y = 0.0;
    double r = 0.0;  // 노드 중심이 이 원 안에 들면 막힌다
  };
  std::vector<Rect> rects;
  std::vector<Circle> circles;
};

struct ResourceNode {
  std::uint32_t id = 0;  // 1부터. 방 안에서 유일
  ResourceKind kind = ResourceKind::kTree;
  float x = 0.0f;  // cm
  float y = 0.0f;  // cm
  std::uint32_t hp = 0;
  std::uint32_t max_hp = 0;
  // hp가 0이고 밤 정산에서 재생 자리를 못 찾았을 때, 이 틱이 되면 다시 찾는다. 재시도 예약이 없으면 kNoRetry.
  std::uint64_t respawn_tick = 0;
};

// 노드를 칠 수 있는가에 대한 판정(쿨다운은 플레이어 쪽 상태라 Room이 따로 본다).
enum class NodeCheck {
  kOk,
  kNotFound,
  kDepleted,
  kTooFar,
};

class ResourceField {
 public:
  static constexpr std::uint64_t kNoRetry = ~std::uint64_t{0};

  explicit ResourceField(const PlayConfig& config);

  const std::vector<ResourceNode>& nodes() const { return nodes_; }
  const ResourceNode* Find(std::uint32_t node_id) const;

  // 위치 (x, y)의 플레이어가 지금 이 노드를 칠 수 있는가. 상태를 바꾸지 않는다.
  // 확인 순서: 노드 없음 -> 고갈 -> 너무 멂. 거리는 중심 사이 거리이며 허용 거리와 같으면 허용한다.
  NodeCheck Check(std::uint32_t node_id, float x, float y) const;

  // 노드를 한 번 친다. Check가 kOk일 때만 부른다. 체력이 1 줄고(0 아래로 내려가지 않는다), 0이 되면 고갈 상태로
  // 다음 밤 정산까지 남는다(재생 시각을 예약하지 않는다). 바뀐 노드로 기록한다. 친 노드의 종류를 돌려준다.
  ResourceKind Hit(std::uint32_t node_id);

  // 밤 정산: 고갈된 노드를 모두 되살린다(체력 가득, 바뀐 노드로 기록). 되살린 노드 수를 돌려준다.
  // 재생 자리는 맵 안에서 blockers를 피하고 다른 노드와 떨어진 곳을 결정적 난수로 고른다(같은 설정, 같은 틱이면 같은 자리).
  // 자리를 못 찾은 노드는 고갈 상태로 두고 tick + respawn_retry_ticks에 Advance가 다시 찾도록 예약한다.
  std::uint32_t RespawnAllDepleted(std::uint64_t tick, const SpotBlockers& blockers);
  std::uint32_t RespawnAllDepleted(std::uint64_t tick) { return RespawnAllDepleted(tick, SpotBlockers{}); }

  // 낮 동안의 재시도: 밤 정산에서 자리를 못 찾아 재시도가 예약된 노드 중 예약 시각이 된 것만 다시 찾는다. 찾으면 되살리고,
  // 못 찾으면 respawn_retry_ticks 뒤로 다시 예약한다. 되살린 노드 수를 돌려준다. 그냥 고갈된 노드(채집으로 고갈)는 건드리지
  // 않는다.
  std::uint32_t Advance(std::uint64_t tick, const SpotBlockers& blockers);
  std::uint32_t Advance(std::uint64_t tick) { return Advance(tick, SpotBlockers{}); }

  std::uint32_t depleted_count() const { return depleted_count_; }

  struct Change {
    std::uint32_t node_id;
    std::uint32_t hp;
    float x;  // 재생으로 자리가 바뀌었을 수 있으니 위치도 함께 보낸다
    float y;
  };
  // 마지막 호출 이후 체력이 바뀐 노드를 (노드 번호 순으로, 번호당 한 번, 마지막 값으로) 꺼내고 비운다.
  std::vector<Change> TakeChanges();

  bool HasChanges() const { return !changed_ids_.empty(); }

 private:
  void MarkChanged(std::uint32_t node_id);
  ResourceNode* FindMutable(std::uint32_t node_id);
  // 노드 node_id를 놓을 자리를 찾는다. 성공하면 x, y에 넣는다.
  bool FindRespawnSpot(std::uint32_t node_id, std::uint64_t tick, const SpotBlockers& blockers, float& x, float& y) const;

  const PlayConfig config_;
  std::vector<ResourceNode> nodes_;  // 번호 순서. nodes_[i].id == i + 1
  std::vector<std::uint32_t> changed_ids_;
  // 자리 하나를 찾아 노드를 되살린다. 못 찾으면 재시도를 예약하고 false.
  bool TryRespawn(ResourceNode& node, std::uint64_t tick, const SpotBlockers& blockers);

  std::uint32_t depleted_count_ = 0;  // 고갈된 노드 수(0이면 RespawnAllDepleted가 훑지 않는다)
  std::uint32_t retry_count_ = 0;     // 재시도가 예약된 노드 수(0이면 Advance가 훑지 않는다)
};

}  // namespace game::play
