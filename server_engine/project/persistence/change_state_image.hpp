#pragma once

#include "../source/source_change_tracker.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace cw::server {

inline constexpr std::uint32_t change_state_image_format_version = 3;

// Compact BUILD decision image. V3 persists probabilistic membership gates only:
// a negative result proves that one USN identity is unrelated to the project;
// a possible match is resolved through the deferred Source Manager exact path.
class change_state_image_view final {
public:
    change_state_image_view() noexcept = default;

    [[nodiscard]] status bind(std::span<const std::byte> image) noexcept;
    void reset() noexcept;

    [[nodiscard]] bool valid() const noexcept {
        return bytes.data() != nullptr;
    }

    [[nodiscard]] bool gate_only() const noexcept {
        return format_version_value == change_state_image_format_version;
    }

    [[nodiscard]] std::size_t source_count() const noexcept {
        return source_count_value;
    }

    [[nodiscard]] source_change_checkpoint change_checkpoint() const noexcept {
        return checkpoint_value;
    }

    [[nodiscard]] std::string_view path(source_id) const noexcept {
        return journal_anchor_path;
    }

    [[nodiscard]] bool may_contain_source_file(
        std::uint64_t file_reference) const noexcept;

    [[nodiscard]] bool may_watch_directory_topology(
        std::uint64_t file_reference) const noexcept;

    [[nodiscard]] bool may_watch_directory_arrival(
        std::uint64_t file_reference) const noexcept;

    // V1/V2 compatibility surfaces. V3 exact lookup is intentionally unavailable.
    [[nodiscard]] source_id find_source_file(
        std::uint64_t file_reference) const noexcept;

    [[nodiscard]] std::uint32_t directory_watch_flags(
        std::uint64_t file_reference) const noexcept;

    [[nodiscard]] status verify_contents() const noexcept;

private:
    std::span<const std::byte> bytes;
    std::uint32_t format_version_value = 0;

    // V1 compatibility.
    const std::byte* legacy_file_index = nullptr;

    // V2 compatibility.
    const std::byte* file_control = nullptr;
    const std::byte* file_references = nullptr;
    const std::byte* file_sources = nullptr;

    // V3 gate-only filters.
    const std::byte* source_bloom = nullptr;
    const std::byte* topology_bloom = nullptr;
    const std::byte* arrival_bloom = nullptr;
    std::size_t source_bloom_bytes = 0;
    std::size_t topology_bloom_bytes = 0;
    std::size_t arrival_bloom_bytes = 0;

    const std::byte* directory_index = nullptr;
    std::string_view journal_anchor_path;

    std::size_t file_index_count = 0;
    std::size_t directory_index_count = 0;
    std::size_t source_count_value = 0;
    source_change_checkpoint checkpoint_value{};

    std::uint64_t file_control_crc = 0;
    std::uint64_t file_reference_crc = 0;
    std::uint64_t file_source_crc = 0;
    std::uint64_t directory_index_crc = 0;
    std::uint64_t legacy_file_index_crc = 0;

    std::uint64_t source_bloom_crc = 0;
    std::uint64_t topology_bloom_crc = 0;
    std::uint64_t arrival_bloom_crc = 0;
};

[[nodiscard]] status encode_change_state_image(
    std::size_t source_count,
    std::string_view journal_anchor_path,
    const source_change_capture& capture,
    std::vector<std::byte>& output) noexcept;

} // namespace cw::server
