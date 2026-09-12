#include "../server_engine/project/project_manager.hpp"

#include <algorithm>
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
    std::vector<std::filesystem::path>& sources) {

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
        sources.reserve(source_count);

        for (std::size_t index = 0; index < source_count; ++index) {
            auto path = tree.path / source_name(index);
            const auto text =
                "struct " + type_name(index) + ";\n";
            if (!write_text(path, text))
                return false;
            sources.push_back(std::move(path));
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
           "manager_ms,configuration_identity_ms,configuration_ms,fingerprint_ms,"
           "baseline_open_ms,manifest_ms,compiled_map_ms,source_manager_map_ms,"
           "build_cache_map_ms,size_validation_us,dirty_detection_ms,"
           "baseline_activation_ms,build_activation_ms,"
           "dirty_backend,dirty_fast,dirty_fallback,"
           "journal_records,journal_matched,"
           "orchestrator_ms,frontend_ms,builder_ms,"
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
            telemetry.configuration_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.fingerprint_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.baseline_open_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.baseline_manifest_validation_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.baseline_compiled_map_ns) / 1'000'000.0 << ','
        << static_cast<double>(
            telemetry.baseline_source_manager_map_ns) / 1'000'000.0 << ','
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
    constexpr double dirty_limit_ms = 100.0;

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
        build.telemetry.configuration_identity_ns != 0 &&
        build.telemetry.configuration_ns == 0 &&
        build.telemetry.fingerprint_ns == 0 &&
        build.telemetry.baseline_build_cache_map_ns == 0 &&
        dirty_ms <= dirty_limit_ms;

    std::cout
        << "D3D_FAST_DIRTY_GATE,"
        << (pass ? "PASS" : "FAIL")
        << ",dirty_ms=" << dirty_ms
        << ",limit_ms=" << dirty_limit_ms
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
    constexpr double dirty_limit_ms = 100.0;

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
        build.telemetry.configuration_identity_ns != 0 &&
        build.telemetry.configuration_ns == 0 &&
        build.telemetry.fingerprint_ns == 0 &&
        build.telemetry.baseline_open_ns != 0 &&
        build.telemetry.baseline_build_cache_map_ns != 0 &&
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
        dirty_ms <= dirty_limit_ms;

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
        << ",limit_ms=" << dirty_limit_ms
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
