#include "../server_engine/project/project_manager.hpp"

#include <algorithm>
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
    baseline_commit_result& committed) {

    project_manager manager;
    diagnostic_buffer diagnostics;
    project_build_result rebuild;

    const auto rebuild_result = manager.rebuild(
        configuration_path,
        operation_id{3100},
        diagnostics,
        rebuild,
        1);

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
