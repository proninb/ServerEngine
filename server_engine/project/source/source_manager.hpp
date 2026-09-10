#pragma once

#include "source_snapshot.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"
#include "../../operation.hpp"
#include "../../status.hpp"

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

// Converts an external filesystem path into the canonical Source Manager spelling.
// Callers that repeatedly resolve the same path should normalize once and use the
// normalized lookup boundary instead of repeating filesystem/path construction.
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

// Immutable Source-local filesystem job. Worker threads may execute it without
// accessing Source Manager or transaction-owned mutable state.
struct source_acquire_job final {
    source_id source{};
    std::filesystem::path path;
    std::optional<file_snapshot_observation> baseline;
};

// Worker-produced acquisition result. Bytes remain detached until the coordinator
// applies the result to one source_manager_update candidate.
struct source_acquire_result final {
    source_id source{};
    source_acquire_result_kind kind = source_acquire_result_kind::unchanged;
    file_snapshot snapshot{};
};

class source_manager_update;

// Owns stable normalized-path -> source_id identity and committed immutable Source
// revisions/dependencies. Path identity uses a compact open-addressed hot index and
// contiguous path arena; filesystem normalization is outside the hot lookup path.
class source_manager final {
public:
    source_manager() = default;

    source_manager(const source_manager&) = delete;
    source_manager& operator=(const source_manager&) = delete;

    [[nodiscard]] source_manager_update begin_update() noexcept;

    [[nodiscard]] std::size_t source_count() const noexcept { return records.size(); }
    [[nodiscard]] source_snapshot current(source_id source) const noexcept;
    [[nodiscard]] std::span<const source_id> includes(source_id source) const noexcept;

    // Returned view remains valid until the next Source Manager publication.
    [[nodiscard]] std::string_view path(source_id source) const noexcept;

    // Hot lookup boundary. normalized_path must already be produced by
    // normalize_source_path() or come directly from Source Manager storage.
    [[nodiscard]] status find(
        std::string_view normalized_path,
        source_id& output) const noexcept;

    [[nodiscard]] std::size_t path_index_bytes() const noexcept {
        return path_index.size() * sizeof(path_slot);
    }

    [[nodiscard]] std::size_t path_storage_bytes() const noexcept {
        return path_storage.size();
    }

    // Compatibility/test convenience: one transactional in-memory publication.
    [[nodiscard]] status publish_memory(
        std::string_view normalized_path,
        std::string_view text,
        source_snapshot& output,
        source_id* identity = nullptr) noexcept;

private:
    struct committed_source final {
        source_snapshot snapshot;
        std::vector<source_id> includes;
    };

    // fingerprint is a folded XXH64 value. Full path comparison resolves the rare
    // fingerprint collision; source==0 is the only empty bucket state.
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

    std::vector<source_record> records;
    std::vector<char> path_storage;
    std::vector<committed_source> states;
    std::vector<path_slot> path_index;
};

// Isolated candidate Source Manager mutation. It discovers stable Source identities,
// owns changed snapshots/include edges, validates the candidate DAG, then publishes
// only after all allocation-sensitive preparation has succeeded.
class source_manager_update final {
public:
    source_manager_update() noexcept = default;
    ~source_manager_update() = default;

    source_manager_update(const source_manager_update&) = delete;
    source_manager_update& operator=(const source_manager_update&) = delete;
    source_manager_update(source_manager_update&&) noexcept = default;
    source_manager_update& operator=(source_manager_update&&) noexcept = default;

    // Cold external boundary: normalizes path then delegates to resolve_normalized().
    [[nodiscard]] status resolve(
        const std::filesystem::path& path,
        source_id& output) noexcept;

    // Hot path identity boundary. No filesystem operations, normalization, sorting,
    // or allocation occur for an already-known Source.
    [[nodiscard]] status resolve_normalized(
        std::string_view normalized_path,
        source_id& output) noexcept;

    [[nodiscard]] status resolve_include(
        source_id including_source,
        std::string_view relative_path,
        source_id& output) noexcept;

    [[nodiscard]] status prepare_acquire(source_id source, source_acquire_job& output) const noexcept;

    [[nodiscard]] static status execute_acquire(
        const source_acquire_job& job,
        source_acquire_result& output) noexcept;

    [[nodiscard]] status apply_acquire(source_acquire_result&& result) noexcept;

    [[nodiscard]] status set_includes(
        source_id source,
        std::span<const source_id> dependencies) noexcept;

    [[nodiscard]] source_snapshot snapshot(source_id source) const noexcept;
    [[nodiscard]] std::span<const source_id> includes(source_id source) const noexcept;
    [[nodiscard]] std::string_view path(source_id source) const noexcept;
    [[nodiscard]] std::size_t source_count() const noexcept;

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
        bool has_snapshot = false;
        bool has_includes = false;
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
    [[nodiscard]] bool valid_source(source_id source) const noexcept;

    source_manager* owner = nullptr;
    std::vector<new_source> new_sources;
    std::vector<candidate_source> candidates;
    std::vector<id_slot> id_index;
    std::vector<local_path_slot> local_path_index;
    std::vector<source_manager::path_slot> prepared_path_index;
    std::size_t prepared_path_storage_size = 0;
    bool prepared = false;
    bool committed = false;
};

} // namespace cw::server
