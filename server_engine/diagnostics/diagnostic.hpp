#pragma once

#include "../operation.hpp"
#include "../source_id.hpp"

#include <cstdint>
#include <string>

namespace cw::server {

using diagnostic_code_value = std::uint32_t;

// Stable catalog identifier for one diagnostic definition.
class diagnostic_id final {
public:
    constexpr diagnostic_id() noexcept = default;
    explicit constexpr diagnostic_id(diagnostic_code_value value) noexcept : value_storage(value) {}

    [[nodiscard]] constexpr diagnostic_code_value value() const noexcept { return value_storage; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value_storage != 0; }

    friend constexpr bool operator==(diagnostic_id, diagnostic_id) noexcept = default;

private:
    diagnostic_code_value value_storage = 0;
};

enum class diagnostic_severity : std::uint8_t {
    note,
    warning,
    error,
    fatal,
};

enum class diagnostic_domain : std::uint8_t {
    unknown = 0,
    server,
    project,
    source,
    parser,
    identity,
    generation,
    persistence,
    runtime,
    shm,
    communication,
};

struct source_range {
    source_id source;
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
};

struct diagnostic_record {
    diagnostic_id id;
    diagnostic_severity severity = diagnostic_severity::error;
    operation_id operation;
    source_range location;
    std::string detail;
};

} // namespace cw::server
