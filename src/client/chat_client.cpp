// chat_client: 서버에 접속해 채팅 방에 입장하고 대화하는 대화형 콘솔 클라이언트 (수동 실험용).
//
// 터미널 두세 개에서 각각 실행하면 서로의 입장/퇴장과 메시지를 실시간으로 볼 수 있다.
//
// 구조
//  - 메인 스레드: io_context를 돌린다. 서버에서 오는 바이트를 읽어 프레임으로 자르고(FrameDecoder)
//    화면에 출력하며, 서버로 보내는 쓰기를 처리한다. 화면 출력과 소켓 사용은 모두 이 스레드에서만 한다.
//  - 입력 스레드: 키보드(표준 입력)를 한 줄씩 읽는다. 읽은 줄은 io_context에 넘겨서 메인 스레드가
//    처리하게 한다. (표준 입력 읽기는 입력이 올 때까지 멈춰 있어서 별도 스레드가 필요하다)
//
// 입력 규칙: "/"로 시작하면 명령, 아니면 채팅 메시지. 사용 가능한 명령은 /help 참고.
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

#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/asio.hpp>

#include "common/error_text.h"
#include "ping.pb.h"
#include "chat.pb.h"
#include "protocol/frame.h"
#include "protocol/message_ids.h"

namespace {

using boost::asio::ip::tcp;
using game::protocol::MessageId;

struct Options {
  std::string host = "127.0.0.1";
  std::uint16_t port = 7777;
  std::string nickname;  // 비어 있지 않으면 접속 직후 자동으로 입장한다.
};

void PrintUsage(const char* program) {
  std::cout << "사용법: " << program << " [옵션]\n"
            << "  --host <주소>     서버 주소 (기본 127.0.0.1)\n"
            << "  --port <번호>     서버 포트 (기본 7777)\n"
            << "  --nick <닉네임>   접속 직후 이 닉네임으로 입장\n"
            << "  --help            이 도움말\n";
}

// 결과 코드를 사람이 읽는 문장으로 바꾼다.
const char* ResultText(game::protocol::ResultCode code) {
  switch (code) {
    case game::protocol::RESULT_OK: return "성공";
    case game::protocol::RESULT_INVALID_NICKNAME: return "닉네임 규칙 위반 (1~16자, 앞뒤 공백/제어 문자 금지)";
    case game::protocol::RESULT_NICKNAME_TAKEN: return "이미 사용 중인 닉네임";
    case game::protocol::RESULT_ALREADY_JOINED: return "이미 입장한 상태";
    case game::protocol::RESULT_NOT_JOINED: return "입장하지 않은 상태 (/join <닉네임> 먼저)";
    case game::protocol::RESULT_EMPTY_MESSAGE: return "빈 메시지";
    case game::protocol::RESULT_MESSAGE_TOO_LONG: return "메시지가 너무 김 (최대 1,000바이트)";
    case game::protocol::RESULT_INVALID_ARGUMENT: return "잘못된 값 (제어 문자 포함 또는 범위 초과)";
    case game::protocol::RESULT_STORAGE_FAILURE: return "서버의 저장소 오류 (저장되지 않음)";
    case game::protocol::RESULT_SERVER_BUSY: return "서버가 혼잡함 (잠시 뒤 다시)";
    default: break;
  }
  return "알 수 없는 결과";
}

// 서버와의 연결 하나와 화면 출력을 담당하는 클래스. io_context 스레드에서만 사용한다.
class ChatClientApp : public std::enable_shared_from_this<ChatClientApp> {
 public:
  explicit ChatClientApp(boost::asio::io_context& io) : io_(io), socket_(io) {}

  // 서버에 연결한다. (이름 해석과 연결은 동기식으로 기다린다) 성공하면 true.
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

  // 서버에서 오는 데이터를 읽기 시작한다.
  void Start() {
    std::cout << "연결되었습니다. 도움말은 /help, 종료는 /quit 입니다.\n";
    DoRead();
  }

  // 키보드에서 읽은 한 줄을 처리한다.
  void HandleLine(const std::string& line) {
    if (line.empty()) {
      return;
    }
    if (line[0] != '/') {
      game::protocol::ChatRequest request;
      request.set_text(line);
      Send(MessageId::kChatRequest, request);
      return;
    }

    std::istringstream words(line);
    std::string command;
    words >> command;

    if (command == "/help") {
      std::cout << "명령:\n"
                << "  /join <닉네임>     방에 입장\n"
                << "  /leave             방에서 퇴장 (연결은 유지)\n"
                << "  /history [개수]    최근 기록 조회 (기본 20, 최대 50)\n"
                << "  /ping              서버 응답 확인\n"
                << "  /wait <밀리초>     다음 줄 처리를 늦춤 (스크립트용)\n"
                << "  /quit              종료\n"
                << "그 밖의 입력은 채팅 메시지로 전송됩니다.\n";
    } else if (command == "/join") {
      std::string nickname;
      std::getline(words >> std::ws, nickname);  // 닉네임에 공백이 들어갈 수 있으므로 줄 끝까지 읽는다
      game::protocol::JoinRequest request;
      request.set_nickname(nickname);
      Send(MessageId::kJoinRequest, request);
    } else if (command == "/leave") {
      Send(MessageId::kLeaveRequest, game::protocol::LeaveRequest{});
    } else if (command == "/history") {
      std::uint32_t limit = 0;
      words >> limit;
      game::protocol::HistoryRequest request;
      request.set_limit(limit);
      Send(MessageId::kHistoryRequest, request);
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

  // 연결을 닫는다. 진행 중인 읽기가 취소되어 io_context가 끝난다.
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
  void DoRead() {
    auto self = shared_from_this();
    socket_.async_read_some(boost::asio::buffer(read_buffer_),
                            [this, self](const boost::system::error_code& ec, std::size_t bytes) {
                              OnRead(ec, bytes);
                            });
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

  // 서버가 보낸 프레임 하나를 사람이 읽는 형태로 출력한다.
  void PrintFrame(const game::protocol::Frame& frame) {
    const auto parse = [&frame](google::protobuf::MessageLite& message) {
      return message.ParseFromArray(frame.payload.data(), static_cast<int>(frame.payload.size()));
    };

    switch (static_cast<MessageId>(frame.message_id)) {
      case MessageId::kJoinResponse: {
        game::protocol::JoinResponse r;
        if (!parse(r)) break;
        if (r.result() == game::protocol::RESULT_OK) {
          std::cout << "[입장] 성공. 접속자:";
          for (const auto& member : r.members()) std::cout << ' ' << member;
          std::cout << "\n";
        } else {
          std::cout << "[입장] 실패: " << ResultText(r.result()) << "\n";
        }
        return;
      }
      case MessageId::kLeaveResponse: {
        game::protocol::LeaveResponse r;
        if (!parse(r)) break;
        std::cout << "[퇴장] " << ResultText(r.result()) << "\n";
        return;
      }
      case MessageId::kChatResponse: {
        game::protocol::ChatResponse r;
        if (!parse(r)) break;
        if (r.result() == game::protocol::RESULT_OK) {
          std::cout << "[전송] 저장됨 (번호 " << r.message_id() << ")\n";
        } else {
          std::cout << "[전송] 실패: " << ResultText(r.result()) << "\n";
        }
        return;
      }
      case MessageId::kChatBroadcast: {
        game::protocol::ChatBroadcast b;
        if (!parse(b)) break;
        std::cout << "[#" << b.entry().message_id() << "] " << b.entry().nickname() << ": " << b.entry().text() << "\n";
        return;
      }
      case MessageId::kHistoryResponse: {
        game::protocol::HistoryResponse r;
        if (!parse(r)) break;
        if (r.result() != game::protocol::RESULT_OK) {
          std::cout << "[기록] 실패: " << ResultText(r.result()) << "\n";
          return;
        }
        std::cout << "[기록] " << r.entries_size() << "건 (오래된 것부터)\n";
        for (const auto& e : r.entries()) {
          std::cout << "  [#" << e.message_id() << "] " << e.nickname() << ": " << e.text() << "\n";
        }
        return;
      }
      case MessageId::kPresenceNotice: {
        game::protocol::PresenceNotice n;
        if (!parse(n)) break;
        std::cout << "[알림] " << n.nickname()
                  << (n.kind() == game::protocol::PresenceNotice::KIND_JOINED ? " 님이 입장했습니다" : " 님이 퇴장했습니다")
                  << "\n";
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

  // message를 프레임으로 만들어 송신 큐에 넣고, 쓰기가 진행 중이 아니면 시작한다.
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
};

bool ParseArgs(int argc, char** argv, Options& options, bool& help) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      help = true;
      return true;
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
    } else {
      std::cerr << "알 수 없는 옵션: " << arg << "\n";
      return false;
    }
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
  auto app = std::make_shared<ChatClientApp>(io);
  if (!app->Connect(options.host, options.port)) {
    return 1;
  }
  app->Start();
  if (!options.nickname.empty()) {
    app->HandleLine("/join " + options.nickname);
  }

  // 입력 스레드: 키보드에서 한 줄씩 읽어 io_context(메인 스레드)에 넘긴다.
  // 입력이 끝나면(Ctrl+Z/Ctrl+D 또는 파이프 종료) 종료 요청을 넘긴다.
  std::thread([&io, app] {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();  // 윈도우 줄바꿈(\r\n) 정리
      }
      if (line.rfind("/wait ", 0) == 0) {
        // 스크립트용: 입력 스레드만 쉬므로 그동안 서버에서 오는 메시지는 계속 출력된다.
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
