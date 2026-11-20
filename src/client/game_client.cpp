// game_client: 서버에 접속해 게임 방을 만들거나 방 코드로 입장하고, 이동 입력을 보내며 서버가 계산한 위치를
// 확인하는 대화형 콘솔 클라이언트 (수동 실험용). Unreal 클라이언트가 만들어지기 전에 4단계 서버를 직접
// 써 보고, Unreal 쪽 구현이 맞는지 비교하는 기준으로도 쓴다.
//
// 터미널 두세 개에서 각각 실행해 한 명이 방을 만들고(/create) 나머지가 코드로 입장(/join)하면, 서로의 입장/퇴장과
// 이동(스냅샷의 위치)이 보인다. 위치는 서버가 계산한 값이다. 이 클라이언트는 방향만 보낸다.
//
// 구조는 chat_client와 같다: 메인 스레드가 io_context로 소켓 읽기/쓰기와 화면 출력을 맡고, 입력 스레드가
// 키보드에서 한 줄씩 읽어 io_context에 넘긴다.
//
// 입력은 "/"로 시작하는 명령이다. 사용 가능한 명령은 /help 참고.
// 스크립트로 자동 실행할 때를 위해 "/wait <밀리초>"로 다음 줄 처리를 늦출 수 있다.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/asio.hpp>

#include "common/error_text.h"
#include "account.pb.h"
#include "game.pb.h"
#include "ping.pb.h"
#include "protocol/frame.h"
#include "protocol/message_ids.h"

namespace {

using boost::asio::ip::tcp;
using game::protocol::MessageId;

struct Options {
  std::string host = "127.0.0.1";
  std::uint16_t port = 7777;
  std::string nickname;     // 자동 입장에 쓸 닉네임
  bool create = false;      // 접속 직후 방을 만든다
  std::string join_code;    // 접속 직후 이 코드로 입장한다
  std::uint32_t snapshot_every = 20;  // N틱마다 위치를 출력한다 (20 = 초당 1번). 0이면 출력하지 않는다.
};

void PrintUsage(const char* program) {
  std::cout << "사용법: " << program << " [옵션]\n"
            << "  --host <주소>        서버 주소 (기본 127.0.0.1)\n"
            << "  --port <번호>        서버 포트 (기본 7777)\n"
            << "  --nick <닉네임>      자동 입장에 쓸 닉네임\n"
            << "  --create             접속 직후 방을 만든다 (--nick 필요)\n"
            << "  --join <방 코드>     접속 직후 이 코드로 입장한다 (--nick 필요)\n"
            << "  --snap <N>           N틱마다 위치를 출력 (기본 20 = 초당 1번, 0이면 끔)\n"
            << "  --help               이 도움말\n";
}

const char* ResultText(game::protocol::GameResult code) {
  switch (code) {
    case game::protocol::GAME_OK: return "성공";
    case game::protocol::GAME_INVALID_NICKNAME: return "닉네임 규칙 위반 (1~16자, 앞뒤 공백/제어 문자 금지)";
    case game::protocol::GAME_INVALID_ROOM_CODE: return "방 코드는 숫자 6자리여야 합니다";
    case game::protocol::GAME_ROOM_NOT_FOUND: return "그 코드의 방이 없습니다";
    case game::protocol::GAME_ROOM_FULL: return "방이 가득 찼습니다 (최대 4명)";
    case game::protocol::GAME_ALREADY_IN_ROOM: return "이미 방에 있습니다 (/leave 먼저)";
    case game::protocol::GAME_NOT_IN_ROOM: return "방에 있지 않습니다";
    case game::protocol::GAME_NICKNAME_TAKEN: return "그 방에 같은 닉네임이 이미 있습니다";
    case game::protocol::GAME_SERVER_BUSY: return "서버가 혼잡합니다 (방 수 상한 등)";
    case game::protocol::GAME_NODE_NOT_FOUND: return "그 번호의 자원 노드가 없습니다";
    case game::protocol::GAME_NODE_DEPLETED: return "노드가 고갈되었습니다 (재생을 기다리는 중)";
    case game::protocol::GAME_TOO_FAR: return "너무 멀리 있습니다 (서버가 아는 내 위치 기준)";
    case game::protocol::GAME_GATHER_COOLDOWN: return "채집 쿨다운 중입니다";
    case game::protocol::GAME_BUILD_INVALID_TYPE: return "모르는 건물 종류입니다";
    case game::protocol::GAME_BUILD_BLOCKED: return "그 자리에는 지을 수 없습니다 (맵 밖, 건물/나무/돌/사람과 겹침)";
    case game::protocol::GAME_NOT_ENOUGH_RESOURCES: return "자원이 모자랍니다 (집: 목재 20, 석재 10)";
    case game::protocol::GAME_BUILD_TOO_FAR: return "너무 멀리 있습니다 (내 위치에서 500 cm 이내)";
    case game::protocol::GAME_STRUCTURE_LIMIT: return "방의 건물 수 상한입니다";
    case game::protocol::GAME_BUILD_OUT_OF_ZONE: return "건설 구역(맵 중앙) 밖입니다";
    case game::protocol::GAME_LOGIN_REQUIRED: return "농장에는 로그인한 사람만 들어갈 수 있습니다 (/login)";
    case game::protocol::GAME_STORAGE_UNAVAILABLE: return "농장을 불러오거나 만들지 못했습니다 (서버 저장소 오류)";
    default: break;
  }
  return "알 수 없는 결과";
}

const char* AccountResultText(game::protocol::AccountResult code) {
  switch (code) {
    case game::protocol::ACCOUNT_OK: return "성공";
    case game::protocol::ACCOUNT_INVALID_LOGIN_NAME: return "ID 규칙 위반 (영문/숫자/밑줄 4~16자)";
    case game::protocol::ACCOUNT_INVALID_PASSWORD: return "비밀번호 규칙 위반 (8~64바이트)";
    case game::protocol::ACCOUNT_LOGIN_NAME_TAKEN: return "이미 있는 ID입니다";
    case game::protocol::ACCOUNT_BAD_CREDENTIALS: return "ID 또는 비밀번호가 틀렸습니다";
    case game::protocol::ACCOUNT_ALREADY_LOGGED_IN: return "이미 로그인했거나, 그 계정이 다른 곳에서 접속 중입니다";
    case game::protocol::ACCOUNT_TOO_MANY_ATTEMPTS: return "로그인 실패가 너무 많습니다 (서버가 연결을 닫습니다)";
    case game::protocol::ACCOUNT_STORAGE_UNAVAILABLE: return "서버의 저장소(DB) 오류입니다";
    case game::protocol::ACCOUNT_SERVER_BUSY: return "서버가 바쁩니다 (이전 요청 처리 중이거나 해시 대기열이 가득 참)";
    case game::protocol::ACCOUNT_NOT_LOGGED_IN: return "로그인이 필요합니다 (/login)";
    default: break;
  }
  return "알 수 없는 결과";
}

class GameClientApp : public std::enable_shared_from_this<GameClientApp> {
 public:
  GameClientApp(boost::asio::io_context& io, std::uint32_t snapshot_every)
      : io_(io), socket_(io), snapshot_every_(snapshot_every) {}

  bool Connect(const std::string& host, std::uint16_t port) {
    boost::system::error_code ec;
    tcp::resolver resolver(io_);
    const auto endpoints = resolver.resolve(host, std::to_string(port), ec);
    if (!ec) {
      boost::asio::connect(socket_, endpoints, ec);
    }
    if (ec) {
      std::cerr << "서버에 연결하지 못했습니다 (" << host << ":" << port << "): "
                << game::common::ErrorText(ec) << "\n";
      return false;
    }
    socket_.set_option(tcp::no_delay(true), ec);
    return true;
  }

  void Start() {
    std::cout << "연결되었습니다. 도움말은 /help, 종료는 /quit 입니다.\n";
    DoRead();
  }

  void HandleLine(const std::string& line) {
    if (line.empty()) {
      return;
    }
    if (line[0] != '/') {
      std::cout << "명령은 /로 시작합니다. /help 를 참고하세요.\n";
      return;
    }

    std::istringstream words(line);
    std::string command;
    words >> command;

    if (command == "/help") {
      std::cout << "명령:\n"
                << "  /register <ID> <비밀번호>  계정을 만든다 (ID는 영문/숫자/밑줄 4~16자, 비밀번호 8~64바이트)\n"
                << "  /login <ID> <비밀번호>     로그인한다. 로그인하면 방에서 ID가 닉네임이 된다\n"
                << "  /profile                 내 누적 기록(채집량, 지은 집 수)을 본다 (로그인 필요)\n"
                << "  /create <닉네임>        방을 만들고 방장이 된다 (방 코드가 출력됨)\n"
                << "  /join <방코드> <닉네임>  코드로 방에 입장\n"
                << "  /leave                  방에서 나간다 (연결은 유지)\n"
                << "  /move <dx> <dy>         이동 방향 (길이 1 이하, 예: /move 1 0). 서버가 계속 그 방향으로 움직인다\n"
                << "  /stop                   정지 (방향 0, 0)\n"
                << "  /snap <N>               N틱마다 위치 출력 (0이면 끔, 20이면 초당 1번)\n"
                << "  /nodes                  자원 노드 목록(번호, 종류, 위치, 체력, 내 위치에서의 거리)\n"
                << "  /gather [번호]          노드를 한 번 친다 (번호를 생략하면 가장 가까운 살아 있는 노드)\n"
                << "  /build <x> <y>          그 자리(100 cm 격자로 맞춰짐)에 집을 짓는다. 내 위치에서 500 cm 이내, 목재 20 + 석재 10\n"
                << "  /structures             건물 목록(번호, 위치, 건설 중/완료)\n"
                << "  /ping                   서버 응답 확인\n"
                << "  /wait <밀리초>          다음 줄 처리를 늦춤 (스크립트용)\n"
                << "  /quit                   종료\n";
    } else if (command == "/register" || command == "/login") {
      // 주의: 비밀번호는 터미널에 그대로 보이고 서버로 평문 전송된다(TLS 없음). 개발용 도구다.
      std::string login_name;
      std::string password;
      if (!(words >> login_name >> password)) {
        std::cout << "사용법: " << command << " <ID> <비밀번호>\n";
        return;
      }
      if (command == "/register") {
        game::protocol::RegisterRequest request;
        request.set_login_name(login_name);
        request.set_password(password);
        Send(MessageId::kRegisterRequest, request);
      } else {
        game::protocol::LoginRequest request;
        request.set_login_name(login_name);
        request.set_password(password);
        Send(MessageId::kLoginRequest, request);
      }
    } else if (command == "/profile") {
      Send(MessageId::kProfileRequest, game::protocol::ProfileRequest{});
    } else if (command == "/create") {
      std::string nickname;
      std::getline(words >> std::ws, nickname);
      game::protocol::CreateRoomRequest request;
      request.set_nickname(nickname);
      Send(MessageId::kCreateRoomRequest, request);
    } else if (command == "/join") {
      std::string code;
      words >> code;
      std::string nickname;
      std::getline(words >> std::ws, nickname);
      game::protocol::JoinRoomRequest request;
      request.set_room_code(code);
      request.set_nickname(nickname);
      Send(MessageId::kJoinRoomRequest, request);
    } else if (command == "/leave") {
      Send(MessageId::kLeaveRoomRequest, game::protocol::LeaveRoomRequest{});
    } else if (command == "/move") {
      float dx = 0.0f;
      float dy = 0.0f;
      if (!(words >> dx >> dy)) {
        std::cout << "사용법: /move <dx> <dy> (예: /move 1 0, /move 0.7 -0.7)\n";
        return;
      }
      SendMove(dx, dy);
    } else if (command == "/stop") {
      SendMove(0.0f, 0.0f);
    } else if (command == "/snap") {
      std::uint32_t every = 0;
      if (!(words >> every)) {
        std::cout << "사용법: /snap <N> (0이면 끔)\n";
        return;
      }
      snapshot_every_ = every;
      std::cout << "위치 출력 주기: " << (every == 0 ? "끔" : std::to_string(every) + "틱마다") << "\n";
    } else if (command == "/build") {
      float x = 0.0f;
      float y = 0.0f;
      if (!(words >> x >> y)) {
        std::cout << "사용법: /build <x> <y> (예: /build 2600 2400)\n";
        return;
      }
      game::protocol::BuildRequest request;
      request.set_structure_type(game::protocol::STRUCTURE_HOUSE);
      request.set_x(x);
      request.set_y(y);
      Send(MessageId::kBuildRequest, request);
    } else if (command == "/structures") {
      if (structures_.empty()) {
        std::cout << "건물이 없습니다.\n";
      }
      for (const auto& [id, s] : structures_) {
        std::cout << "  " << id << "번 집 @(" << s.x() << ", " << s.y() << ") "
                  << (s.state() == game::protocol::STRUCTURE_COMPLETE ? "완료" : "건설 중") << "\n";
      }
    } else if (command == "/nodes") {
      PrintNodes();
    } else if (command == "/gather") {
      std::uint32_t node_id = 0;
      if (!(words >> node_id)) {
        node_id = NearestAliveNode();
        if (node_id == 0) {
          std::cout << "살아 있는 노드가 없거나 아직 방 상태/위치를 받지 못했습니다.\n";
          return;
        }
        std::cout << "가장 가까운 노드 " << node_id << "번을 칩니다.\n";
      }
      game::protocol::GatherRequest request;
      request.set_node_id(node_id);
      Send(MessageId::kGatherRequest, request);
    } else if (command == "/ping") {
      game::protocol::PingRequest request;
      request.set_request_id(++ping_sequence_);
      request.set_payload("ping");
      Send(MessageId::kPingRequest, request);
    } else if (command == "/quit") {
      Quit();
    } else {
      std::cout << "알 수 없는 명령입니다: " << command << " (/help 참고)\n";
    }
  }

  void Quit() {
    if (quitting_) {
      return;
    }
    quitting_ = true;
    boost::system::error_code ignored;
    socket_.shutdown(tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
  }

 private:
  void SendMove(float dx, float dy) {
    game::protocol::MoveInput input;
    input.set_sequence(++move_sequence_);
    input.set_dir_x(dx);
    input.set_dir_y(dy);
    Send(MessageId::kMoveInput, input);
  }

  void DoRead() {
    auto self = shared_from_this();
    socket_.async_read_some(boost::asio::buffer(read_buffer_),
                            [this, self](const boost::system::error_code& ec, std::size_t bytes) { OnRead(ec, bytes); });
  }

  void OnRead(const boost::system::error_code& ec, std::size_t bytes) {
    if (quitting_) {
      return;
    }
    if (ec) {
      std::cout << (ec == boost::asio::error::eof ? "서버가 연결을 닫았습니다."
                                                    : "연결 오류: " + game::common::ErrorText(ec))
                << "\n";
      Quit();
      return;
    }
    decoder_.Append(std::span<const std::uint8_t>(read_buffer_.data(), bytes));

    game::protocol::Frame frame;
    while (true) {
      const auto status = decoder_.Next(frame);
      if (status == game::protocol::DecodeStatus::kNeedMoreData) {
        break;
      }
      if (status == game::protocol::DecodeStatus::kError) {
        std::cout << "서버가 잘못된 데이터를 보냈습니다. 연결을 닫습니다.\n";
        Quit();
        return;
      }
      PrintFrame(frame);
    }
    DoRead();
  }

  struct NodeInfo {
    game::protocol::ResourceKind kind = game::protocol::RESOURCE_TREE;
    float x = 0.0f;
    float y = 0.0f;
    std::uint32_t hp = 0;
    std::uint32_t max_hp = 0;
  };

  double DistanceFromMe(const NodeInfo& node) const {
    return std::hypot(static_cast<double>(my_x_) - node.x, static_cast<double>(my_y_) - node.y);
  }

  std::uint32_t NearestAliveNode() const {
    std::uint32_t best = 0;
    double best_distance = 0.0;
    for (const auto& [id, node] : nodes_) {
      if (node.hp == 0) continue;
      const double distance = DistanceFromMe(node);
      if (best == 0 || distance < best_distance) {
        best = id;
        best_distance = distance;
      }
    }
    return best;
  }

  void PrintNodes() const {
    if (nodes_.empty()) {
      std::cout << "노드 정보가 없습니다 (방에 들어가야 받습니다).\n";
      return;
    }
    std::cout << std::fixed << std::setprecision(0);
    for (const auto& [id, node] : nodes_) {
      std::cout << "  " << id << "번 " << (node.kind == game::protocol::RESOURCE_TREE ? "나무" : "돌") << " @(" << node.x
                << ", " << node.y << ") 체력 " << node.hp << '/' << node.max_hp << " 거리 " << DistanceFromMe(node)
                << "\n";
    }
    std::cout << std::defaultfloat;
  }

  void PrintProfile(const game::protocol::AccountProfile& profile) const {
    std::cout << "[내 기록] " << profile.login_name() << ": 누적 목재 " << profile.wood_gathered() << ", 누적 석재 "
              << profile.stone_gathered() << ", 지은 집 " << profile.houses_built() << "\n";
  }

  void PrintState(const game::protocol::RoomState& state) {
    my_player_id_ = state.your_player_id();
    nodes_.clear();
    for (const auto& node : state.nodes()) {
      nodes_[node.node_id()] = NodeInfo{node.kind(), node.x(), node.y(), node.hp(), node.max_hp()};
    }
    structures_.clear();
    for (const auto& structure : state.structures()) {
      structures_[structure.structure_id()] = structure;
    }
    std::cout << "  자원 노드 " << nodes_.size() << "개 (/nodes 로 목록), 건물 " << structures_.size() << "개 (/structures)\n";
    std::cout << "  방 코드 " << state.room_code() << " / 내 번호 " << state.your_player_id() << " / 방장 번호 "
              << state.host_player_id() << "\n  접속자:";
    for (const auto& player : state.players()) {
      std::cout << ' ' << player.player_id() << ':' << player.nickname();
    }
    std::cout << "\n";
  }

  void PrintFrame(const game::protocol::Frame& frame) {
    const auto parse = [&frame](google::protobuf::MessageLite& message) {
      return message.ParseFromArray(frame.payload.data(), static_cast<int>(frame.payload.size()));
    };

    switch (static_cast<MessageId>(frame.message_id)) {
      case MessageId::kCreateRoomResponse: {
        game::protocol::CreateRoomResponse r;
        if (!parse(r)) break;
        if (r.result() == game::protocol::GAME_OK) {
          std::cout << "[방 만들기] 성공. 다른 사람은 /join " << r.state().room_code() << " <닉네임> 으로 들어옵니다.\n";
          PrintState(r.state());
        } else {
          std::cout << "[방 만들기] 실패: " << ResultText(r.result()) << "\n";
        }
        return;
      }
      case MessageId::kJoinRoomResponse: {
        game::protocol::JoinRoomResponse r;
        if (!parse(r)) break;
        if (r.result() == game::protocol::GAME_OK) {
          std::cout << "[입장] 성공\n";
          PrintState(r.state());
        } else {
          std::cout << "[입장] 실패: " << ResultText(r.result()) << "\n";
        }
        return;
      }
      case MessageId::kLeaveRoomResponse: {
        game::protocol::LeaveRoomResponse r;
        if (!parse(r)) break;
        std::cout << "[나가기] " << ResultText(r.result()) << "\n";
        return;
      }
      case MessageId::kRoomEvent: {
        game::protocol::RoomEvent e;
        if (!parse(e)) break;
        switch (e.kind()) {
          case game::protocol::RoomEvent::PLAYER_JOINED:
            std::cout << "[알림] " << e.player().nickname() << " (번호 " << e.player().player_id() << ") 님이 입장했습니다\n";
            break;
          case game::protocol::RoomEvent::PLAYER_LEFT:
            std::cout << "[알림] " << e.player().nickname() << " (번호 " << e.player().player_id() << ") 님이 나갔습니다\n";
            break;
          case game::protocol::RoomEvent::HOST_CHANGED:
            std::cout << "[알림] 방장이 번호 " << e.host_player_id() << (e.host_player_id() == my_player_id_ ? " (나)" : "")
                      << " 로 바뀌었습니다\n";
            break;
          default:
            std::cout << "[알림] 알 수 없는 방 이벤트\n";
        }
        return;
      }
      case MessageId::kWorldSnapshot: {
        game::protocol::WorldSnapshot s;
        if (!parse(s)) break;
        for (const auto& p : s.players()) {
          if (p.player_id() == my_player_id_) {
            my_x_ = p.x();
            my_y_ = p.y();
          }
        }
        if (snapshot_every_ == 0 || s.tick() % snapshot_every_ != 0) {
          return;  // 초당 20개가 쏟아지므로 일부만 출력한다
        }
        std::cout << "[위치] 틱 " << s.tick() << ":";
        std::cout << std::fixed << std::setprecision(1);
        for (const auto& p : s.players()) {
          std::cout << ' ' << p.player_id() << (p.player_id() == my_player_id_ ? "(나)" : "") << "@(" << p.x() << ", " << p.y() << ")";
        }
        std::cout << std::defaultfloat << "\n";
        return;
      }
      case MessageId::kGatherResponse: {
        game::protocol::GatherResponse r;
        if (!parse(r)) break;
        std::cout << "[채집] 노드 " << r.node_id() << ": " << ResultText(r.result()) << " / 내 목재 " << r.wood()
                  << " 석재 " << r.stone() << "\n";
        return;
      }
      case MessageId::kBuildResponse: {
        game::protocol::BuildResponse r;
        if (!parse(r)) break;
        std::cout << "[건설] " << ResultText(r.result());
        if (r.result() == game::protocol::GAME_OK) std::cout << " (건물 " << r.structure_id() << "번)";
        std::cout << " / 내 목재 " << r.wood() << " 석재 " << r.stone() << "\n";
        return;
      }
      case MessageId::kStructureUpdate: {
        game::protocol::StructureUpdate u;
        if (!parse(u)) break;
        for (const auto& s : u.structures()) {
          structures_[s.structure_id()] = s;
          std::cout << "[건물] 틱 " << u.tick() << ": " << s.structure_id() << "번 @(" << s.x() << ", " << s.y() << ") "
                    << (s.state() == game::protocol::STRUCTURE_COMPLETE ? "완료" : "건설 중") << "\n";
        }
        return;
      }
      case MessageId::kResourceUpdate: {
        game::protocol::ResourceUpdate u;
        if (!parse(u)) break;
        for (const auto& changed : u.nodes()) {
          auto it = nodes_.find(changed.node_id());
          if (it != nodes_.end()) it->second.hp = changed.hp();
          std::cout << "[노드] 틱 " << u.tick() << ": " << changed.node_id() << "번 체력 " << changed.hp()
                    << (changed.hp() == 0 ? " (고갈)" : "") << "\n";
        }
        return;
      }
      case MessageId::kRegisterResponse: {
        game::protocol::RegisterResponse r;
        if (!parse(r)) break;
        std::cout << "[가입] " << AccountResultText(r.result()) << "\n";
        return;
      }
      case MessageId::kLoginResponse: {
        game::protocol::LoginResponse r;
        if (!parse(r)) break;
        std::cout << "[로그인] " << AccountResultText(r.result()) << "\n";
        if (r.result() == game::protocol::ACCOUNT_OK) {
          PrintProfile(r.profile());
        }
        return;
      }
      case MessageId::kProfileResponse: {
        game::protocol::ProfileResponse r;
        if (!parse(r)) break;
        if (r.result() == game::protocol::ACCOUNT_OK) {
          PrintProfile(r.profile());
        } else {
          std::cout << "[내 기록] " << AccountResultText(r.result()) << "\n";
        }
        return;
      }
      case MessageId::kPingResponse: {
        game::protocol::PingResponse r;
        if (!parse(r)) break;
        std::cout << "[Ping] 응답 request_id=" << r.request_id() << "\n";
        return;
      }
      default:
        break;
    }
    std::cout << "[알 수 없는 메시지] id=" << frame.message_id << "\n";
  }

  void Send(MessageId id, const google::protobuf::MessageLite& message) {
    if (quitting_) {
      return;
    }
    std::vector<std::uint8_t> body(message.ByteSizeLong());
    message.SerializeToArray(body.data(), static_cast<int>(body.size()));
    auto wire = game::protocol::EncodeFrame(game::protocol::ToUint16(id), body);
    if (!wire) {
      std::cout << "메시지가 너무 커서 보낼 수 없습니다.\n";
      return;
    }
    send_queue_.push_back(std::move(*wire));
    if (!writing_) {
      DoWrite();
    }
  }

  void DoWrite() {
    writing_ = true;
    auto self = shared_from_this();
    boost::asio::async_write(socket_, boost::asio::buffer(send_queue_.front()),
                             [this, self](const boost::system::error_code& ec, std::size_t) {
                               if (quitting_) return;
                               if (ec) {
                                 std::cout << "전송 오류: " << game::common::ErrorText(ec) << "\n";
                                 Quit();
                                 return;
                               }
                               send_queue_.pop_front();
                               if (send_queue_.empty()) {
                                 writing_ = false;
                               } else {
                                 DoWrite();
                               }
                             });
  }

  boost::asio::io_context& io_;
  tcp::socket socket_;
  std::array<std::uint8_t, 8192> read_buffer_{};
  game::protocol::FrameDecoder decoder_;
  std::deque<std::vector<std::uint8_t>> send_queue_;
  bool writing_ = false;
  bool quitting_ = false;
  std::uint64_t ping_sequence_ = 0;
  std::uint32_t move_sequence_ = 0;
  std::uint32_t my_player_id_ = 0;
  std::uint32_t snapshot_every_ = 20;
  float my_x_ = 0.0f;  // 마지막 스냅샷에서 받은 내 위치
  float my_y_ = 0.0f;
  std::map<std::uint32_t, NodeInfo> nodes_;  // 방 상태와 ResourceUpdate로 유지하는 노드 목록
  std::map<std::uint32_t, game::protocol::Structure> structures_;  // 방 상태와 StructureUpdate로 유지하는 건물 목록
};

bool ParseArgs(int argc, char** argv, Options& options, bool& help) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      help = true;
      return true;
    }
    if (arg == "--create") {
      options.create = true;
      continue;
    }
    if (i + 1 >= argc) {
      std::cerr << "옵션 값이 없습니다: " << arg << "\n";
      return false;
    }
    const std::string value = argv[++i];
    if (arg == "--host") {
      options.host = value;
    } else if (arg == "--port") {
      const long port = std::strtol(value.c_str(), nullptr, 10);
      if (port < 1 || port > 65535) {
        std::cerr << "--port는 1~65535 사이 숫자여야 합니다: " << value << "\n";
        return false;
      }
      options.port = static_cast<std::uint16_t>(port);
    } else if (arg == "--nick") {
      options.nickname = value;
    } else if (arg == "--join") {
      options.join_code = value;
    } else if (arg == "--snap") {
      options.snapshot_every = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    } else {
      std::cerr << "알 수 없는 옵션: " << arg << "\n";
      return false;
    }
  }
  if ((options.create || !options.join_code.empty()) && options.nickname.empty()) {
    std::cerr << "--create, --join에는 --nick이 필요합니다.\n";
    return false;
  }
  if (options.create && !options.join_code.empty()) {
    std::cerr << "--create와 --join은 함께 쓸 수 없습니다.\n";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  // 콘솔 입출력을 UTF-8로 맞춘다. 그래야 한글 입력이 UTF-8 바이트로 서버에 전달되고 출력도 깨지지 않는다.
  SetConsoleCP(CP_UTF8);
  SetConsoleOutputCP(CP_UTF8);
#endif

  Options options;
  bool help = false;
  if (!ParseArgs(argc, argv, options, help)) {
    PrintUsage(argv[0]);
    return 2;
  }
  if (help) {
    PrintUsage(argv[0]);
    return 0;
  }

  boost::asio::io_context io;
  auto app = std::make_shared<GameClientApp>(io, options.snapshot_every);
  if (!app->Connect(options.host, options.port)) {
    return 1;
  }
  app->Start();
  if (options.create) {
    app->HandleLine("/create " + options.nickname);
  } else if (!options.join_code.empty()) {
    app->HandleLine("/join " + options.join_code + " " + options.nickname);
  }

  // 입력 스레드: 키보드에서 한 줄씩 읽어 io_context(메인 스레드)에 넘긴다.
  std::thread([&io, app] {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.rfind("/wait ", 0) == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::strtol(line.c_str() + 6, nullptr, 10)));
        continue;
      }
      boost::asio::post(io, [app, line] { app->HandleLine(line); });
    }
    boost::asio::post(io, [app] { app->Quit(); });
  }).detach();

  io.run();
  return 0;
}
