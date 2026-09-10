#pragma once

#include <cstdint>

namespace cw::server {

// Local zero-based member position inside one record definition.
class member_index final {
public:
    constexpr member_index() noexcept = default;

    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return slot; }
    [[nodiscard]] constexpr bool valid() const noexcept { return slot != invalid_value; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

    friend constexpr bool operator==(member_index, member_index) noexcept = default;

    [[nodiscard]] static constexpr member_index from_zero_based(std::uint32_t value) noexcept {
        return member_index{value};
    }

private:
    static constexpr std::uint32_t invalid_value = 0xffffffffu;
    explicit constexpr member_index(std::uint32_t value) noexcept : slot(value) {}

    std::uint32_t slot = invalid_value;
};

static_assert(sizeof(member_index) == 4);

} // namespace cw::server
