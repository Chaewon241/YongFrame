cmake_minimum_required(VERSION 3.25)
project(game_server_lab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

find_package(Boost CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(Threads REQUIRED)
include(GoogleTest)

# 모든 타깃에 공통으로 적용하는 컴파일 옵션.
function(apply_project_options target)
  if(WIN32)
    target_compile_definitions(${target} PRIVATE _WIN32_WINNT=0x0A00)  # Windows 10
  endif()
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra)
  endif()
endfunction()

# proto/*.proto -> 빌드 디렉터리의 생성 코드. 서버와 테스트 클라이언트가 같은 라이브러리를 쓴다.
add_library(game_proto STATIC proto/ping.proto)
target_link_libraries(game_proto PUBLIC protobuf::libprotobuf)
target_include_directories(game_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
protobuf_generate(
  TARGET game_proto
  IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/proto
  PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)

# 프레임 코덱: 소켓과 무관한 바이트 처리. 서버와 테스트 클라이언트가 함께 쓴다.
add_library(frame_codec STATIC src/protocol/frame.cpp)
target_include_directories(frame_codec PUBLIC src)
apply_project_options(frame_codec)

add_executable(frame_codec_test tests/frame_codec_test.cpp)
target_link_libraries(frame_codec_test PRIVATE frame_codec GTest::gtest_main)
apply_project_options(frame_codec_test)
gtest_discover_tests(frame_codec_test)

# 공용 도우미: 윈도우의 ANSI 오류 메시지를 UTF-8로 바꾼다. 서버와 클라이언트가 함께 쓴다.
add_library(common_util STATIC src/common/error_text.cpp)
target_include_directories(common_util PUBLIC src)
target_link_libraries(common_util PUBLIC Boost::headers)
apply_project_options(common_util)

# 서버 핵심 로직: 연결 하나를 다루는 Session과 연결을 받는 Server. 실행 파일과 분리해 두어
# 나중에 통합 테스트가 같은 코드를 그대로 링크할 수 있다.
add_library(server_core STATIC src/server/session.cpp src/server/server.cpp)
target_include_directories(server_core PUBLIC src)
target_link_libraries(server_core PUBLIC Boost::headers frame_codec game_proto common_util Threads::Threads)
if(WIN32)
  target_link_libraries(server_core PUBLIC ws2_32 mswsock)  # Winsock
endif()
apply_project_options(server_core)

# 서버 실행 파일: 인자 파싱과 io_context 실행만 담당한다.
add_executable(game_server src/server/main.cpp)
target_link_libraries(game_server PRIVATE server_core)
apply_project_options(game_server)

# 동기식 클라이언트: 테스트와 수동 실험용. 모든 대기에 타임아웃이 있다.
add_library(client_core STATIC src/client/blocking_client.cpp)
target_include_directories(client_core PUBLIC src)
target_link_libraries(client_core PUBLIC Boost::headers frame_codec game_proto common_util Threads::Threads)
if(WIN32)
  target_link_libraries(client_core PUBLIC ws2_32 mswsock)  # Winsock
endif()
apply_project_options(client_core)

# 수동 확인용: 서버에 Ping을 보내고 응답이 같은지 검사한다. (부하 생성기가 아니다)
add_executable(ping_client src/client/ping_client.cpp)
target_link_libraries(ping_client PRIVATE client_core)
apply_project_options(ping_client)

# 서버 통합 테스트: 테스트 프로세스 안에서 진짜 서버를 띄워 실제 TCP로 시나리오를 검증한다.
add_executable(server_integration_test tests/server_integration_test.cpp)
target_link_libraries(server_integration_test PRIVATE server_core client_core GTest::gtest_main)
apply_project_options(server_integration_test)
gtest_discover_tests(server_integration_test)

# 툴체인 확인용. 서버 구현이 시작되면 제거한다.
add_executable(hello_asio src/hello_asio.cpp)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT hello_asio)
target_link_libraries(hello_asio PRIVATE Boost::headers game_proto)
apply_project_options(hello_asio)
