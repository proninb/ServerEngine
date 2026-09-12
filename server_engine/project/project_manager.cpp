#include "project_manager.hpp"

#include <chrono>
#include <exception>
#include <new>
#include <optional>
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
    std::uint64_t configuration_ns = 0;
    std::uint64_t fingerprint_ns = 0;
    std::uint64_t baseline_open_ns = 0;

    baseline_store store{configuration_path};
    baseline_snapshot snapshot;
    baseline_probe probe;
    baseline_open_telemetry baseline_open_detail;

    const auto configuration_identity_begin =
        std::chrono::steady_clock::now();

    auto result = store.probe(probe);
    bool fast_configuration = false;

    if (result.ok() &&
        probe.configuration.available) {

        file_snapshot_observation observation;
        result = observe_project_configuration(
            configuration_path,
            observation);

        if (result.ok() &&
            observation ==
                probe.configuration.observation) {

            if (probe.configuration.project_version !=
                    current_project_configuration_version ||
                probe.configuration.abi_target >
                    static_cast<std::uint32_t>(
                        abi_target::posix_x64)) {
                abandon_construction();
                return {status_code::artifact_corrupt};
            }

            configuration.version =
                probe.configuration.project_version;
            configuration.materialized = false;
            configuration.abi.target =
                static_cast<abi_target>(
                    probe.configuration.abi_target);
            configuration.abi.pack =
                probe.configuration.abi_pack;

            if (!is_supported_abi_configuration(
                    configuration.abi)) {
                abandon_construction();
                return {status_code::artifact_corrupt};
            }

            fast_configuration = true;
        }
    }

    const auto configuration_identity_end =
        std::chrono::steady_clock::now();
    const auto configuration_identity_ns =
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<
                std::chrono::nanoseconds>(
                configuration_identity_end -
                configuration_identity_begin).count());

    if (fast_configuration) {
        const auto baseline_open_begin =
            std::chrono::steady_clock::now();

        result = store.open_transaction_ready(
            probe.fingerprint,
            probe.transaction,
            snapshot,
            &baseline_open_detail);

        const auto baseline_open_end =
            std::chrono::steady_clock::now();
        baseline_open_ns =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<
                    std::chrono::nanoseconds>(
                    baseline_open_end -
                    baseline_open_begin).count());
    }
    else {
        const auto configuration_begin =
            std::chrono::steady_clock::now();

        result = load_project_configuration_file(
            configuration_path,
            operation,
            diagnostics,
            configuration);

        const auto configuration_end =
            std::chrono::steady_clock::now();
        configuration_ns =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<
                    std::chrono::nanoseconds>(
                    configuration_end -
                    configuration_begin).count());

        if (!result.ok()) {
            abandon_construction();
            return result;
        }

        baseline_fingerprint fingerprint;
        const auto fingerprint_begin =
            std::chrono::steady_clock::now();

        result = make_project_baseline_fingerprint(
            configuration,
            fingerprint);

        const auto fingerprint_end =
            std::chrono::steady_clock::now();
        fingerprint_ns =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<
                    std::chrono::nanoseconds>(
                    fingerprint_end -
                    fingerprint_begin).count());

        if (!result.ok()) {
            abandon_construction();
            return result;
        }

        const auto baseline_open_begin =
            std::chrono::steady_clock::now();

        result = store.open(
            fingerprint,
            snapshot);

        const auto baseline_open_end =
            std::chrono::steady_clock::now();
        baseline_open_ns =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<
                    std::chrono::nanoseconds>(
                    baseline_open_end -
                    baseline_open_begin).count());

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
    project_dirty_source_telemetry dirty_telemetry;
    result = project_baseline_dirty_sources(
        sources,
        dirty_sources,
        dirty_telemetry);
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

    std::uint64_t baseline_activation_ns = 0;
    std::uint64_t build_activation_ns = 0;

    const auto publish_manager_telemetry = [&](project_build_result& value) noexcept {
        value.telemetry.configuration_identity_ns =
            configuration_identity_ns;
        value.telemetry.configuration_ns = configuration_ns;
        value.telemetry.fingerprint_ns = fingerprint_ns;
        value.telemetry.baseline_open_ns = baseline_open_ns;
        value.telemetry.baseline_manifest_validation_ns =
            baseline_open_detail.manifest_validation_ns;
        value.telemetry.baseline_compiled_map_ns =
            baseline_open_detail.compiled_map_ns;
        value.telemetry.baseline_source_manager_map_ns =
            baseline_open_detail.source_manager_map_ns;
        value.telemetry.baseline_build_cache_map_ns =
            baseline_open_detail.build_cache_map_ns;
        value.telemetry.baseline_size_validation_ns =
            baseline_open_detail.size_validation_ns;
        value.telemetry.dirty_detection_ns = dirty_detection_ns;
        value.telemetry.baseline_activation_ns = baseline_activation_ns;
        value.telemetry.build_activation_ns = build_activation_ns;
        value.telemetry.baseline_sources = baseline_source_count;
        value.telemetry.dirty_sources = dirty_source_count;
        value.telemetry.journal_records =
            dirty_telemetry.journal_records;
        value.telemetry.journal_matched_sources =
            dirty_telemetry.journal_matched_sources;
        value.telemetry.dirty_detection_backend =
            dirty_telemetry.backend;
        value.telemetry.dirty_detection_fast =
            dirty_telemetry.fast_path;
        value.telemetry.dirty_detection_fallback =
            dirty_telemetry.fallback;
        value.telemetry.manager_total_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - manager_begin).count());
    };

    if (dirty_sources.empty()) {
        const auto baseline_activation_begin =
            std::chrono::steady_clock::now();
        const auto activate_result = activate_baseline_reserved(
            std::move(configuration),
            configuration_path,
            std::move(snapshot),
            nullptr);
        const auto baseline_activation_end =
            std::chrono::steady_clock::now();
        baseline_activation_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                baseline_activation_end -
                baseline_activation_begin).count());


        publish_manager_telemetry(output);
        return activate_result;
    }

    if (!snapshot.mapped(
            baseline_artifact_kind::build_cache)) {

        baseline_open_telemetry deferred_cache;
        const auto deferred_begin =
            std::chrono::steady_clock::now();

        result = store.map_build_cache(
            probe.fingerprint,
            probe.transaction,
            snapshot,
            &deferred_cache);

        const auto deferred_end =
            std::chrono::steady_clock::now();

        baseline_open_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                deferred_end - deferred_begin).count());

        baseline_open_detail.manifest_validation_ns +=
            deferred_cache.manifest_validation_ns;
        baseline_open_detail.build_cache_map_ns +=
            deferred_cache.build_cache_map_ns;
        baseline_open_detail.size_validation_ns +=
            deferred_cache.size_validation_ns;

        if (!result.ok()) {
            publish_manager_telemetry(output);
            abandon_construction();
            return result;
        }
    }

    try {
        const auto build_activation_begin =
            std::chrono::steady_clock::now();

        auto candidate = std::unique_ptr<project_context>{
            new project_context(
                std::move(configuration),
                configuration_path,
                project_context::baseline_storage_tag{})};

        result = candidate->activate_build_baseline(
            std::move(snapshot));

        const auto build_activation_end =
            std::chrono::steady_clock::now();
        build_activation_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                build_activation_end -
                build_activation_begin).count());

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

    file_snapshot configuration_file;
    const auto acquisition =
        acquire_file_snapshot(
            project->configuration_path(),
            std::nullopt,
            configuration_file);

    if (acquisition !=
        file_snapshot_result::acquired) {

        if (acquisition ==
            file_snapshot_result::allocation_failed) {
            return {status_code::not_available};
        }

        if (acquisition ==
            file_snapshot_result::missing) {
            return {status_code::not_found};
        }

        return {status_code::io_failed};
    }

    diagnostic_buffer configuration_diagnostics;
    project_configuration configuration;
    auto result = load_project_configuration(
        configuration_file.bytes,
        project->configuration_path(),
        operation_id{},
        configuration_diagnostics,
        configuration);
    if (!result.ok())
        return result;

    baseline_fingerprint fingerprint;
    result = make_project_baseline_fingerprint(
        configuration,
        fingerprint);
    if (!result.ok())
        return result;

    if (project->baseline_backed()) {
        const auto* expected =
            project->baseline_fingerprint_value();
        if (expected == nullptr ||
            !(*expected == fingerprint)) {
            return {status_code::rebuild_required};
        }
    }

    baseline_configuration_state configuration_state;
    configuration_state.observation =
        configuration_file.observation;
    configuration_state.project_version =
        configuration.version;
    configuration_state.abi_target =
        static_cast<std::uint32_t>(
            configuration.abi.target);
    configuration_state.abi_pack =
        configuration.abi.pack;
    configuration_state.available = true;

    baseline_store store{
        project->configuration_path()};

    if (project->construction_backed()) {
        project_baseline_images images;
        result = encode_project_baseline(
            *project,
            configuration,
            images);
        if (!result.ok())
            return result;

        return store.commit(
            fingerprint,
            configuration_state,
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
        configuration_state,
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
