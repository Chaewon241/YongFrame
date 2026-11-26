#pragma once

// InMemoryFarmRepository: 농장을 프로세스 메모리에 저장하는 FarmRepository 구현.
// 서버를 다시 시작하면 사라진다. 기본 저장소이고, 시험에서 저장소의 실패와 지연을 흉내 내는 데도 쓴다.
// 결과는 실제 DB처럼 "나중에"(post 또는 타이머) 돌려준다. 같은 지연이면 요청 순서대로 처리된다.
//
// 계정 누적 기록(account_stats)은 MySQL에서는 같은 DB의 다른 테이블이다. 여기서는 인메모리 계정 저장소를 넘겨 받아
// 저장할 때 그쪽의 누적 기록을 갱신해 준다(그래야 다시 로그인했을 때 저장한 값이 보인다).

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>

#include <boost/asio.hpp>

#include "account/in_memory_account_repository.h"
#include "farm/farm_repository.h"

namespace game::farm {

class InMemoryFarmRepository : public FarmRepository {
 public:
  // accounts: 누적 기록을 함께 갱신할 인메모리 계정 저장소(없으면 누적 기록은 저장하지 않는다).
  explicit InMemoryFarmRepository(boost::asio::io_context& io,
                                  std::shared_ptr<account::InMemoryAccountRepository> accounts = nullptr)
      : io_(io), accounts_(std::move(accounts)) {}

  void LoadFarmCodes(CodesHandler handler) override;
  void FindFarmByOwner(std::uint64_t owner_account_id, FindHandler handler) override;
  void CreateFarm(std::uint64_t owner_account_id, std::string code, CreateHandler handler) override;
  void SaveFarm(FarmSave save, SaveHandler handler) override;

  // ---- 테스트용. 다른 스레드(테스트 스레드)에서 불러도 안전하다. ----
  // true이면 이후의 모든 호출이 실패한다(DB 중단 흉내).
  void SetFailing(bool failing) { failing_.store(failing); }
  // true이면 SaveFarm만 실패한다(불러오기는 성공). 저장 실패 시험용.
  void SetSaveFailing(bool failing) { save_failing_.store(failing); }
  // 이후의 호출 결과를 이 시간만큼 늦게 돌려준다.
  void SetDelay(std::chrono::milliseconds delay) { delay_ms_.store(delay.count()); }
  // 시험 준비: 농장을 미리 만들어 둔다(구조물/인벤토리 포함). 만든 농장을 돌려준다.
  FarmData Seed(std::uint64_t owner_account_id, std::string code, std::vector<StoredStructure> structures = {},
                std::vector<StoredPlayer> players = {}, std::optional<StoredClock> clock = std::nullopt);

  std::size_t farm_count() const;
  std::uint64_t save_calls() const { return save_calls_.load(); }       // 성공한 SaveFarm 수
  std::uint64_t failed_saves() const { return failed_saves_.load(); }   // 실패한 SaveFarm 수
  // 저장된 농장(없으면 빈 값).
  std::optional<FarmData> Get(std::uint64_t owner_account_id) const;

 private:
  template <typename Fn>
  void RunLater(Fn fn);

  boost::asio::io_context& io_;
  std::shared_ptr<account::InMemoryAccountRepository> accounts_;
  std::atomic<bool> failing_{false};
  std::atomic<bool> save_failing_{false};
  std::atomic<long long> delay_ms_{0};
  std::atomic<std::uint64_t> save_calls_{0};
  std::atomic<std::uint64_t> failed_saves_{0};

  mutable std::mutex mutex_;
  std::map<std::uint64_t, FarmData> farms_;  // 키: farm_id
  std::uint64_t next_farm_id_ = 1;
};

}  // namespace game::farm
