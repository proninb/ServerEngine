#pragma once

#include "../logging/log.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cw::server {

inline constexpr std::uint32_t current_server_configuration_version = 1;

enum class transport_kind : std::uint8_t {
    tcp,
};

enum class protocol_kind : std::uint8_t {
    json,
};

struct endpoint_configuration {
    std::string name;
    transport_kind transport = transport_kind::tcp;
    protocol_kind protocol = protocol_kind::json;
    std::string address;
    std::uint16_t port = 0;
};

struct communication_configuration {
    std::vector<endpoint_configuration> endpoints;
    bool console = true;
};

struct logging_configuration {
    log_level minimum_level = log_level::info;
    bool console = true;
};

struct telemetry_configuration {
    bool metrics = true;
};

struct project_reference {
    std::filesystem::path path;
};

// Root configuration for one Server process owning exactly one Project Context.
struct server_configuration {
    std::uint32_t version = 0;
    communication_configuration communication;
    logging_configuration logging;
    telemetry_configuration telemetry;
    project_reference project;
};

} // namespace cw::server
