#pragma once

#include <cstdint>

namespace cw::server {

class string_table;

// Identifies one canonical textual atom for the lifetime of a loaded Project.
// It is text identity only; semantic identity remains identity_ref.
class string_id final {
public:
    constexpr string_id() noexcept = default;

    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return slot; }
    [[nodiscard]] constexpr bool valid() const noexcept { return slot != 0; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

    friend constexpr bool operator==(string_id, string_id) noexcept = default;

private:
    explicit constexpr string_id(std::uint32_t value) noexcept : slot(value) {}

    std::uint32_t slot = 0;

    friend class string_table;
};

static_assert(sizeof(string_id) == 4);

} // namespace cw::server
