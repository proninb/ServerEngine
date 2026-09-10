#pragma once

#include <cstdint>

namespace cw::server {

enum class log_level : std::uint8_t {
    trace,
    debug,
    info,
    warning,
    error,
    critical,
};

} // namespace cw::server
