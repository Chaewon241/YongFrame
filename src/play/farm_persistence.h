#pragma once

// FarmPersistence: 농장의 변경을 모았다가 저장소에 쓰는 "쓰기 대기열"(write-back).
//
// 게임 로직(GameService)은 사건이 생길 때마다 "이 값이 지금 이렇다"(절대값)를 알리기만 하고, 언제 DB에 쓸지는 여기서
// 정한다. 규칙 (docs/persistence.md 5절)
//  - 건설과 완공, 방을 나갈 때, 농장이 닫힐 때, 서버 종료 때는 **바로** 저장한다(Flush).
//  - 채집처럼 자주 일어나는 변경은 대기열에 값만 덮어쓰고, flush_interval_ticks(기본 10초)마다 모아서 저장한다(OnTick).
//  - 저장이 실패해도 게임은 계속된다. 실패한 값은 대기열에 남아(더 새로운 값이 있으면 그것이 이긴다) 다음 주기에 다시
//    저장한다. 저장은 절대값 덮어쓰기라 같은 값을 다시 써도 안전하다(멱등).
//  - 농장 하나당 진행 중인 저장은 한 번에 하나다. 저장 중에 새 변경이 생기면 그 저장이 끝난 뒤에 이어서 저장한다.
//
// 스레드 규칙: 모든 함수와 저장소 핸들러는 서버의 로직 스레드에서만 실행된다.

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "farm/farm_repository.h"

namespace game::play {

struct PersistenceStats {
  using Counter = std::atomic<std::uint64_t>;

  Counter saves_ok{0};                  // 성공한 저장 수
  Counter saves_failed{0};              // 실패한 저장 수
  Counter flushes{0};                   // 저장소로 보낸 저장 요청 수(성공 + 실패 + 진행 중)
  Counter consecutive_failures{0};      // 마지막 성공 이후 연속 실패 수(성공하면 0)
  Counter pending_farms{0};             // 아직 저장하지 못한 변경이 있는 농장 수(관찰용 값)

  std::string Summary() const;
};

class FarmPersistence : public std::enable_shared_from_this<FarmPersistence> {
 public:
  // flush_interval_ticks: 대기열을 모아서 저장하는 주기(게임 틱 수). 0이면 주기 저장을 하지 않는다(시험용).
  static std::shared_ptr<FarmPersistence> Create(std::shared_ptr<farm::FarmRepository> repository,
                                                 std::uint64_t flush_interval_ticks);

  FarmPersistence(const FarmPersistence&) = delete;
  FarmPersistence& operator=(const FarmPersistence&) = delete;

  // 이 농장의 구조물들(절대값)을 대기열에 넣는다. 같은 번호가 이미 있으면 덮어쓴다.
  void UpdateStructures(std::uint64_t farm_id, std::vector<farm::StoredStructure> structures);

  // 이 농장에서의 한 사람의 인벤토리와 그 계정의 누적 기록(절대값)을 대기열에 넣는다.
  void UpdatePlayer(std::uint64_t farm_id, const farm::StoredPlayer& player,
                    const std::optional<account::AccountStats>& stats);

  // 이 농장의 대기열을 지금 저장한다(비어 있으면 아무것도 안 한다). 이미 저장 중이면 그것이 끝난 뒤에 이어서 저장한다.
  void Flush(std::uint64_t farm_id);

  // 게임 틱이 steps만큼 지났다. 주기가 되면 대기 중인 모든 농장을 저장한다.
  void OnTick(std::uint64_t steps);

  // 서버를 끝낼 때: 대기 중인 모든 농장을 바로 저장한다. 이후 실패한 저장은 다시 시도하지 않는다.
  void FlushAll();

  bool HasPending(std::uint64_t farm_id) const;
  const PersistenceStats& stats() const { return stats_; }

 private:
  struct Pending {
    std::map<std::uint32_t, farm::StoredStructure> structures;
    std::map<std::uint64_t, farm::StoredPlayer> players;
    std::map<std::uint64_t, account::AccountStats> stats;
    bool inflight = false;         // 저장소에 보낸 저장이 아직 끝나지 않았다
    bool flush_when_idle = false;  // 저장 중에 새 변경이 생겼으니, 끝나면 이어서 저장한다
    bool empty() const { return structures.empty() && players.empty() && stats.empty(); }
  };

  FarmPersistence(std::shared_ptr<farm::FarmRepository> repository, std::uint64_t flush_interval_ticks)
      : repository_(std::move(repository)), flush_interval_ticks_(flush_interval_ticks) {}

  void OnSaved(std::uint64_t farm_id, farm::FarmSave save, const farm::FarmResult<bool>& result);
  void RefreshPendingGauge();

  std::shared_ptr<farm::FarmRepository> repository_;
  const std::uint64_t flush_interval_ticks_;
  std::uint64_t ticks_since_flush_ = 0;
  bool shutting_down_ = false;
  std::map<std::uint64_t, Pending> farms_;
  PersistenceStats stats_;
};

}  // namespace game::play
