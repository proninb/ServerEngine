#pragma once

#include <cstdint>
#include <type_traits>

namespace cw::server {

class graph;
class generation_builder;
class compiled_image_view;
class build_cache_image_view;

// Identifies one user-type slot inside the current committed Graph. The value
// is Graph-local and is not a persistent semantic identity.
class type_handle final {
public:
    constexpr type_handle() noexcept = default;

    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return slot; }
    [[nodiscard]] constexpr bool valid() const noexcept { return slot != 0; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

    friend constexpr bool operator==(type_handle, type_handle) noexcept = default;

private:
    explicit constexpr type_handle(std::uint32_t value) noexcept : slot(value) {}

    std::uint32_t slot = 0;

    friend class graph;
    friend class generation_builder;
    friend class compiled_image_view;
    friend class build_cache_image_view;
};

static_assert(sizeof(type_handle) == 4);
static_assert(std::is_trivially_copyable_v<type_handle>);
static_assert(std::is_standard_layout_v<type_handle>);

} // namespace cw::server
