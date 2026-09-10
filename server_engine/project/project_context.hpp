#pragma once

#include "project_configuration.hpp"
#include "identity/identity_space.hpp"
#include "source/source_manager.hpp"
#include "frontend/source_frontend_cache.hpp"
#include "builder/source_contribution.hpp"
#include "graph/graph.hpp"

#include <cstddef>
#include <string_view>

namespace cw::server {

// Owns one Project aggregate: configuration, Project-lifetime semantic identity,
// Source state, reconstructable Parser interfaces, build provenance, and the single
// current compiled Graph. No historical Graph generations are retained.
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

    [[nodiscard]] source_manager& sources() noexcept { return source_manager_value; }
    [[nodiscard]] const source_manager& sources() const noexcept { return source_manager_value; }

    [[nodiscard]] source_frontend_cache& frontend_cache() noexcept { return frontend_cache_value; }
    [[nodiscard]] const source_frontend_cache& frontend_cache() const noexcept { return frontend_cache_value; }

    [[nodiscard]] source_contribution_cache& contributions() noexcept { return contribution_cache_value; }
    [[nodiscard]] const source_contribution_cache& contributions() const noexcept { return contribution_cache_value; }

    [[nodiscard]] graph& compiled_graph() noexcept { return graph_value; }
    [[nodiscard]] const graph& compiled_graph() const noexcept { return graph_value; }

private:
    project_configuration project_configuration_value;
    identity_space identities;
    source_manager source_manager_value;
    source_frontend_cache frontend_cache_value;
    source_contribution_cache contribution_cache_value;
    graph graph_value;
};

} // namespace cw::server
