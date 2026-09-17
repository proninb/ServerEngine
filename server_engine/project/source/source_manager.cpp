#include "source_manager.hpp"

#include "../persistence/build_cache_image.hpp"
#include "../persistence/source_manager_image.hpp"
#include "../../diagnostics/diagnostic_descriptor.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
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

[[nodiscard]] std::uint64_t read64(const char* data) noexcept {
    std::uint64_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

[[nodiscard]] std::uint32_t read32(const char* data) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

[[nodiscard]] constexpr std::uint64_t xxh64_round(
    std::uint64_t accumulator,
    std::uint64_t input) noexcept {

    accumulator += input * 14029467366897019727ULL;
    accumulator = std::rotl(accumulator, 31);
    return accumulator * 11400714785074694791ULL;
}

[[nodiscard]] std::uint64_t hash_path(std::string_view value) noexcept {
    constexpr std::uint64_t prime1 = 11400714785074694791ULL;
    constexpr std::uint64_t prime2 = 14029467366897019727ULL;
    constexpr std::uint64_t prime3 = 1609587929392839161ULL;
    constexpr std::uint64_t prime4 = 9650029242287828579ULL;
    constexpr std::uint64_t prime5 = 2870177450012600261ULL;

    const char* position = value.data();
    const char* const end = position + value.size();
    std::uint64_t hash = 0;

    if (value.size() >= 32) {
        std::uint64_t lane1 = prime1 + prime2;
        std::uint64_t lane2 = prime2;
        std::uint64_t lane3 = 0;
        std::uint64_t lane4 = 0 - prime1;
        const char* const limit = end - 32;

        do {
            lane1 = xxh64_round(lane1, read64(position));
            position += 8;
            lane2 = xxh64_round(lane2, read64(position));
            position += 8;
            lane3 = xxh64_round(lane3, read64(position));
            position += 8;
            lane4 = xxh64_round(lane4, read64(position));
            position += 8;
        } while (position <= limit);

        hash =
            std::rotl(lane1, 1) +
            std::rotl(lane2, 7) +
            std::rotl(lane3, 12) +
            std::rotl(lane4, 18);

        const std::uint64_t lanes[] = {lane1, lane2, lane3, lane4};
        for (const auto lane : lanes) {
            hash ^= xxh64_round(0, lane);
            hash = hash * prime1 + prime4;
        }
    }
    else {
        hash = prime5;
    }

    hash += value.size();

    while (position + 8 <= end) {
        const auto lane = xxh64_round(0, read64(position));
        hash ^= lane;
        hash = std::rotl(hash, 27) * prime1 + prime4;
        position += 8;
    }

    if (position + 4 <= end) {
        hash ^= static_cast<std::uint64_t>(read32(position)) * prime1;
        hash = std::rotl(hash, 23) * prime2 + prime3;
        position += 4;
    }

    while (position < end) {
        hash ^= static_cast<unsigned char>(*position++) * prime5;
        hash = std::rotl(hash, 11) * prime1;
    }

    hash ^= hash >> 33;
    hash *= prime2;
    hash ^= hash >> 29;
    hash *= prime3;
    hash ^= hash >> 32;
    return hash;
}

[[nodiscard]] constexpr std::uint32_t path_fingerprint(std::uint64_t hash) noexcept {
    const auto folded = static_cast<std::uint32_t>(hash ^ (hash >> 32));
    return folded == 0 ? 1U : folded;
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

[[nodiscard]] bool incremental_headroom(
    std::size_t size,
    std::size_t minimum_extra,
    std::size_t& output) noexcept {

    const auto proportional = size / 16;
    const auto extra = (std::max)(proportional, minimum_extra);
    if (size > (std::numeric_limits<std::size_t>::max)() - extra)
        return false;
    output = size + extra;
    return true;
}

class sparse_source_set final {
public:
    [[nodiscard]] bool contains(source_id source) const noexcept {
        if (!source || slots.empty())
            return false;
        const auto mask = slots.size() - 1;
        auto position = static_cast<std::size_t>(mix64(source.value())) & mask;
        for (;;) {
            const auto slot = slots[position];
            if (slot == 0)
                return false;
            if (slot == source.value())
                return true;
            position = (position + 1) & mask;
        }
    }

    [[nodiscard]] status insert(source_id source, bool& inserted) noexcept {
        inserted = false;
        if (!source)
            return {status_code::invalid_argument};
        if (slots.empty() || (count + 1) * 2 >= slots.size()) {
            const auto required = slots.empty() ? std::size_t{16} : slots.size() * 2;
            auto result = grow(required);
            if (!result.ok())
                return result;
        }

        const auto mask = slots.size() - 1;
        auto position = static_cast<std::size_t>(mix64(source.value())) & mask;
        for (;;) {
            auto& slot = slots[position];
            if (slot == 0) {
                slot = source.value();
                ++count;
                inserted = true;
                return {};
            }
            if (slot == source.value())
                return {};
            position = (position + 1) & mask;
        }
    }

private:
    [[nodiscard]] status grow(std::size_t capacity) noexcept {
        try {
            std::vector<std::uint32_t> replacement(capacity, 0);
            const auto mask = replacement.size() - 1;
            for (const auto value : slots) {
                if (value == 0)
                    continue;
                auto position = static_cast<std::size_t>(mix64(value)) & mask;
                while (replacement[position] != 0)
                    position = (position + 1) & mask;
                replacement[position] = value;
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

    std::vector<std::uint32_t> slots;
    std::size_t count = 0;
};

class sparse_position_set final {
public:
    [[nodiscard]] status reserve(std::size_t count) noexcept {
        if (count == 0)
            return {};
        const auto capacity = next_capacity(count * 2 + 1);
        if (capacity == 0)
            return {status_code::not_available};
        try {
            slots.assign(capacity, empty_value);
            return {};
        }
        catch (const std::bad_alloc&) {
            return {status_code::not_available};
        }
        catch (const std::length_error&) {
            return {status_code::not_available};
        }
    }

    [[nodiscard]] bool contains(std::size_t value) const noexcept {
        if (slots.empty())
            return false;
        const auto mask = slots.size() - 1;
        auto position = static_cast<std::size_t>(mix64(value)) & mask;
        for (;;) {
            const auto slot = slots[position];
            if (slot == empty_value)
                return false;
            if (slot == value)
                return true;
            position = (position + 1) & mask;
        }
    }

    void insert(std::size_t value) noexcept {
        const auto mask = slots.size() - 1;
        auto position = static_cast<std::size_t>(mix64(value)) & mask;
        while (slots[position] != empty_value && slots[position] != value)
            position = (position + 1) & mask;
        slots[position] = value;
    }

private:
    static constexpr std::size_t empty_value = (std::numeric_limits<std::size_t>::max)();
    std::vector<std::size_t> slots;
};


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

std::size_t
source_manager::snapshot_page_store::next_page_capacity(
    const std::vector<page>& pages,
    std::size_t required) noexcept {

    constexpr std::size_t minimum_page = 4u * 1024u;
    constexpr std::size_t maximum_page = 1024u * 1024u;

    if (required == 0)
        return 0;

    std::size_t capacity = minimum_page;

    if (!pages.empty()) {
        const auto previous = pages.back().capacity;
        capacity = previous >= maximum_page
            ? maximum_page
            : (std::min)(previous * 2, maximum_page);
    }

    return (std::max)(capacity, required);
}

status source_manager::snapshot_page_store::append_bytes(
    std::vector<page>& pages,
    std::string_view value,
    std::string_view& output) noexcept {

    output = {};
    if (value.empty())
        return {};

    try {
        if (pages.empty() ||
            pages.back().capacity - pages.back().used <
                value.size()) {

            const auto capacity =
                next_page_capacity(pages, value.size());

            if (capacity < value.size())
                return {status_code::not_available};

            page next;
            next.bytes = std::make_unique<char[]>(capacity);
            next.capacity = capacity;
            pages.push_back(std::move(next));
        }

        auto& current = pages.back();
        auto* destination =
            current.bytes.get() + current.used;

        std::memcpy(
            destination,
            value.data(),
            value.size());

        current.used += value.size();
        output = {
            destination,
            value.size(),
        };
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

status source_manager::snapshot_page_store::append(
    source_id source,
    std::string_view path,
    std::string_view text,
    std::string_view& path_view,
    std::string_view& text_view) noexcept {

    path_view = {};
    text_view = {};

    if (!source || path.empty())
        return {status_code::invalid_argument};

    if (text.size() >
        (std::numeric_limits<std::size_t>::max)() -
            text_bytes_value) {
        return {status_code::not_available};
    }

    auto result =
        append_bytes(
            path_pages,
            path,
            path_view);

    if (!result.ok())
        return result;

    result =
        append_bytes(
            text_pages,
            text,
            text_view);

    if (!result.ok()) {
        path_view = {};
        return result;
    }

    if (text_source_count == 0) {
        first_text_source = source;
        last_text_source = source;
    }
    else {
        if (last_text_source.value() ==
                (std::numeric_limits<std::uint32_t>::max)() ||
            source.value() !=
                last_text_source.value() + 1) {
            text_source_order_contiguous = false;
        }

        last_text_source = source;
    }

    ++text_source_count;
    text_bytes_value += text.size();
    return {};
}

status source_manager::snapshot_page_store::prepare_absorb(
    const snapshot_page_store& other) noexcept {

    if (other.path_pages.empty() &&
        other.text_pages.empty()) {
        return {};
    }

    if (path_pages.size() >
            (std::numeric_limits<std::size_t>::max)() -
                other.path_pages.size() ||
        text_pages.size() >
            (std::numeric_limits<std::size_t>::max)() -
                other.text_pages.size() ||
        text_source_count >
            (std::numeric_limits<std::size_t>::max)() -
                other.text_source_count ||
        text_bytes_value >
            (std::numeric_limits<std::size_t>::max)() -
                other.text_bytes_value) {
        return {status_code::not_available};
    }

    try {
        path_pages.reserve(
            path_pages.size() +
            other.path_pages.size());

        text_pages.reserve(
            text_pages.size() +
            other.text_pages.size());

        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

void source_manager::snapshot_page_store::absorb_prepared(
    snapshot_page_store&& other) noexcept {

    const auto had_text = text_source_count != 0;
    const auto other_had_text =
        other.text_source_count != 0;

    if (other_had_text) {
        if (!had_text) {
            first_text_source =
                other.first_text_source;
            last_text_source =
                other.last_text_source;
            text_source_order_contiguous =
                other.text_source_order_contiguous;
        }
        else {
            const bool bridge =
                last_text_source.value() !=
                    (std::numeric_limits<std::uint32_t>::max)() &&
                other.first_text_source.value() ==
                    last_text_source.value() + 1;

            text_source_order_contiguous =
                text_source_order_contiguous &&
                other.text_source_order_contiguous &&
                bridge;

            last_text_source =
                other.last_text_source;
        }

        text_source_count +=
            other.text_source_count;
        text_bytes_value +=
            other.text_bytes_value;
    }

    for (auto& item : other.path_pages)
        path_pages.push_back(std::move(item));

    for (auto& item : other.text_pages)
        text_pages.push_back(std::move(item));

    other.path_pages.clear();
    other.text_pages.clear();
    other.first_text_source = {};
    other.last_text_source = {};
    other.text_source_count = 0;
    other.text_bytes_value = 0;
    other.text_source_order_contiguous = true;
}

std::size_t
source_manager::snapshot_page_store::reserved_bytes() const noexcept {

    std::size_t total = 0;

    const auto accumulate =
        [&](const std::vector<page>& pages) noexcept {
            for (const auto& item : pages) {
                if (total >
                    (std::numeric_limits<std::size_t>::max)() -
                        item.capacity) {
                    total =
                        (std::numeric_limits<std::size_t>::max)();
                    return;
                }

                total += item.capacity;
            }
        };

    accumulate(path_pages);

    if (total !=
        (std::numeric_limits<std::size_t>::max)()) {
        accumulate(text_pages);
    }

    return total;
}


bool source_manager::snapshot_page_store::
complete_text_generation(
    std::size_t source_count,
    std::uint64_t text_bytes) const noexcept {

    return
        text_source_order_contiguous &&
        text_source_count == source_count &&
        source_count != 0 &&
        first_text_source.value() == 1 &&
        last_text_source.value() == source_count &&
        text_bytes <=
            (std::numeric_limits<std::size_t>::max)() &&
        text_bytes_value ==
            static_cast<std::size_t>(text_bytes);
}

status source_manager::snapshot_page_store::
release_text_generation(
    source_snapshot_generation_storage& output) noexcept {

    output = {};

    if (!text_source_order_contiguous ||
        text_source_count == 0) {
        return {status_code::invalid_state};
    }

    output.text_pages = std::move(text_pages);
    output.text_bytes = text_bytes_value;
    output.source_count = text_source_count;
    output.complete = true;

    first_text_source = {};
    last_text_source = {};
    text_source_count = 0;
    text_bytes_value = 0;
    text_source_order_contiguous = true;

    return output.valid()
        ? status{}
        : status{status_code::initialization_failed};
}


source_manager::source_manager(
    const source_manager_image_view& baseline_sources_value,
    const build_cache_image_view& baseline_cache_value) noexcept
    : baseline_sources(&baseline_sources_value),
      baseline_cache(&baseline_cache_value),
      baseline_source_count(baseline_sources_value.source_count()),
      persistence_text_bytes_value(
          baseline_cache_value.source_bytes_count()) {

    records.bind_baseline(
        this,
        baseline_source_count,
        &source_manager::read_baseline_record);
    states.bind_baseline(
        this,
        baseline_source_count,
        &source_manager::read_baseline_state);
    generation_storage_value.bind_baseline(
        baseline_source_count);
}


status source_manager::read_baseline_record(
    const void* context,
    std::size_t index,
    source_record& output) noexcept {

    output = {};
    const auto* owner = static_cast<const source_manager*>(context);
    if (owner == nullptr || owner->baseline_sources == nullptr ||
        index >= owner->baseline_source_count) {
        return {status_code::not_found};
    }
    return {};
}

status source_manager::read_baseline_state(
    const void* context,
    std::size_t index,
    committed_source& output) noexcept {

    output = {};
    const auto* owner = static_cast<const source_manager*>(context);
    if (owner == nullptr || owner->baseline_sources == nullptr ||
        owner->baseline_cache == nullptr ||
        index >= owner->baseline_source_count ||
        index >= (std::numeric_limits<std::uint32_t>::max)()) {
        return {status_code::not_found};
    }

    const source_id source{static_cast<std::uint32_t>(index + 1)};
    source_manager_image_physical_state physical;
    auto result = owner->baseline_sources->physical(source, physical);
    if (!result.ok())
        return result;

    try {
        if (physical.present) {
            if (physical.size >
                (std::numeric_limits<std::uintmax_t>::max)()) {
                return {status_code::artifact_corrupt};
            }

            const auto text = owner->baseline_cache->source_text(source);
            if (text.size() != physical.size)
                return {status_code::artifact_corrupt};

            output.snapshot = source_snapshot{
                source,
                owner->baseline_sources->path(source),
                text,
                file_snapshot_observation{
                    physical.write_time_ticks,
                    static_cast<std::uintmax_t>(physical.size)},
                physical.hash};
        }


        const auto includes =
            owner->baseline_sources->includes(source);
        output.baseline_includes.reserve(includes.size());
        for (std::size_t edge = 0; edge < includes.size(); ++edge)
            output.baseline_includes.push_back(includes[edge]);

        const auto dependents =
            owner->baseline_sources->dependents(source);
        output.baseline_dependents.reserve(dependents.size());
        for (std::size_t edge = 0; edge < dependents.size(); ++edge)
            output.baseline_dependents.push_back(dependents[edge]);

        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

status normalize_source_path(
    const std::filesystem::path& input,
    std::string& output) noexcept {

    output.clear();
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

status source_manager::find_in_index(
    std::string_view normalized_path,
    std::span<const path_slot> index,
    source_id& output) const noexcept {

    output = {};
    if (normalized_path.empty() || index.empty())
        return {status_code::not_found};

    const auto hash = hash_path(normalized_path);
    const auto fingerprint = path_fingerprint(hash);
    const auto mask = index.size() - 1;
    auto position = static_cast<std::size_t>(hash) & mask;

    for (std::size_t probe = 0; probe < index.size(); ++probe) {
        const auto& slot = index[position];
        if (!slot.source)
            return {status_code::not_found};

        if (slot.fingerprint == fingerprint &&
            path(slot.source) == normalized_path) {
            output = slot.source;
            return {};
        }

        position = (position + 1) & mask;
    }

    return {status_code::not_found};
}

status source_manager::find(std::string_view normalized_path, source_id& output) const noexcept {
    output = {};
    if (baseline_sources != nullptr &&
        baseline_sources->find(normalized_path, output).ok()) {
        return {};
    }
    return find_in_index(normalized_path, path_index, output);
}

status source_manager::rebuild_path_index(
    std::size_t additional,
    std::vector<path_slot>& output) const noexcept {

    if (records.local_size() > (std::numeric_limits<std::size_t>::max)() - additional)
        return {status_code::not_available};
    const auto count = records.local_size() + additional;
    const auto required = count > ((std::numeric_limits<std::size_t>::max)() / 2)
        ? 0
        : count * 2 + 1;
    const auto capacity = next_capacity(required);
    if (capacity == 0)
        return {status_code::not_available};

    try {
        output.assign(capacity, path_slot{});
        const auto mask = capacity - 1;
        for (std::size_t index = 0; index < records.local_size(); ++index) {
            const auto absolute = baseline_source_count + index + 1;
            if (absolute > (std::numeric_limits<std::uint32_t>::max)())
                return {status_code::not_available};
            const auto source = source_id{static_cast<std::uint32_t>(absolute)};
            const auto normalized = path(source);
            const auto hash = hash_path(normalized);
            auto position = static_cast<std::size_t>(hash) & mask;
            while (output[position].source)
                position = (position + 1) & mask;
            output[position] = path_slot{path_fingerprint(hash), source};
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

source_manager_native_generation_view
source_manager::native_generation() const noexcept {

    source_manager_native_generation_view output;

    if (baseline_backed())
        return output;

    const std::span<const source_record> local_sources{
        records.local_values()};
    const auto local_physical =
        generation_storage_value.local_physical_records();
    const auto local_graph =
        generation_storage_value.local_records();

    output.sources = local_sources;
    output.physical = local_physical;
    output.graph = local_graph;
    output.forward_edges =
        generation_storage_value.forward_edge_arena();
    output.reverse_edges =
        generation_storage_value.reverse_edge_arena();

    output.path_index = {
        reinterpret_cast<const std::byte*>(path_index.data()),
        path_index.size() * sizeof(path_slot)};

    output.path_bytes = {
        reinterpret_cast<const std::byte*>(path_storage.data()),
        path_storage.size()};

    output.complete =
        local_sources.size() == source_count() &&
        local_physical.size() == source_count() &&
        local_graph.size() == source_count() &&
        (source_count() == 0 || !path_index.empty());

    return output;
}

bool source_manager::native_snapshot_text_complete() const noexcept {
    return
        !baseline_backed() &&
        snapshot_storage.complete_text_generation(
            source_count(),
            persistence_text_bytes_value);
}

std::size_t source_manager::
native_snapshot_text_page_count() const noexcept {

    return snapshot_storage.text_page_count();
}

std::span<const std::byte> source_manager::
native_snapshot_text_page(
    std::size_t index) const noexcept {

    return snapshot_storage.text_page(index);
}

status source_manager::release_snapshot_generation_storage(
    source_snapshot_generation_storage& output) noexcept {

    output = {};

    if (!native_snapshot_text_complete())
        return {status_code::invalid_state};

    return snapshot_storage.release_text_generation(
        output);
}


source_snapshot source_manager::current(source_id source) const noexcept {
    if (!source)
        return {};

    const auto index = static_cast<std::size_t>(source.value() - 1);
    if (index >= states.size())
        return {};

    if (const auto* state = states.materialized(index); state != nullptr)
        return state->snapshot;

    if (baseline_sources == nullptr || baseline_cache == nullptr ||
        index >= baseline_source_count) {
        return {};
    }

    source_manager_image_physical_state physical;
    if (!baseline_sources->physical(source, physical).ok() || !physical.present)
        return {};
    if (physical.size > (std::numeric_limits<std::uintmax_t>::max)())
        return {};

    const auto text = baseline_cache->source_text(source);
    if (text.size() != physical.size)
        return {};

    return source_snapshot{
        source,
        baseline_sources->path(source),
        text,
        file_snapshot_observation{
            physical.write_time_ticks,
            static_cast<std::uintmax_t>(physical.size)},
        physical.hash};
}

std::span<const source_id> source_manager::includes(
    source_id source) const noexcept {

    if (!source)
        return {};

    if (generation_storage_value.has_includes(source))
        return generation_storage_value.includes(source);

    const auto index =
        static_cast<std::size_t>(source.value() - 1);

    if (baseline_sources == nullptr ||
        index >= baseline_source_count ||
        index >= states.size()) {
        return {};
    }

    // Only baseline compatibility paths materialize this cache. Fresh G0 and
    // sparse replacements remain in generation_storage_value.
    return states[index].baseline_includes;
}

std::span<const source_id> source_manager::dependents(
    source_id source) const noexcept {

    if (!source)
        return {};

    if (generation_storage_value.has_dependents(source))
        return generation_storage_value.dependents(source);

    const auto index =
        static_cast<std::size_t>(source.value() - 1);

    if (baseline_sources == nullptr ||
        index >= baseline_source_count ||
        index >= states.size()) {
        return {};
    }

    return states[index].baseline_dependents;
}

std::size_t source_manager::include_count(
    source_id source) const noexcept {

    if (!source)
        return 0;

    if (generation_storage_value.has_includes(source))
        return generation_storage_value.includes(source).size();

    const auto index =
        static_cast<std::size_t>(source.value() - 1);

    return baseline_sources != nullptr &&
        index < baseline_source_count
        ? baseline_sources->includes(source).size()
        : 0;
}

source_id source_manager::include_at(
    source_id source,
    std::size_t edge) const noexcept {

    if (!source)
        return {};

    if (generation_storage_value.has_includes(source)) {
        const auto values =
            generation_storage_value.includes(source);
        return edge < values.size()
            ? values[edge]
            : source_id{};
    }

    const auto index =
        static_cast<std::size_t>(source.value() - 1);

    if (baseline_sources != nullptr &&
        index < baseline_source_count) {
        const auto values =
            baseline_sources->includes(source);
        return edge < values.size()
            ? values[edge]
            : source_id{};
    }

    return {};
}

std::size_t source_manager::dependent_count(
    source_id source) const noexcept {

    if (!source)
        return 0;

    if (generation_storage_value.has_dependents(source))
        return generation_storage_value.dependents(source).size();

    const auto index =
        static_cast<std::size_t>(source.value() - 1);

    return baseline_sources != nullptr &&
        index < baseline_source_count
        ? baseline_sources->dependents(source).size()
        : 0;
}

source_id source_manager::dependent_at(
    source_id source,
    std::size_t edge) const noexcept {

    if (!source)
        return {};

    if (generation_storage_value.has_dependents(source)) {
        const auto values =
            generation_storage_value.dependents(source);
        return edge < values.size()
            ? values[edge]
            : source_id{};
    }

    const auto index =
        static_cast<std::size_t>(source.value() - 1);

    if (baseline_sources != nullptr &&
        index < baseline_source_count) {
        const auto values =
            baseline_sources->dependents(source);
        return edge < values.size()
            ? values[edge]
            : source_id{};
    }

    return {};
}

status source_manager::collect_dependents(
    source_id source,
    std::vector<source_id>& output) const noexcept {

    output.clear();
    if (!source || static_cast<std::size_t>(source.value()) > states.size())
        return {status_code::invalid_argument};

    try {
        sparse_source_set visited;
        bool inserted = false;
        auto result = visited.insert(source, inserted);
        if (!result.ok())
            return result;

        std::vector<source_id> queue;
        for (const auto dependent : dependents(source)) {
            result = visited.insert(dependent, inserted);
            if (!result.ok())
                return result;
            if (inserted)
                queue.push_back(dependent);
        }

        for (std::size_t cursor = 0; cursor < queue.size(); ++cursor) {
            const auto current_source = queue[cursor];
            output.push_back(current_source);
            for (const auto dependent : dependents(current_source)) {
                result = visited.insert(dependent, inserted);
                if (!result.ok())
                    return result;
                if (inserted)
                    queue.push_back(dependent);
            }
        }
        return {};
    }
    catch (const std::bad_alloc&) {
        output.clear();
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        output.clear();
        return {status_code::not_available};
    }
}

std::string_view source_manager::path(source_id source) const noexcept {
    if (!source)
        return {};

    const auto index = static_cast<std::size_t>(source.value() - 1);
    if (index >= records.size())
        return {};

    if (baseline_sources != nullptr && index < baseline_source_count)
        return baseline_sources->path(source);

    const auto& record = records[index];
    const auto offset = static_cast<std::size_t>(record.path_offset);
    const auto length = static_cast<std::size_t>(record.path_length);
    if (offset > path_storage.size() || length > path_storage.size() - offset)
        return {};

    return {path_storage.data() + offset, length};
}

status source_manager::publish_memory(
    std::string_view normalized_path,
    std::string_view text,
    source_snapshot& output,
    source_id* identity) noexcept {

    try {
        auto update = begin_update();
        source_id source;
        auto result = update.resolve(
            std::filesystem::path{normalized_path},
            source);

        if (!result.ok())
            return result;

        source_acquire_result acquired;
        acquired.source = source;
        acquired.kind =
            source_acquire_result_kind::present;
        acquired.snapshot.observation.size =
            text.size();
        acquired.snapshot.hash =
            hash_source_content(text);
        acquired.snapshot.bytes.assign(
            text.data(),
            text.size());

        result = update.apply_acquire(
            std::move(acquired));
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
            replacement[position] = local_path_slot{path_fingerprint(hash), index + 1};
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
        if (slot.fingerprint == path_fingerprint(hash)) {
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
    local_path_index[position] = local_path_slot{path_fingerprint(hash), new_source_index};
    return {};
}

status source_manager_update::resolve(
    const std::filesystem::path& path_value,
    source_id& output) noexcept {

    std::string normalized;
    const auto result = normalize_source_path(path_value, normalized);
    if (!result.ok()) {
        output = {};
        return result;
    }

    return resolve_normalized(normalized, output);
}

status source_manager_update::resolve_canonical(
    const std::filesystem::path& path_value,
    source_id& output) noexcept {

    output = {};
    if (owner == nullptr || committed ||
        path_value.empty() || !path_value.is_absolute()) {
        return {status_code::invalid_argument};
    }

    try {
        auto normalized = path_value.generic_string();
#ifdef _WIN32
        if (normalized.size() >= 2 &&
            normalized[1] == ':' &&
            normalized[0] >= 'A' &&
            normalized[0] <= 'Z') {
            normalized[0] =
                static_cast<char>(
                    normalized[0] - 'A' + 'a');
        }
#endif
        if (normalized.empty())
            return {status_code::invalid_argument};

        return resolve_normalized(
            normalized,
            output);
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

status source_manager_update::reserve_sources(
    std::size_t additional) noexcept {

    if (owner == nullptr || committed)
        return {status_code::invalid_argument};

    if (additional == 0)
        return {};

    if (new_sources.size() >
        (std::numeric_limits<std::size_t>::max)() -
            additional) {
        return {status_code::not_available};
    }

    const auto required =
        new_sources.size() + additional;

    if (owner->records.size() >
        static_cast<std::size_t>(
            (std::numeric_limits<std::uint32_t>::max)()) -
            required) {
        return {status_code::not_available};
    }

    if (candidates.size() >
            (std::numeric_limits<std::size_t>::max)() -
                additional ||
        semantic_changes.size() >
            (std::numeric_limits<std::size_t>::max)() -
                additional ||
        physical_changes_value.size() >
            (std::numeric_limits<std::size_t>::max)() -
                additional) {
        return {status_code::not_available};
    }

    const auto candidate_required =
        candidates.size() + additional;
    const auto semantic_required =
        semantic_changes.size() + additional;
    const auto physical_required =
        physical_changes_value.size() + additional;

    try {
        new_sources.reserve(required);
        candidates.reserve(candidate_required);
        semantic_changes.reserve(semantic_required);
        physical_changes_value.reserve(physical_required);
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }

    auto result =
        ensure_local_path_capacity(required);
    if (!result.ok())
        return result;

    return ensure_id_index_capacity(
        candidate_required);
}

status source_manager_update::resolve_normalized(
    std::string_view normalized,
    source_id& output) noexcept {

    output = {};
    if (owner == nullptr || committed || normalized.empty())
        return {status_code::invalid_argument};

    if (owner->find(normalized, output).ok())
        return {};
    if (find_local_path(normalized, output).ok())
        return {};

    if (owner->records.size() + new_sources.size() >=
        static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        return {status_code::not_available};
    }

    try {
        auto result = ensure_local_path_capacity(new_sources.size() + 1);
        if (!result.ok())
            return result;

        const auto value = static_cast<std::uint32_t>(
            owner->records.size() + new_sources.size() + 1);

        new_sources.push_back(new_source{source_id{value}, std::string{normalized}});
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
        return owner->path(source);
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
    const auto result =
        acquire_file_snapshot(
            job.path,
            job.baseline,
            snapshot_value);
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

    const auto previous = snapshot(result.source);
    const bool is_new_source = owner != nullptr &&
        static_cast<std::size_t>(result.source.value()) > owner->records.size();

    candidate_source* item = nullptr;
    auto touch_result = touch(result.source, item);
    if (!touch_result.ok())
        return touch_result;

    const bool first_physical_change =
        !item->has_snapshot;

    if (result.kind == source_acquire_result_kind::missing) {
        if (is_new_source || !previous)
            return {status_code::not_found};

        try {
            item->snapshot = {};
            item->has_snapshot = true;
            semantic_changes.push_back(result.source);
            if (first_physical_change)
                physical_changes_value.push_back(result.source);
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

    try {
        const auto normalized =
            path(result.source);

        if (normalized.empty())
            return {status_code::invalid_argument};

        std::string_view snapshot_path;
        std::string_view snapshot_text;

        auto storage_result =
            candidate_snapshots.append(
                result.source,
                normalized,
                result.snapshot.bytes,
                snapshot_path,
                snapshot_text);

        if (!storage_result.ok())
            return storage_result;

        const bool semantic_change =
            !previous ||
            previous.hash() !=
                result.snapshot.hash;

        item->snapshot = source_snapshot{
            result.source,
            snapshot_path,
            snapshot_text,
            result.snapshot.observation,
            result.snapshot.hash,
            result.snapshot.identity};

        item->has_snapshot = true;

        if (semantic_change)
            semantic_changes.push_back(result.source);
        if (first_physical_change)
            physical_changes_value.push_back(result.source);
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

std::span<const source_id> source_manager_update::dependents(source_id source) const noexcept {
    if (const auto* item = candidate(source); item != nullptr && item->has_dependents)
        return item->dependents;
    return owner != nullptr ? owner->dependents(source) : std::span<const source_id>{};
}

status source_manager_update::collect_dependents(
    source_id source,
    std::vector<source_id>& output) const noexcept {

    if (!valid_source(source) || owner == nullptr)
        return {status_code::invalid_argument};
    if (static_cast<std::size_t>(source.value()) > owner->source_count()) {
        output.clear();
        return {};
    }
    return owner->collect_dependents(source, output);
}

std::size_t source_manager_update::source_count() const noexcept {
    return owner != nullptr ? owner->records.size() + new_sources.size() : 0;
}

status source_manager_update::validate_source_graph(
    operation_id operation,
    diagnostic_buffer& diagnostics) const noexcept {

    ++telemetry_value.source_graph_full_scans;
    const auto count = source_count();
    telemetry_value.source_graph_visited += count;
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

status source_manager_update::validate_changed_source_graph(
    std::span<const source_id> changed,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    std::size_t* visited_sources) const noexcept {

    if (visited_sources != nullptr)
        *visited_sources = 0;

    std::size_t local_visited = 0;
    try {
        for (const auto root : changed) {
            if (!valid_source(root))
                return {status_code::invalid_argument};

            sparse_source_set visited;
            bool inserted = false;
            auto result = visited.insert(root, inserted);
            if (!result.ok())
                return result;

            std::vector<source_id> stack;
            for (const auto dependency : includes(root))
                stack.push_back(dependency);

            while (!stack.empty()) {
                const auto current_source = stack.back();
                stack.pop_back();
                if (current_source == root) {
                    emit_source_failure(
                        diagnostics::source_dependency_cycle, root, operation,
                        "Source include dependency graph contains a cycle", diagnostics);
                    return {status_code::semantic_conflict};
                }

                result = visited.insert(current_source, inserted);
                if (!result.ok())
                    return result;
                if (!inserted)
                    continue;
                ++local_visited;
                if (visited_sources != nullptr)
                    ++*visited_sources;
                for (const auto dependency : includes(current_source))
                    stack.push_back(dependency);
            }
        }
        telemetry_value.source_graph_visited += local_visited;
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

status source_manager_update::prepare_dependent_patches() noexcept {
    struct edge_patch final {
        source_id target{};
        source_id dependent{};
        bool add = false;
    };

    try {
        std::vector<edge_patch> patches;
        const auto initial_candidate_count = candidates.size();
        for (std::size_t index = 0; index < initial_candidate_count; ++index) {
            const auto& item = candidates[index];
            if (!item.has_includes)
                continue;

            const auto old_includes = owner != nullptr &&
                static_cast<std::size_t>(item.source.value()) <= owner->states.size()
                    ? owner->includes(item.source)
                    : std::span<const source_id>{};
            const std::span<const source_id> new_includes{item.includes};

            sparse_source_set old_set;
            sparse_source_set new_set;
            bool inserted = false;
            for (const auto dependency : old_includes) {
                auto result = old_set.insert(dependency, inserted);
                if (!result.ok())
                    return result;
            }
            for (const auto dependency : new_includes) {
                auto result = new_set.insert(dependency, inserted);
                if (!result.ok())
                    return result;
            }
            for (const auto dependency : old_includes) {
                if (!new_set.contains(dependency))
                    patches.push_back(edge_patch{dependency, item.source, false});
            }
            for (const auto dependency : new_includes) {
                if (!old_set.contains(dependency))
                    patches.push_back(edge_patch{dependency, item.source, true});
            }
        }

        telemetry_value.reverse_edge_patches += patches.size();
        for (const auto& patch : patches) {
            candidate_source* target = nullptr;
            auto result = touch(patch.target, target);
            if (!result.ok())
                return result;
            if (!target->has_dependents) {
                const auto committed_dependents = owner != nullptr
                    ? owner->dependents(patch.target)
                    : std::span<const source_id>{};
                target->dependents.assign(
                    committed_dependents.begin(), committed_dependents.end());
                target->has_dependents = true;
            }

            const auto found = std::find(
                target->dependents.begin(), target->dependents.end(), patch.dependent);
            if (patch.add) {
                if (found == target->dependents.end())
                    target->dependents.push_back(patch.dependent);
            } else if (found != target->dependents.end()) {
                *found = target->dependents.back();
                target->dependents.pop_back();
            }
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
    ++telemetry_value.path_index_full_rebuilds;
    auto result = owner->rebuild_path_index(new_sources.size(), prepared_path_index);
    if (!result.ok())
        return result;
    const auto mask = prepared_path_index.size() - 1;
    for (const auto& item : new_sources) {
        const auto hash = hash_path(item.normalized_path);
        auto position = static_cast<std::size_t>(hash) & mask;
        while (prepared_path_index[position].source)
            position = (position + 1) & mask;
        prepared_path_index[position] = source_manager::path_slot{
            path_fingerprint(hash),
            item.source,
        };
    }
    return {};
}

status source_manager_update::build_sparse_path_insertions() noexcept {
    prepared_path_insertions.clear();
    if (owner == nullptr || new_sources.empty())
        return {};
    if (owner->path_index.empty())
        return build_prepared_path_index();

    const auto required_count = owner->records.local_size() + new_sources.size();
    if (required_count > owner->path_index.size() / 2)
        return {status_code::rebuild_required};

    try {
        prepared_path_insertions.reserve(new_sources.size());
        sparse_position_set reserved_positions;
        auto result = reserved_positions.reserve(new_sources.size());
        if (!result.ok())
            return result;

        const auto mask = owner->path_index.size() - 1;
        for (const auto& item : new_sources) {
            const auto hash = hash_path(item.normalized_path);
            auto position = static_cast<std::size_t>(hash) & mask;
            while (owner->path_index[position].source || reserved_positions.contains(position))
                position = (position + 1) & mask;
            reserved_positions.insert(position);
            prepared_path_insertions.push_back(prepared_path_insertion{
                position,
                source_manager::path_slot{path_fingerprint(hash), item.source},
            });
        }
        return {};
    }
    catch (const std::bad_alloc&) {
        prepared_path_insertions.clear();
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        prepared_path_insertions.clear();
        return {status_code::not_available};
    }
}

status source_manager_update::prepare_publish() noexcept {
    if (owner == nullptr || committed)
        return {status_code::invalid_argument};
    if (prepared)
        return {};

    auto result = prepare_dependent_patches();
    if (!result.ok())
        return result;

    std::size_t added_path_bytes = 0;
    if (!new_sources.empty()) {
        result = owner->baseline_backed()
            ? build_prepared_path_index()
            : (owner->records.empty()
                ? build_prepared_path_index()
                : build_sparse_path_insertions());
        if (!result.ok())
            return result;

        for (const auto& item : new_sources) {
            if (added_path_bytes >
                (std::numeric_limits<std::size_t>::max)() - item.normalized_path.size()) {
                return {status_code::not_available};
            }
            added_path_bytes += item.normalized_path.size();
        }

        if (owner->path_storage.size() >
            static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)()) - added_path_bytes) {
            return {status_code::not_available};
        }
    }

    if (!new_sources.empty()) {
        prepared_path_storage_size = owner->path_storage.size() + added_path_bytes;

        if (owner->baseline_backed()) {
            // BUILD runs while Project is CONSTRUCTING with no admitted readers.
            // Reserve only post-baseline append storage; mmap baseline slots remain
            // untouched and do not participate in vector relocation.
            try {
                owner->records.reserve(owner->records.size() + new_sources.size());
                owner->states.reserve(owner->states.size() + new_sources.size());
                owner->path_storage.reserve(prepared_path_storage_size);

                // Publication must be allocation-free even for metadata-only
                // Source updates whose semantic closure is empty.
                for (const auto& item : candidates) {
                    const auto index = static_cast<std::size_t>(item.source.value() - 1);
                    if (index < owner->baseline_source_count)
                        (void)owner->states[index];
                }
                if (!owner->states.read_status().ok())
                    return owner->states.read_status();
            }
            catch (const std::bad_alloc&) {
                return {status_code::not_available};
            }
            catch (const std::length_error&) {
                return {status_code::not_available};
            }
        } else if (owner->records.empty()) {
            // Full construction happens in a detached Project and therefore has no
            // externally readable committed storage to invalidate.
            try {
                std::size_t source_capacity = 0;
                std::size_t path_capacity = 0;
                if (!incremental_headroom(new_sources.size(), 64, source_capacity) ||
                    !incremental_headroom(prepared_path_storage_size, 4096, path_capacity)) {
                    return {status_code::not_available};
                }
                owner->records.reserve(source_capacity);
                owner->states.reserve(source_capacity);
                owner->path_storage.reserve(path_capacity);
            }
            catch (const std::bad_alloc&) {
                return {status_code::not_available};
            }
            catch (const std::length_error&) {
                return {status_code::not_available};
            }
        } else if (owner->records.size() + new_sources.size() > owner->records.capacity() ||
                   owner->states.size() + new_sources.size() > owner->states.capacity() ||
                   prepared_path_storage_size > owner->path_storage.capacity()) {
            // Legacy in-memory incremental mode retains its no-relocation contract.
            prepared_path_index.clear();
            prepared_path_insertions.clear();
            prepared_path_storage_size = 0;
            return {status_code::rebuild_required};
        }
    }

    std::size_t additional_forward_edges = 0;
    std::size_t additional_reverse_edges = 0;

    for (const auto& item : candidates) {
        if (item.has_includes) {
            if (additional_forward_edges >
                (std::numeric_limits<std::size_t>::max)() -
                    item.includes.size()) {
                return {status_code::not_available};
            }
            additional_forward_edges += item.includes.size();
        }

        if (item.has_dependents) {
            if (additional_reverse_edges >
                (std::numeric_limits<std::size_t>::max)() -
                    item.dependents.size()) {
                return {status_code::not_available};
            }
            additional_reverse_edges += item.dependents.size();
        }
    }

    prepared_text_bytes =
        owner->persistence_text_bytes_value;

    for (const auto& item : candidates) {
        if (!item.has_snapshot)
            continue;

        std::uint64_t previous_bytes = 0;
        if (static_cast<std::size_t>(item.source.value()) <=
            owner->records.size()) {

            const auto previous =
                owner->current(item.source);
            if (previous) {
                const auto size =
                    previous.observation().size;
                if (size >
                    (std::numeric_limits<std::uint64_t>::max)()) {
                    return {status_code::not_available};
                }
                previous_bytes =
                    static_cast<std::uint64_t>(size);
            }
        }

        if (previous_bytes > prepared_text_bytes)
            return {status_code::initialization_failed};

        prepared_text_bytes -= previous_bytes;

        if (item.snapshot) {
            const auto size =
                item.snapshot.observation().size;
            if (size >
                (std::numeric_limits<std::uint64_t>::max)()) {
                return {status_code::not_available};
            }

            const auto current_bytes =
                static_cast<std::uint64_t>(size);

            if (prepared_text_bytes >
                (std::numeric_limits<std::uint64_t>::max)() -
                    current_bytes) {
                return {status_code::not_available};
            }

            prepared_text_bytes += current_bytes;
        }
    }

    result = owner->generation_storage_value.prepare_publish(
        new_sources.size(),
        additional_forward_edges,
        additional_reverse_edges);
    if (!result.ok())
        return result;

    for (const auto& item : candidates) {
        if (!item.has_snapshot &&
            !item.has_includes &&
            !item.has_dependents) {
            continue;
        }

        if (item.has_snapshot &&
            item.snapshot &&
            item.snapshot.observation().size >
                (std::numeric_limits<std::uint64_t>::max)()) {
            return {status_code::not_available};
        }

        if (static_cast<std::size_t>(item.source.value()) <=
            owner->generation_storage_value.source_count()) {

            result =
                owner->generation_storage_value.prepare_source(
                    item.source);
            if (!result.ok())
                return result;
        }
    }

    result =
        owner->snapshot_storage.prepare_absorb(
            candidate_snapshots);

    if (!result.ok())
        return result;

    prepared = true;
    return {};
}

void source_manager_update::publish_prepared() noexcept {
    if (!prepared || committed || owner == nullptr)
        return;

    owner->snapshot_storage.absorb_prepared(
        std::move(candidate_snapshots));

    for (const auto& item : new_sources) {
        const auto offset = static_cast<std::uint32_t>(owner->path_storage.size());
        const auto length = static_cast<std::uint32_t>(item.normalized_path.size());
        owner->path_storage.insert(
            owner->path_storage.end(),
            item.normalized_path.begin(),
            item.normalized_path.end());
        owner->records.push_back(source_record{offset, length});
        owner->states.emplace_back();
        owner->generation_storage_value.publish_source();
    }

    for (auto& item : candidates) {
        const auto index = static_cast<std::size_t>(item.source.value() - 1);
        if (index >= owner->states.size())
            continue;
        if (item.has_snapshot) {
            owner->states[index].snapshot = std::move(item.snapshot);
            owner->generation_storage_value.publish_snapshot(
                item.source,
                owner->states[index].snapshot);
        }
        if (item.has_includes) {
            owner->generation_storage_value.publish_includes(
                item.source,
                item.includes);
        }
        if (item.has_dependents) {
            owner->generation_storage_value.publish_dependents(
                item.source,
                item.dependents);
        }
    }

    if (!prepared_path_index.empty()) {
        owner->path_index.swap(prepared_path_index);
    } else {
        for (const auto& insertion : prepared_path_insertions)
            owner->path_index[insertion.position] = insertion.slot;
    }

    owner->persistence_text_bytes_value =
        prepared_text_bytes;

    telemetry_value.snapshot_page_backed = true;
    telemetry_value.snapshot_pages =
        owner->snapshot_storage.page_count();
    telemetry_value.snapshot_reserved_bytes =
        owner->snapshot_storage.reserved_bytes();

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
