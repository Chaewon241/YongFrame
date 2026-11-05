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
find_package(boost_mysql CONFIG REQUIRED)  # 2단계: Asio 기반 MySQL 클라이언트 (Boost.MySQL)
find_package(OpenSSL REQUIRED)             # Boost.MySQL이 인증/TLS에 사용
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
add_library(game_proto STATIC proto/ping.proto proto/chat.proto)
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
add_library(common_util STATIC src/common/error_text.cpp src/common/db_credentials.cpp)
target_include_directories(common_util PUBLIC src)
target_link_libraries(common_util PUBLIC Boost::headers)
apply_project_options(common_util)

# 채팅 로직: 입력 규칙, 저장소 인터페이스와 메모리 구현, 채팅 방. 소켓을 모르므로 가짜 연결로 단위 테스트할 수 있다.
add_library(chat_core STATIC
  src/chat/chat_rules.cpp
  src/chat/chat_room.cpp
  src/chat/in_memory_chat_repository.cpp)
target_include_directories(chat_core PUBLIC src)
target_link_libraries(chat_core PUBLIC Boost::headers frame_codec game_proto Threads::Threads)
if(WIN32)
  target_link_libraries(chat_core PUBLIC ws2_32 mswsock)  # asio가 Winsock을 초기화한다
endif()
apply_project_options(chat_core)

add_executable(chat_rules_test tests/chat_rules_test.cpp)
target_link_libraries(chat_rules_test PRIVATE chat_core GTest::gtest_main)
apply_project_options(chat_rules_test)
gtest_discover_tests(chat_rules_test)

add_executable(chat_room_test tests/chat_room_test.cpp)
target_link_libraries(chat_room_test PRIVATE chat_core GTest::gtest_main)
apply_project_options(chat_room_test)
gtest_discover_tests(chat_room_test)

# MySQL 저장소: Boost.MySQL(비동기, 코루틴)로 ChatRepository를 구현한다. OpenSSL 의존성을 이 라이브러리에 가둔다.
add_library(chat_mysql STATIC src/chat/mysql_chat_repository.cpp)
target_include_directories(chat_mysql PUBLIC src)
target_link_libraries(chat_mysql PUBLIC chat_core Boost::mysql OpenSSL::SSL OpenSSL::Crypto common_util)
if(WIN32)
  target_link_libraries(chat_mysql PUBLIC ws2_32 mswsock)
endif()
apply_project_options(chat_mysql)

# 서버 핵심 로직: 연결 하나를 다루는 Session과 연결을 받는 Server. 실행 파일과 분리해 두어
# 나중에 통합 테스트가 같은 코드를 그대로 링크할 수 있다.
add_library(server_core STATIC src/server/session.cpp src/server/server.cpp)
target_include_directories(server_core PUBLIC src)
target_link_libraries(server_core PUBLIC Boost::headers frame_codec game_proto chat_core common_util Threads::Threads)
if(WIN32)
  target_link_libraries(server_core PUBLIC ws2_32 mswsock)  # Winsock
endif()
apply_project_options(server_core)

# 서버 실행 파일: 인자 파싱과 io_context 실행만 담당한다.
add_executable(game_server src/server/main.cpp)
target_link_libraries(game_server PRIVATE server_core chat_mysql)
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

# 대화형 채팅 클라이언트: 터미널 여러 개에서 실행해 입장/대화/기록 조회를 직접 실험하는 용도.
add_executable(chat_client src/client/chat_client.cpp)
target_link_libraries(chat_client PRIVATE client_core)
apply_project_options(chat_client)

# 서버 통합 테스트: 테스트 프로세스 안에서 진짜 서버를 띄워 실제 TCP로 시나리오를 검증한다.
add_executable(server_integration_test tests/server_integration_test.cpp)
target_link_libraries(server_integration_test PRIVATE server_core client_core GTest::gtest_main)
apply_project_options(server_integration_test)
gtest_discover_tests(server_integration_test)

# 채팅 통합 테스트: 입장/대화/기록/저장소 장애/느린 수신자를 실제 TCP로 검증한다.
add_executable(chat_integration_test tests/chat_integration_test.cpp)
target_link_libraries(chat_integration_test PRIVATE server_core client_core GTest::gtest_main)
apply_project_options(chat_integration_test)
gtest_discover_tests(chat_integration_test)

# 부하 생성기(3단계): 많은 연결로 Ping을 보내 지연, 처리량, 오류, 서버 CPU/메모리를 잰다.
add_executable(load_client src/tools/load_client.cpp)
target_link_libraries(load_client PRIVATE client_core)
if(WIN32)
  target_link_libraries(load_client PRIVATE psapi winmm)  # psapi: 서버 프로세스의 메모리 조회, winmm: 타이머 해상도
endif()
apply_project_options(load_client)

# MySQL 연결 확인 도구: 서버 설치와 계정 설정이 맞는지, 드라이버가 두 환경에서 빌드되는지 본다.
add_executable(mysql_check src/tools/mysql_check.cpp)
target_link_libraries(mysql_check PRIVATE Boost::mysql OpenSSL::SSL OpenSSL::Crypto common_util)
if(WIN32)
  target_link_libraries(mysql_check PRIVATE ws2_32 mswsock)
endif()
apply_project_options(mysql_check)

# DB 마이그레이션 도구: db/migrations의 SQL을 번호 순서대로 적용하고 기록한다.
add_executable(db_migrate src/tools/db_migrate.cpp)
target_link_libraries(db_migrate PRIVATE Boost::mysql OpenSSL::SSL OpenSSL::Crypto common_util)
if(WIN32)
  target_link_libraries(db_migrate PRIVATE ws2_32 mswsock)
endif()
apply_project_options(db_migrate)

# MySQL 통합 테스트: 비밀번호(GAME_DB_PASSWORD 또는 GAME_DB_PASSWORD_FILE)가 없으면 건너뛴다.
add_executable(chat_mysql_test tests/chat_mysql_test.cpp)
target_link_libraries(chat_mysql_test PRIVATE chat_mysql server_core client_core GTest::gtest_main)
apply_project_options(chat_mysql_test)
gtest_discover_tests(chat_mysql_test)

# 툴체인 확인용. 서버 구현이 시작되면 제거한다.
add_executable(hello_asio src/hello_asio.cpp)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT hello_asio)
target_link_libraries(hello_asio PRIVATE Boost::headers game_proto)
apply_project_options(hello_asio)
