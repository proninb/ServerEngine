#pragma once

#include "diagnostic.hpp"

#include <string_view>

namespace cw::server {

struct diagnostic_descriptor {
    diagnostic_id id;
    diagnostic_domain domain = diagnostic_domain::unknown;
    diagnostic_severity default_severity = diagnostic_severity::error;
    std::string_view name;
    std::string_view message;
};

namespace diagnostics {

inline constexpr diagnostic_descriptor server_initialization_failed{
    diagnostic_id{1001}, diagnostic_domain::server, diagnostic_severity::fatal,
    "server.initialization_failed", "Server initialization failed"};
inline constexpr diagnostic_descriptor server_invalid_json{
    diagnostic_id{1002}, diagnostic_domain::server, diagnostic_severity::error,
    "server.invalid_json", "Server configuration contains invalid JSON"};
inline constexpr diagnostic_descriptor server_invalid_configuration{
    diagnostic_id{1003}, diagnostic_domain::server, diagnostic_severity::error,
    "server.invalid_configuration", "Server configuration is invalid"};
inline constexpr diagnostic_descriptor server_unsupported_configuration_version{
    diagnostic_id{1004}, diagnostic_domain::server, diagnostic_severity::error,
    "server.unsupported_configuration_version", "Server configuration version is unsupported"};
inline constexpr diagnostic_descriptor server_configuration_read_failed{
    diagnostic_id{1005}, diagnostic_domain::server, diagnostic_severity::error,
    "server.configuration_read_failed", "Server configuration could not be read"};

inline constexpr diagnostic_descriptor project_initialization_failed{
    diagnostic_id{2001}, diagnostic_domain::project, diagnostic_severity::error,
    "project.initialization_failed", "Project initialization failed"};
inline constexpr diagnostic_descriptor project_invalid_json{
    diagnostic_id{2002}, diagnostic_domain::project, diagnostic_severity::error,
    "project.invalid_json", "Project configuration contains invalid JSON"};
inline constexpr diagnostic_descriptor project_invalid_configuration{
    diagnostic_id{2003}, diagnostic_domain::project, diagnostic_severity::error,
    "project.invalid_configuration", "Project configuration is invalid"};
inline constexpr diagnostic_descriptor project_unsupported_configuration_version{
    diagnostic_id{2004}, diagnostic_domain::project, diagnostic_severity::error,
    "project.unsupported_configuration_version", "Project configuration version is unsupported"};
inline constexpr diagnostic_descriptor project_configuration_read_failed{
    diagnostic_id{2005}, diagnostic_domain::project, diagnostic_severity::error,
    "project.configuration_read_failed", "Project configuration could not be read"};

inline constexpr diagnostic_descriptor identity_initialization_failed{
    diagnostic_id{4501}, diagnostic_domain::identity, diagnostic_severity::fatal,
    "identity.initialization_failed", "Project identity registry initialization failed"};

inline constexpr diagnostic_descriptor generation_build_failed{
    diagnostic_id{5001}, diagnostic_domain::generation, diagnostic_severity::error,
    "generation.build_failed", "Graph generation build failed"};

inline constexpr diagnostic_descriptor persistence_save_failed{
    diagnostic_id{5501}, diagnostic_domain::persistence, diagnostic_severity::error,
    "persistence.save_failed", "Graph snapshot could not be saved"};
inline constexpr diagnostic_descriptor persistence_load_failed{
    diagnostic_id{5502}, diagnostic_domain::persistence, diagnostic_severity::error,
    "persistence.load_failed", "Graph snapshot could not be loaded"};

} // namespace diagnostics

} // namespace cw::server
