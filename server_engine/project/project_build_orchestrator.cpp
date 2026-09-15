#include "project_build_orchestrator.hpp"
#include "generation/project_generation_freeze.hpp"

#include <cstdio>
#include <chrono>
#include <new>
#include <stdexcept>
#include <vector>

namespace cw::server {
namespace {

using build_clock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t elapsed_ns(
    build_clock::time_point begin,
    build_clock::time_point end) noexcept {

    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
}

[[nodiscard]] status collect_roots(
    const project_configuration& configuration,
    std::vector<std::filesystem::path>& roots,
    std::vector<project_item_role>& root_roles,
    bool& roots_are_canonical) noexcept {

    try {
        roots.clear();
        root_roles.clear();
        roots.reserve(configuration.project.size());
        root_roles.reserve(configuration.project.size());
        roots_are_canonical = true;
        for (const auto& item : configuration.project) {
            if (item.role == project_item_role::project)
                return {status_code::not_available};
            roots_are_canonical =
                roots_are_canonical &&
                item.canonical_path;
            roots.push_back(item.path);
            root_roles.push_back(item.role);
        }
        return roots.empty() ? status{status_code::invalid_argument} : status{};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

[[nodiscard]] status collect_builder_inputs(
    const source_frontend_result& frontend,
    const source_contribution_cache& committed,
    std::vector<source_facts>& replacements,
    std::vector<source_id>& removals) noexcept {

    try {
        replacements.clear();
        removals.clear();
        replacements.reserve(frontend.sources().size());
        removals.reserve(frontend.sources().size());
        for (const auto& entry : frontend.sources()) {
            if (entry.removed) {
                if (committed.state(entry.source) != nullptr)
                    removals.push_back(entry.source);
            } else {
                if (!entry.parsed)
                    return {status_code::invalid_argument};
                const auto facts = entry.parsed.facts();
                if (!committed.equivalent(facts))
                    replacements.push_back(facts);
            }
        }
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}




} // namespace

status project_build_orchestrator::construct(
    operation_id operation,
    diagnostic_buffer& diagnostics,
    project_build_result& output) noexcept {

    return rebuild_current(operation, diagnostics, output);
}

status project_build_orchestrator::rebuild(
    operation_id operation,
    diagnostic_buffer& diagnostics,
    project_build_result& output) noexcept {

    output = {};
    try {
        project_context candidate{project.configuration()};

        // GEN-02C19: an internal semantic rebuild uses the same in-memory
        // configuration. Preserve its Generation proof; SAVE will still prove
        // the file token unchanged before using it.
        if (const auto* configuration_proof =
                project.generation_configuration_proof()) {
            candidate.publish_generation_configuration_proof(
                *configuration_proof);
        }

        project_build_orchestrator detached{candidate, worker_limit};
        auto result = detached.rebuild_current(operation, diagnostics, output);
        if (!result.ok())
            return result;
        project.replace_compiled(candidate.release_compiled());
        project.replace_generation_provenance(
            candidate.release_generation_provenance());
        project.replace_generation_native_segments(
            candidate.release_generation_native_segments());
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

status project_build_orchestrator::rebuild_current(
    operation_id operation,
    diagnostic_buffer& diagnostics,
    project_build_result& output) noexcept {

    output = {};
    output.telemetry.storage_before = project.storage_pressure();
    const auto total_begin = build_clock::now();

    try {
        std::vector<std::filesystem::path> roots;
        std::vector<project_item_role> root_roles;
        bool roots_are_canonical = false;
        auto result = collect_roots(
            project.configuration(),
            roots,
            root_roles,
            roots_are_canonical);
        if (!result.ok())
            return result;

        auto& state = project.mutable_compiled();

        source_change_checkpoint generation_checkpoint;
        std::string generation_anchor;

        try {
            if (normalize_source_path(
                    roots.front(),
                    generation_anchor).ok()) {
                const auto checkpoint_result =
                    capture_source_change_checkpoint(
                        std::filesystem::path{
                            generation_anchor},
                        generation_checkpoint);

                if (!checkpoint_result.ok())
                    generation_checkpoint = {};
            }
        }
        catch (const std::bad_alloc&) {
            generation_checkpoint = {};
            generation_anchor.clear();
        }
        catch (const std::length_error&) {
            generation_checkpoint = {};
            generation_anchor.clear();
        }
        catch (const std::system_error&) {
            generation_checkpoint = {};
            generation_anchor.clear();
        }

auto semantic = project.parser_services();
        auto source_update = state.sources.begin_update();
        source_frontend_generation frontend_builder{
            semantic,
            source_update,
            worker_limit,
            acquisition_worker_limit};
        source_frontend_result frontend;

        const auto frontend_begin = build_clock::now();
        result = frontend_builder.build(
            roots,
            operation,
            diagnostics,
            frontend,
            roots_are_canonical);
        const auto frontend_end = build_clock::now();
        output.telemetry.frontend_ns = elapsed_ns(frontend_begin, frontend_end);
        output.telemetry.frontend = frontend.summary();
        if (!result.ok())
            return result;

        std::vector<source_facts> facts;
        facts.reserve(frontend.sources().size());
        for (const auto& entry : frontend.sources()) {
            if (!entry.parsed)
                return {status_code::invalid_argument};
            facts.push_back(entry.parsed.facts());
        }

        generation_builder builder{state.contributions, state.graph_value};
        const auto builder_begin = build_clock::now();
        result = builder.prepare_g0(
            facts, project.configuration().abi, operation, diagnostics);
        const auto builder_end = build_clock::now();
        output.telemetry.builder_prepare_ns = elapsed_ns(builder_begin, builder_end);
        output.telemetry.builder = builder.telemetry();
        if (!result.ok())
            return result;

        auto cache_update = state.frontend_cache.begin_update(true);
        for (auto& entry : frontend.entries) {
            result = cache_update.replace(entry.source, std::move(entry.interface));
            if (!result.ok())
                return result;
        }

        const auto source_prepare_begin = build_clock::now();
        result = source_update.prepare_publish();
        const auto source_prepare_end = build_clock::now();
        output.telemetry.source_prepare_publish_ns =
            elapsed_ns(source_prepare_begin, source_prepare_end);
        if (!result.ok())
            return result;

        const auto cache_prepare_begin = build_clock::now();
        result = cache_update.prepare_publish(source_update.source_count());
        const auto cache_prepare_end = build_clock::now();
        output.telemetry.interface_prepare_publish_ns =
            elapsed_ns(cache_prepare_begin, cache_prepare_end);
        if (!result.ok())
            return result;

        const auto publish_begin = build_clock::now();
        const auto interface_publish_begin = build_clock::now();
        build_clock::time_point publish_end;
        build_clock::time_point interface_publish_end;
        source_update.publish_prepared();
        builder.publish_prepared();
        publish_end = build_clock::now();
        cache_update.publish_prepared();
        source_change_capture generation_change;
        auto provenance_result =
            prepare_generation_source_change_capture(
                state.sources,
                generation_checkpoint,
                generation_anchor,
                generation_change);

        if (!provenance_result.ok() &&
            provenance_result.code ==
                status_code::not_found) {
            provenance_result =
                prepare_disabled_generation_source_change_capture(
                    generation_anchor,
                    generation_change);
        }

        if (!provenance_result.ok())
            return provenance_result;

        std::vector<std::byte> change_segment;
        const auto segment_result =
            freeze_generation_change_segment(
                state.sources.source_count(),
                generation_change,
                change_segment);

        project.publish_generation_source_change(
            std::move(generation_change));

        if (segment_result.ok()) {
            project.publish_generation_change_segment(
                std::move(change_segment));
        }
        else {
            project.clear_generation_change_segment();
        }

        // GEN-02C19: the Generation owns both root Source identities and
        // roles. The Source identities are still moved directly from frontend
        // storage; roles were collected during the existing configuration pass.
        auto resolved_roots =
            frontend.release_roots();

        if (resolved_roots.size() !=
            root_roles.size()) {
            return {status_code::initialization_failed};
        }

        project.publish_generation_roots(
            std::move(resolved_roots),
            std::move(root_roles));

        interface_publish_end = build_clock::now();
        output.telemetry.publication_ns = elapsed_ns(publish_begin, publish_end);
        output.telemetry.interface_publish_ns =
            elapsed_ns(interface_publish_begin, interface_publish_end);
        output.telemetry.total_ns = elapsed_ns(total_begin, interface_publish_end);
        output.telemetry.sources = source_update.telemetry();
        output.telemetry.storage_after = project.storage_pressure();
        output.changed = true;
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

status project_build_orchestrator::update(
    std::span<const source_id> dirty_sources,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    project_build_result& output,
    source_change_checkpoint generation_checkpoint,
    std::string_view generation_anchor) noexcept {

    output = {};
    output.telemetry.storage_before = project.storage_pressure();
    if (output.telemetry.storage_before.rebuild_recommended)
        return {status_code::rebuild_required};

    const auto total_begin = build_clock::now();
    auto& state = project.mutable_compiled();
    if (!state.frontend_cache.complete() || !state.contributions.complete())
        return {status_code::not_available};
    if (dirty_sources.empty())
        return {};

    try {
        auto semantic = project.parser_services();
        auto source_update = state.sources.begin_update();
        source_frontend_generation frontend_builder{
            semantic,
            source_update,
            state.frontend_cache,
            worker_limit,
            acquisition_worker_limit};
        source_frontend_result frontend;

        const auto frontend_begin = build_clock::now();
        auto result = frontend_builder.build_incremental(
            dirty_sources, operation, diagnostics, frontend);
        const auto frontend_end = build_clock::now();
        output.telemetry.frontend_ns = elapsed_ns(frontend_begin, frontend_end);
        output.telemetry.frontend = frontend.summary();
        if (!result.ok()) {
            std::fprintf(
                stderr,
                "[C1C2C-STAGE] frontend.build_incremental code=%u\n",
                static_cast<unsigned>(result.code));
            return result;
        }

        if (frontend.sources().empty()) {
            const auto source_prepare_begin = build_clock::now();
            result = source_update.prepare_publish();
            const auto source_prepare_end = build_clock::now();
            output.telemetry.source_prepare_publish_ns =
                elapsed_ns(source_prepare_begin, source_prepare_end);
            if (!result.ok())
                return result;

            const auto publish_begin = build_clock::now();
            source_update.publish_prepared();
            const auto publish_end = build_clock::now();
            output.telemetry.publication_ns = elapsed_ns(publish_begin, publish_end);
            output.telemetry.total_ns = elapsed_ns(total_begin, publish_end);
            output.telemetry.sources = source_update.telemetry();
            output.telemetry.storage_after = project.storage_pressure();
            output.changed = false;
            return {};
        }

        std::vector<source_facts> replacements;
        std::vector<source_id> removals;
        result = collect_builder_inputs(
            frontend, state.contributions, replacements, removals);
        if (!result.ok()) {
            std::fprintf(
                stderr,
                "[C1C2C-STAGE] collect_builder_inputs code=%u\n",
                static_cast<unsigned>(result.code));
            return result;
        }

        generation_builder builder{state.contributions, state.graph_value};
        if (!replacements.empty() || !removals.empty()) {
            const auto builder_begin = build_clock::now();
            result = builder.prepare_incremental(
                replacements, removals, project.configuration().abi, operation, diagnostics);
            const auto builder_end = build_clock::now();
            output.telemetry.builder_prepare_ns = elapsed_ns(builder_begin, builder_end);
            output.telemetry.builder = builder.telemetry();
            if (!result.ok()) {
                std::fprintf(
                    stderr,
                    "[C1C2C-STAGE] builder.prepare_incremental code=%u\n",
                    static_cast<unsigned>(result.code));
                return result;
            }
        }

        auto cache_update = state.frontend_cache.begin_update(false);
        for (auto& entry : frontend.entries) {
            result = cache_update.replace(
                entry.source,
                entry.removed ? std::unique_ptr<source_interface>{} : std::move(entry.interface));
            if (!result.ok())
                return result;
        }

        const auto source_prepare_begin = build_clock::now();
        result = source_update.prepare_publish();
        const auto source_prepare_end = build_clock::now();
        output.telemetry.source_prepare_publish_ns =
            elapsed_ns(source_prepare_begin, source_prepare_end);
        if (!result.ok()) {
            std::fprintf(
                stderr,
                "[C1C2C-STAGE] source_update.prepare_publish code=%u\n",
                static_cast<unsigned>(result.code));
            return result;
        }

        const auto cache_prepare_begin = build_clock::now();
        result = cache_update.prepare_publish(source_update.source_count());
        const auto cache_prepare_end = build_clock::now();
        output.telemetry.interface_prepare_publish_ns =
            elapsed_ns(cache_prepare_begin, cache_prepare_end);
        if (!result.ok()) {
            std::fprintf(
                stderr,
                "[C1C2C-STAGE] cache_update.prepare_publish code=%u\n",
                static_cast<unsigned>(result.code));
            return result;
        }

        source_change_capture generation_change;
        bool generation_change_ready = false;
        source_change_overlay_fallback_reason
            generation_change_fallback =
                source_change_overlay_fallback_reason::none;

        output.telemetry.generation_checkpoint_available =
            static_cast<bool>(generation_checkpoint);
        output.telemetry.generation_anchor_available =
            !generation_anchor.empty();

        if (!generation_anchor.empty()) {
            auto provenance_result =
                prepare_incremental_generation_source_change_capture(
                    source_update,
                    generation_checkpoint,
                    generation_anchor,
                    generation_change,
                    &generation_change_fallback);

            if (!provenance_result.ok() &&
                provenance_result.code ==
                    status_code::not_found) {
                provenance_result =
                    prepare_disabled_generation_source_change_capture(
                        generation_anchor,
                        generation_change);
            }

            if (!provenance_result.ok())
                return provenance_result;

            generation_change_ready = true;

            output.telemetry.generation_change_ready = true;
            output.telemetry.generation_change_overlay =
                generation_change.baseline_overlay();
            output.telemetry.generation_change_fallback_reason =
                static_cast<std::uint32_t>(
                    generation_change_fallback);
            output.telemetry.generation_change_file_updates =
                static_cast<std::uint64_t>(
                    generation_change.file_updates.size());
            output.telemetry.generation_change_directory_updates =
                static_cast<std::uint64_t>(
                    generation_change.directory_updates.size());
        }

        const auto publish_begin = build_clock::now();
        const auto interface_publish_begin = build_clock::now();
        build_clock::time_point publish_end;
        build_clock::time_point interface_publish_end;
        source_update.publish_prepared();
        builder.publish_prepared();
        publish_end = build_clock::now();
        cache_update.publish_prepared();
        interface_publish_end = build_clock::now();
        output.telemetry.publication_ns = elapsed_ns(publish_begin, publish_end);
        output.telemetry.interface_publish_ns =
            elapsed_ns(interface_publish_begin, interface_publish_end);
        output.telemetry.total_ns = elapsed_ns(total_begin, interface_publish_end);
        output.telemetry.sources = source_update.telemetry();
        output.telemetry.storage_after = project.storage_pressure();

        if (generation_change_ready) {
            project.publish_generation_source_change(
                std::move(generation_change));
        }
        else {
            project.clear_generation_source_change();
        }

        // Sparse provenance borrows the exact unchanged identity set from the
        // baseline. Its compact change-state segment is frozen only after the
        // cold SAVE merge; BUILD performs no O(N) identity serialization.
        project.clear_generation_change_segment();

        output.changed = true;
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

} // namespace cw::server
