#include "project_manager.hpp"

#include <chrono>
#include <exception>
#include <new>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace cw::server {

project_manager::~project_manager() noexcept {
    activity.close();

    if (activity.active_count() != 0)
        std::terminate();

    project.reset();
    lifecycle.store(
        project_lifecycle_state::unloaded,
        std::memory_order_release);
}

bool project_manager::reserve_construction() noexcept {
    auto expected = project_lifecycle_state::unloaded;
    return lifecycle.compare_exchange_strong(
        expected,
        project_lifecycle_state::constructing,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

void project_manager::abandon_construction() noexcept {
    project.reset();
    lifecycle.store(
        project_lifecycle_state::unloaded,
        std::memory_order_release);
}

status project_manager::activate_baseline_reserved(
    project_configuration configuration,
    std::filesystem::path configuration_path,
    baseline_snapshot&& snapshot,
    project_load_result* load_output) noexcept {

    try {
        auto candidate = std::unique_ptr<project_context>{
            new project_context(
                std::move(configuration),
                std::move(configuration_path),
                project_context::baseline_storage_tag{})};

        const auto activation =
            candidate->activate_ready_baseline(std::move(snapshot));
        if (!activation.ok()) {
            abandon_construction();
            return activation;
        }

        if (load_output != nullptr) {
            load_output->transaction.assign(
                candidate->baseline_transaction());
            load_output->build_cache_mapped =
                candidate->build_cache_mapped();
        }

        project = std::move(candidate);
        activity.open();
        lifecycle.store(
            project_lifecycle_state::ready,
            std::memory_order_release);
        return {};
    }
    catch (const std::bad_alloc&) {
        abandon_construction();
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        abandon_construction();
        return {status_code::not_available};
    }
    catch (const std::system_error&) {
        abandon_construction();
        return {status_code::not_available};
    }
}

status project_manager::load(
    const std::filesystem::path& configuration_path,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    project_load_result& output) noexcept {

    output = {};
    if (!reserve_construction())
        return {status_code::invalid_state};

    project_configuration configuration;
    auto result = load_project_configuration_file(
        configuration_path,
        operation,
        diagnostics,
        configuration);

    if (!result.ok()) {
        abandon_construction();
        return result;
    }

    baseline_fingerprint fingerprint;
    result = make_project_baseline_fingerprint(
        configuration,
        fingerprint);
    if (!result.ok()) {
        abandon_construction();
        return result;
    }

    baseline_store store{configuration_path};
    baseline_snapshot snapshot;
    result = store.open_ready(fingerprint, snapshot);
    if (!result.ok()) {
        abandon_construction();
        return result;
    }

    if (snapshot.mapped(baseline_artifact_kind::build_cache) ||
        !snapshot.mapped(baseline_artifact_kind::compiled) ||
        !snapshot.mapped(baseline_artifact_kind::source_manager)) {
        abandon_construction();
        return {status_code::initialization_failed};
    }

    return activate_baseline_reserved(
        std::move(configuration),
        configuration_path,
        std::move(snapshot),
        &output);
}

status project_manager::build(
    const std::filesystem::path& configuration_path,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    project_build_result& output,
    std::size_t worker_limit) noexcept {

    output = {};
    const auto manager_begin = std::chrono::steady_clock::now();

    if (!reserve_construction())
        return {status_code::invalid_state};

    project_configuration configuration;
    auto result = load_project_configuration_file(
        configuration_path,
        operation,
        diagnostics,
        configuration);

    if (!result.ok()) {
        abandon_construction();
        return result;
    }

    baseline_fingerprint fingerprint;
    result = make_project_baseline_fingerprint(
        configuration,
        fingerprint);
    if (!result.ok()) {
        abandon_construction();
        return result;
    }

    baseline_store store{configuration_path};
    baseline_snapshot snapshot;

    const auto baseline_open_begin = std::chrono::steady_clock::now();
    result = store.open(fingerprint, snapshot);
    const auto baseline_open_end = std::chrono::steady_clock::now();
    const auto baseline_open_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            baseline_open_end - baseline_open_begin).count());

    if (result.code == status_code::not_found ||
        result.code == status_code::rebuild_required) {
        return construct_reserved(
            std::move(configuration),
            configuration_path,
            operation,
            diagnostics,
            output,
            worker_limit,
            false);
    }

    if (!result.ok()) {
        abandon_construction();
        return result;
    }

    // BUILD change detection is intentionally Source-Manager-only. Full artifact
    // CRC/cross-image verification is a cold SAVE/test boundary; doing it here
    // would fault the complete baseline before a sparse update can begin.
    const auto dirty_detection_begin = std::chrono::steady_clock::now();

    source_manager_image_view sources;
    result = sources.bind(
        snapshot.artifact(baseline_artifact_kind::source_manager));
    if (!result.ok()) {
        abandon_construction();
        return result;
    }

    std::vector<source_id> dirty_sources;
    result = project_baseline_dirty_sources(
        sources,
        dirty_sources);
    if (!result.ok()) {
        abandon_construction();
        return result;
    }

    const auto dirty_detection_end = std::chrono::steady_clock::now();
    const auto dirty_detection_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            dirty_detection_end - dirty_detection_begin).count());

    const auto baseline_source_count =
        static_cast<std::uint64_t>(sources.source_count());
    const auto dirty_source_count =
        static_cast<std::uint64_t>(dirty_sources.size());

    const auto publish_manager_telemetry = [&](project_build_result& value) noexcept {
        value.telemetry.baseline_open_ns = baseline_open_ns;
        value.telemetry.dirty_detection_ns = dirty_detection_ns;
        value.telemetry.baseline_sources = baseline_source_count;
        value.telemetry.dirty_sources = dirty_source_count;
        value.telemetry.manager_total_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - manager_begin).count());
    };

    if (dirty_sources.empty()) {
        const auto activate_result = activate_baseline_reserved(
            std::move(configuration),
            configuration_path,
            std::move(snapshot),
            nullptr);
        publish_manager_telemetry(output);
        return activate_result;
    }

    try {
        auto candidate = std::unique_ptr<project_context>{
            new project_context(
                std::move(configuration),
                configuration_path,
                project_context::baseline_storage_tag{})};

        result = candidate->activate_build_baseline(
            std::move(snapshot));
        if (!result.ok()) {
            publish_manager_telemetry(output);
            abandon_construction();
            return result;
        }

        project_build_orchestrator builder{
            *candidate,
            worker_limit};

        result = builder.update(
            dirty_sources,
            operation,
            diagnostics,
            output);
        if (!result.ok()) {
            publish_manager_telemetry(output);
            abandon_construction();
            return result;
        }

        project = std::move(candidate);
        output.rebuilt = false;

        activity.open();
        lifecycle.store(
            project_lifecycle_state::ready,
            std::memory_order_release);

        publish_manager_telemetry(output);
        return {};
    }
    catch (const std::bad_alloc&) {
        abandon_construction();
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        abandon_construction();
        return {status_code::not_available};
    }
    catch (const std::system_error&) {
        abandon_construction();
        return {status_code::not_available};
    }
}

status project_manager::rebuild(
    const std::filesystem::path& configuration_path,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    project_build_result& output,
    std::size_t worker_limit) noexcept {

    output = {};
    if (!reserve_construction())
        return {status_code::invalid_state};

    project_configuration configuration;
    const auto configuration_result =
        load_project_configuration_file(
            configuration_path,
            operation,
            diagnostics,
            configuration);

    if (!configuration_result.ok()) {
        abandon_construction();
        return configuration_result;
    }

    return construct_reserved(
        std::move(configuration),
        configuration_path,
        operation,
        diagnostics,
        output,
        worker_limit,
        true);
}

status project_manager::rebuild(
    project_configuration configuration,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    project_build_result& output,
    std::size_t worker_limit) noexcept {

    output = {};
    if (!reserve_construction())
        return {status_code::invalid_state};

    return construct_reserved(
        std::move(configuration),
        {},
        operation,
        diagnostics,
        output,
        worker_limit,
        true);
}

status project_manager::construct_reserved(
    project_configuration configuration,
    std::filesystem::path configuration_path,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    project_build_result& output,
    std::size_t worker_limit,
    bool mark_rebuild) noexcept {

    try {
        auto candidate = std::make_unique<project_context>(
            std::move(configuration),
            std::move(configuration_path));

        project_build_orchestrator builder{
            *candidate,
            worker_limit};

        const auto result =
            builder.construct(
                operation,
                diagnostics,
                output);

        if (!result.ok()) {
            abandon_construction();
            return result;
        }

        project = std::move(candidate);
        output.rebuilt = mark_rebuild;

        activity.open();
        lifecycle.store(
            project_lifecycle_state::ready,
            std::memory_order_release);
        return {};
    }
    catch (const std::bad_alloc&) {
        abandon_construction();
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        abandon_construction();
        return {status_code::not_available};
    }
    catch (const std::system_error&) {
        abandon_construction();
        return {status_code::not_available};
    }
}

status project_manager::save(
    baseline_commit_result& output) noexcept {

    output = {};

    project_access access;
    if (!acquire(access).ok() || !access)
        return {status_code::invalid_state};

    if (project == nullptr ||
        project->configuration_path().empty()) {
        return {status_code::invalid_state};
    }

    baseline_fingerprint fingerprint;
    auto result = make_project_baseline_fingerprint(
        project->configuration(),
        fingerprint);
    if (!result.ok())
        return result;

    baseline_store store{
        project->configuration_path()};

    if (project->construction_backed()) {
        project_baseline_images images;
        result = encode_project_baseline(
            *project,
            images);
        if (!result.ok())
            return result;

        return store.commit(
            fingerprint,
            images.compiled,
            images.source_manager,
            images.build_cache,
            output);
    }

    baseline_snapshot active;
    result = store.open_transaction(
        fingerprint,
        project->baseline_transaction(),
        active);
    if (!result.ok())
        return result;

    compiled_image_view compiled;
    source_manager_image_view sources;
    build_cache_image_view cache;

    result = compiled.bind(
        active.artifact(baseline_artifact_kind::compiled));
    if (!result.ok())
        return result;

    result = sources.bind(
        active.artifact(baseline_artifact_kind::source_manager));
    if (!result.ok())
        return result;

    result = cache.bind(
        active.artifact(baseline_artifact_kind::build_cache));
    if (!result.ok())
        return result;

    result = compiled.verify_contents();
    if (!result.ok())
        return result;

    result = sources.verify_contents();
    if (!result.ok())
        return result;

    result = cache.verify_contents();
    if (!result.ok())
        return result;

    result = cache.verify_against(compiled, sources);
    if (!result.ok())
        return result;

    return store.commit(
        fingerprint,
        active.artifact(baseline_artifact_kind::compiled),
        active.artifact(baseline_artifact_kind::source_manager),
        active.artifact(baseline_artifact_kind::build_cache),
        output);
}

status project_manager::acquire(
    project_access& output) const noexcept {

    output.reset();

    if (lifecycle.load(std::memory_order_acquire) !=
        project_lifecycle_state::ready) {
        return {status_code::not_found};
    }

    if (!activity.try_enter())
        return {status_code::not_found};

    if (lifecycle.load(std::memory_order_acquire) !=
        project_lifecycle_state::ready) {
        activity.leave();
        return {status_code::not_found};
    }

    if (project == nullptr) {
        activity.leave();
        return {status_code::initialization_failed};
    }

    output = project_access{
        activity,
        *project};
    return {};
}

status project_manager::unload(
    project_stop_request stop) noexcept {

    auto expected = project_lifecycle_state::ready;
    if (!lifecycle.compare_exchange_strong(
            expected,
            project_lifecycle_state::draining,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return {status_code::invalid_state};
    }

    activity.close();

    stop();
    activity.wait_drained();

    project.reset();
    lifecycle.store(
        project_lifecycle_state::unloaded,
        std::memory_order_release);
    return {};
}

} // namespace cw::server
