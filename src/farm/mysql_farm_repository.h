#pragma once

// MySqlFarmRepository: 농장을 MySQL에 저장하고 불러오는 FarmRepository 구현.
// 설계는 MySqlChatRepository, MySqlAccountRepository와 같다.
//  1. Boost.MySQL의 비동기 API를 코루틴으로 써서, DB 응답을 기다리는 동안에도 로직 스레드가 다른 일을 한다.
//  2. 연결은 하나이고 작업은 큐로 직렬화한다(요청 순서대로 처리하므로 절대값 덮어쓰기의 순서가 지켜진다).
//  3. 모든 DB 호출에 제한 시간을 건다.
//  4. 오류가 나면 연결을 버린다. 다음 작업이 새로 연결한다(진행 중이던 트랜잭션은 연결이 닫히며 취소된다).
//  5. 쿼리는 모두 준비된 문장에 값을 바인딩해서 보낸다(SQL 인젝션 불가).
//
// SaveFarm은 한 번의 호출이 하나의 트랜잭션이다: START TRANSACTION, 구조물/인벤토리/누적 기록의 upsert(절대값), COMMIT.
// 중간에 하나라도 실패하면 연결을 버려 트랜잭션이 취소되고, 호출한 쪽은 같은 묶음을 그대로 다시 보내도 된다(멱등).
//
// 스레드 규칙: 모든 함수는 서버의 io_context(로직) 스레드에서만 부른다.

#include <deque>
#include <functional>
#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <boost/mysql.hpp>

#include "chat/mysql_chat_repository.h"  // MySqlOptions(접속 정보)를 같이 쓴다
#include "farm/farm_repository.h"

namespace game::farm {

// 테이블 이름. 시험은 운영 테이블을 건드리지 않도록 다른 이름을 쓴다. 영문/숫자/밑줄만 허용한다.
struct FarmTables {
  std::string farms = "farms";
  std::string structures = "farm_structures";
  std::string players = "farm_players";
  std::string account_stats = "account_stats";  // 계정 누적 기록(계정 저장소와 같은 테이블)
};

class MySqlFarmRepository : public FarmRepository, public std::enable_shared_from_this<MySqlFarmRepository> {
 public:
  // 코루틴이 이 객체를 붙들고 있어야 하므로 shared_ptr로만 만든다. options.table은 쓰지 않는다(채팅 테이블 이름이다).
  // 테이블 이름이 규칙에 맞지 않으면 std::invalid_argument를 던진다.
  static std::shared_ptr<MySqlFarmRepository> Create(boost::asio::io_context& io, chat::MySqlOptions options,
                                                     FarmTables tables = {});

  void LoadFarmCodes(CodesHandler handler) override;
  void FindFarmByOwner(std::uint64_t owner_account_id, FindHandler handler) override;
  void CreateFarm(std::uint64_t owner_account_id, std::string code, CreateHandler handler) override;
  void SaveFarm(FarmSave save, SaveHandler handler) override;

  // 테스트용: 현재 연결을 소리 없이 버린다. 다음 작업이 새로 연결하는지 확인할 때 쓴다.
  void DropConnectionForTest() { connection_.reset(); }

 private:
  using Operation = std::function<boost::asio::awaitable<void>()>;

  MySqlFarmRepository(boost::asio::io_context& io, chat::MySqlOptions options, FarmTables tables);

  void Enqueue(Operation operation);
  static boost::asio::awaitable<void> Pump(std::shared_ptr<MySqlFarmRepository> self);

  static boost::asio::awaitable<void> DoLoadCodes(std::shared_ptr<MySqlFarmRepository> self, CodesHandler handler);
  static boost::asio::awaitable<void> DoFind(std::shared_ptr<MySqlFarmRepository> self, std::uint64_t owner,
                                             FindHandler handler);
  static boost::asio::awaitable<void> DoCreate(std::shared_ptr<MySqlFarmRepository> self, std::uint64_t owner,
                                               std::string code, CreateHandler handler);
  static boost::asio::awaitable<void> DoSave(std::shared_ptr<MySqlFarmRepository> self, FarmSave save,
                                             SaveHandler handler);

  // 연결이 없으면 연결하고 문장을 준비한다. 실패하면 error에 이유를 담고 false.
  boost::asio::awaitable<bool> EnsureReady(std::string& error);

  // 준비된 문장 하나를 실행한다(제한 시간 포함). 실패하면 error에 이유를 담는다. 연결은 호출한 쪽이 버린다.
  template <typename Bound>
  boost::asio::awaitable<bool> Run(Bound bound, boost::mysql::results& rows, std::string& error, const char* what,
                                   boost::system::error_code* ec_out = nullptr);
  // 문장 없이 SQL 한 줄(START TRANSACTION, COMMIT)을 실행한다.
  boost::asio::awaitable<bool> RunSql(const char* sql, std::string& error);

  // 농장 한 개를 구조물과 인벤토리까지 읽는다(연결이 준비된 상태에서). 실패하면 error에 이유를 담고 false.
  boost::asio::awaitable<bool> ReadFarm(std::uint64_t owner, std::optional<FarmData>& out, std::string& error);

  boost::asio::io_context& io_;
  const chat::MySqlOptions options_;
  const FarmTables tables_;
  const std::string codes_sql_;
  const std::string find_sql_;
  const std::string structures_sql_;
  const std::string players_sql_;
  const std::string insert_farm_sql_;
  const std::string upsert_structure_sql_;
  const std::string upsert_player_sql_;
  const std::string upsert_stats_sql_;

  std::unique_ptr<boost::mysql::any_connection> connection_;  // 연결 전/오류 뒤에는 null
  boost::mysql::statement codes_stmt_;
  boost::mysql::statement find_stmt_;
  boost::mysql::statement structures_stmt_;
  boost::mysql::statement players_stmt_;
  boost::mysql::statement insert_farm_stmt_;
  boost::mysql::statement upsert_structure_stmt_;
  boost::mysql::statement upsert_player_stmt_;
  boost::mysql::statement upsert_stats_stmt_;

  std::deque<Operation> queue_;
  bool running_ = false;
};

}  // namespace game::farm
