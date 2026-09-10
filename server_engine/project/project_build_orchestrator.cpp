#include "project_build_orchestrator.hpp"

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
    std::vector<std::filesystem::path>& roots) noexcept {

    try {
        roots.clear();
        roots.reserve(configuration.project.size());
        for (const auto& item : configuration.project) {
            if (item.role == project_item_role::project)
                return {status_code::not_available};
            roots.push_back(item.path);
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

status project_build_orchestrator::rebuild(
    operation_id operation,
    diagnostic_buffer& diagnostics,
    project_build_result& output) noexcept {

    output = {};
    const auto total_begin = build_clock::now();

    try {
        std::vector<std::filesystem::path> roots;
        auto result = collect_roots(project.configuration(), roots);
        if (!result.ok())
            return result;

        auto source_update = project.sources().begin_update();
        source_frontend_generation frontend_builder{project, source_update, worker_limit};
        source_frontend_result frontend;

        const auto frontend_begin = build_clock::now();
        result = frontend_builder.build(roots, operation, diagnostics, frontend);
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

        generation_builder builder{project.contributions(), project.compiled_graph()};
        const auto builder_begin = build_clock::now();
        result = builder.prepare_g0(
            facts, project.configuration().abi, operation, diagnostics);
        const auto builder_end = build_clock::now();
        output.telemetry.builder_prepare_ns = elapsed_ns(builder_begin, builder_end);
        output.telemetry.builder = builder.telemetry();
        if (!result.ok())
            return result;

        auto cache_update = project.frontend_cache().begin_update(true);
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
        source_update.publish_prepared();
        builder.publish_prepared();
        const auto publish_end = build_clock::now();
        output.telemetry.publication_ns = elapsed_ns(publish_begin, publish_end);

        const auto interface_publish_begin = build_clock::now();
        cache_update.publish_prepared();
        const auto interface_publish_end = build_clock::now();
        output.telemetry.interface_publish_ns =
            elapsed_ns(interface_publish_begin, interface_publish_end);
        output.telemetry.total_ns = elapsed_ns(total_begin, interface_publish_end);
        output.telemetry.sources = source_update.telemetry();
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
    project_build_result& output) noexcept {

    output = {};
    const auto total_begin = build_clock::now();
    if (!project.frontend_cache().complete() || !project.contributions().complete())
        return {status_code::not_available};
    if (dirty_sources.empty())
        return {};

    try {
        auto source_update = project.sources().begin_update();
        source_frontend_generation frontend_builder{
            project, source_update, project.frontend_cache(), worker_limit};
        source_frontend_result frontend;

        const auto frontend_begin = build_clock::now();
        auto result = frontend_builder.build_incremental(
            dirty_sources, operation, diagnostics, frontend);
        const auto frontend_end = build_clock::now();
        output.telemetry.frontend_ns = elapsed_ns(frontend_begin, frontend_end);
        output.telemetry.frontend = frontend.summary();
        if (!result.ok())
            return result;

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
            output.changed = false;
            return {};
        }

        std::vector<source_facts> replacements;
        std::vector<source_id> removals;
        result = collect_builder_inputs(
            frontend, project.contributions(), replacements, removals);
        if (!result.ok())
            return result;

        generation_builder builder{project.contributions(), project.compiled_graph()};
        if (!replacements.empty() || !removals.empty()) {
            const auto builder_begin = build_clock::now();
            result = builder.prepare_incremental(
                replacements, removals, project.configuration().abi, operation, diagnostics);
            const auto builder_end = build_clock::now();
            output.telemetry.builder_prepare_ns = elapsed_ns(builder_begin, builder_end);
            output.telemetry.builder = builder.telemetry();
            if (!result.ok())
                return result;
        }

        auto cache_update = project.frontend_cache().begin_update(false);
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
        source_update.publish_prepared();
        builder.publish_prepared();
        const auto publish_end = build_clock::now();
        output.telemetry.publication_ns = elapsed_ns(publish_begin, publish_end);

        const auto interface_publish_begin = build_clock::now();
        cache_update.publish_prepared();
        const auto interface_publish_end = build_clock::now();
        output.telemetry.interface_publish_ns =
            elapsed_ns(interface_publish_begin, interface_publish_end);
        output.telemetry.total_ns = elapsed_ns(total_begin, interface_publish_end);
        output.telemetry.sources = source_update.telemetry();
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
