#pragma once

#include <cstdint>

namespace cw::server {

// Process-local handle for one interned string in the Project Context string registry.
class string_id final {
public:
    constexpr string_id() noexcept = default;
    explicit constexpr string_id(std::uint32_t value) noexcept : value_storage(value) {}

    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_storage; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value_storage != 0; }

    friend constexpr bool operator==(string_id, string_id) noexcept = default;

private:
    std::uint32_t value_storage = 0;
};

} // namespace cw::server
