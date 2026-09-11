#pragma once

#include "project_build_orchestrator.hpp"
#include "project_configuration_loader.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <utility>

namespace cw::server {

class project_manager;

enum class project_lifecycle_state : std::uint8_t {
    unloaded,
    constructing,
    ready,
    draining,
};

struct project_stop_request final {
    using function_type = void (*)(void*) noexcept;

    void* context = nullptr;
    function_type function = nullptr;

    void operator()() const noexcept {
        if (function != nullptr)
            function(context);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return function != nullptr;
    }
};

// Admission/drain primitive for READY Project work. The coordinator closes
// admission before requesting Runtime/background stop, then waits for the
// task count to reach zero. Graph access itself never takes a lock.
class project_activity_gate final {
public:
    project_activity_gate() noexcept = default;

    project_activity_gate(const project_activity_gate&) = delete;
    project_activity_gate& operator=(const project_activity_gate&) = delete;

private:
    static constexpr std::uint64_t closed_mask = std::uint64_t{1} << 63;
    static constexpr std::uint64_t count_mask = ~closed_mask;

    [[nodiscard]] bool try_enter() noexcept {
        auto value = state.load(std::memory_order_acquire);
        for (;;) {
            if ((value & closed_mask) != 0)
                return false;
            if ((value & count_mask) == count_mask)
                return false;

            if (state.compare_exchange_weak(
                    value,
                    value + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
    }

    void leave() noexcept {
        const auto previous = state.fetch_sub(1, std::memory_order_acq_rel);
        if ((previous & count_mask) == 1)
            state.notify_all();
    }

    void open() noexcept {
        state.store(0, std::memory_order_release);
        state.notify_all();
    }

    void close() noexcept {
        state.fetch_or(closed_mask, std::memory_order_acq_rel);
        state.notify_all();
    }

    void wait_drained() noexcept {
        auto value = state.load(std::memory_order_acquire);
        while ((value & count_mask) != 0) {
            state.wait(value, std::memory_order_acquire);
            value = state.load(std::memory_order_acquire);
        }
    }

    [[nodiscard]] std::uint64_t active_count() const noexcept {
        return state.load(std::memory_order_acquire) & count_mask;
    }

    mutable std::atomic<std::uint64_t> state{closed_mask};

    friend class project_access;
    friend class project_manager;
};

// Move-only lifetime token for one QUERY/RUN/SAVE task. Any Project pointer,
// span, string_view or Graph view obtained through this token must not escape
// the token lifetime.
class project_access final {
public:
    project_access() noexcept = default;

    ~project_access() noexcept {
        reset();
    }

    project_access(const project_access&) = delete;
    project_access& operator=(const project_access&) = delete;

    project_access(project_access&& other) noexcept
        : gate(std::exchange(other.gate, nullptr)),
          view(other.view) {
        other.view = {};
    }

    project_access& operator=(project_access&& other) noexcept {
        if (this == &other)
            return *this;

        reset();
        gate = std::exchange(other.gate, nullptr);
        view = other.view;
        other.view = {};
        return *this;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return gate != nullptr && view.valid();
    }

    [[nodiscard]] const project_read_view* operator->() const noexcept {
        return view.valid() ? &view : nullptr;
    }

    [[nodiscard]] const project_read_view& get() const noexcept {
        return view;
    }

    void reset() noexcept {
        auto* owned_gate = std::exchange(gate, nullptr);
        view = {};
        if (owned_gate != nullptr)
            owned_gate->leave();
    }

private:
    project_access(
        project_activity_gate& gate_value,
        const project_context& project) noexcept
        : gate(&gate_value),
          view(project) {}

    project_activity_gate* gate = nullptr;
    project_read_view view;

    friend class project_manager;
};

// Coordinator-owned lifecycle. Construction is legal only from UNLOADED.
// A successful construction opens one immutable READY Project. UNLOAD first
// closes admission, then requests stop, drains task tokens, and destroys state.
class project_manager final {
public:
    project_manager() noexcept = default;
    ~project_manager() noexcept;

    project_manager(const project_manager&) = delete;
    project_manager& operator=(const project_manager&) = delete;

    [[nodiscard]] status rebuild(
        const std::filesystem::path& configuration_path,
        operation_id operation,
        diagnostic_buffer& diagnostics,
        project_build_result& output,
        std::size_t worker_limit = 0) noexcept;

    [[nodiscard]] status rebuild(
        project_configuration configuration,
        operation_id operation,
        diagnostic_buffer& diagnostics,
        project_build_result& output,
        std::size_t worker_limit = 0) noexcept;

    [[nodiscard]] status acquire(project_access& output) const noexcept;

    [[nodiscard]] status unload(project_stop_request stop = {}) noexcept;

    [[nodiscard]] project_lifecycle_state state() const noexcept {
        return lifecycle.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool ready() const noexcept {
        return state() == project_lifecycle_state::ready;
    }

private:
    [[nodiscard]] bool reserve_construction() noexcept;
    void abandon_construction() noexcept;

    [[nodiscard]] status rebuild_reserved(
        project_configuration configuration,
        operation_id operation,
        diagnostic_buffer& diagnostics,
        project_build_result& output,
        std::size_t worker_limit) noexcept;

    mutable project_activity_gate activity;
    std::unique_ptr<project_context> project;
    std::atomic<project_lifecycle_state> lifecycle{project_lifecycle_state::unloaded};
};

} // namespace cw::server
