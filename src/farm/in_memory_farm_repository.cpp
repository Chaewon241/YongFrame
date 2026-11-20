#include "farm/in_memory_farm_repository.h"

#include <algorithm>
#include <utility>

namespace game::farm {

namespace {

constexpr const char* kInjectedFailure = "인메모리 저장소에 주입된 장애";

}  // namespace

// delay가 0이면 post로, 아니면 타이머로 fn을 io_context 스레드에서 나중에 실행한다.
template <typename Fn>
void InMemoryFarmRepository::RunLater(Fn fn) {
  const auto delay = std::chrono::milliseconds(delay_ms_.load());
  if (delay.count() <= 0) {
    boost::asio::post(io_, std::move(fn));
    return;
  }
  auto timer = std::make_shared<boost::asio::steady_timer>(io_);
  timer->expires_after(delay);
  timer->async_wait([timer, fn = std::move(fn)](const boost::system::error_code&) mutable { fn(); });
}

void InMemoryFarmRepository::LoadFarmCodes(CodesHandler handler) {
  RunLater([this, handler = std::move(handler)]() mutable {
    FarmResult<std::vector<std::string>> result;
    if (failing_.load()) {
      result.error = FarmError::kFailure;
      result.message = kInjectedFailure;
      handler(std::move(result));
      return;
    }
    result.ok = true;
    {
      std::lock_guard lock(mutex_);
      for (const auto& entry : farms_) {
        result.value.push_back(entry.second.code);
      }
    }
    handler(std::move(result));
  });
}

void InMemoryFarmRepository::FindFarmByOwner(std::uint64_t owner_account_id, FindHandler handler) {
  RunLater([this, owner_account_id, handler = std::move(handler)]() mutable {
    FarmResult<std::optional<FarmData>> result;
    if (failing_.load()) {
      result.error = FarmError::kFailure;
      result.message = kInjectedFailure;
      handler(std::move(result));
      return;
    }
    result.ok = true;
    {
      std::lock_guard lock(mutex_);
      for (const auto& entry : farms_) {
        if (entry.second.owner_account_id == owner_account_id) {
          result.value = entry.second;
          break;
        }
      }
    }
    handler(std::move(result));
  });
}

void InMemoryFarmRepository::CreateFarm(std::uint64_t owner_account_id, std::string code, CreateHandler handler) {
  RunLater([this, owner_account_id, code = std::move(code), handler = std::move(handler)]() mutable {
    FarmResult<FarmData> result;
    if (failing_.load()) {
      result.error = FarmError::kFailure;
      result.message = kInjectedFailure;
      handler(std::move(result));
      return;
    }
    {
      std::lock_guard lock(mutex_);
      const bool owner_taken = std::any_of(farms_.begin(), farms_.end(), [&](const auto& entry) {
        return entry.second.owner_account_id == owner_account_id;
      });
      const bool code_taken = std::any_of(farms_.begin(), farms_.end(), [&](const auto& entry) {
        return entry.second.code == code;
      });
      if (owner_taken) {
        result.error = FarmError::kDuplicateOwner;
        result.message = "이 계정의 농장이 이미 있음";
      } else if (code_taken) {
        result.error = FarmError::kDuplicateCode;
        result.message = "이미 있는 농장 코드";
      } else {
        FarmData data;
        data.farm_id = next_farm_id_++;
        data.code = code;
        data.owner_account_id = owner_account_id;
        farms_.emplace(data.farm_id, data);
        result.ok = true;
        result.value = std::move(data);
      }
    }
    handler(std::move(result));
  });
}

void InMemoryFarmRepository::SaveFarm(FarmSave save, SaveHandler handler) {
  RunLater([this, save = std::move(save), handler = std::move(handler)]() mutable {
    FarmResult<bool> result;
    if (failing_.load() || save_failing_.load()) {
      ++failed_saves_;
      result.error = FarmError::kFailure;
      result.message = kInjectedFailure;
      handler(std::move(result));
      return;
    }
    {
      std::lock_guard lock(mutex_);
      const auto it = farms_.find(save.farm_id);
      if (it == farms_.end()) {
        ++failed_saves_;
        result.error = FarmError::kFailure;
        result.message = "없는 농장";
        handler(std::move(result));
        return;
      }
      FarmData& farm = it->second;
      for (const auto& structure : save.structures) {
        const auto found = std::find_if(farm.structures.begin(), farm.structures.end(), [&](const StoredStructure& s) {
          return s.structure_id == structure.structure_id;
        });
        if (found != farm.structures.end()) {
          *found = structure;
        } else {
          farm.structures.push_back(structure);
        }
      }
      std::sort(farm.structures.begin(), farm.structures.end(),
                [](const StoredStructure& a, const StoredStructure& b) { return a.structure_id < b.structure_id; });
      for (const auto& player : save.players) {
        const auto found = std::find_if(farm.players.begin(), farm.players.end(),
                                        [&](const StoredPlayer& p) { return p.account_id == player.account_id; });
        if (found != farm.players.end()) {
          *found = player;
        } else {
          farm.players.push_back(player);
        }
      }
      if (accounts_) {
        for (const auto& entry : save.stats) {
          accounts_->SetStats(entry.account_id, entry.stats);
        }
      }
    }
    ++save_calls_;
    result.ok = true;
    result.value = true;
    handler(std::move(result));
  });
}

FarmData InMemoryFarmRepository::Seed(std::uint64_t owner_account_id, std::string code,
                                      std::vector<StoredStructure> structures, std::vector<StoredPlayer> players) {
  std::lock_guard lock(mutex_);
  FarmData data;
  data.farm_id = next_farm_id_++;
  data.code = std::move(code);
  data.owner_account_id = owner_account_id;
  data.structures = std::move(structures);
  data.players = std::move(players);
  farms_.emplace(data.farm_id, data);
  return data;
}

std::size_t InMemoryFarmRepository::farm_count() const {
  std::lock_guard lock(mutex_);
  return farms_.size();
}

std::optional<FarmData> InMemoryFarmRepository::Get(std::uint64_t owner_account_id) const {
  std::lock_guard lock(mutex_);
  for (const auto& entry : farms_) {
    if (entry.second.owner_account_id == owner_account_id) {
      return entry.second;
    }
  }
  return std::nullopt;
}

}  // namespace game::farm
