#pragma once

// FarmRepository: 농장(영속하는 방)을 "저장하고 불러오는" 곳을 가리는 인터페이스. (ChatRepository, AccountRepository와 같은 규칙)
//
// 비동기 규칙 (구현이 반드시 지켜야 한다)
//  - 결과는 반환값이 아니라 핸들러로 돌려준다. 저장소가 느려도(DB 응답 대기) 게임 틱과 네트워크 I/O를 막지 않기 위해서다.
//  - 핸들러는 항상 호출한 함수가 "반환한 뒤에", 서버의 io_context(로직) 스레드에서 호출한다.
//  - 핸들러는 정확히 한 번 호출한다.
//  - **같은 저장소 객체의 요청은 호출한 순서대로 처리한다**(FIFO). 저장은 "지금의 값"(절대값)을 덮어쓰는 방식이라 순서가
//    뒤바뀌면 오래된 값이 최신 값을 덮어쓴다.
//
// SaveFarm은 한 번의 호출이 하나의 트랜잭션이다(전부 반영되거나 전부 반영되지 않는다). 같은 값을 두 번 써도 결과가
// 같으므로(멱등) 실패한 저장은 나중에 그대로 다시 보내도 된다.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "account/account_repository.h"

namespace game::farm {

// 농장의 집 하나. 번호는 방 안 번호를 그대로 쓴다(1부터, 건너뜀 없음).
struct StoredStructure {
  std::uint32_t structure_id = 0;
  std::uint32_t type = 0;   // StructureType
  float x = 0.0f;           // 중심, cm
  float y = 0.0f;
  std::uint32_t state = 0;  // StructureState: 0 건설 중, 1 완료
  std::uint64_t remaining_ticks = 0;  // 건설 중이면 완공까지 남은 틱(농장이 닫혀 있는 동안은 흐르지 않는다), 완료면 0
  std::uint64_t built_by_account_id = 0;  // 지은 사람(모르면 0)
};

// (농장, 계정)별 인벤토리.
struct StoredPlayer {
  std::uint64_t account_id = 0;
  std::uint32_t wood = 0;
  std::uint32_t stone = 0;
};

struct StoredAccountStats {
  std::uint64_t account_id = 0;
  account::AccountStats stats;  // 절대값(누적)
};

// 불러온 농장 하나.
struct FarmData {
  std::uint64_t farm_id = 0;
  std::string code;  // 6자리 숫자 문자열. 농장이 있는 동안 바뀌지 않는다
  std::uint64_t owner_account_id = 0;
  std::vector<StoredStructure> structures;  // 번호 순서
  std::vector<StoredPlayer> players;
};

// 한 번에 저장할 묶음. 모두 절대값이다. 비어 있는 목록은 건드리지 않는다.
struct FarmSave {
  std::uint64_t farm_id = 0;
  std::vector<StoredStructure> structures;
  std::vector<StoredPlayer> players;
  std::vector<StoredAccountStats> stats;

  bool empty() const { return structures.empty() && players.empty() && stats.empty(); }
};

enum class FarmError {
  kNone,
  kDuplicateCode,   // 같은 코드의 농장이 이미 있음(CreateFarm)
  kDuplicateOwner,  // 이 계정의 농장이 이미 있음(CreateFarm)
  kFailure,         // 저장소 오류, 연결 실패, 시간 초과
};

template <typename T>
struct FarmResult {
  bool ok = false;
  T value{};
  FarmError error = FarmError::kNone;
  std::string message;  // 로그용(비밀번호와 해시는 들어 있지 않다)
};

class FarmRepository {
 public:
  using CodesHandler = std::function<void(FarmResult<std::vector<std::string>>)>;
  using FindHandler = std::function<void(FarmResult<std::optional<FarmData>>)>;  // 없으면 ok이고 값이 비어 있다
  using CreateHandler = std::function<void(FarmResult<FarmData>)>;
  using SaveHandler = std::function<void(FarmResult<bool>)>;

  virtual ~FarmRepository() = default;

  // 모든 농장의 방 코드. 서버가 시작할 때 읽어, 게스트 방이 농장 코드와 겹치지 않게 한다.
  virtual void LoadFarmCodes(CodesHandler handler) = 0;

  // 이 계정의 농장을 구조물과 인벤토리까지 함께 불러온다. 없으면 ok이고 값이 비어 있다.
  virtual void FindFarmByOwner(std::uint64_t owner_account_id, FindHandler handler) = 0;

  // 새 농장을 만든다. 코드가 이미 있으면 kDuplicateCode, 이 계정의 농장이 이미 있으면 kDuplicateOwner.
  virtual void CreateFarm(std::uint64_t owner_account_id, std::string code, CreateHandler handler) = 0;

  // 한 묶음을 하나의 트랜잭션으로 저장한다(절대값 덮어쓰기). 실패하면 아무것도 반영되지 않는다.
  virtual void SaveFarm(FarmSave save, SaveHandler handler) = 0;
};

}  // namespace game::farm
