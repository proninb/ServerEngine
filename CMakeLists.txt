cmake_minimum_required(VERSION 3.20)
project(ServerEntryV3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(server_entry_v3_core STATIC
    server_entry/diagnostics/diagnostic_buffer.cpp
    server_entry/json/json_parser.cpp
    server_entry/config/server_configuration_loader.cpp
    server_entry/project/project_configuration_loader.cpp
    server_entry/project/string/string_registry.cpp
    server_entry/project/identity/identity_registry.cpp
    server_entry/project/project_context.cpp
)

target_include_directories(server_entry_v3_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/server_entry)

if(MSVC)
    target_compile_options(server_entry_v3_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(server_entry_v3_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(server_entry_v3 server_entry/main.cpp)
target_link_libraries(server_entry_v3 PRIVATE server_entry_v3_core)

add_executable(server_entry_v3_tests tests/foundation_tests.cpp)
target_link_libraries(server_entry_v3_tests PRIVATE server_entry_v3_core)

enable_testing()
add_test(NAME server_entry_v3_foundation COMMAND server_entry_v3_tests)
