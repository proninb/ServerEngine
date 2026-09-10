#include "project_manager.hpp"

#include <new>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace cw::server {

status project_manager::load(
    const std::filesystem::path& configuration_path,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    project_build_result& output,
    std::size_t worker_limit) noexcept {

    output = {};
    project_configuration configuration;
    const auto result = load_project_configuration_file(
        configuration_path, operation, diagnostics, configuration);
    if (!result.ok())
        return result;
    return load(
        std::move(configuration), operation, diagnostics, output, worker_limit);
}

status project_manager::load(
    project_configuration configuration,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    project_build_result& output,
    std::size_t worker_limit) noexcept {

    output = {};
    try {
        std::lock_guard writer_lock{writer_mutex};

        auto candidate = std::make_unique<project_context>(std::move(configuration));
        project_build_orchestrator builder{*candidate, worker_limit};
        const auto result = builder.rebuild(operation, diagnostics, output);
        if (!result.ok())
            return result;

        std::unique_lock state_lock{state_mutex};
        project.swap(candidate);
        loaded_state.store(true, std::memory_order_release);
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
    catch (const std::system_error&) {
        return {status_code::not_available};
    }
}

status project_manager::rebuild(
    operation_id operation,
    diagnostic_buffer& diagnostics,
    project_build_result& output,
    std::size_t worker_limit) noexcept {

    output = {};
    try {
        std::lock_guard writer_lock{writer_mutex};
        if (project == nullptr)
            return {status_code::not_found};

        auto candidate = std::make_unique<project_context>(project->configuration());
        project_build_orchestrator builder{*candidate, worker_limit};
        const auto result = builder.rebuild(operation, diagnostics, output);
        if (!result.ok())
            return result;

        {
            std::unique_lock state_lock{state_mutex};
            project.swap(candidate);
        }
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
    catch (const std::system_error&) {
        return {status_code::not_available};
    }
}

status project_manager::update(
    std::span<const source_id> dirty_sources,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    project_build_result& output,
    std::size_t worker_limit) noexcept {

    output = {};
    try {
        std::lock_guard writer_lock{writer_mutex};
        if (project == nullptr)
            return {status_code::not_found};

        project_build_orchestrator builder{*project, worker_limit, &state_mutex};
        auto result = builder.update(dirty_sources, operation, diagnostics, output);
        if (result.code != status_code::rebuild_required)
            return result;
        const auto pressure_before_rebuild = output.telemetry.storage_before;

        // UPDATE fallback is a detached full rebuild of the same Project. The
        // current compiled state remains readable until the replacement barrier;
        // failure leaves it untouched.
        project_build_result rebuild_output;
        auto candidate = std::make_unique<project_context>(project->configuration());
        project_build_orchestrator rebuild_builder{*candidate, worker_limit};
        result = rebuild_builder.rebuild(operation, diagnostics, rebuild_output);
        if (!result.ok())
            return result;

        {
            std::unique_lock state_lock{state_mutex};
            project.swap(candidate);
        }
        output = rebuild_output;
        output.telemetry.storage_before = pressure_before_rebuild;
        output.rebuilt = true;
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
    catch (const std::system_error&) {
        return {status_code::not_available};
    }
}

status project_manager::read(project_read_guard& output) const noexcept {
    output = {};
    try {
        std::shared_lock state_lock{state_mutex};
        if (project == nullptr)
            return {status_code::not_found};
        output = project_read_guard{std::move(state_lock), *project};
        return {};
    }
    catch (const std::system_error&) {
        return {status_code::not_available};
    }
}

void project_manager::unload() noexcept {
    try {
        std::lock_guard writer_lock{writer_mutex};
        std::unique_lock state_lock{state_mutex};
        project.reset();
        loaded_state.store(false, std::memory_order_release);
    }
    catch (const std::system_error&) {
        // The mutex implementation cannot fail under the single-owner contract.
    }
}

} // namespace cw::server
