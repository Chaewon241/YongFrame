#include "play/farm_persistence.h"

#include <sstream>
#include <utility>

#include "server/log.h"

namespace game::play {

namespace {

using server::Log;
using server::LogLevel;

}  // namespace

std::string PersistenceStats::Summary() const {
  const auto get = [](const Counter& c) { return c.load(std::memory_order_relaxed); };
  std::ostringstream out;
  out << "저장(성공/실패)=" << get(saves_ok) << '/' << get(saves_failed) << " 요청=" << get(flushes)
      << " 연속실패=" << get(consecutive_failures) << " 대기농장=" << get(pending_farms);
  return out.str();
}

std::shared_ptr<FarmPersistence> FarmPersistence::Create(std::shared_ptr<farm::FarmRepository> repository,
                                                         std::uint64_t flush_interval_ticks) {
  return std::shared_ptr<FarmPersistence>(new FarmPersistence(std::move(repository), flush_interval_ticks));
}

void FarmPersistence::RefreshPendingGauge() {
  std::uint64_t pending = 0;
  for (const auto& entry : farms_) {
    if (!entry.second.empty() || entry.second.inflight) {
      ++pending;
    }
  }
  stats_.pending_farms.store(pending, std::memory_order_relaxed);
}

void FarmPersistence::UpdateStructures(std::uint64_t farm_id, std::vector<farm::StoredStructure> structures) {
  Pending& pending = farms_[farm_id];
  for (auto& structure : structures) {
    pending.structures[structure.structure_id] = std::move(structure);
  }
  RefreshPendingGauge();
}

void FarmPersistence::UpdatePlayer(std::uint64_t farm_id, const farm::StoredPlayer& player,
                                   const std::optional<account::AccountStats>& stats) {
  Pending& pending = farms_[farm_id];
  pending.players[player.account_id] = player;
  if (stats) {
    pending.stats[player.account_id] = *stats;
  }
  RefreshPendingGauge();
}

bool FarmPersistence::HasPending(std::uint64_t farm_id) const {
  const auto it = farms_.find(farm_id);
  return it != farms_.end() && (!it->second.empty() || it->second.inflight);
}

void FarmPersistence::Flush(std::uint64_t farm_id) {
  const auto it = farms_.find(farm_id);
  if (it == farms_.end()) {
    return;
  }
  Pending& pending = it->second;
  if (pending.empty()) {
    return;
  }
  if (pending.inflight) {
    pending.flush_when_idle = true;  // 저장 중이다. 끝나면 그동안 쌓인 변경을 이어서 저장한다.
    return;
  }

  // 대기열을 비워 저장 묶음으로 옮긴다. 저장하는 동안 생기는 새 변경은 대기열에 다시 쌓인다.
  farm::FarmSave save;
  save.farm_id = farm_id;
  for (auto& entry : pending.structures) save.structures.push_back(std::move(entry.second));
  for (auto& entry : pending.players) save.players.push_back(std::move(entry.second));
  for (auto& entry : pending.stats) save.stats.push_back(farm::StoredAccountStats{entry.first, entry.second});
  pending.structures.clear();
  pending.players.clear();
  pending.stats.clear();
  pending.inflight = true;
  ++stats_.flushes;
  RefreshPendingGauge();

  // 핸들러는 약한 참조로 잡는다(서버가 먼저 사라졌다면 결과를 버린다). 실패했을 때 되돌릴 수 있게 묶음을 복사해 둔다.
  repository_->SaveFarm(save, [weak = weak_from_this(), farm_id, save](farm::FarmResult<bool> result) {
    if (const auto self = weak.lock()) {
      self->OnSaved(farm_id, save, result);
    }
  });
}

void FarmPersistence::OnSaved(std::uint64_t farm_id, farm::FarmSave save, const farm::FarmResult<bool>& result) {
  Pending& pending = farms_[farm_id];
  pending.inflight = false;

  if (result.ok) {
    ++stats_.saves_ok;
    stats_.consecutive_failures.store(0, std::memory_order_relaxed);
  } else {
    ++stats_.saves_failed;
    ++stats_.consecutive_failures;
    Log(LogLevel::kError, "[농장] 저장 실패 farm=", farm_id, " 연속실패=", stats_.consecutive_failures.load(), ": ",
        result.message);
    // 실패한 값을 대기열로 되돌린다. 저장하는 동안 더 새로운 값이 들어왔다면(이미 키가 있다면) 그것이 이긴다.
    for (auto& structure : save.structures) pending.structures.emplace(structure.structure_id, std::move(structure));
    for (auto& player : save.players) pending.players.emplace(player.account_id, std::move(player));
    for (auto& entry : save.stats) pending.stats.emplace(entry.account_id, entry.stats);
  }

  // 저장 중에 쌓인 변경이 있으면 이어서 저장한다. 실패한 직후에는 바로 다시 시도하지 않는다(다음 주기에 다시 한다).
  if (pending.flush_when_idle) {
    pending.flush_when_idle = false;
    if (result.ok && !pending.empty()) {
      Flush(farm_id);
    }
  }
  if (pending.empty() && !pending.inflight) {
    farms_.erase(farm_id);
  }
  RefreshPendingGauge();
}

void FarmPersistence::OnTick(std::uint64_t steps) {
  if (flush_interval_ticks_ == 0) {
    return;
  }
  ticks_since_flush_ += steps;
  if (ticks_since_flush_ < flush_interval_ticks_) {
    return;
  }
  ticks_since_flush_ = 0;
  // Flush 안에서 farms_가 바뀔 수 있으므로(성공한 빈 항목 삭제) 번호만 먼저 모은다.
  std::vector<std::uint64_t> due;
  for (const auto& entry : farms_) {
    if (!entry.second.empty() && !entry.second.inflight) {
      due.push_back(entry.first);
    }
  }
  for (const std::uint64_t farm_id : due) {
    Flush(farm_id);
  }
}

void FarmPersistence::FlushAll() {
  shutting_down_ = true;
  std::vector<std::uint64_t> all;
  for (const auto& entry : farms_) {
    all.push_back(entry.first);
  }
  for (const std::uint64_t farm_id : all) {
    Flush(farm_id);
  }
}

}  // namespace game::play
