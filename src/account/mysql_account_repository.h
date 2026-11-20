#pragma once

// MySqlAccountRepository: 계정을 MySQL에 저장하고 조회하는 AccountRepository 구현.
// 설계는 MySqlChatRepository와 같다(docs/stage2.md).
//  1. Boost.MySQL의 비동기 API를 코루틴으로 써서, DB 응답을 기다리는 동안에도 로직 스레드가 다른 일을 한다.
//  2. 연결은 하나이고 작업은 큐로 직렬화한다.
//  3. 모든 DB 호출에 제한 시간을 건다.
//  4. 오류가 나면 연결을 버려 다음 작업이 새로 연결한다.
//  5. 쿼리는 모두 준비된 문장에 값을 바인딩해서 보낸다(SQL 인젝션 불가).
//
// 쓰기(CreateAccount)는 자동 재시도하지 않는다. 연결이 끊긴 순간 서버가 이미 반영했는지 알 수 없어서, 재시도하면
// "이미 있는 ID"로 보일 수 있다. 같은 ID를 두 요청이 동시에 만들면 UNIQUE 제약이 하나만 통과시키고 나머지는
// RepoError::kDuplicate가 된다.
//
// 스레드 규칙: 모든 함수는 서버의 io_context(로직) 스레드에서만 부른다.

#include <deque>
#include <functional>
#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <boost/mysql.hpp>

#include "account/account_repository.h"
#include "chat/mysql_chat_repository.h"  // MySqlOptions(접속 정보)를 같이 쓴다

namespace game::account {

// 계정 테이블 이름. 시험은 운영 테이블을 건드리지 않도록 다른 이름을 쓴다. 영문/숫자/밑줄만 허용한다.
struct AccountTables {
  std::string accounts = "accounts";
  std::string stats = "account_stats";
};

class MySqlAccountRepository : public AccountRepository, public std::enable_shared_from_this<MySqlAccountRepository> {
 public:
  // 코루틴이 이 객체를 붙들고 있어야 하므로 shared_ptr로만 만든다. options.table은 쓰지 않는다(채팅 테이블 이름이다).
  // 테이블 이름이 규칙에 맞지 않으면 std::invalid_argument를 던진다.
  static std::shared_ptr<MySqlAccountRepository> Create(boost::asio::io_context& io, chat::MySqlOptions options,
                                                        AccountTables tables = {});

  void CreateAccount(std::string login_name, std::string password_hash, CreateHandler handler) override;
  void FindAccount(std::string login_name, FindHandler handler) override;
  void LoadStats(std::uint64_t account_id, StatsHandler handler) override;

  // 테스트용: 현재 연결을 소리 없이 버린다. 다음 작업이 새로 연결하는지 확인할 때 쓴다.
  void DropConnectionForTest() { connection_.reset(); }

 private:
  using Operation = std::function<boost::asio::awaitable<void>()>;

  MySqlAccountRepository(boost::asio::io_context& io, chat::MySqlOptions options, AccountTables tables);

  void Enqueue(Operation operation);
  static boost::asio::awaitable<void> Pump(std::shared_ptr<MySqlAccountRepository> self);

  static boost::asio::awaitable<void> DoCreate(std::shared_ptr<MySqlAccountRepository> self, std::string login_name,
                                               std::string password_hash, CreateHandler handler);
  static boost::asio::awaitable<void> DoFind(std::shared_ptr<MySqlAccountRepository> self, std::string login_name,
                                             FindHandler handler);
  static boost::asio::awaitable<void> DoLoadStats(std::shared_ptr<MySqlAccountRepository> self,
                                                  std::uint64_t account_id, StatsHandler handler);

  // 연결이 없으면 연결하고 문장을 준비한다. 실패하면 error에 이유를 담고 false.
  boost::asio::awaitable<bool> EnsureReady(std::string& error);

  boost::asio::io_context& io_;
  const chat::MySqlOptions options_;
  const AccountTables tables_;
  const std::string insert_sql_;
  const std::string find_sql_;
  const std::string stats_sql_;

  std::unique_ptr<boost::mysql::any_connection> connection_;  // 연결 전/오류 뒤에는 null
  boost::mysql::statement insert_statement_;
  boost::mysql::statement find_statement_;
  boost::mysql::statement stats_statement_;

  std::deque<Operation> queue_;
  bool running_ = false;
};

}  // namespace game::account
