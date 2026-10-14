cmake_minimum_required(VERSION 3.25)
project(game_server_lab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)

# proto/*.proto -> 빌드 디렉터리의 생성 코드. 서버와 테스트 클라이언트가 같은 라이브러리를 쓴다.
add_library(game_proto STATIC proto/ping.proto)
target_link_libraries(game_proto PUBLIC protobuf::libprotobuf)
target_include_directories(game_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
protobuf_generate(
  TARGET game_proto
  IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/proto
  PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)

# 툴체인 확인용. 서버 구현이 시작되면 제거한다.
add_executable(hello_asio src/hello_asio.cpp)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT hello_asio)
target_link_libraries(hello_asio PRIVATE Boost::headers game_proto)
if(WIN32)
  target_compile_definitions(hello_asio PRIVATE _WIN32_WINNT=0x0A00)  # Windows 10
endif()
if(MSVC)
  target_compile_options(hello_asio PRIVATE /W4 /utf-8)
else()
  target_compile_options(hello_asio PRIVATE -Wall -Wextra)
endif()
