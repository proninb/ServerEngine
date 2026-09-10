#pragma once

#include "diagnostic_descriptor.hpp"

#include <array>
#include <span>

namespace cw::server {

// Provides read-only lookup over the immutable diagnostic descriptor catalog.
class diagnostic_registry_view final {
public:
    constexpr explicit diagnostic_registry_view(std::span<const diagnostic_descriptor> values) noexcept
        : descriptors(values) {}

    [[nodiscard]] constexpr const diagnostic_descriptor* find(diagnostic_id id) const noexcept {
        for (const auto& descriptor : descriptors) {
            if (descriptor.id == id) {
                return &descriptor;
            }
        }
        return nullptr;
    }

private:
    std::span<const diagnostic_descriptor> descriptors;
};

inline constexpr std::array diagnostic_descriptors{
    diagnostics::server_initialization_failed,
    diagnostics::server_invalid_json,
    diagnostics::server_invalid_configuration,
    diagnostics::server_unsupported_configuration_version,
    diagnostics::server_configuration_read_failed,
    diagnostics::project_initialization_failed,
    diagnostics::project_invalid_json,
    diagnostics::project_invalid_configuration,
    diagnostics::project_unsupported_configuration_version,
    diagnostics::project_configuration_read_failed,
    diagnostics::source_acquisition_failed,
    diagnostics::source_dependency_invalid,
    diagnostics::source_dependency_cycle,
    diagnostics::source_unsupported_directive,
    diagnostics::parser_invalid_source_facts,
    diagnostics::parser_syntax_error,
    diagnostics::parser_unsupported_construct,
    diagnostics::parser_unresolved_type,
    diagnostics::parser_semantic_resolution_failed,
    diagnostics::identity_initialization_failed,
    diagnostics::generation_build_failed,
    diagnostics::persistence_save_failed,
    diagnostics::persistence_load_failed,
};

inline constexpr diagnostic_registry_view diagnostic_registry{diagnostic_descriptors};

consteval bool diagnostic_ids_unique() {
    for (std::size_t left = 0; left < diagnostic_descriptors.size(); ++left) {
        for (std::size_t right = left + 1; right < diagnostic_descriptors.size(); ++right) {
            if (diagnostic_descriptors[left].id == diagnostic_descriptors[right].id) {
                return false;
            }
        }
    }
    return true;
}

consteval bool diagnostic_names_unique() {
    for (std::size_t left = 0; left < diagnostic_descriptors.size(); ++left) {
        for (std::size_t right = left + 1; right < diagnostic_descriptors.size(); ++right) {
            if (diagnostic_descriptors[left].name == diagnostic_descriptors[right].name) {
                return false;
            }
        }
    }
    return true;
}

static_assert(diagnostic_ids_unique());
static_assert(diagnostic_names_unique());

} // namespace cw::server
