#pragma once

// 1단계 프레임 형식 (docs/protocol.md):
//   [body_length: uint32][message_id: uint16][payload]   (모두 network byte order)
//   body_length = sizeof(message_id) + payload 크기. 길이 필드 4바이트는 제외한다.
//
// 이 파일은 소켓과 무관하다. 바이트를 받아 프레임으로 나누고, 프레임을 바이트로 만든다.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace game::protocol {

inline constexpr std::size_t kLengthFieldSize = 4;
inline constexpr std::size_t kMessageIdSize = 2;
inline constexpr std::size_t kMinBodyLength = kMessageIdSize;  // payload가 비어 있는 프레임
inline constexpr std::size_t kMaxBodyLength = 65536;
inline constexpr std::size_t kMaxPayloadSize = kMaxBodyLength - kMessageIdSize;

struct Frame {
  std::uint16_t message_id = 0;
  std::vector<std::uint8_t> payload;
};

// 전송할 바이트열을 만든다. payload가 kMaxPayloadSize를 넘으면 nullopt.
std::optional<std::vector<std::uint8_t>> EncodeFrame(std::uint16_t message_id,
                                                     std::span<const std::uint8_t> payload);

enum class DecodeStatus {
  kNeedMoreData,  // 프레임이 아직 완성되지 않았다. 더 읽어서 Append 한다.
  kFrame,         // out에 프레임 하나를 채웠다. 한 번 더 Next를 호출해 본다.
  kError,         // 복구할 수 없다. 이 연결을 닫는다.
};

enum class DecodeError {
  kNone,
  kLengthTooSmall,  // body_length < 2
  kLengthTooLarge,  // body_length > 65,536
};

// 연결 하나당 하나씩 둔다. 스레드 안전하지 않다.
//
// 길이 필드 4바이트가 모이는 즉시 검증하므로, 비정상 길이를 보고 큰 메모리를 잡거나
// 본문이 오기를 기다리지 않는다. 내부 버퍼는 "아직 처리하지 못한 바이트"만 보관하며,
// 정상 입력에서는 (한 프레임 최대 크기 + 이번에 Append한 크기)를 넘지 않는다.
// Append에는 소켓 읽기 버퍼 크기 정도의 조각을 넣는 것을 전제로 한다.
class FrameDecoder {
 public:
  void Append(std::span<const std::uint8_t> data);

  // 완성된 프레임이 있으면 out에 채우고 kFrame을 돌려준다.
  // 오류가 한 번 발생하면 이후 모든 호출이 kError를 돌려준다.
  DecodeStatus Next(Frame& out);

  DecodeError error() const { return error_; }

  // 아직 프레임으로 소비되지 않은 바이트 수 (연결이 끊긴 시점에 남아 있으면 중간 종료).
  std::size_t buffered_bytes() const { return buffer_.size() - read_pos_; }

 private:
  DecodeStatus Fail(DecodeError error);

  std::vector<std::uint8_t> buffer_;
  std::size_t read_pos_ = 0;
  DecodeError error_ = DecodeError::kNone;
};

}  // namespace game::protocol
