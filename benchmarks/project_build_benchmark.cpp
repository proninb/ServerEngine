#include "../server_engine/project/project_build_orchestrator.hpp"

#include <chrono>
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
using clock_type = std::chrono::steady_clock;

struct temporary_tree final {
    std::filesystem::path path;

    ~temporary_tree() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

[[nodiscard]] bool write_file(const std::filesystem::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(output);
}

[[nodiscard]] std::string type_name(std::size_t index) {
    return "Type" + std::to_string(index);
}

void print_header() {
    std::cout
        << "sources,scenario,total_ms,frontend_ms,builder_ms,source_prepare_ms,interface_prepare_ms,"
           "publish_us,interface_publish_us,dirty,changed,affected,acquired,lexed,parsed,reused_interfaces,"
           "source_graph_visited,path_index_full_rebuilds,source_graph_full_scans,reverse_edge_patches,"
           "builder_changed_sources,builder_changed_types,builder_graph_full_scans,"
           "builder_contribution_full_scans,status\n";
}

void print_result(
    std::size_t sources,
    std::string_view scenario,
    const project_build_result& value,
    bool pass) {

    const auto& t = value.telemetry;
    std::cout << sources << ',' << scenario << ','
              << static_cast<double>(t.total_ns) / 1'000'000.0 << ','
              << static_cast<double>(t.frontend_ns) / 1'000'000.0 << ','
              << static_cast<double>(t.builder_prepare_ns) / 1'000'000.0 << ','
              << static_cast<double>(t.source_prepare_publish_ns) / 1'000'000.0 << ','
              << static_cast<double>(t.interface_prepare_publish_ns) / 1'000'000.0 << ','
              << static_cast<double>(t.publication_ns) / 1'000.0 << ','
              << t.frontend.dirty << ',' << t.frontend.changed << ',' << t.frontend.affected << ','
              << t.frontend.acquired << ',' << t.frontend.lexed << ',' << t.frontend.parsed << ','
              << t.frontend.reused_interfaces << ',' << t.frontend.source_graph_visited << ','
              << t.sources.path_index_full_rebuilds << ',' << t.sources.source_graph_full_scans << ','
              << t.sources.reverse_edge_patches << ',' << t.builder.changed_sources << ','
              << t.builder.changed_types << ',' << t.builder.graph_full_scans << ','
              << t.builder.contribution_full_scans << ',' << (pass ? "PASS" : "FAIL") << '\n';
}

[[nodiscard]] bool run_independent(std::size_t count, bool enforce_timing) {
    if (count == 0 || count >= static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)()))
        return false;

    temporary_tree tree;
    tree.path = std::filesystem::temp_directory_path() /
        ("server_engine_v308_benchmark_" + std::to_string(count));
    std::error_code error;
    std::filesystem::remove_all(tree.path, error);
    error.clear();
    std::filesystem::create_directories(tree.path, error);
    if (error)
        return false;

    project_configuration configuration;
    configuration.version = 1;
    configuration.name = "E2E benchmark";
    configuration.project.reserve(count);
    std::vector<std::filesystem::path> paths;
    paths.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        auto path = tree.path / ("s" + std::to_string(index) + ".hpp");
        const auto text = "struct " + type_name(index) + ";\n";
        if (!write_file(path, text))
            return false;
        configuration.project.push_back(project_item_configuration{path, project_item_role::type});
        paths.push_back(std::move(path));
    }

    project_context project{std::move(configuration)};
    project_build_orchestrator orchestrator{project};
    diagnostic_buffer diagnostics;
    project_build_result full;
    auto result = orchestrator.rebuild(operation_id{3000}, diagnostics, full);
    const bool full_pass = result.ok() && !diagnostics.has_errors() &&
        project.sources().source_count() == count && project.compiled_graph().type_count() == count;
    print_result(count, "full_independent", full, full_pass);
    if (!full_pass)
        return false;

    const auto target = count / 2;
    const auto changed_text = "struct " + type_name(target) + " { int value; };\n";
    if (!write_file(paths[target], changed_text))
        return false;

    std::string normalized;
    source_id dirty;
    if (!normalize_source_path(paths[target], normalized).ok() ||
        !project.sources().find(normalized, dirty).ok())
        return false;

    diagnostics.clear();
    project_build_result update;
    const std::array dirty_sources{dirty};
    result = orchestrator.update(dirty_sources, operation_id{3001}, diagnostics, update);
    bool update_pass = result.ok() && !diagnostics.has_errors() && update.changed &&
        update.telemetry.frontend.dirty == 1 && update.telemetry.frontend.changed == 1 &&
        update.telemetry.frontend.affected == 1 && update.telemetry.frontend.acquired == 1 &&
        update.telemetry.frontend.parsed == 1 &&
        update.telemetry.sources.path_index_full_rebuilds == 0 &&
        update.telemetry.sources.source_graph_full_scans == 0 &&
        update.telemetry.builder.graph_full_scans == 0 &&
        update.telemetry.builder.contribution_full_scans == 0;
    if (enforce_timing)
        update_pass = update_pass && update.telemetry.total_ns <= 25'000'000ULL;
    print_result(count, "incremental_modify_one", update, update_pass);
    return update_pass;
}

[[nodiscard]] bool run_fanout(std::size_t dependents, bool enforce_timing) {
    temporary_tree tree;
    tree.path = std::filesystem::temp_directory_path() /
        ("server_engine_v308_fanout_" + std::to_string(dependents));
    std::error_code error;
    std::filesystem::remove_all(tree.path, error);
    error.clear();
    std::filesystem::create_directories(tree.path, error);
    if (error)
        return false;

    const auto common_path = tree.path / "common.hpp";
    if (!write_file(common_path, "struct Common;\n"))
        return false;

    project_configuration configuration;
    configuration.version = 1;
    configuration.name = "fanout";
    configuration.project.reserve(dependents);
    for (std::size_t index = 0; index < dependents; ++index) {
        const auto path = tree.path / ("d" + std::to_string(index) + ".hpp");
        const auto text = "#include \"common.hpp\"\nstruct " + type_name(index) + " { Common* value; };\n";
        if (!write_file(path, text))
            return false;
        configuration.project.push_back(project_item_configuration{path, project_item_role::type});
    }

    project_context project{std::move(configuration)};
    project_build_orchestrator orchestrator{project};
    diagnostic_buffer diagnostics;
    project_build_result full;
    auto result = orchestrator.rebuild(operation_id{3010}, diagnostics, full);
    if (!result.ok() || diagnostics.has_errors())
        return false;

    std::string normalized;
    source_id common_source;
    if (!normalize_source_path(common_path, normalized).ok() ||
        !project.sources().find(normalized, common_source).ok())
        return false;

    if (!write_file(common_path, "struct Common { int value; };\n"))
        return false;
    diagnostics.clear();
    project_build_result update;
    const std::array dirty{common_source};
    result = orchestrator.update(dirty, operation_id{3011}, diagnostics, update);
    const auto expected = dependents + 1;
    bool pass = result.ok() && !diagnostics.has_errors() &&
        update.telemetry.frontend.changed == 1 && update.telemetry.frontend.acquired == 1 &&
        update.telemetry.frontend.affected == expected && update.telemetry.frontend.parsed == expected &&
        update.telemetry.sources.source_graph_full_scans == 0 &&
        update.telemetry.builder.graph_full_scans == 0 &&
        update.telemetry.builder.contribution_full_scans == 0;
    if (enforce_timing)
        pass = pass && update.telemetry.total_ns <= 500'000'000ULL;
    print_result(expected, "incremental_common_fanout", update, pass);
    return pass;
}

} // namespace

int main(int argc, char** argv) {
    std::cout << std::fixed << std::setprecision(6);
    print_header();

    if (argc == 2 && std::string_view{argv[1]} == "--gate") {
        const bool independent = run_independent(8192, true);
        const bool fanout = run_fanout(1024, true);
        const bool pass = independent && fanout;
        std::cout << "PROJECT_BUILD_E2E_GATE," << (pass ? "PASS" : "FAIL")
                  << ",incremental_no_full_scans,modify_one<=25ms,fanout<=500ms\n";
        return pass ? 0 : 1;
    }

    if (argc == 3 && std::string_view{argv[1]} == "--scale") {
        try {
            const auto count = static_cast<std::size_t>(std::stoull(argv[2]));
            return run_independent(count, false) ? 0 : 1;
        }
        catch (...) {
            return 2;
        }
    }

    return 2;
}
