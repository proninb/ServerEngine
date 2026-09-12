#pragma once

#include "../../source_id.hpp"
#include "../../status.hpp"
#include <cstdint>
#include <string>
#include <filesystem>
#include <span>
#include <vector>

namespace cw::server {

class build_cache_image_view;
class change_state_image_view;
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

struct file_change_token final {
    std::uint64_t volume_serial = 0;
    std::uint64_t file_reference = 0;
    std::int64_t file_usn = -1;

    [[nodiscard]] explicit operator bool() const noexcept {
        return volume_serial != 0 &&
            file_reference != 0 &&
            file_usn >= 0;
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
    std::string journal_anchor_path;
    std::vector<source_change_file_index_slot> file_index;
    std::vector<source_change_directory_index_slot> directory_index;

    void reset() noexcept {
        checkpoint = {};
        journal_anchor_path.clear();
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

struct source_change_journal_candidate final {
    std::uint64_t file_reference = 0;
    std::uint64_t parent_file_reference = 0;
    std::uint32_t reason = 0;
    std::uint32_t file_attributes = 0;
};

static_assert(sizeof(source_change_journal_candidate) == 24);

// Owned compact BUILD decision state loaded by targeted reads from the persisted
// Source Manager image. It intentionally owns only the USN checkpoint, the two
// probabilistic identity gates, and one path used to resolve the filesystem volume.
// Captures one file identity plus its current per-file USN. The token may be
// persisted only if prove_file_unchanged() succeeds after the caller's stable
// content read. not_found means this platform/filesystem cannot provide proof.
[[nodiscard]] status capture_file_change_token(
    const std::filesystem::path& path,
    file_change_token& output) noexcept;

// Proves O(1) that the same file identity still has the same per-file USN.
// not_found requires the caller's content-hash fallback.
[[nodiscard]] status prove_file_unchanged(
    const std::filesystem::path& path,
    const file_change_token& token,
    bool& unchanged,
    std::uint64_t* journal_records = nullptr) noexcept;

// SAVE-only preparation. The checkpoint is captured before filesystem validation;
// any change after the checkpoint remains visible to the next BUILD journal query.
[[nodiscard]] status prepare_source_change_capture(
    const source_manager& sources,
    source_change_capture& output) noexcept;

// Fast-path pathname proof. Returns success with same=false when the pathname
// no longer resolves to the persisted file identity. Unsupported backends
// return not_found so the caller falls back to content validation.
[[nodiscard]] status same_file_identity(
    const std::filesystem::path& path,
    const file_change_token& expected,
    bool& same) noexcept;

// BUILD-only journal path. not_found means the caller must use the portable full
// filesystem scan. Allocation failures remain hard failures.
[[nodiscard]] status detect_source_changes(
    const source_manager_image_view& sources,
    std::vector<source_id>& dirty_sources,
    source_change_detection_telemetry& telemetry) noexcept;

[[nodiscard]] status detect_source_changes(
    const change_state_image_view& sources,
    std::vector<source_id>& dirty_sources,
    source_change_detection_telemetry& telemetry) noexcept;

[[nodiscard]] status detect_source_changes(
    const change_state_image_view& sources,
    std::vector<source_change_journal_candidate>& candidates,
    source_change_detection_telemetry& telemetry) noexcept;


// Uses the same volume-journal pass for Source changes and project.json.
// configuration_proven is true only when the Source checkpoint and the
// configuration file identity belong to the same journal continuity domain.
[[nodiscard]] status detect_source_changes(
    const source_manager_image_view& sources,
    const file_change_token& configuration,
    std::vector<source_id>& dirty_sources,
    bool& configuration_proven,
    bool& configuration_changed,
    source_change_detection_telemetry& telemetry) noexcept;

[[nodiscard]] status detect_source_changes(
    const change_state_image_view& sources,
    const file_change_token& configuration,
    std::vector<source_id>& dirty_sources,
    bool& configuration_proven,
    bool& configuration_changed,
    source_change_detection_telemetry& telemetry) noexcept;

[[nodiscard]] status detect_source_changes(
    const change_state_image_view& sources,
    const file_change_token& configuration,
    std::vector<source_change_journal_candidate>& candidates,
    bool& configuration_proven,
    bool& configuration_changed,
    source_change_detection_telemetry& telemetry) noexcept;

[[nodiscard]] status resolve_source_change_candidates(
    const source_manager_image_view& sources,
    std::span<const source_change_journal_candidate> candidates,
    std::vector<source_id>& dirty_sources,
    source_change_detection_telemetry& telemetry) noexcept;


} // namespace cw::server
