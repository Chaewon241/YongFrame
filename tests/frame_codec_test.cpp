#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "protocol/frame.h"
#include "protocol/message_ids.h"

namespace game::protocol {
namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes MustEncode(std::uint16_t id, const Bytes& payload) {
  auto frame = EncodeFrame(id, payload);
  EXPECT_TRUE(frame.has_value());
  return frame.value_or(Bytes{});
}

Bytes Concat(const Bytes& a, const Bytes& b) {
  Bytes out = a;
  out.insert(out.end(), b.begin(), b.end());
  return out;
}

// 길이 필드만 직접 만든 4바이트 헤더. 비정상 길이 입력을 만들 때 쓴다.
Bytes LengthField(std::uint32_t length) {
  return Bytes{static_cast<std::uint8_t>(length >> 24), static_cast<std::uint8_t>(length >> 16),
               static_cast<std::uint8_t>(length >> 8), static_cast<std::uint8_t>(length)};
}

// 프로토콜 계약(docs/protocol.md)의 숫자. 아래 테스트들은 이 상수 대신 리터럴을 써서,
// 상수가 실수로 바뀌어도 기대값이 같이 따라 바뀌지 않게 한다.
TEST(ProtocolConstants, MatchTheContract) {
  EXPECT_EQ(kLengthFieldSize, 4u);
  EXPECT_EQ(kMessageIdSize, 2u);
  EXPECT_EQ(kMinBodyLength, 2u);
  EXPECT_EQ(kMaxBodyLength, 65536u);
  EXPECT_EQ(kMaxPayloadSize, 65534u);
}

// ---------------------------------------------------------------- 인코딩

TEST(EncodeFrame, ProducesBigEndianLengthAndId) {
  // body_length = 2(ID) + 2(payload) = 4
  const Bytes expected{0x00, 0x00, 0x00, 0x04, 0x00, 0x01, 0xAA, 0xBB};
  EXPECT_EQ(MustEncode(1, Bytes{0xAA, 0xBB}), expected);
}

TEST(EncodeFrame, MessageIdUsesBothBytes) {
  const Bytes expected{0x00, 0x00, 0x00, 0x02, 0x12, 0x34};
  EXPECT_EQ(MustEncode(0x1234, Bytes{}), expected);
}

TEST(EncodeFrame, EmptyPayloadIsAllowed) {
  EXPECT_EQ(MustEncode(2, Bytes{}), (Bytes{0x00, 0x00, 0x00, 0x02, 0x00, 0x02}));
}

TEST(EncodeFrame, MaxPayloadIsAcceptedAndOneMoreIsRejected) {
  const Bytes max_payload(65534, 0x7F);  // 65,536 - 2(ID)
  auto ok = EncodeFrame(1, max_payload);
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(ok->size(), 65540u);  // 4(길이 필드) + 65,536
  // body_length 필드는 65,536 = 0x00010000
  EXPECT_EQ(Bytes(ok->begin(), ok->begin() + 4), (Bytes{0x00, 0x01, 0x00, 0x00}));

  const Bytes too_big(65535, 0x7F);
  EXPECT_FALSE(EncodeFrame(1, too_big).has_value());
}

// ---------------------------------------------------------------- 디코딩: 정상 입력

TEST(FrameDecoder, DecodesSingleFrame) {
  FrameDecoder decoder;
  decoder.Append(MustEncode(1, Bytes{1, 2, 3}));

  Frame frame;
  ASSERT_EQ(decoder.Next(frame), DecodeStatus::kFrame);
  EXPECT_EQ(frame.message_id, 1);
  EXPECT_EQ(frame.payload, (Bytes{1, 2, 3}));
  EXPECT_EQ(decoder.Next(frame), DecodeStatus::kNeedMoreData);
  EXPECT_EQ(decoder.buffered_bytes(), 0u);
}

TEST(FrameDecoder, EmptyPayloadFrameIsMinimumValidLength) {
  FrameDecoder decoder;
  decoder.Append(LengthField(2));
  decoder.Append(Bytes{0x00, 0x09});

  Frame frame;
  ASSERT_EQ(decoder.Next(frame), DecodeStatus::kFrame);
  EXPECT_EQ(frame.message_id, 9);
  EXPECT_TRUE(frame.payload.empty());
}

TEST(FrameDecoder, MaxLengthFrameIsAccepted) {
  FrameDecoder decoder;
  decoder.Append(MustEncode(1, Bytes(65534, 0x55)));

  Frame frame;
  ASSERT_EQ(decoder.Next(frame), DecodeStatus::kFrame);
  EXPECT_EQ(frame.payload.size(), 65534u);
}

// ---------------------------------------------------------------- 디코딩: 조각난 입력

TEST(FrameDecoder, PartialHeaderNeedsMoreData) {
  const Bytes wire = MustEncode(1, Bytes{9});
  for (std::size_t n = 0; n < 4; ++n) {
    FrameDecoder decoder;
    decoder.Append(std::span<const std::uint8_t>(wire.data(), n));
    Frame frame;
    EXPECT_EQ(decoder.Next(frame), DecodeStatus::kNeedMoreData) << "header bytes: " << n;
  }
}

TEST(FrameDecoder, PartialBodyNeedsMoreData) {
  const Bytes wire = MustEncode(1, Bytes{1, 2, 3, 4});
  FrameDecoder decoder;
  decoder.Append(std::span<const std::uint8_t>(wire.data(), wire.size() - 1));
  Frame frame;
  EXPECT_EQ(decoder.Next(frame), DecodeStatus::kNeedMoreData);

  decoder.Append(std::span<const std::uint8_t>(wire.data() + wire.size() - 1, 1));
  ASSERT_EQ(decoder.Next(frame), DecodeStatus::kFrame);
  EXPECT_EQ(frame.payload, (Bytes{1, 2, 3, 4}));
}

TEST(FrameDecoder, ByteByByteDelivery) {
  const Bytes wire = MustEncode(2, Bytes{10, 20, 30});
  FrameDecoder decoder;
  Frame frame;
  for (std::size_t i = 0; i < wire.size(); ++i) {
    EXPECT_EQ(decoder.Next(frame), DecodeStatus::kNeedMoreData) << "before byte " << i;
    decoder.Append(std::span<const std::uint8_t>(&wire[i], 1));
  }
  ASSERT_EQ(decoder.Next(frame), DecodeStatus::kFrame);
  EXPECT_EQ(frame.message_id, 2);
  EXPECT_EQ(frame.payload, (Bytes{10, 20, 30}));
  EXPECT_EQ(decoder.Next(frame), DecodeStatus::kNeedMoreData);
}

// ---------------------------------------------------------------- 디코딩: 합쳐진 입력

TEST(FrameDecoder, MultipleFramesInOneAppendKeepOrder) {
  Bytes wire = MustEncode(1, Bytes{1});
  wire = Concat(wire, MustEncode(2, Bytes{2, 2}));
  wire = Concat(wire, MustEncode(1, Bytes{}));

  FrameDecoder decoder;
  decoder.Append(wire);

  Frame frame;
  ASSERT_EQ(decoder.Next(frame), DecodeStatus::kFrame);
  EXPECT_EQ(frame.message_id, 1);
  EXPECT_EQ(frame.payload, (Bytes{1}));
  ASSERT_EQ(decoder.Next(frame), DecodeStatus::kFrame);
  EXPECT_EQ(frame.message_id, 2);
  EXPECT_EQ(frame.payload, (Bytes{2, 2}));
  ASSERT_EQ(decoder.Next(frame), DecodeStatus::kFrame);
  EXPECT_EQ(frame.message_id, 1);
  EXPECT_TRUE(frame.payload.empty());
  EXPECT_EQ(decoder.Next(frame), DecodeStatus::kNeedMoreData);
}

TEST(FrameDecoder, CompleteFrameFollowedByPartialFrame) {
  const Bytes first = MustEncode(1, Bytes{1, 1});
  const Bytes second = MustEncode(2, Bytes{2, 2, 2});

  FrameDecoder decoder;
  decoder.Append(Concat(first, Bytes(second.begin(), second.begin() + 3)));

  Frame frame;
  ASSERT_EQ(decoder.Next(frame), DecodeStatus::kFrame);
  EXPECT_EQ(frame.payload, (Bytes{1, 1}));
  EXPECT_EQ(decoder.Next(frame), DecodeStatus::kNeedMoreData);
  EXPECT_EQ(decoder.buffered_bytes(), 3u);  // 연결이 여기서 끊기면 "중간 종료"

  decoder.Append(std::span<const std::uint8_t>(second.data() + 3, second.size() - 3));
  ASSERT_EQ(decoder.Next(frame), DecodeStatus::kFrame);
  EXPECT_EQ(frame.message_id, 2);
  EXPECT_EQ(frame.payload, (Bytes{2, 2, 2}));
}

// 임의의 크기로 쪼개 넣어도 인코딩한 프레임이 같은 순서로 그대로 나와야 한다.
TEST(FrameDecoder, RandomChunkingRoundTrip) {
  std::mt19937 rng(12345);  // 고정 시드: 실패를 재현할 수 있다
  std::uniform_int_distribution<int> payload_size(0, 300);
  std::uniform_int_distribution<int> byte_value(0, 255);
  std::uniform_int_distribution<int> chunk_size(1, 50);

  std::vector<Frame> sent;
  Bytes wire;
  for (int i = 0; i < 200; ++i) {
    Frame f;
    f.message_id = static_cast<std::uint16_t>(rng());
    f.payload.resize(static_cast<std::size_t>(payload_size(rng)));
    for (auto& b : f.payload) b = static_cast<std::uint8_t>(byte_value(rng));
    wire = Concat(wire, MustEncode(f.message_id, f.payload));
    sent.push_back(std::move(f));
  }

  FrameDecoder decoder;
  std::vector<Frame> received;
  for (std::size_t pos = 0; pos < wire.size();) {
    const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(chunk_size(rng)), wire.size() - pos);
    decoder.Append(std::span<const std::uint8_t>(wire.data() + pos, n));
    pos += n;
    Frame frame;
    while (decoder.Next(frame) == DecodeStatus::kFrame) {
      received.push_back(frame);
    }
    ASSERT_EQ(decoder.error(), DecodeError::kNone);
  }

  ASSERT_EQ(received.size(), sent.size());
  for (std::size_t i = 0; i < sent.size(); ++i) {
    EXPECT_EQ(received[i].message_id, sent[i].message_id) << "frame " << i;
    EXPECT_EQ(received[i].payload, sent[i].payload) << "frame " << i;
  }
  EXPECT_EQ(decoder.buffered_bytes(), 0u);
}

// ---------------------------------------------------------------- 디코딩: 비정상 길이

TEST(FrameDecoder, LengthBelowMinimumIsRejected) {
  for (std::uint32_t length : {0u, 1u}) {
    FrameDecoder decoder;
    decoder.Append(LengthField(length));
    Frame frame;
    EXPECT_EQ(decoder.Next(frame), DecodeStatus::kError) << "length " << length;
    EXPECT_EQ(decoder.error(), DecodeError::kLengthTooSmall);
  }
}

TEST(FrameDecoder, LengthAboveMaximumIsRejected) {
  for (std::uint32_t length : {65537u, 1000000u, 0x7FFFFFFFu, 0xFFFFFFFFu}) {
    FrameDecoder decoder;
    decoder.Append(LengthField(length));
    Frame frame;
    EXPECT_EQ(decoder.Next(frame), DecodeStatus::kError) << "length " << length;
    EXPECT_EQ(decoder.error(), DecodeError::kLengthTooLarge);
  }
}

// 본문이 도착하길 기다리지 않고, 헤더 4바이트만으로 즉시 거부해야 한다.
TEST(FrameDecoder, OversizedLengthIsRejectedWithoutWaitingForBody) {
  FrameDecoder decoder;
  decoder.Append(LengthField(0xFFFFFFFFu));
  Frame frame;
  EXPECT_EQ(decoder.Next(frame), DecodeStatus::kError);
  EXPECT_EQ(decoder.buffered_bytes(), 0u);  // 버퍼도 비운다
}

TEST(FrameDecoder, ErrorIsStickyAndIgnoresLaterInput) {
  FrameDecoder decoder;
  decoder.Append(LengthField(0));
  Frame frame;
  ASSERT_EQ(decoder.Next(frame), DecodeStatus::kError);

  decoder.Append(MustEncode(1, Bytes{1}));  // 이후 정상 프레임이 와도
  EXPECT_EQ(decoder.Next(frame), DecodeStatus::kError);
  EXPECT_EQ(decoder.error(), DecodeError::kLengthTooSmall);
  EXPECT_EQ(decoder.buffered_bytes(), 0u);
}

TEST(FrameDecoder, ValidFramesBeforeBadLengthAreStillDelivered) {
  Bytes wire = MustEncode(1, Bytes{5});
  wire = Concat(wire, LengthField(1));  // 두 번째 프레임이 비정상

  FrameDecoder decoder;
  decoder.Append(wire);

  Frame frame;
  ASSERT_EQ(decoder.Next(frame), DecodeStatus::kFrame);
  EXPECT_EQ(frame.payload, (Bytes{5}));
  EXPECT_EQ(decoder.Next(frame), DecodeStatus::kError);
}

// ---------------------------------------------------------------- 메시지 ID

TEST(MessageId, KnownAndUnknown) {
  EXPECT_TRUE(IsKnownMessageId(1));
  EXPECT_TRUE(IsKnownMessageId(2));
  EXPECT_FALSE(IsKnownMessageId(0));
  EXPECT_FALSE(IsKnownMessageId(3));
  EXPECT_FALSE(IsKnownMessageId(0xFFFF));
}

}  // namespace
}  // namespace game::protocol
