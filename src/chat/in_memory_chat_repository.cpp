#include "chat/in_memory_chat_repository.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace game::chat {

namespace {

std::int64_t NowUnixMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

template <typename Fn>
void InMemoryChatRepository::RunLater(Fn fn) {
  const long long delay = delay_ms_.load();
  if (delay <= 0) {
    boost::asio::post(io_, std::move(fn));
    return;
  }
  // 타이머는 핸들러가 끝날 때까지 살아 있어야 하므로 shared_ptr로 핸들러에 붙들어 둔다.
  auto timer = std::make_shared<boost::asio::steady_timer>(io_, std::chrono::milliseconds(delay));
  timer->async_wait([timer, fn = std::move(fn)](const boost::system::error_code&) mutable { fn(); });
}

// 저장 요청. "DB에 반영되는 순간"은 결과를 돌려주는 시점으로 둔다. 그래야 지연 중에 호출한 쪽이
// 포기(시간 초과)해도 뒤늦게 저장되는 실제 상황을 같은 방식으로 시험할 수 있다.
void InMemoryChatRepository::SaveMessage(NewMessage message, SaveHandler handler) {
  const bool failing = failing_.load();
  RunLater([this, failing, message = std::move(message), handler = std::move(handler)]() mutable {
    StorageResult<StoredMessage> result;
    if (failing) {
      result.error = "저장소 장애 (테스트용 주입)";
      handler(std::move(result));
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      StoredMessage stored;
      stored.id = next_id_++;
      stored.nickname = std::move(message.nickname);
      stored.text = std::move(message.text);
      stored.sent_at_unix_ms = NowUnixMillis();
      messages_.push_back(stored);
      result.value = std::move(stored);
    }
    result.ok = true;
    handler(std::move(result));
  });
}

void InMemoryChatRepository::LoadRecent(std::size_t limit, LoadHandler handler) {
  const bool failing = failing_.load();
  RunLater([this, failing, limit, handler = std::move(handler)]() mutable {
    StorageResult<std::vector<StoredMessage>> result;
    if (failing) {
      result.error = "저장소 장애 (테스트용 주입)";
      handler(std::move(result));
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const std::size_t count = std::min(limit, messages_.size());
      // 뒤에서 count개가 가장 최근 메시지다. 저장 순서(= 오래된 것부터)를 그대로 유지한다.
      result.value.assign(messages_.end() - static_cast<std::ptrdiff_t>(count), messages_.end());
    }
    result.ok = true;
    handler(std::move(result));
  });
}

std::size_t InMemoryChatRepository::stored_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return messages_.size();
}

}  // namespace game::chat
