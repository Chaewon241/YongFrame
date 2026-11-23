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
find_package(unofficial-sodium CONFIG REQUIRED)  # 계정(P1): 비밀번호 해시(Argon2id)
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
add_library(game_proto STATIC proto/ping.proto proto/chat.proto proto/game.proto proto/account.proto)
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

# 계정(docs/persistence.md): 입력 규칙, 비밀번호 해시(libsodium Argon2id), 저장소 인터페이스와 메모리 구현, 계정 서비스.
# 소켓을 모르므로 가짜 연결로 단위 테스트할 수 있다. 해시는 시험에서 빠른 가짜로 바꿔 끼운다.
add_library(account_core STATIC
  src/account/account_rules.cpp
  src/account/password_hasher.cpp
  src/account/in_memory_account_repository.cpp
  src/account/account_service.cpp)
target_include_directories(account_core PUBLIC src)
target_link_libraries(account_core PUBLIC chat_core game_proto Boost::headers unofficial-sodium::sodium Threads::Threads)
apply_project_options(account_core)

add_executable(account_rules_test tests/account_rules_test.cpp)
target_link_libraries(account_rules_test PRIVATE account_core GTest::gtest_main)
apply_project_options(account_rules_test)
gtest_discover_tests(account_rules_test)

add_executable(password_hasher_test tests/password_hasher_test.cpp)
target_link_libraries(password_hasher_test PRIVATE account_core GTest::gtest_main)
apply_project_options(password_hasher_test)
gtest_discover_tests(password_hasher_test)

add_executable(account_service_test tests/account_service_test.cpp)
target_link_libraries(account_service_test PRIVATE account_core GTest::gtest_main)
apply_project_options(account_service_test)
gtest_discover_tests(account_service_test)

# 농장 저장(docs/persistence.md): 저장소 인터페이스(헤더)와 메모리 구현. 소켓을 모르므로 가짜 연결로 단위 테스트할 수 있다.
add_library(farm_core STATIC src/farm/in_memory_farm_repository.cpp)
target_include_directories(farm_core PUBLIC src)
target_link_libraries(farm_core PUBLIC account_core Boost::headers Threads::Threads)
apply_project_options(farm_core)

# 게임 로직(4단계): 월드(이동 계산), 방, 방 관리와 틱. 소켓을 모르므로 가짜 연결로 단위 테스트할 수 있다.
add_library(play_core STATIC src/play/world.cpp src/play/resources.cpp src/play/structures.cpp src/play/room.cpp src/play/farm_persistence.cpp src/play/game_service.cpp)
target_include_directories(play_core PUBLIC src)
target_link_libraries(play_core PUBLIC chat_core account_core farm_core game_proto Boost::headers Threads::Threads)
apply_project_options(play_core)

add_executable(play_world_test tests/play_world_test.cpp)
target_link_libraries(play_world_test PRIVATE play_core GTest::gtest_main)
apply_project_options(play_world_test)
gtest_discover_tests(play_world_test)

add_executable(play_resources_test tests/play_resources_test.cpp)
target_link_libraries(play_resources_test PRIVATE play_core GTest::gtest_main)
apply_project_options(play_resources_test)
gtest_discover_tests(play_resources_test)

add_executable(play_structures_test tests/play_structures_test.cpp)
target_link_libraries(play_structures_test PRIVATE play_core GTest::gtest_main)
apply_project_options(play_structures_test)
gtest_discover_tests(play_structures_test)

add_executable(play_collision_test tests/play_collision_test.cpp)
target_link_libraries(play_collision_test PRIVATE play_core GTest::gtest_main)
apply_project_options(play_collision_test)
gtest_discover_tests(play_collision_test)

add_executable(play_room_test tests/play_room_test.cpp)
target_link_libraries(play_room_test PRIVATE play_core GTest::gtest_main)
apply_project_options(play_room_test)
gtest_discover_tests(play_room_test)

add_executable(game_service_test tests/game_service_test.cpp)
target_link_libraries(game_service_test PRIVATE play_core GTest::gtest_main)
apply_project_options(game_service_test)
gtest_discover_tests(game_service_test)

# MySQL 저장소: Boost.MySQL(비동기, 코루틴)로 ChatRepository를 구현한다. OpenSSL 의존성을 이 라이브러리에 가둔다.
add_library(chat_mysql STATIC src/chat/mysql_chat_repository.cpp)
target_include_directories(chat_mysql PUBLIC src)
target_link_libraries(chat_mysql PUBLIC chat_core Boost::mysql OpenSSL::SSL OpenSSL::Crypto common_util)
if(WIN32)
  target_link_libraries(chat_mysql PUBLIC ws2_32 mswsock)
endif()
apply_project_options(chat_mysql)

# 계정 MySQL 저장소: AccountRepository를 Boost.MySQL로 구현한다(접속 정보와 연결 방식은 채팅 저장소와 같다).
add_library(account_mysql STATIC src/account/mysql_account_repository.cpp)
target_include_directories(account_mysql PUBLIC src)
target_link_libraries(account_mysql PUBLIC account_core chat_mysql Boost::mysql OpenSSL::SSL OpenSSL::Crypto common_util)
if(WIN32)
  target_link_libraries(account_mysql PUBLIC ws2_32 mswsock)
endif()
apply_project_options(account_mysql)

# 농장 MySQL 저장소: FarmRepository를 Boost.MySQL로 구현한다(접속 정보와 연결 방식은 채팅/계정 저장소와 같다).
add_library(farm_mysql STATIC src/farm/mysql_farm_repository.cpp)
target_include_directories(farm_mysql PUBLIC src)
target_link_libraries(farm_mysql PUBLIC farm_core chat_mysql Boost::mysql OpenSSL::SSL OpenSSL::Crypto common_util)
if(WIN32)
  target_link_libraries(farm_mysql PUBLIC ws2_32 mswsock)
endif()
apply_project_options(farm_mysql)

# 서버 핵심 로직: 연결 하나를 다루는 Session과 연결을 받는 Server. 실행 파일과 분리해 두어
# 나중에 통합 테스트가 같은 코드를 그대로 링크할 수 있다.
add_library(server_core STATIC src/server/session.cpp src/server/server.cpp src/server/stats_format.cpp)
target_include_directories(server_core PUBLIC src)
target_link_libraries(server_core PUBLIC Boost::headers frame_codec game_proto chat_core play_core account_core common_util Threads::Threads)
if(WIN32)
  target_link_libraries(server_core PUBLIC ws2_32 mswsock)  # Winsock
endif()
apply_project_options(server_core)

# 서버 실행 파일: 인자 파싱과 io_context 실행만 담당한다.
add_executable(game_server src/server/main.cpp)
target_link_libraries(game_server PRIVATE server_core chat_mysql account_mysql farm_mysql)
if(WIN32)
  target_link_libraries(game_server PRIVATE winmm)  # 게임 틱을 위해 타이머 해상도를 1ms로 올린다
endif()
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

# 대화형 게임 클라이언트(4단계): 방 만들기/코드 입장/이동 입력을 보내고 서버가 계산한 위치를 확인하는 수동 실험용.
add_executable(game_client src/client/game_client.cpp)
target_link_libraries(game_client PRIVATE client_core)
apply_project_options(game_client)

# 서버 통합 테스트: 테스트 프로세스 안에서 진짜 서버를 띄워 실제 TCP로 시나리오를 검증한다.
# 통계 출력 모양(--stats-format block) 단위 시험: 소켓 없이 문자열만 본다.
add_executable(stats_format_test tests/stats_format_test.cpp)
target_link_libraries(stats_format_test PRIVATE server_core GTest::gtest_main)
apply_project_options(stats_format_test)
gtest_discover_tests(stats_format_test)

add_executable(server_integration_test tests/server_integration_test.cpp)
target_link_libraries(server_integration_test PRIVATE server_core client_core GTest::gtest_main)
apply_project_options(server_integration_test)
gtest_discover_tests(server_integration_test)
# 같은 시나리오를 입출력 스레드 4개를 둔 서버로도 돌린다(tests/test_support.h의 GAME_TEST_IO_THREADS).
gtest_discover_tests(server_integration_test TEST_SUFFIX _io4 TEST_LIST server_integration_test_io4_tests PROPERTIES ENVIRONMENT GAME_TEST_IO_THREADS=4)

# 채팅 통합 테스트: 입장/대화/기록/저장소 장애/느린 수신자를 실제 TCP로 검증한다.
add_executable(chat_integration_test tests/chat_integration_test.cpp)
target_link_libraries(chat_integration_test PRIVATE server_core client_core GTest::gtest_main)
apply_project_options(chat_integration_test)
gtest_discover_tests(chat_integration_test)
# 같은 시나리오를 입출력 스레드 4개를 둔 서버로도 돌린다(tests/test_support.h의 GAME_TEST_IO_THREADS).
gtest_discover_tests(chat_integration_test TEST_SUFFIX _io4 TEST_LIST chat_integration_test_io4_tests PROPERTIES ENVIRONMENT GAME_TEST_IO_THREADS=4)

# 게임 통합 테스트(4단계): 방 만들기/입장/이동/스냅샷/연결 끊김/비정상 입력을 실제 TCP로 검증한다.
add_executable(game_integration_test tests/game_integration_test.cpp)
target_link_libraries(game_integration_test PRIVATE server_core client_core GTest::gtest_main)
apply_project_options(game_integration_test)
gtest_discover_tests(game_integration_test)
# 같은 시나리오를 입출력 스레드 4개를 둔 서버로도 돌린다(tests/test_support.h의 GAME_TEST_IO_THREADS).
gtest_discover_tests(game_integration_test TEST_SUFFIX _io4 TEST_LIST game_integration_test_io4_tests PROPERTIES ENVIRONMENT GAME_TEST_IO_THREADS=4)

# 계정 통합 테스트: 가입/로그인/중복 접속/실패 제한/방 닉네임/저장소 장애를 실제 TCP로 검증한다.
add_executable(account_integration_test tests/account_integration_test.cpp)
target_link_libraries(account_integration_test PRIVATE server_core client_core GTest::gtest_main)
apply_project_options(account_integration_test)
gtest_discover_tests(account_integration_test)
# 같은 시나리오를 입출력 스레드 4개를 둔 서버로도 돌린다(tests/test_support.h의 GAME_TEST_IO_THREADS).
gtest_discover_tests(account_integration_test TEST_SUFFIX _io4 TEST_LIST account_integration_test_io4_tests PROPERTIES ENVIRONMENT GAME_TEST_IO_THREADS=4)

# 부하 생성기(3단계): 많은 연결로 Ping을 보내 지연, 처리량, 오류, 서버 CPU/메모리를 잰다.
add_executable(load_client src/tools/load_client.cpp)
target_link_libraries(load_client PRIVATE client_core)
if(WIN32)
  target_link_libraries(load_client PRIVATE psapi winmm)  # psapi: 서버 프로세스의 메모리 조회, winmm: 타이머 해상도
endif()
apply_project_options(load_client)

# 게임 부하 생성기(4단계): 방 R개 x 4명이 입력을 보내고 20Hz 스냅샷을 받으며 지연, 누락, 정합성을 잰다.
add_executable(game_load_client src/tools/game_load_client.cpp)
target_link_libraries(game_load_client PRIVATE client_core)
if(WIN32)
  target_link_libraries(game_load_client PRIVATE psapi winmm)
endif()
apply_project_options(game_load_client)

# MySQL 연결 확인 도구: 서버 설치와 계정 설정이 맞는지, 드라이버가 두 환경에서 빌드되는지 본다.
add_executable(bench_deliver src/tools/bench_deliver.cpp)
target_link_libraries(bench_deliver PRIVATE frame_codec game_proto Boost::headers Threads::Threads)
apply_project_options(bench_deliver)

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

# 농장 저장 대기열 단위 시험(소켓 없이 인메모리 농장 저장소로).
add_executable(farm_persistence_test tests/farm_persistence_test.cpp)
target_link_libraries(farm_persistence_test PRIVATE play_core farm_core GTest::gtest_main)
apply_project_options(farm_persistence_test)
gtest_discover_tests(farm_persistence_test)

add_executable(farm_service_test tests/farm_service_test.cpp)
target_link_libraries(farm_service_test PRIVATE play_core farm_core GTest::gtest_main)
apply_project_options(farm_service_test)
gtest_discover_tests(farm_service_test)

# 농장 통합 테스트: 열기/채집/건설/나가기/다시 열기/저장 시점/저장소 장애를 실제 TCP로 검증한다.
add_executable(farm_integration_test tests/farm_integration_test.cpp)
target_link_libraries(farm_integration_test PRIVATE server_core client_core GTest::gtest_main)
apply_project_options(farm_integration_test)
gtest_discover_tests(farm_integration_test)
# 같은 시나리오를 입출력 스레드 4개를 둔 서버로도 돌린다(tests/test_support.h의 GAME_TEST_IO_THREADS).
gtest_discover_tests(farm_integration_test TEST_SUFFIX _io4 TEST_LIST farm_integration_test_io4_tests PROPERTIES ENVIRONMENT GAME_TEST_IO_THREADS=4)

# 계정 MySQL 시험: 비밀번호(GAME_DB_PASSWORD 또는 GAME_DB_PASSWORD_FILE)가 없으면 건너뛴다. 시험 전용 테이블만 쓴다.
add_executable(account_mysql_test tests/account_mysql_test.cpp)
target_link_libraries(account_mysql_test PRIVATE account_mysql farm_mysql chat_mysql server_core client_core GTest::gtest_main)
apply_project_options(account_mysql_test)
gtest_discover_tests(account_mysql_test)

# 툴체인 확인용. 서버 구현이 시작되면 제거한다.
add_executable(hello_asio src/hello_asio.cpp)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT hello_asio)
target_link_libraries(hello_asio PRIVATE Boost::headers game_proto)
apply_project_options(hello_asio)
