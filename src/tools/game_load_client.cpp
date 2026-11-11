// game_load_client: 4단계 게임 서버의 부하 생성기. 방 R개를 만들고 방마다 4명이 입장해, 플레이어마다 일정한
// 빈도로 MoveInput을 보내며 서버가 보내는 20Hz 스냅샷을 받아 지연, 누락, 정합성을 잰다.
// (3단계 load_client는 Ping만 보내므로 게임 워크로드는 이 도구로 잰다. docs/stage4.md "부하 측정 계획")
//
// 방법은 load_client와 같다.
//  - 열린 루프: 입력은 응답을 기다리지 않고 정해진 시각에 보낸다. 서버가 느려져도 보내는 간격을 늦추지 않는다.
//  - 작업 스레드마다 자기 io_context와 방 묶음을 갖는다. 방 하나의 연결 4개는 같은 스레드가 맡는다.
//  - 생성기 포화 감지: 입력을 정해진 시각보다 늦게 보낸 정도(스케줄 지연)와 생성기 CPU로 판단하고, 포화이면
//    서버의 한계가 아니므로 합격/불합격을 판정하지 않는다.
//  - 합격 기준은 docs/stage4.md에서 측정 전에 확정한 값이다.
//
// 측정하는 것
//  - 스냅샷 도착 간격: 한 클라이언트가 받는 연속한 두 스냅샷 사이 시간
//  - 입력 반영 지연: MoveInput 전송 시각 ~ 그 sequence 이상이 담긴 첫 스냅샷 도착 시각
//  - 누락 스냅샷: 스냅샷의 틱 번호가 건너뛴 만큼
//  - 정합성: 인원, 위치(유한, 맵 안), 틱당 이동 거리 상한(서버 속도), 틱 증가
//  - 반영되지 않은 입력, 끊긴 연결, 서버 CPU/메모리, 받은 스냅샷 대역폭

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <timeapi.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
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
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/asio.hpp>

#include "common/error_text.h"
#include "game.pb.h"
#include "play/play_config.h"
#include "protocol/frame.h"
#include "protocol/message_ids.h"

namespace {

using Clock = std::chrono::steady_clock;
using boost::asio::ip::tcp;
namespace proto = game::protocol;
using game::protocol::MessageId;

// ============================================================ 옵션

struct Options {
  std::string host = "127.0.0.1";
  std::uint16_t port = 7777;
  std::uint32_t rooms = 25;
  std::uint32_t players_per_room = 4;
  double input_rate = 10.0;          // 플레이어당 초당 MoveInput 수
  std::uint32_t warmup_s = 5;
  std::uint32_t duration_s = 30;
  std::uint32_t threads = 4;
  std::uint32_t room_batch = 20;     // 한 번에 시작하는 방 수(전체, 스레드에 나눔)
  std::uint32_t room_gap_ms = 50;    // 방 묶음 사이 간격
  std::uint32_t drain_ms = 3000;     // 측정이 끝난 뒤 남은 입력의 반영을 기다리는 최대 시간
  std::uint32_t server_pid = 0;
  std::string label;
  std::string csv_path;

  // 합격 기준 (docs/stage4.md에서 측정 전에 확정한 값)
  double min_room_rate = 0.999;
  double max_gap_p99_ms = 100.0;
  double max_input_p99_ms = 100.0;
  double max_missing_rate = 0.01;
  double max_unapplied_rate = 0.001;
  double max_mem_growth = 1.05;
  double max_lag_p99_ms = 5.0;
  double max_generator_cpu = 80.0;
};

void PrintUsage(const char* program) {
  std::cout << "사용법: " << program << " [옵션]\n"
            << "  --host <주소> / --port <번호>   서버 (기본 127.0.0.1:7777)\n"
            << "  --rooms <N>             방 수 (기본 25, 방마다 4명)\n"
            << "  --input-rate <Hz>       플레이어당 초당 MoveInput 수 (기본 10)\n"
            << "  --warmup <초> / --duration <초>   워밍업(기본 5) / 측정(기본 30)\n"
            << "  --threads <N>           작업 스레드 수 (기본 4)\n"
            << "  --server-pid <PID>      서버 프로세스의 CPU/메모리를 잰다\n"
            << "  --label <이름> / --csv <파일>      결과 이름 / 결과 한 줄을 덧붙일 CSV\n"
            << "종료 코드: 0 합격, 1 불합격, 2 실행 오류, 3 생성기 포화로 판정 불가\n";
}

bool ParseUint(const std::string& text, std::uint32_t& out) {
  if (text.empty()) return false;
  std::uint64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<std::uint64_t>(c - '0');
    if (value > 0xFFFFFFFFull) return false;
  }
  out = static_cast<std::uint32_t>(value);
  return true;
}

int ParseArgs(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") return 1;
    if (i + 1 >= argc) {
      std::cerr << "옵션 값이 없습니다: " << arg << "\n";
      return 2;
    }
    const std::string value = argv[++i];
    std::uint32_t number = 0;
    const bool numeric = ParseUint(value, number);
    if (arg == "--host") o.host = value;
    else if (arg == "--port" && numeric && number >= 1 && number <= 65535) o.port = static_cast<std::uint16_t>(number);
    else if (arg == "--rooms" && numeric && number >= 1) o.rooms = number;
    else if (arg == "--input-rate") o.input_rate = std::strtod(value.c_str(), nullptr);
    else if (arg == "--warmup" && numeric) o.warmup_s = number;
    else if (arg == "--duration" && numeric && number >= 1) o.duration_s = number;
    else if (arg == "--threads" && numeric && number >= 1) o.threads = number;
    else if (arg == "--server-pid" && numeric) o.server_pid = number;
    else if (arg == "--label") o.label = value;
    else if (arg == "--csv") o.csv_path = value;
    else {
      std::cerr << "알 수 없는 옵션이거나 값이 잘못됨: " << arg << " " << value << "\n";
      return 2;
    }
  }
  if (o.input_rate <= 0.0) {
    std::cerr << "--input-rate는 0보다 커야 합니다.\n";
    return 2;
  }
  return 0;
}

// ============================================================ 프로세스 자원 측정 (윈도우)

struct ProcessUsage {
  bool valid = false;
  double cpu_seconds = 0.0;
  std::uint64_t working_set = 0;
  std::uint64_t peak_working_set = 0;
};

ProcessUsage ReadUsage(std::uint32_t pid) {
  ProcessUsage usage;
#ifdef _WIN32
  HANDLE process = pid == 0 ? GetCurrentProcess() : OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
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
  (void)pid;
#endif
  return usage;
}

std::uint32_t ToMicros(Clock::duration d) {
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(d).count();
  return us < 0 ? 0u : (us > 0xFFFFFFFFLL ? 0xFFFFFFFFu : static_cast<std::uint32_t>(us));
}

// 벽시계 시각 "HH:MM:SS.mmm". 서버 로그의 시각과 같은 형식이라 측정 구간의 서버 통계를 찾는 데 쓴다.
std::string WallClockText() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &seconds);
#else
  localtime_r(&seconds, &local);
#endif
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec,
                static_cast<int>(millis));
  return buffer;
}

// ============================================================ 작업 스레드가 쓰는 상태

class GClient;

// 방 하나. 같은 작업 스레드가 방의 연결 4개를 모두 다루므로 잠금이 없다.
struct RoomCtx {
  std::string code;
  std::vector<std::shared_ptr<GClient>> clients;
  std::uint32_t ready = 0;   // 방 입장을 마친 연결 수
  bool failed = false;
  bool reported = false;     // 방 형성 결과(성공/실패)를 조정자에게 이미 알렸는가
};

// 작업 스레드 하나의 공유 상태와 결과.
struct WorkerCtx {
  Options options;
  game::play::PlayConfig play;                 // 서버 속도/맵 값(정합성 검사용)
  Clock::duration input_interval{};
  tcp::endpoint endpoint;
  boost::asio::io_context* io = nullptr;
  std::mt19937 rng;
  std::atomic<std::uint64_t>* rooms_done = nullptr;  // 방 형성이 끝난(성공/실패) 방 수 (조정자가 읽는다)

  bool measuring = false;
  bool sending = true;
  bool closing = false;

  std::uint64_t rooms_ok = 0, rooms_failed = 0;
  std::uint64_t inputs_sent = 0, inputs_reflected = 0;
  std::uint64_t snapshots = 0, snapshots_missing = 0;
  std::uint64_t violations = 0, roster_mismatches = 0, disconnects = 0, bytes_received = 0;
  std::vector<std::uint32_t> gap_us;      // 스냅샷 도착 간격
  std::vector<std::uint32_t> input_us;    // 입력 반영 지연
  std::vector<std::uint32_t> lag_us;      // 입력 예정 시각 대비 실제 전송 지연
};

void RoomFinished(WorkerCtx& ctx, RoomCtx& room, bool ok) {
  if (room.reported) return;
  room.reported = true;
  if (ok) ++ctx.rooms_ok; else ++ctx.rooms_failed;
  ctx.rooms_done->fetch_add(1);
}

// ============================================================ 연결 하나 (플레이어 한 명)

class GClient : public std::enable_shared_from_this<GClient> {
 public:
  GClient(WorkerCtx& ctx, RoomCtx& room, std::uint32_t index, std::string nickname)
      : ctx_(ctx), room_(room), index_(index), nickname_(std::move(nickname)), socket_(*ctx.io), input_timer_(*ctx.io) {}

  // 연결을 시작한다. 방장(index 0)은 연결 뒤 방을 만들고, 나머지는 방 코드를 받아 입장한다.
  void Connect() {
    auto self = shared_from_this();
    socket_.async_connect(ctx_.endpoint, [this, self](const boost::system::error_code& ec) {
      if (ec) {
        Fail();
        return;
      }
      boost::system::error_code ignored;
      socket_.set_option(tcp::no_delay(true), ignored);
      DoRead();
      if (index_ == 0) {
        proto::CreateRoomRequest request;
        request.set_nickname(nickname_);
        Send(MessageId::kCreateRoomRequest, request);
      } else {
        proto::JoinRoomRequest request;
        request.set_room_code(room_.code);
        request.set_nickname(nickname_);
        Send(MessageId::kJoinRoomRequest, request);
      }
    });
  }

  // 입력 전송을 시작한다. t0 이후 phase만큼 어긋난 시각부터 일정 간격으로 보낸다(연결마다 시각을 흩뜨려
  // 모든 플레이어가 같은 순간에 보내는 인위적인 몰림을 피한다).
  void BeginInputs(Clock::time_point t0) {
    if (!in_room_) return;
    std::uniform_real_distribution<double> phase(0.0, 1.0);
    next_input_ = t0 + std::chrono::duration_cast<Clock::duration>(ctx_.input_interval * phase(ctx_.rng));
    ScheduleInput();
  }

  // 측정 구간에 보낸 입력 중 아직 스냅샷에 반영되지 않은 수.
  std::uint64_t PendingMeasured() const {
    std::uint64_t count = 0;
    for (const auto& p : pending_) if (p.measured) ++count;
    return count;
  }

  // 끊는다. 측정이 끝난 뒤에는 RST로 닫아 TIME_WAIT가 쌓이지 않게 한다.
  void Close() {
    boost::system::error_code ignored;
    input_timer_.cancel();
    socket_.set_option(boost::asio::socket_base::linger(true, 0), ignored);
    socket_.close(ignored);
  }

  bool in_room() const { return in_room_; }

 private:
  struct Pending {
    std::uint32_t seq = 0;
    Clock::time_point sent;
    bool measured = false;
  };

  void Fail() {
    if (ctx_.closing) return;
    room_.failed = true;
    RoomFinished(ctx_, room_, false);
  }

  // ---- 읽기 ----
  void DoRead() {
    auto self = shared_from_this();
    socket_.async_read_some(boost::asio::buffer(read_buffer_),
                            [this, self](const boost::system::error_code& ec, std::size_t bytes) { OnRead(ec, bytes); });
  }

  void OnRead(const boost::system::error_code& ec, std::size_t bytes) {
    if (ctx_.closing) return;
    if (ec) {
      // 방 형성 전이면 방 실패, 방에 들어간 뒤면 끊긴 연결이다.
      if (in_room_) {
        ++ctx_.disconnects;  // 측정 전에 끊겨도 서버 문제이므로 센다
      } else {
        Fail();
      }
      input_timer_.cancel();
      return;
    }
    decoder_.Append(std::span<const std::uint8_t>(read_buffer_.data(), bytes));
    game::protocol::Frame frame;
    while (true) {
      const auto status = decoder_.Next(frame);
      if (status == game::protocol::DecodeStatus::kNeedMoreData) break;
      if (status == game::protocol::DecodeStatus::kError) {
        Fail();
        return;
      }
      OnFrame(frame);
    }
    DoRead();
  }

  void OnFrame(const game::protocol::Frame& frame) {
    const auto arrival = Clock::now();
    switch (static_cast<MessageId>(frame.message_id)) {
      case MessageId::kCreateRoomResponse: {
        proto::CreateRoomResponse r;
        if (!r.ParseFromArray(frame.payload.data(), static_cast<int>(frame.payload.size())) ||
            r.result() != proto::GAME_OK) {
          Fail();
          return;
        }
        room_.code = r.state().room_code();
        player_id_ = r.state().your_player_id();
        EnteredRoom();
        // 방 코드를 알았으니 나머지 플레이어를 연결해 입장시킨다.
        for (std::size_t i = 1; i < room_.clients.size(); ++i) room_.clients[i]->Connect();
        return;
      }
      case MessageId::kJoinRoomResponse: {
        proto::JoinRoomResponse r;
        if (!r.ParseFromArray(frame.payload.data(), static_cast<int>(frame.payload.size())) ||
            r.result() != proto::GAME_OK) {
          Fail();
          return;
        }
        player_id_ = r.state().your_player_id();
        EnteredRoom();
        return;
      }
      case MessageId::kWorldSnapshot:
        OnSnapshot(frame, arrival);
        return;
      default:
        return;  // 방 알림 등은 이 시험에서 쓰지 않는다
    }
  }

  void EnteredRoom() {
    in_room_ = true;
    ++room_.ready;
    if (room_.ready == room_.clients.size()) {
      RoomFinished(ctx_, room_, true);
    }
  }

  // ---- 스냅샷 처리: 간격, 반영 지연, 누락, 정합성 ----
  void OnSnapshot(const game::protocol::Frame& frame, Clock::time_point arrival) {
    proto::WorldSnapshot snapshot;
    if (!snapshot.ParseFromArray(frame.payload.data(), static_cast<int>(frame.payload.size()))) {
      ++ctx_.violations;
      return;
    }
    const bool measuring = ctx_.measuring;

    // 정합성 검사: 인원, 위치, 이동 거리 상한, 틱 증가.
    const std::uint64_t tick = snapshot.tick();
    const bool has_previous = last_tick_ != 0;
    const std::uint64_t delta_ticks = has_previous && tick > last_tick_ ? tick - last_tick_ : 0;
    std::uint64_t violations = 0;
    if (has_previous && tick <= last_tick_) ++violations;
    if (static_cast<std::uint32_t>(snapshot.players_size()) != ctx_.options.players_per_room) {
      if (measuring) ++ctx_.roster_mismatches;
    }
    const float min_edge = ctx_.play.player_radius_cm;
    const float max_x = ctx_.play.map_width_cm - ctx_.play.player_radius_cm;
    const float max_y = ctx_.play.map_height_cm - ctx_.play.player_radius_cm;
    const float per_tick = ctx_.play.move_speed_cm_per_s * ctx_.play.TickSeconds();
    const proto::PlayerState* own = nullptr;
    for (const auto& player : snapshot.players()) {
      if (!std::isfinite(player.x()) || !std::isfinite(player.y()) || player.x() < min_edge - 0.01f ||
          player.x() > max_x + 0.01f || player.y() < min_edge - 0.01f || player.y() > max_y + 0.01f) {
        ++violations;
      }
      if (player.player_id() == player_id_) own = &player;
      const auto previous = last_positions_.find(player.player_id());
      if (previous != last_positions_.end() && delta_ticks > 0) {
        const float dx = player.x() - previous->second.first;
        const float dy = player.y() - previous->second.second;
        const float limit = per_tick * static_cast<float>(delta_ticks) + 0.05f;
        if (std::sqrt(dx * dx + dy * dy) > limit) ++violations;
      }
      last_positions_[player.player_id()] = {player.x(), player.y()};
    }
    if (own == nullptr) ++violations;
    if (measuring) ctx_.violations += violations;

    // 입력 반영 지연: 이 스냅샷이 반영한 마지막 입력 번호까지의 대기 중 입력을 마무리한다.
    if (own != nullptr) {
      while (!pending_.empty() && pending_.front().seq <= own->last_input_seq()) {
        if (pending_.front().measured) {
          ++ctx_.inputs_reflected;
          ctx_.input_us.push_back(ToMicros(arrival - pending_.front().sent));
        }
        pending_.pop_front();
      }
    }

    // 스냅샷 도착 간격과 누락(틱 번호가 건너뛴 만큼).
    if (measuring) {
      ++ctx_.snapshots;
      ctx_.bytes_received += game::protocol::kLengthFieldSize + game::protocol::kMessageIdSize + frame.payload.size();
      if (has_arrival_) ctx_.gap_us.push_back(ToMicros(arrival - last_arrival_));
      if (has_previous && delta_ticks > 1) ctx_.snapshots_missing += delta_ticks - 1;
    }
    last_arrival_ = arrival;
    has_arrival_ = true;
    last_tick_ = tick;
  }

  // ---- 입력 보내기 (열린 루프) ----
  void ScheduleInput() {
    auto self = shared_from_this();
    input_timer_.expires_at(next_input_);
    input_timer_.async_wait([this, self](const boost::system::error_code& ec) {
      if (ec || ctx_.closing || !ctx_.sending) return;
      const auto now = Clock::now();
      if (ctx_.measuring) ctx_.lag_us.push_back(ToMicros(now - next_input_));
      SendInput(now);
      next_input_ += ctx_.input_interval;
      ScheduleInput();
    });
  }

  void SendInput(Clock::time_point now) {
    float dx = 0.0f;
    float dy = 0.0f;
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    if (unit(ctx_.rng) >= 0.1f) {  // 10%는 정지, 나머지는 무작위 방향
      const float angle = unit(ctx_.rng) * 6.2831853f;
      dx = std::cos(angle);
      dy = std::sin(angle);
    }
    proto::MoveInput input;
    input.set_sequence(++sequence_);
    input.set_dir_x(dx);
    input.set_dir_y(dy);
    Send(MessageId::kMoveInput, input);
    pending_.push_back(Pending{sequence_, now, ctx_.measuring});
    if (ctx_.measuring) ++ctx_.inputs_sent;
  }

  void Send(MessageId id, const google::protobuf::MessageLite& message) {
    std::vector<std::uint8_t> body(message.ByteSizeLong());
    message.SerializeToArray(body.data(), static_cast<int>(body.size()));
    auto wire = game::protocol::EncodeFrame(game::protocol::ToUint16(id), body);
    if (!wire) return;
    out_.push_back(std::move(*wire));
    if (!writing_) DoWrite();
  }

  void DoWrite() {
    writing_ = true;
    auto self = shared_from_this();
    boost::asio::async_write(socket_, boost::asio::buffer(out_.front()),
                             [this, self](const boost::system::error_code& ec, std::size_t) {
                               if (ctx_.closing) return;
                               if (ec) {
                                 writing_ = false;
                                 return;  // 읽기 쪽 오류가 연결 끊김으로 처리한다
                               }
                               out_.pop_front();
                               if (out_.empty()) writing_ = false; else DoWrite();
                             });
  }

  WorkerCtx& ctx_;
  RoomCtx& room_;
  const std::uint32_t index_;
  const std::string nickname_;
  tcp::socket socket_;
  boost::asio::steady_timer input_timer_;
  std::array<std::uint8_t, 8192> read_buffer_{};
  game::protocol::FrameDecoder decoder_;
  std::deque<std::vector<std::uint8_t>> out_;
  bool writing_ = false;

  bool in_room_ = false;
  std::uint32_t player_id_ = 0;
  std::uint32_t sequence_ = 0;
  Clock::time_point next_input_{};
  std::deque<Pending> pending_;
  std::uint64_t last_tick_ = 0;
  Clock::time_point last_arrival_{};
  bool has_arrival_ = false;
  std::unordered_map<std::uint32_t, std::pair<float, float>> last_positions_;
};

// ============================================================ 작업 스레드

struct WorkerResult {
  std::uint64_t rooms_ok = 0, rooms_failed = 0, inputs_sent = 0, inputs_reflected = 0, inputs_unapplied = 0;
  std::uint64_t snapshots = 0, snapshots_missing = 0, violations = 0, roster_mismatches = 0, disconnects = 0;
  std::uint64_t bytes_received = 0;
  std::vector<std::uint32_t> gap_us, input_us, lag_us;
};

class Worker {
 public:
  Worker(const Options& options, tcp::endpoint endpoint, std::uint32_t rooms, std::atomic<std::uint64_t>& rooms_done,
         std::uint32_t seed)
      : guard_(boost::asio::make_work_guard(io_)), batch_timer_(io_), room_count_(rooms) {
    ctx_.options = options;
    ctx_.endpoint = endpoint;
    ctx_.io = &io_;
    ctx_.rng.seed(seed);
    ctx_.rooms_done = &rooms_done;
    ctx_.input_interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / options.input_rate));
    thread_ = std::thread([this] { io_.run(); });
  }

  ~Worker() { Join(); }

  // 방을 묶음 단위로 만들어 연결을 시작한다. 한꺼번에 시작하면 서버의 accept와 방 생성이 몰려 측정 전에 과부하가 된다.
  void StartConnecting() {
    boost::asio::post(io_, [this] { StartBatch(); });
  }

  void BeginTraffic(Clock::time_point t0) {
    boost::asio::post(io_, [this, t0] {
      for (auto& room : rooms_) {
        if (room->failed) continue;
        for (auto& client : room->clients) client->BeginInputs(t0);
      }
    });
  }

  void SetMeasuring(bool on) {
    boost::asio::post(io_, [this, on] { ctx_.measuring = on; });
  }

  void StopSending() {
    boost::asio::post(io_, [this] { ctx_.sending = false; });
  }

  std::uint64_t PendingMeasured() {
    return Call([this] {
      std::uint64_t total = 0;
      for (auto& room : rooms_) for (auto& client : room->clients) total += client->PendingMeasured();
      return total;
    });
  }

  WorkerResult Finalize() {
    return Call([this] {
      WorkerResult r;
      r.rooms_ok = ctx_.rooms_ok;
      r.rooms_failed = ctx_.rooms_failed;
      r.inputs_sent = ctx_.inputs_sent;
      r.inputs_reflected = ctx_.inputs_reflected;
      for (auto& room : rooms_) for (auto& client : room->clients) r.inputs_unapplied += client->PendingMeasured();
      r.snapshots = ctx_.snapshots;
      r.snapshots_missing = ctx_.snapshots_missing;
      r.violations = ctx_.violations;
      r.roster_mismatches = ctx_.roster_mismatches;
      r.disconnects = ctx_.disconnects;
      r.bytes_received = ctx_.bytes_received;
      r.gap_us = std::move(ctx_.gap_us);
      r.input_us = std::move(ctx_.input_us);
      r.lag_us = std::move(ctx_.lag_us);
      // 결과를 가져간 뒤 연결을 닫는다(RST).
      ctx_.closing = true;
      for (auto& room : rooms_) for (auto& client : room->clients) client->Close();
      return r;
    });
  }

  void Join() {
    if (!thread_.joinable()) return;
    guard_.reset();
    io_.stop();
    thread_.join();
  }

 private:
  void StartBatch() {
    const std::uint32_t batch = std::max<std::uint32_t>(1, ctx_.options.room_batch / std::max<std::uint32_t>(1, ctx_.options.threads));
    for (std::uint32_t i = 0; i < batch && started_ < room_count_; ++i, ++started_) {
      auto room = std::make_unique<RoomCtx>();
      for (std::uint32_t c = 0; c < ctx_.options.players_per_room; ++c) {
        room->clients.push_back(std::make_shared<GClient>(ctx_, *room, c, "p" + std::to_string(started_) + "_" + std::to_string(c)));
      }
      room->clients[0]->Connect();  // 방장이 방을 만든 뒤 나머지가 이어서 입장한다
      rooms_.push_back(std::move(room));
    }
    if (started_ < room_count_) {
      batch_timer_.expires_after(std::chrono::milliseconds(ctx_.options.room_gap_ms));
      batch_timer_.async_wait([this](const boost::system::error_code& ec) {
        if (!ec) StartBatch();
      });
    }
  }

  template <typename F>
  auto Call(F fn) -> decltype(fn()) {
    auto promise = std::make_shared<std::promise<decltype(fn())>>();
    auto future = promise->get_future();
    boost::asio::post(io_, [promise, fn = std::move(fn)]() mutable { promise->set_value(fn()); });
    return future.get();
  }

  boost::asio::io_context io_;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard_;
  boost::asio::steady_timer batch_timer_;
  WorkerCtx ctx_;
  std::vector<std::unique_ptr<RoomCtx>> rooms_;
  std::uint32_t room_count_;
  std::uint32_t started_ = 0;
  std::thread thread_;
};

// ============================================================ 결과 계산과 출력

double PercentileMs(std::vector<std::uint32_t>& values, double p) {
  if (values.empty()) return 0.0;
  const auto index = static_cast<std::size_t>(std::min(static_cast<double>(values.size() - 1), p / 100.0 * static_cast<double>(values.size())));
  std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
  return static_cast<double>(values[index]) / 1000.0;
}

double MaxMs(const std::vector<std::uint32_t>& values) {
  return values.empty() ? 0.0 : static_cast<double>(*std::max_element(values.begin(), values.end())) / 1000.0;
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
      : io_(io), options_(std::move(options)), poll_timer_(io), phase_timer_(io), sample_timer_(io), drain_timer_(io) {}

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
    const tcp::endpoint endpoint = endpoints.begin()->endpoint();
    threads_ = std::max<std::uint32_t>(1, std::min(options_.threads, options_.rooms));
    Options worker_options = options_;
    worker_options.threads = threads_;
    const std::uint32_t base = options_.rooms / threads_;
    const std::uint32_t extra = options_.rooms % threads_;
    std::random_device random;
    for (std::uint32_t i = 0; i < threads_; ++i) {
      workers_.push_back(std::make_unique<Worker>(worker_options, endpoint, base + (i < extra ? 1 : 0), rooms_done_,
                                                  static_cast<std::uint32_t>(random())));
    }
    connect_started_ = Clock::now();
    std::cout << "방 만들기 시작: 방 " << options_.rooms << "개(플레이어 " << options_.rooms * options_.players_per_room
              << "명), 작업 스레드 " << threads_ << "개\n";
    for (auto& worker : workers_) worker->StartConnecting();
    PollRooms();
  }

 private:
  void PollRooms() {
    const bool all_done = rooms_done_.load() >= options_.rooms;
    const bool timed_out = Clock::now() - connect_started_ > std::chrono::seconds(120);
    if (all_done || timed_out) {
      BeginTraffic();
      return;
    }
    poll_timer_.expires_after(std::chrono::milliseconds(20));
    poll_timer_.async_wait([this](const boost::system::error_code& ec) {
      if (!ec) PollRooms();
    });
  }

  void BeginTraffic() {
    const double seconds = std::chrono::duration<double>(Clock::now() - connect_started_).count();
    std::cout << "방 형성 대기 종료 (" << std::fixed << std::setprecision(1) << seconds << "초)\n";
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
    measure_start_text_ = WallClockText();
    server_start_ = ReadUsage(options_.server_pid);
    generator_start_ = ReadUsage(0);
    std::cout << "측정 " << options_.duration_s << "초 시작\n";
    Sample();
    phase_timer_.expires_after(std::chrono::seconds(options_.duration_s));
    phase_timer_.async_wait([this](const boost::system::error_code& ec) {
      if (!ec) EndMeasure();
    });
  }

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
    measure_end_text_ = WallClockText();
    for (auto& worker : workers_) worker->StopSending();
    // 측정 구간이 끝났으므로 스냅샷 통계 기록을 멈춘다. 이미 보낸 입력의 반영 지연은 입력을 보낸 시점의 표시로
    // 구분되므로 아래 대기(drain) 동안에도 계속 기록된다.
    for (auto& worker : workers_) worker->SetMeasuring(false);
    measure_seconds_ = std::chrono::duration<double>(Clock::now() - measure_started_).count();
    server_end_ = ReadUsage(options_.server_pid);
    generator_end_ = ReadUsage(0);
    drain_started_ = Clock::now();
    CheckDrain();
  }

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
    WorkerResult total;
    for (auto& worker : workers_) {
      WorkerResult r = worker->Finalize();
      total.rooms_ok += r.rooms_ok;
      total.rooms_failed += r.rooms_failed;
      total.inputs_sent += r.inputs_sent;
      total.inputs_reflected += r.inputs_reflected;
      total.inputs_unapplied += r.inputs_unapplied;
      total.snapshots += r.snapshots;
      total.snapshots_missing += r.snapshots_missing;
      total.violations += r.violations;
      total.roster_mismatches += r.roster_mismatches;
      total.disconnects += r.disconnects;
      total.bytes_received += r.bytes_received;
      total.gap_us.insert(total.gap_us.end(), r.gap_us.begin(), r.gap_us.end());
      total.input_us.insert(total.input_us.end(), r.input_us.begin(), r.input_us.end());
      total.lag_us.insert(total.lag_us.end(), r.lag_us.begin(), r.lag_us.end());
    }
    for (auto& worker : workers_) worker->Join();
    Report(total);
    io_.stop();
  }

  void Report(WorkerResult& r) {
    const std::uint32_t players = options_.rooms * options_.players_per_room;
    const double room_rate = options_.rooms == 0 ? 0.0 : static_cast<double>(r.rooms_ok) / options_.rooms;
    const double gap_p50 = PercentileMs(r.gap_us, 50), gap_p95 = PercentileMs(r.gap_us, 95), gap_p99 = PercentileMs(r.gap_us, 99);
    const double gap_max = MaxMs(r.gap_us);
    const double in_p50 = PercentileMs(r.input_us, 50), in_p95 = PercentileMs(r.input_us, 95), in_p99 = PercentileMs(r.input_us, 99);
    const double in_max = MaxMs(r.input_us);
    const double lag_p99 = PercentileMs(r.lag_us, 99);
    const double expected = static_cast<double>(r.snapshots + r.snapshots_missing);
    const double missing_rate = expected == 0.0 ? 0.0 : static_cast<double>(r.snapshots_missing) / expected;
    const double unapplied_rate = r.inputs_sent == 0 ? 0.0 : static_cast<double>(r.inputs_unapplied) / static_cast<double>(r.inputs_sent);
    const double kbps = measure_seconds_ > 0 ? static_cast<double>(r.bytes_received) / measure_seconds_ / 1024.0 : 0.0;

    double server_cpu = 0.0;
    if (server_start_.valid && server_end_.valid && measure_seconds_ > 0) {
      server_cpu = (server_end_.cpu_seconds - server_start_.cpu_seconds) / measure_seconds_ * 100.0;
    }
    double generator_cpu = 0.0;
    if (generator_start_.valid && generator_end_.valid && measure_seconds_ > 0) {
      generator_cpu = (generator_end_.cpu_seconds - generator_start_.cpu_seconds) / measure_seconds_ * 100.0;
    }
    const double generator_cpu_per_thread = generator_cpu / static_cast<double>(threads_);
    double ws_start = 0, ws_end = 0, growth = 0;
    if (!server_ws_samples_.empty()) {
      const std::size_t n = server_ws_samples_.size();
      const std::size_t third = std::max<std::size_t>(1, n / 3);
      const double first = MeanMegabytes(server_ws_samples_, 0, third);
      const double last = MeanMegabytes(server_ws_samples_, n - third, n);
      growth = first > 0 ? last / first : 0.0;
      ws_start = static_cast<double>(server_ws_samples_.front()) / (1024.0 * 1024.0);
      ws_end = static_cast<double>(server_ws_samples_.back()) / (1024.0 * 1024.0);
    }

    const bool saturated = lag_p99 > options_.max_lag_p99_ms || generator_cpu_per_thread > options_.max_generator_cpu;
    struct Check { const char* name; bool pass; };
    const std::vector<Check> checks = {
        {"방 형성 >= 99.9%", room_rate >= options_.min_room_rate},
        {"스냅샷 도착 간격 p99 <= 100ms", gap_p99 <= options_.max_gap_p99_ms},
        {"입력 반영 지연 p99 <= 100ms", in_p99 <= options_.max_input_p99_ms},
        {"누락 스냅샷 <= 1%", missing_rate <= options_.max_missing_rate},
        {"정합성 위반 0건", r.violations == 0 && r.roster_mismatches == 0},
        {"반영되지 않은 입력 <= 0.1%", unapplied_rate <= options_.max_unapplied_rate},
        {"끊긴 연결 0개", r.disconnects == 0},
        {"서버 메모리 후반/전반 <= 1.05", growth == 0.0 || growth <= options_.max_mem_growth},
    };
    bool all_pass = true;
    for (const auto& c : checks) all_pass = all_pass && c.pass;
    const char* verdict = saturated ? "saturated" : (all_pass ? "pass" : "fail");

    std::cout << "\n==== 결과 [" << options_.label << "] ====\n"
              << "워크로드      : 방 " << options_.rooms << "개 x " << options_.players_per_room << "명 (플레이어 " << players
              << "명), 입력 " << options_.input_rate << "Hz/명, 생성기 스레드 " << threads_ << "개, 측정 " << std::fixed
              << std::setprecision(2) << measure_seconds_ << "초\n"
              << "방 형성       : 성공 " << r.rooms_ok << " / 실패 " << r.rooms_failed << " (" << std::setprecision(2) << room_rate * 100 << "%)\n"
              << "입력          : 전송 " << r.inputs_sent << ", 반영 확인 " << r.inputs_reflected << ", 반영 안 됨 " << r.inputs_unapplied << "\n"
              << "스냅샷        : 수신 " << r.snapshots << ", 누락 " << r.snapshots_missing << " (" << missing_rate * 100 << "%), 수신 대역폭 "
              << std::setprecision(1) << kbps << " KiB/s\n"
              << "정합성        : 위반 " << r.violations << ", 인원 불일치 " << r.roster_mismatches << ", 끊긴 연결 " << r.disconnects << "\n"
              << std::setprecision(2)
              << "도착 간격(ms) : p50 " << gap_p50 << "  p95 " << gap_p95 << "  p99 " << gap_p99 << "  최대 " << gap_max << "\n"
              << "입력 반영(ms) : p50 " << in_p50 << "  p95 " << in_p95 << "  p99 " << in_p99 << "  최대 " << in_max << "\n"
              << "생성기 스케줄 지연(ms): p99 " << lag_p99 << "\n";
    if (server_start_.valid) {
      std::cout << "서버          : CPU " << server_cpu << "% (한 코어 기준), 메모리 시작 " << ws_start << "MB, 끝 " << ws_end
                << "MB, 후반/전반 " << growth << "배\n";
    }
    std::cout << "생성기        : CPU 합계 " << generator_cpu << "% (스레드당 평균 " << generator_cpu_per_thread << "%)\n"
              << "측정 구간(벽시계): " << measure_start_text_ << " ~ " << measure_end_text_ << "\n\n"
              << "판정 (기준은 docs/stage4.md)\n";
    for (const auto& c : checks) std::cout << "  " << c.name << " : " << (c.pass ? "통과" : "불합격") << "\n";
    std::cout << "  생성기 포화 아님 : " << (saturated ? "포화(판정 불가)" : "통과") << "\n";
    std::cout << "\n종합: " << (saturated ? "판정 불가(생성기 포화)" : (all_pass ? "합격" : "불합격")) << "\n"
              << "(서버의 틱 건너뜀 비율은 서버 로그의 통계 줄 증가분으로 따로 확인한다)\n";

    if (!options_.csv_path.empty()) WriteCsv(r, players, gap_p50, gap_p95, gap_p99, gap_max, in_p50, in_p95, in_p99, in_max, lag_p99,
                                             missing_rate, kbps, server_cpu, ws_start, ws_end, growth, generator_cpu,
                                             generator_cpu_per_thread, verdict);
    exit_code_ = saturated ? 3 : (all_pass ? 0 : 1);
  }

  void WriteCsv(const WorkerResult& r, std::uint32_t players, double gap_p50, double gap_p95, double gap_p99, double gap_max,
                double in_p50, double in_p95, double in_p99, double in_max, double lag_p99, double missing_rate, double kbps,
                double server_cpu, double ws_start, double ws_end, double growth, double generator_cpu,
                double generator_cpu_per_thread, const char* verdict) {
    const bool exists = std::ifstream(options_.csv_path).good();
    std::ofstream out(options_.csv_path, std::ios::app);
    if (!out) {
      std::cerr << "CSV 파일을 열 수 없습니다: " << options_.csv_path << "\n";
      return;
    }
    if (!exists) {
      out << "label,rooms,players,threads,rooms_ok,rooms_failed,input_rate,measure_s,inputs_sent,inputs_reflected,"
             "inputs_unapplied,snapshots,snapshots_missing,missing_rate,violations,roster_mismatches,disconnects,"
             "gap_p50_ms,gap_p95_ms,gap_p99_ms,gap_max_ms,input_p50_ms,input_p95_ms,input_p99_ms,input_max_ms,"
             "lag_p99_ms,recv_kib_per_s,server_cpu_pct,server_ws_start_mb,server_ws_end_mb,mem_growth,"
             "generator_cpu_total_pct,generator_cpu_per_thread_pct,measure_start,measure_end,verdict\n";
    }
    out << std::fixed << std::setprecision(3) << options_.label << ',' << options_.rooms << ',' << players << ',' << threads_ << ','
        << r.rooms_ok << ',' << r.rooms_failed << ',' << options_.input_rate << ',' << measure_seconds_ << ',' << r.inputs_sent << ','
        << r.inputs_reflected << ',' << r.inputs_unapplied << ',' << r.snapshots << ',' << r.snapshots_missing << ',' << missing_rate
        << ',' << r.violations << ',' << r.roster_mismatches << ',' << r.disconnects << ',' << gap_p50 << ',' << gap_p95 << ','
        << gap_p99 << ',' << gap_max << ',' << in_p50 << ',' << in_p95 << ',' << in_p99 << ',' << in_max << ',' << lag_p99 << ','
        << kbps << ',' << server_cpu << ',' << ws_start << ',' << ws_end << ',' << growth << ',' << generator_cpu << ','
        << generator_cpu_per_thread << ',' << measure_start_text_ << ',' << measure_end_text_ << ',' << verdict << "\n";
  }

  boost::asio::io_context& io_;
  Options options_;
  boost::asio::steady_timer poll_timer_, phase_timer_, sample_timer_, drain_timer_;
  std::vector<std::unique_ptr<Worker>> workers_;
  std::atomic<std::uint64_t> rooms_done_{0};
  std::uint32_t threads_ = 1;
  Clock::time_point connect_started_, measure_started_, drain_started_;
  std::string measure_start_text_, measure_end_text_;
  double measure_seconds_ = 0.0;
  bool measuring_ = false;
  ProcessUsage server_start_, server_end_, generator_start_, generator_end_;
  std::vector<std::uint64_t> server_ws_samples_;
  int exit_code_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  // 입력 전송 시각이 윈도우의 기본 타이머 해상도(약 15.6 ms)로 흔들려 "생성기 포화"로 잘못 판정되지 않도록
  // 해상도를 1 ms로 올리고, 해상도 요청을 무시하지 않게 프로세스를 제외한다(docs/stage3.md "측정 도구에서 겪은 문제").
  timeBeginPeriod(1);
  PROCESS_POWER_THROTTLING_STATE throttling{};
  throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  throttling.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  throttling.StateMask = 0;
  SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
#endif
  Options options;
  if (const int parsed = ParseArgs(argc, argv, options); parsed != 0) {
    PrintUsage(argv[0]);
    return parsed == 1 ? 0 : 2;
  }
  boost::asio::io_context io;
  Runner runner(io, options);
  runner.Start();
  io.run();
  return runner.exit_code();
}
