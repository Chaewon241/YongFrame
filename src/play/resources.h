#pragma once

// ResourceField: 한 방의 자원 노드(나무, 돌)의 배치와 체력, 고갈과 재생. 소켓과 시계를 모르는 순수 계산이며
// 시간은 호출하는 쪽이 넘기는 틱 번호로만 안다(그래서 시험이 결정적이다). docs/stage4-gather.md.
//
// 노드의 위치와 종류는 방이 만들어질 때 고정 시드로 생성되고 바뀌지 않는다. 바뀌는 것은 체력뿐이다.
// 체력이 바뀐 노드는 "바뀜" 목록에 모아 두었다가 TakeChanges로 한 번에 꺼내 간다(틱마다 알림용).

#include <cstdint>
#include <vector>

#include "play/play_config.h"

namespace game::play {

enum class ResourceKind : std::uint32_t {
  kTree = 0,  // 목재
  kRock = 1,  // 석재
};

struct ResourceNode {
  std::uint32_t id = 0;  // 1부터. 방 안에서 유일
  ResourceKind kind = ResourceKind::kTree;
  float x = 0.0f;  // cm
  float y = 0.0f;  // cm
  std::uint32_t hp = 0;
  std::uint32_t max_hp = 0;
  std::uint64_t respawn_tick = 0;  // hp가 0일 때, 이 틱이 되면 재생
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
  explicit ResourceField(const PlayConfig& config);

  const std::vector<ResourceNode>& nodes() const { return nodes_; }
  const ResourceNode* Find(std::uint32_t node_id) const;

  // 위치 (x, y)의 플레이어가 지금 이 노드를 칠 수 있는가. 상태를 바꾸지 않는다.
  // 확인 순서: 노드 없음 -> 고갈 -> 너무 멂. 거리는 중심 사이 거리이며 허용 거리와 같으면 허용한다.
  NodeCheck Check(std::uint32_t node_id, float x, float y) const;

  // 노드를 한 번 친다. Check가 kOk일 때만 부른다. 체력이 1 줄고(0 아래로 내려가지 않는다), 0이 되면
  // 이 틱부터 respawn_ticks 뒤에 재생하도록 예약한다. 바뀐 노드로 기록한다. 친 노드의 종류를 돌려준다.
  ResourceKind Hit(std::uint32_t node_id, std::uint64_t tick);

  // 틱 번호가 tick이 되었을 때 재생할 노드를 되살린다(체력 가득, 바뀐 노드로 기록). 되살린 노드 수를 돌려준다.
  std::uint32_t Advance(std::uint64_t tick);

  struct Change {
    std::uint32_t node_id;
    std::uint32_t hp;
  };
  // 마지막 호출 이후 체력이 바뀐 노드를 (노드 번호 순으로, 번호당 한 번, 마지막 값으로) 꺼내고 비운다.
  std::vector<Change> TakeChanges();

  bool HasChanges() const { return !changed_ids_.empty(); }

 private:
  void MarkChanged(std::uint32_t node_id);
  ResourceNode* FindMutable(std::uint32_t node_id);

  const PlayConfig config_;
  std::vector<ResourceNode> nodes_;  // 번호 순서. nodes_[i].id == i + 1
  std::vector<std::uint32_t> changed_ids_;
  std::uint32_t depleted_count_ = 0;  // 재생 대기 중인 노드 수(0이면 Advance가 훑지 않는다)
};

}  // namespace game::play
