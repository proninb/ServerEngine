#pragma once

#include <cstdint>

namespace cw::server {

// Identifies one externally visible Server operation for diagnostics and telemetry.
class operation_id final {
public:
    constexpr operation_id() noexcept = default;
    explicit constexpr operation_id(std::uint64_t value) noexcept : value_storage(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_storage; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value_storage != 0; }

    friend constexpr bool operator==(operation_id, operation_id) noexcept = default;

private:
    std::uint64_t value_storage = 0;
};

} // namespace cw::server
