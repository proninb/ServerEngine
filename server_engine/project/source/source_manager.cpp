#include "source_manager.hpp"

#include "../../diagnostics/diagnostic_descriptor.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace cw::server {
namespace {

[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}

[[nodiscard]] std::uint64_t hash_path(std::string_view value) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    hash = mix64(hash);
    return hash == 0 ? 1 : hash;
}

[[nodiscard]] std::size_t next_capacity(std::size_t required) noexcept {
    std::size_t capacity = 16;
    while (capacity < required) {
        if (capacity > (std::numeric_limits<std::size_t>::max)() / 2)
            return 0;
        capacity *= 2;
    }
    return capacity;
}


void emit_source_failure(
    const diagnostic_descriptor& descriptor,
    source_id source,
    operation_id operation,
    std::string detail,
    diagnostic_buffer& diagnostics) noexcept {

    try {
        diagnostics.emit(diagnostic_record{
            descriptor.id,
            descriptor.default_severity,
            operation,
            source_range{source, 0, 0},
            std::move(detail),
        });
    }
    catch (...) {
    }
}

} // namespace

status source_manager::find_in_index(
    std::string_view normalized_path,
    std::span<const path_slot> index,
    source_id& output) const noexcept {

    output = {};
    if (normalized_path.empty() || index.empty())
        return {status_code::not_found};

    const auto hash = hash_path(normalized_path);
    const auto mask = index.size() - 1;
    auto position = static_cast<std::size_t>(hash) & mask;
    for (std::size_t probe = 0; probe < index.size(); ++probe) {
        const auto& slot = index[position];
        if (!slot.source)
            return {status_code::not_found};
        if (slot.hash == hash) {
            const auto source_index = static_cast<std::size_t>(slot.source.value() - 1);
            if (source_index < records.size() &&
                records[source_index].record.normalized_path == normalized_path) {
                output = slot.source;
                return {};
            }
        }
        position = (position + 1) & mask;
    }
    return {status_code::not_found};
}

status source_manager::find(std::string_view normalized_path, source_id& output) const noexcept {
    return find_in_index(normalized_path, path_index, output);
}

status source_manager::rebuild_path_index(
    std::size_t additional,
    std::vector<path_slot>& output) const noexcept {

    if (records.size() > (std::numeric_limits<std::size_t>::max)() - additional)
        return {status_code::not_available};
    const auto count = records.size() + additional;
    const auto required = count > ((std::numeric_limits<std::size_t>::max)() / 2)
        ? 0
        : count * 2 + 1;
    const auto capacity = next_capacity(required);
    if (capacity == 0)
        return {status_code::not_available};

    try {
        output.assign(capacity, path_slot{});
        const auto mask = capacity - 1;
        for (const auto& item : records) {
            const auto hash = hash_path(item.record.normalized_path);
            auto position = static_cast<std::size_t>(hash) & mask;
            while (output[position].source)
                position = (position + 1) & mask;
            output[position] = path_slot{hash, item.record.id};
        }
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

source_manager_update source_manager::begin_update() noexcept {
    return source_manager_update{*this};
}

source_snapshot source_manager::current(source_id source) const noexcept {
    if (!source)
        return {};
    const auto index = static_cast<std::size_t>(source.value() - 1);
    return index < records.size() ? records[index].snapshot : source_snapshot{};
}

std::span<const source_id> source_manager::includes(source_id source) const noexcept {
    if (!source)
        return {};
    const auto index = static_cast<std::size_t>(source.value() - 1);
    return index < records.size()
        ? std::span<const source_id>{records[index].includes}
        : std::span<const source_id>{};
}

std::string_view source_manager::path(source_id source) const noexcept {
    if (!source)
        return {};
    const auto index = static_cast<std::size_t>(source.value() - 1);
    return index < records.size()
        ? std::string_view{records[index].record.normalized_path}
        : std::string_view{};
}

status source_manager::publish_memory(
    std::string_view normalized_path,
    std::string_view text,
    source_snapshot& output,
    source_id* identity) noexcept {

    try {
        auto update = begin_update();
        source_id source;
        auto result = update.resolve(std::filesystem::path{normalized_path}, source);
        if (!result.ok())
            return result;

        auto storage = std::make_shared<source_snapshot::storage>();
        storage->source = source;
        storage->normalized_path.assign(update.path(source));
        storage->text.assign(text);
        storage->observation.size = text.size();
        storage->hash = hash_source_content(text);

        source_acquire_result acquired;
        acquired.source = source;
        acquired.kind = source_acquire_result_kind::present;
        acquired.snapshot.observation = storage->observation;
        acquired.snapshot.hash = storage->hash;
        acquired.snapshot.bytes = storage->text;

        result = update.apply_acquire(std::move(acquired));
        if (!result.ok())
            return result;
        result = update.commit();
        if (!result.ok())
            return result;

        output = current(source);
        if (identity != nullptr)
            *identity = source;
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

status source_manager_update::normalize_path(
    const std::filesystem::path& input,
    std::string& output) noexcept {

    if (input.empty())
        return {status_code::invalid_argument};

    try {
        std::error_code error;
        auto normalized = std::filesystem::absolute(input, error);
        if (error)
            return {status_code::io_failed};
        normalized = normalized.lexically_normal();
        output = normalized.generic_string();
#ifdef _WIN32
        if (output.size() >= 2 && output[1] == ':' && output[0] >= 'A' && output[0] <= 'Z')
            output[0] = static_cast<char>(output[0] - 'A' + 'a');
#endif
        return output.empty() ? status{status_code::invalid_argument} : status{};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

status source_manager_update::ensure_id_index_capacity(std::size_t required) noexcept {
    if (id_index.size() >= required * 2 && !id_index.empty())
        return {};
    const auto capacity = next_capacity(required * 2 + 1);
    if (capacity == 0)
        return {status_code::not_available};
    try {
        std::vector<id_slot> replacement(capacity);
        const auto mask = capacity - 1;
        for (std::uint32_t index = 0; index < candidates.size(); ++index) {
            const auto source = candidates[index].source;
            auto position = static_cast<std::size_t>(mix64(source.value())) & mask;
            while (replacement[position].source)
                position = (position + 1) & mask;
            replacement[position] = id_slot{source, index + 1};
        }
        id_index.swap(replacement);
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

status source_manager_update::ensure_local_path_capacity(std::size_t required) noexcept {
    if (local_path_index.size() >= required * 2 && !local_path_index.empty())
        return {};
    const auto capacity = next_capacity(required * 2 + 1);
    if (capacity == 0)
        return {status_code::not_available};
    try {
        std::vector<local_path_slot> replacement(capacity);
        const auto mask = capacity - 1;
        for (std::uint32_t index = 0; index < new_sources.size(); ++index) {
            const auto hash = hash_path(new_sources[index].normalized_path);
            auto position = static_cast<std::size_t>(hash) & mask;
            while (replacement[position].new_source_index != 0)
                position = (position + 1) & mask;
            replacement[position] = local_path_slot{hash, index + 1};
        }
        local_path_index.swap(replacement);
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

status source_manager_update::find_local_path(
    std::string_view normalized,
    source_id& output) const noexcept {

    output = {};
    if (local_path_index.empty())
        return {status_code::not_found};
    const auto hash = hash_path(normalized);
    const auto mask = local_path_index.size() - 1;
    auto position = static_cast<std::size_t>(hash) & mask;
    for (std::size_t probe = 0; probe < local_path_index.size(); ++probe) {
        const auto& slot = local_path_index[position];
        if (slot.new_source_index == 0)
            return {status_code::not_found};
        if (slot.hash == hash) {
            const auto index = static_cast<std::size_t>(slot.new_source_index - 1);
            if (index < new_sources.size() && new_sources[index].normalized_path == normalized) {
                output = new_sources[index].source;
                return {};
            }
        }
        position = (position + 1) & mask;
    }
    return {status_code::not_found};
}

status source_manager_update::insert_local_path(std::uint32_t new_source_index) noexcept {
    if (local_path_index.empty())
        return {status_code::initialization_failed};
    const auto index = static_cast<std::size_t>(new_source_index - 1);
    const auto hash = hash_path(new_sources[index].normalized_path);
    const auto mask = local_path_index.size() - 1;
    auto position = static_cast<std::size_t>(hash) & mask;
    while (local_path_index[position].new_source_index != 0)
        position = (position + 1) & mask;
    local_path_index[position] = local_path_slot{hash, new_source_index};
    return {};
}

status source_manager_update::resolve(
    const std::filesystem::path& path_value,
    source_id& output) noexcept {

    output = {};
    if (owner == nullptr || committed)
        return {status_code::invalid_argument};

    std::string normalized;
    auto result = normalize_path(path_value, normalized);
    if (!result.ok())
        return result;

    if (owner->find(normalized, output).ok())
        return {};
    if (find_local_path(normalized, output).ok())
        return {};

    if (owner->records.size() + new_sources.size() >=
        static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        return {status_code::not_available};
    }

    try {
        result = ensure_local_path_capacity(new_sources.size() + 1);
        if (!result.ok())
            return result;
        const auto value = static_cast<std::uint32_t>(owner->records.size() + new_sources.size() + 1);
        new_sources.push_back(new_source{source_id{value}, std::move(normalized)});
        result = insert_local_path(static_cast<std::uint32_t>(new_sources.size()));
        if (!result.ok()) {
            new_sources.pop_back();
            return result;
        }
        output = source_id{value};
        prepared = false;
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

std::string_view source_manager_update::path(source_id source) const noexcept {
    if (!source || owner == nullptr)
        return {};
    const auto value = static_cast<std::size_t>(source.value());
    if (value <= owner->records.size())
        return owner->records[value - 1].record.normalized_path;
    const auto local = value - owner->records.size() - 1;
    return local < new_sources.size() ? std::string_view{new_sources[local].normalized_path} : std::string_view{};
}

status source_manager_update::resolve_include(
    source_id including_source,
    std::string_view relative_path,
    source_id& output) noexcept {

    const auto including_path = path(including_source);
    if (including_path.empty() || relative_path.empty())
        return {status_code::invalid_argument};
    try {
        const auto parent = std::filesystem::path{including_path}.parent_path();
        return resolve(parent / std::filesystem::path{relative_path}, output);
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

source_manager_update::candidate_source* source_manager_update::candidate(source_id source) noexcept {
    if (!source || id_index.empty())
        return nullptr;
    const auto mask = id_index.size() - 1;
    auto position = static_cast<std::size_t>(mix64(source.value())) & mask;
    for (std::size_t probe = 0; probe < id_index.size(); ++probe) {
        const auto& slot = id_index[position];
        if (!slot.source)
            return nullptr;
        if (slot.source == source) {
            const auto index = static_cast<std::size_t>(slot.candidate_index - 1);
            return index < candidates.size() ? &candidates[index] : nullptr;
        }
        position = (position + 1) & mask;
    }
    return nullptr;
}

const source_manager_update::candidate_source* source_manager_update::candidate(source_id source) const noexcept {
    return const_cast<source_manager_update*>(this)->candidate(source);
}

status source_manager_update::touch(source_id source, candidate_source*& output) noexcept {
    output = candidate(source);
    if (output != nullptr)
        return {};
    if (!valid_source(source))
        return {status_code::invalid_argument};

    try {
        const auto result = ensure_id_index_capacity(candidates.size() + 1);
        if (!result.ok())
            return result;
        candidate_source value;
        value.source = source;
        candidates.push_back(std::move(value));
        const auto mask = id_index.size() - 1;
        auto position = static_cast<std::size_t>(mix64(source.value())) & mask;
        while (id_index[position].source)
            position = (position + 1) & mask;
        id_index[position] = id_slot{source, static_cast<std::uint32_t>(candidates.size())};
        output = &candidates.back();
        prepared = false;
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

bool source_manager_update::valid_source(source_id source) const noexcept {
    return source && static_cast<std::size_t>(source.value()) <= source_count();
}

status source_manager_update::prepare_acquire(
    source_id source,
    source_acquire_job& output) const noexcept {

    const auto normalized = path(source);
    if (normalized.empty())
        return {status_code::invalid_argument};

    try {
        output = {};
        output.source = source;
        output.path = std::filesystem::path{normalized};
        const auto current_snapshot = snapshot(source);
        if (current_snapshot)
            output.baseline = current_snapshot.observation();
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

status source_manager_update::execute_acquire(
    const source_acquire_job& job,
    source_acquire_result& output) noexcept {

    if (!job.source || job.path.empty())
        return {status_code::invalid_argument};

    file_snapshot snapshot_value;
    const auto result = acquire_file_snapshot(job.path, job.baseline, snapshot_value);
    output = {};
    output.source = job.source;

    switch (result) {
    case file_snapshot_result::unchanged:
        output.kind = source_acquire_result_kind::unchanged;
        return {};
    case file_snapshot_result::missing:
        output.kind = source_acquire_result_kind::missing;
        return {};
    case file_snapshot_result::acquired:
        output.kind = source_acquire_result_kind::present;
        output.snapshot = std::move(snapshot_value);
        return {};
    case file_snapshot_result::allocation_failed:
        return {status_code::not_available};
    case file_snapshot_result::changed_during_read:
    case file_snapshot_result::failed:
        return {status_code::io_failed};
    }
    return {status_code::io_failed};
}

status source_manager_update::apply_acquire(source_acquire_result&& result) noexcept {
    if (!valid_source(result.source))
        return {status_code::invalid_argument};
    if (result.kind == source_acquire_result_kind::unchanged)
        return {};
    if (result.kind == source_acquire_result_kind::missing)
        return {status_code::not_found};

    candidate_source* item = nullptr;
    auto touch_result = touch(result.source, item);
    if (!touch_result.ok())
        return touch_result;

    try {
        auto storage = std::make_shared<source_snapshot::storage>();
        storage->source = result.source;
        storage->normalized_path.assign(path(result.source));
        storage->text = std::move(result.snapshot.bytes);
        storage->observation = result.snapshot.observation;
        storage->hash = result.snapshot.hash;
        item->snapshot = source_snapshot{std::move(storage)};
        item->has_snapshot = true;
        prepared = false;
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

status source_manager_update::set_includes(
    source_id source,
    std::span<const source_id> dependencies) noexcept {

    if (!valid_source(source))
        return {status_code::invalid_argument};
    for (const auto dependency : dependencies) {
        if (!valid_source(dependency) || dependency == source)
            return {status_code::invalid_argument};
    }

    candidate_source* item = nullptr;
    auto result = touch(source, item);
    if (!result.ok())
        return result;
    try {
        item->includes.assign(dependencies.begin(), dependencies.end());
        item->has_includes = true;
        prepared = false;
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

source_snapshot source_manager_update::snapshot(source_id source) const noexcept {
    if (const auto* item = candidate(source); item != nullptr && item->has_snapshot)
        return item->snapshot;
    return owner != nullptr ? owner->current(source) : source_snapshot{};
}

std::span<const source_id> source_manager_update::includes(source_id source) const noexcept {
    if (const auto* item = candidate(source); item != nullptr && item->has_includes)
        return item->includes;
    return owner != nullptr ? owner->includes(source) : std::span<const source_id>{};
}

std::size_t source_manager_update::source_count() const noexcept {
    return owner != nullptr ? owner->records.size() + new_sources.size() : 0;
}

status source_manager_update::validate_source_graph(
    operation_id operation,
    diagnostic_buffer& diagnostics) const noexcept {

    const auto count = source_count();
    try {
        std::vector<std::uint32_t> indegree(count + 1, 0);
        for (std::size_t value = 1; value <= count; ++value) {
            const auto source = source_id{static_cast<std::uint32_t>(value)};
            for (const auto dependency : includes(source)) {
                if (!valid_source(dependency)) {
                    emit_source_failure(
                        diagnostics::source_dependency_invalid,
                        source,
                        operation,
                        "Source dependency references an unknown source_id",
                        diagnostics);
                    return {status_code::invalid_argument};
                }
                ++indegree[dependency.value()];
            }
        }

        std::vector<source_id> queue;
        queue.reserve(count);
        for (std::size_t value = 1; value <= count; ++value) {
            if (indegree[value] == 0)
                queue.push_back(source_id{static_cast<std::uint32_t>(value)});
        }

        std::size_t processed = 0;
        for (std::size_t cursor = 0; cursor < queue.size(); ++cursor) {
            const auto source = queue[cursor];
            ++processed;
            for (const auto dependency : includes(source)) {
                auto& degree = indegree[dependency.value()];
                if (--degree == 0)
                    queue.push_back(dependency);
            }
        }

        if (processed != count) {
            emit_source_failure(
                diagnostics::source_dependency_cycle,
                {},
                operation,
                "Source include dependency graph contains a cycle",
                diagnostics);
            return {status_code::semantic_conflict};
        }
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

status source_manager_update::build_prepared_path_index() noexcept {
    if (owner == nullptr)
        return {status_code::invalid_argument};
    auto result = owner->rebuild_path_index(new_sources.size(), prepared_path_index);
    if (!result.ok())
        return result;
    const auto mask = prepared_path_index.size() - 1;
    for (const auto& item : new_sources) {
        const auto hash = hash_path(item.normalized_path);
        auto position = static_cast<std::size_t>(hash) & mask;
        while (prepared_path_index[position].source)
            position = (position + 1) & mask;
        prepared_path_index[position] = source_manager::path_slot{hash, item.source};
    }
    return {};
}

status source_manager_update::prepare_publish() noexcept {
    if (owner == nullptr || committed)
        return {status_code::invalid_argument};
    if (prepared)
        return {};

    auto result = build_prepared_path_index();
    if (!result.ok())
        return result;
    try {
        owner->records.reserve(owner->records.size() + new_sources.size());
        prepared = true;
        return {};
    }
    catch (const std::bad_alloc&) {
        prepared_path_index.clear();
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        prepared_path_index.clear();
        return {status_code::not_available};
    }
}

void source_manager_update::publish_prepared() noexcept {
    if (!prepared || committed || owner == nullptr)
        return;

    for (auto& item : new_sources) {
        source_manager::committed_source committed_source;
        committed_source.record.id = item.source;
        committed_source.record.normalized_path = std::move(item.normalized_path);
        owner->records.push_back(std::move(committed_source));
    }

    for (auto& item : candidates) {
        const auto index = static_cast<std::size_t>(item.source.value() - 1);
        if (index >= owner->records.size())
            continue;
        if (item.has_snapshot)
            owner->records[index].snapshot = std::move(item.snapshot);
        if (item.has_includes)
            owner->records[index].includes = std::move(item.includes);
    }

    owner->path_index.swap(prepared_path_index);
    committed = true;
}

status source_manager_update::commit() noexcept {
    auto result = prepare_publish();
    if (!result.ok())
        return result;
    publish_prepared();
    return committed ? status{} : status{status_code::initialization_failed};
}

} // namespace cw::server
