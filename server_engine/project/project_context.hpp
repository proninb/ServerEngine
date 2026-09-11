#pragma once

#include "compiled_project_state.hpp"
#include "project_configuration.hpp"

#include <cstddef>
#include <memory>
#include <string_view>

namespace cw::server {

class project_build_orchestrator;
class project_manager;
class project_access;

struct project_storage_pressure final {
    std::size_t retained_bytes = 0;
    std::size_t stale_bytes = 0;
    std::size_t reserve_bytes = 0;
    bool rebuild_recommended = false;
};

// Narrow mutable semantic service used by Parser construction only. It can intern
// text atoms and canonicalize declaration identity but cannot access Source,
// contribution, Graph, publication, or lifecycle state.
class project_semantic_services final {
public:
    [[nodiscard]] identity_ref identity_root() const noexcept {
        return state->identities.root();
    }

    [[nodiscard]] identity_view identities() const noexcept {
        return state->identities.view();
    }

    [[nodiscard]] status intern_string(
        std::string_view value,
        string_id& output) const noexcept {
        return state->strings.intern(value, output);
    }

    [[nodiscard]] string_id find_string(std::string_view value) const noexcept {
        return state->strings.find(value);
    }

    [[nodiscard]] status resolve_declaration(
        identity_ref parent,
        std::string_view local_name,
        identity_kind kind,
        identity_ref& identity) const noexcept {

        string_id name;
        const auto result = state->strings.intern(local_name, name);
        if (!result.ok())
            return result;
        return state->identities.resolve_declaration(parent, name, kind, identity);
    }

    [[nodiscard]] status resolve_declaration(
        identity_ref parent,
        string_id local_name,
        identity_kind kind,
        identity_ref& identity) const noexcept {
        return state->identities.resolve_declaration(parent, local_name, kind, identity);
    }

private:
    explicit project_semantic_services(compiled_project_state& state_value) noexcept
        : state(&state_value) {}

    compiled_project_state* state = nullptr;

    friend class project_context;
};

// Internal owner of one Project configuration plus one compiled construction state.
// READY readers obtain project_read_view only while project_access is held.
class project_context final {
public:
    explicit project_context(project_configuration configuration);

    project_context(const project_context&) = delete;
    project_context& operator=(const project_context&) = delete;

    [[nodiscard]] const project_configuration& configuration() const noexcept {
        return project_configuration_value;
    }

    [[nodiscard]] identity_ref identity_root() const noexcept {
        return compiled->identities.root();
    }

    [[nodiscard]] identity_view identity_metadata() const noexcept {
        return compiled->identities.view();
    }

    [[nodiscard]] status intern_string(
        std::string_view value,
        string_id& output) noexcept {
        return compiled->strings.intern(value, output);
    }

    [[nodiscard]] string_id find_string(std::string_view value) const noexcept {
        return compiled->strings.find(value);
    }

    [[nodiscard]] std::string_view string(string_id id) const noexcept {
        return compiled->strings.get(id);
    }

    [[nodiscard]] std::size_t string_count() const noexcept {
        return compiled->strings.size();
    }

    [[nodiscard]] std::size_t string_slot_count() const noexcept {
        return compiled->strings.slot_count();
    }

    [[nodiscard]] string_id string_at_slot(std::size_t index) const noexcept {
        return compiled->strings.at_slot(index);
    }

    [[nodiscard]] status resolve_declaration(
        identity_ref parent,
        std::string_view local_name,
        identity_kind kind,
        identity_ref& identity) noexcept {

        auto semantic = parser_services();
        return semantic.resolve_declaration(parent, local_name, kind, identity);
    }

    [[nodiscard]] status resolve_declaration(
        identity_ref parent,
        string_id local_name,
        identity_kind kind,
        identity_ref& identity) noexcept {
        return compiled->identities.resolve_declaration(parent, local_name, kind, identity);
    }

    [[nodiscard]] identity_ref find_identity(
        identity_ref parent,
        string_id local_name,
        identity_kind kind) const noexcept {
        return compiled->identities.find(parent, local_name, kind);
    }

    [[nodiscard]] std::size_t identity_count() const noexcept {
        return compiled->identities.size();
    }

    [[nodiscard]] std::size_t identity_slot_count() const noexcept {
        return compiled->identities.slot_count();
    }

    [[nodiscard]] identity_ref identity_at_slot(std::size_t index) const noexcept {
        return compiled->identities.at_slot(index);
    }

    [[nodiscard]] std::size_t identity_bytes_reserved() const noexcept {
        return compiled->identities.bytes_reserved();
    }

    [[nodiscard]] std::size_t identity_pages_reserved() const noexcept {
        return compiled->identities.pages_reserved();
    }

    [[nodiscard]] std::size_t identity_bucket_count() const noexcept {
        return compiled->identities.bucket_count();
    }

    [[nodiscard]] identity_index_statistics identity_index_stats() const noexcept {
        return compiled->identities.index_statistics();
    }

    [[nodiscard]] string_table_statistics string_table_stats() const noexcept {
        return compiled->strings.statistics();
    }

    [[nodiscard]] project_storage_pressure storage_pressure() const noexcept;

    [[nodiscard]] const source_manager& sources() const noexcept {
        return compiled->sources;
    }

    [[nodiscard]] const source_frontend_cache& frontend_cache() const noexcept {
        return compiled->frontend_cache;
    }

    [[nodiscard]] const source_contribution_cache& contributions() const noexcept {
        return compiled->contributions;
    }

    [[nodiscard]] const graph& compiled_graph() const noexcept {
        return compiled->graph_value;
    }

    [[nodiscard]] project_semantic_services parser_services() noexcept {
        return project_semantic_services{*compiled};
    }

    [[nodiscard]] status find_type(
        std::string_view path,
        type_handle& output) const noexcept;

    [[nodiscard]] status find_object(
        std::string_view path,
        object_handle& output) const noexcept;

    [[nodiscard]] status find_endpoint(
        std::string_view path,
        object_endpoint& output) const noexcept;

    [[nodiscard]] status find_link(
        std::string_view target_path,
        link_handle& output) const noexcept;

private:
    [[nodiscard]] identity_ref find_named_identity(
        std::string_view path,
        identity_kind final_kind) const noexcept;

    [[nodiscard]] compiled_project_state& mutable_compiled() noexcept {
        return *compiled;
    }

    [[nodiscard]] std::unique_ptr<compiled_project_state> release_compiled() noexcept {
        return std::move(compiled);
    }

    void replace_compiled(std::unique_ptr<compiled_project_state> replacement) noexcept {
        compiled.swap(replacement);
    }

    project_configuration project_configuration_value;
    std::unique_ptr<compiled_project_state> compiled;

    friend class project_build_orchestrator;
    friend class project_manager;
    friend class source_frontend_generation;
};

// Read-only façade. Any pointer/span/string_view obtained through it is valid only
// while the owning project_access remains alive.
class project_read_view final {
public:
    project_read_view() noexcept = default;

    [[nodiscard]] bool valid() const noexcept { return project != nullptr; }

    [[nodiscard]] const project_configuration& configuration() const noexcept {
        return project->configuration();
    }

    [[nodiscard]] const source_manager& sources() const noexcept {
        return project->sources();
    }

    [[nodiscard]] const graph& compiled_graph() const noexcept {
        return project->compiled_graph();
    }

    [[nodiscard]] std::size_t identity_count() const noexcept {
        return project->identity_count();
    }

    [[nodiscard]] string_table_statistics string_table_stats() const noexcept {
        return project->string_table_stats();
    }

    [[nodiscard]] project_storage_pressure storage_pressure() const noexcept {
        return project->storage_pressure();
    }

    [[nodiscard]] std::string_view string(string_id id) const noexcept {
        return project->string(id);
    }

    [[nodiscard]] status find_type(std::string_view path, type_handle& output) const noexcept {
        return project->find_type(path, output);
    }

    [[nodiscard]] status find_object(std::string_view path, object_handle& output) const noexcept {
        return project->find_object(path, output);
    }

    [[nodiscard]] status find_endpoint(std::string_view path, object_endpoint& output) const noexcept {
        return project->find_endpoint(path, output);
    }

    [[nodiscard]] status find_link(std::string_view path, link_handle& output) const noexcept {
        return project->find_link(path, output);
    }

private:
    explicit project_read_view(const project_context& project_value) noexcept
        : project(&project_value) {}

    const project_context* project = nullptr;

    friend class project_manager;
    friend class project_access;
};

} // namespace cw::server
