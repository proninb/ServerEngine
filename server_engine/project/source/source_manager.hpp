#pragma once

#include "source_snapshot.hpp"
#include "source_generation_storage.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"
#include "../../operation.hpp"
#include "../../status.hpp"
#include "../storage/mapped_vector.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cw::server {

class build_cache_image_view;
class source_manager_image_view;

[[nodiscard]] status normalize_source_path(
    const std::filesystem::path& input,
    std::string& output) noexcept;

struct source_record final {
    std::uint32_t path_offset = 0;
    std::uint32_t path_length = 0;
};

static_assert(sizeof(source_record) == 8);

enum class source_acquire_result_kind : std::uint8_t {
    unchanged,
    missing,
    present,
};

struct source_acquire_job final {
    source_id source{};
    std::filesystem::path path;
    std::optional<file_snapshot_observation> baseline;
};

struct source_acquire_result final {
    source_id source{};
    source_acquire_result_kind kind = source_acquire_result_kind::unchanged;
    file_snapshot snapshot{};
};

struct source_manager_update_telemetry final {
    std::uint64_t path_index_full_rebuilds = 0;
    std::uint64_t source_graph_full_scans = 0;
    std::uint64_t source_graph_visited = 0;
    std::uint64_t reverse_edge_patches = 0;
};

class source_manager_update;

// Owns stable Source identity. Persisted Sources remain in the logical Source
// Manager / Build Cache images and are decoded only when an affected BUILD path
// touches them.
class source_manager final {
public:
    source_manager() = default;
    source_manager(
        const source_manager_image_view& baseline_sources_value,
        const build_cache_image_view& baseline_cache_value) noexcept;

    source_manager(const source_manager&) = delete;
    source_manager& operator=(const source_manager&) = delete;

    [[nodiscard]] source_manager_update begin_update() noexcept;

    [[nodiscard]] std::size_t source_count() const noexcept { return records.size(); }
    [[nodiscard]] source_snapshot current(source_id source) const noexcept;
    [[nodiscard]] std::span<const source_id> includes(source_id source) const noexcept;
    [[nodiscard]] std::span<const source_id> dependents(source_id source) const noexcept;

    // Allocation-free persistence access. Untouched baseline edges are read
    // directly from source_manager.bin; sparse patches override them locally.
    [[nodiscard]] std::size_t include_count(source_id source) const noexcept;
    [[nodiscard]] source_id include_at(source_id source, std::size_t index) const noexcept;
    [[nodiscard]] std::size_t dependent_count(source_id source) const noexcept;
    [[nodiscard]] source_id dependent_at(source_id source, std::size_t index) const noexcept;

    [[nodiscard]] status collect_dependents(
        source_id source,
        std::vector<source_id>& output) const noexcept;

    [[nodiscard]] std::string_view path(source_id source) const noexcept;

    [[nodiscard]] status find(
        std::string_view normalized_path,
        source_id& output) const noexcept;

    [[nodiscard]] std::size_t path_index_bytes() const noexcept {
        return path_index.size() * sizeof(path_slot);
    }

    [[nodiscard]] std::size_t path_storage_bytes() const noexcept {
        return path_storage.size();
    }

    [[nodiscard]] bool baseline_backed() const noexcept {
        return baseline_sources != nullptr;
    }

    [[nodiscard]] const source_generation_storage&
    generation_storage() const noexcept {
        return generation_storage_value;
    }

    [[nodiscard]] status publish_memory(
        std::string_view normalized_path,
        std::string_view text,
        source_snapshot& output,
        source_id* identity = nullptr) noexcept;

private:
    struct committed_source final {
        source_snapshot snapshot;

        // Compatibility cache for mmap baseline edges only. Fresh G0 and all
        // published replacements live in source_generation_storage arenas.
        std::vector<source_id> baseline_includes;
        std::vector<source_id> baseline_dependents;
    };

    struct path_slot final {
        std::uint32_t fingerprint = 0;
        source_id source{};
    };

    static_assert(sizeof(path_slot) == 8);

    friend class source_manager_update;

    [[nodiscard]] status rebuild_path_index(
        std::size_t additional,
        std::vector<path_slot>& output) const noexcept;

    [[nodiscard]] status find_in_index(
        std::string_view normalized_path,
        std::span<const path_slot> index,
        source_id& output) const noexcept;

    [[nodiscard]] static status read_baseline_record(
        const void* context,
        std::size_t index,
        source_record& output) noexcept;

    [[nodiscard]] static status read_baseline_state(
        const void* context,
        std::size_t index,
        committed_source& output) noexcept;

    const source_manager_image_view* baseline_sources = nullptr;
    const build_cache_image_view* baseline_cache = nullptr;
    std::size_t baseline_source_count = 0;

    mapped_vector<source_record> records;
    std::vector<char> path_storage;
    mapped_vector<committed_source> states;
    source_generation_storage generation_storage_value;
    std::vector<path_slot> path_index;
};

class source_manager_update final {
public:
    source_manager_update() noexcept = default;
    ~source_manager_update() = default;

    source_manager_update(const source_manager_update&) = delete;
    source_manager_update& operator=(const source_manager_update&) = delete;
    source_manager_update(source_manager_update&&) noexcept = default;
    source_manager_update& operator=(source_manager_update&&) noexcept = default;

    [[nodiscard]] status resolve(
        const std::filesystem::path& path,
        source_id& output) noexcept;

    // Fast root path for configuration entries already resolved to an absolute,
    // lexically-normal path by the validated project configuration loader.
    [[nodiscard]] status resolve_canonical(
        const std::filesystem::path& path,
        source_id& output) noexcept;

    // Prepares fresh Source identity storage for a known root batch. This changes
    // capacity only; source_id allocation order remains first-resolution order.
    [[nodiscard]] status reserve_sources(
        std::size_t additional) noexcept;

    [[nodiscard]] status resolve_normalized(
        std::string_view normalized_path,
        source_id& output) noexcept;

    [[nodiscard]] status resolve_include(
        source_id including_source,
        std::string_view relative_path,
        source_id& output) noexcept;

    [[nodiscard]] status prepare_acquire(
        source_id source,
        source_acquire_job& output) const noexcept;

    [[nodiscard]] static status execute_acquire(
        const source_acquire_job& job,
        source_acquire_result& output) noexcept;

    [[nodiscard]] status apply_acquire(source_acquire_result&& result) noexcept;

    [[nodiscard]] status set_includes(
        source_id source,
        std::span<const source_id> dependencies) noexcept;

    [[nodiscard]] source_snapshot snapshot(source_id source) const noexcept;
    [[nodiscard]] std::span<const source_id> includes(source_id source) const noexcept;
    [[nodiscard]] std::span<const source_id> dependents(source_id source) const noexcept;
    [[nodiscard]] std::span<const source_id> changed_sources() const noexcept { return semantic_changes; }

    [[nodiscard]] status collect_dependents(
        source_id source,
        std::vector<source_id>& output) const noexcept;

    [[nodiscard]] status validate_changed_source_graph(
        std::span<const source_id> changed,
        operation_id operation,
        diagnostic_buffer& diagnostics,
        std::size_t* visited_sources = nullptr) const noexcept;

    [[nodiscard]] std::string_view path(source_id source) const noexcept;
    [[nodiscard]] std::size_t source_count() const noexcept;
    [[nodiscard]] const source_manager_update_telemetry& telemetry() const noexcept {
        return telemetry_value;
    }

    [[nodiscard]] status validate_source_graph(
        operation_id operation,
        diagnostic_buffer& diagnostics) const noexcept;

    [[nodiscard]] status prepare_publish() noexcept;
    void publish_prepared() noexcept;
    [[nodiscard]] status commit() noexcept;

private:
    struct candidate_source final {
        source_id source{};
        source_snapshot snapshot;
        std::vector<source_id> includes;
        std::vector<source_id> dependents;
        bool has_snapshot = false;
        bool has_includes = false;
        bool has_dependents = false;
    };

    struct new_source final {
        source_id source{};
        std::string normalized_path;
    };

    struct id_slot final {
        source_id source{};
        std::uint32_t candidate_index = 0;
    };

    struct local_path_slot final {
        std::uint32_t fingerprint = 0;
        std::uint32_t new_source_index = 0;
    };

    struct prepared_path_insertion final {
        std::size_t position = 0;
        source_manager::path_slot slot{};
    };

    static_assert(sizeof(local_path_slot) == 8);

    friend class source_manager;
    explicit source_manager_update(source_manager& owner_value) noexcept : owner(&owner_value) {}

    [[nodiscard]] candidate_source* candidate(source_id source) noexcept;
    [[nodiscard]] const candidate_source* candidate(source_id source) const noexcept;
    [[nodiscard]] status touch(source_id source, candidate_source*& output) noexcept;
    [[nodiscard]] status ensure_id_index_capacity(std::size_t required) noexcept;
    [[nodiscard]] status ensure_local_path_capacity(std::size_t required) noexcept;
    [[nodiscard]] status find_local_path(std::string_view normalized, source_id& output) const noexcept;
    [[nodiscard]] status insert_local_path(std::uint32_t new_source_index) noexcept;
    [[nodiscard]] status build_prepared_path_index() noexcept;
    [[nodiscard]] status build_sparse_path_insertions() noexcept;
    [[nodiscard]] bool valid_source(source_id source) const noexcept;
    [[nodiscard]] status prepare_dependent_patches() noexcept;

    source_manager* owner = nullptr;
    std::vector<new_source> new_sources;
    std::vector<candidate_source> candidates;
    std::vector<id_slot> id_index;
    std::vector<local_path_slot> local_path_index;
    std::vector<source_manager::path_slot> prepared_path_index;
    std::vector<prepared_path_insertion> prepared_path_insertions;
    std::vector<source_id> semantic_changes;
    mutable source_manager_update_telemetry telemetry_value{};
    std::size_t prepared_path_storage_size = 0;
    bool prepared = false;
    bool committed = false;
};

} // namespace cw::server
