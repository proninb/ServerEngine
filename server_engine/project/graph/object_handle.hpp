#pragma once

#include <cstdint>

namespace cw::server {

class graph;
class generation_builder;

// Identifies one object slot in the single current compiled Graph. The handle is
// not Project semantic identity and becomes invalid when the Project is unloaded.
class object_handle final {
public:
    constexpr object_handle() noexcept = default;

    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return slot; }
    [[nodiscard]] constexpr bool valid() const noexcept { return slot != 0; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

    friend constexpr bool operator==(object_handle, object_handle) noexcept = default;

private:
    explicit constexpr object_handle(std::uint32_t value) noexcept : slot(value) {}

    std::uint32_t slot = 0;

    friend class graph;
    friend class generation_builder;
};

static_assert(sizeof(object_handle) == 4);

} // namespace cw::server
