#include "config/server_configuration_loader.hpp"
#include "diagnostics/diagnostic_registry.hpp"
#include "project/project_configuration_loader.hpp"
#include "project/project_context.hpp"

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

    project_configuration project;
    if (!load_project_configuration_file(
            server.project.path, operation_id{2}, diagnostics, project).ok()) {
        print_diagnostics(diagnostics);
        return 2;
    }

    project_context context{std::move(project)};

    std::cout << "Server Engine foundation initialized\n";
    std::cout << "Project: " << context.configuration().name << '\n';
    std::cout << "Project items: " << context.configuration().project.size() << '\n';
    std::cout << "Semantic identities: " << context.identity_count() << " (root only)\n";
    return 0;
}
