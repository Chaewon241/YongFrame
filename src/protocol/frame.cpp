#include "protocol/frame.h"

namespace game::protocol {

namespace {

// 정수를 memcpy/reinterpret_cast 하지 않고 바이트 단위로 읽고 쓴다.
// 호스트 바이트 순서와 정렬에 의존하지 않으므로 Windows/Linux에서 같은 결과가 나온다.
std::uint32_t ReadU32(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

std::uint16_t ReadU16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

}  // namespace

std::optional<std::vector<std::uint8_t>> EncodeFrame(std::uint16_t message_id,
                                                     std::span<const std::uint8_t> payload) {
  if (payload.size() > kMaxPayloadSize) {
    return std::nullopt;
  }
  const auto body_length = static_cast<std::uint32_t>(kMessageIdSize + payload.size());

  std::vector<std::uint8_t> frame;
  frame.reserve(kLengthFieldSize + body_length);
  frame.push_back(static_cast<std::uint8_t>(body_length >> 24));
  frame.push_back(static_cast<std::uint8_t>(body_length >> 16));
  frame.push_back(static_cast<std::uint8_t>(body_length >> 8));
  frame.push_back(static_cast<std::uint8_t>(body_length));
  frame.push_back(static_cast<std::uint8_t>(message_id >> 8));
  frame.push_back(static_cast<std::uint8_t>(message_id));
  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

void FrameDecoder::Append(std::span<const std::uint8_t> data) {
  if (error_ != DecodeError::kNone) {
    return;
  }
  // 이미 소비한 앞부분을 버려서 버퍼가 계속 자라지 않게 한다.
  if (read_pos_ > 0) {
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(read_pos_));
    read_pos_ = 0;
  }
  buffer_.insert(buffer_.end(), data.begin(), data.end());
}

DecodeStatus FrameDecoder::Next(Frame& out) {
  if (error_ != DecodeError::kNone) {
    return DecodeStatus::kError;
  }

  const std::size_t available = buffer_.size() - read_pos_;
  if (available < kLengthFieldSize) {
    return DecodeStatus::kNeedMoreData;
  }

  // 본문을 기다리거나 메모리를 잡기 전에 길이부터 검증한다.
  const std::uint8_t* header = buffer_.data() + read_pos_;
  const std::uint32_t body_length = ReadU32(header);
  if (body_length < kMinBodyLength) {
    return Fail(DecodeError::kLengthTooSmall);
  }
  if (body_length > kMaxBodyLength) {
    return Fail(DecodeError::kLengthTooLarge);
  }

  const std::size_t frame_size = kLengthFieldSize + body_length;
  if (available < frame_size) {
    return DecodeStatus::kNeedMoreData;
  }

  const std::uint8_t* body = header + kLengthFieldSize;
  out.message_id = ReadU16(body);
  out.payload.assign(body + kMessageIdSize, body + body_length);
  read_pos_ += frame_size;
  return DecodeStatus::kFrame;
}

DecodeStatus FrameDecoder::Fail(DecodeError error) {
  error_ = error;
  buffer_.clear();
  read_pos_ = 0;
  return DecodeStatus::kError;
}

}  // namespace game::protocol
