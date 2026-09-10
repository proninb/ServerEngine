#pragma once

#include "include_discovery.hpp"
#include "source_frontend_cache.hpp"
#include "../parser/source_environment.hpp"
#include "../parser/source_parser.hpp"
#include "../source/source_manager.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace cw::server {

class project_build_orchestrator;

struct source_frontend_summary final {
    std::uint32_t roots = 0;
    std::uint32_t dirty = 0;
    std::uint32_t changed = 0;
    std::uint32_t affected = 0;
    std::uint32_t discovered = 0;
    std::uint32_t acquired = 0;
    std::uint32_t lexed = 0;
    std::uint32_t parsed = 0;
    std::uint32_t reused_interfaces = 0;
    std::size_t source_graph_visited = 0;
    std::size_t worker_limit = 0;
    std::size_t max_active_workers = 0;
};

struct source_frontend_entry final {
    source_id source{};
    parsed_source parsed;
    std::unique_ptr<source_interface> interface;
    bool removed = false;
};

// Owns the dependency-ready Parser products for one frontend generation. Interface
// pointees remain stable when entries move, preserving transitive import references.
class source_frontend_result final {
public:
    source_frontend_result() = default;
    source_frontend_result(const source_frontend_result&) = delete;
    source_frontend_result& operator=(const source_frontend_result&) = delete;
    source_frontend_result(source_frontend_result&&) noexcept = default;
    source_frontend_result& operator=(source_frontend_result&&) noexcept = default;

    [[nodiscard]] std::span<const source_frontend_entry> sources() const noexcept { return entries; }
    [[nodiscard]] const source_frontend_entry* find(source_id source) const noexcept;
    [[nodiscard]] const source_frontend_summary& summary() const noexcept { return statistics; }

private:
    friend class source_frontend_generation;
    friend class project_build_orchestrator;
    std::vector<source_frontend_entry> entries;
    source_frontend_summary statistics{};
};

// G0 frontend coordinator adapted from the proven OLD dependency scheduler. Source
// acquisition and lex/parse workers operate only on immutable jobs/snapshots; all
// Source Manager mutation and dependency publication remain coordinator-owned.
class source_frontend_generation final {
public:
    source_frontend_generation(
        project_context& project_value,
        source_manager_update& source_update,
        std::size_t worker_limit = 0) noexcept;

    source_frontend_generation(
        project_context& project_value,
        source_manager_update& source_update,
        const source_frontend_cache& cache_value,
        std::size_t worker_limit = 0) noexcept;

    [[nodiscard]] status build(
        std::span<const std::filesystem::path> roots,
        operation_id operation,
        diagnostic_buffer& diagnostics,
        source_frontend_result& output) noexcept;

    [[nodiscard]] status build_incremental(
        std::span<const source_id> dirty_sources,
        operation_id operation,
        diagnostic_buffer& diagnostics,
        source_frontend_result& output) noexcept;

private:
    project_context& project;
    source_manager_update& sources;
    const source_frontend_cache* cache = nullptr;
    std::size_t worker_limit = 1;
};

} // namespace cw::server
