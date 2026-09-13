#include "source_frontend_generation.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <new>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

namespace cw::server {
namespace {

using frontend_clock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t frontend_elapsed_ns(
    frontend_clock::time_point begin,
    frontend_clock::time_point end) noexcept {

    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            end - begin).count());
}

struct resolved_import final {
    std::uint32_t visible_from = 0;
    source_id dependency{};
};

struct generation_source_state final {
    source_id source{};
    source_snapshot snapshot;
    std::vector<parser_token> tokens;
    std::vector<source_include_directive> include_directives;
    std::vector<source_id> dependencies;
    std::vector<resolved_import> imports;
    std::vector<std::uint32_t> dependents;
    parsed_source parsed;
    std::unique_ptr<source_interface> interface;
    status work_status{};
    std::uint32_t remaining = 0;
};

[[nodiscard]] std::size_t default_workers() noexcept {
    const auto value = std::thread::hardware_concurrency();
    return value == 0 ? 1 : static_cast<std::size_t>(value);
}

[[nodiscard]] std::size_t default_acquisition_workers(
    std::size_t cpu_workers) noexcept {

#ifdef _WIN32
    // Small-file CreateFile/ReadFile/CloseHandle is latency-bound and benefits
    // from modest oversubscription. Keep CPU phases at cpu_workers.
    const auto extra =
        (cpu_workers + 2) / 3;
    const auto oversubscribed =
        cpu_workers > (std::numeric_limits<std::size_t>::max)() - extra
        ? cpu_workers
        : cpu_workers + extra;

    constexpr std::size_t practical_cap = 64;
    return (std::max)(
        cpu_workers,
        (std::min)(
            oversubscribed,
            practical_cap));
#else
    return cpu_workers;
#endif
}

class operation_worker_team final {
public:
    operation_worker_team() noexcept = default;

    operation_worker_team(const operation_worker_team&) = delete;
    operation_worker_team& operator=(const operation_worker_team&) = delete;

    ~operation_worker_team() noexcept {
        shutdown();
    }

    [[nodiscard]] status initialize(
        std::size_t requested_workers) noexcept {

        if (initialized)
            return {status_code::invalid_state};

        worker_limit =
            (std::max)(std::size_t{1}, requested_workers);

        if (worker_limit == 1) {
            initialized = true;
            return {};
        }

        worker_count = worker_limit;

        try {
            threads.reserve(worker_count);
            for (std::size_t index = 0;
                 index < worker_count;
                 ++index) {
                threads.emplace_back(
                    [this, index] {
                        worker_loop(index);
                    });
            }

            initialized = true;
            return {};
        }
        catch (const std::bad_alloc&) {
            shutdown();
            return {status_code::not_available};
        }
        catch (const std::length_error&) {
            shutdown();
            return {status_code::not_available};
        }
        catch (const std::system_error&) {
            shutdown();
            return {status_code::not_available};
        }
        catch (...) {
            shutdown();
            return {status_code::initialization_failed};
        }
    }

    template<class Function>
    [[nodiscard]] status run(
        std::size_t count,
        std::size_t dispatch_worker_limit,
        std::atomic<std::size_t>& max_active,
        Function&& function) noexcept {

        if (count == 0)
            return {};

        if (!initialized)
            return {status_code::invalid_state};

        ++dispatch_count_value;

        const auto active_workers =
            (std::min)(
                count,
                (std::min)(
                    worker_limit,
                    (std::max)(
                        std::size_t{1},
                        dispatch_worker_limit)));

        publish_max_active(
            max_active,
            active_workers);

        if (active_workers == 1) {
            try {
                for (std::size_t index = 0;
                     index < count;
                     ++index) {
                    function(index);
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
            catch (...) {
                return {status_code::initialization_failed};
            }
        }

        using function_type =
            std::remove_reference_t<Function>;

        current_context =
            static_cast<void*>(
                std::addressof(function));

        current_function =
            [](void* context,
               std::size_t index) {
                (*static_cast<function_type*>(
                    context))(index);
            };

        current_count = count;
        current_active_workers =
            active_workers;

        next.store(
            0,
            std::memory_order_relaxed);

        completed_workers.store(
            0,
            std::memory_order_relaxed);

        worker_failure.store(
            status_code::ok,
            std::memory_order_relaxed);

        generation.fetch_add(
            1,
            std::memory_order_release);
        generation.notify_all();

        auto completed =
            completed_workers.load(
                std::memory_order_acquire);

        while (completed < worker_count) {
            completed_workers.wait(
                completed,
                std::memory_order_acquire);

            completed =
                completed_workers.load(
                    std::memory_order_acquire);
        }

        current_function = nullptr;
        current_context = nullptr;

        return {
            worker_failure.load(
                std::memory_order_acquire)};
    }

    [[nodiscard]] std::size_t threads_created() const noexcept {
        return threads.size();
    }

    [[nodiscard]] std::uint64_t dispatch_count() const noexcept {
        return dispatch_count_value;
    }

private:
    using function_pointer =
        void (*)(void*, std::size_t);

    static void publish_max_active(
        std::atomic<std::size_t>& max_active,
        std::size_t value) noexcept {

        auto observed =
            max_active.load(
                std::memory_order_relaxed);

        while (value > observed &&
               !max_active.compare_exchange_weak(
                   observed,
                   value,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
    }

    void publish_failure(
        status_code value) noexcept {

        auto expected =
            status_code::ok;

        (void)worker_failure.compare_exchange_strong(
            expected,
            value,
            std::memory_order_release,
            std::memory_order_relaxed);
    }

    void execute_work() noexcept {
        try {
            for (;;) {
                if (worker_failure.load(
                        std::memory_order_acquire) !=
                    status_code::ok) {
                    break;
                }

                const auto index =
                    next.fetch_add(
                        1,
                        std::memory_order_relaxed);

                if (index >= current_count)
                    break;

                current_function(
                    current_context,
                    index);
            }
        }
        catch (const std::bad_alloc&) {
            publish_failure(
                status_code::not_available);
        }
        catch (const std::length_error&) {
            publish_failure(
                status_code::not_available);
        }
        catch (const std::system_error&) {
            publish_failure(
                status_code::not_available);
        }
        catch (...) {
            publish_failure(
                status_code::initialization_failed);
        }
    }

    void worker_loop(
        std::size_t worker_index) noexcept {

        std::uint64_t observed_generation = 0;

        for (;;) {
            const auto current_generation =
                generation.load(
                    std::memory_order_acquire);

            if (current_generation ==
                observed_generation) {
                generation.wait(
                    observed_generation,
                    std::memory_order_acquire);
                continue;
            }

            observed_generation =
                current_generation;

            if (stopping.load(
                    std::memory_order_acquire)) {
                return;
            }

            if (worker_index <
                current_active_workers) {
                execute_work();
            }

            const auto completed =
                completed_workers.fetch_add(
                    1,
                    std::memory_order_acq_rel) +
                1;

            if (completed == worker_count)
                completed_workers.notify_all();
        }
    }

    void shutdown() noexcept {
        if (threads.empty()) {
            initialized = false;
            worker_count = 0;
            return;
        }

        stopping.store(
            true,
            std::memory_order_release);

        generation.fetch_add(
            1,
            std::memory_order_release);
        generation.notify_all();

        threads.clear();

        worker_count = 0;
        initialized = false;
    }

    std::vector<std::jthread> threads;

    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> completed_workers{0};
    std::atomic<status_code> worker_failure{
        status_code::ok};
    std::atomic<bool> stopping{false};

    function_pointer current_function = nullptr;
    void* current_context = nullptr;
    std::size_t current_count = 0;
    std::size_t current_active_workers = 0;
    std::size_t worker_limit = 1;
    std::size_t worker_count = 0;
    std::uint64_t dispatch_count_value = 0;
    bool initialized = false;
};

[[nodiscard]] constexpr std::uint64_t edge_mix(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

class source_edge_set final {
public:
    [[nodiscard]] status insert(
        source_id owner,
        source_id dependency,
        bool& inserted) noexcept {

        inserted = false;
        if (!owner || !dependency)
            return {status_code::invalid_argument};

        if (slots.empty() || (count + 1) * 2 >= slots.size()) {
            const auto capacity = slots.empty() ? std::size_t{16} : slots.size() * 2;
            const auto result = grow(capacity);
            if (!result.ok())
                return result;
        }

        const auto key = (static_cast<std::uint64_t>(owner.value()) << 32) |
            static_cast<std::uint64_t>(dependency.value());
        const auto mask = slots.size() - 1;
        auto position = static_cast<std::size_t>(edge_mix(key)) & mask;
        for (;;) {
            auto& slot = slots[position];
            if (slot == 0) {
                slot = key;
                ++count;
                inserted = true;
                return {};
            }
            if (slot == key)
                return {};
            position = (position + 1) & mask;
        }
    }

private:
    [[nodiscard]] status grow(std::size_t capacity) noexcept {
        try {
            std::vector<std::uint64_t> replacement(capacity, 0);
            const auto mask = replacement.size() - 1;
            for (const auto key : slots) {
                if (key == 0)
                    continue;
                auto position = static_cast<std::size_t>(edge_mix(key)) & mask;
                while (replacement[position] != 0)
                    position = (position + 1) & mask;
                replacement[position] = key;
            }
            slots.swap(replacement);
            return {};
        }
        catch (const std::bad_alloc&) {
            return {status_code::not_available};
        }
        catch (const std::length_error&) {
            return {status_code::not_available};
        }
    }

    std::vector<std::uint64_t> slots;
    std::size_t count = 0;
};

void merge_diagnostics(const diagnostic_buffer& from, diagnostic_buffer& to) {
    for (const auto& record : from.records())
        to.emit(record);
}


} // namespace

const source_frontend_entry* source_frontend_result::find(source_id source) const noexcept {
    for (const auto& entry : entries) {
        if (entry.source == source)
            return &entry;
    }
    return nullptr;
}

source_frontend_generation::source_frontend_generation(
    project_semantic_services semantic_value,
    source_manager_update& source_update,
    std::size_t worker_limit_value,
    std::size_t acquisition_worker_limit_value) noexcept
    : semantic(semantic_value), sources(source_update),
      worker_limit(worker_limit_value == 0 ? default_workers() : worker_limit_value),
      acquisition_worker_limit(
          acquisition_worker_limit_value == 0
              ? default_acquisition_workers(worker_limit)
              : (std::max)(
                    std::size_t{1},
                    acquisition_worker_limit_value)) {}

source_frontend_generation::source_frontend_generation(
    project_context& project_value,
    source_manager_update& source_update,
    std::size_t worker_limit_value,
    std::size_t acquisition_worker_limit_value) noexcept
    : source_frontend_generation(
          project_value.parser_services(),
          source_update,
          worker_limit_value,
          acquisition_worker_limit_value) {}

status source_frontend_generation::build(
    std::span<const std::filesystem::path> roots,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    source_frontend_result& output,
    bool roots_are_canonical) noexcept {

    if (roots.empty())
        return {status_code::invalid_argument};

    try {
        std::vector<generation_source_state> states;
        states.reserve(roots.size() * 2 + 8);
        std::vector<std::uint32_t> state_by_source(1,
            (std::numeric_limits<std::uint32_t>::max)());
        std::size_t unique_roots = 0;
        source_edge_set dependency_edges;

        auto add_state = [&](source_id source, bool& inserted) -> status {
            inserted = false;
            if (!source)
                return {status_code::invalid_argument};
            const auto source_index = static_cast<std::size_t>(source.value());
            if (state_by_source.size() <= source_index) {
                try {
                    state_by_source.resize(
                        source_index + 1,
                        (std::numeric_limits<std::uint32_t>::max)());
                }
                catch (const std::bad_alloc&) {
                    return {status_code::not_available};
                }
                catch (const std::length_error&) {
                    return {status_code::not_available};
                }
            }
            if (state_by_source[source_index] !=
                (std::numeric_limits<std::uint32_t>::max)()) {
                return {};
            }
            if (states.size() >=
                static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
                return {status_code::not_available};
            }
            generation_source_state state;
            state.source = source;
            states.push_back(std::move(state));
            state_by_source[source_index] =
                static_cast<std::uint32_t>(states.size() - 1);
            inserted = true;
            return {};
        };

        const auto root_resolve_begin = frontend_clock::now();

        auto result =
            sources.reserve_sources(roots.size());
        if (!result.ok())
            return result;

        for (const auto& root_path : roots) {
            source_id source;
            result = roots_are_canonical
                ? sources.resolve_canonical(
                    root_path,
                    source)
                : sources.resolve(
                    root_path,
                    source);
            if (!result.ok())
                return result;
            bool inserted = false;
            result = add_state(source, inserted);
            if (!result.ok())
                return result;
            if (inserted)
                ++unique_roots;
        }

        const auto root_resolve_end = frontend_clock::now();

        source_frontend_summary summary;
        summary.roots = static_cast<std::uint32_t>(unique_roots);
        summary.worker_limit = worker_limit;
        summary.acquisition_worker_limit =
            acquisition_worker_limit;
        summary.root_resolve_ns =
            frontend_elapsed_ns(
                root_resolve_begin,
                root_resolve_end);
        std::atomic<std::size_t> max_active{0};

        operation_worker_team workers;
        const auto team_worker_limit =
            (std::max)(
                worker_limit,
                acquisition_worker_limit);
        auto worker_team_result =
            workers.initialize(team_worker_limit);
        if (!worker_team_result.ok())
            return worker_team_result;

        std::size_t wave_begin = 0;
        while (wave_begin < states.size()) {
            const auto wave_end = states.size();
            const auto wave_count = wave_end - wave_begin;

            const auto wave_setup_begin = frontend_clock::now();
            std::vector<source_acquire_job> jobs(wave_count);
            std::vector<source_acquire_result> acquisition_results(wave_count);
            std::vector<diagnostic_buffer> worker_diagnostics(wave_count);
            summary.wave_setup_ns +=
                frontend_elapsed_ns(
                    wave_setup_begin,
                    frontend_clock::now());

            const auto acquire_prepare_begin = frontend_clock::now();
            for (std::size_t local = 0; local < wave_count; ++local) {
                auto result = sources.prepare_acquire(states[wave_begin + local].source, jobs[local]);
                if (!result.ok())
                    return result;
            }

            summary.acquire_prepare_ns +=
                frontend_elapsed_ns(
                    acquire_prepare_begin,
                    frontend_clock::now());

            const auto acquire_execute_begin = frontend_clock::now();
            auto parallel_result = workers.run(
                wave_count, acquisition_worker_limit, max_active, [&](std::size_t local) {
                    states[wave_begin + local].work_status =
                        source_manager_update::execute_acquire(jobs[local], acquisition_results[local]);
                });
            summary.acquire_execute_ns +=
                frontend_elapsed_ns(
                    acquire_execute_begin,
                    frontend_clock::now());
            if (!parallel_result.ok())
                return parallel_result;

            const auto acquire_apply_begin = frontend_clock::now();
            for (std::size_t local = 0; local < wave_count; ++local) {
                auto& state = states[wave_begin + local];
                if (!state.work_status.ok())
                    return state.work_status;
                auto result = sources.apply_acquire(
                    std::move(acquisition_results[local]));
                if (!result.ok())
                    return result;
                state.snapshot = sources.snapshot(state.source);
                if (!state.snapshot)
                    return {status_code::not_found};
                ++summary.acquired;
            }
            summary.acquire_apply_ns +=
                frontend_elapsed_ns(
                    acquire_apply_begin,
                    frontend_clock::now());

            const auto lex_discovery_begin = frontend_clock::now();
            parallel_result = workers.run(
                wave_count, worker_limit, max_active, [&](std::size_t local) {
                    auto& state = states[wave_begin + local];
                    std::vector<directive_span> directives;
                    auto result = lex_source(
                        state.snapshot, operation, worker_diagnostics[local], state.tokens, &directives);
                    if (result.ok()) {
                        result = discover_source_includes(
                            state.snapshot, state.tokens, directives, operation,
                            worker_diagnostics[local], state.include_directives);
                    }
                    state.work_status = result;
                });
            summary.lex_discovery_ns +=
                frontend_elapsed_ns(
                    lex_discovery_begin,
                    frontend_clock::now());
            if (!parallel_result.ok())
                return parallel_result;

            const auto dependency_publish_begin = frontend_clock::now();
            for (std::size_t local = 0; local < wave_count; ++local) {
                merge_diagnostics(worker_diagnostics[local], diagnostics);
                const auto state_index = wave_begin + local;
                if (!states[state_index].work_status.ok())
                    return states[state_index].work_status;
                ++summary.lexed;

                states[state_index].dependencies.clear();
                states[state_index].imports.clear();
                const auto include_count = states[state_index].include_directives.size();
                for (std::size_t include_index = 0; include_index < include_count; ++include_index) {
                    const auto include = states[state_index].include_directives[include_index];
                    const auto source = states[state_index].source;
                    const auto snapshot = states[state_index].snapshot;
                    const auto path_text = snapshot.text().substr(include.path.offset, include.path.length);
                    source_id dependency;
                    auto result = sources.resolve_include(source, path_text, dependency);
                    if (!result.ok())
                        return result;
                    bool edge_inserted = false;
                    result = dependency_edges.insert(source, dependency, edge_inserted);
                    if (!result.ok())
                        return result;
                    if (edge_inserted) {
                        states[state_index].dependencies.push_back(dependency);
                        bool state_inserted = false;
                        result = add_state(dependency, state_inserted);
                        if (!result.ok())
                            return result;
                    }
                    states[state_index].imports.push_back(resolved_import{include.visible_from, dependency});
                }
                auto result = sources.set_includes(
                    states[state_index].source, states[state_index].dependencies);
                if (!result.ok())
                    return result;
            }
            summary.dependency_publish_ns +=
                frontend_elapsed_ns(
                    dependency_publish_begin,
                    frontend_clock::now());

            wave_begin = wave_end;
        }

        const auto graph_schedule_begin = frontend_clock::now();
        summary.discovered = static_cast<std::uint32_t>(states.size());
        result = sources.validate_source_graph(operation, diagnostics);
        if (!result.ok())
            return result;

        for (std::uint32_t index = 0; index < states.size(); ++index) {
            auto& state = states[index];
            state.remaining = static_cast<std::uint32_t>(state.dependencies.size());
            for (const auto dependency : state.dependencies) {
                const auto dependency_index = state_by_source[dependency.value()];
                if (dependency_index == (std::numeric_limits<std::uint32_t>::max)())
                    return {status_code::invalid_argument};
                states[dependency_index].dependents.push_back(index);
            }
        }

        std::vector<std::uint32_t> ready;
        for (std::uint32_t index = 0; index < states.size(); ++index) {
            if (states[index].remaining == 0)
                ready.push_back(index);
        }

        summary.graph_schedule_ns +=
            frontend_elapsed_ns(
                graph_schedule_begin,
                frontend_clock::now());

        const auto parse_begin = frontend_clock::now();
        source_parser parser{semantic};
        std::size_t parsed_count = 0;
        while (!ready.empty()) {
            std::vector<diagnostic_buffer> worker_diagnostics(ready.size());
            auto parallel_result = workers.run(
                ready.size(), worker_limit, max_active, [&](std::size_t local) {
                auto& state = states[ready[local]];
                std::vector<source_environment_import> environment_imports;
                std::vector<const source_interface*> interface_imports;
                try {
                    environment_imports.reserve(state.imports.size());
                    interface_imports.reserve(state.dependencies.size());
                    for (const auto& item : state.imports) {
                        const auto dependency_index = state_by_source[item.dependency.value()];
                        const auto* imported = states[dependency_index].interface.get();
                        if (imported == nullptr) {
                            state.work_status = {status_code::initialization_failed};
                            return;
                        }
                        environment_imports.push_back(source_environment_import{item.visible_from, imported});
                    }
                    for (const auto dependency : state.dependencies) {
                        const auto dependency_index = state_by_source[dependency.value()];
                        const auto* imported = states[dependency_index].interface.get();
                        if (imported == nullptr) {
                            state.work_status = {status_code::initialization_failed};
                            return;
                        }
                        interface_imports.push_back(imported);
                    }

                    const source_environment environment{environment_imports};
                    auto parse_result = parser.parse(
                        state.snapshot, state.tokens, environment, operation,
                        worker_diagnostics[local], state.parsed);
                    if (!parse_result.ok()) {
                        state.work_status = parse_result;
                        return;
                    }

                    const auto facts = state.parsed.facts();

                    state.interface = std::make_unique<source_interface>();
                    state.work_status = state.interface->initialize(facts, semantic.identities(), interface_imports);
                }
                catch (const std::bad_alloc&) {
                    state.work_status = {status_code::not_available};
                }
                catch (const std::length_error&) {
                    state.work_status = {status_code::not_available};
                }
                });
            if (!parallel_result.ok())
                return parallel_result;

            std::vector<std::uint32_t> next_ready;
            for (std::size_t local = 0; local < ready.size(); ++local) {
                merge_diagnostics(worker_diagnostics[local], diagnostics);
                auto& state = states[ready[local]];
                if (!state.work_status.ok())
                    return state.work_status;
                ++parsed_count;
                ++summary.parsed;
                for (const auto dependent_index : state.dependents) {
                    auto& dependent = states[dependent_index];
                    if (dependent.remaining == 0)
                        return {status_code::initialization_failed};
                    --dependent.remaining;
                    if (dependent.remaining == 0)
                        next_ready.push_back(dependent_index);
                }
            }
            ready.swap(next_ready);
        }

        if (parsed_count != states.size())
            return {status_code::semantic_conflict};

        summary.parse_ns +=
            frontend_elapsed_ns(
                parse_begin,
                frontend_clock::now());

        const auto result_materialize_begin = frontend_clock::now();
        source_frontend_result candidate;
        candidate.entries.reserve(states.size());
        for (auto& state : states) {
            candidate.entries.push_back(source_frontend_entry{
                state.source,
                std::move(state.parsed),
                std::move(state.interface),
            });
        }
        summary.result_materialize_ns +=
            frontend_elapsed_ns(
                result_materialize_begin,
                frontend_clock::now());
        summary.max_active_workers = max_active.load(std::memory_order_relaxed);
        summary.parallel_dispatches =
            workers.dispatch_count();
        summary.worker_threads_created =
            workers.threads_created();
        candidate.statistics = summary;
        output = std::move(candidate);
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
