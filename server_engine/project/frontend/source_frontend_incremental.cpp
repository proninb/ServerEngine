#include "source_frontend_generation.hpp"

#include <cstdio>
#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>

namespace cw::server {
namespace {

[[nodiscard]] std::size_t default_incremental_acquisition_workers(
    std::size_t cpu_workers) noexcept {

#ifdef _WIN32
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

struct incremental_import final {
    std::uint32_t visible_from = 0;
    source_id dependency{};
};

struct incremental_source_state final {
    source_id source{};
    source_snapshot snapshot;
    std::vector<parser_token> tokens;
    std::vector<source_include_directive> directives;
    std::vector<source_id> dependencies;
    std::vector<incremental_import> imports;
    std::vector<std::uint32_t> dependents;
    parsed_source parsed;
    std::unique_ptr<source_interface> interface;
    std::uint32_t remaining = 0;
    bool physical_changed = false;
    bool new_source = false;
    bool removed = false;
    bool discovered = false;
    bool parsed_value = false;
};

[[nodiscard]] std::uint64_t source_mix(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

class source_index final {
public:
    [[nodiscard]] status find(source_id source, std::uint32_t& output) const noexcept {
        output = 0;
        if (!source || slots.empty())
            return {status_code::not_found};
        const auto mask = slots.size() - 1;
        auto position = static_cast<std::size_t>(source_mix(source.value())) & mask;
        for (std::size_t probe = 0; probe < slots.size(); ++probe) {
            const auto& slot = slots[position];
            if (slot.source == 0)
                return {status_code::not_found};
            if (slot.source == source.value()) {
                output = slot.position;
                return {};
            }
            position = (position + 1) & mask;
        }
        return {status_code::not_found};
    }

    [[nodiscard]] status insert(source_id source, std::uint32_t position_value, bool& inserted) noexcept {
        inserted = false;
        if (!source || position_value == 0)
            return {status_code::invalid_argument};
        if (slots.empty() || (count + 1) * 2 >= slots.size()) {
            auto result = grow(slots.empty() ? std::size_t{16} : slots.size() * 2);
            if (!result.ok())
                return result;
        }
        const auto mask = slots.size() - 1;
        auto position = static_cast<std::size_t>(source_mix(source.value())) & mask;
        for (;;) {
            auto& slot = slots[position];
            if (slot.source == 0) {
                slot = index_slot{source.value(), position_value};
                ++count;
                inserted = true;
                return {};
            }
            if (slot.source == source.value())
                return {};
            position = (position + 1) & mask;
        }
    }

private:
    struct index_slot final {
        std::uint32_t source = 0;
        std::uint32_t position = 0;
    };

    [[nodiscard]] status grow(std::size_t capacity) noexcept {
        try {
            std::vector<index_slot> replacement(capacity);
            const auto mask = replacement.size() - 1;
            for (const auto& old : slots) {
                if (old.source == 0)
                    continue;
                auto position = static_cast<std::size_t>(source_mix(old.source)) & mask;
                while (replacement[position].source != 0)
                    position = (position + 1) & mask;
                replacement[position] = old;
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

    std::vector<index_slot> slots;
    std::size_t count = 0;
};


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
        auto position = static_cast<std::size_t>(source_mix(key)) & mask;
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
                auto position = static_cast<std::size_t>(source_mix(key)) & mask;
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

source_frontend_generation::source_frontend_generation(
    project_semantic_services semantic_value,
    source_manager_update& source_update,
    const source_frontend_cache& cache_value,
    std::size_t worker_limit_value,
    std::size_t acquisition_worker_limit_value) noexcept
    : semantic(semantic_value), sources(source_update), cache(&cache_value),
      worker_limit(worker_limit_value == 0
          ? (std::thread::hardware_concurrency() == 0 ? 1 : std::thread::hardware_concurrency())
          : worker_limit_value),
      acquisition_worker_limit(
          acquisition_worker_limit_value == 0
              ? default_incremental_acquisition_workers(
                    worker_limit)
              : (std::max)(
                    std::size_t{1},
                    acquisition_worker_limit_value)) {}

source_frontend_generation::source_frontend_generation(
    project_context& project_value,
    source_manager_update& source_update,
    const source_frontend_cache& cache_value,
    std::size_t worker_limit_value,
    std::size_t acquisition_worker_limit_value) noexcept
    : source_frontend_generation(
          project_value.parser_services(),
          source_update,
          cache_value,
          worker_limit_value,
          acquisition_worker_limit_value) {}

status source_frontend_generation::build_incremental(
    std::span<const source_id> dirty_sources,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    source_frontend_result& output) noexcept {

    output = {};
    if (cache == nullptr || !cache->complete())
        return {status_code::not_available};
    if (dirty_sources.empty())
        return {};

    try {
        source_frontend_summary summary;
        summary.dirty = static_cast<std::uint32_t>(dirty_sources.size());
        summary.worker_limit = worker_limit;
        summary.acquisition_worker_limit =
            acquisition_worker_limit;
        const auto initial_source_count = sources.source_count();

        source_index dirty_index;
        std::vector<source_id> unique_dirty;
        unique_dirty.reserve(dirty_sources.size());
        for (const auto source : dirty_sources) {
            if (!source ||
                static_cast<std::size_t>(source.value()) >
                    initial_source_count) {
                std::fprintf(
                    stderr,
                    "[C1C2C-STAGE] incremental dirty invalid source=%u count=%zu\n",
                    source.value(),
                    initial_source_count);
                return {status_code::invalid_argument};
            }
            bool inserted = false;
            auto result = dirty_index.insert(
                source, static_cast<std::uint32_t>(unique_dirty.size() + 1), inserted);
            if (!result.ok())
                return result;
            if (inserted)
                unique_dirty.push_back(source);
        }
        summary.dirty = static_cast<std::uint32_t>(unique_dirty.size());

        std::vector<source_acquire_job> jobs(unique_dirty.size());
        std::vector<source_acquire_result> acquired(unique_dirty.size());
        for (std::size_t index = 0; index < unique_dirty.size(); ++index) {
            auto result =
                sources.prepare_acquire(
                    unique_dirty[index],
                    jobs[index]);
            if (!result.ok()) {
                std::fprintf(
                    stderr,
                    "[C1C2C-STAGE] incremental prepare_acquire code=%u source=%u\n",
                    static_cast<unsigned>(result.code),
                    unique_dirty[index].value());
                return result;
            }
        }

        std::vector<status> acquire_status(unique_dirty.size());
        const auto worker_count = (std::min)(
            unique_dirty.size(),
            (std::max)(
                std::size_t{1},
                acquisition_worker_limit));
        if (worker_count <= 1) {
            for (std::size_t index = 0; index < jobs.size(); ++index)
                acquire_status[index] = source_manager_update::execute_acquire(jobs[index], acquired[index]);
        } else {
            std::atomic<std::size_t> next{0};
            std::vector<std::thread> workers;
            workers.reserve(worker_count);
            for (std::size_t worker = 0; worker < worker_count; ++worker) {
                workers.emplace_back([&] {
                    for (;;) {
                        const auto index = next.fetch_add(1, std::memory_order_relaxed);
                        if (index >= jobs.size())
                            break;
                        acquire_status[index] = source_manager_update::execute_acquire(jobs[index], acquired[index]);
                    }
                });
            }
            for (auto& worker : workers)
                worker.join();
        }
        summary.max_active_workers = worker_count;

        for (std::size_t index = 0; index < acquired.size(); ++index) {
            if (!acquire_status[index].ok())
                return acquire_status[index];
            auto result =
                sources.apply_acquire(
                    std::move(acquired[index]));
            if (!result.ok()) {
                std::fprintf(
                    stderr,
                    "[C1C2C-STAGE] incremental apply_acquire code=%u index=%zu\n",
                    static_cast<unsigned>(result.code),
                    index);
                return result;
            }
            ++summary.acquired;
        }

        const auto changed = sources.changed_sources();
        summary.changed = static_cast<std::uint32_t>(changed.size());
        if (changed.empty()) {
            source_frontend_result candidate;
            candidate.statistics = summary;
            output = std::move(candidate);
            return {};
        }

        std::vector<incremental_source_state> states;
        source_index state_index;
        auto add_state = [&](source_id source, bool physical_changed, bool new_source) -> status {
            std::uint32_t existing = 0;
            if (state_index.find(source, existing).ok()) {
                auto& state = states[existing - 1];
                state.physical_changed = state.physical_changed || physical_changed;
                state.new_source = state.new_source || new_source;
                return {};
            }
            if (states.size() >= static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)()))
                return {status_code::not_available};
            incremental_source_state state;
            state.source = source;
            state.physical_changed = physical_changed;
            state.new_source = new_source;
            states.push_back(std::move(state));
            bool inserted = false;
            return state_index.insert(
                source, static_cast<std::uint32_t>(states.size()), inserted);
        };

        std::vector<source_id> old_dependents;
        for (const auto source : changed) {
            auto result = add_state(source, true, false);
            if (!result.ok())
                return result;
            result =
                sources.collect_dependents(
                    source,
                    old_dependents);
            if (!result.ok()) {
                std::fprintf(
                    stderr,
                    "[C1C2C-STAGE] incremental collect_dependents code=%u source=%u\n",
                    static_cast<unsigned>(result.code),
                    source.value());
                return result;
            }
            for (const auto dependent : old_dependents) {
                result = add_state(dependent, false, false);
                if (!result.ok())
                    return result;
            }
        }

        std::vector<source_id> include_changed;
        source_edge_set dependency_edges;
        std::size_t discovery_cursor = 0;
        while (discovery_cursor < states.size()) {
            const auto current_source = states[discovery_cursor].source;
            states[discovery_cursor].snapshot = sources.snapshot(current_source);
            if (!states[discovery_cursor].snapshot) {
                if (!states[discovery_cursor].physical_changed || states[discovery_cursor].new_source)
                    return {status_code::not_found};
                states[discovery_cursor].removed = true;
                auto result = sources.set_includes(current_source, {});
                if (!result.ok())
                    return result;
                include_changed.push_back(current_source);
                states[discovery_cursor].discovered = true;
                ++discovery_cursor;
                continue;
            }

            diagnostic_buffer local_diagnostics;
            std::vector<directive_span> directive_spans;
            auto result = lex_source(
                states[discovery_cursor].snapshot, operation, local_diagnostics,
                states[discovery_cursor].tokens, &directive_spans);
            if (result.ok()) {
                result = discover_source_includes(
                    states[discovery_cursor].snapshot,
                    states[discovery_cursor].tokens,
                    directive_spans,
                    operation,
                    local_diagnostics,
                    states[discovery_cursor].directives);
            }
            merge_diagnostics(local_diagnostics, diagnostics);
            if (!result.ok())
                return result;
            ++summary.lexed;

            const auto directive_count = states[discovery_cursor].directives.size();
            for (std::size_t directive_index = 0; directive_index < directive_count; ++directive_index) {
                const auto include = states[discovery_cursor].directives[directive_index];
                const auto snapshot = states[discovery_cursor].snapshot;
                const auto path_text = snapshot.text().substr(include.path.offset, include.path.length);
                source_id dependency;
                result = sources.resolve_include(current_source, path_text, dependency);
                if (!result.ok())
                    return result;
                bool edge_inserted = false;
                result = dependency_edges.insert(current_source, dependency, edge_inserted);
                if (!result.ok())
                    return result;
                if (edge_inserted)
                    states[discovery_cursor].dependencies.push_back(dependency);
                states[discovery_cursor].imports.push_back(
                    incremental_import{include.visible_from, dependency});

                if (static_cast<std::size_t>(dependency.value()) > initial_source_count) {
                    std::uint32_t dependency_position = 0;
                    if (!state_index.find(dependency, dependency_position).ok()) {
                        source_acquire_job job;
                        source_acquire_result acquired_dependency;
                        result = sources.prepare_acquire(dependency, job);
                        if (!result.ok())
                            return result;
                        result = source_manager_update::execute_acquire(job, acquired_dependency);
                        if (!result.ok())
                            return result;
                        result = sources.apply_acquire(std::move(acquired_dependency));
                        if (!result.ok())
                            return result;
                        ++summary.acquired;
                        result = add_state(dependency, true, true);
                        if (!result.ok())
                            return result;
                    }
                }
            }

            if (states[discovery_cursor].physical_changed || states[discovery_cursor].new_source) {
                result = sources.set_includes(
                    current_source,
                    states[discovery_cursor].dependencies);
                if (!result.ok()) {
                    std::fprintf(
                        stderr,
                        "[C1C2C-STAGE] incremental set_includes code=%u source=%u deps=%zu\n",
                        static_cast<unsigned>(result.code),
                        current_source.value(),
                        states[discovery_cursor].dependencies.size());
                    return result;
                }
                include_changed.push_back(current_source);
            }
            states[discovery_cursor].discovered = true;
            ++discovery_cursor;
        }

        summary.discovered = static_cast<std::uint32_t>(states.size());
        summary.affected = summary.discovered;
        auto result =
            sources.validate_changed_source_graph(
                include_changed,
                operation,
                diagnostics,
                &summary.source_graph_visited);
        if (!result.ok()) {
            std::fprintf(
                stderr,
                "[C1C2C-STAGE] incremental validate_changed_graph code=%u roots=%zu\n",
                static_cast<unsigned>(result.code),
                include_changed.size());
            return result;
        }

        for (std::uint32_t index = 0; index < states.size(); ++index) {
            auto& state = states[index];
            if (state.removed)
                continue;
            for (const auto dependency : state.dependencies) {
                std::uint32_t dependency_position = 0;
                if (!state_index.find(dependency, dependency_position).ok())
                    continue;
                auto& dependency_state = states[dependency_position - 1];
                if (dependency_state.removed)
                    continue;
                ++state.remaining;
                dependency_state.dependents.push_back(index);
            }
        }

        std::vector<std::uint32_t> ready;
        for (std::uint32_t index = 0; index < states.size(); ++index) {
            if (!states[index].removed && states[index].remaining == 0)
                ready.push_back(index);
        }

        source_parser parser{semantic};
        std::size_t parsed_count = 0;
        for (std::size_t cursor = 0; cursor < ready.size(); ++cursor) {
            auto& state = states[ready[cursor]];
            std::vector<source_environment_import> environment_imports;
            std::vector<const source_interface*> interface_imports;
            environment_imports.reserve(state.imports.size());
            interface_imports.reserve(state.dependencies.size());

            for (const auto& item : state.imports) {
                std::uint32_t dependency_position = 0;
                const source_interface* imported = nullptr;
                if (state_index.find(item.dependency, dependency_position).ok())
                    imported = states[dependency_position - 1].interface.get();
                else {
                    imported = cache->interface(item.dependency);
                    if (imported != nullptr)
                        ++summary.reused_interfaces;
                }
                if (imported == nullptr)
                    return {status_code::not_found};
                environment_imports.push_back(source_environment_import{item.visible_from, imported});
            }

            for (const auto dependency : state.dependencies) {
                std::uint32_t dependency_position = 0;
                const source_interface* imported = nullptr;
                if (state_index.find(dependency, dependency_position).ok())
                    imported = states[dependency_position - 1].interface.get();
                else
                    imported = cache->interface(dependency);
                if (imported == nullptr)
                    return {status_code::not_found};
                interface_imports.push_back(imported);
            }

            diagnostic_buffer local_diagnostics;
            const source_environment environment{environment_imports};
            result = parser.parse(
                state.snapshot, state.tokens, environment, operation,
                local_diagnostics, state.parsed);
            merge_diagnostics(local_diagnostics, diagnostics);
            if (!result.ok())
                return result;

            const auto facts = state.parsed.facts();

            state.interface = std::make_unique<source_interface>();
            result =
                state.interface->initialize(
                    facts,
                    semantic.identities(),
                    interface_imports);
            if (!result.ok()) {
                std::fprintf(
                    stderr,
                    "[C1C2C-STAGE] incremental interface.initialize code=%u source=%u\n",
                    static_cast<unsigned>(result.code),
                    state.source.value());
                return result;
            }
            state.parsed_value = true;
            ++parsed_count;
            ++summary.parsed;

            for (const auto dependent_index : state.dependents) {
                auto& dependent = states[dependent_index];
                if (dependent.remaining == 0)
                    return {status_code::invalid_argument};
                --dependent.remaining;
                if (dependent.remaining == 0)
                    ready.push_back(dependent_index);
            }
        }

        std::size_t expected_parsed = 0;
        for (const auto& state : states) {
            if (!state.removed)
                ++expected_parsed;
        }
        if (parsed_count != expected_parsed)
            return {status_code::semantic_conflict};

        source_frontend_result candidate;
        candidate.entries.reserve(states.size());
        for (auto& state : states) {
            candidate.entries.push_back(source_frontend_entry{
                state.source,
                std::move(state.parsed),
                std::move(state.interface),
                state.removed,
            });
        }
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
