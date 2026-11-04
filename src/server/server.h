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
//
// 스레드 규칙: Session과 마찬가지로 모든 핸들러가 같은 스레드에서 실행된다는 전제로 동작한다.
// Stop()은 io_context를 돌리는 스레드에서 불러야 한다.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include <boost/asio.hpp>

#include "chat/chat_repository.h"
#include "chat/chat_room.h"
#include "server/session.h"
#include "server/stats.h"

namespace game::server {

struct ServerConfig {
  // 기본값은 로컬 전용이다. 다른 PC나 VM에서 접속하려면 "0.0.0.0"으로 바꾼다.
  std::string bind_address = "127.0.0.1";
  std::uint16_t port = 7777;  // 0이면 운영체제가 빈 포트를 골라 준다. 실제 값은 port()로 확인.
  SessionOptions session;
  chat::ChatRoomOptions chat;
  std::uint32_t stats_interval_seconds = 10;  // 0이면 주기 통계 출력을 끈다.
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

  // accept 반복과 주기 통계 출력을 시작한다. 이후 io_context::run()이 일을 처리한다.
  void Start();

  // 새 연결 받기를 멈추고, 열린 모든 연결을 닫는다. 여러 번 불러도 안전하다.
  void Stop();

  // 실제로 listen 중인 포트. config.port를 0으로 줬을 때 확인용.
  std::uint16_t port() const { return port_; }

  const Stats& stats() const { return stats_; }
  const chat::ChatStats& chat_stats() const { return room_->stats(); }

  // 서버 통계와 채팅 통계를 한 줄로 합친 문자열. 주기 출력과 종료 시 출력에 쓴다.
  std::string FullSummary() const;

 private:
  void DoAccept();
  void OnAccept(const boost::system::error_code& ec, boost::asio::ip::tcp::socket socket);
  void ScheduleAcceptRetry();
  void ScheduleStatsReport();

  const ServerConfig config_;
  boost::asio::ip::tcp::acceptor acceptor_;
  boost::asio::steady_timer accept_retry_timer_;
  boost::asio::steady_timer stats_timer_;
  std::uint16_t port_ = 0;

  Stats stats_;
  std::shared_ptr<chat::ChatRepository> repository_;
  std::unique_ptr<chat::ChatRoom> room_;
  std::uint64_t next_session_id_ = 1;
  // 살아 있는 세션들. 키는 연결 번호. Session이 닫힐 때 콜백으로 여기서 지워진다.
  std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions_;
  bool stopped_ = false;
};

}  // namespace game::server
