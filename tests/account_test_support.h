#pragma once

// 계정 시험이 공유하는 도우미.
//
//  - FastPasswordHasher: 빠른 가짜 해시. 실제 Argon2id는 한 번에 수십 ms가 걸려 시험이 느려지므로, 서버 시험은 이
//    구현을 끼운다(진짜 해시는 password_hasher_test가 따로 확인한다). 호출 횟수를 세고, 지연을 줄 수 있다.
//  - FakeClient: 계정 서비스가 보는 연결(ChatClient)의 가짜. 받은 프레임을 모아 둔다.
//  - PumpUntil: io_context를 조금씩 돌리며 조건이 참이 될 때까지 기다린다(저장소와 해시 결과는 "나중에" 온다).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio.hpp>

#include "account/password_hasher.h"
#include "chat/chat_client.h"

namespace game::testing_support {

class FastPasswordHasher : public account::PasswordHasher {
 public:
  // delay가 0보다 크면 Hash/Verify가 그만큼 걸린다(일꾼 스레드에서 도는지, 로직 스레드를 막지 않는지 확인하는 용도).
  explicit FastPasswordHasher(std::chrono::milliseconds delay = std::chrono::milliseconds(0)) : delay_(delay) {}

  std::string Hash(std::string_view password) const override {
    ++hash_calls_;
    Sleep();
    // 솔트 대신 호출마다 다른 번호를 붙여, 같은 비밀번호도 해시가 달라지는 것을 흉내 낸다.
    return "fast$" + std::to_string(counter_++) + "$" + std::string(password);
  }

  bool Verify(std::string_view hash, std::string_view password) const override {
    ++verify_calls_;
    Sleep();
    const auto first = hash.find('$');
    const auto second = first == std::string_view::npos ? first : hash.find('$', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos || hash.substr(0, first) != "fast") {
      return false;
    }
    return hash.substr(second + 1) == password;
  }

  std::uint64_t hash_calls() const { return hash_calls_.load(); }
  std::uint64_t verify_calls() const { return verify_calls_.load(); }

 private:
  void Sleep() const {
    if (delay_.count() > 0) {
      std::this_thread::sleep_for(delay_);
    }
  }

  std::chrono::milliseconds delay_;
  mutable std::atomic<std::uint64_t> counter_{1};
  mutable std::atomic<std::uint64_t> hash_calls_{0};
  mutable std::atomic<std::uint64_t> verify_calls_{0};
};

// 받은 프레임(메시지 ID와 본문)을 모아 두는 가짜 연결.
class FakeClient : public chat::ChatClient {
 public:
  explicit FakeClient(std::uint64_t id) : id_(id) {}

  std::uint64_t client_id() const override { return id_; }

  void Deliver(protocol::MessageId id, const google::protobuf::MessageLite& message) override {
    delivered_.emplace_back(id, message.SerializeAsString());
  }

  std::size_t count() const { return delivered_.size(); }

  // 마지막으로 받은 메시지를 T로 해석한다(ID가 expected와 다르면 기본값을 돌려준다).
  template <typename T>
  T Last(protocol::MessageId expected) const {
    T message;
    if (!delivered_.empty() && delivered_.back().first == expected) {
      message.ParseFromString(delivered_.back().second);
    }
    return message;
  }

  bool LastIs(protocol::MessageId expected) const {
    return !delivered_.empty() && delivered_.back().first == expected;
  }

 private:
  std::uint64_t id_;
  std::vector<std::pair<protocol::MessageId, std::string>> delivered_;
};

// io_context를 조금씩 돌리며 pred가 참이 될 때까지 기다린다. 제한 시간 안에 참이 되면 true.
template <typename Predicate>
bool PumpUntil(boost::asio::io_context& io, Predicate pred,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    io.restart();
    io.poll();
    if (pred()) {
      return true;
    }
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

}  // namespace game::testing_support
