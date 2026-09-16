#pragma once

#include "compiled_project_state.hpp"
#include "persistence/baseline_store.hpp"
#include "persistence/build_cache_image.hpp"
#include "persistence/compiled_image.hpp"
#include "persistence/source_manager_image.hpp"
#include "project_configuration.hpp"
#include "project_generation.hpp"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>

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

// Internal owner of one Project configuration. LOAD uses a pure mmap READY view;
// changed BUILD owns the same immutable mappings as a baseline plus one sparse
// construction overlay that is fully prepared before READY publication.
class project_context final {
public:
    explicit project_context(
        project_configuration configuration,
        std::filesystem::path configuration_path = {});

    project_context(const project_context&) = delete;
    project_context& operator=(const project_context&) = delete;

    [[nodiscard]] const project_configuration& configuration() const noexcept {
        return project_configuration_value;
    }

    [[nodiscard]] const std::filesystem::path& configuration_path() const noexcept {
        return project_configuration_path;
    }

    [[nodiscard]] bool construction_backed() const noexcept {
        return compiled != nullptr;
    }

    [[nodiscard]] bool baseline_backed() const noexcept {
        return baseline != nullptr;
    }

    [[nodiscard]] bool build_cache_mapped() const noexcept {
        return baseline != nullptr &&
            baseline->mapped(baseline_artifact_kind::build_cache);
    }

    [[nodiscard]] std::string_view baseline_transaction() const noexcept {
        return baseline != nullptr ? baseline->transaction() : std::string_view{};
    }

    // Persistence-only capability boundary. The pinned baseline is the sole
    // authority that can mint a valid whole-section direct-borrow proof.
    [[nodiscard]] baseline_section_provenance prove_baseline_section_borrow(
        baseline_artifact_kind artifact,
        std::size_t section,
        std::span<const std::byte> bytes) const noexcept {

        return baseline != nullptr
            ? baseline->prove_section_borrow(
                artifact,
                section,
                bytes)
            : baseline_section_provenance{};
    }

    // Identifies the transaction known to contain the exact active Project
    // state. Pure READY baselines derive it from their immutable mapping;
    // construction-backed states acquire it only after a successful SAVE.
    [[nodiscard]] std::string_view persisted_transaction() const noexcept {
        if (!persisted_transaction_value.empty())
            return persisted_transaction_value;
        if (compiled == nullptr && baseline != nullptr)
            return baseline->transaction();
        return {};
    }

    // Performance metadata only. Failure to cache this identity must never turn
    // a successful durable SAVE into a failed Project operation.
    void remember_persisted_transaction(std::string_view transaction) noexcept {
        try {
            persisted_transaction_value.assign(transaction);
        }
        catch (...) {
            persisted_transaction_value.clear();
        }
    }

    void forget_persisted_transaction() noexcept {
        persisted_transaction_value.clear();
    }

    [[nodiscard]] const baseline_fingerprint* baseline_fingerprint_value() const noexcept {
        return baseline != nullptr
            ? &baseline->fingerprint()
            : nullptr;
    }

    // Semantic configuration identity used to construct the active Project.
    // SAVE must never publish this Graph under a different configuration.
    [[nodiscard]] const baseline_fingerprint* build_fingerprint() const noexcept {
        return build_fingerprint_available
            ? &build_fingerprint_value
            : nullptr;
    }

    [[nodiscard]] const project_generation_provenance&
    generation_provenance() const noexcept {
        return generation_provenance_value;
    }

    [[nodiscard]] const project_generation_native_segments&
    generation_native_segments() const noexcept {
        return generation_native_segments_value;
    }

    [[nodiscard]] const project_generation_configuration_proof*
    generation_configuration_proof() const noexcept {
        return generation_provenance_value.configuration();
    }

    // Full generations own root identities/roles. Sparse baseline-backed
    // generations borrow the immutable root records from their pinned baseline,
    // so BUILD never performs an O(N) root copy merely to enable SAVE.
    [[nodiscard]] std::size_t generation_root_count() const noexcept {
        const auto owned =
            generation_provenance_value.root_count();
        if (owned != 0)
            return owned;

        return mapped_sources.valid()
            ? mapped_sources.root_count()
            : 0;
    }

    [[nodiscard]] status generation_root(
        std::size_t index,
        source_manager_image_root& output) const noexcept {

        output = {};

        const auto owned =
            generation_provenance_value.root_count();

        if (owned != 0) {
            if (index >= owned)
                return {status_code::not_found};

            const auto root =
                generation_provenance_value.root(index);
            if (!root.source)
                return {status_code::initialization_failed};

            output = {
                root.source,
                root.role,
            };
            return {};
        }

        return mapped_sources.valid()
            ? mapped_sources.root(index, output)
            : status{status_code::not_found};
    }

    [[nodiscard]] identity_ref identity_root() const noexcept {
        return compiled != nullptr
            ? compiled->identities.root()
            : mapped_compiled.identity_root();
    }

    // Construction-only metadata service. READY mapped readers use high-level
    // project query functions backed directly by compiled_image_view.
    [[nodiscard]] identity_view identity_metadata() const noexcept {
        return compiled != nullptr
            ? compiled->identities.view()
            : identity_view{};
    }

    [[nodiscard]] status intern_string(
        std::string_view value,
        string_id& output) noexcept {
        if (compiled == nullptr) {
            output = {};
            return {status_code::invalid_state};
        }
        return compiled->strings.intern(value, output);
    }

    [[nodiscard]] string_id find_string(std::string_view value) const noexcept {
        if (compiled != nullptr)
            return compiled->strings.find(value);

        string_id output;
        return mapped_compiled.find_string(value, output).ok()
            ? output
            : string_id{};
    }

    [[nodiscard]] std::string_view string(string_id id) const noexcept {
        return compiled != nullptr
            ? compiled->strings.get(id)
            : mapped_compiled.string(id);
    }

    [[nodiscard]] std::size_t string_count() const noexcept {
        return compiled != nullptr
            ? compiled->strings.size()
            : mapped_compiled.string_count();
    }

    [[nodiscard]] std::size_t string_slot_count() const noexcept {
        return compiled != nullptr
            ? compiled->strings.slot_count()
            : mapped_compiled.string_slot_count();
    }

    [[nodiscard]] string_id string_at_slot(std::size_t index) const noexcept {
        if (compiled != nullptr)
            return compiled->strings.at_slot(index);
        return {};
    }

    [[nodiscard]] status resolve_declaration(
        identity_ref parent,
        std::string_view local_name,
        identity_kind kind,
        identity_ref& identity) noexcept {

        if (compiled == nullptr) {
            identity = {};
            return {status_code::invalid_state};
        }

        auto semantic = parser_services();
        return semantic.resolve_declaration(parent, local_name, kind, identity);
    }

    [[nodiscard]] status resolve_declaration(
        identity_ref parent,
        string_id local_name,
        identity_kind kind,
        identity_ref& identity) noexcept {

        if (compiled == nullptr) {
            identity = {};
            return {status_code::invalid_state};
        }

        return compiled->identities.resolve_declaration(
            parent, local_name, kind, identity);
    }

    [[nodiscard]] identity_ref find_identity(
        identity_ref parent,
        string_id local_name,
        identity_kind kind) const noexcept {

        if (compiled != nullptr)
            return compiled->identities.find(parent, local_name, kind);

        identity_ref output;
        return mapped_compiled.find_identity(
                   parent, local_name, kind, output).ok()
            ? output
            : identity_ref{};
    }

    [[nodiscard]] std::size_t identity_count() const noexcept {
        return compiled != nullptr
            ? compiled->identities.size()
            : mapped_compiled.identity_count();
    }

    [[nodiscard]] std::size_t identity_slot_count() const noexcept {
        return compiled != nullptr
            ? compiled->identities.slot_count()
            : mapped_compiled.identity_slot_count();
    }

    [[nodiscard]] identity_ref identity_at_slot(std::size_t index) const noexcept {
        if (compiled != nullptr)
            return compiled->identities.at_slot(index);
        return {};
    }

    [[nodiscard]] std::size_t identity_bytes_reserved() const noexcept {
        return compiled != nullptr
            ? compiled->identities.bytes_reserved()
            : 0;
    }

    [[nodiscard]] std::size_t identity_pages_reserved() const noexcept {
        return compiled != nullptr
            ? compiled->identities.pages_reserved()
            : 0;
    }

    [[nodiscard]] std::size_t identity_bucket_count() const noexcept {
        return compiled != nullptr
            ? compiled->identities.bucket_count()
            : 0;
    }

    [[nodiscard]] identity_index_statistics identity_index_stats() const noexcept {
        return compiled != nullptr
            ? compiled->identities.index_statistics()
            : identity_index_statistics{};
    }

    [[nodiscard]] string_table_statistics string_table_stats() const noexcept {
        if (compiled != nullptr)
            return compiled->strings.statistics();

        string_table_statistics output;
        output.strings = mapped_compiled.string_count();
        return output;
    }

    [[nodiscard]] project_storage_pressure storage_pressure() const noexcept;

    // Construction-only storage boundaries used by Parser/Builder and persistence
    // encoders before READY publication.
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

    // Storage-neutral READY read boundaries.
    [[nodiscard]] status source_count(std::size_t& output) const noexcept {
        output = 0;
        if (compiled != nullptr) {
            output = compiled->sources.source_count();
            return {};
        }
        const auto ready = ensure_sources_mapped();
        if (!ready.ok())
            return ready;
        output = mapped_sources.source_count();
        return {};
    }

    [[nodiscard]] status source_path(
        source_id source,
        std::string_view& output) const noexcept {
        output = {};
        if (compiled != nullptr) {
            output = compiled->sources.path(source);
            return output.empty() ? status{status_code::not_found} : status{};
        }
        const auto ready = ensure_sources_mapped();
        if (!ready.ok())
            return ready;
        output = mapped_sources.path(source);
        return output.empty() ? status{status_code::not_found} : status{};
    }

    [[nodiscard]] std::size_t source_count() const noexcept {
        std::size_t output = 0;
        (void)source_count(output);
        return output;
    }

    [[nodiscard]] std::string_view source_path(source_id source) const noexcept {
        std::string_view output;
        (void)source_path(source, output);
        return output;
    }

    [[nodiscard]] status find_source(
        std::string_view normalized_path,
        source_id& output) const noexcept {
        if (compiled != nullptr)
            return compiled->sources.find(normalized_path, output);
        const auto ready = ensure_sources_mapped();
        if (!ready.ok()) {
            output = {};
            return ready;
        }
        return mapped_sources.find(normalized_path, output);
    }

    [[nodiscard]] std::size_t type_count() const noexcept {
        return compiled != nullptr
            ? compiled->graph_value.type_count()
            : mapped_compiled.type_count();
    }

    [[nodiscard]] std::size_t object_count() const noexcept {
        return compiled != nullptr
            ? compiled->graph_value.object_count()
            : mapped_compiled.object_count();
    }

    [[nodiscard]] std::size_t link_count() const noexcept {
        return compiled != nullptr
            ? compiled->graph_value.link_count()
            : mapped_compiled.link_count();
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
    struct baseline_storage_tag final {};

    project_context(
        project_configuration configuration,
        std::filesystem::path configuration_path,
        baseline_storage_tag) noexcept;

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

    void set_build_fingerprint(
        const baseline_fingerprint& fingerprint) noexcept {
        build_fingerprint_value = fingerprint;
        build_fingerprint_available = true;
    }

    void publish_generation_source_change(
        source_change_capture&& capture) noexcept {
        generation_provenance_value.publish_source_change(
            std::move(capture));
    }

    void publish_generation_roots(
        std::vector<source_id>&& roots,
        std::vector<project_item_role>&& roles) noexcept {
        generation_provenance_value.publish_roots(
            std::move(roots),
            std::move(roles));
    }

    void publish_generation_configuration_proof(
        const project_generation_configuration_proof& proof) noexcept {
        generation_provenance_value.publish_configuration(proof);
    }

    void clear_generation_source_change() noexcept {
        generation_provenance_value.clear_source_change();
    }

    void publish_generation_change_segment(
        std::vector<std::byte>&& segment) noexcept {
        generation_native_segments_value.publish_change(
            std::move(segment));
    }

    void clear_generation_change_segment() noexcept {
        generation_native_segments_value.clear_change();
    }

    [[nodiscard]] project_generation_provenance
    release_generation_provenance() noexcept {
        return std::move(generation_provenance_value);
    }

    void replace_generation_provenance(
        project_generation_provenance&& replacement) noexcept {
        generation_provenance_value =
            std::move(replacement);
    }

    [[nodiscard]] project_generation_native_segments
    release_generation_native_segments() noexcept {
        return std::move(generation_native_segments_value);
    }

    void replace_generation_native_segments(
        project_generation_native_segments&& replacement) noexcept {
        generation_native_segments_value =
            std::move(replacement);
    }

    [[nodiscard]] status activate_ready_baseline(
        baseline_snapshot&& snapshot) noexcept;

    [[nodiscard]] status activate_build_baseline(
        baseline_snapshot&& snapshot) noexcept;

    [[nodiscard]] status ensure_sources_mapped() const noexcept;

    project_configuration project_configuration_value;
    std::filesystem::path project_configuration_path;
    std::unique_ptr<compiled_project_state> compiled;
    std::unique_ptr<baseline_snapshot> baseline;
    compiled_image_view mapped_compiled;
    mutable source_manager_image_view mapped_sources;
    mutable std::atomic<bool> source_mapping_ready{false};
    mutable std::mutex source_mapping_mutex;
    mutable status source_mapping_status{};
    mutable bool source_mapping_attempted = false;
    build_cache_image_view mapped_build_cache;
    std::string persisted_transaction_value;
    baseline_fingerprint build_fingerprint_value{};
    bool build_fingerprint_available = false;
    project_generation_provenance generation_provenance_value;
    project_generation_native_segments generation_native_segments_value;

    friend class project_build_orchestrator;
    friend class project_manager;
    friend class source_frontend_generation;
};

// Storage-neutral Source Manager subset exposed to READY readers.
class project_source_read_view final {
public:
    project_source_read_view() noexcept = default;

    [[nodiscard]] std::size_t source_count() const noexcept {
        return project != nullptr ? project->source_count() : 0;
    }

    [[nodiscard]] std::string_view path(source_id source) const noexcept {
        return project != nullptr
            ? project->source_path(source)
            : std::string_view{};
    }

    [[nodiscard]] status find(
        std::string_view normalized_path,
        source_id& output) const noexcept {
        if (project == nullptr) {
            output = {};
            return {status_code::not_found};
        }
        return project->find_source(normalized_path, output);
    }

private:
    explicit project_source_read_view(const project_context& value) noexcept
        : project(&value) {}

    const project_context* project = nullptr;

    friend class project_read_view;
};

// Storage-neutral Graph summary exposed to READY readers. Semantic queries remain
// on project_read_view so no pointer-returning Graph API leaks mmap ownership.
class project_graph_read_view final {
public:
    project_graph_read_view() noexcept = default;

    [[nodiscard]] std::size_t type_count() const noexcept {
        return project != nullptr ? project->type_count() : 0;
    }

    [[nodiscard]] std::size_t object_count() const noexcept {
        return project != nullptr ? project->object_count() : 0;
    }

    [[nodiscard]] std::size_t link_count() const noexcept {
        return project != nullptr ? project->link_count() : 0;
    }

private:
    explicit project_graph_read_view(const project_context& value) noexcept
        : project(&value) {}

    const project_context* project = nullptr;

    friend class project_read_view;
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

    [[nodiscard]] project_source_read_view sources() const noexcept {
        return project != nullptr
            ? project_source_read_view{*project}
            : project_source_read_view{};
    }

    [[nodiscard]] project_graph_read_view compiled_graph() const noexcept {
        return project != nullptr
            ? project_graph_read_view{*project}
            : project_graph_read_view{};
    }

    [[nodiscard]] bool baseline_backed() const noexcept {
        return project != nullptr && project->baseline_backed();
    }

    [[nodiscard]] bool build_cache_mapped() const noexcept {
        return project != nullptr && project->build_cache_mapped();
    }

    [[nodiscard]] std::string_view baseline_transaction() const noexcept {
        return project != nullptr
            ? project->baseline_transaction()
            : std::string_view{};
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

    [[nodiscard]] status find_type(
        std::string_view path,
        type_handle& output) const noexcept {
        return project->find_type(path, output);
    }

    [[nodiscard]] status find_object(
        std::string_view path,
        object_handle& output) const noexcept {
        return project->find_object(path, output);
    }

    [[nodiscard]] status find_endpoint(
        std::string_view path,
        object_endpoint& output) const noexcept {
        return project->find_endpoint(path, output);
    }

    [[nodiscard]] status find_link(
        std::string_view path,
        link_handle& output) const noexcept {
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
