#pragma once

#include "project_build_orchestrator.hpp"
#include "project_configuration_loader.hpp"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>

namespace cw::server {

class project_manager;

// Holds a shared lifecycle lock for one read/query operation scope. Every pointer,
// span and string_view obtained through the guard becomes invalid when the guard
// is released; LOAD replacement, REBUILD publication, UPDATE and UNLOAD exclude it.
class project_read_guard final {
public:
    project_read_guard() noexcept = default;

    project_read_guard(const project_read_guard&) = delete;
    project_read_guard& operator=(const project_read_guard&) = delete;

    project_read_guard(project_read_guard&&) noexcept = default;
    project_read_guard& operator=(project_read_guard&&) noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return view.valid();
    }

    [[nodiscard]] const project_read_view* operator->() const noexcept {
        return view.valid() ? &view : nullptr;
    }

    [[nodiscard]] const project_read_view& get() const noexcept {
        return view;
    }

private:
    project_read_guard(
        std::shared_lock<std::shared_mutex>&& lock_value,
        const project_context& project) noexcept
        : lock(std::move(lock_value)), view(project) {}

    std::shared_lock<std::shared_mutex> lock;
    project_read_view view;

    friend class project_manager;
};

// Owns the optional loaded Project and serializes all Project writers. LOAD and
// REBUILD construct detached state; UPDATE prepares against current state while
// readers remain active. Only the no-fail publication/replacement boundary takes
// the exclusive lifecycle lock.
class project_manager final {
public:
    project_manager() noexcept = default;

    project_manager(const project_manager&) = delete;
    project_manager& operator=(const project_manager&) = delete;

    [[nodiscard]] status load(
        const std::filesystem::path& configuration_path,
        operation_id operation,
        diagnostic_buffer& diagnostics,
        project_build_result& output,
        std::size_t worker_limit = 0) noexcept;

    [[nodiscard]] status load(
        project_configuration configuration,
        operation_id operation,
        diagnostic_buffer& diagnostics,
        project_build_result& output,
        std::size_t worker_limit = 0) noexcept;

    [[nodiscard]] status rebuild(
        operation_id operation,
        diagnostic_buffer& diagnostics,
        project_build_result& output,
        std::size_t worker_limit = 0) noexcept;

    [[nodiscard]] status update(
        std::span<const source_id> dirty_sources,
        operation_id operation,
        diagnostic_buffer& diagnostics,
        project_build_result& output,
        std::size_t worker_limit = 0) noexcept;

    [[nodiscard]] status read(project_read_guard& output) const noexcept;

    void unload() noexcept;

    [[nodiscard]] bool loaded() const noexcept {
        return loaded_state.load(std::memory_order_acquire);
    }

private:
    mutable std::shared_mutex state_mutex;
    std::mutex writer_mutex;
    std::unique_ptr<project_context> project;
    std::atomic_bool loaded_state{false};
};

} // namespace cw::server
