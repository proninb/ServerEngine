cmake_minimum_required(VERSION 3.20)
project(ServerEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(server_engine_core STATIC
    server_engine/diagnostics/diagnostic_buffer.cpp
    server_engine/json/json_parser.cpp
    server_engine/config/server_configuration_loader.cpp
    server_engine/project/project_configuration_loader.cpp
    server_engine/project/string/string_registry.cpp
    server_engine/project/identity/identity_registry.cpp
    server_engine/project/project_context.cpp
)

target_include_directories(server_engine_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/server_engine)

if(MSVC)
    target_compile_options(server_engine_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(server_engine_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(server_engine server_engine/main.cpp)
target_link_libraries(server_engine PRIVATE server_engine_core)

add_executable(server_engine_tests tests/foundation_tests.cpp)
target_link_libraries(server_engine_tests PRIVATE server_engine_core)

enable_testing()
add_test(NAME server_engine_foundation COMMAND server_engine_tests)
