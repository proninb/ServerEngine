#pragma once

#include "source_hash.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace cw::server {

// Filesystem metadata used only as the fast-path change token for one process.
// Content equality is established by source_content_hash after a full acquisition.
struct file_snapshot_observation final {
    std::int64_t write_time_ticks = 0;
    std::uintmax_t size = 0;

    friend constexpr bool operator==(
        const file_snapshot_observation&,
        const file_snapshot_observation&) noexcept = default;
};

enum class file_snapshot_result : std::uint8_t {
    acquired,
    unchanged,
    missing,
    changed_during_read,
    failed,
    allocation_failed,
};

// Owns one stable filesystem read before Source Manager turns it into an immutable
// project Source snapshot. changed_during_read is reported rather than published.
struct file_snapshot final {
    file_snapshot_observation observation{};
    source_content_hash hash{};
    std::string bytes;
};

[[nodiscard]] file_snapshot_result acquire_file_snapshot(
    const std::filesystem::path& path,
    const std::optional<file_snapshot_observation>& baseline,
    file_snapshot& output) noexcept;

} // namespace cw::server
