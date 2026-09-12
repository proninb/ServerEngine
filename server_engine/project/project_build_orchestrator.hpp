#pragma once

#include "builder/generation_builder.hpp"
#include "frontend/source_frontend_generation.hpp"
#include "project_context.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace cw::server {

struct project_build_telemetry final {
    project_storage_pressure storage_before{};
    project_storage_pressure storage_after{};
    // project_manager BUILD boundary. These fields are populated for persisted
    // baseline BUILDs; orchestrator-only tests leave them zero.
    std::uint64_t configuration_identity_ns = 0;
    std::uint64_t configuration_ns = 0;
    std::uint64_t fingerprint_ns = 0;
    std::uint64_t baseline_open_ns = 0;
    std::uint64_t dirty_detection_ns = 0;
    std::uint64_t baseline_activation_ns = 0;
    std::uint64_t build_activation_ns = 0;
    std::uint64_t manager_total_ns = 0;
    std::uint64_t baseline_sources = 0;
    std::uint64_t dirty_sources = 0;
    std::uint64_t journal_records = 0;
    std::uint64_t journal_matched_sources = 0;
    std::uint32_t dirty_detection_backend = 0;
    bool dirty_detection_fast = false;
    bool dirty_detection_fallback = false;

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
    bool rebuilt = false;
};

// Coordinates Project construction before READY. No Project readers exist while
// construct() executes, so publication is an internal ownership boundary rather
// than a reader/writer synchronization boundary. update() remains a low-level
// construction primitive for incremental Builder tests and future BUILD storage.
class project_build_orchestrator final {
public:
    explicit project_build_orchestrator(
        project_context& project_value,
        std::size_t worker_limit_value = 0) noexcept
        : project(project_value),
          worker_limit(worker_limit_value) {}

    // Builds directly into a construction-only Project Context. Failure leaves
    // that candidate disposable; project_manager destroys it and returns UNLOADED.
    [[nodiscard]] status construct(
        operation_id operation,
        diagnostic_buffer& diagnostics,
        project_build_result& output) noexcept;

    // Detached full replacement retained as a low-level compatibility primitive.
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
    [[nodiscard]] status rebuild_current(
        operation_id operation,
        diagnostic_buffer& diagnostics,
        project_build_result& output) noexcept;

    project_context& project;
    std::size_t worker_limit = 0;
};

} // namespace cw::server
