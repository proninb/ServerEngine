#pragma once

#include <cstdint>
#include <system_error>

namespace cw::server {

// Failed hard-link reuse may fall back to a fresh write only when linking is
// unavailable as a filesystem capability or across devices. All other errors
// are fail-closed.
enum class hard_link_failure_action : std::uint8_t {
    fail = 0,
    rewrite = 1,
};

[[nodiscard]] hard_link_failure_action classify_hard_link_failure(
    const std::error_code& error) noexcept;

}
