#include "project_manager.hpp"

#include <cstdio>
#include <chrono>
#include <exception>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
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

    baseline_store store{configuration_path};
    baseline_probe probe;
    baseline_snapshot snapshot;

    auto result = store.open_current_ready(
        probe,
        snapshot);

    project_configuration configuration;
    bool fast_configuration = false;

    if (result.ok() &&
        probe.configuration.available &&
        probe.configuration.change_token_available &&
        probe.configuration.content_hash_available) {

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

        bool unchanged = false;
        const auto proof = prove_file_unchanged(
            configuration_path,
            probe.configuration.change_token,
            unchanged);

        if (proof.ok() && unchanged) {
            fast_configuration = true;
        }
        else if (!proof.ok() &&
                 proof.code != status_code::not_found) {
            abandon_construction();
            return proof;
        }
    }

    if (!fast_configuration) {
        configuration = {};

        result = load_project_configuration_file(
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

        if (snapshot.valid()) {
            if (!(fingerprint == probe.fingerprint)) {
                abandon_construction();
                return {status_code::rebuild_required};
            }
        }
        else {
            result = store.open_ready(
                fingerprint,
                snapshot);
            if (!result.ok()) {
                abandon_construction();
                return result;
            }

            if (snapshot.mapped(
                    baseline_artifact_kind::build_cache) ||
                !snapshot.mapped(
                    baseline_artifact_kind::compiled) ||
                !snapshot.mapped(
                    baseline_artifact_kind::source_manager)) {
                abandon_construction();
                return {status_code::initialization_failed};
            }

            return activate_baseline_reserved(
                std::move(configuration),
                configuration_path,
                std::move(snapshot),
                &output);
        }
    }

    if (!snapshot.mapped(
            baseline_artifact_kind::compiled)) {
        abandon_construction();
        return {status_code::initialization_failed};
    }


    if (snapshot.mapped(
            baseline_artifact_kind::build_cache) ||
        !snapshot.mapped(
            baseline_artifact_kind::compiled)) {
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
    std::size_t worker_limit,
    std::size_t acquisition_worker_limit) noexcept {

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

    const auto baseline_open_begin =
        std::chrono::steady_clock::now();

    auto result = store.open_current_decision(
        probe,
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

    const std::uint64_t configuration_probe_ns = 0;

    const auto configuration_identity_begin =
        std::chrono::steady_clock::now();

    bool fast_configuration = false;

    if (result.ok() &&
        probe.configuration.available &&
        probe.configuration.change_token_available &&
        probe.configuration.content_hash_available) {

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

        // Provisional only. The same Source USN pass below must prove that
        // project.json had no event since the persisted Source checkpoint.
        fast_configuration = true;
    }

    const auto configuration_identity_end =
        std::chrono::steady_clock::now();
    const auto configuration_identity_ns =
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<
                std::chrono::nanoseconds>(
                configuration_identity_end -
                configuration_identity_begin).count());

    const auto configuration_gate_ns =
        configuration_identity_ns;

    if (!fast_configuration) {
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
        baseline_open_ns +=
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
                    acquisition_worker_limit,
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

    change_state_image_view changes;
    result = changes.bind(
        snapshot.artifact(
            baseline_artifact_kind::change_state));
    if (!result.ok()) {
        abandon_construction();
        return result;
    }

    // This is the Generation epoch, not a SAVE epoch. Capture it before dirty
    // detection can read Source contents so every later filesystem event remains
    // visible to the next BUILD even if SAVE happens much later.
    source_change_checkpoint generation_checkpoint;
    std::string generation_anchor;

    try {
        const auto anchor =
            changes.path(source_id{1});

        if (!anchor.empty()) {
            generation_anchor.assign(anchor);

            if (changes.change_checkpoint()) {
                const auto checkpoint_result =
                    capture_source_change_checkpoint(
                        std::filesystem::path{
                            generation_anchor},
                        generation_checkpoint);

                if (!checkpoint_result.ok() &&
                    checkpoint_result.code !=
                        status_code::not_found) {
                    abandon_construction();
                    return checkpoint_result;
                }
            }
        }
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
        generation_checkpoint = {};
    }

    std::vector<source_id> dirty_sources;
    project_dirty_source_telemetry dirty_telemetry;
    std::vector<source_change_journal_candidate> journal_candidates;
    bool configuration_proven = !fast_configuration;
    bool configuration_changed = false;

    source_manager_image_view full_sources;
    const auto full_source_fallback = [&]() noexcept -> status {
        if (!snapshot.mapped(baseline_artifact_kind::source_manager)) {
            baseline_open_telemetry deferred_sources;
            const auto source_map_begin = std::chrono::steady_clock::now();
            auto map_result = store.map_source_manager_cached(
                probe.fingerprint,
                probe.transaction,
                probe.source_manager_size,
                snapshot,
                &deferred_sources);
            const auto source_map_end = std::chrono::steady_clock::now();
            baseline_open_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    source_map_end - source_map_begin).count());
            baseline_open_detail.source_manager_map_ns += deferred_sources.source_manager_map_ns;
            baseline_open_detail.size_validation_ns += deferred_sources.size_validation_ns;
            if (!map_result.ok())
                return map_result;
        }
        if (!full_sources.valid()) {
            auto bind_result = full_sources.bind(
                snapshot.artifact(baseline_artifact_kind::source_manager));
            if (!bind_result.ok())
                return bind_result;
        }
        if (!journal_candidates.empty()) {
            auto candidate_result =
                project_baseline_resolve_candidates(
                    full_sources,
                    journal_candidates,
                    dirty_sources,
                    dirty_telemetry);

            if (candidate_result.ok())
                return {};

            if (candidate_result.code !=
                status_code::not_found) {
                return candidate_result;
            }
        }

        dirty_telemetry.fallback = true;
        return project_baseline_dirty_sources(
            full_sources,
            dirty_sources);
    };

    if (fast_configuration) {
        bool configuration_path_identity_same = false;
        const auto identity_result =
            same_file_identity(
                configuration_path,
                probe.configuration.change_token,
                configuration_path_identity_same);

        if (identity_result.ok() &&
            configuration_path_identity_same) {
            result = project_baseline_dirty_sources(
                changes,
                probe.configuration.change_token,
                journal_candidates,
                configuration_proven,
                configuration_changed,
                dirty_telemetry);
        }
        else if (identity_result.code ==
                 status_code::not_found ||
                 (identity_result.ok() &&
                  !configuration_path_identity_same)) {
            // The saved file reference alone is insufficient after a parent
            // directory rename/replacement. Force content validation by path.
            configuration_proven = false;
            configuration_changed = false;
            result = project_baseline_dirty_sources(
                changes,
                journal_candidates,
                dirty_telemetry);
        }
        else {
            result = identity_result;
        }
    }
    else {
        result = project_baseline_dirty_sources(
            changes,
            dirty_sources,
            dirty_telemetry);
    }

    if (result.ok() &&
        !journal_candidates.empty()) {
        result = full_source_fallback();
    }
    else if (result.code == status_code::not_found) {
        journal_candidates.clear();
        result = full_source_fallback();
    }

    if (!result.ok()) {
        abandon_construction();
        return result;
    }

    if (fast_configuration &&
        (!configuration_proven ||
         configuration_changed)) {

        file_snapshot configuration_file;
        const auto acquisition =
            acquire_file_snapshot(
                configuration_path,
                std::nullopt,
                configuration_file);

        if (acquisition ==
                file_snapshot_result::allocation_failed) {
            abandon_construction();
            return {status_code::not_available};
        }
        if (acquisition ==
                file_snapshot_result::missing) {
            abandon_construction();
            return {status_code::not_found};
        }
        if (acquisition !=
                file_snapshot_result::acquired) {
            abandon_construction();
            return {status_code::io_failed};
        }

        if (configuration_file.hash !=
            probe.configuration.content_hash) {

            diagnostic_buffer configuration_diagnostics;
            project_configuration current_configuration;

            result = load_project_configuration(
                configuration_file.bytes,
                configuration_path,
                operation,
                configuration_diagnostics,
                current_configuration);
            if (!result.ok()) {
                abandon_construction();
                return result;
            }

            baseline_fingerprint current_fingerprint;
            result = make_project_baseline_fingerprint(
                current_configuration,
                current_fingerprint);
            if (!result.ok()) {
                abandon_construction();
                return result;
            }

            if (!(current_fingerprint ==
                  probe.fingerprint)) {
                return construct_reserved(
                    std::move(current_configuration),
                    configuration_path,
                    operation,
                    diagnostics,
                    output,
                    worker_limit,
                    acquisition_worker_limit,
                    false);
            }

            configuration =
                std::move(current_configuration);
        }
    }

    const auto dirty_detection_end = std::chrono::steady_clock::now();
    const auto dirty_detection_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            dirty_detection_end - dirty_detection_begin).count());

    // A full-scan fallback can prove content equality but cannot prove that every
    // unchanged pathname still has the persisted file identity. Do not carry
    // exact D3D identity state across that boundary.
    if (!dirty_telemetry.fast_path ||
        dirty_telemetry.fallback) {
        generation_checkpoint = {};
    }

    const auto baseline_source_count =
        static_cast<std::uint64_t>(changes.source_count());
    const auto dirty_source_count =
        static_cast<std::uint64_t>(dirty_sources.size());

    std::uint64_t baseline_activation_ns = 0;
    std::uint64_t build_activation_ns = 0;

    const auto publish_manager_telemetry = [&](project_build_result& value) noexcept {
        value.telemetry.configuration_identity_ns =
            configuration_identity_ns;
        value.telemetry.configuration_probe_ns =
            configuration_probe_ns;
        value.telemetry.configuration_gate_ns =
            configuration_gate_ns;
        value.telemetry.configuration_ns = configuration_ns;
        value.telemetry.fingerprint_ns = fingerprint_ns;
        value.telemetry.baseline_open_ns = baseline_open_ns;
        value.telemetry.baseline_current_read_ns =
            baseline_open_detail.current_read_ns;
        value.telemetry.baseline_embedded_manifest_parse_ns =
            baseline_open_detail.embedded_manifest_parse_ns;
        value.telemetry.baseline_manifest_validation_ns =
            baseline_open_detail.manifest_validation_ns;
        value.telemetry.baseline_compiled_map_ns =
            baseline_open_detail.compiled_map_ns;
        value.telemetry.baseline_source_manager_map_ns =
            baseline_open_detail.source_manager_map_ns;
        value.telemetry.baseline_change_state_map_ns =
            baseline_open_detail.change_state_map_ns;
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

    if (!dirty_sources.empty() &&
        !snapshot.mapped(
            baseline_artifact_kind::source_manager)) {

        baseline_open_telemetry deferred_sources;
        const auto source_map_begin =
            std::chrono::steady_clock::now();

        result = store.map_source_manager_cached(
            probe.fingerprint,
            probe.transaction,
            probe.source_manager_size,
            snapshot,
            &deferred_sources);

        const auto source_map_end =
            std::chrono::steady_clock::now();

        baseline_open_ns +=
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<
                    std::chrono::nanoseconds>(
                    source_map_end -
                    source_map_begin).count());

        baseline_open_detail.source_manager_map_ns +=
            deferred_sources.source_manager_map_ns;

        if (!result.ok()) {
            abandon_construction();
            return result;
        }
    }

    if (!snapshot.mapped(
            baseline_artifact_kind::build_cache)) {

        baseline_open_telemetry deferred_cache;
        const auto deferred_begin =
            std::chrono::steady_clock::now();

        result = store.map_build_cache_cached(
            probe.fingerprint,
            probe.transaction,
            probe.build_cache_size,
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
            std::fprintf(
                stderr,
                "[C1C2C-STAGE] activate_build_baseline code=%u\n",
                static_cast<unsigned>(result.code));
            publish_manager_telemetry(output);
            abandon_construction();
            return result;
        }

        if (generation_anchor.empty() &&
            !dirty_sources.empty()) {
            try {
                generation_anchor.assign(
                    candidate->sources().path(
                        dirty_sources.front()));
            }
            catch (const std::bad_alloc&) {
                publish_manager_telemetry(output);
                abandon_construction();
                return {status_code::not_available};
            }
            catch (const std::length_error&) {
                publish_manager_telemetry(output);
                abandon_construction();
                return {status_code::not_available};
            }
        }

        project_build_orchestrator builder{
            *candidate,
            worker_limit,
            acquisition_worker_limit};

        result = builder.update(
            dirty_sources,
            operation,
            diagnostics,
            output,
            generation_checkpoint,
            generation_anchor);
        if (!result.ok()) {
            std::fprintf(
                stderr,
                "[C1C2C-STAGE] builder.update code=%u dirty=%zu "
                "frontend_ns=%llu builder_ns=%llu source_prepare_ns=%llu "
                "interface_prepare_ns=%llu\n",
                static_cast<unsigned>(result.code),
                dirty_sources.size(),
                static_cast<unsigned long long>(
                    output.telemetry.frontend_ns),
                static_cast<unsigned long long>(
                    output.telemetry.builder_prepare_ns),
                static_cast<unsigned long long>(
                    output.telemetry.source_prepare_publish_ns),
                static_cast<unsigned long long>(
                    output.telemetry.interface_prepare_publish_ns));
            publish_manager_telemetry(output);
            abandon_construction();
            return result;
        }

        if (!output.changed) {
            candidate->remember_persisted_transaction(
                candidate->baseline_transaction());
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
    std::size_t worker_limit,
    std::size_t acquisition_worker_limit) noexcept {

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
                    acquisition_worker_limit,
        true);
}

status project_manager::rebuild(
    project_configuration configuration,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    project_build_result& output,
    std::size_t worker_limit,
    std::size_t acquisition_worker_limit) noexcept {

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
                    acquisition_worker_limit,
        true);
}

status project_manager::construct_reserved(
    project_configuration configuration,
    std::filesystem::path configuration_path,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    project_build_result& output,
    std::size_t worker_limit,
    std::size_t acquisition_worker_limit,
    bool mark_rebuild) noexcept {

    try {
        baseline_fingerprint build_fingerprint;
        auto fingerprint_result =
            make_project_baseline_fingerprint(
                configuration,
                build_fingerprint);
        if (!fingerprint_result.ok()) {
            abandon_construction();
            return fingerprint_result;
        }

        auto candidate = std::make_unique<project_context>(
            std::move(configuration),
            std::move(configuration_path));
        candidate->set_build_fingerprint(build_fingerprint);

        project_build_orchestrator builder{
            *candidate,
            worker_limit,
            acquisition_worker_limit};

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

    // Idempotent SAVE decision gate. A Project state already known to be
    // persisted may return without reading/parsing project.json when CURRENT
    // still selects that transaction and the persisted configuration token
    // proves the configuration file unchanged.
    const auto fast_persisted_transaction =
        project->persisted_transaction();

    if (!fast_persisted_transaction.empty()) {
        baseline_store fast_store{
            project->configuration_path()};

        baseline_probe current;
        auto fast_result = fast_store.probe(current);
        if (!fast_result.ok())
            return fast_result;

        const auto* fast_expected =
            project->build_fingerprint();
        if (fast_expected == nullptr)
            return {status_code::invalid_state};

        // Never republish or silently bless an older active state when another
        // transaction has become CURRENT.
        if (!(current.fingerprint == *fast_expected) ||
            current.transaction !=
                fast_persisted_transaction) {
            return {status_code::rebuild_required};
        }

        if (current.configuration.available &&
            current.configuration.change_token_available) {

            bool unchanged = false;
            fast_result = prove_file_unchanged(
                project->configuration_path(),
                current.configuration.change_token,
                unchanged);

            if (fast_result.ok() && unchanged) {
                try {
                    output.transaction =
                        current.transaction;
                }
                catch (const std::bad_alloc&) {
                    return {status_code::not_available};
                }
                catch (const std::length_error&) {
                    return {status_code::not_available};
                }

                output.bytes_written = 0;
                return {};
            }

            if (!fast_result.ok() &&
                fast_result.code !=
                    status_code::not_found) {
                return fast_result;
            }
        }

        // Legacy baseline, unavailable token, or a changed token: fall through
        // to the semantic content/fingerprint validation below.
    }

    file_change_token configuration_change_token;
    const auto token_result =
        capture_file_change_token(
            project->configuration_path(),
            configuration_change_token);

    if (!token_result.ok() &&
        token_result.code != status_code::not_found) {
        return token_result;
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

    const auto* expected =
        project->build_fingerprint();
    if (expected == nullptr ||
        !(*expected == fingerprint)) {
        return {status_code::rebuild_required};
    }

    baseline_configuration_state configuration_state;
    configuration_state.observation =
        configuration_file.observation;
    configuration_state.content_hash =
        configuration_file.hash;
    configuration_state.content_hash_available = true;

    if (token_result.ok()) {
        bool unchanged = false;
        const auto proof_result =
            prove_file_unchanged(
                project->configuration_path(),
                configuration_change_token,
                unchanged);

        if (!proof_result.ok()) {
            if (proof_result.code !=
                status_code::not_found) {
                return proof_result;
            }
        }
        else if (!unchanged) {
            return {status_code::rebuild_required};
        }
        else {
            configuration_state.change_token =
                configuration_change_token;
            configuration_state.change_token_available = true;
        }
    }
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

    const auto persisted_transaction =
        project->persisted_transaction();

    if (!persisted_transaction.empty()) {
        baseline_probe current;
        result = store.probe(current);
        if (!result.ok())
            return result;

        // Do not republish an older active state over a different CURRENT.
        // A matching CURRENT proves this SAVE is already durable.
        if (!(current.fingerprint == fingerprint) ||
            current.transaction != persisted_transaction) {
            return {status_code::rebuild_required};
        }

        try {
            output.transaction = current.transaction;
        }
        catch (const std::bad_alloc&) {
            return {status_code::not_available};
        }
        catch (const std::length_error&) {
            return {status_code::not_available};
        }

        output.bytes_written = 0;
        return {};
    }

    if (project->construction_backed()) {
        project_generation_storage generation;
        result = freeze_project_generation(
                *project,
                configuration,
                generation);
        if (!result.ok())
            return result;

        result = store.commit(
            fingerprint,
            configuration_state,
            generation.segments(),
            output);

        if (result.ok())
            project->remember_persisted_transaction(output.transaction);
        return result;
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

    result = store.commit(
        fingerprint,
        configuration_state,
        active.segments(),
        output);

    if (result.ok())
        project->remember_persisted_transaction(output.transaction);
    return result;
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
