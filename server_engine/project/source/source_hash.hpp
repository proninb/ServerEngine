#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace cw::server {

// Content identity used by Source Manager change detection. The digest is derived
// only from immutable Source bytes and is independent of path/source_id identity.
struct source_content_hash final {
    std::array<std::byte, 32> bytes{};

    friend constexpr bool operator==(
        const source_content_hash&,
        const source_content_hash&) noexcept = default;
};

[[nodiscard]] source_content_hash hash_source_content(std::string_view bytes) noexcept;

} // namespace cw::server
