#pragma once

#include "../project_root.hpp"
#include "../project_generation_segments.hpp"
#include "../source/source_change_tracker.hpp"
#include "../source/source_manager.hpp"

#include <cstddef>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace cw::server {

inline constexpr std::uint32_t source_manager_image_format_version = 3;
inline constexpr std::size_t source_manager_image_header_size = 160;
inline constexpr std::size_t source_manager_image_directory_count = 10;
inline constexpr std::size_t source_manager_image_directory_entry_size = 32;

inline constexpr std::size_t source_manager_image_prefix_size =
    (source_manager_image_header_size +
     source_manager_image_directory_count *
         source_manager_image_directory_entry_size +
     63u) &
    ~std::size_t{63u};

enum class source_manager_image_section : std::uint32_t {
    source_core = 1,
    physical_state = 2,
    graph_records = 3,
    forward_edges = 4,
    reverse_edges = 5,
    roots = 6,
    path_index = 7,
    path_bytes = 8,
    source_file_identity_index = 9,
    tracked_directory_identity_index = 10,
};

struct source_manager_image_root final {
    source_id source{};
    project_item_role role = project_item_role::type;
};

struct source_manager_image_options final {
    std::uint64_t generation = 0;
    std::span<const source_manager_image_root> roots;
    source_change_checkpoint change_checkpoint{};
    std::span<const source_change_file_index_slot> file_identity_index;
    std::span<const source_change_directory_index_slot> directory_identity_index;
};

struct source_manager_image_physical_state final {
    bool present = false;
    std::int64_t write_time_ticks = 0;
    std::uint64_t size = 0;
    source_content_hash hash{};
};

// Owns only the metadata that cannot be borrowed from the committed Source
// Generation. The large Source arrays remain zero-copy spans into Source Manager
// storage; segment() assembles the durable file as scatter/gather extents.
class source_manager_native_image_storage final {
public:
    source_manager_native_image_storage() noexcept = default;

    void reset() noexcept;

    [[nodiscard]] bool valid() const noexcept {
        return valid_value;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_value;
    }

    [[nodiscard]] project_generation_segment
    segment() const noexcept;

private:
    std::array<
        std::byte,
        source_manager_image_prefix_size> prefix{};
    std::vector<std::byte> roots;
    source_manager_native_generation_view native;
    std::span<const source_change_file_index_slot>
        file_identity_index;
    std::span<const source_change_directory_index_slot>
        directory_identity_index;
    std::size_t size_value = 0;
    bool valid_value = false;

    friend status freeze_source_manager_native_image(
        const source_manager&,
        const source_manager_image_options&,
        source_manager_native_image_storage&) noexcept;
};

// Builds only Source image metadata and CRCs. Source records, physical state,
// graph records, edge arenas, path index/bytes, and change indexes are borrowed
// directly from immutable Generation storage; no O(N) serialization copy occurs.
[[nodiscard]] status freeze_source_manager_native_image(
    const source_manager& manager,
    const source_manager_image_options& options,
    source_manager_native_image_storage& output) noexcept;

// Read-only sequence of source_id values encoded as little-endian uint32_t.
// It borrows the mapped source_manager.bin image and performs no allocation.
class source_id_image_range final {
public:
    source_id_image_range() noexcept = default;

    [[nodiscard]] std::size_t size() const noexcept { return count; }
    [[nodiscard]] bool empty() const noexcept { return count == 0; }
    [[nodiscard]] source_id operator[](std::size_t index) const noexcept;

private:
    source_id_image_range(const std::byte* bytes_value, std::size_t count_value) noexcept
        : bytes(bytes_value), count(count_value) {}

    const std::byte* bytes = nullptr;
    std::size_t count = 0;

    friend class source_manager_image_view;
};

// Mmap-native Source Manager baseline view. bind() validates only structural
// metadata needed for safe direct access. verify_contents() is an explicit
// cold full-image integrity pass and is never required for fast LOAD.
class source_manager_image_view final {
public:
    source_manager_image_view() noexcept = default;

    [[nodiscard]] status bind(std::span<const std::byte> image) noexcept;
    void reset() noexcept;

    [[nodiscard]] bool valid() const noexcept { return bytes.data() != nullptr; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_value; }
    [[nodiscard]] std::size_t source_count() const noexcept { return source_count_value; }
    [[nodiscard]] std::size_t root_count() const noexcept { return root_count_value; }

    [[nodiscard]] std::string_view path(source_id source) const noexcept;
    [[nodiscard]] status physical(
        source_id source,
        source_manager_image_physical_state& output) const noexcept;

    [[nodiscard]] source_id_image_range includes(source_id source) const noexcept;
    [[nodiscard]] source_id_image_range dependents(source_id source) const noexcept;

    [[nodiscard]] status root(
        std::size_t index,
        source_manager_image_root& output) const noexcept;

    // Hot lookup boundary for an already-normalized path. It probes the persisted
    // open-addressed path index directly and performs no filesystem work,
    // normalization, sorting, or allocation.
    [[nodiscard]] status find(
        std::string_view normalized_path,
        source_id& output) const noexcept;

    [[nodiscard]] source_change_checkpoint change_checkpoint() const noexcept {
        return change_checkpoint_value;
    }

    [[nodiscard]] source_id find_source_file(
        std::uint64_t file_reference) const noexcept;

    // Cold persistence enumeration. These expose the already-mapped exact
    // identity tables without filesystem access so SAVE can merge a sparse
    // Generation overlay into a new durable baseline.
    [[nodiscard]] std::size_t
    source_file_identity_slot_count() const noexcept;

    [[nodiscard]] status source_file_identity_slot(
        std::size_t index,
        source_change_file_index_slot& output) const noexcept;

    [[nodiscard]] std::size_t
    tracked_directory_identity_slot_count() const noexcept;

    [[nodiscard]] status tracked_directory_identity_slot(
        std::size_t index,
        source_change_directory_index_slot& output) const noexcept;

    [[nodiscard]] std::uint32_t directory_watch_flags(
        std::uint64_t file_reference) const noexcept;

    // Explicit maintenance/diagnostic integrity pass. This reads every section.
    [[nodiscard]] status verify_contents() const noexcept;

private:
    struct section_view final {
        const std::byte* data = nullptr;
        std::uint64_t count = 0;
        std::uint32_t record_size = 0;
        std::uint64_t crc64 = 0;
    };

    [[nodiscard]] const section_view& section(source_manager_image_section kind) const noexcept;
    [[nodiscard]] bool valid_source(source_id source) const noexcept {
        return source && static_cast<std::size_t>(source.value()) <= source_count_value;
    }

    std::span<const std::byte> bytes;
    section_view sections[source_manager_image_directory_count]{};
    std::uint64_t generation_value = 0;
    std::size_t source_count_value = 0;
    std::size_t root_count_value = 0;
    source_change_checkpoint change_checkpoint_value{};
};

// Deterministic field-wise little-endian encoder for source_manager.bin v3.
// Sources are emitted strictly in dense source_id order; roots remain in caller
// order; no sorting and no runtime hash-table traversal is used.
[[nodiscard]] status encode_source_manager_image(
    const source_manager& manager,
    const source_manager_image_options& options,
    std::vector<std::byte>& output) noexcept;

} // namespace cw::server
