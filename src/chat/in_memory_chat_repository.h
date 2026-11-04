#pragma once

// InMemoryChatRepository: 메시지를 프로세스 메모리에 저장하는 ChatRepository 구현.
//
// 서버를 다시 시작하면 기록이 사라진다. MySQL을 붙이기 전(2-2)의 기본 저장소이고,
// 테스트에서 저장소의 실패와 지연을 흉내 내는 데도 쓴다.
//
// 결과는 실제 DB처럼 "나중에" 돌려준다. 즉시 호출하지 않고 io_context에 post 하거나 타이머를 걸어서
// 호출하므로, 방 로직이 비동기 완료를 전제로 올바르게 짜였는지 이 구현으로도 검증된다.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <vector>

#include <boost/asio.hpp>

#include "chat/chat_repository.h"

namespace game::chat {

class InMemoryChatRepository : public ChatRepository {
 public:
  explicit InMemoryChatRepository(boost::asio::io_context& io) : io_(io) {}

  void SaveMessage(NewMessage message, SaveHandler handler) override;
  void LoadRecent(std::size_t limit, LoadHandler handler) override;

  // ---- 테스트용 장애/지연 주입. 다른 스레드(테스트 스레드)에서 불러도 안전하다. ----

  // true이면 이후의 모든 저장/조회가 실패한다. (DB 중단 흉내)
  void SetFailing(bool failing) { failing_.store(failing); }

  // 이후의 저장/조회 결과를 이 시간만큼 늦게 돌려준다. (느린 DB 흉내, 시간 초과 시험)
  // 지연이 있어도 저장 자체는 "지연이 끝난 시점"에 반영된다. 시간 초과로 방이 실패 처리한 뒤에도
  // 저장이 뒤늦게 완료될 수 있는 실제 DB의 불확실성이 그대로 재현된다.
  void SetDelay(std::chrono::milliseconds delay) { delay_ms_.store(delay.count()); }

  // 저장된 메시지 수 (테스트에서 "저장되지 않았다"를 확인하는 용도)
  std::size_t stored_count() const;

 private:
  // delay가 0이면 post로, 아니면 타이머로 fn을 io_context 스레드에서 나중에 실행한다.
  template <typename Fn>
  void RunLater(Fn fn);

  boost::asio::io_context& io_;
  std::atomic<bool> failing_{false};
  std::atomic<long long> delay_ms_{0};

  mutable std::mutex mutex_;  // messages_와 next_id_ 보호 (테스트 스레드가 stored_count를 읽기 때문)
  std::vector<StoredMessage> messages_;
  std::uint64_t next_id_ = 1;
};

}  // namespace game::chat
