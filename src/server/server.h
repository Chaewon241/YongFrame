#pragma once

// Server: 리스닝 소켓을 열고 새 연결을 받아 Session을 만드는 클래스.
//
// 하는 일
//  1. 지정한 주소/포트에 바인드하고 listen 한다. (생성자)
//  2. 새 연결이 올 때마다 accept 하고 Session을 만들어 시작시킨다. (Start / DoAccept / OnAccept)
//  3. 살아 있는 Session 목록을 들고 있다가, 종료할 때 모두 닫는다. (Stop)
//  4. 일정 주기로 통계 한 줄을 로그에 남긴다. (stats_interval_seconds)
//  5. 채팅 방(ChatRoom) 하나와 그 방이 쓰는 저장소(ChatRepository)를 소유하고, 모든 Session이
//     같은 방을 쓰게 한다. (지금은 단일 방)
//  6. 게임 서비스(GameService)를 소유한다. 여러 게임 방과 고정 간격 틱을 관리하며 모든 Session이 공유한다.
//
// 스레드 규칙: Server, ChatRoom, GameService는 "논리 스레드"(생성자에 넘긴 io_context를 돌리는 스레드 1개)에서만
// 실행된다는 전제로 동작한다. 소켓 입출력만 config.io_threads가 0보다 클 때 서버가 따로 만든 입출력 스레드들이
// 맡는다(Session 참고). Stop()은 논리 스레드에서 불러야 한다. 서버를 소멸시키면 입출력 스레드를 모두 정리한다.

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/asio.hpp>

#include "chat/chat_repository.h"
#include "chat/chat_room.h"
#include "play/game_service.h"
#include "server/session.h"
#include "server/stats.h"

namespace game::server {

struct ServerConfig {
  // 기본값은 로컬 전용이다. 다른 PC나 VM에서 접속하려면 "0.0.0.0"으로 바꾼다.
  std::string bind_address = "127.0.0.1";
  std::uint16_t port = 7777;  // 0이면 운영체제가 빈 포트를 골라 준다. 실제 값은 port()로 확인.
  SessionOptions session;
  chat::ChatRoomOptions chat;
  play::PlayConfig play;
  std::uint32_t stats_interval_seconds = 10;  // 0이면 주기 통계 출력을 끈다.

  // 소켓 읽기/쓰기를 맡을 입출력 스레드 수. 0이면 따로 두지 않고 Server에 넘긴 io_context(논리 스레드)가
  // 입출력도 함께 한다(예전과 같은 단일 스레드 동작). N이면 N개 스레드가 연결별 strand로 소켓 입출력을 하고,
  // 게임 로직과 채팅 방은 여전히 논리 스레드 하나에서만 돈다. (docs/stage4.md: 송신 스레드 분리 실험)
  std::uint32_t io_threads = 0;
};

class Server {
 public:
  // 리스닝 소켓을 만들고 바인드/listen 까지 한다. 주소 형식이 틀리거나 포트가 이미 사용 중이면
  // boost::system::system_error 예외를 던진다. (프로세스 시작 단계의 실패라 예외로 알린다)
  //
  // repository: 채팅 기록을 저장하는 곳. 넘기지 않으면 메모리에 저장하는 구현을 만들어 쓴다.
  //             (서버가 꺼지면 기록이 사라진다. 테스트에서는 장애를 주입할 수 있는 구현을 넘긴다)
  Server(boost::asio::io_context& io, ServerConfig config,
         std::shared_ptr<chat::ChatRepository> repository = nullptr);

  // 입출력 스레드를 정리한다(열린 연결이 남아 있으면 강제로 멈춘다).
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // accept 반복과 주기 통계 출력을 시작한다. 이후 io_context::run()이 일을 처리한다.
  void Start();

  // 새 연결 받기를 멈추고, 열린 모든 연결을 닫는다. 여러 번 불러도 안전하다.
  void Stop();

  // 실제로 listen 중인 포트. config.port를 0으로 줬을 때 확인용.
  std::uint16_t port() const { return port_; }

  const Stats& stats() const { return stats_; }
  const chat::ChatStats& chat_stats() const { return room_->stats(); }
  const play::GameStats& game_stats() const { return game_->stats(); }
  play::GameService& game() { return *game_; }

  // 서버 통계와 채팅 통계를 한 줄로 합친 문자열. 주기 출력과 종료 시 출력에 쓴다.
  std::string FullSummary() const;

 private:
  void DoAccept();
  void OnAccept(const boost::system::error_code& ec, boost::asio::ip::tcp::socket socket);
  void ScheduleAcceptRetry();
  void ScheduleStatsReport();

  // 입출력 스레드 풀(io_threads가 0이면 만들지 않는다). 세션의 소켓이 이 io_context에 속하므로, 세션은
  // 풀을 공유 소유한다(Session의 io_keepalive). 서버가 먼저 소멸하고 논리 io_context의 대기 핸들러가 나중에
  // 세션을 해제하더라도, 그 세션의 소켓이 사라진 풀을 건드리지 않게 하기 위해서다.
  std::shared_ptr<boost::asio::io_context> io_pool_;
  using PoolGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
  std::unique_ptr<PoolGuard> io_pool_guard_;
  std::vector<std::thread> io_threads_;

  boost::asio::io_context& logic_io_;   // 논리 스레드의 io_context
  boost::asio::io_context& socket_io_;  // 새 연결의 소켓을 만들 io_context (io_threads가 0이면 logic_io_와 같다)

  const ServerConfig config_;
  boost::asio::ip::tcp::acceptor acceptor_;
  boost::asio::steady_timer accept_retry_timer_;
  boost::asio::steady_timer stats_timer_;
  std::uint16_t port_ = 0;

  Stats stats_;
  std::shared_ptr<chat::ChatRepository> repository_;
  std::unique_ptr<chat::ChatRoom> room_;
  std::unique_ptr<play::GameService> game_;
  std::uint64_t next_session_id_ = 1;
  // 살아 있는 세션들. 키는 연결 번호. Session이 닫힐 때 콜백으로 여기서 지워진다.
  std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions_;
  bool stopped_ = false;
};

}  // namespace game::server
