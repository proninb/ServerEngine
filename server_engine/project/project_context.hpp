#pragma once

#include "project_configuration.hpp"
#include "identity/identity_space.hpp"

#include <cstddef>
#include <string_view>

namespace cw::server {

// Owns Project-lifetime semantic identity and configuration. Identity state is
// project state only; generation-specific semantic facts remain exclusively in G.
class project_context final {
public:
    explicit project_context(project_configuration configuration);

    [[nodiscard]] const project_configuration& configuration() const noexcept {
        return project_configuration_value;
    }

    [[nodiscard]] identity_ref identity_root() const noexcept {
        return identities.root();
    }

    [[nodiscard]] status resolve_declaration(
        identity_ref parent,
        std::string_view local_name,
        identity_kind kind,
        identity_ref& identity) noexcept {

        return identities.resolve_declaration(parent, local_name, kind, identity);
    }

    [[nodiscard]] std::size_t identity_count() const noexcept {
        return identities.size();
    }

    [[nodiscard]] std::size_t identity_bytes_reserved() const noexcept {
        return identities.bytes_reserved();
    }

    [[nodiscard]] std::size_t identity_pages_reserved() const noexcept {
        return identities.pages_reserved();
    }

    [[nodiscard]] std::size_t identity_bucket_count() const noexcept {
        return identities.bucket_count();
    }

    [[nodiscard]] identity_index_statistics identity_index_stats() const noexcept {
        return identities.index_statistics();
    }

private:
    project_configuration project_configuration_value;
    identity_space identities;
};

} // namespace cw::server
