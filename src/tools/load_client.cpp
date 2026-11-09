// load_client: 서버에 많은 연결을 만들어 Ping을 보내고 지연, 처리량, 오류, 서버 자원 사용량을 재는 부하 생성기.
// (3단계. 측정 방법과 합격 기준은 docs/stage3.md가 기준이다)
//
// 한 번의 실행 흐름
//   1. 연결 수립: 연결 수를 속도 제한을 걸어 맺는다.
//   2. 워밍업: 요청을 보내지만 결과는 기록하지 않는다.
//   3. 측정: 요청을 보내고 기록한다. 서버 프로세스의 CPU와 메모리를 1초마다 읽는다.
//   4. 마무리: 더 보내지 않고 남은 응답을 잠시 기다린 뒤 결과를 출력한다.
//
// 중요한 설계
//  - 열린 루프(open loop): 요청은 응답을 기다리지 않고 "정해진 시각"에 보낸다. 서버가 느려져도 보내는
//    간격을 늦추지 않으므로 지연이 숨겨지지 않는다. (닫힌 루프는 서버가 느릴수록 요청을 덜 보내 지연을 낮게
//    보이게 하는 "조정된 누락" 문제가 있다)
//  - 생성기 포화 감지: 예정 시각보다 실제로 얼마나 늦게 보냈는지(스케줄 지연)와 생성기 자신의 CPU를 잰다.
//    생성기가 한계에 닿으면 그 측정은 서버의 한계가 아니므로 합격/불합격을 판정하지 않는다.
//  - 요청마다 request_id와 payload를 대조한다. 응답이 다르면 오류로 센다.
//
// 연결은 --threads 개의 작업 스레드가 나눠 맡는다. 스레드마다 자기 io_context, 연결, 통계를 가져서 잠금이
// 필요 없고, 측정이 끝난 뒤에만 결과를 합친다. 한 스레드로는 초당 4만 건쯤에서 생성기가 먼저 포화되므로
// (results/stage3/baseline.md) 큰 부하는 스레드를 늘려서 만든다. 프로세스 CPU/메모리 측정은 윈도우에서만 지원한다.
//
// 종료 코드: 0=합격, 1=불합격, 2=인자 오류/실행 실패, 3=생성기 포화로 판정 불가

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <timeapi.h>  // timeBeginPeriod: 윈도우 타이머 해상도 조정
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio.hpp>

#include "common/error_text.h"
#include "ping.pb.h"
#include "protocol/frame.h"
#include "protocol/message_ids.h"

namespace {

using Clock = std::chrono::steady_clock;
using boost::asio::ip::tcp;
namespace proto = game::protocol;

// ============================================================ 옵션

struct Options {
  std::string host = "127.0.0.1";
  std::uint16_t port = 7777;
  std::uint32_t connections = 100;
  std::string mode = "active";          // idle | active
  double rate = 10.0;                   // active: 연결당 초당 요청 수
  double idle_interval_s = 5.0;         // idle: Ping 간격(초)
  std::uint32_t payload = 64;           // 요청 payload 바이트
  std::uint32_t warmup_s = 5;
  std::uint32_t duration_s = 30;
  std::uint32_t threads = 1;            // 연결을 나눠 맡는 작업 스레드 수
  std::uint32_t connect_batch = 200;    // 한 번에 시작하는 연결 수(전체, 스레드에 나눔)
  std::uint32_t connect_gap_ms = 20;    // 연결 묶음 사이 간격
  std::uint32_t drain_ms = 3000;        // 측정이 끝난 뒤 남은 응답을 기다리는 최대 시간
  std::uint32_t server_pid = 0;         // 서버 프로세스의 CPU/메모리를 읽을 PID (0이면 생략)
  std::string label;                    // 결과 구분용 이름
  std::string csv_path;                 // 지정하면 결과 한 줄을 이 파일에 덧붙인다

  // 합격 기준 (docs/stage3.md에서 측정 전에 확정한 값)
  double min_connect_rate = 0.999;
  double max_error_rate = 0.001;
  double max_p99_ms = 50.0;
  double max_mem_growth = 1.05;
  double max_lag_p99_ms = 5.0;
  double max_generator_cpu = 80.0;      // 한 코어 기준 %
};

void PrintUsage(const char* program) {
  std::cout
      << "사용법: " << program << " [옵션]\n"
      << "  --host <주소>        서버 주소 (기본 127.0.0.1)\n"
      << "  --port <번호>        서버 포트 (기본 7777)\n"
      << "  --connections <N>    동시 연결 수 (기본 100)\n"
      << "  --mode <idle|active> idle: 5초마다 Ping, active: 연결당 초당 --rate건 (기본 active)\n"
      << "  --rate <건/초>       active 모드에서 연결당 초당 요청 수 (기본 10)\n"
      << "  --payload <바이트>   요청 payload 크기 (기본 64)\n"
      << "  --warmup <초>        워밍업 시간 (기본 5)\n"
      << "  --duration <초>      측정 시간 (기본 30)\n"
      << "  --server-pid <PID>   서버 프로세스의 CPU/메모리를 함께 측정 (윈도우)\n"
      << "  --label <이름>       결과에 붙일 이름\n"
      << "  --csv <파일>         결과 한 줄을 CSV 파일에 덧붙임\n"
      << "  --threads <N>        연결을 나눠 맡는 작업 스레드 수 (기본 1)\n"
      << "  --connect-batch <N>  한 번에 시작하는 연결 수 (기본 200)\n"
      << "  --help               이 도움말\n"
      << "종료 코드: 0=합격, 1=불합격, 2=실행 실패, 3=생성기 포화로 판정 불가\n";
}

bool ParseDouble(const std::string& text, double& out) {
  char* end = nullptr;
  out = std::strtod(text.c_str(), &end);
  return end != text.c_str() && *end == '\0' && out >= 0.0;
}

bool ParseUint(const std::string& text, std::uint32_t max_value, std::uint32_t& out) {
  if (text.empty()) return false;
  std::uint64_t value = 0;
  for (const char ch : text) {
    if (ch < '0' || ch > '9') return false;
    value = value * 10 + static_cast<std::uint64_t>(ch - '0');
    if (value > max_value) return false;
  }
  out = static_cast<std::uint32_t>(value);
  return true;
}

// 0: 실행, 1: 정상 종료(--help), 2: 인자 오류
int ParseArgs(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      return 1;
    }
    if (i + 1 >= argc) {
      std::cerr << "옵션 값이 없습니다: " << arg << "\n";
      return 2;
    }
    const std::string value = argv[++i];
    std::uint32_t n = 0;
    bool ok = true;
    if (arg == "--host") o.host = value;
    else if (arg == "--port") { ok = ParseUint(value, 65535, n) && n > 0; o.port = static_cast<std::uint16_t>(n); }
    else if (arg == "--connections") { ok = ParseUint(value, 200000, n) && n > 0; o.connections = n; }
    else if (arg == "--mode") { ok = value == "idle" || value == "active"; o.mode = value; }
    else if (arg == "--rate") ok = ParseDouble(value, o.rate) && o.rate > 0.0;
    else if (arg == "--payload") { ok = ParseUint(value, 60000, n); o.payload = n; }
    else if (arg == "--warmup") { ok = ParseUint(value, 3600, n); o.warmup_s = n; }
    else if (arg == "--duration") { ok = ParseUint(value, 86400, n) && n > 0; o.duration_s = n; }
    else if (arg == "--server-pid") { ok = ParseUint(value, 0xFFFFFFFF, n); o.server_pid = n; }
    else if (arg == "--threads") { ok = ParseUint(value, 256, n) && n > 0; o.threads = n; }
    else if (arg == "--label") o.label = value;
    else if (arg == "--csv") o.csv_path = value;
    else if (arg == "--connect-batch") { ok = ParseUint(value, 100000, n) && n > 0; o.connect_batch = n; }
    else { std::cerr << "알 수 없는 옵션: " << arg << "\n"; PrintUsage(argv[0]); return 2; }
    if (!ok) {
      std::cerr << "잘못된 값입니다: " << arg << " " << value << "\n";
      return 2;
    }
  }
  return 0;
}

// ============================================================ 프로세스 자원 측정 (윈도우)

struct ProcessUsage {
  bool valid = false;
  double cpu_seconds = 0.0;          // 사용자 + 커널 시간 누적
  std::uint64_t working_set = 0;     // 지금 실제 메모리에 올라와 있는 크기(바이트)
  std::uint64_t peak_working_set = 0;
};

// pid가 0이면 이 프로세스 자신을 읽는다.
ProcessUsage ReadUsage(std::uint32_t pid) {
  ProcessUsage usage;
#ifdef _WIN32
  HANDLE process = pid == 0 ? GetCurrentProcess()
                            : OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (process == nullptr) return usage;
  FILETIME created, exited, kernel, user;
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  if (GetProcessTimes(process, &created, &exited, &kernel, &user) &&
      GetProcessMemoryInfo(process, &counters, sizeof(counters))) {
    const auto to_seconds = [](const FILETIME& t) {
      return static_cast<double>((static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime) / 1e7;
    };
    usage.valid = true;
    usage.cpu_seconds = to_seconds(kernel) + to_seconds(user);
    usage.working_set = counters.WorkingSetSize;
    usage.peak_working_set = counters.PeakWorkingSetSize;
  }
  if (pid != 0) CloseHandle(process);
#else
  (void)pid;  // 리눅스는 /proc을 읽어야 한다. 아직 구현하지 않았다. (Linux 측정은 보류)
#endif
  return usage;
}

// ============================================================ 공유 상태와 결과

// 모든 연결이 함께 쓰는 상태. io_context 스레드 하나에서만 접근하므로 잠금이 없다.
struct Shared {
  Options options;
  Clock::duration interval{};      // 연결당 요청 간격
  bool measuring = false;          // 지금 보내는 요청의 결과를 기록하는 중인가
  bool sending = true;             // 요청을 계속 보낼 것인가
  bool closing = false;            // 종료 중이면 연결이 끊겨도 오류로 세지 않는다

  std::uint64_t connect_ok = 0, connect_fail = 0;
  std::uint64_t sent = 0, received = 0, mismatches = 0, send_errors = 0, disconnects = 0;
  std::vector<std::uint32_t> rtt_us;   // 측정 구간에 보낸 요청의 왕복 시간(마이크로초)
  std::vector<std::uint32_t> lag_us;   // 예정 시각 대비 실제 전송 지연(마이크로초)
  std::atomic<std::uint64_t>* total_done = nullptr;  // 모든 작업 스레드의 연결 시도 완료 수 (조정자가 읽는다)
};

// payload 내용을 request_id와 위치로 정한다. 응답을 받으면 같은 규칙으로 다시 만들어 대조한다.
void FillPayload(std::string& out, std::uint64_t id, std::size_t size) {
  out.resize(size);
  for (std::size_t i = 0; i < size; ++i) out[i] = static_cast<char>('a' + (id + i) % 26);
}

std::uint32_t ToMicros(Clock::duration d) {
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(d).count();
  return us < 0 ? 0u : (us > 0xFFFFFFFFLL ? 0xFFFFFFFFu : static_cast<std::uint32_t>(us));
}

// ============================================================ 연결 하나

class Conn : public std::enable_shared_from_this<Conn> {
 public:
  Conn(boost::asio::io_context& io, Shared& shared) : shared_(shared), socket_(io), timer_(io) {}

  void Connect(const tcp::endpoint& endpoint) {
    auto self = shared_from_this();
    socket_.async_connect(endpoint, [this, self](const boost::system::error_code& ec) {
      if (ec) {
        ++shared_.connect_fail;
      } else {
        ++shared_.connect_ok;
        connected_ = true;
        boost::system::error_code ignored;
        socket_.set_option(tcp::no_delay(true), ignored);
        DoRead();
      }
      if (shared_.total_done) shared_.total_done->fetch_add(1);  // 연결 수와 실패 수를 센 뒤에 알린다
    });
  }

  bool connected() const { return connected_ && !closed_; }

  // first_send 시각부터 interval 간격으로 요청을 보내기 시작한다.
  void StartTraffic(Clock::time_point first_send) {
    next_ = first_send;
    ArmTimer();
  }

  void Close() {
    closed_ = true;
    boost::system::error_code ignored;
    timer_.cancel();
    // 측정이 끝난 뒤 닫을 때는 RST로 끊는다. 일반적인 FIN 종료는 닫은 쪽에 TIME_WAIT를 남기는데, 연결을
    // 만 개 단위로 반복해서 열고 닫으면 윈도우의 임시 포트(약 16,000개)가 TIME_WAIT로 차서 다음 실행의
    // 연결이 실패한다.
    if (shared_.closing) socket_.set_option(boost::asio::socket_base::linger(true, 0), ignored);
    socket_.close(ignored);
  }

  // 측정 구간에 보냈는데 아직 응답을 못 받은 요청 수
  std::uint64_t UnansweredMeasured() const {
    return static_cast<std::uint64_t>(
        std::count_if(pending_.begin(), pending_.end(), [](const Pending& p) { return p.measured; }));
  }

  bool HasPendingMeasured() const { return UnansweredMeasured() > 0; }

 private:
  struct Pending {
    Clock::time_point sent;
    std::uint64_t id;
    bool measured;  // 측정 구간에 보낸 요청인가
  };

  void ArmTimer() {
    auto self = shared_from_this();
    timer_.expires_at(next_);
    timer_.async_wait([this, self](const boost::system::error_code& ec) {
      if (ec || closed_ || !shared_.sending) return;
      const auto now = Clock::now();
      if (shared_.measuring) shared_.lag_us.push_back(ToMicros(now - next_));  // 예정 시각보다 얼마나 늦었나
      SendOne(now);
      next_ += shared_.interval;  // 직전 전송 시각이 아니라 "예정 시각"에서 더한다: 밀려도 간격을 유지(열린 루프)
      ArmTimer();
    });
  }

  void SendOne(Clock::time_point now) {
    const std::uint64_t id = next_id_++;
    FillPayload(payload_, id, shared_.options.payload);
    proto::PingRequest request;
    request.set_request_id(id);
    request.set_payload(payload_);
    std::vector<std::uint8_t> body(request.ByteSizeLong());
    request.SerializeToArray(body.data(), static_cast<int>(body.size()));
    auto wire = proto::EncodeFrame(proto::ToUint16(proto::MessageId::kPingRequest), body);
    if (!wire) {
      ++shared_.send_errors;
      return;
    }
    pending_.push_back(Pending{now, id, shared_.measuring});
    if (shared_.measuring) ++shared_.sent;
    out_.push_back(std::move(*wire));
    if (!writing_) DoWrite();
  }

  void DoWrite() {
    writing_ = true;
    auto self = shared_from_this();
    boost::asio::async_write(socket_, boost::asio::buffer(out_.front()),
                             [this, self](const boost::system::error_code& ec, std::size_t) {
                               if (closed_) return;
                               if (ec) {
                                 if (!shared_.closing) ++shared_.send_errors;
                                 Close();
                                 return;
                               }
                               out_.pop_front();
                               if (out_.empty()) writing_ = false; else DoWrite();
                             });
  }

  void DoRead() {
    auto self = shared_from_this();
    socket_.async_read_some(boost::asio::buffer(read_buffer_),
                            [this, self](const boost::system::error_code& ec, std::size_t bytes) {
                              if (closed_) return;
                              if (ec) {
                                if (!shared_.closing) ++shared_.disconnects;
                                Close();
                                return;
                              }
                              decoder_.Append(std::span<const std::uint8_t>(read_buffer_.data(), bytes));
                              proto::Frame frame;
                              while (decoder_.Next(frame) == proto::DecodeStatus::kFrame) OnResponse(frame);
                              if (decoder_.error() != proto::DecodeError::kNone) {
                                ++shared_.mismatches;
                                Close();
                                return;
                              }
                              DoRead();
                            });
  }

  // 응답 하나를 가장 오래된 미응답 요청과 대조한다. 서버는 연결별 요청 순서를 지키므로 맨 앞과 맞아야 한다.
  void OnResponse(const proto::Frame& frame) {
    const auto now = Clock::now();
    proto::PingResponse response;
    const bool parsed = frame.message_id == proto::ToUint16(proto::MessageId::kPingResponse) &&
                        response.ParseFromArray(frame.payload.data(), static_cast<int>(frame.payload.size()));
    if (pending_.empty()) {
      if (shared_.measuring) ++shared_.mismatches;  // 요청한 적 없는 응답
      return;
    }
    const Pending pending = pending_.front();
    pending_.pop_front();

    bool matches = parsed && response.request_id() == pending.id;
    if (matches) {
      FillPayload(expected_, pending.id, shared_.options.payload);
      matches = response.payload() == expected_;
    }
    if (!pending.measured) return;  // 워밍업 중의 요청은 기록하지 않는다
    if (!matches) {
      ++shared_.mismatches;
      return;
    }
    ++shared_.received;
    shared_.rtt_us.push_back(ToMicros(now - pending.sent));
  }

  Shared& shared_;
  tcp::socket socket_;
  boost::asio::steady_timer timer_;
  bool connected_ = false;
  bool closed_ = false;

  Clock::time_point next_{};
  std::uint64_t next_id_ = 1;
  std::string payload_, expected_;
  std::deque<Pending> pending_;

  std::array<std::uint8_t, 2048> read_buffer_{};
  proto::FrameDecoder decoder_;
  std::deque<std::vector<std::uint8_t>> out_;
  bool writing_ = false;
};

// ============================================================ 실행 흐름과 결과 출력

double Percentile(const std::vector<std::uint32_t>& sorted, double p) {
  if (sorted.empty()) return 0.0;
  const double position = p / 100.0 * static_cast<double>(sorted.size());
  const auto last = static_cast<double>(sorted.size() - 1);
  const auto index = static_cast<std::size_t>(std::min(last, position));
  return static_cast<double>(sorted[index]) / 1000.0;  // 마이크로초 -> 밀리초
}

double MeanMegabytes(const std::vector<std::uint64_t>& v, std::size_t begin, std::size_t end) {
  if (end <= begin) return 0.0;
  double sum = 0.0;
  for (std::size_t i = begin; i < end; ++i) sum += static_cast<double>(v[i]);
  return sum / static_cast<double>(end - begin) / (1024.0 * 1024.0);
}

// 작업 스레드 하나가 맡은 연결들의 결과. 측정이 끝난 뒤 한 번에 모아 합친다.
struct WorkerResult {
  std::uint64_t connect_ok = 0, connect_fail = 0, sent = 0, received = 0, mismatches = 0, send_errors = 0, disconnects = 0;
  std::uint64_t unanswered = 0;  // 측정 구간에 보냈는데 끝까지 응답이 없는 요청
  std::uint64_t alive = 0;       // 측정이 끝났을 때 살아 있는 연결
  std::vector<std::uint32_t> rtt_us, lag_us;
};

// 작업 스레드 하나. 자기 io_context와 연결 묶음을 갖고, 그 연결들의 모든 일을 자기 스레드에서만 한다.
// 그래서 연결과 통계에는 잠금이 없다. 다른 스레드(조정자)가 부탁할 때는 post로 일을 넘기고, 값을 돌려받아야
// 하면 Call로 결과를 기다린다.
class Worker {
 public:
  Worker(const Options& options, Clock::duration interval, std::uint32_t connections,
         std::atomic<std::uint64_t>& total_done, std::uint32_t seed)
      : guard_(boost::asio::make_work_guard(io_)), connections_(connections), connect_timer_(io_), rng_(seed) {
    shared_.options = options;
    shared_.interval = interval;
    shared_.total_done = &total_done;
    thread_ = std::thread([this] { io_.run(); });
  }

  ~Worker() {
    boost::asio::post(io_, [this] {
      shared_.closing = true;
      for (auto& conn : conns_) conn->Close();
      connect_timer_.cancel();
      guard_.reset();
    });
    Join();
  }

  void StartConnecting(const tcp::endpoint& endpoint) {
    boost::asio::post(io_, [this, endpoint] {
      endpoint_ = endpoint;
      ConnectBatch();
    });
  }

  // 연결된 소켓마다 t0 근처(한 간격 안에 흩어 놓은 시각)부터 요청 보내기를 시작한다.
  void BeginTraffic(Clock::time_point t0) {
    boost::asio::post(io_, [this, t0] {
      std::uniform_real_distribution<double> offset(0.0, 1.0);
      const double interval_seconds = std::chrono::duration<double>(shared_.interval).count();
      for (auto& conn : conns_) {
        if (!conn->connected()) continue;
        const auto jitter = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(offset(rng_) * interval_seconds));
        conn->StartTraffic(t0 + jitter);
      }
    });
  }

  void SetMeasuring(bool on) {
    boost::asio::post(io_, [this, on] { shared_.measuring = on; });
  }

  void StopSending() {
    boost::asio::post(io_, [this] {
      shared_.measuring = false;
      shared_.sending = false;
    });
  }

  std::pair<std::uint64_t, std::uint64_t> ConnectCounts() {
    return Call([this] { return std::make_pair(shared_.connect_ok, shared_.connect_fail); });
  }

  // 측정 구간에 보낸 요청 중 아직 응답이 오지 않은 수
  std::uint64_t PendingMeasured() {
    return Call([this] {
      std::uint64_t n = 0;
      for (const auto& conn : conns_) n += conn->UnansweredMeasured();
      return n;
    });
  }

  // 결과를 모으고 연결을 모두 닫는다. 이후 스레드는 할 일이 없어 끝난다.
  WorkerResult Finalize() {
    return Call([this] {
      shared_.closing = true;
      shared_.sending = false;
      shared_.measuring = false;
      WorkerResult r;
      r.connect_ok = shared_.connect_ok;
      r.connect_fail = shared_.connect_fail;
      r.sent = shared_.sent;
      r.received = shared_.received;
      r.mismatches = shared_.mismatches;
      r.send_errors = shared_.send_errors;
      r.disconnects = shared_.disconnects;
      for (const auto& conn : conns_) {
        r.unanswered += conn->UnansweredMeasured();
        if (conn->connected()) ++r.alive;
      }
      r.rtt_us = std::move(shared_.rtt_us);
      r.lag_us = std::move(shared_.lag_us);
      for (auto& conn : conns_) conn->Close();
      connect_timer_.cancel();
      guard_.reset();
      return r;
    });
  }

  void Join() {
    if (thread_.joinable()) thread_.join();
  }

 private:
  // 이 스레드에서 fn을 실행시키고 결과를 기다려 받는다.
  template <typename F>
  auto Call(F fn) -> decltype(fn()) {
    using R = decltype(fn());
    std::promise<R> promise;
    auto future = promise.get_future();
    boost::asio::post(io_, [&] { promise.set_value(fn()); });
    return future.get();
  }

  void ConnectBatch() {
    const Options& o = shared_.options;
    for (std::uint32_t i = 0; i < o.connect_batch && launched_ < connections_; ++i, ++launched_) {
      conns_.push_back(std::make_shared<Conn>(io_, shared_));
      conns_.back()->Connect(endpoint_);
    }
    if (launched_ < connections_) {
      connect_timer_.expires_after(std::chrono::milliseconds(o.connect_gap_ms));
      connect_timer_.async_wait([this](const boost::system::error_code& ec) {
        if (!ec) ConnectBatch();
      });
    }
  }

  // 선언 순서가 곧 초기화 순서다. io_context가 가장 먼저, 스레드가 가장 나중이다.
  boost::asio::io_context io_;
  Shared shared_;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard_;
  std::uint32_t connections_;
  boost::asio::steady_timer connect_timer_;
  std::mt19937_64 rng_;
  tcp::endpoint endpoint_;
  std::uint32_t launched_ = 0;
  std::vector<std::shared_ptr<Conn>> conns_;
  std::thread thread_;
};

// 실행 흐름을 조정하는 쪽(메인 스레드). 작업 스레드들에 단계를 알리고, 서버 자원을 읽고, 결과를 합쳐 출력한다.
class Runner {
 public:
  Runner(boost::asio::io_context& io, Options options)
      : io_(io), options_(std::move(options)), poll_timer_(io), phase_timer_(io), sample_timer_(io), drain_timer_(io) {
    const double seconds = options_.mode == "idle" ? options_.idle_interval_s : 1.0 / options_.rate;
    interval_ = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
  }

  int exit_code() const { return exit_code_; }

  void Start() {
    boost::system::error_code ec;
    tcp::resolver resolver(io_);
    const auto endpoints = resolver.resolve(options_.host, std::to_string(options_.port), ec);
    if (ec || endpoints.empty()) {
      std::cerr << "서버 주소를 해석할 수 없습니다: " << game::common::ErrorText(ec) << "\n";
      exit_code_ = 2;
      return;
    }
    endpoint_ = endpoints.begin()->endpoint();

    // 연결을 작업 스레드들에 나눈다. 나머지는 앞쪽 스레드가 하나씩 더 맡는다.
    threads_ = std::max<std::uint32_t>(1, std::min(options_.threads, options_.connections));
    Options worker_options = options_;
    worker_options.connect_batch = std::max<std::uint32_t>(1, options_.connect_batch / threads_);
    const std::uint32_t base = options_.connections / threads_;
    const std::uint32_t extra = options_.connections % threads_;
    std::random_device random;
    for (std::uint32_t i = 0; i < threads_; ++i) {
      workers_.push_back(std::make_unique<Worker>(worker_options, interval_, base + (i < extra ? 1 : 0), total_done_,
                                                  static_cast<std::uint32_t>(random())));
    }

    connect_started_ = Clock::now();
    std::cout << "연결 수립 시작: " << options_.connections << "개, 작업 스레드 " << threads_ << "개 (스레드당 "
              << worker_options.connect_batch << "개씩 " << options_.connect_gap_ms << "ms 간격)\n";
    for (auto& worker : workers_) worker->StartConnecting(endpoint_);
    PollConnect();
  }

 private:
  // 모든 연결 시도가 끝났는지 20ms마다 확인한다. 너무 오래 걸리면(120초) 된 것만으로 진행한다.
  void PollConnect() {
    const bool all_done = total_done_.load() >= options_.connections;
    const bool timed_out = Clock::now() - connect_started_ > std::chrono::seconds(120);
    if (all_done || timed_out) {
      BeginTraffic();
      return;
    }
    poll_timer_.expires_after(std::chrono::milliseconds(20));
    poll_timer_.async_wait([this](const boost::system::error_code& ec) {
      if (!ec) PollConnect();
    });
  }

  void BeginTraffic() {
    std::uint64_t ok = 0, fail = 0;
    for (auto& worker : workers_) {
      const auto counts = worker->ConnectCounts();
      ok += counts.first;
      fail += counts.second;
    }
    const double connect_seconds = std::chrono::duration<double>(Clock::now() - connect_started_).count();
    std::cout << "연결 완료: 성공 " << ok << ", 실패 " << fail << " (" << std::fixed << std::setprecision(1)
              << connect_seconds << "초)\n";
    if (ok == 0) {
      std::cerr << "연결된 소켓이 없어 측정할 수 없습니다.\n";
      exit_code_ = 2;
      FinishWithoutReport();
      return;
    }

    const auto t0 = Clock::now() + std::chrono::milliseconds(500);
    for (auto& worker : workers_) worker->BeginTraffic(t0);
    std::cout << "워밍업 " << options_.warmup_s << "초 (기록하지 않음)\n";
    phase_timer_.expires_at(t0 + std::chrono::seconds(options_.warmup_s));
    phase_timer_.async_wait([this](const boost::system::error_code& ec) {
      if (!ec) BeginMeasure();
    });
  }

  void BeginMeasure() {
    for (auto& worker : workers_) worker->SetMeasuring(true);
    measuring_ = true;
    measure_started_ = Clock::now();
    server_start_ = ReadUsage(options_.server_pid);
    generator_start_ = ReadUsage(0);
    std::cout << "측정 " << options_.duration_s << "초 시작\n";
    Sample();
    phase_timer_.expires_after(std::chrono::seconds(options_.duration_s));
    phase_timer_.async_wait([this](const boost::system::error_code& ec) {
      if (!ec) EndMeasure();
    });
  }

  // 1초마다 서버 메모리를 읽어 둔다. 메모리가 계속 늘어나는지(누수) 보려는 것이다.
  void Sample() {
    if (!measuring_) return;
    if (options_.server_pid != 0) {
      const ProcessUsage usage = ReadUsage(options_.server_pid);
      if (usage.valid) server_ws_samples_.push_back(usage.working_set);
    }
    sample_timer_.expires_after(std::chrono::seconds(1));
    sample_timer_.async_wait([this](const boost::system::error_code& ec) {
      if (!ec) Sample();
    });
  }

  void EndMeasure() {
    measuring_ = false;
    for (auto& worker : workers_) worker->StopSending();  // 더 보내지 않는다
    measure_seconds_ = std::chrono::duration<double>(Clock::now() - measure_started_).count();
    server_end_ = ReadUsage(options_.server_pid);
    generator_end_ = ReadUsage(0);
    drain_started_ = Clock::now();
    CheckDrain();
  }

  // 측정 구간에 보낸 요청의 응답이 모두 오거나 최대 대기 시간이 지날 때까지 기다린다.
  void CheckDrain() {
    std::uint64_t pending = 0;
    for (auto& worker : workers_) pending += worker->PendingMeasured();
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - drain_started_);
    if (pending == 0 || waited.count() >= options_.drain_ms) {
      Finish();
      return;
    }
    drain_timer_.expires_after(std::chrono::milliseconds(50));
    drain_timer_.async_wait([this](const boost::system::error_code& ec) {
      if (!ec) CheckDrain();
    });
  }

  void Finish() {
    // 작업 스레드들의 결과를 모아 하나로 합친다. 연결은 이 과정에서 닫힌다.
    WorkerResult total;
    for (auto& worker : workers_) {
      WorkerResult r = worker->Finalize();
      total.connect_ok += r.connect_ok;
      total.connect_fail += r.connect_fail;
      total.sent += r.sent;
      total.received += r.received;
      total.mismatches += r.mismatches;
      total.send_errors += r.send_errors;
      total.disconnects += r.disconnects;
      total.unanswered += r.unanswered;
      total.alive += r.alive;
      total.rtt_us.insert(total.rtt_us.end(), r.rtt_us.begin(), r.rtt_us.end());
      total.lag_us.insert(total.lag_us.end(), r.lag_us.begin(), r.lag_us.end());
    }
    for (auto& worker : workers_) worker->Join();
    Report(total);
    CancelTimers();
  }

  void FinishWithoutReport() {
    for (auto& worker : workers_) worker->Finalize();
    for (auto& worker : workers_) worker->Join();
    CancelTimers();
  }

  void CancelTimers() {
    poll_timer_.cancel();
    phase_timer_.cancel();
    sample_timer_.cancel();
    drain_timer_.cancel();
  }

  void Report(WorkerResult& t) {
    const Options& o = options_;
    std::sort(t.rtt_us.begin(), t.rtt_us.end());
    std::sort(t.lag_us.begin(), t.lag_us.end());

    const double p50 = Percentile(t.rtt_us, 50), p95 = Percentile(t.rtt_us, 95), p99 = Percentile(t.rtt_us, 99),
                 p999 = Percentile(t.rtt_us, 99.9), pmax = t.rtt_us.empty() ? 0.0 : t.rtt_us.back() / 1000.0;
    const double lag_p99 = Percentile(t.lag_us, 99);
    const double lag_max = t.lag_us.empty() ? 0.0 : t.lag_us.back() / 1000.0;

    const double connect_rate = static_cast<double>(t.connect_ok) / static_cast<double>(o.connections);
    const double errors = static_cast<double>(t.mismatches + t.unanswered + t.send_errors);
    const double error_rate = t.sent == 0 ? 0.0 : errors / static_cast<double>(t.sent);
    const double throughput = measure_seconds_ > 0 ? static_cast<double>(t.received) / measure_seconds_ : 0.0;

    // 서버/생성기 CPU는 측정 구간 동안 쓴 CPU 시간을 실제 흐른 시간으로 나눈 값(한 코어 기준 %)이다.
    const auto cpu_percent = [this](const ProcessUsage& a, const ProcessUsage& b) {
      return (a.valid && b.valid && measure_seconds_ > 0) ? (b.cpu_seconds - a.cpu_seconds) / measure_seconds_ * 100.0 : -1.0;
    };
    const double server_cpu = cpu_percent(server_start_, server_end_);
    const double generator_cpu = cpu_percent(generator_start_, generator_end_);  // 모든 스레드의 합
    // 포화 판정은 "스레드당 평균"으로 한다. 스레드 하나가 한 코어의 80%를 넘게 쓰면 그 스레드는 일을 따라가기 어렵다.
    const double generator_cpu_per_thread = generator_cpu >= 0 ? generator_cpu / static_cast<double>(threads_) : -1.0;

    // 메모리 증가: 측정 구간의 후반 1/3 평균을 전반 1/3 평균으로 나눈다.
    double mem_growth = -1.0;
    const auto& ws = server_ws_samples_;
    if (ws.size() >= 6) {
      const std::size_t third = ws.size() / 3;
      const double first = MeanMegabytes(ws, 0, third);
      const double last = MeanMegabytes(ws, ws.size() - third, ws.size());
      mem_growth = first > 0 ? last / first : -1.0;
    }

    const double mb = 1024.0 * 1024.0;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n==== 결과" << (o.label.empty() ? "" : " [" + o.label + "]") << " ====\n"
              << "워크로드      : " << o.mode << ", 연결 " << o.connections << "개, 생성기 스레드 " << threads_ << "개, "
              << (o.mode == "idle" ? "Ping 간격 " + std::to_string(o.idle_interval_s) + "초"
                                   : "연결당 " + std::to_string(o.rate) + "건/초")
              << ", payload " << o.payload << "B, 측정 " << measure_seconds_ << "초\n"
              << "연결          : 성공 " << t.connect_ok << " / 실패 " << t.connect_fail << " (성공률 " << connect_rate * 100
              << "%), 측정 끝에 살아 있는 연결 " << t.alive << ", 도중에 끊긴 연결 " << t.disconnects << "\n"
              << "요청          : 전송 " << t.sent << ", 응답 " << t.received << ", 불일치 " << t.mismatches << ", 응답 없음 "
              << t.unanswered << ", 전송 오류 " << t.send_errors << " (오류율 " << error_rate * 100 << "%)\n"
              << "처리량        : " << throughput << " 응답/초\n"
              << "왕복 지연(ms) : p50 " << p50 << "  p95 " << p95 << "  p99 " << p99 << "  p99.9 " << p999 << "  최대 " << pmax << "\n"
              << "생성기 스케줄 지연(ms): p99 " << lag_p99 << "  최대 " << lag_max << "\n";
    if (server_cpu >= 0) {
      std::cout << "서버          : CPU " << server_cpu << "% (한 코어 기준), 메모리 시작 " << server_start_.working_set / mb
                << "MB, 끝 " << server_end_.working_set / mb << "MB, 최대 " << server_end_.peak_working_set / mb << "MB";
      if (mem_growth >= 0) std::cout << ", 후반/전반 " << mem_growth << "배";
      std::cout << "\n";
    } else {
      std::cout << "서버          : CPU/메모리를 측정하지 않음 (--server-pid 필요, 윈도우만 지원)\n";
    }
    if (generator_cpu >= 0) {
      std::cout << "생성기        : 스레드 " << threads_ << "개, CPU 합계 " << generator_cpu << "% (스레드당 평균 "
                << generator_cpu_per_thread << "%), 메모리 " << generator_end_.working_set / mb << "MB\n";
    }

    // ---- 판정 ----
    const bool saturated = lag_p99 > o.max_lag_p99_ms || (generator_cpu_per_thread >= 0 && generator_cpu_per_thread > o.max_generator_cpu);
    const bool connect_pass = connect_rate >= o.min_connect_rate;
    const bool error_pass = error_rate <= o.max_error_rate;
    const bool latency_pass = p99 <= o.max_p99_ms;
    const bool memory_pass = mem_growth < 0 || mem_growth <= o.max_mem_growth;  // 측정하지 못했으면 판정에서 제외
    const bool pass = connect_pass && error_pass && latency_pass && memory_pass;

    const auto mark = [](bool ok) { return ok ? "통과" : "불합격"; };
    std::cout << "\n판정 (기준은 docs/stage3.md)\n"
              << "  연결 성공률 >= " << o.min_connect_rate * 100 << "%      : " << mark(connect_pass) << "\n"
              << "  오류율 <= " << o.max_error_rate * 100 << "%            : " << mark(error_pass) << "\n"
              << "  p99 지연 <= " << o.max_p99_ms << "ms              : " << mark(latency_pass) << "\n"
              << "  메모리 후반/전반 <= " << o.max_mem_growth << "      : " << (mem_growth < 0 ? "측정 안 함" : mark(memory_pass)) << "\n"
              << "  생성기 포화 아님                : " << (saturated ? "포화(아래 참고)" : "통과") << "\n";
    if (saturated) {
      std::cout << "\n[주의] 부하 생성기가 한계에 닿아 이 측정은 서버의 한계를 나타내지 않습니다. 합격/불합격을 판정하지 않습니다.\n";
      exit_code_ = 3;
    } else {
      std::cout << "\n종합: " << (pass ? "합격" : "불합격") << "\n";
      exit_code_ = pass ? 0 : 1;
    }

    if (!o.csv_path.empty()) {
      WriteCsv(t, throughput, p50, p95, p99, p999, pmax, lag_p99, server_cpu, generator_cpu, generator_cpu_per_thread,
               mem_growth, saturated ? "saturated" : (pass ? "pass" : "fail"));
    }
  }

  void WriteCsv(const WorkerResult& t, double throughput, double p50, double p95, double p99, double p999, double pmax,
                double lag_p99, double server_cpu, double generator_cpu, double generator_cpu_per_thread,
                double mem_growth, const char* verdict) {
    const Options& o = options_;
    const bool exists = std::ifstream(o.csv_path).good() && std::ifstream(o.csv_path).peek() != std::ifstream::traits_type::eof();
    std::ofstream file(o.csv_path, std::ios::app);
    if (!file) {
      std::cerr << "CSV 파일을 열 수 없습니다: " << o.csv_path << "\n";
      return;
    }
    if (!exists) {
      file << "label,mode,connections,threads,connect_ok,connect_fail,alive_at_end,disconnects,rate,payload,measure_s,sent,received,"
              "mismatches,unanswered,send_errors,throughput_per_s,p50_ms,p95_ms,p99_ms,p999_ms,max_ms,lag_p99_ms,"
              "server_cpu_pct,server_ws_start_mb,server_ws_end_mb,server_ws_peak_mb,mem_growth,generator_cpu_total_pct,"
              "generator_cpu_per_thread_pct,verdict\n";
    }
    const double mb = 1024.0 * 1024.0;
    file << std::fixed << std::setprecision(3) << o.label << ',' << o.mode << ',' << o.connections << ',' << threads_ << ','
         << t.connect_ok << ',' << t.connect_fail << ',' << t.alive << ',' << t.disconnects << ','
         << (o.mode == "idle" ? 1.0 / o.idle_interval_s : o.rate) << ',' << o.payload << ',' << measure_seconds_ << ',' << t.sent
         << ',' << t.received << ',' << t.mismatches << ',' << t.unanswered << ',' << t.send_errors << ',' << throughput << ','
         << p50 << ',' << p95 << ',' << p99 << ',' << p999 << ',' << pmax << ',' << lag_p99 << ',' << server_cpu << ','
         << server_start_.working_set / mb << ',' << server_end_.working_set / mb << ',' << server_end_.peak_working_set / mb << ','
         << mem_growth << ',' << generator_cpu << ',' << generator_cpu_per_thread << ',' << verdict << '\n';
  }

  boost::asio::io_context& io_;
  Options options_;
  Clock::duration interval_{};
  tcp::endpoint endpoint_;
  std::uint32_t threads_ = 1;
  std::atomic<std::uint64_t> total_done_{0};
  std::vector<std::unique_ptr<Worker>> workers_;
  int exit_code_ = 0;
  bool measuring_ = false;

  boost::asio::steady_timer poll_timer_, phase_timer_, sample_timer_, drain_timer_;

  Clock::time_point connect_started_{}, measure_started_{}, drain_started_{};
  double measure_seconds_ = 0.0;
  ProcessUsage server_start_, server_end_, generator_start_, generator_end_;
  std::vector<std::uint64_t> server_ws_samples_;
};

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  // 윈도우의 기본 타이머 해상도는 약 15.6ms라서, 타이머가 정해진 시각보다 최대 15.6ms 늦게 깨어난다.
  // 그러면 요청을 정확한 간격으로 보낼 수 없고, "스케줄 지연"이 생성기 포화로 잘못 판정된다.
  // 프로세스가 사는 동안 해상도를 1ms로 올린다. (Linux는 기본 해상도가 충분히 작아 필요 없다)
  timeBeginPeriod(1);
  // 윈도우 11은 창이 없거나 백그라운드인 프로세스의 타이머 해상도 요청을 무시할 수 있다. 기준 측정에서
  // 일부 실행만 스케줄 지연이 15ms로 돌아온 것이 이 경우로 의심된다(원인은 증명하지 못했다). 해상도 요청을
  // 무시하지 않도록 이 프로세스를 명시적으로 제외한다.
  PROCESS_POWER_THROTTLING_STATE throttling{};
  throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  throttling.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  throttling.StateMask = 0;  // 0: 요청을 무시하는 동작을 끈다
  SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
#endif
  Options options;
  if (const int parsed = ParseArgs(argc, argv, options); parsed != 0) {
    return parsed == 1 ? 0 : 2;
  }

  boost::asio::io_context io;
  Runner runner(io, options);
  runner.Start();
  io.run();
#ifdef _WIN32
  timeEndPeriod(1);
#endif
  return runner.exit_code();
}
