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


// Read-only construction Generation view. Fresh G0 exposes dense native arrays;
// baseline-backed Gn remains sparse and therefore reports complete=false.
struct source_manager_native_generation_view final {
    std::span<const source_record> sources;
    std::span<const source_generation_physical_record> physical;
    std::span<const source_generation_record> graph;
    std::span<const source_id> forward_edges;
    std::span<const source_id> reverse_edges;
    std::span<const std::byte> path_index;
    std::span<const std::byte> path_bytes;
    bool complete = false;
};

// R5E4-B2: one immutable page of Source snapshot bytes. Text pages are
// persistence carriers; moving this owner never changes byte addresses.
struct source_snapshot_page final {
    std::unique_ptr<char[]> bytes;
    std::size_t capacity = 0;
    std::size_t used = 0;

    [[nodiscard]] std::span<const std::byte> view() const noexcept {
        return {
            reinterpret_cast<const std::byte*>(bytes.get()),
            used,
        };
    }
};

// Lifetime owner transferred from construction Source Manager to Generation.
// Only text pages move; path pages remain construction-only and die with the
// Source Manager after READY publication.
struct source_snapshot_generation_storage final {
    std::vector<source_snapshot_page> text_pages;
    std::size_t text_bytes = 0;
    std::size_t source_count = 0;
    bool complete = false;

    [[nodiscard]] bool valid() const noexcept {
        return complete &&
            source_count != 0 &&
            (text_bytes == 0 || !text_pages.empty());
    }

    [[nodiscard]] std::size_t page_count() const noexcept {
        return text_pages.size();
    }

    [[nodiscard]] std::span<const std::byte> page(
        std::size_t index) const noexcept {

        return index < text_pages.size()
            ? text_pages[index].view()
            : std::span<const std::byte>{};
    }
};

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

    // R5E3-C1: acquired Source path/text bytes are page-owned by the Source
    // Manager. Per-Source snapshots retain only borrowed immutable views.
    std::uint64_t snapshot_pages = 0;
    std::uint64_t snapshot_reserved_bytes = 0;
    bool snapshot_page_backed = false;
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

    // Total persisted Source text bytes for the current Generation. Maintained
    // by Source publication deltas so SAVE never rescans every Source to size
    // Build Cache source_bytes.
    [[nodiscard]] std::uint64_t persistence_text_bytes() const noexcept {
        return persistence_text_bytes_value;
    }

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

    // Internal persistence boundary used only by cold Generation materialization.
    // The returned view is pinned by the owning Project baseline snapshot.
    [[nodiscard]] const source_manager_image_view*
    baseline_source_image() const noexcept {
        return baseline_sources;
    }

    [[nodiscard]] const source_generation_storage&
    generation_storage() const noexcept {
        return generation_storage_value;
    }

    [[nodiscard]] source_manager_native_generation_view
    native_generation() const noexcept;

    // Fresh-G0 Build Cache source_bytes may borrow these pages directly.
    [[nodiscard]] bool native_snapshot_text_complete() const noexcept;
    [[nodiscard]] std::size_t native_snapshot_text_page_count() const noexcept;
    [[nodiscard]] std::span<const std::byte> native_snapshot_text_page(
        std::size_t index) const noexcept;

    [[nodiscard]] status release_snapshot_generation_storage(
        source_snapshot_generation_storage& output) noexcept;

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

    // Transaction-friendly page owner for immutable Source snapshot bytes.
    // Update-local pages move into the committed construction Source Manager;
    // string_view addresses never change during the ownership transfer.
    class snapshot_page_store final {
    public:
        snapshot_page_store() = default;

        snapshot_page_store(
            const snapshot_page_store&) = delete;
        snapshot_page_store& operator=(
            const snapshot_page_store&) = delete;
        snapshot_page_store(
            snapshot_page_store&&) noexcept = default;
        snapshot_page_store& operator=(
            snapshot_page_store&&) noexcept = default;

        [[nodiscard]] status append(
            source_id source,
            std::string_view path,
            std::string_view text,
            std::string_view& path_view,
            std::string_view& text_view) noexcept;

        [[nodiscard]] status prepare_absorb(
            const snapshot_page_store& other) noexcept;

        void absorb_prepared(
            snapshot_page_store&& other) noexcept;

        [[nodiscard]] std::size_t page_count() const noexcept {
            return path_pages.size() + text_pages.size();
        }

        [[nodiscard]] std::size_t reserved_bytes() const noexcept;

        [[nodiscard]] bool complete_text_generation(
            std::size_t source_count,
            std::uint64_t text_bytes) const noexcept;

        [[nodiscard]] std::size_t text_page_count() const noexcept {
            return text_pages.size();
        }

        [[nodiscard]] std::span<const std::byte> text_page(
            std::size_t index) const noexcept {
            return index < text_pages.size()
                ? text_pages[index].view()
                : std::span<const std::byte>{};
        }

        [[nodiscard]] status release_text_generation(
            source_snapshot_generation_storage& output) noexcept;

    private:
        using page = source_snapshot_page;

        [[nodiscard]] static status append_bytes(
            std::vector<page>& pages,
            std::string_view value,
            std::string_view& output) noexcept;

        [[nodiscard]] static std::size_t next_page_capacity(
            const std::vector<page>& pages,
            std::size_t required) noexcept;

        std::vector<page> path_pages;
        std::vector<page> text_pages;

        source_id first_text_source{};
        source_id last_text_source{};
        std::size_t text_source_count = 0;
        std::size_t text_bytes_value = 0;
        bool text_source_order_contiguous = true;
    };

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

    // Construction-only Source bytes. Canonical persisted Source bytes already
    // belong to the committed Generation after finalization.
    snapshot_page_store snapshot_storage;

    std::vector<path_slot> path_index;
    std::uint64_t persistence_text_bytes_value = 0;
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

    // Every non-unchanged acquisition, including an identity-only replacement
    // with identical content. Generation provenance must use this set rather
    // than the semantic-change set.
    [[nodiscard]] std::span<const source_id> physical_changes() const noexcept {
        return physical_changes_value;
    }

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
    std::vector<source_id> physical_changes_value;

    // Candidate snapshot pages are transaction-owned until publish_prepared().
    // Failed updates release only their own pages.
    source_manager::snapshot_page_store candidate_snapshots;

    mutable source_manager_update_telemetry telemetry_value{};
    std::size_t prepared_path_storage_size = 0;
    std::uint64_t prepared_text_bytes = 0;
    bool prepared = false;
    bool committed = false;
};

} // namespace cw::server
