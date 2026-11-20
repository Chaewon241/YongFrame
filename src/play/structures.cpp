#include "play/structures.h"

#include <algorithm>

namespace game::play {

const Structure* StructureSet::Find(std::uint32_t id) const {
  if (id == 0 || id > structures_.size()) {
    return nullptr;
  }
  return &structures_[id - 1];
}

bool StructureSet::FootprintOf(StructureType type, float& width, float& height) const {
  switch (type) {
    case StructureType::kHouse:
      width = config_.house_width_cm;
      height = config_.house_height_cm;
      return true;
  }
  return false;
}

std::uint32_t StructureSet::Add(StructureType type, float x, float y, std::uint64_t tick) {
  Structure structure;
  structure.id = static_cast<std::uint32_t>(structures_.size()) + 1;
  structure.type = type;
  structure.x = x;
  structure.y = y;
  FootprintOf(type, structure.width, structure.height);
  structure.state = StructureState::kBuilding;
  structure.ready_tick = tick + config_.build_ticks;
  structures_.push_back(structure);
  ++building_count_;
  changed_ids_.push_back(structure.id);
  return structure.id;
}

std::uint32_t StructureSet::Restore(StructureType type, float x, float y, StructureState state,
                                    std::uint64_t remaining_ticks, std::uint64_t tick) {
  Structure structure;
  structure.type = type;
  if (!FootprintOf(type, structure.width, structure.height)) {
    return 0;
  }
  structure.id = static_cast<std::uint32_t>(structures_.size()) + 1;
  structure.x = x;
  structure.y = y;
  structure.state = state;
  structure.ready_tick = state == StructureState::kBuilding ? tick + remaining_ticks : tick;
  structures_.push_back(structure);
  if (state == StructureState::kBuilding) {
    ++building_count_;
  }
  return structure.id;
}

std::uint32_t StructureSet::Advance(std::uint64_t tick) {
  if (building_count_ == 0) {
    return 0;
  }
  std::uint32_t completed = 0;
  for (auto& structure : structures_) {
    if (structure.state == StructureState::kBuilding && tick >= structure.ready_tick) {
      structure.state = StructureState::kComplete;
      --building_count_;
      ++completed;
      if (std::find(changed_ids_.begin(), changed_ids_.end(), structure.id) == changed_ids_.end()) {
        changed_ids_.push_back(structure.id);
      }
    }
  }
  return completed;
}

std::vector<Structure> StructureSet::TakeChanges() {
  std::sort(changed_ids_.begin(), changed_ids_.end());
  std::vector<Structure> changes;
  changes.reserve(changed_ids_.size());
  for (const std::uint32_t id : changed_ids_) {
    changes.push_back(structures_[id - 1]);
  }
  changed_ids_.clear();
  return changes;
}

}  // namespace game::play
