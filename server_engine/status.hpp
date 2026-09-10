#pragma once

#include <cstdint>

namespace cw::server {

enum class status_code : std::uint32_t {
    ok = 0,
    initialization_failed,
    configuration_failed,
    io_failed,
    invalid_argument,
    semantic_conflict,
    not_found,
    persistence_failed,
    artifact_corrupt,
    rebuild_required,
    not_available,
};

struct status {
    status_code code = status_code::ok;

    [[nodiscard]] constexpr bool ok() const noexcept {
        return code == status_code::ok;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return ok();
    }
};

} // namespace cw::server
