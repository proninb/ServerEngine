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

    std::uint64_t changed_sources = 0;
    std::uint64_t changed_types = 0;
    std::uint64_t added_types = 0;
    std::uint64_t removed_types = 0;
    std::uint64_t validation_visited_types = 0;
    std::uint64_t validation_visited_type_refs = 0;
    std::uint64_t validation_dependency_edges = 0;
    std::uint64_t graph_full_scans = 0;
    std::uint64_t contribution_full_scans = 0;
};

// Builds a detached full Graph or a sparse incremental candidate directly from Parser-
// resolved identity_ref values. Builder performs no source-language/name lookup,
// stable ID allocation, String Registry canonicalization, semantic sort, or
// mutex-based shared mutation.
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

    // replacements are complete new facts for modified/added Sources. removals
    // contain Sources absent from the candidate Project state. A Source may
    // appear in exactly one input set.
    [[nodiscard]] status prepare_incremental(
        std::span<const source_facts> replacements,
        std::span<const source_id> removals,
        const abi_configuration& abi,
        operation_id operation,
        diagnostic_buffer& diagnostics) noexcept;

    // Publication performs only pre-reserved append/patch operations or detached
    // swaps. All allocation-sensitive work and validation is complete beforehand.
    void publish_prepared() noexcept;

    [[nodiscard]] bool ready() const noexcept { return prepared; }
    [[nodiscard]] bool published() const noexcept { return published_value; }
    [[nodiscard]] const generation_build_telemetry& telemetry() const noexcept { return telemetry_value; }

private:
    enum class build_mode : std::uint8_t {
        none,
        rebuild,
        incremental,
    };

    [[nodiscard]] status prepare_graph(
        const abi_configuration& abi,
        operation_id operation,
        diagnostic_buffer& diagnostics) noexcept;

    [[nodiscard]] status prepare_incremental_graph(
        const abi_configuration& abi,
        operation_id operation,
        diagnostic_buffer& diagnostics) noexcept;

    source_contribution_cache& contribution_cache;
    source_contribution_cache_update contributions;
    source_contribution_sparse_update sparse_contributions;
    graph& target;
    prepared_graph_generation prepared_graph;
    prepared_graph_update prepared_update;
    generation_build_telemetry telemetry_value{};
    build_mode mode = build_mode::none;
    bool prepared = false;
    bool published_value = false;
};

} // namespace cw::server
