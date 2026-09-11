#pragma once

#include "builder/source_contribution.hpp"
#include "frontend/source_frontend_cache.hpp"
#include "graph/graph.hpp"
#include "identity/identity_space.hpp"
#include "source/source_manager.hpp"
#include "string/string_table.hpp"

namespace cw::server {

class build_cache_image_view;
class compiled_image_view;
class source_manager_image_view;
class project_build_orchestrator;
class project_context;
class project_semantic_services;

// Owns every ID-bearing compiled Project structure that is replaced together by
// successful REBUILD. A BUILD baseline binds these structures to mmap and keeps
// only sparse post-baseline materialization locally.
class compiled_project_state final {
public:
    compiled_project_state() noexcept = default;

    compiled_project_state(
        const compiled_image_view& compiled,
        const source_manager_image_view& sources_value,
        const build_cache_image_view& build_cache) noexcept
        : strings(compiled),
          identities(compiled),
          sources(sources_value, build_cache),
          frontend_cache(build_cache, sources_value),
          contributions(build_cache),
          graph_value(compiled, build_cache) {}

    compiled_project_state(const compiled_project_state&) = delete;
    compiled_project_state& operator=(const compiled_project_state&) = delete;
    compiled_project_state(compiled_project_state&&) = delete;
    compiled_project_state& operator=(compiled_project_state&&) = delete;

private:
    string_table strings;
    identity_space identities;
    source_manager sources;
    source_frontend_cache frontend_cache;
    source_contribution_cache contributions;
    graph graph_value;

    friend class project_build_orchestrator;
    friend class project_context;
    friend class project_semantic_services;
};

} // namespace cw::server
