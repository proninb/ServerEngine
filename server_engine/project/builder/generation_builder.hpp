#pragma once

#include "source_contribution.hpp"
#include "../graph/graph.hpp"
#include "../project_configuration.hpp"

#include <cstdint>
#include <span>

namespace cw::server {

class diagnostic_buffer;

struct generation_build_telemetry final {
    std::uint64_t contribution_capture_ns = 0;
    std::uint64_t identity_to_handle_ns = 0;
    std::uint64_t type_ref_materialization_ns = 0;
    std::uint64_t definition_materialization_ns = 0;
    std::uint64_t validation_ns = 0;
    std::uint64_t prepare_publish_ns = 0;
    std::uint64_t total_prepare_ns = 0;
    std::uint64_t publish_ns = 0;

    std::uint64_t sources = 0;
    std::uint64_t type_declarations = 0;
    std::uint64_t unique_types = 0;
    std::uint64_t members = 0;
    std::uint64_t enum_values = 0;
    std::uint64_t canonical_type_refs = 0;
    std::uint64_t derived_type_refs = 0;
};

// Builds one detached Graph generation directly from Parser-resolved identity_ref
// values. No source-language/name lookup, stable ID allocation, String Registry,
// semantic sorting, or committed Graph mutation occurs before publish_prepared().
class generation_builder final {
public:
    generation_builder(
        source_contribution_cache& contribution_cache,
        graph& target_graph) noexcept;

    generation_builder(const generation_builder&) = delete;
    generation_builder& operator=(const generation_builder&) = delete;
    generation_builder(generation_builder&&) = delete;
    generation_builder& operator=(generation_builder&&) = delete;

    [[nodiscard]] status prepare_g0(
        std::span<const source_facts> sources,
        const abi_configuration& abi,
        operation_id operation,
        diagnostic_buffer& diagnostics) noexcept;

    // O(1) vector swaps only. Every allocation-sensitive operation and semantic
    // validation has completed before this no-fail publication barrier.
    void publish_prepared() noexcept;

    [[nodiscard]] bool ready() const noexcept { return prepared; }
    [[nodiscard]] bool published() const noexcept { return published_value; }
    [[nodiscard]] const generation_build_telemetry& telemetry() const noexcept { return telemetry_value; }

private:
    [[nodiscard]] status prepare_graph(
        const abi_configuration& abi,
        operation_id operation,
        diagnostic_buffer& diagnostics) noexcept;

    source_contribution_cache_update contributions;
    graph& target;
    prepared_graph_generation prepared_graph;
    generation_build_telemetry telemetry_value{};
    bool prepared = false;
    bool published_value = false;
};

} // namespace cw::server
