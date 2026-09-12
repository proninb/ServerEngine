#pragma once

#include "../../source_id.hpp"
#include "../../status.hpp"
#include <cstdint>
#include <vector>

namespace cw::server {

class build_cache_image_view;
class source_manager;
class source_manager_image_view;

enum class source_change_backend : std::uint32_t {
    none = 0,
    windows_usn = 1,
};

struct source_change_checkpoint final {
    source_change_backend backend = source_change_backend::none;
    std::uint64_t volume_serial = 0;
    std::uint64_t journal_id = 0;
    std::int64_t next_usn = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return backend != source_change_backend::none;
    }
};

struct source_change_file_index_slot final {
    std::uint64_t file_reference = 0;
    source_id source{};
    std::uint32_t reserved = 0;
};

static_assert(sizeof(source_change_file_index_slot) == 16);

inline constexpr std::uint32_t source_change_directory_watch_topology = 0x01u;
inline constexpr std::uint32_t source_change_directory_watch_arrival = 0x02u;
inline constexpr std::uint32_t source_change_directory_watch_known =
    source_change_directory_watch_topology |
    source_change_directory_watch_arrival;

struct source_change_directory_index_slot final {
    std::uint64_t file_reference = 0;
    std::uint32_t flags = 0;
    std::uint32_t reserved = 0;
};

static_assert(sizeof(source_change_directory_index_slot) == 16);

struct source_change_capture final {
    source_change_checkpoint checkpoint{};
    std::vector<source_change_file_index_slot> file_index;
    std::vector<source_change_directory_index_slot> directory_index;

    void reset() noexcept {
        checkpoint = {};
        file_index.clear();
        directory_index.clear();
    }
};

struct source_change_detection_telemetry final {
    source_change_backend backend = source_change_backend::none;
    std::uint64_t journal_records = 0;
    std::uint64_t matched_sources = 0;
    bool fast_path = false;
    bool fallback = false;
};

// SAVE-only preparation. The checkpoint is captured before filesystem validation;
// any change after the checkpoint remains visible to the next BUILD journal query.
[[nodiscard]] status prepare_source_change_capture(
    const source_manager& sources,
    source_change_capture& output) noexcept;

// BUILD-only journal path. not_found means the caller must use the portable full
// filesystem scan. Allocation failures remain hard failures.
[[nodiscard]] status detect_source_changes(
    const source_manager_image_view& sources,
    std::vector<source_id>& dirty_sources,
    source_change_detection_telemetry& telemetry) noexcept;

} // namespace cw::server
