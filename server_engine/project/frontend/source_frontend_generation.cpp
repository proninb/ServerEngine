#include "source_frontend_generation.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>

namespace cw::server {
namespace {

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

template<class Function>
void parallel_for(
    std::size_t count,
    std::size_t worker_limit,
    std::atomic<std::size_t>& max_active,
    Function&& function) {

    if (count == 0)
        return;
    const auto workers = (std::min)(count, (std::max)(std::size_t{1}, worker_limit));
    if (workers == 1) {
        max_active.store((std::max)(max_active.load(), std::size_t{1}), std::memory_order_relaxed);
        for (std::size_t index = 0; index < count; ++index)
            function(index);
        return;
    }

    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> active{0};
    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (std::size_t worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&] {
            const auto now = active.fetch_add(1, std::memory_order_relaxed) + 1;
            auto observed = max_active.load(std::memory_order_relaxed);
            while (now > observed && !max_active.compare_exchange_weak(
                observed, now, std::memory_order_relaxed, std::memory_order_relaxed)) {
            }
            for (;;) {
                const auto index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= count)
                    break;
                function(index);
            }
            active.fetch_sub(1, std::memory_order_relaxed);
        });
    }
    for (auto& thread : threads)
        thread.join();
}

void merge_diagnostics(const diagnostic_buffer& from, diagnostic_buffer& to) {
    for (const auto& record : from.records())
        to.emit(record);
}

[[nodiscard]] bool contains_source(std::span<const source_id> values, source_id source) noexcept {
    for (const auto value : values) {
        if (value == source)
            return true;
    }
    return false;
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
    project_context& project_value,
    source_manager_update& source_update,
    std::size_t worker_limit_value) noexcept
    : project(project_value), sources(source_update),
      worker_limit(worker_limit_value == 0 ? default_workers() : worker_limit_value) {}

status source_frontend_generation::build(
    std::span<const std::filesystem::path> roots,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    source_frontend_result& output) noexcept {

    if (roots.empty())
        return {status_code::invalid_argument};

    try {
        std::vector<generation_source_state> states;
        states.reserve(roots.size() * 2 + 8);
        std::vector<std::uint32_t> state_by_source(1,
            (std::numeric_limits<std::uint32_t>::max)());
        std::vector<source_id> root_ids;
        root_ids.reserve(roots.size());

        auto add_state = [&](source_id source) -> status {
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
            return {};
        };

        for (const auto& root_path : roots) {
            source_id source;
            auto result = sources.resolve(root_path, source);
            if (!result.ok())
                return result;
            if (!contains_source(root_ids, source))
                root_ids.push_back(source);
            result = add_state(source);
            if (!result.ok())
                return result;
        }

        source_frontend_summary summary;
        summary.roots = static_cast<std::uint32_t>(root_ids.size());
        summary.worker_limit = worker_limit;
        std::atomic<std::size_t> max_active{0};

        std::size_t wave_begin = 0;
        while (wave_begin < states.size()) {
            const auto wave_end = states.size();
            const auto wave_count = wave_end - wave_begin;
            std::vector<source_acquire_job> jobs(wave_count);
            std::vector<source_acquire_result> acquisition_results(wave_count);
            std::vector<diagnostic_buffer> worker_diagnostics(wave_count);

            for (std::size_t local = 0; local < wave_count; ++local) {
                auto result = sources.prepare_acquire(states[wave_begin + local].source, jobs[local]);
                if (!result.ok())
                    return result;
            }

            parallel_for(wave_count, worker_limit, max_active, [&](std::size_t local) {
                states[wave_begin + local].work_status =
                    source_manager_update::execute_acquire(jobs[local], acquisition_results[local]);
            });

            for (std::size_t local = 0; local < wave_count; ++local) {
                auto& state = states[wave_begin + local];
                if (!state.work_status.ok())
                    return state.work_status;
                auto result = sources.apply_acquire(std::move(acquisition_results[local]));
                if (!result.ok())
                    return result;
                state.snapshot = sources.snapshot(state.source);
                if (!state.snapshot)
                    return {status_code::not_found};
                ++summary.acquired;
            }

            parallel_for(wave_count, worker_limit, max_active, [&](std::size_t local) {
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
                    if (!contains_source(states[state_index].dependencies, dependency)) {
                        states[state_index].dependencies.push_back(dependency);
                        result = add_state(dependency);
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

            wave_begin = wave_end;
        }

        summary.discovered = static_cast<std::uint32_t>(states.size());
        auto result = sources.validate_source_graph(operation, diagnostics);
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

        source_parser parser{project};
        std::size_t parsed_count = 0;
        while (!ready.empty()) {
            std::vector<diagnostic_buffer> worker_diagnostics(ready.size());
            parallel_for(ready.size(), worker_limit, max_active, [&](std::size_t local) {
                auto& state = states[ready[local]];
                std::vector<source_environment_import> environment_imports;
                std::vector<const source_interface*> interface_imports;
                std::vector<identity_ref> local_types;
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
                    local_types.reserve(facts.records().size() + facts.enums().size());
                    for (const auto& record : facts.records())
                        local_types.push_back(record.identity);
                    for (const auto& enum_fact : facts.enums())
                        local_types.push_back(enum_fact.identity);

                    state.interface = std::make_unique<source_interface>();
                    state.work_status = state.interface->initialize(local_types, interface_imports);
                }
                catch (const std::bad_alloc&) {
                    state.work_status = {status_code::not_available};
                }
                catch (const std::length_error&) {
                    state.work_status = {status_code::not_available};
                }
            });

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

        source_frontend_result candidate;
        candidate.entries.reserve(states.size());
        for (auto& state : states) {
            candidate.entries.push_back(source_frontend_entry{
                state.source,
                std::move(state.parsed),
                std::move(state.interface),
            });
        }
        summary.max_active_workers = max_active.load(std::memory_order_relaxed);
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
