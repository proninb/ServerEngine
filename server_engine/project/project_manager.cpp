#include "project_manager.hpp"

#include <exception>
#include <new>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace cw::server {

project_manager::~project_manager() noexcept {
    activity.close();

    // Explicit UNLOAD owns stop-before-wait. Destruction must never perform a
    // blind wait that could deadlock on a long-running RUN task.
    if (activity.active_count() != 0)
        std::terminate();

    project.reset();
    lifecycle.store(project_lifecycle_state::unloaded, std::memory_order_release);
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
    lifecycle.store(project_lifecycle_state::unloaded, std::memory_order_release);
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
    const auto configuration_result = load_project_configuration_file(
        configuration_path, operation, diagnostics, configuration);
    if (!configuration_result.ok()) {
        abandon_construction();
        return configuration_result;
    }

    return rebuild_reserved(
        std::move(configuration), operation, diagnostics, output, worker_limit);
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

    return rebuild_reserved(
        std::move(configuration), operation, diagnostics, output, worker_limit);
}

status project_manager::rebuild_reserved(
    project_configuration configuration,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    project_build_result& output,
    std::size_t worker_limit) noexcept {

    try {
        auto candidate = std::make_unique<project_context>(std::move(configuration));
        project_build_orchestrator builder{*candidate, worker_limit};

        const auto result = builder.construct(operation, diagnostics, output);
        if (!result.ok()) {
            abandon_construction();
            return result;
        }

        project = std::move(candidate);
        output.rebuilt = true;

        // Project and gate are fully initialized before READY becomes observable.
        // acquire() uses the READY release/acquire edge as the publication barrier.
        activity.open();
        lifecycle.store(project_lifecycle_state::ready, std::memory_order_release);
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

status project_manager::acquire(project_access& output) const noexcept {
    output.reset();

    // READY is the publication barrier. The second state check closes the race
    // where UNLOAD changes READY -> DRAINING between the first check and gate entry.
    if (lifecycle.load(std::memory_order_acquire) != project_lifecycle_state::ready)
        return {status_code::not_found};

    if (!activity.try_enter())
        return {status_code::not_found};

    if (lifecycle.load(std::memory_order_acquire) != project_lifecycle_state::ready) {
        activity.leave();
        return {status_code::not_found};
    }

    if (project == nullptr) {
        activity.leave();
        return {status_code::initialization_failed};
    }

    output = project_access{activity, *project};
    return {};
}

status project_manager::unload(project_stop_request stop) noexcept {
    auto expected = project_lifecycle_state::ready;
    if (!lifecycle.compare_exchange_strong(
            expected,
            project_lifecycle_state::draining,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return {status_code::invalid_state};
    }

    // Admission must close before stop is requested. Long-running RUN/background
    // work can then observe the stop request, release its token, and let drain finish.
    activity.close();

    stop();
    activity.wait_drained();

    project.reset();
    lifecycle.store(project_lifecycle_state::unloaded, std::memory_order_release);
    return {};
}

} // namespace cw::server
