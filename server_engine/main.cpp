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
    if (!projects.rebuild(
            server.project.path, operation_id{2}, diagnostics, build).ok()) {
        print_diagnostics(diagnostics);
        return 2;
    }

    project_access access;
    if (!projects.acquire(access).ok() || !access)
        return 3;

    const auto& graph = access->compiled_graph();
    std::cout << "Server Engine project ready\n";
    std::cout << "Project: " << access->configuration().name << '\n';
    std::cout << "Project items: " << access->configuration().project.size() << '\n';
    std::cout << "Semantic identities: " << access->identity_count() << '\n';
    std::cout << "Types: " << graph.type_count() << '\n';
    std::cout << "Objects: " << graph.object_count() << '\n';
    std::cout << "Links: " << graph.link_count() << '\n';

    access.reset();
    if (!projects.unload().ok())
        return 4;
    return 0;
}
