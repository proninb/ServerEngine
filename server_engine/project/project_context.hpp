#pragma once

#include "project_configuration.hpp"
#include "identity/identity_registry.hpp"
#include "string/string_registry.hpp"

namespace cw::server {

// Owns Project-lifetime infrastructure. Graph generations may reference identities, but identities never reference a Graph.
class project_context final {
public:
    explicit project_context(project_configuration configuration);

    [[nodiscard]] const project_configuration& configuration() const noexcept { return project_configuration_value; }
    [[nodiscard]] string_registry& strings() noexcept { return string_registry_value; }
    [[nodiscard]] identity_registry& identities() noexcept { return identity_registry_value; }

private:
    project_configuration project_configuration_value;
    string_registry string_registry_value;
    identity_registry identity_registry_value;
};

} // namespace cw::server
