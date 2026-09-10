#pragma once

#include <cstdint>

namespace cw::server {

// Identifies one normalized Source inside a Project Context; it is not semantic type identity.
class source_id final {
public:
    constexpr source_id() noexcept = default;
    explicit constexpr source_id(std::uint32_t value) noexcept : value_storage(value) {}

    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_storage; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value_storage != 0; }

    friend constexpr bool operator==(source_id, source_id) noexcept = default;

private:
    std::uint32_t value_storage = 0;
};

} // namespace cw::server
