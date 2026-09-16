#include "../server_engine/project/project_manager.hpp"
#include "../server_engine/project/persistence/baseline_store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace cw::server;

struct temporary_tree final {
    std::filesystem::path path;

    ~temporary_tree() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

struct sparse_case_result final {
    project_build_result build{};
    bool pass = false;
};

[[nodiscard]] bool write_text(
    const std::filesystem::path& path,
    std::string_view text) {

    std::ofstream output(
        path,
        std::ios::binary | std::ios::trunc);
    if (!output)
        return false;

    output.write(
        text.data(),
        static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(output);
}

[[nodiscard]] std::string source_name(std::size_t index) {
    return "s" + std::to_string(index) + ".hpp";
}

[[nodiscard]] std::string type_name(std::size_t index) {
    return "Type" + std::to_string(index);
}

[[nodiscard]] bool write_project_configuration(
    const std::filesystem::path& path,
    std::size_t source_count) {

    std::ofstream output(
        path,
        std::ios::binary | std::ios::trunc);
    if (!output)
        return false;

    output <<
        R"({"version":1,"name":"D3C Sparse Benchmark","project":[)";

    for (std::size_t index = 0; index < source_count; ++index) {
        if (index != 0)
            output << ',';

        output <<
            R"({"path":")" <<
            source_name(index) <<
            R"(","role":"type"})";
    }

    output <<
        R"(],"configuration":{"abi":{"target":"windows-x64","pack":8}}})";

    return static_cast<bool>(output);
}

[[nodiscard]] bool prepare_project(
    std::size_t source_count,
    temporary_tree& tree,
    std::filesystem::path& configuration_path,
    std::vector<std::filesystem::path>& sources,
    bool retain_source_paths = true,
    std::size_t progress_interval = 0) {

    if (source_count == 0 ||
        source_count >=
            static_cast<std::size_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
        return false;
    }

    tree.path =
        std::filesystem::temp_directory_path() /
        ("server_engine_v310d3c_" + std::to_string(source_count));

    std::error_code error;
    std::filesystem::remove_all(tree.path, error);
    error.clear();
    std::filesystem::create_directories(tree.path, error);
    if (error)
        return false;

    configuration_path = tree.path / "project.json";

    try {
        sources.clear();
        if (retain_source_paths)
            sources.reserve(source_count);

        for (std::size_t index = 0; index < source_count; ++index) {
            auto path = tree.path / source_name(index);
            const auto text =
                "struct " + type_name(index) + ";\n";
            if (!write_text(path, text))
                return false;

            if (retain_source_paths)
                sources.push_back(std::move(path));

            if (progress_interval != 0 &&
                (index + 1) % progress_interval == 0) {
                std::cerr
                    << "SETUP_PROGRESS,files="
                    << (index + 1)
                    << ",total=" << source_count
                    << '\n';
            }
        }
    }
    catch (...) {
        return false;
    }

    return write_project_configuration(
        configuration_path,
        source_count);
}

[[nodiscard]] bool create_baseline(
    const std::filesystem::path& configuration_path,
    baseline_commit_result& committed,
    std::size_t worker_limit = 1) {

    project_manager manager;
    diagnostic_buffer diagnostics;
    project_build_result rebuild;

    const auto rebuild_result = manager.rebuild(
        configuration_path,
        operation_id{3100},
        diagnostics,
        rebuild,
        worker_limit);

    if (!rebuild_result.ok() ||
        diagnostics.has_errors() ||
        !rebuild.changed ||
        !rebuild.rebuilt) {
        return false;
    }

    if (!manager.save(committed).ok() ||
        committed.transaction.empty() ||
        committed.bytes_written == 0) {
        return false;
    }

    return manager.unload().ok();
}

[[nodiscard]] std::size_t heap_growth(
    const project_build_result& value) noexcept {

    return value.telemetry.storage_after.retained_bytes >
        value.telemetry.storage_before.retained_bytes
        ? value.telemetry.storage_after.retained_bytes -
            value.telemetry.storage_before.retained_bytes
        : 0;
}

void print_header() {
    std::cout
        << "baseline_sources,scenario,"
           "manager_ms,configuration_identity_ms,configuration_probe_ms,configuration_gate_ms,"
           "configuration_ms,fingerprint_ms,"
           "baseline_open_ms,current_read_ms,embedded_manifest_parse_us,manifest_ms,compiled_map_ms,source_manager_map_ms,"
           "change_state_map_ms,build_cache_map_ms,size_validation_us,dirty_detection_ms,"
           "baseline_activation_ms,build_activation_ms,"
           "dirty_backend,dirty_fast,dirty_fallback,"
           "journal_records,journal_matched,"
           "orchestrator_ms,frontend_ms,"
           "frontend_root_resolve_ms,frontend_wave_setup_ms,"
           "frontend_acquire_prepare_ms,frontend_acquire_execute_ms,"
           "frontend_acquire_apply_ms,frontend_lex_discovery_ms,"
           "frontend_dependency_ms,frontend_graph_schedule_ms,"
           "frontend_parse_ms,frontend_materialize_ms,"
           "frontend_workers,frontend_max_workers,builder_ms,"
           "source_prepare_ms,interface_prepare_ms,publish_us,"
           "dirty_sources,frontend_dirty,frontend_changed,affected,"
           "acquired,lexed,parsed,reused_interfaces,"
           "source_graph_visited,reverse_edge_patches,"
           "builder_changed_sources,builder_changed_types,"
           "validation_visited_types,validation_dependency_edges,"
           "graph_full_scans,contribution_full_scans,"
           "heap_before,heap_after,heap_growth,status\n";
}

void print_result(
    std::size_t baseline_sources,
    std::string_view scenario,
    const project_build_result& value,
    bool pass) {

    const auto& telemetry = value.telemetry;

    std::cout
        << baseline_sources << ','
        << scenario << ','
        << static_cast<double>(
            telemetry.manager_total_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.configuration_identity_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.configuration_probe_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.configuration_gate_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.configuration_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.fingerprint_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.baseline_open_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.baseline_current_read_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.baseline_embedded_manifest_parse_ns) / 1'000.0 << ','
        << static_cast<double>(
            telemetry.baseline_manifest_validation_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.baseline_compiled_map_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.baseline_source_manager_map_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.baseline_change_state_map_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.baseline_build_cache_map_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.baseline_size_validation_ns) / 1'000.0 << ','
        << static_cast<double>(
            telemetry.dirty_detection_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.baseline_activation_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.build_activation_ns) / 1'000'000.0 << ','
        << telemetry.dirty_detection_backend << ','
        << (telemetry.dirty_detection_fast ? 1 : 0) << ','
        << (telemetry.dirty_detection_fallback ? 1 : 0) << ','
        << telemetry.journal_records << ','
        << telemetry.journal_matched_sources << ','
        << static_cast<double>(
            telemetry.total_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.frontend_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.frontend.root_resolve_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.frontend.wave_setup_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.frontend.acquire_prepare_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.frontend.acquire_execute_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.frontend.acquire_apply_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.frontend.lex_discovery_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.frontend.dependency_publish_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.frontend.graph_schedule_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.frontend.parse_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.frontend.result_materialize_ns) / 1'000'000.0 << ','
        << telemetry.frontend.worker_limit << ','
        << telemetry.frontend.max_active_workers << ','
        << static_cast<double>(
            telemetry.builder_prepare_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.source_prepare_publish_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.interface_prepare_publish_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.publication_ns) / 1'000.0 << ','
        << telemetry.dirty_sources << ','
        << telemetry.frontend.dirty << ','
        << telemetry.frontend.changed << ','
        << telemetry.frontend.affected << ','
        << telemetry.frontend.acquired << ','
        << telemetry.frontend.lexed << ','
        << telemetry.frontend.parsed << ','
        << telemetry.frontend.reused_interfaces << ','
        << telemetry.sources.source_graph_visited << ','
        << telemetry.sources.reverse_edge_patches << ','
        << telemetry.builder.changed_sources << ','
        << telemetry.builder.changed_types << ','
        << telemetry.builder.validation_visited_types << ','
        << telemetry.builder.validation_dependency_edges << ','
        << telemetry.builder.graph_full_scans << ','
        << telemetry.builder.contribution_full_scans << ','
        << telemetry.storage_before.retained_bytes << ','
        << telemetry.storage_after.retained_bytes << ','
        << heap_growth(value) << ','
        << (pass ? "PASS" : "FAIL")
        << '\n';
}

[[nodiscard]] bool validate_sparse_modify(
    std::size_t expected_sources,
    const project_build_result& value) noexcept {

    const auto& telemetry = value.telemetry;

    return
        value.changed &&
        !value.rebuilt &&
        telemetry.baseline_sources == expected_sources &&
        telemetry.dirty_sources == 1 &&
        telemetry.baseline_open_ns != 0 &&
        telemetry.dirty_detection_ns != 0 &&
        telemetry.manager_total_ns != 0 &&
        telemetry.frontend.dirty == 1 &&
        telemetry.frontend.changed == 1 &&
        telemetry.frontend.affected == 1 &&
        telemetry.frontend.acquired == 1 &&
        telemetry.frontend.lexed == 1 &&
        telemetry.frontend.parsed == 1 &&
        telemetry.builder.changed_sources == 1 &&
        telemetry.builder.changed_types == 1 &&
        telemetry.sources.path_index_full_rebuilds == 0 &&
        telemetry.sources.source_graph_full_scans == 0 &&
        telemetry.builder.graph_full_scans == 0 &&
        telemetry.builder.contribution_full_scans == 0;
}

[[nodiscard]] bool validate_no_change(
    std::size_t expected_sources,
    const project_build_result& value) noexcept {

    const auto& telemetry = value.telemetry;

    return
        !value.changed &&
        !value.rebuilt &&
        telemetry.baseline_sources == expected_sources &&
        telemetry.dirty_sources == 0 &&
        telemetry.baseline_open_ns != 0 &&
        telemetry.dirty_detection_ns != 0 &&
        telemetry.manager_total_ns != 0 &&
        telemetry.total_ns == 0 &&
        telemetry.frontend_ns == 0 &&
        telemetry.builder_prepare_ns == 0 &&
        telemetry.frontend.dirty == 0 &&
        telemetry.frontend.affected == 0 &&
        telemetry.builder.changed_sources == 0 &&
        telemetry.builder.changed_types == 0;
}


using lifecycle_clock = std::chrono::steady_clock;

[[nodiscard]] double elapsed_ms(
    lifecycle_clock::time_point begin,
    lifecycle_clock::time_point end) noexcept {

    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            end - begin).count()) /
        1'000'000.0;
}

void print_lifecycle_header() {
    std::cout
        << "baseline_sources,operation,worker_limit,wall_ms,"
           "manager_ms,baseline_open_ms,current_read_ms,"
           "source_manager_map_ms,build_cache_map_ms,"
           "dirty_detection_ms,baseline_activation_ms,"
           "build_activation_ms,orchestrator_ms,frontend_ms,"
           "frontend_root_resolve_ms,frontend_wave_setup_ms,"
           "frontend_acquire_prepare_ms,frontend_acquire_execute_ms,"
           "frontend_acquire_apply_ms,frontend_lex_discovery_ms,"
           "frontend_dependency_ms,frontend_graph_schedule_ms,"
           "frontend_parse_ms,frontend_materialize_ms,"
           "frontend_workers,frontend_max_workers,builder_ms,"
           "bytes_written,mib_written,mib_per_s,"
           "dirty_sources,changed,rebuilt,build_cache_mapped,"
           "transaction,status\n";
}

void print_lifecycle_result(
    std::size_t source_count,
    std::string_view operation,
    std::size_t worker_limit,
    double wall_ms,
    const project_build_result* build,
    const baseline_commit_result* commit,
    const project_load_result* load,
    bool pass) {

    const auto manager_ms =
        build != nullptr
            ? static_cast<double>(
                  build->telemetry.manager_total_ns) /
                  1'000'000.0
            : 0.0;

    const auto baseline_open_ms =
        build != nullptr
            ? static_cast<double>(
                  build->telemetry.baseline_open_ns) /
                  1'000'000.0
            : 0.0;

    const auto current_read_ms =
        build != nullptr
            ? static_cast<double>(
                  build->telemetry.baseline_current_read_ns) /
                  1'000'000.0
            : 0.0;

    const auto source_manager_map_ms =
        build != nullptr
            ? static_cast<double>(
                  build->telemetry.baseline_source_manager_map_ns) /
                  1'000'000.0
            : 0.0;

    const auto build_cache_map_ms =
        build != nullptr
            ? static_cast<double>(
                  build->telemetry.baseline_build_cache_map_ns) /
                  1'000'000.0
            : 0.0;

    const auto dirty_detection_ms =
        build != nullptr
            ? static_cast<double>(
                  build->telemetry.dirty_detection_ns) /
                  1'000'000.0
            : 0.0;

    const auto baseline_activation_ms =
        build != nullptr
            ? static_cast<double>(
                  build->telemetry.baseline_activation_ns) /
                  1'000'000.0
            : 0.0;

    const auto build_activation_ms =
        build != nullptr
            ? static_cast<double>(
                  build->telemetry.build_activation_ns) /
                  1'000'000.0
            : 0.0;

    const auto orchestrator_ms =
        build != nullptr
            ? static_cast<double>(
                  build->telemetry.total_ns) /
                  1'000'000.0
            : 0.0;

    const auto frontend_ms =
        build != nullptr
            ? static_cast<double>(
                  build->telemetry.frontend_ns) /
                  1'000'000.0
            : 0.0;

    const auto frontend_root_resolve_ms =
        build != nullptr
            ? static_cast<double>(
                  build->telemetry.frontend.root_resolve_ns) /
                  1'000'000.0
            : 0.0;

    const auto frontend_wave_setup_ms =
        build != nullptr
            ? static_cast<double>(
                  build->telemetry.frontend.wave_setup_ns) /
                  1'000'000.0
            : 0.0;

    const auto frontend_acquire_prepare_ms =
        build != nullptr
            ? static_cast<double>(
                  build->telemetry.frontend.acquire_prepare_ns) /
                  1'000'000.0
            : 0.0;

    const auto frontend_acquire_execute_ms =
        build != nullptr
            ? static_cast<double>(
                  build->telemetry.frontend.acquire_execute_ns) /
                  1'000'000.0
            : 0.0;

    const auto frontend_acquire_apply_ms =
        build != nullptr
            ? static_cast<double>(
                  build->telemetry.frontend.acquire_apply_ns) /
                  1'000'000.0
            : 0.0;

    const auto frontend_lex_discovery_ms =
        build != nullptr
            ? static_cast<double>(
                  build->telemetry.frontend.lex_discovery_ns) /
                  1'000'000.0
            : 0.0;

    const auto frontend_dependency_ms =
        build != nullptr
            ? static_cast<double>(
                  build->telemetry.frontend.dependency_publish_ns) /
                  1'000'000.0
            : 0.0;

    const auto frontend_graph_schedule_ms =
        build != nullptr
            ? static_cast<double>(
                  build->telemetry.frontend.graph_schedule_ns) /
                  1'000'000.0
            : 0.0;

    const auto frontend_parse_ms =
        build != nullptr
            ? static_cast<double>(
                  build->telemetry.frontend.parse_ns) /
                  1'000'000.0
            : 0.0;

    const auto frontend_materialize_ms =
        build != nullptr
            ? static_cast<double>(
                  build->telemetry.frontend.result_materialize_ns) /
                  1'000'000.0
            : 0.0;

    const auto frontend_workers =
        build != nullptr
            ? build->telemetry.frontend.worker_limit
            : std::size_t{0};

    const auto frontend_max_workers =
        build != nullptr
            ? build->telemetry.frontend.max_active_workers
            : std::size_t{0};

    const auto builder_ms =
        build != nullptr
            ? static_cast<double>(
                  build->telemetry.builder_prepare_ns) /
                  1'000'000.0
            : 0.0;

    const auto bytes_written =
        commit != nullptr
            ? commit->bytes_written
            : std::uint64_t{0};

    const auto mib_written =
        static_cast<double>(bytes_written) /
        (1024.0 * 1024.0);

    const auto mib_per_s =
        wall_ms > 0.0
            ? mib_written / (wall_ms / 1000.0)
            : 0.0;

    const auto dirty_sources =
        build != nullptr
            ? build->telemetry.dirty_sources
            : std::uint64_t{0};

    const auto changed =
        build != nullptr && build->changed;

    const auto rebuilt =
        build != nullptr && build->rebuilt;

    const auto build_cache_mapped =
        load != nullptr && load->build_cache_mapped;

    std::string_view transaction;
    if (commit != nullptr)
        transaction = commit->transaction;
    else if (load != nullptr)
        transaction = load->transaction;

    std::cout
        << source_count << ','
        << operation << ','
        << worker_limit << ','
        << wall_ms << ','
        << manager_ms << ','
        << baseline_open_ms << ','
        << current_read_ms << ','
        << source_manager_map_ms << ','
        << build_cache_map_ms << ','
        << dirty_detection_ms << ','
        << baseline_activation_ms << ','
        << build_activation_ms << ','
        << orchestrator_ms << ','
        << frontend_ms << ','
        << frontend_root_resolve_ms << ','
        << frontend_wave_setup_ms << ','
        << frontend_acquire_prepare_ms << ','
        << frontend_acquire_execute_ms << ','
        << frontend_acquire_apply_ms << ','
        << frontend_lex_discovery_ms << ','
        << frontend_dependency_ms << ','
        << frontend_graph_schedule_ms << ','
        << frontend_parse_ms << ','
        << frontend_materialize_ms << ','
        << frontend_workers << ','
        << frontend_max_workers << ','
        << builder_ms << ','
        << bytes_written << ','
        << mib_written << ','
        << mib_per_s << ','
        << dirty_sources << ','
        << (changed ? 1 : 0) << ','
        << (rebuilt ? 1 : 0) << ','
        << (build_cache_mapped ? 1 : 0) << ','
        << transaction << ','
        << (pass ? "PASS" : "FAIL")
        << '\n';
}


[[nodiscard]] int run_lazy_source_cycle_profile(
    std::size_t source_count,
    std::size_t worker_limit) {

    if (source_count == 0)
        return 2;

    temporary_tree tree;
    std::filesystem::path configuration_path;
    std::vector<std::filesystem::path> source_paths;

    const auto progress_interval =
        source_count >= 1'000'000
            ? std::size_t{100'000}
            : source_count >= 100'000
                ? std::size_t{10'000}
                : std::size_t{0};

    std::cerr
        << "LAZY_SOURCE_CYCLE_SETUP_BEGIN,sources="
        << source_count
        << '\n';

    const auto setup_begin = lifecycle_clock::now();
    if (!prepare_project(
            source_count,
            tree,
            configuration_path,
            source_paths,
            false,
            progress_interval)) {
        return 1;
    }
    const auto setup_end = lifecycle_clock::now();

    project_manager manager;
    diagnostic_buffer diagnostics;

    project_build_result rebuild;
    const auto rebuild_begin = lifecycle_clock::now();
    auto result = manager.rebuild(
        configuration_path,
        operation_id{4100},
        diagnostics,
        rebuild,
        worker_limit);
    const auto rebuild_end = lifecycle_clock::now();

    if (!result.ok() ||
        diagnostics.has_errors() ||
        !manager.ready()) {
        std::cout
            << "LAZY_SOURCE_CYCLE,FAIL,stage=rebuild"
            << ",status="
            << static_cast<unsigned>(result.code)
            << '\n';
        return 1;
    }

    baseline_commit_result save;
    const auto save_begin = lifecycle_clock::now();
    result = manager.save(save);
    const auto save_end = lifecycle_clock::now();

    if (!result.ok()) {
        std::cout
            << "LAZY_SOURCE_CYCLE,FAIL,stage=save"
            << ",status="
            << static_cast<unsigned>(result.code)
            << '\n';
        return 1;
    }

    const auto unload_after_save_begin =
        lifecycle_clock::now();
    result = manager.unload();
    const auto unload_after_save_end =
        lifecycle_clock::now();

    if (!result.ok()) {
        std::cout
            << "LAZY_SOURCE_CYCLE,FAIL,stage=unload_after_save"
            << ",status="
            << static_cast<unsigned>(result.code)
            << '\n';
        return 1;
    }

    project_load_result load;
    diagnostics.clear();

    const auto load_begin = lifecycle_clock::now();
    result = manager.load(
        configuration_path,
        operation_id{4101},
        diagnostics,
        load);
    const auto load_end = lifecycle_clock::now();

    if (!result.ok() ||
        diagnostics.has_errors() ||
        !manager.ready()) {
        std::cout
            << "LAZY_SOURCE_CYCLE,FAIL,stage=load"
            << ",status="
            << static_cast<unsigned>(result.code)
            << '\n';
        return 1;
    }

    project_access load_access;
    result = manager.acquire(load_access);
    if (!result.ok() || !load_access) {
        std::cout
            << "LAZY_SOURCE_CYCLE,FAIL,stage=load_acquire"
            << '\n';
        return 1;
    }

    const auto load_first_source_begin =
        lifecycle_clock::now();
    const auto load_first_count =
        load_access->sources().source_count();
    const auto load_first_source_end =
        lifecycle_clock::now();

    const auto load_warm_source_begin =
        lifecycle_clock::now();
    const auto load_warm_count =
        load_access->sources().source_count();
    const auto load_warm_source_end =
        lifecycle_clock::now();

    load_access.reset();

    const auto unload_after_load_begin =
        lifecycle_clock::now();
    result = manager.unload();
    const auto unload_after_load_end =
        lifecycle_clock::now();

    if (!result.ok()) {
        std::cout
            << "LAZY_SOURCE_CYCLE,FAIL,stage=unload_after_load"
            << '\n';
        return 1;
    }

    project_build_result build;
    diagnostics.clear();

    const auto build_begin = lifecycle_clock::now();
    result = manager.build(
        configuration_path,
        operation_id{4102},
        diagnostics,
        build,
        worker_limit);
    const auto build_end = lifecycle_clock::now();

    if (!result.ok() ||
        diagnostics.has_errors() ||
        !manager.ready()) {
        std::cout
            << "LAZY_SOURCE_CYCLE,FAIL,stage=build"
            << ",status="
            << static_cast<unsigned>(result.code)
            << '\n';
        return 1;
    }

    project_access build_access;
    result = manager.acquire(build_access);
    if (!result.ok() || !build_access) {
        std::cout
            << "LAZY_SOURCE_CYCLE,FAIL,stage=build_acquire"
            << '\n';
        return 1;
    }

    const auto build_first_source_begin =
        lifecycle_clock::now();
    const auto build_first_count =
        build_access->sources().source_count();
    const auto build_first_source_end =
        lifecycle_clock::now();

    const auto build_warm_source_begin =
        lifecycle_clock::now();
    const auto build_warm_count =
        build_access->sources().source_count();
    const auto build_warm_source_end =
        lifecycle_clock::now();

    build_access.reset();

    const auto unload_after_build_begin =
        lifecycle_clock::now();
    result = manager.unload();
    const auto unload_after_build_end =
        lifecycle_clock::now();

    const bool pass =
        result.ok() &&
        load_first_count == source_count &&
        load_warm_count == source_count &&
        build_first_count == source_count &&
        build_warm_count == source_count &&
        !load.build_cache_mapped;

    std::cout
        << std::fixed
        << std::setprecision(6)
        << "LAZY_SOURCE_CYCLE,"
        << (pass ? "PASS" : "FAIL")
        << ",sources=" << source_count
        << ",workers=" << worker_limit
        << ",setup_ms="
        << elapsed_ms(setup_begin, setup_end)
        << ",rebuild_ms="
        << elapsed_ms(rebuild_begin, rebuild_end)
        << ",save_ms="
        << elapsed_ms(save_begin, save_end)
        << ",unload_after_save_ms="
        << elapsed_ms(
               unload_after_save_begin,
               unload_after_save_end)
        << ",load_ms="
        << elapsed_ms(load_begin, load_end)
        << ",load_first_source_ms="
        << elapsed_ms(
               load_first_source_begin,
               load_first_source_end)
        << ",load_warm_source_ms="
        << elapsed_ms(
               load_warm_source_begin,
               load_warm_source_end)
        << ",unload_after_load_ms="
        << elapsed_ms(
               unload_after_load_begin,
               unload_after_load_end)
        << ",build_no_change_ms="
        << elapsed_ms(build_begin, build_end)
        << ",build_first_source_ms="
        << elapsed_ms(
               build_first_source_begin,
               build_first_source_end)
        << ",build_warm_source_ms="
        << elapsed_ms(
               build_warm_source_begin,
               build_warm_source_end)
        << ",unload_after_build_ms="
        << elapsed_ms(
               unload_after_build_begin,
               unload_after_build_end)
        << ",load_build_cache_mapped="
        << (load.build_cache_mapped ? 1 : 0)
        << ",build_dirty_sources="
        << build.telemetry.dirty_sources
        << '\n';

    return pass ? 0 : 1;
}

[[nodiscard]] int run_rebuild_profile(
    std::size_t source_count,
    std::size_t worker_limit,
    std::size_t acquisition_worker_limit = 0) {

    if (source_count == 0)
        return 2;

    temporary_tree tree;
    std::filesystem::path configuration_path;
    std::vector<std::filesystem::path> unused_source_paths;

    const auto progress_interval =
        source_count >= 1'000'000
            ? std::size_t{100'000}
            : source_count >= 100'000
                ? std::size_t{10'000}
                : std::size_t{0};

    std::cerr
        << "REBUILD_PROFILE_SETUP_BEGIN,sources="
        << source_count
        << '\n';

    const auto setup_begin = lifecycle_clock::now();
    if (!prepare_project(
            source_count,
            tree,
            configuration_path,
            unused_source_paths,
            false,
            progress_interval)) {
        return 1;
    }
    const auto setup_end = lifecycle_clock::now();

    project_manager manager;
    diagnostic_buffer diagnostics;
    project_build_result rebuild;

    const auto rebuild_begin = lifecycle_clock::now();
    const auto result = manager.rebuild(
        configuration_path,
        operation_id{3210},
        diagnostics,
        rebuild,
        worker_limit,
        acquisition_worker_limit);
    const auto rebuild_end = lifecycle_clock::now();

    const bool pass =
        result.ok() &&
        !diagnostics.has_errors() &&
        manager.ready() &&
        rebuild.changed &&
        rebuild.rebuilt;

    print_lifecycle_header();
    print_lifecycle_result(
        source_count,
        "rebuild_profile",
        worker_limit,
        elapsed_ms(rebuild_begin, rebuild_end),
        &rebuild,
        nullptr,
        nullptr,
        pass);

    const auto& f = rebuild.telemetry.frontend;
    const auto frontend_ms =
        static_cast<double>(
            rebuild.telemetry.frontend_ns) /
        1'000'000.0;

    const auto accounted_frontend_ms =
        static_cast<double>(
            f.root_resolve_ns +
            f.wave_setup_ns +
            f.acquire_prepare_ns +
            f.acquire_execute_ns +
            f.acquire_apply_ns +
            f.lex_discovery_ns +
            f.dependency_publish_ns +
            f.graph_schedule_ns +
            f.parse_ns +
            f.result_materialize_ns) /
        1'000'000.0;

    const auto frontend_unaccounted_ms =
        frontend_ms > accounted_frontend_ms
            ? frontend_ms - accounted_frontend_ms
            : 0.0;

    std::cout
        << "REBUILD_FRONTEND_BREAKDOWN,"
        << (pass ? "PASS" : "FAIL")
        << ",sources=" << source_count
        << ",wall_ms="
        << elapsed_ms(rebuild_begin, rebuild_end)
        << ",frontend_ms=" << frontend_ms
        << ",root_resolve_ms="
        << static_cast<double>(f.root_resolve_ns) / 1'000'000.0
        << ",wave_setup_ms="
        << static_cast<double>(f.wave_setup_ns) / 1'000'000.0
        << ",acquire_prepare_ms="
        << static_cast<double>(f.acquire_prepare_ns) / 1'000'000.0
        << ",acquire_execute_ms="
        << static_cast<double>(f.acquire_execute_ns) / 1'000'000.0
        << ",acquire_apply_ms="
        << static_cast<double>(f.acquire_apply_ns) / 1'000'000.0
        << ",lex_discovery_ms="
        << static_cast<double>(f.lex_discovery_ns) / 1'000'000.0
        << ",dependency_ms="
        << static_cast<double>(f.dependency_publish_ns) / 1'000'000.0
        << ",graph_schedule_ms="
        << static_cast<double>(f.graph_schedule_ns) / 1'000'000.0
        << ",parse_ms="
        << static_cast<double>(f.parse_ns) / 1'000'000.0
        << ",materialize_ms="
        << static_cast<double>(f.result_materialize_ns) / 1'000'000.0
        << ",frontend_unaccounted_ms="
        << frontend_unaccounted_ms
        << ",builder_ms="
        << static_cast<double>(
            rebuild.telemetry.builder_prepare_ns) /
            1'000'000.0
        << ",workers=" << f.worker_limit
        << ",io_workers=" << f.acquisition_worker_limit
        << ",max_workers=" << f.max_active_workers
        << ",dispatches=" << f.parallel_dispatches
        << ",worker_threads_created="
        << f.worker_threads_created
        << ",setup_ms="
        << elapsed_ms(setup_begin, setup_end)
        << '\n';

    if (manager.ready())
        (void)manager.unload();

    return pass ? 0 : 1;
}

[[nodiscard]] int run_lifecycle_scale(
    std::size_t source_count,
    std::size_t worker_limit) {

    if (source_count == 0)
        return 2;

    temporary_tree tree;
    std::filesystem::path configuration_path;
    std::vector<std::filesystem::path> unused_source_paths;

    std::cerr
        << "LIFECYCLE_SETUP_BEGIN,sources="
        << source_count
        << '\n';

    const auto setup_begin = lifecycle_clock::now();

    const auto progress_interval =
        source_count >= 1'000'000
            ? std::size_t{100'000}
            : source_count >= 100'000
                ? std::size_t{10'000}
                : std::size_t{0};

    if (!prepare_project(
            source_count,
            tree,
            configuration_path,
            unused_source_paths,
            false,
            progress_interval)) {
        std::cerr
            << "LIFECYCLE_SETUP,FAIL,sources="
            << source_count
            << '\n';
        return 1;
    }

    const auto setup_end = lifecycle_clock::now();
    const auto setup_ms =
        elapsed_ms(setup_begin, setup_end);

    std::cerr
        << "LIFECYCLE_SETUP,PASS,sources="
        << source_count
        << ",setup_ms=" << setup_ms
        << '\n';

    print_lifecycle_header();

    project_manager manager;
    diagnostic_buffer diagnostics;

    project_build_result rebuild;
    const auto rebuild_begin = lifecycle_clock::now();
    const auto rebuild_status = manager.rebuild(
        configuration_path,
        operation_id{3200},
        diagnostics,
        rebuild,
        worker_limit);
    const auto rebuild_end = lifecycle_clock::now();

    const bool rebuild_pass =
        rebuild_status.ok() &&
        !diagnostics.has_errors() &&
        manager.ready() &&
        rebuild.changed &&
        rebuild.rebuilt;

    print_lifecycle_result(
        source_count,
        "rebuild_g0",
        worker_limit,
        elapsed_ms(rebuild_begin, rebuild_end),
        &rebuild,
        nullptr,
        nullptr,
        rebuild_pass);

    if (!rebuild_pass) {
        if (manager.ready())
            (void)manager.unload();
        return 1;
    }

    baseline_commit_result save_g0;
    const auto save_g0_begin = lifecycle_clock::now();
    const auto save_g0_status =
        manager.save(save_g0);
    const auto save_g0_end = lifecycle_clock::now();

    const bool save_g0_pass =
        save_g0_status.ok() &&
        !save_g0.transaction.empty() &&
        save_g0.bytes_written != 0;

    print_lifecycle_result(
        source_count,
        "save_g0",
        worker_limit,
        elapsed_ms(save_g0_begin, save_g0_end),
        nullptr,
        &save_g0,
        nullptr,
        save_g0_pass);

    if (!save_g0_pass ||
        !manager.unload().ok()) {
        return 1;
    }

    diagnostics.clear();
    project_load_result load;
    const auto load_begin = lifecycle_clock::now();
    const auto load_status = manager.load(
        configuration_path,
        operation_id{3201},
        diagnostics,
        load);
    const auto load_end = lifecycle_clock::now();

    const bool load_pass =
        load_status.ok() &&
        !diagnostics.has_errors() &&
        manager.ready() &&
        !load.transaction.empty() &&
        !load.build_cache_mapped;

    print_lifecycle_result(
        source_count,
        "load_g0_warm",
        worker_limit,
        elapsed_ms(load_begin, load_end),
        nullptr,
        nullptr,
        &load,
        load_pass);

    if (!load_pass ||
        !manager.unload().ok()) {
        return 1;
    }

    diagnostics.clear();
    project_build_result no_change;
    const auto no_change_begin = lifecycle_clock::now();
    const auto no_change_status = manager.build(
        configuration_path,
        operation_id{3202},
        diagnostics,
        no_change,
        worker_limit);
    const auto no_change_end = lifecycle_clock::now();

    const bool no_change_pass =
        no_change_status.ok() &&
        !diagnostics.has_errors() &&
        manager.ready() &&
        validate_no_change(
            source_count,
            no_change);

    print_lifecycle_result(
        source_count,
        "build_no_change",
        worker_limit,
        elapsed_ms(
            no_change_begin,
            no_change_end),
        &no_change,
        nullptr,
        nullptr,
        no_change_pass);

    if (!no_change_pass ||
        !manager.unload().ok()) {
        return 1;
    }

    const auto target = source_count / 2;
    const auto target_path =
        tree.path / source_name(target);
    const auto changed_text =
        "struct " + type_name(target) +
        " { int value; };\n";

    if (!write_text(
            target_path,
            changed_text)) {
        return 1;
    }

    diagnostics.clear();
    project_build_result modify_one;
    const auto modify_begin = lifecycle_clock::now();
    const auto modify_status = manager.build(
        configuration_path,
        operation_id{3203},
        diagnostics,
        modify_one,
        worker_limit);
    const auto modify_end = lifecycle_clock::now();

    const bool modify_pass =
        modify_status.ok() &&
        !diagnostics.has_errors() &&
        manager.ready() &&
        validate_sparse_modify(
            source_count,
            modify_one) &&
        modify_one.telemetry
                .journal_matched_sources == 1 &&
        modify_one.telemetry
                .sources.source_graph_visited <= 1 &&
        modify_one.telemetry
                .builder.validation_visited_types == 1 &&
        modify_one.telemetry
                .builder.graph_full_scans == 0 &&
        modify_one.telemetry
                .builder.contribution_full_scans == 0;

    print_lifecycle_result(
        source_count,
        "build_modify_one",
        worker_limit,
        elapsed_ms(modify_begin, modify_end),
        &modify_one,
        nullptr,
        nullptr,
        modify_pass);

    if (!modify_pass) {
        if (manager.ready())
            (void)manager.unload();
        return 1;
    }

    baseline_commit_result save_g1;
    const auto save_g1_begin = lifecycle_clock::now();
    const auto save_g1_status =
        manager.save(save_g1);
    const auto save_g1_end = lifecycle_clock::now();

    const bool save_g1_pass =
        save_g1_status.ok() &&
        !save_g1.transaction.empty() &&
        save_g1.bytes_written != 0;

    print_lifecycle_result(
        source_count,
        "save_g1_after_modify",
        worker_limit,
        elapsed_ms(save_g1_begin, save_g1_end),
        nullptr,
        &save_g1,
        nullptr,
        save_g1_pass);

    if (manager.ready() &&
        !manager.unload().ok()) {
        return 1;
    }

    const bool pass =
        rebuild_pass &&
        save_g0_pass &&
        load_pass &&
        no_change_pass &&
        modify_pass &&
        save_g1_pass;

    std::cout
        << "LIFECYCLE_GATE,"
        << (pass ? "PASS" : "FAIL")
        << ",sources=" << source_count
        << ",worker_limit=" << worker_limit
        << ",setup_ms=" << setup_ms
        << '\n';

    return pass ? 0 : 1;
}

struct idempotent_save_timing final {
    double minimum_ms = 0.0;
    double median_ms = 0.0;
    double average_ms = 0.0;
    double maximum_ms = 0.0;
};

[[nodiscard]] idempotent_save_timing summarize_idempotent_save_timings(
    std::vector<double> samples) {

    idempotent_save_timing output;
    if (samples.empty())
        return output;

    double total = 0.0;
    output.minimum_ms = samples.front();
    output.maximum_ms = samples.front();

    for (const auto sample : samples) {
        total += sample;
        output.minimum_ms =
            (std::min)(output.minimum_ms, sample);
        output.maximum_ms =
            (std::max)(output.maximum_ms, sample);
    }

    std::sort(samples.begin(), samples.end());
    const auto middle = samples.size() / 2;
    if ((samples.size() & 1u) != 0u) {
        output.median_ms = samples[middle];
    }
    else {
        output.median_ms =
            (samples[middle - 1] + samples[middle]) / 2.0;
    }

    output.average_ms =
        total / static_cast<double>(samples.size());
    return output;
}

[[nodiscard]] int run_idempotent_save_benchmark(
    std::size_t source_count,
    std::size_t worker_limit,
    std::size_t repeat_count) {

    if (source_count == 0 || repeat_count == 0)
        return 2;

    temporary_tree tree;
    std::filesystem::path configuration_path;
    std::vector<std::filesystem::path> unused_source_paths;

    const auto progress_interval =
        source_count >= 1'000'000
            ? std::size_t{100'000}
            : source_count >= 100'000
                ? std::size_t{10'000}
                : std::size_t{0};

    std::cerr
        << "IDEMPOTENT_SAVE_SETUP_BEGIN,sources="
        << source_count
        << ",repeats=" << repeat_count
        << '\n';

    const auto setup_begin = lifecycle_clock::now();
    if (!prepare_project(
            source_count,
            tree,
            configuration_path,
            unused_source_paths,
            false,
            progress_interval)) {
        return 1;
    }
    const auto setup_end = lifecycle_clock::now();

    std::error_code file_size_error;
    const auto configuration_bytes =
        std::filesystem::file_size(
            configuration_path,
            file_size_error);
    if (file_size_error)
        return 1;

    project_manager manager;
    diagnostic_buffer diagnostics;
    project_build_result rebuild;

    const auto rebuild_begin = lifecycle_clock::now();
    auto result = manager.rebuild(
        configuration_path,
        operation_id{4200},
        diagnostics,
        rebuild,
        worker_limit);
    const auto rebuild_end = lifecycle_clock::now();

    if (!result.ok() ||
        diagnostics.has_errors() ||
        !manager.ready() ||
        !rebuild.changed ||
        !rebuild.rebuilt) {

        std::cout
            << "IDEMPOTENT_SAVE_GATE,FAIL,stage=rebuild"
            << ",sources=" << source_count
            << ",status="
            << static_cast<unsigned>(result.code)
            << '\n';
        return 1;
    }

    baseline_commit_result first_save;
    const auto first_save_begin = lifecycle_clock::now();
    result = manager.save(first_save);
    const auto first_save_end = lifecycle_clock::now();

    if (!result.ok() ||
        first_save.transaction.empty() ||
        first_save.bytes_written == 0) {

        std::cout
            << "IDEMPOTENT_SAVE_GATE,FAIL,stage=first_save"
            << ",sources=" << source_count
            << ",status="
            << static_cast<unsigned>(result.code)
            << '\n';
        return 1;
    }

    std::vector<double> construction_samples;
    construction_samples.reserve(repeat_count);

    std::uint64_t construction_repeat_bytes = 0;
    bool construction_pass = true;

    for (std::size_t index = 0;
         index < repeat_count;
         ++index) {

        baseline_commit_result repeated;
        const auto begin = lifecycle_clock::now();
        const auto repeated_status =
            manager.save(repeated);
        const auto end = lifecycle_clock::now();

        construction_samples.push_back(
            elapsed_ms(begin, end));
        construction_repeat_bytes +=
            repeated.bytes_written;

        if (!repeated_status.ok() ||
            repeated.transaction !=
                first_save.transaction ||
            repeated.bytes_written != 0) {
            construction_pass = false;
            break;
        }
    }

    const auto construction_timing =
        summarize_idempotent_save_timings(
            construction_samples);

    if (!construction_pass ||
        construction_samples.size() != repeat_count) {

        std::cout
            << "IDEMPOTENT_SAVE_GATE,FAIL,"
               "stage=construction_repeat"
            << ",sources=" << source_count
            << ",samples="
            << construction_samples.size()
            << ",repeat_bytes="
            << construction_repeat_bytes
            << '\n';

        if (manager.ready())
            (void)manager.unload();
        return 1;
    }

    if (!manager.unload().ok())
        return 1;

    diagnostics.clear();
    project_load_result load;

    const auto load_begin = lifecycle_clock::now();
    result = manager.load(
        configuration_path,
        operation_id{4201},
        diagnostics,
        load);
    const auto load_end = lifecycle_clock::now();

    if (!result.ok() ||
        diagnostics.has_errors() ||
        !manager.ready() ||
        load.transaction != first_save.transaction ||
        load.build_cache_mapped) {

        std::cout
            << "IDEMPOTENT_SAVE_GATE,FAIL,stage=load"
            << ",sources=" << source_count
            << ",status="
            << static_cast<unsigned>(result.code)
            << '\n';
        return 1;
    }

    std::vector<double> load_samples;
    load_samples.reserve(repeat_count);

    std::uint64_t load_repeat_bytes = 0;
    bool load_save_pass = true;

    for (std::size_t index = 0;
         index < repeat_count;
         ++index) {

        baseline_commit_result repeated;
        const auto begin = lifecycle_clock::now();
        const auto repeated_status =
            manager.save(repeated);
        const auto end = lifecycle_clock::now();

        load_samples.push_back(
            elapsed_ms(begin, end));
        load_repeat_bytes +=
            repeated.bytes_written;

        if (!repeated_status.ok() ||
            repeated.transaction !=
                first_save.transaction ||
            repeated.bytes_written != 0) {
            load_save_pass = false;
            break;
        }
    }

    const auto load_timing =
        summarize_idempotent_save_timings(
            load_samples);

    const bool pass =
        load_save_pass &&
        load_samples.size() == repeat_count &&
        construction_repeat_bytes == 0 &&
        load_repeat_bytes == 0;

    const auto first_save_total_ms =
        static_cast<double>(
            first_save.telemetry.save_total_ns) /
        1'000'000.0;

    const auto first_save_top_level_accounted_ms =
        static_cast<double>(
            first_save.telemetry.configuration_token_ns +
            first_save.telemetry.configuration_read_ns +
            first_save.telemetry.configuration_parse_ns +
            first_save.telemetry.fingerprint_ns +
            first_save.telemetry.generation_freeze_ns +
            first_save.telemetry.store_commit_ns) /
        1'000'000.0;

    const auto first_save_unaccounted_ms =
        first_save_total_ms >
            first_save_top_level_accounted_ms
        ? first_save_total_ms -
            first_save_top_level_accounted_ms
        : 0.0;

    const auto first_save_freeze_accounted_ms =
        static_cast<double>(
            first_save.telemetry.generation_freeze_materialize_change_ns +
            first_save.telemetry.generation_freeze_compiled_ns +
            first_save.telemetry.generation_freeze_roots_ns +
            first_save.telemetry.generation_freeze_source_manager_ns +
            first_save.telemetry.generation_freeze_change_state_ns +
            first_save.telemetry.generation_freeze_build_cache_ns +
            first_save.telemetry.generation_freeze_bind_ns +
            first_save.telemetry.generation_freeze_verify_change_state_ns +
            first_save.telemetry.generation_freeze_verify_build_cache_ns) /
        1'000'000.0;

    const auto source_frontend_sampled =
        first_save.telemetry.
            generation_freeze_build_cache_source_frontend_sampled_sources;
    const auto source_frontend_total =
        first_save.telemetry.
            generation_freeze_build_cache_source_frontend_total_sources;

    const auto source_frontend_sample_scale =
        source_frontend_sampled != 0
        ? static_cast<double>(source_frontend_total) /
            static_cast<double>(source_frontend_sampled)
        : 0.0;

    const auto sampled_estimated_ms =
        [&](std::uint64_t sample_ns) noexcept {
            return static_cast<double>(sample_ns) *
                source_frontend_sample_scale /
                1'000'000.0;
        };

    const auto first_save_sf_source_lookup_estimated_ms =
        sampled_estimated_ms(
            first_save.telemetry.
                generation_freeze_build_cache_source_lookup_sample_ns);
    const auto first_save_sf_text_copy_estimated_ms =
        sampled_estimated_ms(
            first_save.telemetry.
                generation_freeze_build_cache_source_text_copy_sample_ns);
    const auto first_save_sf_frontend_record_estimated_ms =
        sampled_estimated_ms(
            first_save.telemetry.
                generation_freeze_build_cache_frontend_record_sample_ns);
    const auto first_save_sf_local_types_estimated_ms =
        sampled_estimated_ms(
            first_save.telemetry.
                generation_freeze_build_cache_frontend_local_types_sample_ns);
    const auto first_save_sf_type_slots_estimated_ms =
        sampled_estimated_ms(
            first_save.telemetry.
                generation_freeze_build_cache_frontend_type_slots_sample_ns);
    const auto first_save_sf_object_slots_estimated_ms =
        sampled_estimated_ms(
            first_save.telemetry.
                generation_freeze_build_cache_frontend_object_slots_sample_ns);
    const auto first_save_sf_member_slots_estimated_ms =
        sampled_estimated_ms(
            first_save.telemetry.
                generation_freeze_build_cache_frontend_member_slots_sample_ns);

    const auto first_save_sf_sampled_accounted_estimated_ms =
        first_save_sf_source_lookup_estimated_ms +
        first_save_sf_text_copy_estimated_ms +
        first_save_sf_frontend_record_estimated_ms +
        first_save_sf_local_types_estimated_ms +
        first_save_sf_type_slots_estimated_ms +
        first_save_sf_object_slots_estimated_ms +
        first_save_sf_member_slots_estimated_ms;

    const auto first_save_sf_total_ms =
        static_cast<double>(
            first_save.telemetry.
                generation_freeze_build_cache_source_frontend_ns) /
        1'000'000.0;

    const auto first_save_sf_directory_other_estimated_ms =
        first_save_sf_total_ms >
            first_save_sf_sampled_accounted_estimated_ms
        ? first_save_sf_total_ms -
            first_save_sf_sampled_accounted_estimated_ms
        : 0.0;

    const auto first_save_sf_exact_source_directory_text_ms =
        static_cast<double>(
            first_save.telemetry.generation_freeze_build_cache_source_directory_text_ns) /
        1'000'000.0;
    const auto first_save_sf_exact_frontend_record_ranges_ms =
        static_cast<double>(
            first_save.telemetry.generation_freeze_build_cache_frontend_record_ranges_ns) /
        1'000'000.0;
    const auto first_save_sf_exact_local_types_ms =
        static_cast<double>(
            first_save.telemetry.generation_freeze_build_cache_frontend_local_types_ns) /
        1'000'000.0;
    const auto first_save_sf_exact_type_slots_ms =
        static_cast<double>(
            first_save.telemetry.generation_freeze_build_cache_frontend_type_slots_ns) /
        1'000'000.0;
    const auto first_save_sf_exact_object_slots_ms =
        static_cast<double>(
            first_save.telemetry.generation_freeze_build_cache_frontend_object_slots_ns) /
        1'000'000.0;
    const auto first_save_sf_exact_member_slots_ms =
        static_cast<double>(
            first_save.telemetry.generation_freeze_build_cache_frontend_member_slots_ns) /
        1'000'000.0;
    const auto first_save_sf_exact_accounted_ms =
        first_save_sf_exact_source_directory_text_ms +
        first_save_sf_exact_frontend_record_ranges_ms +
        first_save_sf_exact_local_types_ms +
        first_save_sf_exact_type_slots_ms +
        first_save_sf_exact_object_slots_ms +
        first_save_sf_exact_member_slots_ms;
    const auto first_save_sf_exact_unaccounted_ms =
        first_save_sf_total_ms > first_save_sf_exact_accounted_ms
        ? first_save_sf_total_ms - first_save_sf_exact_accounted_ms
        : 0.0;

    const auto first_save_build_cache_accounted_ms =
        static_cast<double>(
            first_save.telemetry.generation_freeze_build_cache_layout_allocate_ns +
            first_save.telemetry.generation_freeze_build_cache_source_frontend_ns +
            first_save.telemetry.generation_freeze_build_cache_contribution_ns +
            first_save.telemetry.generation_freeze_build_cache_graph_ns +
            first_save.telemetry.generation_freeze_build_cache_change_identity_ns +
            first_save.telemetry.generation_freeze_build_cache_section_crc_ns +
            first_save.telemetry.generation_freeze_build_cache_header_directory_ns +
            first_save.telemetry.generation_freeze_build_cache_bind_ns +
            first_save.telemetry.generation_freeze_build_cache_verify_ns) /
        1'000'000.0;

    const auto first_save_build_cache_total_ms =
        static_cast<double>(
            first_save.telemetry.generation_freeze_build_cache_ns) /
        1'000'000.0;

    const auto first_save_build_cache_unaccounted_ms =
        first_save_build_cache_total_ms >
            first_save_build_cache_accounted_ms
        ? first_save_build_cache_total_ms -
            first_save_build_cache_accounted_ms
        : 0.0;

    const auto first_save_freeze_internal_ms =
        static_cast<double>(
            first_save.telemetry.generation_freeze_internal_ns) /
        1'000'000.0;

    const auto first_save_freeze_unaccounted_ms =
        first_save_freeze_internal_ms >
            first_save_freeze_accounted_ms
        ? first_save_freeze_internal_ms -
            first_save_freeze_accounted_ms
        : 0.0;

    std::cout
        << std::fixed
        << std::setprecision(6)
        << "IDEMPOTENT_SAVE_RESULT,"
        << (pass ? "PASS" : "FAIL")
        << ",sources=" << source_count
        << ",workers=" << worker_limit
        << ",repeats=" << repeat_count
        << ",configuration_bytes="
        << configuration_bytes
        << ",setup_ms="
        << elapsed_ms(setup_begin, setup_end)
        << ",rebuild_ms="
        << elapsed_ms(rebuild_begin, rebuild_end)
        << ",first_save_ms="
        << elapsed_ms(first_save_begin, first_save_end)
        << ",first_save_total_ms="
        << first_save_total_ms
        << ",first_save_configuration_token_ms="
        << static_cast<double>(
            first_save.telemetry.configuration_token_ns) /
            1'000'000.0
        << ",first_save_configuration_read_ms="
        << static_cast<double>(
            first_save.telemetry.configuration_read_ns) /
            1'000'000.0
        << ",first_save_configuration_parse_ms="
        << static_cast<double>(
            first_save.telemetry.configuration_parse_ns) /
            1'000'000.0
        << ",first_save_fingerprint_ms="
        << static_cast<double>(
            first_save.telemetry.fingerprint_ns) /
            1'000'000.0
        << ",first_save_generation_freeze_ms="
        << static_cast<double>(
            first_save.telemetry.generation_freeze_ns) /
            1'000'000.0
        << ",first_save_freeze_internal_ms="
        << static_cast<double>(
            first_save.telemetry.generation_freeze_internal_ns) /
            1'000'000.0
        << ",first_save_freeze_materialize_change_ms="
        << static_cast<double>(
            first_save.telemetry.generation_freeze_materialize_change_ns) /
            1'000'000.0
        << ",first_save_freeze_compiled_ms="
        << static_cast<double>(
            first_save.telemetry.generation_freeze_compiled_ns) /
            1'000'000.0
        << ",first_save_freeze_roots_ms="
        << static_cast<double>(
            first_save.telemetry.generation_freeze_roots_ns) /
            1'000'000.0
        << ",first_save_freeze_source_manager_ms="
        << static_cast<double>(
            first_save.telemetry.generation_freeze_source_manager_ns) /
            1'000'000.0
        << ",first_save_freeze_change_state_ms="
        << static_cast<double>(
            first_save.telemetry.generation_freeze_change_state_ns) /
            1'000'000.0
        << ",first_save_freeze_build_cache_ms="
        << static_cast<double>(
            first_save.telemetry.generation_freeze_build_cache_ns) /
            1'000'000.0
        << ",first_save_build_cache_layout_allocate_ms="
        << static_cast<double>(
            first_save.telemetry.generation_freeze_build_cache_layout_allocate_ns) /
            1'000'000.0
        << ",first_save_build_cache_source_frontend_ms="
        << static_cast<double>(
            first_save.telemetry.generation_freeze_build_cache_source_frontend_ns) /
            1'000'000.0
        << ",first_save_sf_exact_source_directory_text_ms="
        << first_save_sf_exact_source_directory_text_ms
        << ",first_save_sf_exact_frontend_record_ranges_ms="
        << first_save_sf_exact_frontend_record_ranges_ms
        << ",first_save_sf_exact_local_types_ms="
        << first_save_sf_exact_local_types_ms
        << ",first_save_sf_exact_type_slots_ms="
        << first_save_sf_exact_type_slots_ms
        << ",first_save_sf_exact_object_slots_ms="
        << first_save_sf_exact_object_slots_ms
        << ",first_save_sf_exact_member_slots_ms="
        << first_save_sf_exact_member_slots_ms
        << ",first_save_sf_exact_accounted_ms="
        << first_save_sf_exact_accounted_ms
        << ",first_save_sf_exact_unaccounted_ms="
        << first_save_sf_exact_unaccounted_ms
        << ",first_save_sf_total_sources="
        << source_frontend_total
        << ",first_save_sf_sampled_sources="
        << source_frontend_sampled
        << ",first_save_sf_sample_scale="
        << source_frontend_sample_scale
        << ",first_save_sf_source_lookup_estimated_ms="
        << first_save_sf_source_lookup_estimated_ms
        << ",first_save_sf_text_copy_estimated_ms="
        << first_save_sf_text_copy_estimated_ms
        << ",first_save_sf_frontend_record_estimated_ms="
        << first_save_sf_frontend_record_estimated_ms
        << ",first_save_sf_local_types_estimated_ms="
        << first_save_sf_local_types_estimated_ms
        << ",first_save_sf_type_slots_estimated_ms="
        << first_save_sf_type_slots_estimated_ms
        << ",first_save_sf_object_slots_estimated_ms="
        << first_save_sf_object_slots_estimated_ms
        << ",first_save_sf_member_slots_estimated_ms="
        << first_save_sf_member_slots_estimated_ms
        << ",first_save_sf_sampled_accounted_estimated_ms="
        << first_save_sf_sampled_accounted_estimated_ms
        << ",first_save_sf_directory_other_estimated_ms="
        << first_save_sf_directory_other_estimated_ms
        << ",first_save_build_cache_contribution_ms="
        << static_cast<double>(
            first_save.telemetry.generation_freeze_build_cache_contribution_ns) /
            1'000'000.0
        << ",first_save_build_cache_graph_ms="
        << static_cast<double>(
            first_save.telemetry.generation_freeze_build_cache_graph_ns) /
            1'000'000.0
        << ",first_save_build_cache_change_identity_ms="
        << static_cast<double>(
            first_save.telemetry.generation_freeze_build_cache_change_identity_ns) /
            1'000'000.0
        << ",first_save_build_cache_section_crc_ms="
        << static_cast<double>(
            first_save.telemetry.generation_freeze_build_cache_section_crc_ns) /
            1'000'000.0
        << ",first_save_build_cache_header_directory_ms="
        << static_cast<double>(
            first_save.telemetry.generation_freeze_build_cache_header_directory_ns) /
            1'000'000.0
        << ",first_save_build_cache_bind_ms="
        << static_cast<double>(
            first_save.telemetry.generation_freeze_build_cache_bind_ns) /
            1'000'000.0
        << ",first_save_build_cache_verify_ms="
        << static_cast<double>(
            first_save.telemetry.generation_freeze_build_cache_verify_ns) /
            1'000'000.0
        << ",first_save_build_cache_accounted_ms="
        << first_save_build_cache_accounted_ms
        << ",first_save_build_cache_unaccounted_ms="
        << first_save_build_cache_unaccounted_ms
        << ",first_save_freeze_bind_ms="
        << static_cast<double>(
            first_save.telemetry.generation_freeze_bind_ns) /
            1'000'000.0
        << ",first_save_freeze_verify_change_state_ms="
        << static_cast<double>(
            first_save.telemetry.generation_freeze_verify_change_state_ns) /
            1'000'000.0
        << ",first_save_freeze_verify_build_cache_ms="
        << static_cast<double>(
            first_save.telemetry.generation_freeze_verify_build_cache_ns) /
            1'000'000.0
        << ",first_save_freeze_accounted_ms="
        << first_save_freeze_accounted_ms
        << ",first_save_freeze_unaccounted_ms="
        << first_save_freeze_unaccounted_ms
        << ",first_save_store_commit_ms="
        << static_cast<double>(
            first_save.telemetry.store_commit_ns) /
            1'000'000.0
        << ",first_save_transaction_write_ms="
        << static_cast<double>(
            first_save.telemetry.transaction_write_ns) /
            1'000'000.0
        << ",first_save_transaction_flush_ms="
        << static_cast<double>(
            first_save.telemetry.transaction_flush_ns) /
            1'000'000.0
        << ",first_save_directory_flush_ms="
        << static_cast<double>(
            first_save.telemetry.directory_flush_ns) /
            1'000'000.0
        << ",first_save_current_write_ms="
        << static_cast<double>(
            first_save.telemetry.current_write_ns) /
            1'000'000.0
        << ",first_save_current_flush_ms="
        << static_cast<double>(
            first_save.telemetry.current_flush_ns) /
            1'000'000.0
        << ",first_save_current_replace_ms="
        << static_cast<double>(
            first_save.telemetry.current_replace_ns) /
            1'000'000.0
        << ",first_save_unaccounted_ms="
        << first_save_unaccounted_ms
        << ",first_save_bytes="
        << first_save.bytes_written
        << ",construction_repeat_min_ms="
        << construction_timing.minimum_ms
        << ",construction_repeat_median_ms="
        << construction_timing.median_ms
        << ",construction_repeat_avg_ms="
        << construction_timing.average_ms
        << ",construction_repeat_max_ms="
        << construction_timing.maximum_ms
        << ",construction_repeat_bytes="
        << construction_repeat_bytes
        << ",load_ms="
        << elapsed_ms(load_begin, load_end)
        << ",load_repeat_min_ms="
        << load_timing.minimum_ms
        << ",load_repeat_median_ms="
        << load_timing.median_ms
        << ",load_repeat_avg_ms="
        << load_timing.average_ms
        << ",load_repeat_max_ms="
        << load_timing.maximum_ms
        << ",load_repeat_bytes="
        << load_repeat_bytes
        << ",transaction="
        << first_save.transaction
        << '\n';

    std::cout
        << "IDEMPOTENT_SAVE_GATE,"
        << (pass ? "PASS" : "FAIL")
        << ",sources=" << source_count
        << ",repeats=" << repeat_count
        << ",construction_bytes=0"
        << ",load_bytes=0"
        << ",same_transaction=1"
        << '\n';

    if (manager.ready() &&
        !manager.unload().ok()) {
        return 1;
    }

    return pass ? 0 : 1;
}


[[nodiscard]] bool run_sparse_case(
    std::size_t source_count,
    bool print_no_change,
    sparse_case_result& output) {

    output = {};

    temporary_tree tree;
    std::filesystem::path configuration_path;
    std::vector<std::filesystem::path> source_paths;
    if (!prepare_project(
            source_count,
            tree,
            configuration_path,
            source_paths)) {
        return false;
    }

    baseline_commit_result baseline;
    if (!create_baseline(
            configuration_path,
            baseline)) {
        return false;
    }

    const auto target = source_count / 2;
    const auto changed_text =
        "struct " + type_name(target) +
        " { int value; };\n";

    if (!write_text(
            source_paths[target],
            changed_text)) {
        return false;
    }

    project_manager manager;
    diagnostic_buffer diagnostics;
    project_build_result build;

    const auto build_status = manager.build(
        configuration_path,
        operation_id{3101},
        diagnostics,
        build,
        1);

    bool pass =
        build_status.ok() &&
        !diagnostics.has_errors() &&
        manager.state() ==
            project_lifecycle_state::ready &&
        validate_sparse_modify(
            source_count,
            build);

    print_result(
        source_count,
        "persisted_modify_one",
        build,
        pass);

    output.build = build;
    output.pass = pass;

    if (!pass) {
        if (manager.ready())
            (void)manager.unload();
        return false;
    }

    if (!print_no_change)
        return manager.unload().ok();

    baseline_commit_result changed_commit;
    if (!manager.save(changed_commit).ok() ||
        changed_commit.transaction.empty() ||
        !manager.unload().ok()) {
        return false;
    }

    diagnostics.clear();
    project_build_result no_change;
    const auto no_change_status = manager.build(
        configuration_path,
        operation_id{3102},
        diagnostics,
        no_change,
        1);

    const bool no_change_pass =
        no_change_status.ok() &&
        !diagnostics.has_errors() &&
        manager.state() ==
            project_lifecycle_state::ready &&
        validate_no_change(
            source_count,
            no_change);

    print_result(
        source_count,
        "persisted_no_change",
        no_change,
        no_change_pass);

    if (manager.ready() &&
        !manager.unload().ok()) {
        return false;
    }

    return no_change_pass;
}

[[nodiscard]] bool run_gate() {
    sparse_case_result small;
    sparse_case_result large;

    if (!run_sparse_case(
            1024,
            false,
            small)) {
        return false;
    }

    if (!run_sparse_case(
            8192,
            true,
            large)) {
        return false;
    }

    const auto small_heap =
        std::max<std::size_t>(
            heap_growth(small.build),
            1);

    const auto large_heap =
        heap_growth(large.build);

    // A baseline that is 8x larger must not produce an 8x construction heap
    // for the same one-Source semantic change. Allow generous allocator noise.
    const auto sparse_heap_limit =
        small_heap * 4 + 256u * 1024u;

    const bool pass =
        large_heap <= sparse_heap_limit;

    std::cout
        << "D3C_SPARSE_BUILD_GATE,"
        << (pass ? "PASS" : "FAIL")
        << ",small_heap=" << small_heap
        << ",large_heap=" << large_heap
        << ",limit=" << sparse_heap_limit
        << ",affected=1,no_full_scans\n";

    return pass;
}

[[nodiscard]] int run_fast_gate() {
#ifdef _WIN32
    constexpr std::size_t source_count = 100'000;
    constexpr double dirty_limit_ms = 10.0;
    constexpr double manager_limit_ms = 5.0;
    constexpr double baseline_open_limit_ms = 2.0;

    temporary_tree tree;
    std::filesystem::path configuration_path;
    std::vector<std::filesystem::path> source_paths;

    if (!prepare_project(
            source_count,
            tree,
            configuration_path,
            source_paths)) {
        return 1;
    }

    baseline_commit_result baseline;
    if (!create_baseline(
            configuration_path,
            baseline)) {
        return 1;
    }

    project_manager manager;
    diagnostic_buffer diagnostics;
    project_build_result build;

    const auto build_status = manager.build(
        configuration_path,
        operation_id{3110},
        diagnostics,
        build,
        1);

    const auto dirty_ms =
        static_cast<double>(
            build.telemetry.dirty_detection_ns) /
        1'000'000.0;

    const bool correctness =
        build_status.ok() &&
        !diagnostics.has_errors() &&
        manager.state() ==
            project_lifecycle_state::ready &&
        validate_no_change(
            source_count,
            build);

    print_result(
        source_count,
        "d3d_fast_no_change",
        build,
        correctness);

    if (manager.ready())
        (void)manager.unload();

    if (!correctness)
        return 1;

    if (!build.telemetry.dirty_detection_fast) {
        std::cout
            << "D3D_FAST_DIRTY_GATE,UNAVAILABLE,"
            << "backend="
            << build.telemetry.dirty_detection_backend
            << ",fallback="
            << (build.telemetry.dirty_detection_fallback ? 1 : 0)
            << ",run elevated on NTFS to require USN fast path\n";
        return 3;
    }

    const bool pass =
        !build.telemetry.dirty_detection_fallback &&
        build.telemetry.dirty_sources == 0 &&
        build.telemetry.configuration_probe_ns == 0 &&
        build.telemetry.configuration_ns == 0 &&
        build.telemetry.fingerprint_ns == 0 &&
        build.telemetry.baseline_open_ns != 0 &&
        build.telemetry.baseline_current_read_ns != 0 &&
        build.telemetry.baseline_embedded_manifest_parse_ns != 0 &&
        build.telemetry.baseline_change_state_map_ns == 0 &&
        build.telemetry.baseline_source_manager_map_ns == 0 &&
        build.telemetry.baseline_build_cache_map_ns == 0 &&
        dirty_ms <= dirty_limit_ms &&
        static_cast<double>(
            build.telemetry.manager_total_ns) / 1'000'000.0 <=
                manager_limit_ms &&
        static_cast<double>(
            build.telemetry.baseline_open_ns) / 1'000'000.0 <=
                baseline_open_limit_ms;

    std::cout
        << "D3D_FAST_DIRTY_GATE,"
        << (pass ? "PASS" : "FAIL")
        << ",dirty_ms=" << dirty_ms
        << ",dirty_limit_ms=" << dirty_limit_ms
        << ",manager_ms="
        << static_cast<double>(
            build.telemetry.manager_total_ns) / 1'000'000.0
        << ",manager_limit_ms=" << manager_limit_ms
        << ",baseline_open_ms="
        << static_cast<double>(
            build.telemetry.baseline_open_ns) / 1'000'000.0
        << ",baseline_open_limit_ms=" << baseline_open_limit_ms
        << ",embedded_gate=1"
        << ",journal_records="
        << build.telemetry.journal_records
        << ",baseline_sources=" << source_count
        << '\n';

    return pass ? 0 : 1;
#else
    std::cout
        << "D3D_FAST_DIRTY_GATE,UNAVAILABLE,"
        << "backend=0,platform=non_windows\n";
    return 3;
#endif
}

[[nodiscard]] int run_fast_modify_gate() {
#ifdef _WIN32
    constexpr std::size_t source_count = 100'000;
    constexpr double dirty_limit_ms = 10.0;
    constexpr double manager_limit_ms = 8.0;
    constexpr double baseline_open_limit_ms = 4.0;

    temporary_tree tree;
    std::filesystem::path configuration_path;
    std::vector<std::filesystem::path> source_paths;

    if (!prepare_project(
            source_count,
            tree,
            configuration_path,
            source_paths)) {
        return 1;
    }

    baseline_commit_result baseline;
    if (!create_baseline(
            configuration_path,
            baseline)) {
        return 1;
    }

    const auto target = source_count / 2;
    const auto changed_text =
        "struct " + type_name(target) +
        " { int value; };\n";

    if (!write_text(
            source_paths[target],
            changed_text)) {
        return 1;
    }

    project_manager manager;
    diagnostic_buffer diagnostics;
    project_build_result build;

    const auto build_status = manager.build(
        configuration_path,
        operation_id{3111},
        diagnostics,
        build,
        1);

    const auto dirty_ms =
        static_cast<double>(
            build.telemetry.dirty_detection_ns) /
        1'000'000.0;

    const bool sparse_correctness =
        build_status.ok() &&
        !diagnostics.has_errors() &&
        manager.state() ==
            project_lifecycle_state::ready &&
        validate_sparse_modify(
            source_count,
            build) &&
        build.telemetry.journal_matched_sources == 1 &&
        build.telemetry.sources.source_graph_visited <= 1 &&
        build.telemetry.sources.reverse_edge_patches == 0 &&
        build.telemetry.builder.validation_visited_types == 1 &&
        build.telemetry.builder.validation_dependency_edges == 0;

    const bool timing_correctness =
        build.telemetry.configuration_probe_ns == 0 &&
        build.telemetry.configuration_ns == 0 &&
        build.telemetry.fingerprint_ns == 0 &&
        build.telemetry.baseline_open_ns != 0 &&
        build.telemetry.baseline_source_manager_map_ns != 0 &&
        build.telemetry.baseline_build_cache_map_ns == 0 &&
        build.telemetry.dirty_detection_ns != 0 &&
        build.telemetry.baseline_activation_ns == 0 &&
        build.telemetry.build_activation_ns != 0 &&
        build.telemetry.manager_total_ns != 0;

    const bool correctness =
        sparse_correctness &&
        timing_correctness;

    print_result(
        source_count,
        "d3d_fast_modify_one",
        build,
        correctness);

    if (manager.ready())
        (void)manager.unload();

    if (!correctness)
        return 1;

    if (!build.telemetry.dirty_detection_fast) {
        std::cout
            << "D3D_FAST_MODIFY_GATE,UNAVAILABLE,"
            << "backend="
            << build.telemetry.dirty_detection_backend
            << ",fallback="
            << (build.telemetry.dirty_detection_fallback ? 1 : 0)
            << ",run elevated on NTFS to require USN fast path\n";
        return 3;
    }

    const bool pass =
        build.telemetry.dirty_detection_backend == 1 &&
        !build.telemetry.dirty_detection_fallback &&
        build.telemetry.dirty_sources == 1 &&
        build.telemetry.journal_matched_sources == 1 &&
        build.telemetry.baseline_source_manager_map_ns != 0 &&
        build.telemetry.baseline_build_cache_map_ns == 0 &&
        dirty_ms <= dirty_limit_ms &&
        static_cast<double>(
            build.telemetry.manager_total_ns) / 1'000'000.0 <=
                manager_limit_ms &&
        static_cast<double>(
            build.telemetry.baseline_open_ns) / 1'000'000.0 <=
                baseline_open_limit_ms;

    const auto accounted_ns =
        build.telemetry.configuration_identity_ns +
        build.telemetry.configuration_ns +
        build.telemetry.fingerprint_ns +
        build.telemetry.baseline_open_ns +
        build.telemetry.dirty_detection_ns +
        build.telemetry.build_activation_ns +
        build.telemetry.total_ns;

    const auto unaccounted_ns =
        build.telemetry.manager_total_ns > accounted_ns
            ? build.telemetry.manager_total_ns - accounted_ns
            : 0;

    std::cout
        << "D3D_FAST_MODIFY_GATE,"
        << (pass ? "PASS" : "FAIL")
        << ",dirty_ms=" << dirty_ms
        << ",dirty_limit_ms=" << dirty_limit_ms
        << ",manager_ms="
        << static_cast<double>(
            build.telemetry.manager_total_ns) / 1'000'000.0
        << ",manager_limit_ms=" << manager_limit_ms
        << ",baseline_open_ms="
        << static_cast<double>(
            build.telemetry.baseline_open_ns) / 1'000'000.0
        << ",baseline_open_limit_ms=" << baseline_open_limit_ms
        << ",source_manager_map_ms="
        << static_cast<double>(
            build.telemetry.baseline_source_manager_map_ns) / 1'000'000.0
        << ",build_cache_map_ms="
        << static_cast<double>(
            build.telemetry.baseline_build_cache_map_ns) / 1'000'000.0
        << ",packed_build_state=1"
        << ",journal_records="
        << build.telemetry.journal_records
        << ",journal_matched="
        << build.telemetry.journal_matched_sources
        << ",dirty_sources="
        << build.telemetry.dirty_sources
        << ",source_graph_visited="
        << build.telemetry.sources.source_graph_visited
        << ",validation_visited_types="
        << build.telemetry.builder.validation_visited_types
        << ",manager_ms="
        << static_cast<double>(
            build.telemetry.manager_total_ns) / 1'000'000.0
        << ",unaccounted_ms="
        << static_cast<double>(unaccounted_ns) / 1'000'000.0
        << ",baseline_sources=" << source_count
        << '\n';

    return pass ? 0 : 1;
#else
    std::cout
        << "D3D_FAST_MODIFY_GATE,UNAVAILABLE,"
        << "backend=0,platform=non_windows\n";
    return 3;
#endif
}

[[nodiscard]] int run_million_fast_build_gate() {
#ifdef _WIN32
    constexpr std::size_t source_count = 1'000'000;
    constexpr std::size_t sample_count = 5;

    constexpr double no_change_manager_median_limit_ms = 5.0;
    constexpr double no_change_baseline_median_limit_ms = 3.0;
    constexpr double no_change_dirty_median_limit_ms = 2.0;
    constexpr double no_change_manager_max_limit_ms = 8.0;

    constexpr double modify_manager_median_limit_ms = 5.0;
    constexpr double modify_baseline_median_limit_ms = 3.0;
    constexpr double modify_dirty_median_limit_ms = 2.0;
    constexpr double modify_orchestrator_median_limit_ms = 0.5;
    constexpr double modify_frontend_median_limit_ms = 0.4;
    constexpr double modify_builder_median_limit_ms = 0.1;
    constexpr double modify_manager_max_limit_ms = 8.0;

    struct timing_summary final {
        double minimum_ms = 0.0;
        double median_ms = 0.0;
        double maximum_ms = 0.0;
    };

    const auto summarize =
        [](std::vector<double> samples) -> timing_summary {

        timing_summary output;
        if (samples.empty())
            return output;

        std::sort(samples.begin(), samples.end());
        output.minimum_ms = samples.front();
        output.maximum_ms = samples.back();
        output.median_ms = samples[samples.size() / 2];
        return output;
    };

    const auto ms = [](std::uint64_t value) noexcept {
        return static_cast<double>(value) / 1'000'000.0;
    };

    temporary_tree tree;
    std::filesystem::path configuration_path;
    std::vector<std::filesystem::path> unused_source_paths;

    std::cerr
        << "MILLION_FAST_BUILD_SETUP_BEGIN,sources="
        << source_count
        << ",samples=" << sample_count
        << '\n';

    if (!prepare_project(
            source_count,
            tree,
            configuration_path,
            unused_source_paths,
            false,
            100'000)) {
        return 1;
    }

    std::cerr
        << "MILLION_FAST_BUILD_BASELINE_BEGIN,sources="
        << source_count
        << '\n';

    project_manager baseline_manager;
    diagnostic_buffer baseline_diagnostics;
    project_build_result baseline_rebuild;

    const auto baseline_rebuild_begin =
        lifecycle_clock::now();
    const auto baseline_rebuild_status =
        baseline_manager.rebuild(
            configuration_path,
            operation_id{3118},
            baseline_diagnostics,
            baseline_rebuild,
            0);
    const auto baseline_rebuild_end =
        lifecycle_clock::now();

    std::cout
        << "MILLION_FAST_BUILD_BASELINE_REBUILD,"
        << "status_code="
        << static_cast<unsigned>(
            baseline_rebuild_status.code)
        << ",diagnostic_errors="
        << (baseline_diagnostics.has_errors() ? 1 : 0)
        << ",ready="
        << (baseline_manager.ready() ? 1 : 0)
        << ",changed="
        << (baseline_rebuild.changed ? 1 : 0)
        << ",rebuilt="
        << (baseline_rebuild.rebuilt ? 1 : 0)
        << ",wall_ms="
        << elapsed_ms(
            baseline_rebuild_begin,
            baseline_rebuild_end)
        << ",frontend_ms="
        << ms(baseline_rebuild.telemetry.frontend_ns)
        << ",builder_ms="
        << ms(baseline_rebuild.telemetry.builder_prepare_ns)
        << '\n';

    if (!baseline_rebuild_status.ok() ||
        baseline_diagnostics.has_errors() ||
        !baseline_manager.ready() ||
        !baseline_rebuild.changed ||
        !baseline_rebuild.rebuilt) {

        if (baseline_manager.ready())
            (void)baseline_manager.unload();
        return 1;
    }

    baseline_commit_result baseline;

    const auto baseline_save_begin =
        lifecycle_clock::now();
    const auto baseline_save_status =
        baseline_manager.save(baseline);
    const auto baseline_save_end =
        lifecycle_clock::now();

    std::cout
        << "MILLION_FAST_BUILD_BASELINE_SAVE,"
        << "status_code="
        << static_cast<unsigned>(
            baseline_save_status.code)
        << ",transaction="
        << (baseline.transaction.empty()
            ? std::string_view{"<empty>"}
            : std::string_view{baseline.transaction})
        << ",bytes_written="
        << baseline.bytes_written
        << ",wall_ms="
        << elapsed_ms(
            baseline_save_begin,
            baseline_save_end)
        << ",save_total_ms="
        << ms(baseline.telemetry.save_total_ns)
        << ",freeze_ms="
        << ms(baseline.telemetry.generation_freeze_ns)
        << ",store_ms="
        << ms(baseline.telemetry.store_commit_ns)
        << '\n';

    if (!baseline_save_status.ok() ||
        baseline.transaction.empty() ||
        baseline.bytes_written == 0) {

        if (baseline_manager.ready())
            (void)baseline_manager.unload();
        return 1;
    }

    const auto baseline_unload_begin =
        lifecycle_clock::now();
    const auto baseline_unload_status =
        baseline_manager.unload();
    const auto baseline_unload_end =
        lifecycle_clock::now();

    std::cout
        << "MILLION_FAST_BUILD_BASELINE_UNLOAD,"
        << "status_code="
        << static_cast<unsigned>(
            baseline_unload_status.code)
        << ",wall_ms="
        << elapsed_ms(
            baseline_unload_begin,
            baseline_unload_end)
        << '\n';

    if (!baseline_unload_status.ok())
        return 1;

    std::vector<double> no_change_manager_samples;
    std::vector<double> no_change_baseline_samples;
    std::vector<double> no_change_dirty_samples;

    std::vector<double> modify_manager_samples;
    std::vector<double> modify_baseline_samples;
    std::vector<double> modify_source_manager_samples;
    std::vector<double> modify_dirty_samples;
    std::vector<double> modify_orchestrator_samples;
    std::vector<double> modify_frontend_samples;
    std::vector<double> modify_builder_samples;

    no_change_manager_samples.reserve(sample_count);
    no_change_baseline_samples.reserve(sample_count);
    no_change_dirty_samples.reserve(sample_count);

    modify_manager_samples.reserve(sample_count);
    modify_baseline_samples.reserve(sample_count);
    modify_source_manager_samples.reserve(sample_count);
    modify_dirty_samples.reserve(sample_count);
    modify_orchestrator_samples.reserve(sample_count);
    modify_frontend_samples.reserve(sample_count);
    modify_builder_samples.reserve(sample_count);

    project_manager manager;
    diagnostic_buffer diagnostics;

    for (std::size_t sample = 0;
         sample < sample_count;
         ++sample) {

        diagnostics.clear();
        project_build_result no_change;

        const auto build_status = manager.build(
            configuration_path,
            operation_id{
                static_cast<std::uint64_t>(
                    3120 + sample)},
            diagnostics,
            no_change,
            1);

        const bool correctness =
            build_status.ok() &&
            !diagnostics.has_errors() &&
            manager.state() ==
                project_lifecycle_state::ready &&
            validate_no_change(
                source_count,
                no_change) &&
            no_change.telemetry.configuration_probe_ns == 0 &&
            no_change.telemetry.configuration_ns == 0 &&
            no_change.telemetry.fingerprint_ns == 0 &&
            no_change.telemetry.baseline_open_ns != 0 &&
            no_change.telemetry.baseline_source_manager_map_ns == 0 &&
            no_change.telemetry.baseline_build_cache_map_ns == 0 &&
            no_change.telemetry.baseline_activation_ns != 0 &&
            no_change.telemetry.build_activation_ns == 0 &&
            no_change.telemetry.dirty_detection_backend == 1 &&
            no_change.telemetry.dirty_detection_fast &&
            !no_change.telemetry.dirty_detection_fallback &&
            no_change.telemetry.dirty_sources == 0 &&
            no_change.telemetry.sources.path_index_full_rebuilds == 0 &&
            no_change.telemetry.sources.source_graph_full_scans == 0 &&
            no_change.telemetry.builder.graph_full_scans == 0 &&
            no_change.telemetry.builder.contribution_full_scans == 0;

        const auto scenario =
            "d3d_million_fast_no_change_" +
            std::to_string(sample + 1);

        print_result(
            source_count,
            scenario,
            no_change,
            correctness);

        const auto unload_status =
            manager.ready()
                ? manager.unload()
                : status{status_code::invalid_state};

        if (!correctness ||
            !unload_status.ok()) {
            return 1;
        }

        no_change_manager_samples.push_back(
            ms(no_change.telemetry.manager_total_ns));
        no_change_baseline_samples.push_back(
            ms(no_change.telemetry.baseline_open_ns));
        no_change_dirty_samples.push_back(
            ms(no_change.telemetry.dirty_detection_ns));
    }

    const auto target = source_count / 2;
    const auto target_path =
        tree.path / source_name(target);
    const auto changed_text =
        "struct " + type_name(target) +
        " { int value; };\n";

    if (!write_text(
            target_path,
            changed_text)) {
        return 1;
    }

    for (std::size_t sample = 0;
         sample < sample_count;
         ++sample) {

        diagnostics.clear();
        project_build_result modify_one;

        const auto build_status = manager.build(
            configuration_path,
            operation_id{
                static_cast<std::uint64_t>(
                    3130 + sample)},
            diagnostics,
            modify_one,
            1);

        const bool correctness =
            build_status.ok() &&
            !diagnostics.has_errors() &&
            manager.state() ==
                project_lifecycle_state::ready &&
            validate_sparse_modify(
                source_count,
                modify_one) &&
            modify_one.telemetry.configuration_probe_ns == 0 &&
            modify_one.telemetry.configuration_ns == 0 &&
            modify_one.telemetry.fingerprint_ns == 0 &&
            modify_one.telemetry.baseline_open_ns != 0 &&
            modify_one.telemetry.baseline_source_manager_map_ns != 0 &&
            modify_one.telemetry.baseline_build_cache_map_ns == 0 &&
            modify_one.telemetry.baseline_activation_ns == 0 &&
            modify_one.telemetry.build_activation_ns != 0 &&
            modify_one.telemetry.dirty_detection_backend == 1 &&
            modify_one.telemetry.dirty_detection_fast &&
            !modify_one.telemetry.dirty_detection_fallback &&
            modify_one.telemetry.dirty_sources == 1 &&
            modify_one.telemetry.journal_matched_sources == 1 &&
            modify_one.telemetry.sources.source_graph_visited <= 1 &&
            modify_one.telemetry.sources.reverse_edge_patches == 0 &&
            modify_one.telemetry.builder.validation_visited_types == 1 &&
            modify_one.telemetry.builder.validation_dependency_edges == 0 &&
            modify_one.telemetry.sources.path_index_full_rebuilds == 0 &&
            modify_one.telemetry.sources.source_graph_full_scans == 0 &&
            modify_one.telemetry.builder.graph_full_scans == 0 &&
            modify_one.telemetry.builder.contribution_full_scans == 0;

        const auto scenario =
            "d3d_million_fast_modify_one_" +
            std::to_string(sample + 1);

        print_result(
            source_count,
            scenario,
            modify_one,
            correctness);

        const auto unload_status =
            manager.ready()
                ? manager.unload()
                : status{status_code::invalid_state};

        if (!correctness ||
            !unload_status.ok()) {
            return 1;
        }

        modify_manager_samples.push_back(
            ms(modify_one.telemetry.manager_total_ns));
        modify_baseline_samples.push_back(
            ms(modify_one.telemetry.baseline_open_ns));
        modify_source_manager_samples.push_back(
            ms(modify_one.telemetry.baseline_source_manager_map_ns));
        modify_dirty_samples.push_back(
            ms(modify_one.telemetry.dirty_detection_ns));
        modify_orchestrator_samples.push_back(
            ms(modify_one.telemetry.total_ns));
        modify_frontend_samples.push_back(
            ms(modify_one.telemetry.frontend_ns));
        modify_builder_samples.push_back(
            ms(modify_one.telemetry.builder_prepare_ns));
    }

    const auto no_change_manager =
        summarize(no_change_manager_samples);
    const auto no_change_baseline =
        summarize(no_change_baseline_samples);
    const auto no_change_dirty =
        summarize(no_change_dirty_samples);

    const auto modify_manager =
        summarize(modify_manager_samples);
    const auto modify_baseline =
        summarize(modify_baseline_samples);
    const auto modify_source_manager =
        summarize(modify_source_manager_samples);
    const auto modify_dirty =
        summarize(modify_dirty_samples);
    const auto modify_orchestrator =
        summarize(modify_orchestrator_samples);
    const auto modify_frontend =
        summarize(modify_frontend_samples);
    const auto modify_builder =
        summarize(modify_builder_samples);

    const bool pass =
        no_change_manager_samples.size() == sample_count &&
        modify_manager_samples.size() == sample_count &&

        no_change_manager.median_ms <=
            no_change_manager_median_limit_ms &&
        no_change_baseline.median_ms <=
            no_change_baseline_median_limit_ms &&
        no_change_dirty.median_ms <=
            no_change_dirty_median_limit_ms &&
        no_change_manager.maximum_ms <=
            no_change_manager_max_limit_ms &&

        modify_manager.median_ms <=
            modify_manager_median_limit_ms &&
        modify_baseline.median_ms <=
            modify_baseline_median_limit_ms &&
        modify_dirty.median_ms <=
            modify_dirty_median_limit_ms &&
        modify_orchestrator.median_ms <=
            modify_orchestrator_median_limit_ms &&
        modify_frontend.median_ms <=
            modify_frontend_median_limit_ms &&
        modify_builder.median_ms <=
            modify_builder_median_limit_ms &&
        modify_manager.maximum_ms <=
            modify_manager_max_limit_ms;

    std::cout
        << "D3D_MILLION_FAST_BUILD_MEDIAN_GATE,"
        << (pass ? "PASS" : "FAIL")
        << ",sources=" << source_count
        << ",samples=" << sample_count

        << ",no_change_manager_min_ms="
        << no_change_manager.minimum_ms
        << ",no_change_manager_median_ms="
        << no_change_manager.median_ms
        << ",no_change_manager_max_ms="
        << no_change_manager.maximum_ms

        << ",no_change_baseline_min_ms="
        << no_change_baseline.minimum_ms
        << ",no_change_baseline_median_ms="
        << no_change_baseline.median_ms
        << ",no_change_baseline_max_ms="
        << no_change_baseline.maximum_ms

        << ",no_change_dirty_min_ms="
        << no_change_dirty.minimum_ms
        << ",no_change_dirty_median_ms="
        << no_change_dirty.median_ms
        << ",no_change_dirty_max_ms="
        << no_change_dirty.maximum_ms

        << ",modify_manager_min_ms="
        << modify_manager.minimum_ms
        << ",modify_manager_median_ms="
        << modify_manager.median_ms
        << ",modify_manager_max_ms="
        << modify_manager.maximum_ms

        << ",modify_baseline_min_ms="
        << modify_baseline.minimum_ms
        << ",modify_baseline_median_ms="
        << modify_baseline.median_ms
        << ",modify_baseline_max_ms="
        << modify_baseline.maximum_ms

        << ",modify_source_manager_min_ms="
        << modify_source_manager.minimum_ms
        << ",modify_source_manager_median_ms="
        << modify_source_manager.median_ms
        << ",modify_source_manager_max_ms="
        << modify_source_manager.maximum_ms

        << ",modify_dirty_min_ms="
        << modify_dirty.minimum_ms
        << ",modify_dirty_median_ms="
        << modify_dirty.median_ms
        << ",modify_dirty_max_ms="
        << modify_dirty.maximum_ms

        << ",modify_orchestrator_min_ms="
        << modify_orchestrator.minimum_ms
        << ",modify_orchestrator_median_ms="
        << modify_orchestrator.median_ms
        << ",modify_orchestrator_max_ms="
        << modify_orchestrator.maximum_ms

        << ",modify_frontend_min_ms="
        << modify_frontend.minimum_ms
        << ",modify_frontend_median_ms="
        << modify_frontend.median_ms
        << ",modify_frontend_max_ms="
        << modify_frontend.maximum_ms

        << ",modify_builder_min_ms="
        << modify_builder.minimum_ms
        << ",modify_builder_median_ms="
        << modify_builder.median_ms
        << ",modify_builder_max_ms="
        << modify_builder.maximum_ms

        << ",no_change_manager_median_limit_ms="
        << no_change_manager_median_limit_ms
        << ",no_change_baseline_median_limit_ms="
        << no_change_baseline_median_limit_ms
        << ",no_change_dirty_median_limit_ms="
        << no_change_dirty_median_limit_ms
        << ",no_change_manager_max_limit_ms="
        << no_change_manager_max_limit_ms

        << ",modify_manager_median_limit_ms="
        << modify_manager_median_limit_ms
        << ",modify_baseline_median_limit_ms="
        << modify_baseline_median_limit_ms
        << ",modify_dirty_median_limit_ms="
        << modify_dirty_median_limit_ms
        << ",modify_orchestrator_median_limit_ms="
        << modify_orchestrator_median_limit_ms
        << ",modify_frontend_median_limit_ms="
        << modify_frontend_median_limit_ms
        << ",modify_builder_median_limit_ms="
        << modify_builder_median_limit_ms
        << ",modify_manager_max_limit_ms="
        << modify_manager_max_limit_ms

        << ",build_cache_map_ms=0"
        << ",dirty_no_change=0"
        << ",dirty_modify=1"
        << ",journal_matched_modify=1"
        << ",graph_full_scans=0"
        << ",contribution_full_scans=0"
        << ",timing_policy=frozen_limits"
        << '\n';

    return pass ? 0 : 1;
#else
    std::cout
        << "D3D_MILLION_FAST_BUILD_MEDIAN_GATE,UNAVAILABLE,"
        << "backend=0,platform=non_windows\n";
    return 3;
#endif
}


[[nodiscard]] int run_d4a_sparse_save_materialization_profile(
    std::size_t source_count,
    std::size_t io_worker_budget = 0) {

    if (source_count == 0)
        return 2;

    temporary_tree tree;
    std::filesystem::path configuration_path;
    std::vector<std::filesystem::path> unused_source_paths;

    const auto progress_interval =
        source_count >= 1'000'000
            ? std::size_t{100'000}
            : source_count >= 100'000
                ? std::size_t{10'000}
                : std::size_t{0};

    std::cerr
        << "D4A_SETUP_BEGIN,sources="
        << source_count
        << '\n';

    if (!prepare_project(
            source_count,
            tree,
            configuration_path,
            unused_source_paths,
            false,
            progress_interval)) {
        return 1;
    }

    baseline_commit_result baseline;
    if (!create_baseline(
            configuration_path,
            baseline,
            0)) {
        std::cout
            << "D4A_SPARSE_SAVE_MATERIALIZATION,FAIL,"
            << "stage=baseline,sources="
            << source_count
            << '\n';
        return 1;
    }

    const auto target = source_count / 2;
    const auto target_path =
        tree.path / source_name(target);
    const auto changed_text =
        "struct " + type_name(target) +
        " { int value; };\n";

    if (!write_text(
            target_path,
            changed_text)) {
        return 1;
    }

    project_manager manager;
    diagnostic_buffer diagnostics;
    project_build_result build;

    const auto build_status = manager.build(
        configuration_path,
        operation_id{4400},
        diagnostics,
        build,
        1);

    const bool build_pass =
        build_status.ok() &&
        !diagnostics.has_errors() &&
        manager.ready() &&
        validate_sparse_modify(
            source_count,
            build) &&
        build.telemetry.dirty_detection_backend == 1 &&
        build.telemetry.dirty_detection_fast &&
        !build.telemetry.dirty_detection_fallback &&
        build.telemetry.dirty_sources == 1 &&
        build.telemetry.journal_matched_sources == 1 &&
        // D4I2 stores Build Cache separately. A dirty BUILD maps it lazily
        // after Source dirty detection proves that construction is required.
        build.telemetry.baseline_build_cache_map_ns != 0 &&
        build.telemetry.generation_checkpoint_available &&
        build.telemetry.generation_anchor_available &&
        build.telemetry.generation_change_ready &&
        build.telemetry.generation_change_overlay &&
        build.telemetry.generation_change_fallback_reason == 0 &&
        build.telemetry.generation_change_file_updates == 1;

    if (!build_pass) {
        std::cout
            << "D4A_SPARSE_SAVE_MATERIALIZATION,FAIL,"
            << "stage=build,sources="
            << source_count
            << ",status_code="
            << static_cast<unsigned>(build_status.code)
            << ",diagnostic_errors="
            << (diagnostics.has_errors() ? 1 : 0)
            << ",manager_ready="
            << (manager.ready() ? 1 : 0)
            << ",changed="
            << (build.changed ? 1 : 0)
            << ",rebuilt="
            << (build.rebuilt ? 1 : 0)
            << ",baseline_sources="
            << build.telemetry.baseline_sources
            << ",frontend_dirty="
            << build.telemetry.frontend.dirty
            << ",frontend_changed="
            << build.telemetry.frontend.changed
            << ",frontend_affected="
            << build.telemetry.frontend.affected
            << ",frontend_acquired="
            << build.telemetry.frontend.acquired
            << ",frontend_lexed="
            << build.telemetry.frontend.lexed
            << ",frontend_parsed="
            << build.telemetry.frontend.parsed
            << ",builder_changed_sources="
            << build.telemetry.builder.changed_sources
            << ",builder_changed_types="
            << build.telemetry.builder.changed_types
            << ",graph_full_scans="
            << build.telemetry.builder.graph_full_scans
            << ",contribution_full_scans="
            << build.telemetry.builder.contribution_full_scans
            << ",backend="
            << build.telemetry.dirty_detection_backend
            << ",fast="
            << (build.telemetry.dirty_detection_fast ? 1 : 0)
            << ",fallback="
            << (build.telemetry.dirty_detection_fallback ? 1 : 0)
            << ",journal_matched="
            << build.telemetry.journal_matched_sources
            << ",build_cache_map_ms="
            << static_cast<double>(
                build.telemetry.baseline_build_cache_map_ns) /
                1'000'000.0
            << ",path_index_full_rebuilds="
            << build.telemetry.sources.path_index_full_rebuilds
            << ",source_graph_full_scans="
            << build.telemetry.sources.source_graph_full_scans
            << ",dirty_sources="
            << build.telemetry.dirty_sources
            << ",generation_checkpoint="
            << (build.telemetry.generation_checkpoint_available ? 1 : 0)
            << ",generation_anchor="
            << (build.telemetry.generation_anchor_available ? 1 : 0)
            << ",generation_change_ready="
            << (build.telemetry.generation_change_ready ? 1 : 0)
            << ",generation_change_overlay="
            << (build.telemetry.generation_change_overlay ? 1 : 0)
            << ",generation_change_fallback_reason="
            << build.telemetry.generation_change_fallback_reason
            << ",generation_change_file_updates="
            << build.telemetry.generation_change_file_updates
            << ",generation_change_directory_updates="
            << build.telemetry.generation_change_directory_updates
            << '\n';

        if (manager.ready())
            (void)manager.unload();

        return build.telemetry.dirty_detection_fast
            ? 1
            : 3;
    }

    baseline_commit_result save;

    const auto save_begin =
        lifecycle_clock::now();
    const auto save_status =
        manager.save(
            save,
            io_worker_budget);
    const auto save_end =
        lifecycle_clock::now();

    const auto& telemetry = save.telemetry;

    const auto ns_ms =
        [](std::uint64_t value) noexcept {
            return static_cast<double>(value) /
                1'000'000.0;
        };

    const auto phase_sum_ns =
        telemetry.generation_freeze_materialize_change_update_index_allocate_zero_ns +
        telemetry.generation_freeze_materialize_change_baseline_file_count_ns +
        telemetry.generation_freeze_materialize_change_file_index_allocate_zero_ns +
        telemetry.generation_freeze_materialize_change_baseline_file_merge_ns +
        telemetry.generation_freeze_materialize_change_sparse_file_updates_ns +
        telemetry.generation_freeze_materialize_change_baseline_directory_count_ns +
        telemetry.generation_freeze_materialize_change_directory_index_allocate_zero_ns +
        telemetry.generation_freeze_materialize_change_baseline_directory_merge_ns +
        telemetry.generation_freeze_materialize_change_sparse_directory_updates_ns;

    const auto materialize_unaccounted_ns =
        telemetry.generation_freeze_materialize_change_ns >
            phase_sum_ns
        ? telemetry.generation_freeze_materialize_change_ns -
            phase_sum_ns
        : 0;

    // D4O1C: for this deterministic dirty-one workload, every reused Build
    // Cache section must be explained by exactly one of two disjoint proofs:
    // direct baseline provenance or the existing exact-file compare path.
    // This guards against accidentally routing provenance sections back
    // through memcmp while preserving the non-proven exact proof path.
    const bool d4o1c_provenance_contract =
        telemetry.transaction_build_cache_sectioned == 1 &&
        telemetry.generation_freeze_build_cache_provenance_sections != 0 &&
        telemetry.generation_freeze_build_cache_provenance_bytes != 0 &&
        telemetry.transaction_build_cache_provenance_reused_sections ==
            telemetry.generation_freeze_build_cache_provenance_sections &&
        telemetry.transaction_build_cache_provenance_reused_bytes ==
            telemetry.generation_freeze_build_cache_provenance_bytes &&
        telemetry.transaction_build_cache_compare_sections != 0 &&
        telemetry.transaction_build_cache_compare_bytes != 0 &&
        telemetry.transaction_build_cache_reused_sections ==
            telemetry.transaction_build_cache_provenance_reused_sections +
            telemetry.transaction_build_cache_compare_sections &&
        telemetry.transaction_build_cache_reused_bytes ==
            telemetry.transaction_build_cache_provenance_reused_bytes +
            telemetry.transaction_build_cache_compare_bytes &&
        telemetry.transaction_build_cache_fallback_reason == 0 &&
        telemetry.transaction_build_cache_hard_link_fallback_sections == 0;

    const bool save_pass =
        save_status.ok() &&
        !save.transaction.empty() &&
        save.bytes_written != 0 &&
        telemetry.generation_freeze_materialize_change_ns != 0 &&
        telemetry.generation_freeze_materialize_change_source_count ==
            source_count &&
        telemetry.generation_freeze_materialize_change_file_updates == 1 &&
        telemetry.generation_freeze_source_manager_mode == 3 &&
        telemetry.generation_freeze_source_manager_allocate_zero_ns == 0 &&
        telemetry.generation_freeze_source_manager_source_records_ns == 0 &&
        telemetry.generation_freeze_source_manager_path_index_ns == 0 &&
        telemetry.generation_freeze_source_manager_verify_ns == 0 &&
        telemetry.generation_freeze_build_cache_mapped_baseline_bulk_bytes != 0 &&
        telemetry.generation_freeze_build_cache_mapped_baseline_bulk_sections >= 22 &&
        telemetry.generation_freeze_build_cache_provenance_bytes != 0 &&
        telemetry.generation_freeze_build_cache_provenance_sections != 0 &&
        telemetry.transaction_build_cache_provenance_reused_bytes ==
            telemetry.generation_freeze_build_cache_provenance_bytes &&
        telemetry.transaction_build_cache_provenance_reused_sections ==
            telemetry.generation_freeze_build_cache_provenance_sections &&
        d4o1c_provenance_contract;

    std::cout
        << "D4A_SPARSE_SAVE_MATERIALIZATION,"
        << (save_pass ? "PASS" : "FAIL")
        << ",sources=" << source_count
        << ",dirty_sources=" << build.telemetry.dirty_sources
        << ",generation_checkpoint="
        << (build.telemetry.generation_checkpoint_available ? 1 : 0)
        << ",generation_anchor="
        << (build.telemetry.generation_anchor_available ? 1 : 0)
        << ",generation_change_ready="
        << (build.telemetry.generation_change_ready ? 1 : 0)
        << ",generation_change_overlay="
        << (build.telemetry.generation_change_overlay ? 1 : 0)
        << ",generation_change_fallback_reason="
        << build.telemetry.generation_change_fallback_reason
        << ",generation_change_file_updates="
        << build.telemetry.generation_change_file_updates
        << ",generation_change_directory_updates="
        << build.telemetry.generation_change_directory_updates
        << ",save_wall_ms="
        << elapsed_ms(save_begin, save_end)
        << ",save_total_ms="
        << ns_ms(telemetry.save_total_ns)
        << ",freeze_ms="
        << ns_ms(telemetry.generation_freeze_ns)
        << ",materialize_total_ms="
        << ns_ms(
            telemetry.generation_freeze_materialize_change_ns)
        << ",materialize_accounted_ms="
        << ns_ms(phase_sum_ns)
        << ",materialize_unaccounted_ms="
        << ns_ms(materialize_unaccounted_ns)

        << ",update_index_allocate_zero_ms="
        << ns_ms(
            telemetry.generation_freeze_materialize_change_update_index_allocate_zero_ns)
        << ",baseline_file_count_ms="
        << ns_ms(
            telemetry.generation_freeze_materialize_change_baseline_file_count_ns)
        << ",file_index_allocate_zero_ms="
        << ns_ms(
            telemetry.generation_freeze_materialize_change_file_index_allocate_zero_ns)
        << ",baseline_file_merge_ms="
        << ns_ms(
            telemetry.generation_freeze_materialize_change_baseline_file_merge_ns)
        << ",sparse_file_updates_ms="
        << ns_ms(
            telemetry.generation_freeze_materialize_change_sparse_file_updates_ns)

        << ",baseline_directory_count_ms="
        << ns_ms(
            telemetry.generation_freeze_materialize_change_baseline_directory_count_ns)
        << ",directory_index_allocate_zero_ms="
        << ns_ms(
            telemetry.generation_freeze_materialize_change_directory_index_allocate_zero_ns)
        << ",baseline_directory_merge_ms="
        << ns_ms(
            telemetry.generation_freeze_materialize_change_baseline_directory_merge_ns)
        << ",sparse_directory_updates_ms="
        << ns_ms(
            telemetry.generation_freeze_materialize_change_sparse_directory_updates_ns)

        << ",baseline_file_capacity="
        << telemetry.generation_freeze_materialize_change_baseline_file_capacity
        << ",baseline_file_occupied="
        << telemetry.generation_freeze_materialize_change_baseline_file_occupied
        << ",baseline_directory_capacity="
        << telemetry.generation_freeze_materialize_change_baseline_directory_capacity
        << ",baseline_directory_occupied="
        << telemetry.generation_freeze_materialize_change_baseline_directory_occupied

        << ",file_updates="
        << telemetry.generation_freeze_materialize_change_file_updates
        << ",directory_updates="
        << telemetry.generation_freeze_materialize_change_directory_updates

        << ",update_index_bytes="
        << telemetry.generation_freeze_materialize_change_update_index_bytes
        << ",file_index_bytes="
        << telemetry.generation_freeze_materialize_change_file_index_bytes
        << ",directory_index_bytes="
        << telemetry.generation_freeze_materialize_change_directory_index_bytes
        << ",peak_temporary_bytes="
        << telemetry.generation_freeze_materialize_change_peak_temporary_bytes
        << ",peak_materialization_owned_bytes="
        << telemetry.generation_freeze_materialize_change_peak_owned_bytes

        // D4A2_FREEZE_BREAKDOWN: coarse accounting around complete freeze phases.
        << ",freeze_internal_ms="
        << ns_ms(
            telemetry.generation_freeze_internal_ns)
        << ",freeze_compiled_ms="
        << ns_ms(
            telemetry.generation_freeze_compiled_ns)
        << ",compiled_total_ms="
        << ns_ms(
            telemetry.generation_freeze_compiled_total_ns)
        << ",compiled_sizing_layout_ms="
        << ns_ms(
            telemetry.generation_freeze_compiled_sizing_layout_ns)
        << ",compiled_allocate_zero_ms="
        << ns_ms(
            telemetry.generation_freeze_compiled_allocate_zero_ns)
        << ",compiled_strings_ms="
        << ns_ms(
            telemetry.generation_freeze_compiled_strings_ns)
        << ",compiled_identities_ms="
        << ns_ms(
            telemetry.generation_freeze_compiled_identities_ns)
        << ",compiled_graph_arrays_ms="
        << ns_ms(
            telemetry.generation_freeze_compiled_graph_arrays_ns)
        << ",compiled_graph_indexes_ms="
        << ns_ms(
            telemetry.generation_freeze_compiled_graph_indexes_ns)
        << ",compiled_section_crc_ms="
        << ns_ms(
            telemetry.generation_freeze_compiled_section_crc_ns)
        << ",compiled_header_bind_ms="
        << ns_ms(
            telemetry.generation_freeze_compiled_header_bind_ns)
        << ",compiled_baseline_bulk_bytes="
        << telemetry.generation_freeze_compiled_baseline_bulk_bytes
        << ",compiled_baseline_bulk_sections="
        << telemetry.generation_freeze_compiled_baseline_bulk_sections
        << ",compiled_output_bytes="
        << telemetry.generation_freeze_compiled_output_bytes
        << ",freeze_roots_ms="
        << ns_ms(
            telemetry.generation_freeze_roots_ns)
        << ",freeze_source_manager_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_ns)
        << ",source_manager_mode="
        << telemetry.generation_freeze_source_manager_mode
        << ",source_manager_sparse_fallback_reason="
        << telemetry.generation_freeze_source_manager_sparse_fallback_reason
        << ",source_manager_internal_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_internal_ns)
        << ",source_manager_preflight_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_preflight_ns)
        << ",source_manager_layout_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_layout_ns)
        << ",source_manager_allocate_zero_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_allocate_zero_ns)
        << ",source_manager_source_records_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_source_records_ns)
        << ",source_manager_roots_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_roots_ns)
        << ",source_manager_path_index_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_path_index_ns)
        << ",source_manager_file_identity_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_file_identity_ns)
        << ",source_manager_directory_identity_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_directory_identity_ns)
        << ",source_manager_crc_wall_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_crc_wall_ns)
        << ",source_manager_crc_worker_count="
        << telemetry.generation_freeze_source_manager_crc_worker_count
        << ",source_manager_crc_total_bytes="
        << telemetry.generation_freeze_source_manager_crc_total_bytes
        << ",source_manager_crc_section_elapsed_sum_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_crc_source_core_ns +
            telemetry.generation_freeze_source_manager_crc_physical_state_ns +
            telemetry.generation_freeze_source_manager_crc_graph_records_ns +
            telemetry.generation_freeze_source_manager_crc_forward_edges_ns +
            telemetry.generation_freeze_source_manager_crc_reverse_edges_ns +
            telemetry.generation_freeze_source_manager_crc_roots_ns +
            telemetry.generation_freeze_source_manager_crc_path_index_ns +
            telemetry.generation_freeze_source_manager_crc_path_bytes_ns +
            telemetry.generation_freeze_source_manager_crc_file_identity_ns +
            telemetry.generation_freeze_source_manager_crc_directory_identity_ns)

        << ",sm_crc_source_core_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_crc_source_core_ns)
        << ",sm_crc_source_core_bytes="
        << telemetry.generation_freeze_source_manager_crc_source_core_bytes
        << ",sm_crc_physical_state_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_crc_physical_state_ns)
        << ",sm_crc_physical_state_bytes="
        << telemetry.generation_freeze_source_manager_crc_physical_state_bytes
        << ",sm_crc_graph_records_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_crc_graph_records_ns)
        << ",sm_crc_graph_records_bytes="
        << telemetry.generation_freeze_source_manager_crc_graph_records_bytes
        << ",sm_crc_forward_edges_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_crc_forward_edges_ns)
        << ",sm_crc_forward_edges_bytes="
        << telemetry.generation_freeze_source_manager_crc_forward_edges_bytes
        << ",sm_crc_reverse_edges_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_crc_reverse_edges_ns)
        << ",sm_crc_reverse_edges_bytes="
        << telemetry.generation_freeze_source_manager_crc_reverse_edges_bytes
        << ",sm_crc_roots_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_crc_roots_ns)
        << ",sm_crc_roots_bytes="
        << telemetry.generation_freeze_source_manager_crc_roots_bytes
        << ",sm_crc_path_index_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_crc_path_index_ns)
        << ",sm_crc_path_index_bytes="
        << telemetry.generation_freeze_source_manager_crc_path_index_bytes
        << ",sm_crc_path_bytes_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_crc_path_bytes_ns)
        << ",sm_crc_path_bytes_bytes="
        << telemetry.generation_freeze_source_manager_crc_path_bytes_bytes
        << ",sm_crc_file_identity_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_crc_file_identity_ns)
        << ",sm_crc_file_identity_bytes="
        << telemetry.generation_freeze_source_manager_crc_file_identity_bytes
        << ",sm_crc_directory_identity_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_crc_directory_identity_ns)
        << ",sm_crc_directory_identity_bytes="
        << telemetry.generation_freeze_source_manager_crc_directory_identity_bytes

        << ",source_manager_prefix_directory_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_prefix_directory_encode_ns)
        << ",source_manager_directory_crc_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_directory_crc_ns)
        << ",source_manager_header_crc_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_header_crc_ns)
        << ",source_manager_bind_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_bind_ns)
        << ",source_manager_verify_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_verify_ns)
        << ",source_manager_segment_validate_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_segment_validate_ns)
        << ",source_manager_identity_copy_ms="
        << ns_ms(
            telemetry.generation_freeze_source_manager_identity_copy_ns)
        << ",source_manager_extent_count="
        << telemetry.generation_freeze_source_manager_extent_count
        << ",source_manager_required_extent_count="
        << telemetry.
            generation_freeze_source_manager_sparse_required_extent_count
        << ",freeze_change_state_ms="
        << ns_ms(
            telemetry.generation_freeze_change_state_ns)
        << ",freeze_build_cache_ms="
        << ns_ms(
            telemetry.generation_freeze_build_cache_ns)
        << ",freeze_bind_ms="
        << ns_ms(
            telemetry.generation_freeze_bind_ns)
        << ",freeze_verify_change_state_ms="
        << ns_ms(
            telemetry.generation_freeze_verify_change_state_ns)
        << ",freeze_verify_build_cache_ms="
        << ns_ms(
            telemetry.generation_freeze_verify_build_cache_ns)

        << ",build_cache_layout_allocate_ms="
        << ns_ms(
            telemetry.generation_freeze_build_cache_layout_allocate_ns)
        << ",build_cache_source_frontend_ms="
        << ns_ms(
            telemetry.generation_freeze_build_cache_source_frontend_ns)
        << ",build_cache_source_directory_text_ms="
        << ns_ms(
            telemetry.generation_freeze_build_cache_source_directory_text_ns)
        << ",build_cache_frontend_record_ranges_ms="
        << ns_ms(
            telemetry.generation_freeze_build_cache_frontend_record_ranges_ns)
        << ",build_cache_frontend_local_types_ms="
        << ns_ms(
            telemetry.generation_freeze_build_cache_frontend_local_types_ns)
        << ",build_cache_frontend_type_slots_ms="
        << ns_ms(
            telemetry.generation_freeze_build_cache_frontend_type_slots_ns)
        << ",build_cache_frontend_object_slots_ms="
        << ns_ms(
            telemetry.generation_freeze_build_cache_frontend_object_slots_ns)
        << ",build_cache_frontend_member_slots_ms="
        << ns_ms(
            telemetry.generation_freeze_build_cache_frontend_member_slots_ns)
        << ",build_cache_contribution_ms="
        << ns_ms(
            telemetry.generation_freeze_build_cache_contribution_ns)
        << ",build_cache_graph_ms="
        << ns_ms(
            telemetry.generation_freeze_build_cache_graph_ns)
        << ",build_cache_change_identity_ms="
        << ns_ms(
            telemetry.generation_freeze_build_cache_change_identity_ns)
        << ",build_cache_section_crc_ms="
        << ns_ms(
            telemetry.generation_freeze_build_cache_section_crc_ns)
        << ",build_cache_header_directory_ms="
        << ns_ms(
            telemetry.generation_freeze_build_cache_header_directory_ns)
        << ",build_cache_bind_ms="
        << ns_ms(
            telemetry.generation_freeze_build_cache_bind_ns)
        << ",build_cache_verify_ms="
        << ns_ms(
            telemetry.generation_freeze_build_cache_verify_ns)
        << ",build_cache_mapped_bulk_bytes="
        << telemetry.generation_freeze_build_cache_mapped_baseline_bulk_bytes
        << ",build_cache_mapped_borrowed_bytes="
        << telemetry.generation_freeze_build_cache_mapped_baseline_borrowed_bytes
        << ",build_cache_mapped_borrowed_sections="
        << telemetry.generation_freeze_build_cache_mapped_baseline_borrowed_sections
        << ",build_cache_mapped_sparse_borrowed_bytes="
        << telemetry.generation_freeze_build_cache_mapped_baseline_sparse_borrowed_bytes
        << ",build_cache_mapped_sparse_borrowed_extents="
        << telemetry.generation_freeze_build_cache_mapped_baseline_sparse_borrowed_extents
        << ",build_cache_mapped_sparse_directory_borrowed_bytes="
        << telemetry.generation_freeze_build_cache_mapped_baseline_sparse_directory_borrowed_bytes
        << ",build_cache_mapped_sparse_directory_borrowed_extents="
        << telemetry.generation_freeze_build_cache_mapped_baseline_sparse_directory_borrowed_extents
        << ",build_cache_mapped_patch_records="
        << telemetry.generation_freeze_build_cache_mapped_baseline_patch_records
        << ",build_cache_mapped_append_records="
        << telemetry.generation_freeze_build_cache_mapped_baseline_append_records
        << ",build_cache_mapped_bulk_sections="
        << telemetry.generation_freeze_build_cache_mapped_baseline_bulk_sections
        << ",build_cache_provenance_bytes="
        << telemetry.generation_freeze_build_cache_provenance_bytes
        << ",build_cache_provenance_sections="
        << telemetry.generation_freeze_build_cache_provenance_sections

        << ",freeze_accounted_ms="
        << ns_ms(
            telemetry.generation_freeze_materialize_change_ns +
            telemetry.generation_freeze_compiled_ns +
            telemetry.generation_freeze_roots_ns +
            telemetry.generation_freeze_source_manager_ns +
            telemetry.generation_freeze_change_state_ns +
            telemetry.generation_freeze_build_cache_ns +
            telemetry.generation_freeze_bind_ns +
            telemetry.generation_freeze_verify_change_state_ns +
            telemetry.generation_freeze_verify_build_cache_ns)
        << ",freeze_unaccounted_ms="
        << ns_ms(
            telemetry.generation_freeze_internal_ns >
                telemetry.generation_freeze_materialize_change_ns +
                telemetry.generation_freeze_compiled_ns +
                telemetry.generation_freeze_roots_ns +
                telemetry.generation_freeze_source_manager_ns +
                telemetry.generation_freeze_change_state_ns +
                telemetry.generation_freeze_build_cache_ns +
                telemetry.generation_freeze_bind_ns +
                telemetry.generation_freeze_verify_change_state_ns +
                telemetry.generation_freeze_verify_build_cache_ns
            ? telemetry.generation_freeze_internal_ns -
                (telemetry.generation_freeze_materialize_change_ns +
                 telemetry.generation_freeze_compiled_ns +
                 telemetry.generation_freeze_roots_ns +
                 telemetry.generation_freeze_source_manager_ns +
                 telemetry.generation_freeze_change_state_ns +
                 telemetry.generation_freeze_build_cache_ns +
                 telemetry.generation_freeze_bind_ns +
                 telemetry.generation_freeze_verify_change_state_ns +
                 telemetry.generation_freeze_verify_build_cache_ns)
            : 0)

        << ",persisted_bytes_written="
        << save.bytes_written
        << ",store_commit_ms="
        << ns_ms(telemetry.store_commit_ns)
        << ",transaction_write_ms="
        << ns_ms(telemetry.transaction_write_ns)
        << ",transaction_flush_ms="
        << ns_ms(telemetry.transaction_flush_ns)
        << ",transaction_io_wall_ms="
        << ns_ms(telemetry.transaction_io_wall_ns)
        << ",transaction_io_workers="
        << telemetry.transaction_io_worker_count
        << ",transaction_io_budget_requested="
        << io_worker_budget
        << ",transaction_io_budget="
        << telemetry.transaction_io_budget
        << ",transaction_io_peak_active="
        << telemetry.transaction_io_peak_active
        << ",transaction_io_budget_wait_ms="
        << ns_ms(
            telemetry.transaction_io_budget_wait_ns)
        << ",tx_compiled_write_ms="
        << ns_ms(telemetry.transaction_compiled_write_ns)
        << ",tx_compiled_flush_ms="
        << ns_ms(telemetry.transaction_compiled_flush_ns)
        << ",tx_build_state_write_ms="
        << ns_ms(telemetry.transaction_build_state_write_ns)
        << ",tx_build_state_flush_ms="
        << ns_ms(telemetry.transaction_build_state_flush_ns)
        << ",tx_source_manager_write_ms="
        << ns_ms(telemetry.transaction_source_manager_write_ns)
        << ",tx_source_manager_flush_ms="
        << ns_ms(telemetry.transaction_source_manager_flush_ns)
        << ",tx_source_manager_sectioned="
        << telemetry.transaction_source_manager_sectioned
        << ",tx_source_manager_link_ms="
        << ns_ms(telemetry.transaction_source_manager_link_ns)
        << ",tx_source_manager_compare_ms="
        << ns_ms(telemetry.transaction_source_manager_compare_ns)
        << ",tx_source_manager_compare_bytes="
        << telemetry.transaction_source_manager_compare_bytes
        << ",tx_source_manager_compare_sections="
        << telemetry.transaction_source_manager_compare_sections
        << ",tx_source_manager_provenance_reused_bytes="
        << telemetry.transaction_source_manager_provenance_reused_bytes
        << ",tx_source_manager_provenance_reused_sections="
        << telemetry.transaction_source_manager_provenance_reused_sections
        << ",tx_source_manager_fallback_reason="
        << telemetry.transaction_source_manager_fallback_reason
        << ",tx_source_manager_failed_attempt_ms="
        << ns_ms(telemetry.transaction_source_manager_failed_attempt_ns)
        << ",tx_source_manager_hard_link_fallback_sections="
        << telemetry.transaction_source_manager_hard_link_fallback_sections
        << ",tx_source_manager_io_wall_ms="
        << ns_ms(telemetry.transaction_source_manager_io_wall_ns)
        << ",tx_source_manager_io_workers="
        << telemetry.transaction_source_manager_io_worker_count
        << ",tx_source_manager_directory_flush_ms="
        << ns_ms(
            telemetry.transaction_source_manager_directory_flush_ns)
        << ",tx_source_manager_written_bytes="
        << telemetry.transaction_source_manager_written_bytes
        << ",tx_source_manager_reused_bytes="
        << telemetry.transaction_source_manager_reused_bytes
        << ",tx_source_manager_written_sections="
        << telemetry.transaction_source_manager_written_sections
        << ",tx_source_manager_reused_sections="
        << telemetry.transaction_source_manager_reused_sections
        << ",tx_build_cache_write_ms="
        << ns_ms(telemetry.transaction_build_cache_write_ns)
        << ",tx_build_cache_flush_ms="
        << ns_ms(telemetry.transaction_build_cache_flush_ns)
        << ",tx_build_cache_sectioned="
        << telemetry.transaction_build_cache_sectioned
        << ",tx_build_cache_link_ms="
        << ns_ms(telemetry.transaction_build_cache_link_ns)
        << ",tx_build_cache_compare_ms="
        << ns_ms(telemetry.transaction_build_cache_compare_ns)
        << ",tx_build_cache_compare_bytes="
        << telemetry.transaction_build_cache_compare_bytes
        << ",tx_build_cache_compare_sections="
        << telemetry.transaction_build_cache_compare_sections
        << ",tx_build_cache_provenance_reused_bytes="
        << telemetry.transaction_build_cache_provenance_reused_bytes
        << ",tx_build_cache_provenance_reused_sections="
        << telemetry.transaction_build_cache_provenance_reused_sections
        << ",tx_build_cache_fallback_reason="
        << telemetry.transaction_build_cache_fallback_reason
        << ",tx_build_cache_failed_attempt_ms="
        << ns_ms(telemetry.transaction_build_cache_failed_attempt_ns)
        << ",tx_build_cache_hard_link_fallback_sections="
        << telemetry.transaction_build_cache_hard_link_fallback_sections
        << ",tx_build_cache_io_wall_ms="
        << ns_ms(telemetry.transaction_build_cache_io_wall_ns)
        << ",tx_build_cache_io_workers="
        << telemetry.transaction_build_cache_io_worker_count
        << ",tx_build_cache_directory_flush_ms="
        << ns_ms(
            telemetry.transaction_build_cache_directory_flush_ns)
        << ",tx_build_cache_written_bytes="
        << telemetry.transaction_build_cache_written_bytes
        << ",tx_build_cache_reused_bytes="
        << telemetry.transaction_build_cache_reused_bytes
        << ",tx_build_cache_written_sections="
        << telemetry.transaction_build_cache_written_sections
        << ",tx_build_cache_reused_sections="
        << telemetry.transaction_build_cache_reused_sections
        << ",tx_change_state_write_ms="
        << ns_ms(telemetry.transaction_change_state_write_ns)
        << ",tx_change_state_flush_ms="
        << ns_ms(telemetry.transaction_change_state_flush_ns)
        << ",tx_manifest_write_ms="
        << ns_ms(telemetry.transaction_manifest_write_ns)
        << ",tx_manifest_flush_ms="
        << ns_ms(telemetry.transaction_manifest_flush_ns)
        << ",directory_flush_ms="
        << ns_ms(telemetry.directory_flush_ns)
        << ",current_write_ms="
        << ns_ms(telemetry.current_write_ns)
        << ",current_flush_ms="
        << ns_ms(telemetry.current_flush_ns)
        << ",current_replace_ms="
        << ns_ms(telemetry.current_replace_ns)
        << '\n';

    if (!save_pass) {
        if (manager.ready())
            (void)manager.unload();
        return 1;
    }

    if (!manager.ready() ||
        !manager.unload().ok()) {
        return 1;
    }

    project_manager post_save_manager;
    diagnostic_buffer post_save_diagnostics;
    project_build_result post_save_build;

    const auto post_save_status =
        post_save_manager.build(
            configuration_path,
            operation_id{4401},
            post_save_diagnostics,
            post_save_build,
            1);

    const bool post_save_pass =
        post_save_status.ok() &&
        !post_save_diagnostics.has_errors() &&
        post_save_manager.ready() &&
        validate_no_change(
            source_count,
            post_save_build) &&
        post_save_build.telemetry.dirty_detection_backend == 1 &&
        post_save_build.telemetry.dirty_detection_fast &&
        !post_save_build.telemetry.dirty_detection_fallback &&
        post_save_build.telemetry.dirty_sources == 0 &&
        post_save_build.telemetry.journal_matched_sources == 0 &&
        post_save_build.telemetry.baseline_source_manager_map_ns == 0 &&
        post_save_build.telemetry.baseline_build_cache_map_ns == 0 &&
        post_save_build.telemetry.sources.path_index_full_rebuilds == 0 &&
        post_save_build.telemetry.sources.source_graph_full_scans == 0 &&
        post_save_build.telemetry.builder.graph_full_scans == 0 &&
        post_save_build.telemetry.builder.contribution_full_scans == 0;

    std::cout
        << "D4B_POST_SAVE_FAST_BUILD,"
        << (post_save_pass ? "PASS" : "FAIL")
        << ",sources=" << source_count
        << ",manager_ms="
        << ns_ms(post_save_build.telemetry.manager_total_ns)
        << ",baseline_open_ms="
        << ns_ms(post_save_build.telemetry.baseline_open_ns)
        << ",dirty_ms="
        << ns_ms(post_save_build.telemetry.dirty_detection_ns)
        << ",backend="
        << post_save_build.telemetry.dirty_detection_backend
        << ",fast="
        << (post_save_build.telemetry.dirty_detection_fast ? 1 : 0)
        << ",fallback="
        << (post_save_build.telemetry.dirty_detection_fallback ? 1 : 0)
        << ",journal_records="
        << post_save_build.telemetry.journal_records
        << ",journal_matched="
        << post_save_build.telemetry.journal_matched_sources
        << ",dirty_sources="
        << post_save_build.telemetry.dirty_sources
        << ",source_manager_map_ms="
        << ns_ms(
            post_save_build.telemetry.baseline_source_manager_map_ns)
        << ",build_cache_map_ms="
        << ns_ms(
            post_save_build.telemetry.baseline_build_cache_map_ns)
        << ",graph_full_scans="
        << post_save_build.telemetry.builder.graph_full_scans
        << ",contribution_full_scans="
        << post_save_build.telemetry.builder.contribution_full_scans
        << '\n';

    if (post_save_manager.ready() &&
        !post_save_manager.unload().ok()) {
        return 1;
    }

    if (!post_save_pass)
        return 1;

    // D4L3A: hard-linked immutable sections must remain valid after the
    // transaction that originally owned the linked directory entries is
    // reclaimed. Verify both READY LOAD and no-change BUILD afterwards.
    baseline_store lifecycle_store{
        configuration_path};

    baseline_probe current_probe;
    const auto probe_status =
        lifecycle_store.probe(
            current_probe);

    const bool current_before_gc =
        probe_status.ok() &&
        current_probe.transaction ==
            save.transaction &&
        baseline.transaction !=
            save.transaction;

    const auto gc_status =
        current_before_gc
            ? lifecycle_store.collect_garbage()
            : status{
                status_code::invalid_state};

    baseline_snapshot retired_snapshot;
    const auto retired_status =
        gc_status.ok()
            ? lifecycle_store.open_transaction(
                current_probe.fingerprint,
                baseline.transaction,
                retired_snapshot)
            : status{
                status_code::invalid_state};

    const bool retired_removed =
        gc_status.ok() &&
        !retired_status.ok();

    project_manager lifecycle_manager;
    diagnostic_buffer lifecycle_diagnostics;
    project_load_result lifecycle_load;

    const auto load_status =
        retired_removed
            ? lifecycle_manager.load(
                configuration_path,
                operation_id{4402},
                lifecycle_diagnostics,
                lifecycle_load)
            : status{
                status_code::invalid_state};

    std::size_t load_source_count = 0;
    bool load_transaction_current = false;

    if (load_status.ok() &&
        !lifecycle_diagnostics.has_errors() &&
        lifecycle_manager.ready()) {

        project_access access;
        const auto acquire_status =
            lifecycle_manager.acquire(access);

        if (acquire_status.ok() &&
            access) {
            load_source_count =
                access->sources().source_count();
            load_transaction_current =
                access->baseline_transaction() ==
                    save.transaction;
        }
    }

    const bool load_pass =
        load_status.ok() &&
        !lifecycle_diagnostics.has_errors() &&
        lifecycle_manager.ready() &&
        !lifecycle_load.build_cache_mapped &&
        load_source_count == source_count &&
        load_transaction_current;

    const auto unload_after_load_status =
        lifecycle_manager.ready()
            ? lifecycle_manager.unload()
            : status{
                status_code::invalid_state};

    lifecycle_diagnostics.clear();
    project_build_result lifecycle_build;

    const auto build_status_after_gc =
        load_pass &&
        unload_after_load_status.ok()
            ? lifecycle_manager.build(
                configuration_path,
                operation_id{4403},
                lifecycle_diagnostics,
                lifecycle_build,
                1)
            : status{
                status_code::invalid_state};

    std::size_t build_source_count = 0;
    bool build_transaction_current = false;

    if (build_status_after_gc.ok() &&
        !lifecycle_diagnostics.has_errors() &&
        lifecycle_manager.ready()) {

        project_access access;
        const auto acquire_status =
            lifecycle_manager.acquire(access);

        if (acquire_status.ok() &&
            access) {
            build_source_count =
                access->sources().source_count();
            build_transaction_current =
                access->baseline_transaction() ==
                    save.transaction;
        }
    }

    const bool build_after_gc_pass =
        build_status_after_gc.ok() &&
        !lifecycle_diagnostics.has_errors() &&
        lifecycle_manager.ready() &&
        validate_no_change(
            source_count,
            lifecycle_build) &&
        build_source_count == source_count &&
        build_transaction_current &&
        lifecycle_build.telemetry.dirty_sources == 0 &&
        lifecycle_build.telemetry.builder.graph_full_scans == 0 &&
        lifecycle_build.telemetry.builder.contribution_full_scans == 0;

    const auto final_unload_status =
        lifecycle_manager.ready()
            ? lifecycle_manager.unload()
            : status{
                status_code::invalid_state};

    // D4N2: after GC removed the transaction that originally owned reused
    // section directory entries, force a dirty BUILD from the surviving
    // persisted baseline. This must map and consume the persisted Build Cache.
    const auto d4n2_source_path =
        tree.path / source_name(0);
    const auto d4n2_source_text =
        "struct " + type_name(0) +
        " { int d4n2_value; };\n";

    const bool d4n2_write_ok =
        final_unload_status.ok() &&
        write_text(
            d4n2_source_path,
            d4n2_source_text);

    lifecycle_diagnostics.clear();
    project_build_result lifecycle_dirty_build;

    const auto dirty_build_status_after_gc =
        d4n2_write_ok
            ? lifecycle_manager.build(
                configuration_path,
                operation_id{4404},
                lifecycle_diagnostics,
                lifecycle_dirty_build,
                1)
            : status{
                status_code::invalid_state};

    std::size_t dirty_build_source_count = 0;
    bool dirty_build_transaction_current = false;

    if (dirty_build_status_after_gc.ok() &&
        !lifecycle_diagnostics.has_errors() &&
        lifecycle_manager.ready()) {

        project_access access;
        const auto acquire_status =
            lifecycle_manager.acquire(access);

        if (acquire_status.ok() &&
            access) {
            dirty_build_source_count =
                access->sources().source_count();
            dirty_build_transaction_current =
                access->baseline_transaction() ==
                    save.transaction;
        }
    }

    const bool dirty_build_cache_mapped =
        lifecycle_dirty_build.telemetry
                .baseline_build_cache_map_ns != 0;

    const bool d4n2_build_pass =
        dirty_build_status_after_gc.ok() &&
        !lifecycle_diagnostics.has_errors() &&
        lifecycle_manager.ready() &&
        validate_sparse_modify(
            source_count,
            lifecycle_dirty_build) &&
        dirty_build_cache_mapped &&
        dirty_build_source_count == source_count &&
        dirty_build_transaction_current &&
        lifecycle_dirty_build.telemetry.dirty_sources == 1 &&
        lifecycle_dirty_build.telemetry.builder.graph_full_scans == 0 &&
        lifecycle_dirty_build.telemetry.builder.contribution_full_scans == 0;

    const auto dirty_build_unload_status =
        lifecycle_manager.ready()
            ? lifecycle_manager.unload()
            : status{
                status_code::invalid_state};

    const bool d4n2_pass =
        d4n2_write_ok &&
        d4n2_build_pass &&
        dirty_build_unload_status.ok();

    const bool lifecycle_pass =
        current_before_gc &&
        gc_status.ok() &&
        retired_removed &&
        load_pass &&
        unload_after_load_status.ok() &&
        build_after_gc_pass &&
        final_unload_status.ok();

    const bool d4o1c_pass =
        d4o1c_provenance_contract &&
        lifecycle_pass &&
        d4n2_pass;

    const auto section_mask_count =
        [](std::uint32_t mask) noexcept {
            std::uint32_t count = 0;
            while (mask != 0) {
                count += mask & 1u;
                mask >>= 1u;
            }
            return count;
        };

    constexpr std::uint32_t build_cache_section_mask =
        (std::uint32_t{1} <<
            build_cache_image_directory_count) -
        std::uint32_t{1};

    const auto provenance_mask =
        telemetry.transaction_build_cache_provenance_reused_mask;
    const auto compare_attempt_mask =
        telemetry.transaction_build_cache_compare_attempt_mask;
    const auto compare_reused_mask =
        telemetry.transaction_build_cache_compare_reused_mask;

    const bool d4o2a_mask_contract =
        (provenance_mask & ~build_cache_section_mask) == 0 &&
        (compare_attempt_mask & ~build_cache_section_mask) == 0 &&
        (compare_reused_mask & ~build_cache_section_mask) == 0 &&
        (provenance_mask & compare_attempt_mask) == 0 &&
        (compare_reused_mask & ~compare_attempt_mask) == 0 &&
        section_mask_count(provenance_mask) ==
            telemetry.transaction_build_cache_provenance_reused_sections &&
        section_mask_count(compare_attempt_mask) ==
            telemetry.transaction_build_cache_compare_sections &&
        section_mask_count(compare_reused_mask) ==
            telemetry.transaction_build_cache_reused_sections -
            telemetry.transaction_build_cache_provenance_reused_sections &&
        compare_reused_mask != 0;

    const bool d4o2a_pass =
        d4o1c_pass &&
        d4o2a_mask_contract;

    const auto build_cache_section_bit =
        [](build_cache_image_section section) noexcept {
            const auto raw =
                static_cast<std::uint32_t>(section);
            return raw >= 1 &&
                    raw <= build_cache_image_directory_count
                ? std::uint32_t{1} << (raw - 1)
                : std::uint32_t{0};
        };

    // D4O2D freezes the architectural result, not an optimizer-specific
    // implementation detail. These sections must be encoder-proven exact for
    // the deterministic dirty-one workload. Additional future provenance is
    // allowed. The residual exact-compare path must remain exercised so the
    // generic durable fallback continues to have regression coverage.
    const std::uint32_t d4o2d_required_provenance_mask =
        build_cache_section_bit(
            build_cache_image_section::frontend_local_types) |
        build_cache_section_bit(
            build_cache_image_section::frontend_type_slots) |
        build_cache_section_bit(
            build_cache_image_section::graph_named_refs) |
        build_cache_section_bit(
            build_cache_image_section::graph_derived_index) |
        build_cache_section_bit(
            build_cache_image_section::
                graph_reverse_dependency_heads) |
        build_cache_section_bit(
            build_cache_image_section::
                graph_type_identity_index) |
        build_cache_section_bit(
            build_cache_image_section::
                graph_object_identity_index) |
        build_cache_section_bit(
            build_cache_image_section::
                graph_link_target_index);

    const bool d4o2d_identity_contract =
        (provenance_mask &
            d4o2d_required_provenance_mask) ==
                d4o2d_required_provenance_mask &&
        (provenance_mask & compare_attempt_mask) == 0 &&
        compare_reused_mask != 0 &&
        telemetry.transaction_build_cache_compare_bytes != 0 &&
        telemetry.transaction_build_cache_fallback_reason == 0 &&
        telemetry.transaction_build_cache_hard_link_fallback_sections == 0;

    const bool d4o2d_pass =
        d4o2a_pass &&
        d4o2d_identity_contract &&
        lifecycle_pass &&
        d4n2_pass;

    const bool d4p1_pass =
        d4o2d_pass &&
        telemetry.
            transaction_build_cache_provenance_binding_rejected_sections == 0 &&
        telemetry.transaction_build_cache_provenance_reused_sections ==
            telemetry.generation_freeze_build_cache_provenance_sections &&
        telemetry.transaction_build_cache_provenance_reused_bytes ==
            telemetry.generation_freeze_build_cache_provenance_bytes;

    const auto none_origin =
        static_cast<std::uint32_t>(
            project_generation_persistence_origin::none);
    const auto maximum_origin =
        static_cast<std::uint32_t>(
            project_generation_persistence_origin::
                mixed_baseline);

    const auto valid_origin =
        [none_origin, maximum_origin](
            std::uint32_t value) noexcept {
            return
                value > none_origin &&
                value <= maximum_origin;
        };

    // D4Q1 is observational. The gate validates that ownership is completely
    // classified and internally consistent; it must not freeze today's
    // implementation choices, because D4Q2/D4Q3 are expected to change those
    // origins from reconstructed to native/borrowed representations.
    const bool d4q1_pass =
        d4p1_pass &&
        telemetry.generation_freeze_audit_staging_ns != 0 &&
        telemetry.generation_freeze_audit_validation_ns != 0 &&
        telemetry.generation_freeze_audit_compiled_bytes != 0 &&
        telemetry.generation_freeze_audit_source_manager_bytes != 0 &&
        telemetry.generation_freeze_audit_change_state_bytes != 0 &&
        telemetry.generation_freeze_audit_build_cache_bytes != 0 &&
        valid_origin(
            telemetry.generation_freeze_audit_compiled_origin) &&
        valid_origin(
            telemetry.generation_freeze_audit_source_manager_origin) &&
        valid_origin(
            telemetry.generation_freeze_audit_change_state_origin) &&
        valid_origin(
            telemetry.generation_freeze_audit_build_cache_origin) &&
        telemetry.
            generation_freeze_audit_source_manager_baseline_direct_borrow_bytes !=
                0 &&
        telemetry.
            generation_freeze_audit_source_manager_baseline_direct_borrow_sections !=
                0 &&
        telemetry.generation_freeze_audit_build_cache_baseline_exact_bytes ==
            telemetry.generation_freeze_build_cache_provenance_bytes &&
        telemetry.generation_freeze_audit_build_cache_baseline_exact_sections ==
            telemetry.generation_freeze_build_cache_provenance_sections;

    const auto build_cache_bulk_bytes =
        telemetry.
            generation_freeze_build_cache_mapped_baseline_bulk_bytes;
    const auto build_cache_borrowed_bytes =
        telemetry.
            generation_freeze_build_cache_mapped_baseline_borrowed_bytes;
    const auto build_cache_sparse_borrowed_bytes =
        telemetry.
            generation_freeze_build_cache_mapped_baseline_sparse_borrowed_bytes;

    const auto build_cache_sparse_directory_borrowed_bytes =
        telemetry.
            generation_freeze_build_cache_mapped_baseline_sparse_directory_borrowed_bytes;

    const auto build_cache_sparse_directory_borrowed_extents =
        telemetry.
            generation_freeze_build_cache_mapped_baseline_sparse_directory_borrowed_extents;

    const auto build_cache_total_borrowed_bytes =
        build_cache_borrowed_bytes +
        build_cache_sparse_borrowed_bytes;

    const auto build_cache_copied_bytes =
        build_cache_bulk_bytes >=
            build_cache_total_borrowed_bytes
        ? build_cache_bulk_bytes -
            build_cache_total_borrowed_bytes
        : std::uint64_t{0};

    const bool d4q2b_pass =
        d4q1_pass &&
        build_cache_bulk_bytes != 0 &&
        build_cache_borrowed_bytes != 0 &&
        telemetry.
            generation_freeze_build_cache_mapped_baseline_borrowed_sections !=
                0 &&
        build_cache_borrowed_bytes <
            build_cache_bulk_bytes &&
        build_cache_copied_bytes <
            build_cache_bulk_bytes &&
        telemetry.
            transaction_build_cache_fallback_reason == 0 &&
        telemetry.
            transaction_build_cache_provenance_binding_rejected_sections == 0;

    const bool d4q2c1_pass =
        d4q2b_pass &&
        build_cache_borrowed_bytes ==
            telemetry.
                generation_freeze_build_cache_provenance_bytes &&
        telemetry.
            generation_freeze_build_cache_mapped_baseline_borrowed_sections ==
                telemetry.
                    generation_freeze_build_cache_provenance_sections;

    const bool d4q2c2a_pass =
        d4q2c1_pass &&
        build_cache_sparse_borrowed_bytes != 0 &&
        telemetry.
            generation_freeze_build_cache_mapped_baseline_sparse_borrowed_extents !=
                0 &&
        build_cache_total_borrowed_bytes <=
            build_cache_bulk_bytes &&
        telemetry.
            transaction_build_cache_fallback_reason == 0 &&
        telemetry.
            transaction_build_cache_provenance_binding_rejected_sections == 0;

    const bool d4q2c2b_pass =
        d4q2c2a_pass &&
        build_cache_sparse_directory_borrowed_bytes != 0 &&
        build_cache_sparse_directory_borrowed_extents != 0 &&
        build_cache_sparse_directory_borrowed_bytes <=
            build_cache_sparse_borrowed_bytes &&
        telemetry.
            transaction_build_cache_fallback_reason == 0 &&
        telemetry.
            transaction_build_cache_provenance_binding_rejected_sections == 0;






    std::cout
        << "D4L3A_SECTIONED_LIFECYCLE_GC,"
        << (lifecycle_pass ? "PASS" : "FAIL")
        << ",sources=" << source_count
        << ",baseline_tx="
        << baseline.transaction
        << ",current_tx="
        << save.transaction
        << ",probe_status="
        << static_cast<unsigned>(
            probe_status.code)
        << ",gc_status="
        << static_cast<unsigned>(
            gc_status.code)
        << ",retired_open_status="
        << static_cast<unsigned>(
            retired_status.code)
        << ",retired_removed="
        << (retired_removed ? 1 : 0)
        << ",load_status="
        << static_cast<unsigned>(
            load_status.code)
        << ",load_build_cache_mapped="
        << (lifecycle_load.build_cache_mapped ? 1 : 0)
        << ",load_sources="
        << load_source_count
        << ",load_current_tx="
        << (load_transaction_current ? 1 : 0)
        << ",build_status="
        << static_cast<unsigned>(
            build_status_after_gc.code)
        << ",build_dirty_sources="
        << lifecycle_build.telemetry.dirty_sources
        << ",build_sources="
        << build_source_count
        << ",build_current_tx="
        << (build_transaction_current ? 1 : 0)
        << ",graph_full_scans="
        << lifecycle_build.telemetry.builder.graph_full_scans
        << ",contribution_full_scans="
        << lifecycle_build.telemetry.builder.contribution_full_scans
        << '\n';

    std::cout
        << "D4N2_BUILD_CACHE_AFTER_GC,"
        << (d4n2_pass ? "PASS" : "FAIL")
        << ",sources=" << source_count
        << ",write_ok="
        << (d4n2_write_ok ? 1 : 0)
        << ",build_status="
        << static_cast<unsigned>(
            dirty_build_status_after_gc.code)
        << ",dirty_sources="
        << lifecycle_dirty_build.telemetry.dirty_sources
        << ",source_manager_map_ms="
        << ns_ms(
            lifecycle_dirty_build.telemetry
                .baseline_source_manager_map_ns)
        << ",build_cache_map_ms="
        << ns_ms(
            lifecycle_dirty_build.telemetry
                .baseline_build_cache_map_ns)
        << ",build_cache_mapped="
        << (dirty_build_cache_mapped ? 1 : 0)
        << ",build_sources="
        << dirty_build_source_count
        << ",build_current_tx="
        << (dirty_build_transaction_current ? 1 : 0)
        << ",graph_full_scans="
        << lifecycle_dirty_build.telemetry
                .builder.graph_full_scans
        << ",contribution_full_scans="
        << lifecycle_dirty_build.telemetry
                .builder.contribution_full_scans
        << '\n';

    std::cout
        << "D4O1C_BUILD_CACHE_PROVENANCE_GATE,"
        << (d4o1c_pass ? "PASS" : "FAIL")
        << ",sources=" << source_count
        << ",freeze_provenance_sections="
        << telemetry.generation_freeze_build_cache_provenance_sections
        << ",commit_provenance_sections="
        << telemetry.transaction_build_cache_provenance_reused_sections
        << ",compare_sections="
        << telemetry.transaction_build_cache_compare_sections
        << ",reused_sections="
        << telemetry.transaction_build_cache_reused_sections
        << ",freeze_provenance_bytes="
        << telemetry.generation_freeze_build_cache_provenance_bytes
        << ",commit_provenance_bytes="
        << telemetry.transaction_build_cache_provenance_reused_bytes
        << ",compare_bytes="
        << telemetry.transaction_build_cache_compare_bytes
        << ",reused_bytes="
        << telemetry.transaction_build_cache_reused_bytes
        << ",fallback_reason="
        << telemetry.transaction_build_cache_fallback_reason
        << ",hard_link_fallback_sections="
        << telemetry.transaction_build_cache_hard_link_fallback_sections
        << ",retired_removed="
        << (retired_removed ? 1 : 0)
        << ",dirty_build_cache_mapped="
        << (dirty_build_cache_mapped ? 1 : 0)
        << ",lifecycle_pass="
        << (lifecycle_pass ? 1 : 0)
        << ",dirty_after_gc_pass="
        << (d4n2_pass ? 1 : 0)
        << '\n';

    constexpr std::array<std::string_view, 25>
        d4o2a_section_names{
            "source_directory",
            "source_bytes",
            "frontend_local_types",
            "frontend_type_slots",
            "frontend_object_slots",
            "frontend_member_slots",
            "contribution_states",
            "contribution_types",
            "contribution_members",
            "contribution_modifiers",
            "contribution_enum_values",
            "contribution_objects",
            "contribution_links",
            "construction_states",
            "graph_intrinsic_refs",
            "graph_named_refs",
            "graph_derived_index",
            "graph_dependency_versions",
            "graph_reverse_dependency_heads",
            "graph_dependency_edges",
            "graph_type_identity_index",
            "graph_object_identity_index",
            "graph_link_target_index",
            "source_file_identity_index",
            "tracked_directory_identity_index",
        };

    std::cout
        << "D4O2A_BUILD_CACHE_SECTION_AUDIT,"
        << (d4o2a_pass ? "PASS" : "FAIL")
        << ",sources=" << source_count
        << ",provenance_mask="
        << provenance_mask
        << ",compare_attempt_mask="
        << compare_attempt_mask
        << ",compare_reused_mask="
        << compare_reused_mask
        << ",provenance_sections="
        << telemetry.transaction_build_cache_provenance_reused_sections
        << ",compare_sections="
        << telemetry.transaction_build_cache_compare_sections
        << ",reused_sections="
        << telemetry.transaction_build_cache_reused_sections
        << ",provenance_bytes="
        << telemetry.transaction_build_cache_provenance_reused_bytes
        << ",compare_bytes="
        << telemetry.transaction_build_cache_compare_bytes
        << ",reused_bytes="
        << telemetry.transaction_build_cache_reused_bytes
        << '\n';

    for (std::size_t index = 0;
         index < d4o2a_section_names.size();
         ++index) {

        const auto bit =
            std::uint32_t{1} << index;

        if ((provenance_mask |
             compare_attempt_mask |
             compare_reused_mask) & bit) {

            std::cout
                << "D4O2A_BUILD_CACHE_SECTION,"
                << "index=" << (index + 1)
                << ",name="
                << d4o2a_section_names[index]
                << ",provenance="
                << ((provenance_mask & bit) != 0 ? 1 : 0)
                << ",compare_attempt="
                << ((compare_attempt_mask & bit) != 0 ? 1 : 0)
                << ",compare_reused="
                << ((compare_reused_mask & bit) != 0 ? 1 : 0)
                << '\n';
        }
    }

    std::cout
        << "D4O2D_BUILD_CACHE_IDENTITY_CONTRACT,"
        << (d4o2d_pass ? "PASS" : "FAIL")
        << ",sources=" << source_count
        << ",required_provenance_mask="
        << d4o2d_required_provenance_mask
        << ",actual_provenance_mask="
        << provenance_mask
        << ",compare_attempt_mask="
        << compare_attempt_mask
        << ",compare_reused_mask="
        << compare_reused_mask
        << ",provenance_sections="
        << telemetry.transaction_build_cache_provenance_reused_sections
        << ",compare_sections="
        << telemetry.transaction_build_cache_compare_sections
        << ",provenance_bytes="
        << telemetry.transaction_build_cache_provenance_reused_bytes
        << ",compare_bytes="
        << telemetry.transaction_build_cache_compare_bytes
        << ",reused_bytes="
        << telemetry.transaction_build_cache_reused_bytes
        << ",fallback_reason="
        << telemetry.transaction_build_cache_fallback_reason
        << ",hard_link_fallback_sections="
        << telemetry.transaction_build_cache_hard_link_fallback_sections
        << ",lifecycle_pass="
        << (lifecycle_pass ? 1 : 0)
        << ",dirty_after_gc_pass="
        << (d4n2_pass ? 1 : 0)
        << '\n';

    std::cout
        << "D4P1_FROZEN_BUILD_CACHE_CAPABILITY,"
        << (d4p1_pass ? "PASS" : "FAIL")
        << ",sources=" << source_count
        << ",freeze_provenance_sections="
        << telemetry.generation_freeze_build_cache_provenance_sections
        << ",commit_provenance_sections="
        << telemetry.transaction_build_cache_provenance_reused_sections
        << ",freeze_provenance_bytes="
        << telemetry.generation_freeze_build_cache_provenance_bytes
        << ",commit_provenance_bytes="
        << telemetry.transaction_build_cache_provenance_reused_bytes
        << ",binding_rejected_sections="
        << telemetry.
            transaction_build_cache_provenance_binding_rejected_sections
        << ",compare_bytes="
        << telemetry.transaction_build_cache_compare_bytes
        << ",fallback_reason="
        << telemetry.transaction_build_cache_fallback_reason
        << '\n';

        std::cout
        << "D4Q1_FREEZE_OWNERSHIP_AUDIT,"
        << (d4q1_pass ? "PASS" : "FAIL")
        << ",sources=" << source_count
        << ",staging_ms="
        << ns_ms(
            telemetry.generation_freeze_audit_staging_ns)
        << ",validation_ms="
        << ns_ms(
            telemetry.generation_freeze_audit_validation_ns)
        << ",unclassified_ms="
        << ns_ms(
            telemetry.generation_freeze_audit_unclassified_ns)
        << ",compiled_origin="
        << telemetry.generation_freeze_audit_compiled_origin
        << ",compiled_bytes="
        << telemetry.generation_freeze_audit_compiled_bytes
        << ",source_manager_origin="
        << telemetry.generation_freeze_audit_source_manager_origin
        << ",source_manager_bytes="
        << telemetry.generation_freeze_audit_source_manager_bytes
        << ",source_manager_baseline_direct_borrow_bytes="
        << telemetry.
            generation_freeze_audit_source_manager_baseline_direct_borrow_bytes
        << ",source_manager_baseline_direct_borrow_sections="
        << telemetry.
            generation_freeze_audit_source_manager_baseline_direct_borrow_sections
        << ",change_state_origin="
        << telemetry.generation_freeze_audit_change_state_origin
        << ",change_state_bytes="
        << telemetry.generation_freeze_audit_change_state_bytes
        << ",build_cache_origin="
        << telemetry.generation_freeze_audit_build_cache_origin
        << ",build_cache_bytes="
        << telemetry.generation_freeze_audit_build_cache_bytes
        << ",build_cache_baseline_bulk_bytes="
        << telemetry.
            generation_freeze_build_cache_mapped_baseline_bulk_bytes
        << ",build_cache_baseline_exact_bytes="
        << telemetry.
            generation_freeze_audit_build_cache_baseline_exact_bytes
        << ",build_cache_baseline_exact_sections="
        << telemetry.
            generation_freeze_audit_build_cache_baseline_exact_sections
        << '\n';

    std::cout
        << "D4Q2B_EXACT_SECTION_ZERO_COPY,"
        << (d4q2b_pass ? "PASS" : "FAIL")
        << ",sources=" << source_count
        << ",baseline_bulk_bytes="
        << build_cache_bulk_bytes
        << ",baseline_copied_bytes="
        << build_cache_copied_bytes
        << ",baseline_borrowed_bytes="
        << build_cache_borrowed_bytes
        << ",baseline_borrowed_sections="
        << telemetry.
            generation_freeze_build_cache_mapped_baseline_borrowed_sections
        << ",provenance_bytes="
        << telemetry.
            generation_freeze_build_cache_provenance_bytes
        << ",provenance_sections="
        << telemetry.
            generation_freeze_build_cache_provenance_sections
        << ",build_cache_ms="
        << ns_ms(
            telemetry.
                generation_freeze_build_cache_ns)
        << ",fallback_reason="
        << telemetry.
            transaction_build_cache_fallback_reason
        << '\n';

    std::cout
        << "D4Q2C1_ALL_EXACT_SECTIONS_ZERO_COPY,"
        << (d4q2c1_pass ? "PASS" : "FAIL")
        << ",sources=" << source_count
        << ",baseline_bulk_bytes="
        << build_cache_bulk_bytes
        << ",baseline_copied_bytes="
        << build_cache_copied_bytes
        << ",baseline_borrowed_bytes="
        << build_cache_borrowed_bytes
        << ",baseline_borrowed_sections="
        << telemetry.
            generation_freeze_build_cache_mapped_baseline_borrowed_sections
        << ",provenance_bytes="
        << telemetry.
            generation_freeze_build_cache_provenance_bytes
        << ",provenance_sections="
        << telemetry.
            generation_freeze_build_cache_provenance_sections
        << ",build_cache_ms="
        << ns_ms(
            telemetry.
                generation_freeze_build_cache_ns)
        << ",source_frontend_ms="
        << ns_ms(
            telemetry.
                generation_freeze_build_cache_source_frontend_ns)
        << ",fallback_reason="
        << telemetry.
            transaction_build_cache_fallback_reason
        << '\n';

    std::cout
        << "D4Q2C2A_SOURCE_BYTES_PARTIAL_ZERO_COPY,"
        << (d4q2c2a_pass ? "PASS" : "FAIL")
        << ",sources=" << source_count
        << ",baseline_bulk_bytes="
        << build_cache_bulk_bytes
        << ",whole_borrowed_bytes="
        << build_cache_borrowed_bytes
        << ",sparse_borrowed_bytes="
        << build_cache_sparse_borrowed_bytes
        << ",sparse_borrowed_extents="
        << telemetry.
            generation_freeze_build_cache_mapped_baseline_sparse_borrowed_extents
        << ",total_borrowed_bytes="
        << build_cache_total_borrowed_bytes
        << ",baseline_copied_bytes="
        << build_cache_copied_bytes
        << ",build_cache_ms="
        << ns_ms(
            telemetry.
                generation_freeze_build_cache_ns)
        << ",source_frontend_ms="
        << ns_ms(
            telemetry.
                generation_freeze_build_cache_source_frontend_ns)
        << ",section_crc_ms="
        << ns_ms(
            telemetry.
                generation_freeze_build_cache_section_crc_ns)
        << ",fallback_reason="
        << telemetry.
            transaction_build_cache_fallback_reason
        << '\n';

    std::cout
        << "D4Q2C2B_SOURCE_DIRECTORY_PARTIAL_ZERO_COPY,"
        << (d4q2c2b_pass ? "PASS" : "FAIL")
        << ",sources=" << source_count
        << ",directory_borrowed_bytes="
        << build_cache_sparse_directory_borrowed_bytes
        << ",directory_borrowed_extents="
        << build_cache_sparse_directory_borrowed_extents
        << ",sparse_borrowed_bytes="
        << build_cache_sparse_borrowed_bytes
        << ",total_borrowed_bytes="
        << build_cache_total_borrowed_bytes
        << ",baseline_copied_bytes="
        << build_cache_copied_bytes
        << ",build_cache_ms="
        << ns_ms(
            telemetry.
                generation_freeze_build_cache_ns)
        << ",source_frontend_ms="
        << ns_ms(
            telemetry.
                generation_freeze_build_cache_source_frontend_ns)
        << ",section_crc_ms="
        << ns_ms(
            telemetry.
                generation_freeze_build_cache_section_crc_ns)
        << ",fallback_reason="
        << telemetry.
            transaction_build_cache_fallback_reason
        << '\n';

return d4q2c2b_pass ? 0 : 1;
}


struct d4l3b_extent_case final {
    std::size_t update_count = 0;
    bool distributed = false;
    std::uint32_t required_extents = 0;
    bool expect_sparse = false;
};

[[nodiscard]] int run_d4l3b_extent_geometry_gate(
    std::size_t source_count) {

#ifdef _WIN32
    // For adjacent physical_state records, only the first patch needs a
    // borrowed gap. Distributed records add one borrowed gap per later patch.
    // Both geometries therefore exercise the same dirty-source count with
    // different exact logical extent counts.
    constexpr d4l3b_extent_case cases[]{
        {13, false, 18, true},
        {13, true, 30, true},
        {14, false, 19, true},
        {14, true, 32, true},
        {15, false, 20, true},
        {15, true, 34, false},
        {27, false, 32, true},
        {27, true, 58, false},
        {28, false, 33, false},
    };

    if (source_count < 100)
        return 2;

    temporary_tree tree;
    std::filesystem::path configuration_path;
    std::vector<std::filesystem::path> unused_source_paths;

    const auto progress_interval =
        source_count >= 1'000'000
            ? std::size_t{100'000}
            : source_count >= 100'000
                ? std::size_t{10'000}
                : std::size_t{0};

    std::cerr
        << "D4L3B_SETUP_BEGIN,sources="
        << source_count
        << '\n';

    if (!prepare_project(
            source_count,
            tree,
            configuration_path,
            unused_source_paths,
            false,
            progress_interval)) {
        return 1;
    }

    baseline_commit_result baseline;
    if (!create_baseline(
            configuration_path,
            baseline,
            0)) {
        std::cout
            << "D4L3B_EXTENT_GEOMETRY_GATE,FAIL,"
            << "stage=baseline,sources="
            << source_count
            << '\n';
        return 1;
    }

    std::size_t revision = 0;

    for (const auto& test : cases) {
        ++revision;

        for (std::size_t ordinal = 0;
             ordinal < test.update_count;
             ++ordinal) {

            const auto source_index =
                test.distributed
                    ? ordinal * 2
                    : ordinal;

            if (source_index >= source_count)
                return 2;

            const auto changed_text =
                "struct " +
                type_name(source_index) +
                " { int d4l3b_" +
                std::to_string(revision) +
                "; };\n";

            if (!write_text(
                    tree.path /
                        source_name(source_index),
                    changed_text)) {
                return 1;
            }
        }

        project_manager manager;
        diagnostic_buffer diagnostics;
        project_build_result build;

        const auto build_status =
            manager.build(
                configuration_path,
                operation_id{4600},
                diagnostics,
                build,
                1);

        const bool build_pass =
            build_status.ok() &&
            !diagnostics.has_errors() &&
            manager.ready() &&
            build.changed &&
            !build.rebuilt &&
            build.telemetry.baseline_sources ==
                source_count &&
            build.telemetry.dirty_detection_backend == 1 &&
            build.telemetry.dirty_detection_fast &&
            !build.telemetry.dirty_detection_fallback &&
            build.telemetry.dirty_sources ==
                test.update_count &&
            build.telemetry.generation_checkpoint_available &&
            build.telemetry.generation_anchor_available &&
            build.telemetry.generation_change_ready &&
            build.telemetry.generation_change_overlay &&
            build.telemetry.generation_change_fallback_reason == 0 &&
            build.telemetry.generation_change_file_updates ==
                test.update_count &&
            build.telemetry.sources.path_index_full_rebuilds == 0 &&
            build.telemetry.sources.source_graph_full_scans == 0 &&
            build.telemetry.builder.graph_full_scans == 0 &&
            build.telemetry.builder.contribution_full_scans == 0;

        if (!build_pass) {
            std::cout
                << "D4L3B_EXTENT_GEOMETRY,FAIL,"
                << "stage=build,sources="
                << source_count
                << ",geometry="
                << (test.distributed
                        ? "distributed"
                        : "adjacent")
                << ",updates="
                << test.update_count
                << ",dirty_sources="
                << build.telemetry.dirty_sources
                << ",status_code="
                << static_cast<unsigned>(
                    build_status.code)
                << '\n';

            if (manager.ready())
                (void)manager.unload();

            return 1;
        }

        baseline_commit_result save;
        const auto save_status =
            manager.save(save);
        const auto& telemetry =
            save.telemetry;

        const bool required_extents_preserved =
            telemetry.
                generation_freeze_source_manager_sparse_required_extent_count ==
            test.required_extents;

        const bool source_manager_pass =
            required_extents_preserved &&
            (test.expect_sparse
                ? telemetry.generation_freeze_source_manager_mode == 3 &&
                  telemetry.generation_freeze_source_manager_sparse_fallback_reason == 0 &&
                  telemetry.generation_freeze_source_manager_extent_count ==
                      test.required_extents
                : telemetry.generation_freeze_source_manager_mode == 2 &&
                  telemetry.generation_freeze_source_manager_sparse_fallback_reason == 10 &&
                  telemetry.generation_freeze_source_manager_extent_count == 1);

        const bool save_pass =
            save_status.ok() &&
            !save.transaction.empty() &&
            save.bytes_written != 0 &&
            telemetry.generation_freeze_materialize_change_file_updates ==
                test.update_count &&
            source_manager_pass;

        std::cout
            << "D4L3B_EXTENT_GEOMETRY,"
            << (save_pass ? "PASS" : "FAIL")
            << ",sources="
            << source_count
            << ",geometry="
            << (test.distributed
                    ? "distributed"
                    : "adjacent")
            << ",updates="
            << test.update_count
            << ",required_extents="
            << test.required_extents
            << ",budget=32"
            << ",expect_sparse="
            << (test.expect_sparse ? 1 : 0)
            << ",source_manager_mode="
            << telemetry.generation_freeze_source_manager_mode
            << ",fallback_reason="
            << telemetry.generation_freeze_source_manager_sparse_fallback_reason
            << ",required_extent_count="
            << telemetry.
                generation_freeze_source_manager_sparse_required_extent_count
            << ",reported_extent_count="
            << telemetry.generation_freeze_source_manager_extent_count
            << ",save_total_ms="
            << static_cast<double>(
                telemetry.save_total_ns) /
                1'000'000.0
            << '\n';

        if (!save_pass) {
            if (manager.ready())
                (void)manager.unload();
            return 1;
        }

        if (!manager.ready() ||
            !manager.unload().ok()) {
            return 1;
        }
    }

    project_manager verify_manager;
    diagnostic_buffer verify_diagnostics;
    project_build_result verify_build;

    const auto verify_status =
        verify_manager.build(
            configuration_path,
            operation_id{4601},
            verify_diagnostics,
            verify_build,
            1);

    const bool verify_pass =
        verify_status.ok() &&
        !verify_diagnostics.has_errors() &&
        verify_manager.ready() &&
        validate_no_change(
            source_count,
            verify_build) &&
        verify_build.telemetry.dirty_detection_backend == 1 &&
        verify_build.telemetry.dirty_detection_fast &&
        !verify_build.telemetry.dirty_detection_fallback &&
        verify_build.telemetry.builder.graph_full_scans == 0 &&
        verify_build.telemetry.builder.contribution_full_scans == 0;

    if (verify_manager.ready() &&
        !verify_manager.unload().ok()) {
        return 1;
    }

    std::cout
        << "D4L3B_EXTENT_GEOMETRY_GATE,"
        << (verify_pass ? "PASS" : "FAIL")
        << ",sources="
        << source_count
        << ",dirty_sources="
        << verify_build.telemetry.dirty_sources
        << ",graph_full_scans="
        << verify_build.telemetry.builder.graph_full_scans
        << ",contribution_full_scans="
        << verify_build.telemetry.builder.contribution_full_scans
        << '\n';

    return verify_pass ? 0 : 1;
#else
    (void)source_count;
    std::cout
        << "D4L3B_EXTENT_GEOMETRY_GATE,UNAVAILABLE,"
        << "backend=0,platform=non_windows\n";
    return 3;
#endif
}

[[nodiscard]] bool run_matrix() {
    constexpr std::size_t matrix[]{
        1'000,
        10'000,
        100'000,
    };

    bool pass = true;
    for (const auto count : matrix) {
        sparse_case_result result;
        pass =
            run_sparse_case(
                count,
                true,
                result) &&
            pass;
    }
    return pass;
}

} // namespace

int main(int argc, char** argv) {
    std::cout
        << std::fixed
        << std::setprecision(6);

    if ((argc == 3 || argc == 4) &&
        std::string_view{argv[1]} ==
            "--lazy-source-cycle") {
        try {
            const auto count =
                static_cast<std::size_t>(
                    std::stoull(argv[2]));

            const auto workers =
                argc == 4
                    ? static_cast<std::size_t>(
                          std::stoull(argv[3]))
                    : std::size_t{0};

            return run_lazy_source_cycle_profile(
                count,
                workers);
        }
        catch (...) {
            return 2;
        }
    }

    if (argc == 5 &&
        std::string_view{argv[1]} ==
            "--rebuild-profile-io") {
        try {
            const auto count =
                static_cast<std::size_t>(
                    std::stoull(argv[2]));
            const auto workers =
                static_cast<std::size_t>(
                    std::stoull(argv[3]));
            const auto io_workers =
                static_cast<std::size_t>(
                    std::stoull(argv[4]));

            return run_rebuild_profile(
                count,
                workers,
                io_workers);
        }
        catch (...) {
            return 2;
        }
    }

    if ((argc == 3 || argc == 4) &&
        std::string_view{argv[1]} ==
            "--rebuild-profile") {
        try {
            const auto count =
                static_cast<std::size_t>(
                    std::stoull(argv[2]));

            const auto workers =
                argc == 4
                    ? static_cast<std::size_t>(
                          std::stoull(argv[3]))
                    : std::size_t{0};

            return run_rebuild_profile(
                count,
                workers);
        }
        catch (...) {
            return 2;
        }
    }

    if ((argc == 3 ||
         argc == 4 ||
         argc == 5) &&
        std::string_view{argv[1]} ==
            "--idempotent-save") {
        try {
            const auto count =
                static_cast<std::size_t>(
                    std::stoull(argv[2]));

            const auto workers =
                argc >= 4
                    ? static_cast<std::size_t>(
                          std::stoull(argv[3]))
                    : std::size_t{0};

            const auto repeats =
                argc == 5
                    ? static_cast<std::size_t>(
                          std::stoull(argv[4]))
                    : std::size_t{5};

            return run_idempotent_save_benchmark(
                count,
                workers,
                repeats);
        }
        catch (...) {
            return 2;
        }
    }

    if (argc == 2 &&
        std::string_view{argv[1]} ==
            "--million-idempotent-save") {
        return run_idempotent_save_benchmark(
            1'000'000,
            0,
            5);
    }

    if (argc == 2 &&
        std::string_view{argv[1]} ==
            "--million-lifecycle") {
        return run_lifecycle_scale(
            1'000'000,
            0);
    }

    if ((argc == 3 || argc == 4) &&
        std::string_view{argv[1]} ==
            "--lifecycle") {
        try {
            const auto count =
                static_cast<std::size_t>(
                    std::stoull(argv[2]));

            const auto workers =
                argc == 4
                    ? static_cast<std::size_t>(
                          std::stoull(argv[3]))
                    : std::size_t{0};

            if (count == 0)
                return 2;

            return run_lifecycle_scale(
                count,
                workers);
        }
        catch (...) {
            return 2;
        }
    }


    if (argc == 4 &&
        std::string_view{argv[1]} ==
            "--d4m1-io-budget") {
        try {
            const auto count =
                static_cast<std::size_t>(
                    std::stoull(argv[2]));
            const auto budget =
                static_cast<std::size_t>(
                    std::stoull(argv[3]));

            if (count == 0 ||
                budget == 0) {
                return 2;
            }

            return run_d4a_sparse_save_materialization_profile(
                count,
                budget);
        }
        catch (...) {
            return 2;
        }
    }

    if (argc == 2 &&
        std::string_view{argv[1]} ==
            "--d4l3b-geometry") {
        return run_d4l3b_extent_geometry_gate(
            100'000);
    }

    if (argc == 3 &&
        std::string_view{argv[1]} ==
            "--d4l3b-geometry") {
        try {
            const auto count =
                static_cast<std::size_t>(
                    std::stoull(argv[2]));

            return run_d4l3b_extent_geometry_gate(
                count);
        }
        catch (...) {
            return 2;
        }
    }

    if (argc == 2 &&
        std::string_view{argv[1]} == "--d4e") {
        return run_d4l3b_extent_geometry_gate(
            100'000);
    }

    if (argc == 3 &&
        std::string_view{argv[1]} ==
            "--d4e-sparse-save") {
        try {
            const auto count =
                static_cast<std::size_t>(
                    std::stoull(argv[2]));
            return run_d4l3b_extent_geometry_gate(
                count);
        }
        catch (...) {
            return 2;
        }
    }

    if (argc == 2 &&
        std::string_view{argv[1]} == "--d4a") {

        const auto first =
            run_d4a_sparse_save_materialization_profile(
                100'000);
        if (first != 0)
            return first;

        return run_d4a_sparse_save_materialization_profile(
            1'000'000);
    }

    if (argc == 3 &&
        std::string_view{argv[1]} ==
            "--d4a-sparse-save") {
        try {
            const auto count =
                static_cast<std::size_t>(
                    std::stoull(argv[2]));
            return run_d4a_sparse_save_materialization_profile(
                count);
        }
        catch (...) {
            return 2;
        }
    }

    print_header();

    if (argc == 2 &&
        std::string_view{argv[1]} == "--gate") {
        return run_gate() ? 0 : 1;
    }

    if (argc == 2 &&
        std::string_view{argv[1]} == "--fast-gate") {
        return run_fast_gate();
    }

    if (argc == 2 &&
        std::string_view{argv[1]} == "--fast-modify-gate") {
        return run_fast_modify_gate();
    }

    if (argc == 2 &&
        std::string_view{argv[1]} == "--million-fast-build-gate") {
        return run_million_fast_build_gate();
    }

    if (argc == 2 &&
        std::string_view{argv[1]} == "--matrix") {
        return run_matrix() ? 0 : 1;
    }

    if (argc == 3 &&
        std::string_view{argv[1]} == "--scale") {
        try {
            const auto count =
                static_cast<std::size_t>(
                    std::stoull(argv[2]));

            sparse_case_result result;
            return run_sparse_case(
                count,
                true,
                result)
                ? 0
                : 1;
        }
        catch (...) {
            return 2;
        }
    }

    return 2;
}
