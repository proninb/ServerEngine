#pragma once

#include "builder/generation_builder.hpp"
#include "frontend/source_frontend_generation.hpp"
#include "project_context.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace cw::server {

struct project_build_telemetry final {
    std::uint64_t frontend_ns = 0;
    std::uint64_t builder_prepare_ns = 0;
    std::uint64_t source_prepare_publish_ns = 0;
    std::uint64_t interface_prepare_publish_ns = 0;
    std::uint64_t publication_ns = 0;
    std::uint64_t interface_publish_ns = 0;
    std::uint64_t total_ns = 0;

    source_frontend_summary frontend{};
    source_manager_update_telemetry sources{};
    generation_build_telemetry builder{};
};

struct project_build_result final {
    project_build_telemetry telemetry{};
    bool changed = false;
};

// Coordinates one complete Project build. Authoritative Source Manager,
// SourceContribution and Graph candidates are fully prepared before a fixed no-fail
// publication barrier. Parser interfaces are reconstructable COLD acceleration
// state and publish only after that barrier succeeds.
class project_build_orchestrator final {
public:
    explicit project_build_orchestrator(
        project_context& project_value,
        std::size_t worker_limit_value = 0) noexcept
        : project(project_value), worker_limit(worker_limit_value) {}

    [[nodiscard]] status rebuild(
        operation_id operation,
        diagnostic_buffer& diagnostics,
        project_build_result& output) noexcept;

    [[nodiscard]] status update(
        std::span<const source_id> dirty_sources,
        operation_id operation,
        diagnostic_buffer& diagnostics,
        project_build_result& output) noexcept;

private:
    project_context& project;
    std::size_t worker_limit = 0;
};

} // namespace cw::server
