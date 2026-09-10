cmake_minimum_required(VERSION 3.20)
project(ServerEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(server_engine_core STATIC
    server_engine/diagnostics/diagnostic_buffer.cpp
    server_engine/json/json_parser.cpp
    server_engine/config/server_configuration_loader.cpp
    server_engine/project/project_configuration_loader.cpp
    server_engine/project/identity/identity_arena.cpp
    server_engine/project/identity/identity_space.cpp
    server_engine/project/frontend/source_facts_validation.cpp
    server_engine/project/frontend/include_discovery.cpp
    server_engine/project/frontend/source_frontend_generation.cpp
    server_engine/project/source/source_hash.cpp
    server_engine/project/source/file_snapshot.cpp
    server_engine/project/source/source_manager.cpp
    server_engine/project/parser/lexer.cpp
    server_engine/project/parser/source_environment.cpp
    server_engine/project/parser/source_parser.cpp
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
target_link_libraries(server_engine_tests PRIVATE server_engine_core Threads::Threads)

add_executable(server_engine_identity_benchmark benchmarks/identity_benchmark.cpp)
target_link_libraries(server_engine_identity_benchmark PRIVATE server_engine_core Threads::Threads)

add_executable(server_engine_source_manager_benchmark benchmarks/source_manager_benchmark.cpp)
target_link_libraries(server_engine_source_manager_benchmark PRIVATE server_engine_core Threads::Threads)

if(MSVC)
    target_compile_options(server_engine_tests PRIVATE /W4 /permissive-)
    target_compile_options(server_engine_identity_benchmark PRIVATE /W4 /permissive-)
    target_compile_options(server_engine_source_manager_benchmark PRIVATE /W4 /permissive-)
else()
    target_compile_options(server_engine_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(server_engine_identity_benchmark PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(server_engine_source_manager_benchmark PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME server_configuration COMMAND server_engine_tests server_configuration)
add_test(NAME project_configuration COMMAND server_engine_tests project_configuration)
add_test(NAME project_identity_resolution COMMAND server_engine_tests project_identity_resolution)
add_test(NAME project_identity_name_lifetime COMMAND server_engine_tests project_identity_name_lifetime)
add_test(NAME project_identity_concurrency COMMAND server_engine_tests project_identity_concurrency)
add_test(NAME project_identity_index_statistics COMMAND server_engine_tests project_identity_index_statistics)
add_test(NAME source_facts_contract COMMAND server_engine_tests source_facts_contract)
add_test(NAME source_facts_validation COMMAND server_engine_tests source_facts_validation)
add_test(NAME source_facts_enum_validation COMMAND server_engine_tests source_facts_enum_validation)
add_test(NAME source_facts_order_contract COMMAND server_engine_tests source_facts_order_contract)
add_test(NAME source_hash_sha256 COMMAND server_engine_tests source_hash_sha256)
add_test(NAME source_snapshot_immutability COMMAND server_engine_tests source_snapshot_immutability)
add_test(NAME source_manager_transactional_identity COMMAND server_engine_tests source_manager_transactional_identity)
add_test(NAME source_manager_file_acquisition COMMAND server_engine_tests source_manager_file_acquisition)
add_test(NAME lexer_include_discovery COMMAND server_engine_tests lexer_include_discovery)
add_test(NAME parser_source_facts_producer COMMAND server_engine_tests parser_source_facts_producer)
add_test(NAME parser_enum_facts COMMAND server_engine_tests parser_enum_facts)
add_test(NAME parser_cross_source_visibility COMMAND server_engine_tests parser_cross_source_visibility)
add_test(NAME parser_positional_include_visibility COMMAND server_engine_tests parser_positional_include_visibility)
add_test(NAME parser_unresolved_type COMMAND server_engine_tests parser_unresolved_type)
add_test(NAME parser_enclosing_namespace_lookup COMMAND server_engine_tests parser_enclosing_namespace_lookup)
add_test(NAME parser_parallel_sources COMMAND server_engine_tests parser_parallel_sources)
add_test(NAME frontend_include_pipeline COMMAND server_engine_tests frontend_include_pipeline)
add_test(NAME frontend_include_cycle COMMAND server_engine_tests frontend_include_cycle)
add_test(NAME frontend_parallel_roots COMMAND server_engine_tests frontend_parallel_roots)

add_test(NAME frontend_transitive_include_pipeline COMMAND server_engine_tests frontend_transitive_include_pipeline)
add_test(NAME frontend_shared_include_identity COMMAND server_engine_tests frontend_shared_include_identity)
add_test(NAME frontend_unsupported_directive COMMAND server_engine_tests frontend_unsupported_directive)
add_test(NAME frontend_include_inside_scope COMMAND server_engine_tests frontend_include_inside_scope)
