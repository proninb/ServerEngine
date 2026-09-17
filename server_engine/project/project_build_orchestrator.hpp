#pragma once

#include "builder/generation_builder.hpp"
#include "frontend/source_frontend_generation.hpp"
#include "project_context.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace cw::server {

struct project_build_telemetry final {
    project_storage_pressure storage_before{};
    project_storage_pressure storage_after{};
    // project_manager BUILD boundary. These fields are populated for persisted
    // baseline BUILDs; orchestrator-only tests leave them zero.
    std::uint64_t configuration_identity_ns = 0;
    std::uint64_t configuration_probe_ns = 0;
    std::uint64_t configuration_gate_ns = 0;
    std::uint64_t configuration_ns = 0;
    std::uint64_t fingerprint_ns = 0;
    std::uint64_t baseline_open_ns = 0;
    std::uint64_t baseline_current_read_ns = 0;
    std::uint64_t baseline_embedded_manifest_parse_ns = 0;
    std::uint64_t baseline_manifest_validation_ns = 0;
    std::uint64_t baseline_compiled_map_ns = 0;
    std::uint64_t baseline_source_manager_map_ns = 0;
    std::uint64_t baseline_change_state_map_ns = 0;
    std::uint64_t baseline_build_cache_map_ns = 0;
    std::uint64_t baseline_size_validation_ns = 0;
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

    std::uint64_t generation_change_file_updates = 0;
    std::uint64_t generation_change_directory_updates = 0;
    std::uint32_t generation_change_fallback_reason = 0;
    bool generation_checkpoint_available = false;
    bool generation_anchor_available = false;
    bool generation_change_ready = false;
    bool generation_change_overlay = false;

    std::uint64_t frontend_ns = 0;
    std::uint64_t builder_prepare_ns = 0;
    std::uint64_t source_prepare_publish_ns = 0;
    std::uint64_t interface_prepare_publish_ns = 0;
    std::uint64_t publication_ns = 0;
    std::uint64_t interface_publish_ns = 0;
    std::uint64_t total_ns = 0;

    // Full-construction Generation finalization. These bytes become the sole
    // READY owner before project_manager publishes the Project.
    std::uint64_t generation_finalize_ns = 0;
    std::uint64_t generation_finalize_freeze_ns = 0;

    // R5E4-A: exact construction-to-READY freeze accounting. These are
    // observational only; they do not alter publication or persistence.
    std::uint64_t generation_finalize_freeze_internal_ns = 0;
    std::uint64_t generation_finalize_freeze_roots_ns = 0;

    std::uint64_t generation_finalize_freeze_source_manager_ns = 0;
    std::uint64_t generation_finalize_freeze_source_manager_internal_ns = 0;
    std::uint64_t generation_finalize_freeze_source_manager_preflight_ns = 0;
    std::uint64_t generation_finalize_freeze_source_manager_layout_ns = 0;
    std::uint64_t generation_finalize_freeze_source_manager_allocate_zero_ns = 0;
    std::uint64_t generation_finalize_freeze_source_manager_source_records_ns = 0;
    std::uint64_t generation_finalize_freeze_source_manager_roots_ns = 0;
    std::uint64_t generation_finalize_freeze_source_manager_path_index_ns = 0;
    std::uint64_t generation_finalize_freeze_source_manager_file_identity_ns = 0;
    std::uint64_t generation_finalize_freeze_source_manager_directory_identity_ns = 0;
    std::uint64_t generation_finalize_freeze_source_manager_crc_wall_ns = 0;
    std::uint64_t generation_finalize_freeze_source_manager_prefix_directory_encode_ns = 0;
    std::uint64_t generation_finalize_freeze_source_manager_directory_crc_ns = 0;
    std::uint64_t generation_finalize_freeze_source_manager_header_crc_ns = 0;
    std::uint64_t generation_finalize_freeze_source_manager_bind_ns = 0;
    std::uint64_t generation_finalize_freeze_source_manager_verify_ns = 0;
    std::uint64_t generation_finalize_freeze_source_manager_segment_validate_ns = 0;
    std::uint64_t generation_finalize_freeze_source_manager_identity_copy_ns = 0;
    std::uint32_t generation_finalize_freeze_source_manager_mode = 0;
    std::uint32_t generation_finalize_freeze_source_manager_extent_count = 0;

    std::uint64_t generation_finalize_freeze_change_state_ns = 0;

    std::uint64_t generation_finalize_freeze_build_cache_ns = 0;
    std::uint64_t generation_finalize_freeze_build_cache_layout_allocate_ns = 0;
    std::uint64_t generation_finalize_freeze_build_cache_source_frontend_ns = 0;
    std::uint64_t generation_finalize_freeze_build_cache_contribution_ns = 0;
    std::uint64_t generation_finalize_freeze_build_cache_graph_ns = 0;
    std::uint64_t generation_finalize_freeze_build_cache_change_identity_ns = 0;
    std::uint64_t generation_finalize_freeze_build_cache_section_crc_ns = 0;
    std::uint64_t generation_finalize_freeze_build_cache_header_directory_ns = 0;
    std::uint64_t generation_finalize_freeze_build_cache_bind_ns = 0;
    std::uint64_t generation_finalize_freeze_build_cache_verify_ns = 0;

    std::uint64_t generation_finalize_freeze_bind_ns = 0;
    std::uint64_t generation_finalize_freeze_verify_change_state_ns = 0;
    std::uint64_t generation_finalize_freeze_verify_build_cache_ns = 0;
    std::uint64_t generation_finalize_freeze_audit_staging_ns = 0;
    std::uint64_t generation_finalize_freeze_audit_validation_ns = 0;
    std::uint64_t generation_finalize_freeze_audit_unclassified_ns = 0;

    std::uint64_t generation_finalize_materialize_owned_ns = 0;
    std::uint64_t generation_finalize_compiled_bytes = 0;
    std::uint64_t generation_finalize_compiled_build_ns = 0;
    std::uint64_t generation_finalize_compiled_strings_ns = 0;
    std::uint64_t generation_finalize_compiled_identities_ns = 0;
    std::uint64_t generation_finalize_compiled_graph_arrays_ns = 0;
    std::uint64_t generation_finalize_compiled_graph_indexes_ns = 0;
    std::uint64_t generation_finalize_compiled_crc_ns = 0;
    std::uint64_t generation_finalize_compiled_header_ns = 0;
    std::uint64_t generation_finalize_release_graph_ns = 0;
    std::uint64_t generation_finalize_frontend_handoff_ns = 0;
    bool generation_finalize_frontend_move_owned = false;
    std::uint64_t generation_finalize_contribution_handoff_ns = 0;
    bool generation_finalize_contribution_move_owned = false;
    std::uint64_t generation_finalize_segment_snapshot_ns = 0;
    std::uint64_t generation_finalize_activate_ready_ns = 0;
    std::uint64_t generation_finalize_activate_owner_ns = 0;
    std::uint64_t generation_finalize_activate_segments_ns = 0;
    std::uint64_t generation_finalize_activate_bind_compiled_ns = 0;
    std::uint64_t generation_finalize_activate_bind_sources_ns = 0;
    std::uint64_t generation_finalize_activate_bind_build_ns = 0;
    std::uint64_t generation_finalize_activate_verify_ns = 0;
    std::uint64_t generation_finalize_activate_publish_ns = 0;
    std::uint64_t generation_finalize_activate_compiled_destroy_ns = 0;
    std::uint64_t generation_finalize_activate_teardown_graph_ns = 0;
    std::uint64_t generation_finalize_activate_teardown_contributions_ns = 0;
    std::uint64_t generation_finalize_activate_teardown_frontend_cache_ns = 0;
    std::uint64_t generation_finalize_activate_teardown_source_manager_ns = 0;
    std::uint64_t generation_finalize_activate_teardown_identities_ns = 0;
    std::uint64_t generation_finalize_activate_baseline_destroy_ns = 0;
    std::uint64_t generation_finalize_activate_cleanup_ns = 0;
    std::uint64_t generation_finalize_compiled_native_graph_bytes = 0;
    std::uint64_t generation_finalize_compiled_derived_graph_bytes = 0;
    std::uint64_t generation_finalize_compiled_fallback_graph_bytes = 0;
    std::uint32_t generation_finalize_compiled_native_graph_sections = 0;
    std::uint32_t generation_finalize_compiled_derived_graph_sections = 0;
    std::uint32_t generation_finalize_compiled_fallback_graph_mask = 0;
    std::uint32_t generation_finalize_compiled_native_nonempty_graph_mask = 0;
    std::uint32_t generation_finalize_compiled_expected_nonzero_graph_mask = 0;
    std::uint64_t generation_finalize_source_manager_bytes = 0;
    std::uint64_t generation_finalize_change_state_bytes = 0;
    std::uint64_t generation_finalize_build_cache_bytes = 0;

    // R5E2D storage proof for the committed Build Cache.
    std::uint64_t generation_finalize_build_cache_logical_bytes = 0;
    std::uint64_t generation_finalize_build_cache_prefix_bytes = 0;
    std::uint64_t generation_finalize_build_cache_owned_section_bytes = 0;
    std::uint64_t generation_finalize_build_cache_direct_frontend_bytes = 0;
    std::uint64_t generation_finalize_build_cache_direct_contribution_bytes = 0;
    std::uint64_t generation_finalize_build_cache_physical_bytes = 0;
    std::uint64_t generation_finalize_build_cache_alignment_padding_bytes = 0;
    std::uint64_t generation_finalize_build_cache_encoder_owned_capacity_bytes = 0;
    std::uint64_t generation_finalize_build_cache_monolithic_staging_bytes = 0;
    std::uint32_t generation_finalize_build_cache_physical_extents = 0;
    std::uint32_t generation_finalize_build_cache_direct_frontend_extents = 0;
    std::uint32_t generation_finalize_build_cache_direct_frontend_sections = 0;
    std::uint32_t generation_finalize_build_cache_direct_frontend_fallback = 0;
    std::uint32_t generation_finalize_build_cache_direct_contribution_extents = 0;
    std::uint32_t generation_finalize_build_cache_direct_contribution_sections = 0;
    std::uint32_t generation_finalize_build_cache_direct_contribution_fallback = 0;

    bool generation_finalized = false;

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
        std::size_t worker_limit_value = 0,
        std::size_t acquisition_worker_limit_value = 0) noexcept
        : project(project_value),
          worker_limit(worker_limit_value),
          acquisition_worker_limit(
              acquisition_worker_limit_value) {}

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
        project_build_result& output,
        source_change_checkpoint generation_checkpoint = {},
        std::string_view generation_anchor = {}) noexcept;

private:
    [[nodiscard]] status rebuild_current(
        operation_id operation,
        diagnostic_buffer& diagnostics,
        project_build_result& output) noexcept;

    project_context& project;
    std::size_t worker_limit = 0;
    std::size_t acquisition_worker_limit = 0;
};

} // namespace cw::server
