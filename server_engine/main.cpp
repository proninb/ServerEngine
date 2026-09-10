#include "config/server_configuration_loader.hpp"
#include "diagnostics/diagnostic_registry.hpp"
#include "project/project_manager.hpp"

#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

void print_diagnostics(const cw::server::diagnostic_buffer& diagnostics) {
    using namespace cw::server;
    for (const auto& record : diagnostics.records()) {
        const auto* descriptor = diagnostic_registry.find(record.id);
        const std::string_view name = descriptor != nullptr ? descriptor->name : "unknown";
        const std::string_view message = descriptor != nullptr ? descriptor->message : "Unknown diagnostic";
        std::cerr << name << ": " << message;
        if (!record.detail.empty()) std::cerr << " (" << record.detail << ')';
        std::cerr << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    using namespace cw::server;

    const std::filesystem::path server_path = argc > 1
        ? std::filesystem::path{argv[1]}
        : std::filesystem::path{"server.json"};

    diagnostic_buffer diagnostics;
    server_configuration server;
    if (!load_server_configuration_file(
            server_path, operation_id{1}, diagnostics, server).ok()) {
        print_diagnostics(diagnostics);
        return 1;
    }

    project_manager projects;
    project_build_result build;
    if (!projects.load(
            server.project.path, operation_id{2}, diagnostics, build).ok()) {
        print_diagnostics(diagnostics);
        return 2;
    }

    project_read_guard read;
    if (!projects.read(read).ok() || !read)
        return 3;

    const auto& graph = read->compiled_graph();
    std::cout << "Server Engine project loaded\n";
    std::cout << "Project: " << read->configuration().name << '\n';
    std::cout << "Project items: " << read->configuration().project.size() << '\n';
    std::cout << "Semantic identities: " << read->identity_count() << '\n';
    std::cout << "Types: " << graph.type_count() << '\n';
    std::cout << "Objects: " << graph.object_count() << '\n';
    std::cout << "Links: " << graph.link_count() << '\n';

    read = {};
    projects.unload();
    return 0;
}
