#pragma once

// MySqlChatRepository: 채팅 메시지를 MySQL에 저장하고 조회하는 ChatRepository 구현.
//
// 핵심 설계
//  1. DB 호출이 서버의 네트워크 I/O를 막지 않는다. Boost.MySQL의 비동기 API를 코루틴으로 써서, DB 응답을
//     기다리는 동안에도 같은 io_context 스레드가 다른 연결을 처리한다. 새 스레드가 필요 없다.
//  2. 연결은 하나이고 작업은 큐로 직렬화한다. MySQL 연결 하나는 한 번에 작업 하나만 처리할 수 있는데,
//     방은 저장과 기록 조회를 동시에 요청할 수 있기 때문이다.
//  3. 모든 DB 호출에 제한 시간을 건다. DB가 응답하지 않아도 작업이 영원히 걸려 큐 전체를 막지 않는다.
//  4. 오류가 나면 연결을 버린다. 다음 작업이 새로 연결하므로 DB가 내려갔다 올라와도 서버를 다시 시작할
//     필요가 없다. (취소되거나 오류가 난 MySQL 연결은 상태를 믿을 수 없어서 재사용하지 않는다)
//  5. 쿼리는 모두 준비된 문장(prepared statement)에 값을 바인딩해서 보낸다. 사용자 입력이 SQL 문자열에
//     섞이지 않으므로 SQL 인젝션이 불가능하다. (테이블 이름만 문자열로 넣는데, 영문/숫자/밑줄만 허용한다)
//
// 쓰기에는 자동 재시도를 하지 않는다. 연결이 끊긴 순간 서버가 이미 반영했는지 알 수 없어서 재시도하면 같은
// 메시지가 두 번 저장될 수 있다. 실패는 그대로 알리고(STORAGE_FAILURE), 다음 요청이 새 연결을 쓴다.
//
// 스레드 규칙: SaveMessage / LoadRecent / CheckConnection은 서버의 io_context 스레드에서만 부른다.

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <boost/mysql.hpp>

#include "chat/chat_repository.h"

namespace game::chat {

struct MySqlOptions {
  std::string host = "127.0.0.1";
  std::uint16_t port = 3306;
  std::string user = "game";
  std::string password;  // 로그에 출력하지 않는다
  std::string database = "game_chat";
  std::string table = "chat_messages";  // 영문/숫자/밑줄만 허용 (테이블 이름은 값 바인딩이 안 되기 때문)

  // 연결, 문장 준비, 질의 실행 각 단계에 거는 제한 시간. 한 작업이 여러 단계를 거치므로
  // 방(ChatRoom)의 저장 제한 시간보다 작게 잡는다.
  std::chrono::milliseconds step_timeout{1200};
};

class MySqlChatRepository : public ChatRepository, public std::enable_shared_from_this<MySqlChatRepository> {
 public:
  // 코루틴이 이 객체를 붙들고 있어야 하므로 shared_ptr로만 만든다.
  // 테이블 이름이 규칙에 맞지 않으면 std::invalid_argument를 던진다.
  static std::shared_ptr<MySqlChatRepository> Create(boost::asio::io_context& io, MySqlOptions options);

  void SaveMessage(NewMessage message, SaveHandler handler) override;
  void LoadRecent(std::size_t limit, LoadHandler handler) override;

  // 연결과 준비된 문장을 확인한다. 서버 시작 시 DB 상태를 로그로 보여 주는 용도이고, 실패해도 서버는 계속
  // 동작한다. (DB가 나중에 올라와도 되도록) 결과는 io_context 스레드에서 핸들러로 알린다.
  void CheckConnection(std::function<void(bool ok, std::string detail)> handler);

  // 테스트용: 현재 연결을 소리 없이 버린다. 다음 작업이 새로 연결하는지 확인할 때 쓴다.
  void DropConnectionForTest();

 private:
  using Operation = std::function<boost::asio::awaitable<void>()>;

  MySqlChatRepository(boost::asio::io_context& io, MySqlOptions options);

  // 작업을 큐 뒤에 넣고, 큐를 처리하는 코루틴이 없으면 시작한다.
  void Enqueue(Operation operation);
  static boost::asio::awaitable<void> Pump(std::shared_ptr<MySqlChatRepository> self);

  // 큐에서 꺼낸 작업 하나. 값은 코루틴 프레임에 복사해 두므로 호출이 끝난 뒤에도 안전하다.
  static boost::asio::awaitable<void> DoSave(std::shared_ptr<MySqlChatRepository> self, NewMessage message,
                                             SaveHandler handler);
  static boost::asio::awaitable<void> DoLoad(std::shared_ptr<MySqlChatRepository> self, std::size_t limit,
                                             LoadHandler handler);
  static boost::asio::awaitable<void> DoCheck(std::shared_ptr<MySqlChatRepository> self,
                                              std::function<void(bool, std::string)> handler);

  // 연결이 없으면 연결하고 문장을 준비한다. 실패하면 error에 이유를 담고 false.
  boost::asio::awaitable<bool> EnsureReady(std::string& error);

  // 오류가 났을 때 연결을 버려서 다음 작업이 새로 연결하게 한다.
  void ResetConnection() { connection_.reset(); }

  boost::asio::io_context& io_;
  const MySqlOptions options_;
  const std::string insert_sql_;
  const std::string select_sql_;

  std::unique_ptr<boost::mysql::any_connection> connection_;  // 연결 전/오류 뒤에는 null
  boost::mysql::statement insert_statement_;
  boost::mysql::statement select_statement_;

  std::deque<Operation> queue_;
  bool running_ = false;
};

}  // namespace game::chat
