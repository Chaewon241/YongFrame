// 서버 통합 테스트.
//
// 테스트 프로세스 안에서 진짜 Server를 다른 스레드로 띄우고(포트는 OS가 자동 배정), 실제 TCP 소켓으로
// 접속해서 docs/protocol.md의 "1단계 검증 입력" 표를 하나씩 확인한다.
//
// 결과 확인 방법
//  - 응답이 오는 시나리오: 클라이언트가 받은 응답 내용을 비교한다.
//  - 연결이 끊겨야 하는 시나리오: 클라이언트가 "서버가 연결을 닫았다"를 확인하고, 서버의 통계
//    카운터(Stats)에 올바른 이유가 기록됐는지 본다. 그리고 다른 연결로 Ping이 되는지 확인해
//    "서버 프로세스가 살아 있다"를 검증한다.
//
// 서버가 처리하는 시각과 테스트가 확인하는 시각이 다른 스레드라서, 통계처럼 "곧 반영될" 값은
// WaitUntil로 제한 시간 동안 기다리며 확인한다. (고정 sleep으로 맞추지 않는다)

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "client/blocking_client.h"
#include "protocol/frame.h"
#include "protocol/message_ids.h"
#include "server/server.h"
#include "test_support.h"

namespace game {
namespace {

using client::BlockingClient;
using Bytes = std::vector<std::uint8_t>;
using Millis = std::chrono::milliseconds;
using Status = BlockingClient::ReceiveStatus;

constexpr Millis kTimeout{3000};

// 서버를 띄우는 TestServer, 조건 대기 WaitUntil 등은 tests/test_support.h에 있다.
using testing_support::DefaultServerConfig;
using testing_support::Load;
using testing_support::TestServer;
using testing_support::WaitUntil;

[[maybe_unused]] ::testing::Environment* const kQuietLog =
    ::testing::AddGlobalTestEnvironment(new testing_support::QuietLogEnvironment);

// 서버에 연결하는 클라이언트를 만든다. 연결에 실패하면 테스트를 즉시 실패시킨다.
std::unique_ptr<BlockingClient> Connect(const TestServer& server) {
  auto client = std::make_unique<BlockingClient>();
  EXPECT_TRUE(client->Connect("127.0.0.1", server.port())) << client->last_error();
  return client;
}

// Ping을 보내 같은 request_id와 payload가 돌아오는지 확인한다.
::testing::AssertionResult PingRoundTrip(BlockingClient& client, std::uint64_t request_id,
                                         const std::string& payload) {
  if (!client.SendPing(request_id, payload)) {
    return ::testing::AssertionFailure() << "전송 실패: " << client.last_error();
  }
  protocol::PingResponse response;
  const Status status = client.ReceivePingResponse(response, kTimeout);
  if (status != Status::kFrame) {
    return ::testing::AssertionFailure()
           << "응답을 받지 못함 (상태 " << static_cast<int>(status) << "): " << client.last_error();
  }
  if (response.request_id() != request_id) {
    return ::testing::AssertionFailure()
           << "request_id 불일치: 보냄 " << request_id << ", 받음 " << response.request_id();
  }
  if (response.payload() != payload) {
    return ::testing::AssertionFailure() << "payload 불일치 (보낸 " << payload.size() << "바이트, 받은 "
                                         << response.payload().size() << "바이트)";
  }
  return ::testing::AssertionSuccess();
}

// 새 연결로 Ping이 되는지 본다. "비정상 입력을 받은 뒤에도 서버 프로세스가 살아 있다"의 검증이다.
::testing::AssertionResult ServerStillWorks(const TestServer& server) {
  BlockingClient client;
  if (!client.Connect("127.0.0.1", server.port())) {
    return ::testing::AssertionFailure() << "새 연결 실패: " << client.last_error();
  }
  return PingRoundTrip(client, 9999, "still-alive");
}

// PingRequest를 직접 직렬화해 프레임 바이트열로 만든다. 쪼개 보내기/합쳐 보내기 테스트에 쓴다.
Bytes PingFrameBytes(std::uint64_t request_id, const std::string& payload) {
  protocol::PingRequest request;
  request.set_request_id(request_id);
  request.set_payload(payload);
  Bytes body(request.ByteSizeLong());
  request.SerializeToArray(body.data(), static_cast<int>(body.size()));
  auto wire = protocol::EncodeFrame(protocol::ToUint16(protocol::MessageId::kPingRequest), body);
  EXPECT_TRUE(wire.has_value());
  return wire.value_or(Bytes{});
}

// 길이 필드 4바이트(network byte order)만 만든다. 비정상 길이 테스트용.
Bytes LengthField(std::uint32_t length) {
  return Bytes{static_cast<std::uint8_t>(length >> 24), static_cast<std::uint8_t>(length >> 16),
               static_cast<std::uint8_t>(length >> 8), static_cast<std::uint8_t>(length)};
}

Bytes Concat(const Bytes& a, const Bytes& b) {
  Bytes out = a;
  out.insert(out.end(), b.begin(), b.end());
  return out;
}

// ============================================================ 정상 동작

TEST(ServerIntegration, SinglePingEchoesIdAndPayload) {
  TestServer server;
  auto client = Connect(server);
  EXPECT_TRUE(PingRoundTrip(*client, 42, "hello"));
}

TEST(ServerIntegration, EmptyPayloadIsEchoed) {
  TestServer server;
  auto client = Connect(server);
  EXPECT_TRUE(PingRoundTrip(*client, 1, ""));
}

TEST(ServerIntegration, ConsecutiveRequestsOnOneConnection) {
  TestServer server;
  auto client = Connect(server);
  for (std::uint64_t id = 1; id <= 50; ++id) {
    ASSERT_TRUE(PingRoundTrip(*client, id, std::string(id, 'x'))) << "request_id=" << id;
  }
  EXPECT_TRUE(WaitUntil([&] { return Load(server.stats().frames_sent) == 50; }));
  EXPECT_EQ(Load(server.stats().frames_received), 50u);
}

TEST(ServerIntegration, MaxSizeFrameRoundTrips) {
  TestServer server;
  auto client = Connect(server);
  // protobuf 오버헤드 8바이트를 포함해 본문이 정확히 65,534바이트 -> body_length 65,536
  EXPECT_TRUE(PingRoundTrip(*client, 1, std::string(65528, 'M')));
}

// ============================================================ 쪼개지거나 합쳐진 입력

TEST(ServerIntegration, FrameSentOneByteAtATime) {
  TestServer server;
  auto client = Connect(server);

  for (const std::uint8_t byte : PingFrameBytes(7, "split-me")) {
    ASSERT_TRUE(client->SendRaw(std::span<const std::uint8_t>(&byte, 1))) << client->last_error();
    std::this_thread::sleep_for(Millis(2));
  }
  protocol::PingResponse response;
  ASSERT_EQ(client->ReceivePingResponse(response, kTimeout), Status::kFrame);
  EXPECT_EQ(response.request_id(), 7u);
  EXPECT_EQ(response.payload(), "split-me");
}

TEST(ServerIntegration, FrameSplitInsideHeaderAndInsideBody) {
  TestServer server;
  auto client = Connect(server);
  const Bytes wire = PingFrameBytes(8, "abcdefghij");

  // 길이 필드 중간(2바이트), 본문 중간에서 각각 끊어 세 번에 나눠 보낸다.
  ASSERT_TRUE(client->SendRaw(std::span<const std::uint8_t>(wire.data(), 2)));
  std::this_thread::sleep_for(Millis(30));
  ASSERT_TRUE(client->SendRaw(std::span<const std::uint8_t>(wire.data() + 2, 6)));
  std::this_thread::sleep_for(Millis(30));
  ASSERT_TRUE(client->SendRaw(std::span<const std::uint8_t>(wire.data() + 8, wire.size() - 8)));

  protocol::PingResponse response;
  ASSERT_EQ(client->ReceivePingResponse(response, kTimeout), Status::kFrame);
  EXPECT_EQ(response.request_id(), 8u);
  EXPECT_EQ(response.payload(), "abcdefghij");
}

TEST(ServerIntegration, ManyFramesInOneSendAreAnsweredInOrder) {
  TestServer server;
  auto client = Connect(server);

  Bytes wire;
  for (std::uint64_t id = 1; id <= 10; ++id) {
    wire = Concat(wire, PingFrameBytes(id, std::string(3, static_cast<char>('a' + id))));
  }
  ASSERT_TRUE(client->SendRaw(wire));

  for (std::uint64_t id = 1; id <= 10; ++id) {
    protocol::PingResponse response;
    ASSERT_EQ(client->ReceivePingResponse(response, kTimeout), Status::kFrame) << "id=" << id;
    EXPECT_EQ(response.request_id(), id);  // 요청한 순서대로 응답이 와야 한다.
    EXPECT_EQ(response.payload(), std::string(3, static_cast<char>('a' + id)));
  }
}

TEST(ServerIntegration, ThreeFramesPlusPartialFourthThenTheRest) {
  TestServer server;
  auto client = Connect(server);

  Bytes first_three;
  for (std::uint64_t id = 1; id <= 3; ++id) {
    first_three = Concat(first_three, PingFrameBytes(id, "m"));
  }
  const Bytes fourth = PingFrameBytes(4, "tail");

  ASSERT_TRUE(client->SendRaw(Concat(first_three, Bytes(fourth.begin(), fourth.begin() + 5))));
  for (std::uint64_t id = 1; id <= 3; ++id) {
    protocol::PingResponse response;
    ASSERT_EQ(client->ReceivePingResponse(response, kTimeout), Status::kFrame);
    EXPECT_EQ(response.request_id(), id);
  }

  // 네 번째 프레임은 아직 응답이 없어야 한다.
  protocol::PingResponse none;
  EXPECT_EQ(client->ReceivePingResponse(none, Millis(150)), Status::kTimeout);

  ASSERT_TRUE(client->SendRaw(std::span<const std::uint8_t>(fourth.data() + 5, fourth.size() - 5)));
  protocol::PingResponse response;
  ASSERT_EQ(client->ReceivePingResponse(response, kTimeout), Status::kFrame);
  EXPECT_EQ(response.request_id(), 4u);
  EXPECT_EQ(response.payload(), "tail");
}

// ============================================================ 비정상 길이

class InvalidLengthTest : public ::testing::TestWithParam<std::uint32_t> {};

TEST_P(InvalidLengthTest, ConnectionIsClosedAndServerKeepsRunning) {
  TestServer server;
  auto client = Connect(server);

  ASSERT_TRUE(client->SendRaw(LengthField(GetParam())));
  EXPECT_TRUE(client->WaitForServerClose(kTimeout)) << "길이 " << GetParam();

  EXPECT_TRUE(WaitUntil([&] { return Load(server.stats().decode_errors) == 1; }));
  EXPECT_TRUE(ServerStillWorks(server));
}

// 길이 0, 1은 최소(2)보다 작고, 65,537 이상은 최대(65,536)보다 크다.
INSTANTIATE_TEST_SUITE_P(Lengths, InvalidLengthTest,
                         ::testing::Values(0u, 1u, 65537u, 1000000u, 0x7FFFFFFFu, 0xFFFFFFFFu));

// ============================================================ 알 수 없는 메시지 ID

class UnknownMessageIdTest : public ::testing::TestWithParam<std::uint16_t> {};

TEST_P(UnknownMessageIdTest, ConnectionIsClosedAndServerKeepsRunning) {
  TestServer server;
  auto client = Connect(server);

  // 내용이 올바른 PingRequest여도 ID가 서버가 받는 요청(1)이 아니면 거부해야 한다.
  protocol::PingRequest request;
  request.set_request_id(1);
  request.set_payload("x");
  Bytes body(request.ByteSizeLong());
  request.SerializeToArray(body.data(), static_cast<int>(body.size()));
  ASSERT_TRUE(client->SendFrame(GetParam(), body));

  EXPECT_TRUE(client->WaitForServerClose(kTimeout)) << "message_id " << GetParam();
  EXPECT_TRUE(WaitUntil([&] { return Load(server.stats().unknown_message_ids) == 1; }));
  EXPECT_TRUE(ServerStillWorks(server));
}

// 0, 99, 65535는 정의되지 않은 ID. 2(PingResponse)는 서버가 보내는 쪽이라 요청으로는 받지 않는다.
INSTANTIATE_TEST_SUITE_P(Ids, UnknownMessageIdTest, ::testing::Values(0, 2, 3, 99, 65535));

// ============================================================ 잘못된 Protobuf

class InvalidProtobufTest : public ::testing::TestWithParam<Bytes> {};

TEST_P(InvalidProtobufTest, ConnectionIsClosedAndServerKeepsRunning) {
  TestServer server;
  auto client = Connect(server);

  ASSERT_TRUE(client->SendFrame(protocol::ToUint16(protocol::MessageId::kPingRequest), GetParam()));
  EXPECT_TRUE(client->WaitForServerClose(kTimeout));
  EXPECT_TRUE(WaitUntil([&] { return Load(server.stats().parse_errors) == 1; }));
  EXPECT_TRUE(ServerStillWorks(server));
}

INSTANTIATE_TEST_SUITE_P(
    Payloads, InvalidProtobufTest,
    ::testing::Values(Bytes{0x08, 0xFF, 0xFF},         // varint가 끝나지 않고 잘림
                      Bytes{0x12, 0x7F, 'a', 'b'},      // 길이 127을 선언했지만 2바이트뿐
                      Bytes{0x00, 0x01},                // 필드 번호 0은 허용되지 않음
                      Bytes{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));

// ============================================================ 전송 도중 연결 종료

TEST(ServerIntegration, ClosingInTheMiddleOfHeaderIsCleanedUp) {
  TestServer server;
  {
    auto client = Connect(server);
    ASSERT_TRUE(client->SendRaw(Bytes{0x00, 0x00}));  // 길이 필드 4바이트 중 2바이트만
    client->Close();
  }
  EXPECT_TRUE(WaitUntil([&] { return Load(server.stats().truncated_connections) == 1; }));
  EXPECT_TRUE(WaitUntil([&] { return Load(server.stats().current_connections) == 0; }));
  EXPECT_TRUE(ServerStillWorks(server));
}

TEST(ServerIntegration, ClosingInTheMiddleOfBodyIsCleanedUp) {
  TestServer server;
  {
    auto client = Connect(server);
    const Bytes wire = PingFrameBytes(5, "truncated");
    ASSERT_TRUE(client->SendRaw(std::span<const std::uint8_t>(wire.data(), wire.size() - 3)));
    client->Close();
  }
  EXPECT_TRUE(WaitUntil([&] { return Load(server.stats().truncated_connections) == 1; }));
  EXPECT_TRUE(WaitUntil([&] { return Load(server.stats().current_connections) == 0; }));
  EXPECT_TRUE(ServerStillWorks(server));
}

TEST(ServerIntegration, ClosingBetweenFramesIsNotCountedAsTruncated) {
  TestServer server;
  {
    auto client = Connect(server);
    ASSERT_TRUE(PingRoundTrip(*client, 1, "done"));
    client->Close();  // 프레임 경계에서 정상 종료
  }
  EXPECT_TRUE(WaitUntil([&] { return Load(server.stats().current_connections) == 0; }));
  EXPECT_EQ(Load(server.stats().truncated_connections), 0u);
}

// ============================================================ 여러 연결

TEST(ServerIntegration, ManyConcurrentConnections) {
  constexpr int kConnections = 100;
  TestServer server;

  std::vector<std::unique_ptr<BlockingClient>> clients;
  for (int i = 0; i < kConnections; ++i) {
    clients.push_back(Connect(server));
  }
  for (int i = 0; i < kConnections; ++i) {
    ASSERT_TRUE(clients[i]->SendPing(static_cast<std::uint64_t>(i), "c" + std::to_string(i)));
  }
  for (int i = 0; i < kConnections; ++i) {
    protocol::PingResponse response;
    ASSERT_EQ(clients[i]->ReceivePingResponse(response, kTimeout), Status::kFrame) << "연결 " << i;
    EXPECT_EQ(response.request_id(), static_cast<std::uint64_t>(i));
    EXPECT_EQ(response.payload(), "c" + std::to_string(i));
  }

  EXPECT_EQ(Load(server.stats().total_accepted), static_cast<std::uint64_t>(kConnections));
  EXPECT_EQ(Load(server.stats().peak_connections), static_cast<std::uint64_t>(kConnections));
}

TEST(ServerIntegration, IdleConnectionDoesNotBlockOthers) {
  TestServer server;
  auto idle = Connect(server);  // 연결만 하고 아무것도 보내지 않는다.
  EXPECT_TRUE(ServerStillWorks(server));
  EXPECT_TRUE(ServerStillWorks(server));
}

TEST(ServerIntegration, BadConnectionDoesNotAffectGoodConnection) {
  TestServer server;
  auto good = Connect(server);
  auto bad = Connect(server);

  ASSERT_TRUE(PingRoundTrip(*good, 1, "before"));
  ASSERT_TRUE(bad->SendRaw(LengthField(0)));
  EXPECT_TRUE(bad->WaitForServerClose(kTimeout));
  EXPECT_TRUE(PingRoundTrip(*good, 2, "after"));  // 다른 연결의 오류가 이 연결에 영향이 없어야 한다.
}

// ============================================================ 느린 수신자

TEST(ServerIntegration, ClientThatNeverReadsIsDisconnectedWhenSendQueueOverflows) {
  server::ServerConfig config = DefaultServerConfig();
  config.session.max_pending_send_bytes = 200000;  // 테스트가 빨리 끝나도록 작은 한도
  TestServer server(config);

  auto client = Connect(server);
  const std::string payload(10000, 'S');
  // 요청만 계속 보내고 응답은 읽지 않는다. 서버가 연결을 끊으면 전송이 실패해서 반복이 끝난다.
  for (std::uint64_t id = 0; id < 6000; ++id) {
    if (!client->SendPing(id, payload)) {
      break;
    }
  }

  EXPECT_TRUE(WaitUntil([&] { return Load(server.stats().send_queue_overflows) == 1; }));
  EXPECT_TRUE(client->WaitForServerClose(kTimeout));
  EXPECT_TRUE(ServerStillWorks(server));
}

// ============================================================ 서버 종료

TEST(ServerIntegration, StopClosesAllOpenConnections) {
  TestServer server;
  std::vector<std::unique_ptr<BlockingClient>> clients;
  for (int i = 0; i < 3; ++i) {
    clients.push_back(Connect(server));
  }
  ASSERT_TRUE(WaitUntil([&] { return Load(server.stats().current_connections) == 3; }));

  server.Stop();

  for (auto& client : clients) {
    EXPECT_TRUE(client->WaitForServerClose(kTimeout));
  }
  EXPECT_EQ(Load(server.stats().current_connections), 0u);
}

TEST(ServerIntegration, StoppedServerRefusesNewConnections) {
  TestServer server;
  const std::uint16_t port = server.port();
  server.Stop();

  BlockingClient client;
  EXPECT_FALSE(client.Connect("127.0.0.1", port, Millis(1000)));
}

// ============================================================ 입출력 스레드 (io_threads > 0)
//
// 소켓 읽기/쓰기를 논리 스레드 밖의 여러 스레드가 맡을 때만 생길 수 있는 문제를 겨냥한다: 한 연결 안의 응답
// 순서, 부분 쓰기, 전송 중 닫힘, 송신 큐 초과, 다른 연결과 서로 영향을 주지 않기, 트래픽 중 서버 종료.
// 환경변수와 상관없이 항상 입출력 스레드 4개로 돌린다. (나머지 테스트는 *_io4 ctest 항목이 같은 서버 설정으로 반복한다)

server::ServerConfig IoThreadConfig() {
  server::ServerConfig config = DefaultServerConfig();
  config.io_threads = 4;
  return config;
}

TEST(ServerIntegrationIoThreads, ResponsesKeepRequestOrderOnEveryConnectionUnderConcurrency) {
  constexpr int kConnections = 24;
  constexpr int kRequests = 150;
  TestServer server(IoThreadConfig());

  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int c = 0; c < kConnections; ++c) {
    threads.emplace_back([&, c] {
      BlockingClient client;
      if (!client.Connect("127.0.0.1", server.port())) {
        ++failures;
        return;
      }
      // 한 번에 몰아서 보내 파이프라인으로 처리하게 한다. 페이로드 크기를 섞어(큰 것은 부분 쓰기를 일으킨다)
      // 쓰기 도중에 다음 응답이 큐에 들어오는 경우를 만든다.
      Bytes all;
      std::vector<std::string> payloads;
      for (int i = 0; i < kRequests; ++i) {
        const std::size_t size = (i % 10 == 0) ? 50000 : static_cast<std::size_t>(1 + (i * 7 + c) % 200);
        payloads.emplace_back(size, static_cast<char>('a' + (i + c) % 26));
        const Bytes frame = PingFrameBytes(static_cast<std::uint64_t>(c) * 1000 + static_cast<std::uint64_t>(i), payloads.back());
        all.insert(all.end(), frame.begin(), frame.end());
      }
      if (!client.SendRaw(all)) {
        ++failures;
        return;
      }
      for (int i = 0; i < kRequests; ++i) {
        protocol::PingResponse response;
        if (client.ReceivePingResponse(response, kTimeout) != Status::kFrame ||
            response.request_id() != static_cast<std::uint64_t>(c) * 1000 + static_cast<std::uint64_t>(i) ||
            response.payload() != payloads[static_cast<std::size_t>(i)]) {
          ++failures;
          return;
        }
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(Load(server.stats().frames_received), static_cast<std::uint64_t>(kConnections) * kRequests);
  EXPECT_TRUE(WaitUntil([&] {
    return Load(server.stats().frames_sent) == static_cast<std::uint64_t>(kConnections) * kRequests;
  }));
}

TEST(ServerIntegrationIoThreads, ClientsThatDisconnectWhileServerIsSendingDoNotBreakTheServer) {
  constexpr int kRounds = 30;
  TestServer server(IoThreadConfig());

  // 큰 응답이 쓰이는 도중에(또는 큐에 쌓인 채로) 상대가 연결을 끊는 상황을 반복한다.
  const std::string payload(60000, 'D');
  for (int round = 0; round < kRounds; ++round) {
    std::vector<std::unique_ptr<BlockingClient>> clients;
    for (int i = 0; i < 4; ++i) {
      clients.push_back(Connect(server));
      Bytes burst;
      for (int k = 0; k < 20; ++k) {
        const Bytes frame = PingFrameBytes(static_cast<std::uint64_t>(k), payload);
        burst.insert(burst.end(), frame.begin(), frame.end());
      }
      clients.back()->SendRaw(burst);
    }
    clients.clear();  // 응답을 읽지 않고 바로 닫는다.
  }

  EXPECT_TRUE(WaitUntil([&] { return Load(server.stats().current_connections) == 0; }));
  EXPECT_EQ(Load(server.stats().internal_errors), 0u);
  EXPECT_TRUE(ServerStillWorks(server));
}

TEST(ServerIntegrationIoThreads, SlowClientOverflowDoesNotDisturbAHealthyClient) {
  server::ServerConfig config = IoThreadConfig();
  config.session.max_pending_send_bytes = 200000;
  TestServer server(config);

  auto slow = Connect(server);
  auto healthy = Connect(server);

  std::atomic<bool> stop{false};
  std::atomic<int> healthy_failures{0};
  std::atomic<int> healthy_done{0};
  std::thread healthy_thread([&] {
    for (std::uint64_t id = 1; !stop.load(); ++id) {
      if (!PingRoundTrip(*healthy, id, std::string(2000, 'h'))) {
        ++healthy_failures;
        return;
      }
      ++healthy_done;
    }
  });

  const std::string payload(10000, 'S');
  for (std::uint64_t id = 0; id < 6000; ++id) {
    if (!slow->SendPing(id, payload)) {
      break;
    }
  }
  EXPECT_TRUE(WaitUntil([&] { return Load(server.stats().send_queue_overflows) == 1; }));
  EXPECT_TRUE(slow->WaitForServerClose(kTimeout));
  const int done_at_overflow = healthy_done.load();
  EXPECT_TRUE(WaitUntil([&] { return healthy_done.load() >= done_at_overflow + 20; }));  // 끊긴 뒤에도 계속 정상
  stop = true;
  healthy_thread.join();

  EXPECT_EQ(healthy_failures.load(), 0);
  EXPECT_EQ(Load(server.stats().send_queue_overflows), 1u);  // 정상 연결은 한 번도 넘치지 않았다
}

TEST(ServerIntegrationIoThreads, StopWhileTrafficIsFlowingClosesEverythingAndEndsCleanly) {
  constexpr int kConnections = 16;
  TestServer server(IoThreadConfig());

  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;
  std::atomic<int> closed_seen{0};
  for (int c = 0; c < kConnections; ++c) {
    threads.emplace_back([&, c] {
      BlockingClient client;
      if (!client.Connect("127.0.0.1", server.port())) {
        return;
      }
      const std::string payload(3000, 't');
      for (std::uint64_t id = 0; !stop.load(); ++id) {
        if (!client.SendPing(id, payload)) {
          break;  // 서버가 닫음
        }
        protocol::PingResponse response;
        if (client.ReceivePingResponse(response, kTimeout) != Status::kFrame) {
          break;
        }
      }
      if (client.WaitForServerClose(kTimeout)) {
        ++closed_seen;
      }
      (void)c;
    });
  }
  ASSERT_TRUE(WaitUntil([&] { return Load(server.stats().current_connections) == kConnections; }));
  std::this_thread::sleep_for(Millis(200));  // 트래픽이 한창일 때

  server.Stop();
  stop = true;
  for (auto& t : threads) {
    t.join();
  }
  EXPECT_EQ(closed_seen.load(), kConnections);
  EXPECT_EQ(Load(server.stats().current_connections), 0u);
  EXPECT_EQ(Load(server.stats().internal_errors), 0u);
}

}  // namespace
}  // namespace game
