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

namespace {

[[nodiscard]] project_generation_configuration_proof
configuration_proof_from_baseline(
    const baseline_configuration_state& state) noexcept {

    project_generation_configuration_proof output;
    output.observation = state.observation;
    output.content_hash = state.content_hash;
    output.change_token = state.change_token;
    output.content_hash_available =
        state.content_hash_available;
    output.change_token_available =
        state.change_token_available;
    return output;
}

[[nodiscard]] status load_generation_configuration(
    const std::filesystem::path& configuration_path,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    project_configuration& configuration,
    project_generation_configuration_proof& proof,
    bool& proof_available) noexcept {

    configuration = {};
    proof = {};
    proof_available = false;

    file_change_token token;
    const auto token_result =
        capture_file_change_token(
            configuration_path,
            token);

    if (!token_result.ok() &&
        token_result.code != status_code::not_found) {
        return token_result;
    }

    file_snapshot snapshot;
    const auto acquisition =
        acquire_file_snapshot(
            configuration_path,
            std::nullopt,
            snapshot);

    if (acquisition ==
        file_snapshot_result::allocation_failed) {
        return {status_code::not_available};
    }
    if (acquisition ==
        file_snapshot_result::missing) {
        return {status_code::not_found};
    }
    if (acquisition !=
        file_snapshot_result::acquired) {
        return {status_code::io_failed};
    }

    auto result = load_project_configuration(
        snapshot.bytes,
        configuration_path,
        operation,
        diagnostics,
        configuration);
    if (!result.ok())
        return result;

    proof.observation = snapshot.observation;
    proof.content_hash = snapshot.hash;
    proof.content_hash_available = true;

    if (token_result.ok()) {
        bool unchanged = false;
        const auto token_proof =
            prove_file_unchanged(
                configuration_path,
                token,
                unchanged);

        if (!token_proof.ok()) {
            if (token_proof.code !=
                status_code::not_found) {
                return token_proof;
            }
        }
        else if (!unchanged) {
            // The bytes parsed above no longer identify the current
            // configuration file. Never publish a mixed Generation.
            return {status_code::rebuild_required};
        }
        else {
            proof.change_token = token;
            proof.change_token_available = true;
        }
    }

    proof_available = true;
    return {};
}

} // namespace

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
    project_generation_configuration_proof
        generation_configuration_proof;
    bool generation_configuration_proof_available = false;

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

        result = load_generation_configuration(
            configuration_path,
            operation,
            diagnostics,
            configuration,
            generation_configuration_proof,
            generation_configuration_proof_available);

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
                generation_configuration_proof_available
                    ? &generation_configuration_proof
                    : nullptr,
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
            auto bind_result =
                snapshot.bind_source_manager(
                    full_sources);
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
        configuration_proven &&
        !configuration_changed) {

        generation_configuration_proof =
            configuration_proof_from_baseline(
                probe.configuration);
        generation_configuration_proof_available =
            probe.configuration.available &&
            probe.configuration.content_hash_available;
    }

    if (fast_configuration &&
        (!configuration_proven ||
         configuration_changed)) {

        file_change_token current_configuration_token;
        const auto current_token_result =
            capture_file_change_token(
                configuration_path,
                current_configuration_token);

        if (!current_token_result.ok() &&
            current_token_result.code !=
                status_code::not_found) {
            abandon_construction();
            return current_token_result;
        }

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

        generation_configuration_proof = {};
        generation_configuration_proof.observation =
            configuration_file.observation;
        generation_configuration_proof.content_hash =
            configuration_file.hash;
        generation_configuration_proof.content_hash_available = true;
        generation_configuration_proof_available = true;

        if (current_token_result.ok()) {
            bool current_unchanged = false;
            const auto current_proof_result =
                prove_file_unchanged(
                    configuration_path,
                    current_configuration_token,
                    current_unchanged);

            if (!current_proof_result.ok()) {
                if (current_proof_result.code !=
                    status_code::not_found) {
                    abandon_construction();
                    return current_proof_result;
                }
            }
            else if (!current_unchanged) {
                abandon_construction();
                return {status_code::rebuild_required};
            }
            else {
                generation_configuration_proof.change_token =
                    current_configuration_token;
                generation_configuration_proof.change_token_available =
                    true;
            }
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
                    generation_configuration_proof_available
                        ? &generation_configuration_proof
                        : nullptr,
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

        if (generation_configuration_proof_available) {
            candidate->publish_generation_configuration_proof(
                generation_configuration_proof);
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
    project_generation_configuration_proof
        configuration_proof;
    bool configuration_proof_available = false;

    const auto configuration_result =
        load_generation_configuration(
            configuration_path,
            operation,
            diagnostics,
            configuration,
            configuration_proof,
            configuration_proof_available);

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
        configuration_proof_available
            ? &configuration_proof
            : nullptr,
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
        nullptr,
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
    const project_generation_configuration_proof* configuration_proof,
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

        if (configuration_proof != nullptr) {
            candidate->publish_generation_configuration_proof(
                *configuration_proof);
        }

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
    baseline_commit_result& output,
    std::size_t io_worker_budget) noexcept {

    output = {};

    const auto save_begin =
        std::chrono::steady_clock::now();

    std::uint64_t configuration_token_ns = 0;
    std::uint64_t configuration_read_ns = 0;
    std::uint64_t configuration_parse_ns = 0;
    std::uint64_t fingerprint_ns = 0;
    std::uint64_t generation_freeze_ns = 0;
    project_generation_freeze_telemetry generation_freeze_detail;

    const auto publish_save_telemetry = [&]() noexcept {
        output.telemetry.configuration_token_ns =
            configuration_token_ns;
        output.telemetry.configuration_read_ns =
            configuration_read_ns;
        output.telemetry.configuration_parse_ns =
            configuration_parse_ns;
        output.telemetry.fingerprint_ns =
            fingerprint_ns;
        output.telemetry.generation_freeze_ns =
            generation_freeze_ns;
        output.telemetry.generation_freeze_internal_ns =
            generation_freeze_detail.internal_ns;
        output.telemetry.generation_freeze_materialize_change_ns =
            generation_freeze_detail.materialize_change_ns;
        output.telemetry.generation_freeze_materialize_change_update_index_allocate_zero_ns =
            generation_freeze_detail.materialize_change_update_index_allocate_zero_ns;
        output.telemetry.generation_freeze_materialize_change_baseline_file_count_ns =
            generation_freeze_detail.materialize_change_baseline_file_count_ns;
        output.telemetry.generation_freeze_materialize_change_file_index_allocate_zero_ns =
            generation_freeze_detail.materialize_change_file_index_allocate_zero_ns;
        output.telemetry.generation_freeze_materialize_change_baseline_file_merge_ns =
            generation_freeze_detail.materialize_change_baseline_file_merge_ns;
        output.telemetry.generation_freeze_materialize_change_sparse_file_updates_ns =
            generation_freeze_detail.materialize_change_sparse_file_updates_ns;
        output.telemetry.generation_freeze_materialize_change_baseline_directory_count_ns =
            generation_freeze_detail.materialize_change_baseline_directory_count_ns;
        output.telemetry.generation_freeze_materialize_change_directory_index_allocate_zero_ns =
            generation_freeze_detail.materialize_change_directory_index_allocate_zero_ns;
        output.telemetry.generation_freeze_materialize_change_baseline_directory_merge_ns =
            generation_freeze_detail.materialize_change_baseline_directory_merge_ns;
        output.telemetry.generation_freeze_materialize_change_sparse_directory_updates_ns =
            generation_freeze_detail.materialize_change_sparse_directory_updates_ns;
        output.telemetry.generation_freeze_materialize_change_source_count =
            generation_freeze_detail.materialize_change_source_count;
        output.telemetry.generation_freeze_materialize_change_baseline_file_capacity =
            generation_freeze_detail.materialize_change_baseline_file_capacity;
        output.telemetry.generation_freeze_materialize_change_baseline_file_occupied =
            generation_freeze_detail.materialize_change_baseline_file_occupied;
        output.telemetry.generation_freeze_materialize_change_baseline_directory_capacity =
            generation_freeze_detail.materialize_change_baseline_directory_capacity;
        output.telemetry.generation_freeze_materialize_change_baseline_directory_occupied =
            generation_freeze_detail.materialize_change_baseline_directory_occupied;
        output.telemetry.generation_freeze_materialize_change_file_updates =
            generation_freeze_detail.materialize_change_file_updates;
        output.telemetry.generation_freeze_materialize_change_directory_updates =
            generation_freeze_detail.materialize_change_directory_updates;
        output.telemetry.generation_freeze_materialize_change_update_index_bytes =
            generation_freeze_detail.materialize_change_update_index_bytes;
        output.telemetry.generation_freeze_materialize_change_file_index_bytes =
            generation_freeze_detail.materialize_change_file_index_bytes;
        output.telemetry.generation_freeze_materialize_change_directory_index_bytes =
            generation_freeze_detail.materialize_change_directory_index_bytes;
        output.telemetry.generation_freeze_materialize_change_peak_temporary_bytes =
            generation_freeze_detail.materialize_change_peak_temporary_bytes;
        output.telemetry.generation_freeze_materialize_change_peak_owned_bytes =
            generation_freeze_detail.materialize_change_peak_owned_bytes;
        output.telemetry.generation_freeze_compiled_ns =
            generation_freeze_detail.compiled_ns;
        output.telemetry.generation_freeze_compiled_total_ns =
            generation_freeze_detail.compiled_total_ns;
        output.telemetry.generation_freeze_compiled_sizing_layout_ns =
            generation_freeze_detail.compiled_sizing_layout_ns;
        output.telemetry.generation_freeze_compiled_allocate_zero_ns =
            generation_freeze_detail.compiled_allocate_zero_ns;
        output.telemetry.generation_freeze_compiled_strings_ns =
            generation_freeze_detail.compiled_strings_ns;
        output.telemetry.generation_freeze_compiled_identities_ns =
            generation_freeze_detail.compiled_identities_ns;
        output.telemetry.generation_freeze_compiled_graph_arrays_ns =
            generation_freeze_detail.compiled_graph_arrays_ns;
        output.telemetry.generation_freeze_compiled_graph_indexes_ns =
            generation_freeze_detail.compiled_graph_indexes_ns;
        output.telemetry.generation_freeze_compiled_section_crc_ns =
            generation_freeze_detail.compiled_section_crc_ns;
        output.telemetry.generation_freeze_compiled_header_bind_ns =
            generation_freeze_detail.compiled_header_bind_ns;
        output.telemetry.generation_freeze_compiled_baseline_bulk_bytes =
            generation_freeze_detail.compiled_baseline_bulk_bytes;
        output.telemetry.generation_freeze_compiled_baseline_bulk_sections =
            generation_freeze_detail.compiled_baseline_bulk_sections;
        output.telemetry.generation_freeze_compiled_output_bytes =
            generation_freeze_detail.compiled_output_bytes;
        output.telemetry.generation_freeze_roots_ns =
            generation_freeze_detail.roots_ns;
        output.telemetry.generation_freeze_source_manager_ns =
            generation_freeze_detail.source_manager_ns;
        output.telemetry.generation_freeze_source_manager_internal_ns =
            generation_freeze_detail.source_manager_internal_ns;
        output.telemetry.generation_freeze_source_manager_preflight_ns =
            generation_freeze_detail.source_manager_preflight_ns;
        output.telemetry.generation_freeze_source_manager_layout_ns =
            generation_freeze_detail.source_manager_layout_ns;
        output.telemetry.generation_freeze_source_manager_allocate_zero_ns =
            generation_freeze_detail.source_manager_allocate_zero_ns;
        output.telemetry.generation_freeze_source_manager_source_records_ns =
            generation_freeze_detail.source_manager_source_records_ns;
        output.telemetry.generation_freeze_source_manager_roots_ns =
            generation_freeze_detail.source_manager_roots_ns;
        output.telemetry.generation_freeze_source_manager_path_index_ns =
            generation_freeze_detail.source_manager_path_index_ns;
        output.telemetry.generation_freeze_source_manager_file_identity_ns =
            generation_freeze_detail.source_manager_file_identity_ns;
        output.telemetry.generation_freeze_source_manager_directory_identity_ns =
            generation_freeze_detail.source_manager_directory_identity_ns;
        output.telemetry.generation_freeze_source_manager_crc_wall_ns =
            generation_freeze_detail.source_manager_crc_wall_ns;
        output.telemetry.generation_freeze_source_manager_crc_total_bytes =
            generation_freeze_detail.source_manager_crc_total_bytes;
        output.telemetry.generation_freeze_source_manager_crc_source_core_ns =
            generation_freeze_detail.source_manager_crc_source_core_ns;
        output.telemetry.generation_freeze_source_manager_crc_source_core_bytes =
            generation_freeze_detail.source_manager_crc_source_core_bytes;
        output.telemetry.generation_freeze_source_manager_crc_physical_state_ns =
            generation_freeze_detail.source_manager_crc_physical_state_ns;
        output.telemetry.generation_freeze_source_manager_crc_physical_state_bytes =
            generation_freeze_detail.source_manager_crc_physical_state_bytes;
        output.telemetry.generation_freeze_source_manager_crc_graph_records_ns =
            generation_freeze_detail.source_manager_crc_graph_records_ns;
        output.telemetry.generation_freeze_source_manager_crc_graph_records_bytes =
            generation_freeze_detail.source_manager_crc_graph_records_bytes;
        output.telemetry.generation_freeze_source_manager_crc_forward_edges_ns =
            generation_freeze_detail.source_manager_crc_forward_edges_ns;
        output.telemetry.generation_freeze_source_manager_crc_forward_edges_bytes =
            generation_freeze_detail.source_manager_crc_forward_edges_bytes;
        output.telemetry.generation_freeze_source_manager_crc_reverse_edges_ns =
            generation_freeze_detail.source_manager_crc_reverse_edges_ns;
        output.telemetry.generation_freeze_source_manager_crc_reverse_edges_bytes =
            generation_freeze_detail.source_manager_crc_reverse_edges_bytes;
        output.telemetry.generation_freeze_source_manager_crc_roots_ns =
            generation_freeze_detail.source_manager_crc_roots_ns;
        output.telemetry.generation_freeze_source_manager_crc_roots_bytes =
            generation_freeze_detail.source_manager_crc_roots_bytes;
        output.telemetry.generation_freeze_source_manager_crc_path_index_ns =
            generation_freeze_detail.source_manager_crc_path_index_ns;
        output.telemetry.generation_freeze_source_manager_crc_path_index_bytes =
            generation_freeze_detail.source_manager_crc_path_index_bytes;
        output.telemetry.generation_freeze_source_manager_crc_path_bytes_ns =
            generation_freeze_detail.source_manager_crc_path_bytes_ns;
        output.telemetry.generation_freeze_source_manager_crc_path_bytes_bytes =
            generation_freeze_detail.source_manager_crc_path_bytes_bytes;
        output.telemetry.generation_freeze_source_manager_crc_file_identity_ns =
            generation_freeze_detail.source_manager_crc_file_identity_ns;
        output.telemetry.generation_freeze_source_manager_crc_file_identity_bytes =
            generation_freeze_detail.source_manager_crc_file_identity_bytes;
        output.telemetry.generation_freeze_source_manager_crc_directory_identity_ns =
            generation_freeze_detail.source_manager_crc_directory_identity_ns;
        output.telemetry.generation_freeze_source_manager_crc_directory_identity_bytes =
            generation_freeze_detail.source_manager_crc_directory_identity_bytes;
        output.telemetry.generation_freeze_source_manager_prefix_directory_encode_ns =
            generation_freeze_detail.source_manager_prefix_directory_encode_ns;
        output.telemetry.generation_freeze_source_manager_directory_crc_ns =
            generation_freeze_detail.source_manager_directory_crc_ns;
        output.telemetry.generation_freeze_source_manager_header_crc_ns =
            generation_freeze_detail.source_manager_header_crc_ns;
        output.telemetry.generation_freeze_source_manager_bind_ns =
            generation_freeze_detail.source_manager_bind_ns;
        output.telemetry.generation_freeze_source_manager_verify_ns =
            generation_freeze_detail.source_manager_verify_ns;
        output.telemetry.generation_freeze_source_manager_segment_validate_ns =
            generation_freeze_detail.source_manager_segment_validate_ns;
        output.telemetry.generation_freeze_source_manager_identity_copy_ns =
            generation_freeze_detail.source_manager_identity_copy_ns;
        output.telemetry.generation_freeze_source_manager_mode =
            generation_freeze_detail.source_manager_mode;
        output.telemetry.generation_freeze_source_manager_crc_worker_count =
            generation_freeze_detail.source_manager_crc_worker_count;
        output.telemetry.generation_freeze_source_manager_extent_count =
            generation_freeze_detail.source_manager_extent_count;
        output.telemetry.generation_freeze_source_manager_sparse_required_extent_count =
            generation_freeze_detail.
                source_manager_sparse_required_extent_count;
        output.telemetry.generation_freeze_source_manager_sparse_fallback_reason =
            generation_freeze_detail.source_manager_sparse_fallback_reason;
        output.telemetry.generation_freeze_change_state_ns =
            generation_freeze_detail.change_state_ns;
        output.telemetry.generation_freeze_build_cache_ns =
            generation_freeze_detail.build_cache_ns;
        output.telemetry.generation_freeze_build_cache_layout_allocate_ns =
            generation_freeze_detail.build_cache_layout_allocate_ns;
        output.telemetry.generation_freeze_build_cache_source_frontend_ns =
            generation_freeze_detail.build_cache_source_frontend_ns;
        output.telemetry.generation_freeze_build_cache_source_directory_text_ns =
            generation_freeze_detail.build_cache_source_directory_text_ns;
        output.telemetry.generation_freeze_build_cache_frontend_record_ranges_ns =
            generation_freeze_detail.build_cache_frontend_record_ranges_ns;
        output.telemetry.generation_freeze_build_cache_frontend_local_types_ns =
            generation_freeze_detail.build_cache_frontend_local_types_ns;
        output.telemetry.generation_freeze_build_cache_frontend_type_slots_ns =
            generation_freeze_detail.build_cache_frontend_type_slots_ns;
        output.telemetry.generation_freeze_build_cache_frontend_object_slots_ns =
            generation_freeze_detail.build_cache_frontend_object_slots_ns;
        output.telemetry.generation_freeze_build_cache_frontend_member_slots_ns =
            generation_freeze_detail.build_cache_frontend_member_slots_ns;
        output.telemetry.generation_freeze_build_cache_source_frontend_total_sources =
            generation_freeze_detail.build_cache_source_frontend_total_sources;
        output.telemetry.generation_freeze_build_cache_source_frontend_sampled_sources =
            generation_freeze_detail.build_cache_source_frontend_sampled_sources;
        output.telemetry.generation_freeze_build_cache_source_lookup_sample_ns =
            generation_freeze_detail.build_cache_source_lookup_sample_ns;
        output.telemetry.generation_freeze_build_cache_source_text_copy_sample_ns =
            generation_freeze_detail.build_cache_source_text_copy_sample_ns;
        output.telemetry.generation_freeze_build_cache_frontend_record_sample_ns =
            generation_freeze_detail.build_cache_frontend_record_sample_ns;
        output.telemetry.generation_freeze_build_cache_frontend_local_types_sample_ns =
            generation_freeze_detail.build_cache_frontend_local_types_sample_ns;
        output.telemetry.generation_freeze_build_cache_frontend_type_slots_sample_ns =
            generation_freeze_detail.build_cache_frontend_type_slots_sample_ns;
        output.telemetry.generation_freeze_build_cache_frontend_object_slots_sample_ns =
            generation_freeze_detail.build_cache_frontend_object_slots_sample_ns;
        output.telemetry.generation_freeze_build_cache_frontend_member_slots_sample_ns =
            generation_freeze_detail.build_cache_frontend_member_slots_sample_ns;
        output.telemetry.generation_freeze_build_cache_contribution_ns =
            generation_freeze_detail.build_cache_contribution_ns;
        output.telemetry.generation_freeze_build_cache_graph_ns =
            generation_freeze_detail.build_cache_graph_ns;
        output.telemetry.generation_freeze_build_cache_change_identity_ns =
            generation_freeze_detail.build_cache_change_identity_ns;
        output.telemetry.generation_freeze_build_cache_section_crc_ns =
            generation_freeze_detail.build_cache_section_crc_ns;
        output.telemetry.generation_freeze_build_cache_header_directory_ns =
            generation_freeze_detail.build_cache_header_directory_ns;
        output.telemetry.generation_freeze_build_cache_bind_ns =
            generation_freeze_detail.build_cache_bind_ns;
        output.telemetry.generation_freeze_build_cache_verify_ns =
            generation_freeze_detail.build_cache_verify_ns;
        output.telemetry.generation_freeze_build_cache_mapped_baseline_bulk_bytes =
            generation_freeze_detail.build_cache_mapped_baseline_bulk_bytes;
        output.telemetry.generation_freeze_build_cache_mapped_baseline_patch_records =
            generation_freeze_detail.build_cache_mapped_baseline_patch_records;
        output.telemetry.generation_freeze_build_cache_mapped_baseline_append_records =
            generation_freeze_detail.build_cache_mapped_baseline_append_records;
        output.telemetry.generation_freeze_build_cache_mapped_baseline_bulk_sections =
            generation_freeze_detail.build_cache_mapped_baseline_bulk_sections;
        output.telemetry.generation_freeze_bind_ns =
            generation_freeze_detail.bind_ns;
        output.telemetry.generation_freeze_verify_change_state_ns =
            generation_freeze_detail.verify_change_state_ns;
        output.telemetry.generation_freeze_verify_build_cache_ns =
            generation_freeze_detail.verify_build_cache_ns;
        output.telemetry.save_total_ns =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<
                    std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() -
                        save_begin).count());
    };

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
                publish_save_telemetry();
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

    // GEN-02C19: zero-read SAVE. A prepared Generation owns the exact
    // configuration identity used to build it. SAVE proves the stored file token
    // unchanged and consumes configuration/fingerprint/proof directly.
    project_configuration fallback_configuration;
    const project_configuration* save_configuration =
        &project->configuration();

    baseline_fingerprint fingerprint{};
    baseline_configuration_state configuration_state{};
    auto result = status{};
    bool configuration_ready = false;

    const auto* expected =
        project->build_fingerprint();
    if (expected == nullptr)
        return {status_code::invalid_state};

    const auto* generation_configuration =
        project->generation_configuration_proof();

    if (generation_configuration != nullptr &&
        generation_configuration->content_hash_available &&
        generation_configuration->change_token_available) {

        const auto configuration_token_begin =
            std::chrono::steady_clock::now();

        bool unchanged = false;
        const auto proof_result =
            prove_file_unchanged(
                project->configuration_path(),
                generation_configuration->change_token,
                unchanged);

        configuration_token_ns +=
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<
                    std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() -
                        configuration_token_begin).count());

        if (proof_result.ok() && unchanged) {
            fingerprint = *expected;

            configuration_state.observation =
                generation_configuration->observation;
            configuration_state.content_hash =
                generation_configuration->content_hash;
            configuration_state.content_hash_available = true;
            configuration_state.change_token =
                generation_configuration->change_token;
            configuration_state.change_token_available = true;
            configuration_state.project_version =
                project->configuration().version;
            configuration_state.abi_target =
                static_cast<std::uint32_t>(
                    project->configuration().abi.target);
            configuration_state.abi_pack =
                project->configuration().abi.pack;
            configuration_state.available = true;

            configuration_ready = true;
        }
        else if (!proof_result.ok() &&
                 proof_result.code !=
                    status_code::not_found) {
            return proof_result;
        }
        // A changed token or unsupported proof deliberately falls through to
        // semantic validation. This preserves the old behavior where formatting-
        // only project.json changes may still be accepted by fingerprint.
    }

    if (!configuration_ready) {
        file_change_token configuration_change_token;
        const auto configuration_token_begin =
            std::chrono::steady_clock::now();
        const auto token_result =
            capture_file_change_token(
                project->configuration_path(),
                configuration_change_token);
        configuration_token_ns +=
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<
                    std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() -
                        configuration_token_begin).count());

        if (!token_result.ok() &&
            token_result.code != status_code::not_found) {
            return token_result;
        }

        file_snapshot configuration_file;
        const auto configuration_read_begin =
            std::chrono::steady_clock::now();
        const auto acquisition =
            acquire_file_snapshot(
                project->configuration_path(),
                std::nullopt,
                configuration_file);
        configuration_read_ns =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<
                    std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() -
                        configuration_read_begin).count());

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

        const auto configuration_parse_begin =
            std::chrono::steady_clock::now();
        result = load_project_configuration(
            configuration_file.bytes,
            project->configuration_path(),
            operation_id{},
            configuration_diagnostics,
            fallback_configuration);
        configuration_parse_ns =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<
                    std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() -
                        configuration_parse_begin).count());

        if (!result.ok())
            return result;

        const auto fingerprint_begin =
            std::chrono::steady_clock::now();
        result = make_project_baseline_fingerprint(
            fallback_configuration,
            fingerprint);
        fingerprint_ns =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<
                    std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() -
                        fingerprint_begin).count());
        if (!result.ok())
            return result;

        if (!(*expected == fingerprint))
            return {status_code::rebuild_required};

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
            fallback_configuration.version;
        configuration_state.abi_target =
            static_cast<std::uint32_t>(
                fallback_configuration.abi.target);
        configuration_state.abi_pack =
            fallback_configuration.abi.pack;
        configuration_state.available = true;

        save_configuration =
            &fallback_configuration;
    }

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
        publish_save_telemetry();
        return {};
    }

    if (project->construction_backed()) {
        project_generation_storage generation;

        const auto generation_freeze_begin =
            std::chrono::steady_clock::now();
        result = freeze_project_generation(
                *project,
                *save_configuration,
                generation,
                &generation_freeze_detail);
        generation_freeze_ns =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<
                    std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() -
                        generation_freeze_begin).count());

        if (!result.ok())
            return result;

        result = store.commit(
            fingerprint,
            configuration_state,
            generation.segments(),
            generation.commit_provenance(),
            output,
            io_worker_budget);

        publish_save_telemetry();

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

    result =
        active.bind_source_manager(
            sources);
    if (!result.ok())
        return result;

    result =
        active.bind_build_cache(
            cache);
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

    publish_save_telemetry();

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
