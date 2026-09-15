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
    std::uint64_t generation_freeze_bind_ns = 0;
    std::uint64_t generation_freeze_verify_change_state_ns = 0;
    std::uint64_t generation_freeze_verify_build_cache_ns = 0;

    // baseline_store durable commit boundary.
    std::uint64_t store_commit_ns = 0;
    std::uint64_t transaction_write_ns = 0;
    std::uint64_t transaction_flush_ns = 0;
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
    read_only_file_mapping change_state;

    // CURRENT v3 may own the compact BUILD decision gate directly. Canonical
    // change_state.bin remains available for transaction compatibility.
    std::vector<std::byte> embedded_change_state;

    read_only_file_mapping build_cache;

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

    [[nodiscard]] std::filesystem::path root_path() const;

    std::filesystem::path configuration_path;
};

} // namespace cw::server
