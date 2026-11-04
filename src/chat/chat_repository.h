#pragma once

// ChatRepository: 채팅 메시지를 "저장하고 조회하는" 곳을 가리는 인터페이스.
//
// 채팅 방(ChatRoom)은 이 인터페이스에만 의존한다. 그래서
//  - 지금(2-2)은 메모리에 저장하는 구현(InMemoryChatRepository)을 끼우고,
//  - 2-3에서 MySQL 구현을 끼우되 방 로직은 바꾸지 않으며,
//  - 테스트에서는 실패와 지연을 일부러 일으키는 구현으로 "DB 중단" 같은 상황을 재현한다.
//
// 비동기 규칙 (구현이 반드시 지켜야 한다)
//  - 결과는 반환값이 아니라 핸들러로 돌려준다. 저장소가 느려도(DB 응답 대기) 서버의 네트워크 I/O를
//    막지 않기 위해서다.
//  - 핸들러는 항상 SaveMessage / LoadRecent가 "반환한 뒤에", 서버의 io_context 스레드에서 호출한다.
//    (같은 스레드에서 호출하므로 호출받는 쪽에 잠금이 필요 없다)
//  - 핸들러는 정확히 한 번 호출한다.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace game::chat {

// 저장할 새 메시지. 번호(id)와 시각은 저장소가 정한다.
struct NewMessage {
  std::string nickname;
  std::string text;
};

// 저장된 메시지.
struct StoredMessage {
  std::uint64_t id = 0;            // 저장소가 부여한 번호. 저장 순서대로 증가한다.
  std::string nickname;
  std::string text;
  std::int64_t sent_at_unix_ms = 0;  // 저장된 시각 (UTC, 밀리초)
};

// 저장소 작업의 결과. ok가 false이면 value는 의미가 없고 error에 이유(로그용)가 있다.
template <typename T>
struct StorageResult {
  bool ok = false;
  T value{};
  std::string error;
};

class ChatRepository {
 public:
  using SaveHandler = std::function<void(StorageResult<StoredMessage>)>;
  using LoadHandler = std::function<void(StorageResult<std::vector<StoredMessage>>)>;

  virtual ~ChatRepository() = default;

  // 메시지를 저장한다. 성공하면 id와 시각이 채워진 StoredMessage를 돌려준다.
  virtual void SaveMessage(NewMessage message, SaveHandler handler) = 0;

  // 가장 최근 limit개를 "오래된 것부터" 돌려준다.
  virtual void LoadRecent(std::size_t limit, LoadHandler handler) = 0;
};

}  // namespace game::chat
