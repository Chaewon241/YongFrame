cmake_minimum_required(VERSION 3.25)
project(game_server_lab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

find_package(Boost CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
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

# 툴체인 확인용. 서버 구현이 시작되면 제거한다.
add_executable(hello_asio src/hello_asio.cpp)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT hello_asio)
target_link_libraries(hello_asio PRIVATE Boost::headers game_proto)
apply_project_options(hello_asio)
