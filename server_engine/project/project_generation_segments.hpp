#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace cw::server {

enum class project_generation_segment_kind : std::uint8_t {
    compiled = 0,
    sources = 1,
    change = 2,
    build = 3,
    count = 4,
};

inline constexpr std::size_t project_generation_segment_count =
    static_cast<std::size_t>(
        project_generation_segment_kind::count);

inline constexpr std::size_t project_generation_segment_max_extents = 32;

// Storage-neutral immutable segment view. A loaded Generation normally has one
// mmap extent; a construction Generation may expose several native extents
// without assembling a second contiguous copy.
class project_generation_segment final {
public:
    constexpr project_generation_segment() noexcept = default;

    constexpr explicit project_generation_segment(
        std::span<const std::byte> bytes) noexcept {

        (void)append(bytes);
    }

    [[nodiscard]] constexpr bool append(
        std::span<const std::byte> bytes) noexcept {

        if (bytes.empty())
            return true;

        if (extent_count_value >= extents.size() ||
            total_size >
                (std::numeric_limits<std::size_t>::max)() -
                    bytes.size()) {
            return false;
        }

        extents[extent_count_value++] = bytes;
        total_size += bytes.size();
        return true;
    }

    [[nodiscard]] constexpr std::size_t extent_count() const noexcept {
        return extent_count_value;
    }

    [[nodiscard]] constexpr std::span<const std::byte> extent(
        std::size_t index) const noexcept {

        return index < extent_count_value
            ? extents[index]
            : std::span<const std::byte>{};
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return total_size;
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return total_size == 0;
    }

    [[nodiscard]] constexpr bool is_contiguous() const noexcept {
        return extent_count_value <= 1;
    }

    [[nodiscard]] constexpr std::span<const std::byte>
    contiguous() const noexcept {

        return extent_count_value == 1
            ? extents[0]
            : std::span<const std::byte>{};
    }

private:
    std::array<
        std::span<const std::byte>,
        project_generation_segment_max_extents> extents{};
    std::size_t extent_count_value = 0;
    std::size_t total_size = 0;
};

// Read-only view of one Generation's immutable storage. The compatibility
// accessors return a contiguous span only while a segment has one extent.
// New zero-copy persistence code must consume *_segment() instead.
class project_generation_segments final {
public:
    constexpr project_generation_segments() noexcept = default;

    constexpr project_generation_segments(
        std::span<const std::byte> compiled,
        std::span<const std::byte> sources,
        std::span<const std::byte> change,
        std::span<const std::byte> build) noexcept
        : values{
            project_generation_segment{compiled},
            project_generation_segment{sources},
            project_generation_segment{change},
            project_generation_segment{build},
        } {}

    constexpr project_generation_segments(
        project_generation_segment compiled,
        project_generation_segment sources,
        project_generation_segment change,
        project_generation_segment build) noexcept
        : values{
            compiled,
            sources,
            change,
            build,
        } {}

    [[nodiscard]] constexpr const project_generation_segment& segment(
        project_generation_segment_kind kind) const noexcept {

        const auto index =
            static_cast<std::size_t>(kind);

        return index < values.size()
            ? values[index]
            : empty_segment();
    }

    [[nodiscard]] constexpr const project_generation_segment&
    compiled_segment() const noexcept {
        return segment(
            project_generation_segment_kind::compiled);
    }

    [[nodiscard]] constexpr const project_generation_segment&
    sources_segment() const noexcept {
        return segment(
            project_generation_segment_kind::sources);
    }

    [[nodiscard]] constexpr const project_generation_segment&
    change_segment() const noexcept {
        return segment(
            project_generation_segment_kind::change);
    }

    [[nodiscard]] constexpr const project_generation_segment&
    build_segment() const noexcept {
        return segment(
            project_generation_segment_kind::build);
    }

    // Compatibility surfaces for current contiguous encoders/writer.
    [[nodiscard]] constexpr std::span<const std::byte>
    compiled() const noexcept {
        return compiled_segment().contiguous();
    }

    [[nodiscard]] constexpr std::span<const std::byte>
    sources() const noexcept {
        return sources_segment().contiguous();
    }

    [[nodiscard]] constexpr std::span<const std::byte>
    change() const noexcept {
        return change_segment().contiguous();
    }

    [[nodiscard]] constexpr std::span<const std::byte>
    build() const noexcept {
        return build_segment().contiguous();
    }

    [[nodiscard]] constexpr bool persistable() const noexcept {
        return
            !compiled_segment().empty() &&
            !sources_segment().empty() &&
            !build_segment().empty();
    }

private:
    [[nodiscard]] static constexpr const project_generation_segment&
    empty_segment() noexcept {
        return empty_value;
    }

    std::array<
        project_generation_segment,
        project_generation_segment_count> values{};

    inline static constexpr project_generation_segment empty_value{};
};

static_assert(project_generation_segment_count == 4);
static_assert(project_generation_segment_max_extents >= 25);

} // namespace cw::server
