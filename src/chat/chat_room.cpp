#include "chat/chat_room.h"

#include <algorithm>
#include <sstream>
#include <utility>

#include "chat/chat_rules.h"
#include "protocol/frame.h"

namespace game::chat {

namespace {

using protocol::MessageId;

// target이 value보다 작으면 value로 올린다. (최댓값 기록용)
void UpdateMax(ChatStats::Counter& target, std::uint64_t value) {
  std::uint64_t current = target.load(std::memory_order_relaxed);
  while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
  }
}

// 저장소가 돌려준 메시지를 프로토콜 메시지로 옮긴다.
void FillEntry(const StoredMessage& stored, protocol::ChatEntry& entry) {
  entry.set_message_id(stored.id);
  entry.set_nickname(stored.nickname);
  entry.set_text(stored.text);
  entry.set_sent_at_unix_ms(stored.sent_at_unix_ms);
}

}  // namespace

std::string ChatStats::Summary() const {
  const auto get = [](const Counter& c) { return c.load(std::memory_order_relaxed); };
  std::ostringstream out;
  out << "채팅{접속(현재/최대)=" << get(current_members) << '/' << get(peak_members)
      << " 저장=" << get(messages_saved) << " 거부=" << get(requests_rejected)
      << " 저장실패=" << get(storage_failures) << " 시간초과=" << get(storage_timeouts)
      << " 혼잡거부=" << get(busy_rejections) << " 최대대기=" << get(max_pending_depth) << '}';
  return out.str();
}

ChatRoom::ChatRoom(boost::asio::io_context& io, std::shared_ptr<ChatRepository> repository,
                   ChatRoomOptions options)
    : io_(io), repository_(std::move(repository)), options_(options) {}

// ============================================================ 접속자 관리

ChatRoom::Member* ChatRoom::FindMember(std::uint64_t client_id) {
  const auto it = std::find_if(members_.begin(), members_.end(),
                               [client_id](const Member& m) { return m.client_id == client_id; });
  return it == members_.end() ? nullptr : &*it;
}

bool ChatRoom::NicknameInUse(const std::string& nickname) const {
  return std::any_of(members_.begin(), members_.end(),
                     [&nickname](const Member& m) { return m.nickname == nickname; });
}

void ChatRoom::RemoveMember(std::uint64_t client_id) {
  const auto it = std::find_if(members_.begin(), members_.end(),
                               [client_id](const Member& m) { return m.client_id == client_id; });
  if (it != members_.end()) {
    members_.erase(it);
    --stats_.current_members;
  }
}

// ============================================================ 전송 도우미

void ChatRoom::DeliverTo(const std::weak_ptr<ChatClient>& client, MessageId id,
                         const google::protobuf::MessageLite& message) {
  // 약한 참조를 강한 참조로 바꿀 수 있으면 연결이 아직 살아 있는 것이다. 이미 사라졌으면 건너뛴다.
  if (const auto strong = client.lock()) {
    strong->Deliver(id, message);
  }
}

void ChatRoom::Broadcast(MessageId id, const google::protobuf::MessageLite& message,
                         std::uint64_t except_id) {
  // 전송 중에 어떤 연결이 닫혀(송신 큐 초과 등) OnDisconnect가 members_를 바꿀 수 있다.
  // 순회 도중 컨테이너가 바뀌지 않도록 수신자 목록의 복사본을 만든 뒤 그 복사본으로 보낸다.
  std::vector<std::weak_ptr<ChatClient>> recipients;
  recipients.reserve(members_.size());
  for (const Member& member : members_) {
    if (member.client_id != except_id) {
      recipients.push_back(member.client);
    }
  }
  for (const auto& recipient : recipients) {
    DeliverTo(recipient, id, message);
  }
}

void ChatRoom::NotifyPresence(protocol::PresenceNotice::Kind kind, const std::string& nickname,
                              std::uint64_t except_id) {
  if (shutting_down_) {
    return;
  }
  protocol::PresenceNotice notice;
  notice.set_kind(kind);
  notice.set_nickname(nickname);
  Broadcast(MessageId::kPresenceNotice, notice, except_id);
}

// ============================================================ 입장 / 퇴장

void ChatRoom::HandleJoin(const std::shared_ptr<ChatClient>& client, const protocol::JoinRequest& request) {
  protocol::JoinResponse response;
  const std::uint64_t id = client->client_id();

  // 판정 순서: 이미 입장했는가 -> 닉네임 규칙 -> 닉네임 중복
  protocol::ResultCode result = protocol::RESULT_OK;
  if (FindMember(id) != nullptr) {
    result = protocol::RESULT_ALREADY_JOINED;
  } else {
    result = ValidateNickname(request.nickname());
    if (result == protocol::RESULT_OK && NicknameInUse(request.nickname())) {
      result = protocol::RESULT_NICKNAME_TAKEN;
    }
  }

  if (result != protocol::RESULT_OK) {
    ++stats_.requests_rejected;
    response.set_result(result);
    client->Deliver(MessageId::kJoinResponse, response);
    return;
  }

  members_.push_back(Member{id, client, request.nickname()});
  const std::uint64_t now_in_room = ++stats_.current_members;
  UpdateMax(stats_.peak_members, now_in_room);

  // 입장한 사람에게는 현재 접속자(자신 포함) 목록을 준다.
  response.set_result(protocol::RESULT_OK);
  for (const Member& member : members_) {
    response.add_members(member.nickname);
  }
  client->Deliver(MessageId::kJoinResponse, response);

  // 다른 접속자에게는 입장 사실을 알린다. 본인에게는 보내지 않는다.
  NotifyPresence(protocol::PresenceNotice::KIND_JOINED, request.nickname(), id);
}

void ChatRoom::HandleLeave(const std::shared_ptr<ChatClient>& client) {
  protocol::LeaveResponse response;
  const std::uint64_t id = client->client_id();

  const Member* member = FindMember(id);
  if (member == nullptr) {
    ++stats_.requests_rejected;
    response.set_result(protocol::RESULT_NOT_JOINED);
    client->Deliver(MessageId::kLeaveResponse, response);
    return;
  }

  const std::string nickname = member->nickname;  // RemoveMember가 member를 지우기 전에 복사
  RemoveMember(id);
  response.set_result(protocol::RESULT_OK);
  client->Deliver(MessageId::kLeaveResponse, response);
  NotifyPresence(protocol::PresenceNotice::KIND_LEFT, nickname, id);
}

void ChatRoom::OnDisconnect(std::uint64_t client_id) {
  const Member* member = FindMember(client_id);
  if (member == nullptr) {
    return;  // 입장하지 않은 연결이 끊긴 것
  }
  const std::string nickname = member->nickname;
  RemoveMember(client_id);
  NotifyPresence(protocol::PresenceNotice::KIND_LEFT, nickname, client_id);
}

// ============================================================ 채팅

void ChatRoom::HandleChat(const std::shared_ptr<ChatClient>& client, const protocol::ChatRequest& request) {
  protocol::ChatResponse response;
  const Member* member = FindMember(client->client_id());

  protocol::ResultCode result = protocol::RESULT_OK;
  if (member == nullptr) {
    result = protocol::RESULT_NOT_JOINED;
  } else {
    result = ValidateMessageText(request.text());
  }
  if (result == protocol::RESULT_OK && pending_.size() >= options_.max_pending_chats) {
    result = protocol::RESULT_SERVER_BUSY;
    ++stats_.busy_rejections;
  }

  if (result != protocol::RESULT_OK) {
    ++stats_.requests_rejected;
    response.set_result(result);
    client->Deliver(MessageId::kChatResponse, response);
    return;
  }

  // 검증을 통과했다. 저장 대기열에 넣고, 진행 중인 저장이 없으면 바로 시작한다.
  pending_.push_back(PendingChat{client->client_id(), client, member->nickname, request.text()});
  RecordPendingDepth();
  if (!saving_) {
    SaveNext();
  }
}

void ChatRoom::RecordPendingDepth() { UpdateMax(stats_.max_pending_depth, pending_.size()); }

// 대기열 맨 앞의 채팅 하나를 저장한다. 저장이 끝나(또는 시간 초과로 포기해)야 다음 채팅으로 넘어간다.
// 이렇게 한 번에 하나씩만 저장하므로 방의 전파 순서가 저장 순서(message_id)와 같아진다.
void ChatRoom::SaveNext() {
  if (shutting_down_ || pending_.empty()) {
    saving_ = false;
    return;
  }
  saving_ = true;

  auto chat = std::make_shared<PendingChat>(std::move(pending_.front()));
  pending_.pop_front();

  NewMessage message{chat->nickname, chat->text};
  RunWithTimeout<StorageResult<StoredMessage>>(
      [this, message = std::move(message)](ChatRepository::SaveHandler done) mutable {
        repository_->SaveMessage(std::move(message), std::move(done));
      },
      [this, chat](StorageResult<StoredMessage> result) { OnSaveFinished(*chat, std::move(result)); },
      [this, chat] { OnSaveTimedOut(*chat); });
}

void ChatRoom::OnSaveFinished(const PendingChat& chat, StorageResult<StoredMessage> result) {
  protocol::ChatResponse response;
  if (result.ok) {
    ++stats_.messages_saved;
    response.set_result(protocol::RESULT_OK);
    response.set_message_id(result.value.id);
    // 보낸 사람에게 결과를 먼저 알리고, 이어서 방 전체(보낸 사람 포함)에 전파한다.
    DeliverTo(chat.sender, MessageId::kChatResponse, response);

    protocol::ChatBroadcast broadcast;
    FillEntry(result.value, *broadcast.mutable_entry());
    Broadcast(MessageId::kChatBroadcast, broadcast);
  } else {
    ++stats_.storage_failures;
    response.set_result(protocol::RESULT_STORAGE_FAILURE);
    DeliverTo(chat.sender, MessageId::kChatResponse, response);
  }
  SaveNext();  // 다음 대기 채팅이 있으면 이어서 저장
}

// 제한 시간 안에 저장 결과가 오지 않았다. 실제로는 저장이 나중에 끝날 수도 있지만(결과 불명),
// 보낸 사람에게는 실패로 알리고 전파하지 않는다. (docs/protocol-chat.md "알려진 불확실성")
void ChatRoom::OnSaveTimedOut(const PendingChat& chat) {
  ++stats_.storage_failures;
  ++stats_.storage_timeouts;
  protocol::ChatResponse response;
  response.set_result(protocol::RESULT_STORAGE_FAILURE);
  DeliverTo(chat.sender, MessageId::kChatResponse, response);
  SaveNext();
}

// ============================================================ 기록 조회

void ChatRoom::HandleHistory(const std::shared_ptr<ChatClient>& client, const protocol::HistoryRequest& request) {
  protocol::HistoryResponse response;

  protocol::ResultCode result = protocol::RESULT_OK;
  std::uint32_t limit = request.limit();
  if (FindMember(client->client_id()) == nullptr) {
    result = protocol::RESULT_NOT_JOINED;
  } else if (limit > options_.max_history_limit) {
    result = protocol::RESULT_INVALID_ARGUMENT;
  }
  if (result != protocol::RESULT_OK) {
    ++stats_.requests_rejected;
    response.set_result(result);
    client->Deliver(MessageId::kHistoryResponse, response);
    return;
  }
  if (limit == 0) {
    limit = options_.default_history_limit;
  }

  const std::weak_ptr<ChatClient> weak = client;
  RunWithTimeout<StorageResult<std::vector<StoredMessage>>>(
      [this, limit](ChatRepository::LoadHandler done) { repository_->LoadRecent(limit, std::move(done)); },
      [this, weak](StorageResult<std::vector<StoredMessage>> loaded) {
        protocol::HistoryResponse reply;
        if (!loaded.ok) {
          ++stats_.storage_failures;
          reply.set_result(protocol::RESULT_STORAGE_FAILURE);
        } else {
          reply.set_result(protocol::RESULT_OK);
          for (const StoredMessage& stored : loaded.value) {
            FillEntry(stored, *reply.add_entries());
          }
          // 응답이 프레임의 최대 크기를 넘으면 가장 오래된 항목부터 덜어낸다.
          // (정상 설정에서는 50개가 약 55KB라 넘지 않는다)
          while (reply.ByteSizeLong() > protocol::kMaxPayloadSize && reply.entries_size() > 0) {
            reply.mutable_entries()->DeleteSubrange(0, 1);
          }
        }
        DeliverTo(weak, MessageId::kHistoryResponse, reply);
      },
      [this, weak] {
        ++stats_.storage_failures;
        ++stats_.storage_timeouts;
        protocol::HistoryResponse reply;
        reply.set_result(protocol::RESULT_STORAGE_FAILURE);
        DeliverTo(weak, MessageId::kHistoryResponse, reply);
      });
}

// ============================================================ 시간 제한 / 종료

template <typename Result, typename StartFn, typename OnResult, typename OnTimeout>
void ChatRoom::RunWithTimeout(StartFn start, OnResult on_result, OnTimeout on_timeout) {
  auto guard = std::make_shared<Guard>(io_);
  guard->id = ++next_guard_id_;
  guards_.emplace(guard->id, guard);

  // 제한 시간 타이머. 먼저 울리면 시간 초과로 처리하고, 결과가 먼저 오면 취소된다.
  guard->timer.expires_after(options_.storage_timeout);
  guard->timer.async_wait([this, guard, on_timeout](const boost::system::error_code& ec) {
    if (ec || guard->finished) {
      return;  // 타이머가 취소됐거나, 결과가 이미 처리됐다
    }
    guard->finished = true;
    guards_.erase(guard->id);
    on_timeout();
  });

  // 저장소에 작업을 맡기고, 결과가 오면 먼저 도착했는지 확인한 뒤 처리한다.
  start(std::function<void(Result)>([this, guard, on_result](Result result) mutable {
    if (guard->finished) {
      return;  // 이미 시간 초과로 처리했다. 뒤늦게 도착한 결과는 버린다.
    }
    guard->finished = true;
    guard->timer.cancel();
    guards_.erase(guard->id);
    on_result(std::move(result));
  }));
}

void ChatRoom::Shutdown() {
  shutting_down_ = true;
  // 진행 중인 저장 대기를 모두 끝난 것으로 표시하고 타이머를 취소한다. 이후 도착하는 저장소 결과나
  // 타이머 통지는 finished를 보고 아무것도 하지 않으므로, 서버 종료를 지연시키거나 사라진 객체를
  // 건드리지 않는다.
  for (auto& entry : guards_) {
    entry.second->finished = true;
    entry.second->timer.cancel();
  }
  guards_.clear();
  pending_.clear();
  saving_ = false;
}

}  // namespace game::chat
