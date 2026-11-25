#pragma once

// StructureSet: 한 방의 구조물(집) 목록과 건설 진행. 소켓과 시계를 모르는 순수 계산이며 시간은 틱 번호로만 안다.
// 배치가 가능한지(거리, 겹침, 비용) 판정하는 것은 Room::Build이고, 이 클래스는 이미 허용된 구조물을 보관하고
// 건설 중 -> 완료 전환과 "바뀐 구조물" 모으기만 한다. docs/content-roadmap.md.
// 완공은 틱이 아니라 밤 정산(CompleteAll)에 일어난다(docs/content-day.md 5.4).

#include <cstdint>
#include <vector>

#include "play/geometry.h"
#include "play/play_config.h"

namespace game::play {

enum class StructureType : std::uint32_t {
  kHouse = 0,
};

enum class StructureState : std::uint32_t {
  kBuilding = 0,  // 건설 중(충돌은 처음부터 있다)
  kComplete = 1,
};

struct Structure {
  std::uint32_t id = 0;  // 1부터. 방 안에서 유일하고 재사용하지 않는다
  StructureType type = StructureType::kHouse;
  float x = 0.0f;  // 중심, cm
  float y = 0.0f;
  float width = 0.0f;   // x 방향, cm
  float height = 0.0f;  // y 방향, cm
  StructureState state = StructureState::kBuilding;
  std::uint64_t ready_tick = 0;  // 건설 중이면 0(다음 밤 정산에 완료), 완료되었으면 완료된 틱

  Rect Footprint() const {
    return Rect{static_cast<double>(x), static_cast<double>(y), static_cast<double>(width) * 0.5,
                static_cast<double>(height) * 0.5};
  }
};

class StructureSet {
 public:
  explicit StructureSet(const PlayConfig& config) : config_(config) {}

  const std::vector<Structure>& structures() const { return structures_; }
  std::size_t size() const { return structures_.size(); }
  const Structure* Find(std::uint32_t id) const;

  // 종류별 크기(cm). 모르는 종류면 false.
  bool FootprintOf(StructureType type, float& width, float& height) const;

  // 구조물을 더한다. 허용 여부는 호출하는 쪽이 이미 판정했다. 건설 중으로 시작하고(ready_tick = 0) 다음 밤 정산의
  // CompleteAll에서 완료된다. 새 번호를 돌려준다. 바뀐 구조물로 기록한다.
  std::uint32_t Add(StructureType type, float x, float y);

  // 저장되어 있던 구조물을 되살린다(농장을 열 때). 건설 중이면 건설 중으로(다음 밤 정산에 완료), 완료면 지금(tick) 완료된
  // 것으로 둔다. 저장 형식의 남은 틱(remaining_ticks)은 이제 쓰지 않는다. 허용 여부는 이미 판정된 것이라 겹침 등은 검사하지
  // 않는다. 새 번호(1부터 이어짐)를 돌려주고, 모르는 종류면 0을 돌려준다(아무것도 더하지 않는다). 바뀐 구조물로 기록하지
  // 않는다(방 상태에 이미 들어간다).
  std::uint32_t Restore(StructureType type, float x, float y, StructureState state, std::uint64_t tick);

  // 밤 정산: 건설 중인 구조물을 모두 완료로 바꾸고 ready_tick을 tick으로 둔다(바뀐 구조물로 기록). 완료한 수를 돌려준다.
  std::uint32_t CompleteAll(std::uint64_t tick);
  std::uint32_t building_count() const { return building_count_; }

  // 마지막 호출 이후 새로 생기거나 상태가 바뀐 구조물을 (번호 순으로, 한 번씩, 마지막 상태로) 꺼내고 비운다.
  std::vector<Structure> TakeChanges();
  bool HasChanges() const { return !changed_ids_.empty(); }

 private:
  const PlayConfig config_;
  std::vector<Structure> structures_;  // 번호 순서. structures_[i].id == i + 1
  std::vector<std::uint32_t> changed_ids_;
  std::uint32_t building_count_ = 0;  // 건설 중인 구조물 수(0이면 CompleteAll이 훑지 않는다)
};

}  // namespace game::play
