#pragma once

#include "builder/source_contribution.hpp"
#include "frontend/source_frontend_cache.hpp"
#include "graph/graph.hpp"
#include "identity/identity_space.hpp"
#include "source/source_manager.hpp"
#include "string/string_table.hpp"

#include <chrono>
#include <cstdint>

namespace cw::server {

class build_cache_image_view;
class compiled_image_view;
class source_manager_image_view;
class project_build_orchestrator;
class project_context;
class project_semantic_services;

struct compiled_project_state_teardown_telemetry final {
    std::uint64_t graph_ns = 0;
    std::uint64_t contributions_ns = 0;
    std::uint64_t frontend_cache_ns = 0;
    std::uint64_t source_manager_ns = 0;
    std::uint64_t identities_ns = 0;
};

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
    class teardown_marker final {
    public:
        teardown_marker() noexcept = default;

        void bind(
            std::chrono::steady_clock::time_point& previous_value,
            std::uint64_t* output_value) noexcept {

            previous = &previous_value;
            output = output_value;
        }

        ~teardown_marker() noexcept {
            if (previous == nullptr)
                return;

            const auto now =
                std::chrono::steady_clock::now();

            if (output != nullptr) {
                *output = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<
                        std::chrono::nanoseconds>(
                            now - *previous).count());
            }

            *previous = now;
        }

    private:
        std::chrono::steady_clock::time_point* previous = nullptr;
        std::uint64_t* output = nullptr;
    };

    void begin_teardown_audit(
        compiled_project_state_teardown_telemetry& output) noexcept {

        output = {};

        teardown_after_graph.bind(
            teardown_previous,
            nullptr);
        teardown_after_contributions.bind(
            teardown_previous,
            &output.graph_ns);
        teardown_after_frontend.bind(
            teardown_previous,
            &output.contributions_ns);
        teardown_after_sources.bind(
            teardown_previous,
            &output.frontend_cache_ns);
        teardown_after_identities.bind(
            teardown_previous,
            &output.source_manager_ns);
        teardown_after_strings.bind(
            teardown_previous,
            &output.identities_ns);
    }

    std::chrono::steady_clock::time_point teardown_previous{};

    string_table strings;
    teardown_marker teardown_after_strings;

    identity_space identities;
    teardown_marker teardown_after_identities;

    source_manager sources;
    teardown_marker teardown_after_sources;

    source_frontend_cache frontend_cache;
    teardown_marker teardown_after_frontend;

    source_contribution_cache contributions;
    teardown_marker teardown_after_contributions;

    graph graph_value;
    teardown_marker teardown_after_graph;

    friend class project_build_orchestrator;
    friend class project_context;
    friend class project_semantic_services;
};

} // namespace cw::server
