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
// 스레드 1개로 모든 연결을 비동기로 다룬다. 프로세스 CPU/메모리 측정은 윈도우에서만 지원한다.
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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
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
  std::uint32_t connect_batch = 200;    // 한 번에 시작하는 연결 수
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

  std::uint64_t connect_ok = 0, connect_fail = 0, connect_done = 0;
  std::uint64_t sent = 0, received = 0, mismatches = 0, send_errors = 0, disconnects = 0;
  std::vector<std::uint32_t> rtt_us;   // 측정 구간에 보낸 요청의 왕복 시간(마이크로초)
  std::vector<std::uint32_t> lag_us;   // 예정 시각 대비 실제 전송 지연(마이크로초)
  std::function<void()> on_connect_done;
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
      ++shared_.connect_done;
      if (ec) {
        ++shared_.connect_fail;
      } else {
        ++shared_.connect_ok;
        connected_ = true;
        boost::system::error_code ignored;
        socket_.set_option(tcp::no_delay(true), ignored);
        DoRead();
      }
      if (shared_.on_connect_done) shared_.on_connect_done();
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

class Runner {
 public:
  Runner(boost::asio::io_context& io, Options options)
      : io_(io),
        connect_timer_(io),
        phase_timer_(io),
        sample_timer_(io),
        drain_timer_(io),
        rng_(std::random_device{}()) {
    shared_.options = std::move(options);
    const auto& o = shared_.options;
    const double seconds = o.mode == "idle" ? o.idle_interval_s : 1.0 / o.rate;
    shared_.interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
    shared_.on_connect_done = [this] { OnConnectDone(); };
  }

  int exit_code() const { return exit_code_; }

  void Start() {
    const auto& o = shared_.options;
    boost::system::error_code ec;
    tcp::resolver resolver(io_);
    const auto endpoints = resolver.resolve(o.host, std::to_string(o.port), ec);
    if (ec || endpoints.empty()) {
      std::cerr << "서버 주소를 해석할 수 없습니다: " << game::common::ErrorText(ec) << "\n";
      exit_code_ = 2;
      return;
    }
    endpoint_ = endpoints.begin()->endpoint();
    connect_started_ = Clock::now();
    std::cout << "연결 수립 시작: " << o.connections << "개 (" << o.connect_batch << "개씩 " << o.connect_gap_ms << "ms 간격)\n";
    ConnectBatch();
  }

 private:
  void ConnectBatch() {
    const auto& o = shared_.options;
    for (std::uint32_t i = 0; i < o.connect_batch && launched_ < o.connections; ++i, ++launched_) {
      conns_.push_back(std::make_shared<Conn>(io_, shared_));
      conns_.back()->Connect(endpoint_);
    }
    if (launched_ < o.connections) {
      connect_timer_.expires_after(std::chrono::milliseconds(o.connect_gap_ms));
      connect_timer_.async_wait([this](const boost::system::error_code& ec) { if (!ec) ConnectBatch(); });
    }
  }

  void OnConnectDone() {
    if (traffic_started_ || shared_.connect_done < shared_.options.connections) return;
    BeginTraffic();
  }

  void BeginTraffic() {
    traffic_started_ = true;
    connect_seconds_ = std::chrono::duration<double>(Clock::now() - connect_started_).count();
    std::cout << "연결 완료: 성공 " << shared_.connect_ok << ", 실패 " << shared_.connect_fail << " (" << std::fixed
              << std::setprecision(1) << connect_seconds_ << "초)\n";
    if (shared_.connect_ok == 0) {
      std::cerr << "연결된 소켓이 없어 측정할 수 없습니다.\n";
      exit_code_ = 2;
      Shutdown();
      return;
    }

    // 모든 연결이 같은 순간에 보내면 서버에 순간 폭주가 생긴다. 첫 전송 시각을 한 간격 안에 흩어 놓는다.
    const auto t0 = Clock::now() + std::chrono::milliseconds(500);
    std::uniform_real_distribution<double> offset(0.0, 1.0);
    for (auto& conn : conns_) {
      if (!conn->connected()) continue;
      const auto jitter = std::chrono::duration_cast<Clock::duration>(
          std::chrono::duration<double>(offset(rng_) * std::chrono::duration<double>(shared_.interval).count()));
      conn->StartTraffic(t0 + jitter);
    }
    std::cout << "워밍업 " << shared_.options.warmup_s << "초 (기록하지 않음)\n";
    phase_timer_.expires_at(t0 + std::chrono::seconds(shared_.options.warmup_s));
    phase_timer_.async_wait([this](const boost::system::error_code& ec) { if (!ec) BeginMeasure(); });
  }

  void BeginMeasure() {
    shared_.measuring = true;
    measure_started_ = Clock::now();
    server_start_ = ReadUsage(shared_.options.server_pid);
    generator_start_ = ReadUsage(0);
    std::cout << "측정 " << shared_.options.duration_s << "초 시작\n";
    Sample();
    phase_timer_.expires_after(std::chrono::seconds(shared_.options.duration_s));
    phase_timer_.async_wait([this](const boost::system::error_code& ec) { if (!ec) EndMeasure(); });
  }

  // 1초마다 서버 메모리를 읽어 둔다. 메모리가 계속 늘어나는지(누수) 보려는 것이다.
  void Sample() {
    if (!shared_.measuring) return;
    if (shared_.options.server_pid != 0) {
      const ProcessUsage usage = ReadUsage(shared_.options.server_pid);
      if (usage.valid) server_ws_samples_.push_back(usage.working_set);
    }
    sample_timer_.expires_after(std::chrono::seconds(1));
    sample_timer_.async_wait([this](const boost::system::error_code& ec) { if (!ec) Sample(); });
  }

  void EndMeasure() {
    shared_.measuring = false;
    shared_.sending = false;  // 더 보내지 않는다
    measure_seconds_ = std::chrono::duration<double>(Clock::now() - measure_started_).count();
    server_end_ = ReadUsage(shared_.options.server_pid);
    generator_end_ = ReadUsage(0);
    drain_started_ = Clock::now();
    CheckDrain();
  }

  // 측정 구간에 보낸 요청의 응답이 모두 오거나 최대 대기 시간이 지날 때까지 기다린다.
  void CheckDrain() {
    bool pending = false;
    for (const auto& conn : conns_) {
      if (conn->connected() && conn->HasPendingMeasured()) { pending = true; break; }
    }
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - drain_started_);
    if (!pending || waited.count() >= shared_.options.drain_ms) {
      Finish();
      return;
    }
    drain_timer_.expires_after(std::chrono::milliseconds(50));
    drain_timer_.async_wait([this](const boost::system::error_code& ec) { if (!ec) CheckDrain(); });
  }

  void Finish() {
    shared_.closing = true;
    std::uint64_t unanswered = 0;
    std::uint64_t alive = 0;
    for (const auto& conn : conns_) {
      unanswered += conn->UnansweredMeasured();
      if (conn->connected()) ++alive;
    }
    Report(unanswered, alive);
    Shutdown();
  }

  void Shutdown() {
    shared_.closing = true;
    shared_.sending = false;
    for (auto& conn : conns_) conn->Close();
    connect_timer_.cancel();
    phase_timer_.cancel();
    sample_timer_.cancel();
    drain_timer_.cancel();
  }

  void Report(std::uint64_t unanswered, std::uint64_t alive) {
    const Options& o = shared_.options;
    std::sort(shared_.rtt_us.begin(), shared_.rtt_us.end());
    std::sort(shared_.lag_us.begin(), shared_.lag_us.end());

    const double p50 = Percentile(shared_.rtt_us, 50), p95 = Percentile(shared_.rtt_us, 95),
                 p99 = Percentile(shared_.rtt_us, 99), p999 = Percentile(shared_.rtt_us, 99.9),
                 pmax = shared_.rtt_us.empty() ? 0.0 : shared_.rtt_us.back() / 1000.0;
    const double lag_p99 = Percentile(shared_.lag_us, 99);
    const double lag_max = shared_.lag_us.empty() ? 0.0 : shared_.lag_us.back() / 1000.0;

    const double connect_rate = static_cast<double>(shared_.connect_ok) / static_cast<double>(o.connections);
    const double errors = static_cast<double>(shared_.mismatches + unanswered + shared_.send_errors);
    const double error_rate = shared_.sent == 0 ? 0.0 : errors / static_cast<double>(shared_.sent);
    const double throughput = measure_seconds_ > 0 ? static_cast<double>(shared_.received) / measure_seconds_ : 0.0;

    // 서버/생성기 CPU는 측정 구간 동안 쓴 CPU 시간을 실제 흐른 시간으로 나눈 값(한 코어 기준 %)이다.
    const auto cpu_percent = [this](const ProcessUsage& a, const ProcessUsage& b) {
      return (a.valid && b.valid && measure_seconds_ > 0) ? (b.cpu_seconds - a.cpu_seconds) / measure_seconds_ * 100.0 : -1.0;
    };
    const double server_cpu = cpu_percent(server_start_, server_end_);
    const double generator_cpu = cpu_percent(generator_start_, generator_end_);

    // 메모리 증가: 측정 구간의 후반 1/3 평균을 전반 1/3 평균으로 나눈다.
    double mem_growth = -1.0, ws_first = 0, ws_last = 0;
    const auto& ws = server_ws_samples_;
    if (ws.size() >= 6) {
      const std::size_t third = ws.size() / 3;
      ws_first = MeanMegabytes(ws, 0, third);
      ws_last = MeanMegabytes(ws, ws.size() - third, ws.size());
      mem_growth = ws_first > 0 ? ws_last / ws_first : -1.0;
    }

    const double mb = 1024.0 * 1024.0;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n==== 결과" << (o.label.empty() ? "" : " [" + o.label + "]") << " ====\n"
              << "워크로드      : " << o.mode << ", 연결 " << o.connections << "개, "
              << (o.mode == "idle" ? "Ping 간격 " + std::to_string(o.idle_interval_s) + "초"
                                   : "연결당 " + std::to_string(o.rate) + "건/초")
              << ", payload " << o.payload << "B, 측정 " << measure_seconds_ << "초\n"
              << "연결          : 성공 " << shared_.connect_ok << " / 실패 " << shared_.connect_fail << " (성공률 "
              << connect_rate * 100 << "%), 측정 끝에 살아 있는 연결 " << alive << ", 도중에 끊긴 연결 " << shared_.disconnects << "\n"
              << "요청          : 전송 " << shared_.sent << ", 응답 " << shared_.received << ", 불일치 " << shared_.mismatches
              << ", 응답 없음 " << unanswered << ", 전송 오류 " << shared_.send_errors << " (오류율 " << error_rate * 100 << "%)\n"
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
      std::cout << "생성기        : CPU " << generator_cpu << "% (한 코어 기준), 메모리 " << generator_end_.working_set / mb << "MB\n";
    }

    // ---- 판정 ----
    const bool saturated = lag_p99 > o.max_lag_p99_ms || (generator_cpu >= 0 && generator_cpu > o.max_generator_cpu);
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

    if (!o.csv_path.empty()) WriteCsv(o, unanswered, alive, throughput, p50, p95, p99, p999, pmax, lag_p99, server_cpu,
                                     generator_cpu, mem_growth, saturated ? "saturated" : (pass ? "pass" : "fail"));
  }

  void WriteCsv(const Options& o, std::uint64_t unanswered, std::uint64_t alive, double throughput, double p50,
                double p95, double p99, double p999, double pmax, double lag_p99, double server_cpu,
                double generator_cpu, double mem_growth, const char* verdict) {
    const bool exists = std::ifstream(o.csv_path).good() && std::ifstream(o.csv_path).peek() != std::ifstream::traits_type::eof();
    std::ofstream file(o.csv_path, std::ios::app);
    if (!file) {
      std::cerr << "CSV 파일을 열 수 없습니다: " << o.csv_path << "\n";
      return;
    }
    if (!exists) {
      file << "label,mode,connections,connect_ok,connect_fail,alive_at_end,disconnects,rate,payload,measure_s,sent,received,"
              "mismatches,unanswered,send_errors,throughput_per_s,p50_ms,p95_ms,p99_ms,p999_ms,max_ms,lag_p99_ms,"
              "server_cpu_pct,server_ws_start_mb,server_ws_end_mb,server_ws_peak_mb,mem_growth,generator_cpu_pct,verdict\n";
    }
    const double mb = 1024.0 * 1024.0;
    file << std::fixed << std::setprecision(3) << o.label << ',' << o.mode << ',' << o.connections << ',' << shared_.connect_ok
         << ',' << shared_.connect_fail << ',' << alive << ',' << shared_.disconnects << ',' << (o.mode == "idle" ? 1.0 / o.idle_interval_s : o.rate)
         << ',' << o.payload << ',' << measure_seconds_ << ',' << shared_.sent << ',' << shared_.received << ',' << shared_.mismatches
         << ',' << unanswered << ',' << shared_.send_errors << ',' << throughput << ',' << p50 << ',' << p95 << ',' << p99
         << ',' << p999 << ',' << pmax << ',' << lag_p99 << ',' << server_cpu << ',' << server_start_.working_set / mb << ','
         << server_end_.working_set / mb << ',' << server_end_.peak_working_set / mb << ',' << mem_growth << ',' << generator_cpu
         << ',' << verdict << '\n';
  }

  boost::asio::io_context& io_;
  Shared shared_;
  std::vector<std::shared_ptr<Conn>> conns_;
  tcp::endpoint endpoint_;
  std::uint32_t launched_ = 0;
  bool traffic_started_ = false;
  int exit_code_ = 0;

  boost::asio::steady_timer connect_timer_, phase_timer_, sample_timer_, drain_timer_;
  std::mt19937_64 rng_;

  Clock::time_point connect_started_{}, measure_started_{}, drain_started_{};
  double connect_seconds_ = 0.0, measure_seconds_ = 0.0;
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
