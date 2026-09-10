#pragma once

#include "server_configuration.hpp"
#include "../diagnostics/diagnostic_buffer.hpp"
#include "../operation.hpp"
#include "../status.hpp"

#include <filesystem>
#include <string_view>

namespace cw::server {

// Parses and validates Server configuration transactionally; output changes only on success.
[[nodiscard]] status load_server_configuration(
    std::string_view text,
    const std::filesystem::path& configuration_path,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    server_configuration& output) noexcept;

[[nodiscard]] status load_server_configuration_file(
    const std::filesystem::path& configuration_path,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    server_configuration& output) noexcept;

} // namespace cw::server
