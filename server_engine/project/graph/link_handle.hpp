#pragma once

#include <cstdint>

namespace cw::server {

class graph;
class generation_builder;
class compiled_image_view;

// Identifies one link slot in the single current compiled Graph. A link is
// canonical by its target endpoint; no textual name survives Graph construction.
class link_handle final {
public:
    constexpr link_handle() noexcept = default;

    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return slot; }
    [[nodiscard]] constexpr bool valid() const noexcept { return slot != 0; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

    friend constexpr bool operator==(link_handle, link_handle) noexcept = default;

private:
    explicit constexpr link_handle(std::uint32_t value) noexcept : slot(value) {}

    std::uint32_t slot = 0;

    friend class graph;
    friend class generation_builder;
    friend class compiled_image_view;
};

static_assert(sizeof(link_handle) == 4);

} // namespace cw::server
