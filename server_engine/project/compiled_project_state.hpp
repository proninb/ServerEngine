#pragma once

#include "builder/source_contribution.hpp"
#include "frontend/source_frontend_cache.hpp"
#include "graph/graph.hpp"
#include "identity/identity_space.hpp"
#include "source/source_manager.hpp"
#include "string/string_table.hpp"

namespace cw::server {

class project_build_orchestrator;
class project_context;
class project_semantic_services;

// Owns every ID-bearing compiled Project structure that is replaced together by
// successful REBUILD and destroyed together by LOAD replacement or UNLOAD.
class compiled_project_state final {
public:
    compiled_project_state() noexcept = default;

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
