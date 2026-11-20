#pragma once

// 서버 전체 통계 카운터. (project.md 1단계 4번: 현재 연결 수, 수신/송신 프레임 수, 오류 수,
// 송신 큐 크기를 관찰할 수 있어야 한다)
//
// 모든 값은 std::atomic이다. 지금은 io_context를 스레드 1개로 돌리지만, 3단계에서
// 스레드 수를 바꿔 비교할 때 코드를 고치지 않아도 되도록 미리 스레드 안전하게 만든다.
// 통계 출력용이라 정확한 순서 보장은 필요 없어 relaxed 순서로 갱신한다.

#include <atomic>
#include <cstdint>
#include <sstream>
#include <string>

namespace game::server {

struct Stats {
  using Counter = std::atomic<std::uint64_t>;

  // ---- 연결 ----
  Counter current_connections{0};  // 지금 열려 있는 연결 수
  Counter peak_connections{0};     // 지금까지 동시에 열렸던 최대 연결 수
  Counter total_accepted{0};       // 지금까지 받아들인 연결 누계

  // ---- 프레임 ----
  Counter frames_received{0};  // 디코더가 온전히 잘라낸 프레임 수
  Counter frames_sent{0};      // 소켓에 쓰기가 끝난 프레임 수

  // ---- 연결이 "오류로" 끝난 횟수 (이유별) ----
  Counter decode_errors{0};          // 길이 필드가 범위(2~65,536) 밖
  Counter unknown_message_ids{0};    // 서버가 처리하지 않는 메시지 ID
  Counter parse_errors{0};           // Protobuf 파싱 실패
  Counter network_errors{0};         // 읽기/쓰기 오류 (연결 리셋 등)
  Counter send_queue_overflows{0};   // 느린 수신자로 송신 큐가 한도를 넘음
  Counter truncated_connections{0};  // 프레임 중간에 상대가 연결을 끊음
  Counter auth_failure_closes{0};    // 로그인 실패 횟수 초과로 서버가 연결을 닫음
  Counter internal_errors{0};        // 서버 내부 문제 (정상이라면 0)

  // ---- 송신 큐 ----
  Counter max_send_queue_depth{0};  // 연결 하나에서 관측된 송신 큐 길이(프레임 수)의 최댓값

  // value가 target보다 크면 target을 value로 올린다. 여러 스레드가 동시에 호출해도 안전하다.
  static void UpdateMax(Counter& target, std::uint64_t value) {
    std::uint64_t current = target.load(std::memory_order_relaxed);
    while (value > current &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
      // compare_exchange_weak가 실패하면 current가 최신 값으로 갱신되므로 다시 비교한다.
    }
  }

  // 한 줄 요약 문자열. 주기 출력과 종료 시 출력에 쓴다.
  std::string Summary() const {
    const auto get = [](const Counter& c) { return c.load(std::memory_order_relaxed); };
    std::ostringstream out;
    out << "연결(현재/최대/누적)=" << get(current_connections) << '/' << get(peak_connections) << '/'
        << get(total_accepted) << " 프레임(수신/송신)=" << get(frames_received) << '/'
        << get(frames_sent) << " 최대송신큐=" << get(max_send_queue_depth) << " 오류{길이="
        << get(decode_errors) << " 알수없는ID=" << get(unknown_message_ids)
        << " 파싱=" << get(parse_errors) << " 네트워크=" << get(network_errors)
        << " 송신큐초과=" << get(send_queue_overflows) << " 중간종료=" << get(truncated_connections)
        << " 로그인실패종료=" << get(auth_failure_closes) << " 내부=" << get(internal_errors) << '}';
    return out.str();
  }
};

}  // namespace game::server
