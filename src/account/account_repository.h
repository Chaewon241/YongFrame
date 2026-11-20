#pragma once

// AccountRepository: 계정을 "저장하고 조회하는" 곳을 가리는 인터페이스. (ChatRepository와 같은 규칙)
//
// 비동기 규칙 (구현이 반드시 지켜야 한다)
//  - 결과는 반환값이 아니라 핸들러로 돌려준다. 저장소가 느려도(DB 응답 대기) 서버의 네트워크 I/O와 게임 틱을 막지
//    않기 위해서다.
//  - 핸들러는 항상 호출한 함수가 "반환한 뒤에", 서버의 io_context(로직) 스레드에서 호출한다. 호출받는 쪽에 잠금이
//    필요 없다.
//  - 핸들러는 정확히 한 번 호출한다.
//
// 비밀번호는 평문이 아니라 PasswordHasher가 만든 해시만 다룬다. login_name은 호출한 쪽이 이미 소문자로 바꿔 넘긴다.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace game::account {

// 저장된 계정 한 개.
struct AccountRecord {
  std::uint64_t id = 0;
  std::string login_name;
  std::string password_hash;
};

// 계정의 누적 기록.
struct AccountStats {
  std::uint64_t wood_gathered = 0;
  std::uint64_t stone_gathered = 0;
  std::uint64_t houses_built = 0;
};

enum class RepoError {
  kNone,
  kDuplicate,  // 같은 ID가 이미 있음(CreateAccount)
  kFailure,    // 저장소 오류, 연결 실패, 시간 초과
};

// 저장소 작업의 결과. ok가 false이면 value는 의미가 없고 error와 message(로그용, 비밀번호는 들어 있지 않다)가 있다.
template <typename T>
struct RepoResult {
  bool ok = false;
  T value{};
  RepoError error = RepoError::kNone;
  std::string message;
};

class AccountRepository {
 public:
  using CreateHandler = std::function<void(RepoResult<AccountRecord>)>;
  using FindHandler = std::function<void(RepoResult<std::optional<AccountRecord>>)>;  // 없으면 ok이고 값이 비어 있다
  using StatsHandler = std::function<void(RepoResult<AccountStats>)>;

  virtual ~AccountRepository() = default;

  // 새 계정을 만든다. 같은 ID가 이미 있으면 error=kDuplicate. 성공하면 번호(id)가 채워진 레코드를 돌려준다.
  virtual void CreateAccount(std::string login_name, std::string password_hash, CreateHandler handler) = 0;

  // ID로 계정을 찾는다. 없는 것은 오류가 아니다(ok=true, 값 없음).
  virtual void FindAccount(std::string login_name, FindHandler handler) = 0;

  // 누적 기록을 읽는다. 기록이 아직 없으면 0으로 채워 돌려준다.
  virtual void LoadStats(std::uint64_t account_id, StatsHandler handler) = 0;
};

}  // namespace game::account
