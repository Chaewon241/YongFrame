// 툴체인 확인용: Boost.Asio 실행, Protobuf 직렬화 왕복, 버전 출력.
#include <boost/asio.hpp>
#include <boost/version.hpp>
#include <google/protobuf/stubs/common.h>

#include <iostream>
#include <string>

#include "ping.pb.h"

int main() {
  boost::asio::io_context io;
  bool ran = false;
  boost::asio::post(io, [&ran] { ran = true; });
  io.run();

  game::protocol::PingRequest req;
  req.set_request_id(42);
  req.set_payload("hello");
  std::string bytes;
  req.SerializeToString(&bytes);

  game::protocol::PingRequest parsed;
  const bool ok = parsed.ParseFromString(bytes) && parsed.request_id() == 42 &&
                  parsed.payload() == "hello";

  std::cout << "Boost " << BOOST_VERSION / 100000 << '.' << BOOST_VERSION / 100 % 1000 << '.'
            << BOOST_VERSION % 100 << ", Protobuf " << google::protobuf::internal::VersionString(GOOGLE_PROTOBUF_VERSION)
            << "\nasio post: " << (ran ? "ok" : "FAIL") << ", protobuf roundtrip: " << (ok ? "ok" : "FAIL")
            << ", wire size " << bytes.size() << "B\n";
  return ran && ok ? 0 : 1;
}
