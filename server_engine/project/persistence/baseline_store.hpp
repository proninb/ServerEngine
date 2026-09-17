#pragma once

#include "../../status.hpp"
#include "../source/file_snapshot.hpp"
#include "../source/source_change_tracker.hpp"
#include "../project_generation_segments.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cw::server {

class build_cache_image_view;
class source_manager_image_view;
class baseline_snapshot;
class project_generation_storage;

inline constexpr std::uint32_t baseline_format_version = 1;
inline constexpr std::size_t baseline_fingerprint_size = 32;

struct baseline_fingerprint final {
    std::array<std::uint8_t, baseline_fingerprint_size> bytes{};

    friend constexpr bool operator==(
        const baseline_fingerprint&,
        const baseline_fingerprint&) noexcept = default;
};

enum class baseline_artifact_kind : std::uint8_t {
    compiled,
    source_manager,
    change_state,
    build_cache,
};

// Full-write fallback is legal only when the optimized physical
// representation is structurally inapplicable. Runtime I/O/flush/corruption
// failures are errors and must never be hidden by a second full write.
enum class baseline_sectioned_fallback_reason : std::uint32_t {
    none = 0,
    structural_ineligible = 1,
};

// Capability-style proof that one logical section is the exact whole physical
// section file mapped by one pinned immutable baseline. Only baseline_snapshot
// can mint a valid proof; callers cannot construct one from metadata or CRC.
class baseline_section_provenance final {
public:
    baseline_section_provenance() noexcept = default;

    [[nodiscard]] bool valid() const noexcept {
        return owner_value != nullptr;
    }

    [[nodiscard]] const baseline_snapshot* owner() const noexcept {
        return owner_value;
    }

    [[nodiscard]] baseline_artifact_kind artifact() const noexcept {
        return artifact_value;
    }

    [[nodiscard]] std::uint32_t section() const noexcept {
        return section_value;
    }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {
            data_value,
            static_cast<std::size_t>(size_value)};
    }

private:
    baseline_section_provenance(
        const baseline_snapshot* owner,
        baseline_artifact_kind artifact,
        std::uint32_t section,
        const std::byte* data,
        std::uint64_t size) noexcept
        : owner_value(owner),
          artifact_value(artifact),
          section_value(section),
          data_value(data),
          size_value(size) {}

    const baseline_snapshot* owner_value = nullptr;
    baseline_artifact_kind artifact_value =
        baseline_artifact_kind::compiled;
    std::uint32_t section_value = 0;
    const std::byte* data_value = nullptr;
    std::uint64_t size_value = 0;

    friend class baseline_snapshot;
};

// Capability binding one encoder-proven exact baseline section to the immutable
// Build Cache bytes owned by one frozen project_generation_storage. The object
// is move-only so a caller cannot detach a valid binding from the frozen
// storage lifetime and later pair it with an unrelated Generation.
class frozen_baseline_section_provenance final {
public:
    frozen_baseline_section_provenance() noexcept = default;

    frozen_baseline_section_provenance(
        const frozen_baseline_section_provenance&) = delete;
    frozen_baseline_section_provenance& operator=(
        const frozen_baseline_section_provenance&) = delete;

    frozen_baseline_section_provenance(
        frozen_baseline_section_provenance&&) noexcept = default;
    frozen_baseline_section_provenance& operator=(
        frozen_baseline_section_provenance&&) noexcept = default;

    [[nodiscard]] bool valid() const noexcept {
        return baseline_value.valid() &&
            data_value != nullptr &&
            size_value != 0 &&
            baseline_value.bytes().size() == size_value;
    }

    [[nodiscard]] const baseline_section_provenance&
    baseline() const noexcept {
        return baseline_value;
    }

    [[nodiscard]] std::span<const std::byte>
    frozen_bytes() const noexcept {
        return {
            data_value,
            static_cast<std::size_t>(size_value)};
    }

    // Pointer identity is intentional. project_generation_storage owns the
    // Build Cache vector privately and exposes only const spans after freeze;
    // therefore the bound bytes cannot be replaced without changing this span.
    [[nodiscard]] bool valid_for(
        std::span<const std::byte> current) const noexcept {

        return valid() &&
            current.data() == data_value &&
            current.size() == size_value;
    }

private:
    frozen_baseline_section_provenance(
        const baseline_section_provenance& baseline,
        std::span<const std::byte> frozen) noexcept
        : baseline_value(baseline),
          data_value(frozen.data()),
          size_value(frozen.size()) {}

    baseline_section_provenance baseline_value{};
    const std::byte* data_value = nullptr;
    std::uint64_t size_value = 0;

    friend class project_generation_storage;
};

// Commit-scoped provenance. It is move-only and remains owned by the frozen
// project_generation_storage for the complete synchronous SAVE/commit call.
struct baseline_commit_provenance final {
    baseline_commit_provenance() noexcept = default;

    baseline_commit_provenance(
        const baseline_commit_provenance&) = delete;
    baseline_commit_provenance& operator=(
        const baseline_commit_provenance&) = delete;

    baseline_commit_provenance(
        baseline_commit_provenance&&) noexcept = default;
    baseline_commit_provenance& operator=(
        baseline_commit_provenance&&) noexcept = default;

    std::array<baseline_section_provenance, 10>
        source_manager{};
    std::array<
        frozen_baseline_section_provenance,
        25> build_cache{};
};

struct baseline_commit_telemetry final {
    // project_manager SAVE boundary.
    std::uint64_t save_total_ns = 0;
    std::uint64_t configuration_token_ns = 0;
    std::uint64_t configuration_read_ns = 0;
    std::uint64_t configuration_parse_ns = 0;
    std::uint64_t fingerprint_ns = 0;
    std::uint64_t generation_freeze_ns = 0;

    // freeze_project_generation breakdown. The outer generation_freeze_ns
    // includes call overhead; internal_ns is measured inside the freeze body.
    std::uint64_t generation_freeze_internal_ns = 0;
    std::uint64_t generation_freeze_materialize_change_ns = 0;
    std::uint64_t generation_freeze_materialize_change_update_index_allocate_zero_ns = 0;
    std::uint64_t generation_freeze_materialize_change_baseline_file_count_ns = 0;
    std::uint64_t generation_freeze_materialize_change_file_index_allocate_zero_ns = 0;
    std::uint64_t generation_freeze_materialize_change_baseline_file_merge_ns = 0;
    std::uint64_t generation_freeze_materialize_change_sparse_file_updates_ns = 0;
    std::uint64_t generation_freeze_materialize_change_baseline_directory_count_ns = 0;
    std::uint64_t generation_freeze_materialize_change_directory_index_allocate_zero_ns = 0;
    std::uint64_t generation_freeze_materialize_change_baseline_directory_merge_ns = 0;
    std::uint64_t generation_freeze_materialize_change_sparse_directory_updates_ns = 0;
    std::uint64_t generation_freeze_materialize_change_source_count = 0;
    std::uint64_t generation_freeze_materialize_change_baseline_file_capacity = 0;
    std::uint64_t generation_freeze_materialize_change_baseline_file_occupied = 0;
    std::uint64_t generation_freeze_materialize_change_baseline_directory_capacity = 0;
    std::uint64_t generation_freeze_materialize_change_baseline_directory_occupied = 0;
    std::uint64_t generation_freeze_materialize_change_file_updates = 0;
    std::uint64_t generation_freeze_materialize_change_directory_updates = 0;
    std::uint64_t generation_freeze_materialize_change_update_index_bytes = 0;
    std::uint64_t generation_freeze_materialize_change_file_index_bytes = 0;
    std::uint64_t generation_freeze_materialize_change_directory_index_bytes = 0;
    std::uint64_t generation_freeze_materialize_change_peak_temporary_bytes = 0;
    std::uint64_t generation_freeze_materialize_change_peak_owned_bytes = 0;
    std::uint64_t generation_freeze_compiled_ns = 0;
    std::uint64_t generation_freeze_compiled_total_ns = 0;
    std::uint64_t generation_freeze_compiled_sizing_layout_ns = 0;
    std::uint64_t generation_freeze_compiled_allocate_zero_ns = 0;
    std::uint64_t generation_freeze_compiled_strings_ns = 0;
    std::uint64_t generation_freeze_compiled_identities_ns = 0;
    std::uint64_t generation_freeze_compiled_graph_arrays_ns = 0;
    std::uint64_t generation_freeze_compiled_graph_indexes_ns = 0;
    std::uint64_t generation_freeze_compiled_section_crc_ns = 0;
    std::uint64_t generation_freeze_compiled_header_bind_ns = 0;
    std::uint64_t generation_freeze_compiled_baseline_bulk_bytes = 0;
    std::uint32_t generation_freeze_compiled_baseline_bulk_sections = 0;
    std::uint64_t generation_freeze_compiled_output_bytes = 0;
    std::uint64_t generation_freeze_roots_ns = 0;
    std::uint64_t generation_freeze_source_manager_ns = 0;
    std::uint64_t generation_freeze_source_manager_internal_ns = 0;
    std::uint64_t generation_freeze_source_manager_preflight_ns = 0;
    std::uint64_t generation_freeze_source_manager_layout_ns = 0;
    std::uint64_t generation_freeze_source_manager_allocate_zero_ns = 0;
    std::uint64_t generation_freeze_source_manager_source_records_ns = 0;
    std::uint64_t generation_freeze_source_manager_roots_ns = 0;
    std::uint64_t generation_freeze_source_manager_path_index_ns = 0;
    std::uint64_t generation_freeze_source_manager_file_identity_ns = 0;
    std::uint64_t generation_freeze_source_manager_directory_identity_ns = 0;
    std::uint64_t generation_freeze_source_manager_crc_wall_ns = 0;
    std::uint64_t generation_freeze_source_manager_crc_total_bytes = 0;
    std::uint64_t generation_freeze_source_manager_crc_source_core_ns = 0;
    std::uint64_t generation_freeze_source_manager_crc_source_core_bytes = 0;
    std::uint64_t generation_freeze_source_manager_crc_physical_state_ns = 0;
    std::uint64_t generation_freeze_source_manager_crc_physical_state_bytes = 0;
    std::uint64_t generation_freeze_source_manager_crc_graph_records_ns = 0;
    std::uint64_t generation_freeze_source_manager_crc_graph_records_bytes = 0;
    std::uint64_t generation_freeze_source_manager_crc_forward_edges_ns = 0;
    std::uint64_t generation_freeze_source_manager_crc_forward_edges_bytes = 0;
    std::uint64_t generation_freeze_source_manager_crc_reverse_edges_ns = 0;
    std::uint64_t generation_freeze_source_manager_crc_reverse_edges_bytes = 0;
    std::uint64_t generation_freeze_source_manager_crc_roots_ns = 0;
    std::uint64_t generation_freeze_source_manager_crc_roots_bytes = 0;
    std::uint64_t generation_freeze_source_manager_crc_path_index_ns = 0;
    std::uint64_t generation_freeze_source_manager_crc_path_index_bytes = 0;
    std::uint64_t generation_freeze_source_manager_crc_path_bytes_ns = 0;
    std::uint64_t generation_freeze_source_manager_crc_path_bytes_bytes = 0;
    std::uint64_t generation_freeze_source_manager_crc_file_identity_ns = 0;
    std::uint64_t generation_freeze_source_manager_crc_file_identity_bytes = 0;
    std::uint64_t generation_freeze_source_manager_crc_directory_identity_ns = 0;
    std::uint64_t generation_freeze_source_manager_crc_directory_identity_bytes = 0;
    std::uint64_t generation_freeze_source_manager_prefix_directory_encode_ns = 0;
    std::uint64_t generation_freeze_source_manager_directory_crc_ns = 0;
    std::uint64_t generation_freeze_source_manager_header_crc_ns = 0;
    std::uint64_t generation_freeze_source_manager_bind_ns = 0;
    std::uint64_t generation_freeze_source_manager_verify_ns = 0;
    std::uint64_t generation_freeze_source_manager_segment_validate_ns = 0;
    std::uint64_t generation_freeze_source_manager_identity_copy_ns = 0;
    std::uint32_t generation_freeze_source_manager_mode = 0;
    std::uint32_t generation_freeze_source_manager_crc_worker_count = 0;
    std::uint32_t generation_freeze_source_manager_extent_count = 0;
    std::uint64_t generation_freeze_source_manager_sparse_required_extent_count = 0;
    std::uint32_t generation_freeze_source_manager_sparse_fallback_reason = 0;
    std::uint64_t generation_freeze_change_state_ns = 0;
    std::uint64_t generation_freeze_build_cache_ns = 0;
    std::uint64_t generation_freeze_build_cache_layout_allocate_ns = 0;
    std::uint64_t generation_freeze_build_cache_source_frontend_ns = 0;
    std::uint64_t generation_freeze_build_cache_source_directory_text_ns = 0;
    std::uint64_t generation_freeze_build_cache_frontend_record_ranges_ns = 0;
    std::uint64_t generation_freeze_build_cache_frontend_local_types_ns = 0;
    std::uint64_t generation_freeze_build_cache_frontend_type_slots_ns = 0;
    std::uint64_t generation_freeze_build_cache_frontend_object_slots_ns = 0;
    std::uint64_t generation_freeze_build_cache_frontend_member_slots_ns = 0;
    std::uint64_t generation_freeze_build_cache_source_frontend_total_sources = 0;
    std::uint64_t generation_freeze_build_cache_source_frontend_sampled_sources = 0;
    std::uint64_t generation_freeze_build_cache_source_lookup_sample_ns = 0;
    std::uint64_t generation_freeze_build_cache_source_text_copy_sample_ns = 0;
    std::uint64_t generation_freeze_build_cache_frontend_record_sample_ns = 0;
    std::uint64_t generation_freeze_build_cache_frontend_local_types_sample_ns = 0;
    std::uint64_t generation_freeze_build_cache_frontend_type_slots_sample_ns = 0;
    std::uint64_t generation_freeze_build_cache_frontend_object_slots_sample_ns = 0;
    std::uint64_t generation_freeze_build_cache_frontend_member_slots_sample_ns = 0;
    std::uint64_t generation_freeze_build_cache_contribution_ns = 0;
    std::uint64_t generation_freeze_build_cache_graph_ns = 0;
    std::uint64_t generation_freeze_build_cache_change_identity_ns = 0;
    std::uint64_t generation_freeze_build_cache_section_crc_ns = 0;
    std::uint64_t generation_freeze_build_cache_header_directory_ns = 0;
    std::uint64_t generation_freeze_build_cache_bind_ns = 0;
    std::uint64_t generation_freeze_build_cache_verify_ns = 0;
    std::uint64_t generation_freeze_build_cache_mapped_baseline_bulk_bytes = 0;
    std::uint64_t generation_freeze_build_cache_mapped_baseline_borrowed_bytes = 0;
    std::uint64_t generation_freeze_build_cache_mapped_baseline_sparse_borrowed_bytes = 0;
    std::uint64_t generation_freeze_build_cache_mapped_baseline_sparse_directory_borrowed_bytes = 0;
    std::uint64_t generation_freeze_build_cache_mapped_baseline_sparse_frontend_borrowed_bytes = 0;
    std::uint64_t generation_freeze_build_cache_mapped_baseline_sparse_frontend_owned_bytes = 0;
    std::uint64_t generation_freeze_build_cache_mapped_baseline_frontend_element_reads = 0;
    std::uint64_t generation_freeze_build_cache_mapped_baseline_frontend_elements_encoded = 0;
    std::uint64_t generation_freeze_build_cache_mapped_baseline_patch_records = 0;
    std::uint64_t generation_freeze_build_cache_mapped_baseline_append_records = 0;
    std::uint32_t generation_freeze_build_cache_mapped_baseline_bulk_sections = 0;
    std::uint32_t generation_freeze_build_cache_mapped_baseline_borrowed_sections = 0;
    std::uint32_t generation_freeze_build_cache_mapped_baseline_sparse_borrowed_extents = 0;
    std::uint32_t generation_freeze_build_cache_mapped_baseline_sparse_directory_borrowed_extents = 0;
    std::uint32_t generation_freeze_build_cache_mapped_baseline_sparse_frontend_borrowed_extents = 0;
    std::uint32_t generation_freeze_build_cache_mapped_baseline_sparse_frontend_owned_extents = 0;
    std::uint64_t generation_freeze_build_cache_provenance_bytes = 0;
    std::uint32_t generation_freeze_build_cache_provenance_sections = 0;
    std::uint64_t generation_freeze_bind_ns = 0;
    std::uint64_t generation_freeze_verify_change_state_ns = 0;
    std::uint64_t generation_freeze_verify_build_cache_ns = 0;

    // D4Q1 observational frozen-Generation ownership audit.
    std::uint64_t generation_freeze_audit_staging_ns = 0;
    std::uint64_t generation_freeze_audit_validation_ns = 0;
    std::uint64_t generation_freeze_audit_unclassified_ns = 0;
    std::uint64_t generation_freeze_audit_compiled_bytes = 0;
    std::uint64_t generation_freeze_audit_source_manager_bytes = 0;
    std::uint64_t generation_freeze_audit_change_state_bytes = 0;
    std::uint64_t generation_freeze_audit_build_cache_bytes = 0;
    std::uint64_t
        generation_freeze_audit_source_manager_baseline_direct_borrow_bytes = 0;
    std::uint32_t
        generation_freeze_audit_source_manager_baseline_direct_borrow_sections = 0;
    std::uint64_t
        generation_freeze_audit_build_cache_baseline_exact_bytes = 0;
    std::uint32_t
        generation_freeze_audit_build_cache_baseline_exact_sections = 0;
    std::uint32_t generation_freeze_audit_compiled_origin = 0;
    std::uint32_t generation_freeze_audit_source_manager_origin = 0;
    std::uint32_t generation_freeze_audit_change_state_origin = 0;
    std::uint32_t generation_freeze_audit_build_cache_origin = 0;

    // baseline_store durable commit boundary.
    std::uint64_t store_commit_ns = 0;
    std::uint64_t transaction_write_ns = 0;
    std::uint64_t transaction_flush_ns = 0;
    std::uint64_t transaction_io_wall_ns = 0;
    std::uint64_t transaction_io_budget_wait_ns = 0;
    std::uint32_t transaction_io_worker_count = 0;
    std::uint32_t transaction_io_budget = 0;
    std::uint32_t transaction_io_peak_active = 0;
    std::uint64_t transaction_compiled_write_ns = 0;
    std::uint64_t transaction_compiled_flush_ns = 0;
    std::uint64_t transaction_build_state_write_ns = 0;
    std::uint64_t transaction_build_state_flush_ns = 0;
    std::uint64_t transaction_source_manager_write_ns = 0;
    std::uint64_t transaction_source_manager_flush_ns = 0;
    std::uint64_t transaction_source_manager_link_ns = 0;
    std::uint64_t transaction_source_manager_compare_ns = 0;
    std::uint64_t transaction_source_manager_compare_bytes = 0;
    std::uint32_t transaction_source_manager_compare_sections = 0;
    std::uint64_t transaction_source_manager_provenance_reused_bytes = 0;
    std::uint32_t transaction_source_manager_provenance_reused_sections = 0;
    std::uint64_t transaction_source_manager_failed_attempt_ns = 0;
    std::uint32_t transaction_source_manager_fallback_reason = 0;
    std::uint32_t transaction_source_manager_hard_link_fallback_sections = 0;
    std::uint64_t transaction_source_manager_io_wall_ns = 0;
    std::uint32_t transaction_source_manager_io_worker_count = 0;
    std::uint64_t transaction_source_manager_directory_flush_ns = 0;
    std::uint64_t transaction_source_manager_written_bytes = 0;
    std::uint64_t transaction_source_manager_reused_bytes = 0;
    std::uint32_t transaction_source_manager_written_sections = 0;
    std::uint32_t transaction_source_manager_reused_sections = 0;
    std::uint32_t transaction_source_manager_sectioned = 0;
    std::uint64_t transaction_build_cache_write_ns = 0;
    std::uint64_t transaction_build_cache_flush_ns = 0;
    std::uint64_t transaction_build_cache_link_ns = 0;
    std::uint64_t transaction_build_cache_compare_ns = 0;
    std::uint64_t transaction_build_cache_compare_bytes = 0;
    std::uint32_t transaction_build_cache_compare_sections = 0;
    std::uint64_t transaction_build_cache_provenance_reused_bytes = 0;
    std::uint32_t transaction_build_cache_provenance_reused_sections = 0;
    std::uint32_t transaction_build_cache_provenance_binding_rejected_sections = 0;

    // D4O2A section audit. Bit N corresponds to Build Cache section N+1.
    // attempt_mask identifies exact-file compares that were executed;
    // reused_mask identifies compares that proved equality and hard-linked.
    std::uint32_t transaction_build_cache_provenance_reused_mask = 0;
    std::uint32_t transaction_build_cache_compare_attempt_mask = 0;
    std::uint32_t transaction_build_cache_compare_reused_mask = 0;

    std::uint64_t transaction_build_cache_failed_attempt_ns = 0;
    std::uint32_t transaction_build_cache_fallback_reason = 0;
    std::uint32_t transaction_build_cache_hard_link_fallback_sections = 0;
    std::uint64_t transaction_build_cache_io_wall_ns = 0;
    std::uint32_t transaction_build_cache_io_worker_count = 0;
    std::uint64_t transaction_build_cache_directory_flush_ns = 0;
    std::uint64_t transaction_build_cache_written_bytes = 0;
    std::uint64_t transaction_build_cache_reused_bytes = 0;
    std::uint32_t transaction_build_cache_written_sections = 0;
    std::uint32_t transaction_build_cache_reused_sections = 0;
    std::uint32_t transaction_build_cache_sectioned = 0;
    std::uint64_t transaction_change_state_write_ns = 0;
    std::uint64_t transaction_change_state_flush_ns = 0;
    std::uint64_t transaction_manifest_write_ns = 0;
    std::uint64_t transaction_manifest_flush_ns = 0;
    std::uint64_t directory_flush_ns = 0;
    std::uint64_t current_write_ns = 0;
    std::uint64_t current_flush_ns = 0;
    std::uint64_t current_replace_ns = 0;
};

struct baseline_commit_result final {
    std::string transaction;
    std::uint64_t bytes_written = 0;
    baseline_commit_telemetry telemetry{};
};

// Persisted project.json fast-path identity. Filesystem metadata is only a
// change token; semantic compatibility remains the baseline fingerprint.
struct baseline_configuration_state final {
    file_snapshot_observation observation{};
    source_content_hash content_hash{};
    file_change_token change_token{};
    std::uint32_t project_version = 0;
    std::uint32_t abi_target = 0;
    std::uint32_t abi_pack = 0;
    bool available = false;
    bool content_hash_available = false;
    bool change_token_available = false;
};

struct baseline_probe final {
    baseline_fingerprint fingerprint{};
    baseline_configuration_state configuration{};
    std::string transaction;
    std::uint64_t source_manager_size = 0;
    std::uint64_t build_cache_size = 0;
};

struct baseline_open_telemetry final {
    std::uint64_t current_read_ns = 0;
    std::uint64_t embedded_manifest_parse_ns = 0;
    std::uint64_t manifest_validation_ns = 0;
    std::uint64_t compiled_map_ns = 0;
    std::uint64_t source_manager_map_ns = 0;
    std::uint64_t change_state_map_ns = 0;
    std::uint64_t build_cache_map_ns = 0;
    std::uint64_t size_validation_ns = 0;
};

// Owns one read-only mapped file. Mapping lifetime pins the backing artifact even
// after a later SAVE commits a different baseline for the next LOAD/BUILD.
class read_only_file_mapping final {
public:
    read_only_file_mapping() noexcept;
    ~read_only_file_mapping() noexcept;

    read_only_file_mapping(const read_only_file_mapping&) = delete;
    read_only_file_mapping& operator=(const read_only_file_mapping&) = delete;
    read_only_file_mapping(read_only_file_mapping&&) noexcept;
    read_only_file_mapping& operator=(read_only_file_mapping&&) noexcept;

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
    [[nodiscard]] bool open() const noexcept;

private:
    [[nodiscard]] status map(const std::filesystem::path& path) noexcept;

    struct state;
    std::unique_ptr<state> value;

    friend class baseline_store;
};

// One committed persisted baseline. READY LOAD may intentionally own only the
// compiled/source-manager mappings; BUILD/SAVE may open all three artifacts.
class baseline_snapshot final {
public:
    baseline_snapshot() noexcept = default;

    baseline_snapshot(const baseline_snapshot&) = delete;
    baseline_snapshot& operator=(const baseline_snapshot&) = delete;
    baseline_snapshot(baseline_snapshot&&) noexcept = default;
    baseline_snapshot& operator=(baseline_snapshot&&) noexcept = default;

    [[nodiscard]] std::span<const std::byte> artifact(
        baseline_artifact_kind kind) const noexcept;

    [[nodiscard]] bool mapped(
        baseline_artifact_kind kind) const noexcept;

    [[nodiscard]] project_generation_segments
    segments() const noexcept;

    // Binds either legacy contiguous/packed Source Manager storage or the
    // immutable sectioned physical backend to the same logical v3 view.
    [[nodiscard]] status bind_source_manager(
        source_manager_image_view& output) const noexcept;

    // Mints a direct-borrow capability only when bytes are exactly one whole
    // physical immutable section mapped by this pinned baseline.
    [[nodiscard]] baseline_section_provenance prove_section_borrow(
        baseline_artifact_kind artifact,
        std::size_t section,
        std::span<const std::byte> bytes) const noexcept;

    [[nodiscard]] bool validate_section_borrow(
        const baseline_section_provenance& proof) const noexcept;

    // Binds either legacy contiguous/packed Build Cache storage or the new
    // immutable sectioned physical backend to the same logical v4 view.
    [[nodiscard]] status bind_build_cache(
        build_cache_image_view& output) const noexcept;

    [[nodiscard]] const baseline_fingerprint& fingerprint() const noexcept {
        return fingerprint_value;
    }

    [[nodiscard]] std::string_view transaction() const noexcept {
        return transaction_value;
    }

    [[nodiscard]] bool valid() const noexcept {
        return !transaction_value.empty();
    }

private:
    baseline_fingerprint fingerprint_value{};
    std::string transaction_value;
    read_only_file_mapping compiled;
    read_only_file_mapping source_manager;

    // D4K sectioned Source Manager physical backend.
    read_only_file_mapping source_manager_prefix;
    std::array<
        read_only_file_mapping,
        10> source_manager_sections;
    bool sectioned_source_manager = false;

    read_only_file_mapping change_state;

    // CURRENT v3 may own the compact BUILD decision gate directly. Canonical
    // change_state.bin remains available for transaction compatibility.
    std::vector<std::byte> embedded_change_state;

    read_only_file_mapping build_cache;

    // Sectioned Build Cache physical backend. The canonical logical v4 header
    // and directory remain in prefix.bin; sections are independent immutable
    // files so unchanged sections can be hard-linked across transactions.
    read_only_file_mapping build_cache_prefix;
    std::array<
        read_only_file_mapping,
        25> build_cache_sections;
    bool sectioned_build_cache = false;

    // New SAVE transactions may physically append the logical Build Cache
    // image to source_manager.bin. Logical artifact spans remain independent.
    std::uint64_t source_manager_size_value = 0;
    std::uint64_t build_cache_size_value = 0;
    bool packed_build_state = false;
    bool packed_build_cache_enabled = false;

    friend class baseline_store;
};

// Transactional persistent-baseline storage. Artifact directories are immutable;
// CURRENT is the only selector changed during commit.
class baseline_store final {
public:
    explicit baseline_store(std::filesystem::path project_configuration_path)
        : configuration_path(std::move(project_configuration_path)) {}

    baseline_store(const baseline_store&) = delete;
    baseline_store& operator=(const baseline_store&) = delete;

    // Reads only the fixed CURRENT selector prefix and manifest identity.
    // The embedded BUILD decision gate and baseline artifacts remain untouched.
    [[nodiscard]] status probe(
        baseline_probe& output) const noexcept;

    // BUILD fast path: reads CURRENT + manifest once, returns the
    // persisted configuration identity, and maps compiled + Source Manager
    // from that same immutable transaction. build_cache stays deferred.
    [[nodiscard]] status open_current_decision(
        baseline_probe& probe,
        baseline_snapshot& output,
        baseline_open_telemetry* telemetry = nullptr) const noexcept;

    // LOAD fast path: reads only the CURRENT selector header + embedded
    // manifest and maps compiled.bin. Source Manager remains lazy.
    [[nodiscard]] status open_current_ready(
        baseline_probe& probe,
        baseline_snapshot& output,
        baseline_open_telemetry* telemetry = nullptr) const noexcept;


    // BUILD/SAVE boundary: maps all three artifacts.
    [[nodiscard]] status open(
        const baseline_fingerprint& expected,
        baseline_snapshot& output) const noexcept;

    // Fast LOAD boundary: maps compiled.bin and source_manager.bin only.
    // The logical Build Cache artifact is not opened and therefore cannot
    // fault on READY LOAD.
    [[nodiscard]] status open_ready(
        const baseline_fingerprint& expected,
        baseline_snapshot& output) const noexcept;

    // Opens one immutable transaction by name. SAVE-after-LOAD uses this to copy
    // the active baseline without rebinding the READY Project to CURRENT.
    [[nodiscard]] status open_transaction(
        const baseline_fingerprint& expected,
        std::string_view transaction,
        baseline_snapshot& output,
        baseline_open_telemetry* telemetry = nullptr) const noexcept;

    // Opens only compiled + Source Manager. build_cache stays unmapped until
    // dirty detection proves that a sparse construction overlay is required.
    [[nodiscard]] status open_transaction_ready(
        const baseline_fingerprint& expected,
        std::string_view transaction,
        baseline_snapshot& output,
        baseline_open_telemetry* telemetry = nullptr) const noexcept;

    [[nodiscard]] status open_transaction_decision(
        const baseline_fingerprint& expected,
        std::string_view transaction,
        baseline_snapshot& output,
        baseline_open_telemetry* telemetry = nullptr) const noexcept;

    [[nodiscard]] status map_source_manager(
        const baseline_fingerprint& expected,
        std::string_view transaction,
        baseline_snapshot& snapshot,
        baseline_open_telemetry* telemetry = nullptr) const noexcept;

    [[nodiscard]] status map_source_manager_cached(
        const baseline_fingerprint& expected,
        std::string_view transaction,
        std::uint64_t expected_source_manager_size,
        baseline_snapshot& snapshot,
        baseline_open_telemetry* telemetry = nullptr) const noexcept;


    [[nodiscard]] status map_build_cache(
        const baseline_fingerprint& expected,
        std::string_view transaction,
        baseline_snapshot& snapshot,
        baseline_open_telemetry* telemetry = nullptr) const noexcept;

    // Fused BUILD path: manifest identity and artifact size were already
    // validated while opening CURRENT. This maps only the immutable cache file.
    [[nodiscard]] status map_build_cache_cached(
        const baseline_fingerprint& expected,
        std::string_view transaction,
        std::uint64_t expected_build_cache_size,
        baseline_snapshot& snapshot,
        baseline_open_telemetry* telemetry = nullptr) const noexcept;

    [[nodiscard]] status commit(
        const baseline_fingerprint& fingerprint,
        const baseline_configuration_state& configuration,
        project_generation_segments generation,
        baseline_commit_result& output) const noexcept;

    [[nodiscard]] status commit(
        const baseline_fingerprint& fingerprint,
        const baseline_configuration_state& configuration,
        project_generation_segments generation,
        const baseline_commit_provenance& provenance,
        baseline_commit_result& output,
        std::size_t io_worker_budget = 0) const noexcept;

    [[nodiscard]] status commit(
        const baseline_fingerprint& fingerprint,
        const baseline_configuration_state& configuration,
        std::span<const std::byte> compiled,
        std::span<const std::byte> source_manager,
        std::span<const std::byte> change_state,
        std::span<const std::byte> build_cache,
        baseline_commit_result& output) const noexcept;

    [[nodiscard]] status commit(
        const baseline_fingerprint& fingerprint,
        const baseline_configuration_state& configuration,
        std::span<const std::byte> compiled,
        std::span<const std::byte> source_manager,
        std::span<const std::byte> build_cache,
        baseline_commit_result& output) const noexcept;

    // Compatibility boundary for low-level persistence callers. This overload
    // intentionally commits without a fast project.json identity.
    [[nodiscard]] status commit(
        const baseline_fingerprint& fingerprint,
        std::span<const std::byte> compiled,
        std::span<const std::byte> source_manager,
        std::span<const std::byte> build_cache,
        baseline_commit_result& output) const noexcept;

    // Cold maintenance boundary. The current transaction and the optional pinned
    // transaction are retained; all other tx-* directories are reclaimable.
    [[nodiscard]] status collect_garbage(
        std::string_view pinned_transaction = {}) const noexcept;

private:
    [[nodiscard]] status open_selected(
        const baseline_fingerprint& expected,
        std::string_view transaction,
        bool include_source_manager,
        bool include_change_state,
        bool include_build_cache,
        baseline_snapshot& output,
        baseline_open_telemetry* telemetry) const noexcept;

    [[nodiscard]] status map_source_manager_artifact(
        const std::filesystem::path& directory,
        std::uint64_t expected_size,
        baseline_snapshot& snapshot,
        baseline_open_telemetry* telemetry) const noexcept;

    [[nodiscard]] status map_build_cache_artifact(
        const std::filesystem::path& directory,
        std::uint64_t expected_size,
        baseline_snapshot& snapshot,
        baseline_open_telemetry* telemetry) const noexcept;

    [[nodiscard]] std::filesystem::path root_path() const;

    std::filesystem::path configuration_path;
};

} // namespace cw::server
