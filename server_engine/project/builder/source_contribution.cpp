#include "source_contribution.hpp"

#include "../frontend/source_facts_validation.hpp"
#include "../persistence/build_cache_image.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"
#include "../../diagnostics/diagnostic_descriptor.hpp"

#include <bit>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace cw::server {
namespace {

template<class T>
[[nodiscard]] std::span<const T> checked_span(
    const mapped_vector<T>& values,
    source_fact_range range) {

    const auto begin = static_cast<std::size_t>(range.begin);
    const auto count = static_cast<std::size_t>(range.count);
    if (begin > values.size() || count > values.size() - begin || count == 0)
        return {};
    return values.span(begin, count);
}

template<class T>
[[nodiscard]] std::span<const T> combined_span(
    const mapped_vector<T>& committed,
    const mapped_vector<T>& appended,
    std::size_t base,
    source_fact_range range) {

    const auto begin = static_cast<std::size_t>(range.begin);
    const auto count = static_cast<std::size_t>(range.count);
    if (count == 0)
        return {};
    if (begin < base) {
        if (begin > committed.size() || count > committed.size() - begin || begin + count > base)
            return {};
        return committed.span(begin, count);
    }
    const auto local = begin - base;
    if (local > appended.size() || count > appended.size() - local)
        return {};
    return appended.span(local, count);
}

[[nodiscard]] bool add_u32(
    std::size_t base,
    std::uint32_t relative,
    std::uint32_t& output) noexcept {

    const auto value = base + static_cast<std::size_t>(relative);
    if (value > (std::numeric_limits<std::uint32_t>::max)())
        return false;
    output = static_cast<std::uint32_t>(value);
    return true;
}

[[nodiscard]] bool absolute_u32(
    std::size_t base,
    std::size_t local,
    std::uint32_t& output) noexcept {

    if (local > (std::numeric_limits<std::size_t>::max)() - base)
        return false;
    const auto value = base + local;
    if (value > (std::numeric_limits<std::uint32_t>::max)())
        return false;
    output = static_cast<std::uint32_t>(value);
    return true;
}

[[nodiscard]] std::size_t patch_capacity(std::size_t count) noexcept {
    if (count == 0)
        return 0;
    if (count > (std::numeric_limits<std::size_t>::max)() / 2)
        return 0;
    const auto minimum = count * 2;
    return std::bit_ceil(minimum < 8 ? std::size_t{8} : minimum);
}

[[nodiscard]] std::size_t patch_hash(std::uint32_t value) noexcept {
    auto mixed = static_cast<std::uint64_t>(value) * 0x9e3779b97f4a7c15ULL;
    mixed ^= mixed >> 29;
    mixed *= 0xbf58476d1ce4e5b9ULL;
    return static_cast<std::size_t>(mixed ^ (mixed >> 32));
}

template<class T>
void append_prepared(mapped_vector<T>& target, const mapped_vector<T>& source) noexcept {
    for (const auto& value : source.local_values())
        target.push_back(value);
}

[[nodiscard]] bool subtract_count(std::size_t& value, std::size_t decrement) noexcept {
    if (decrement > value)
        return false;
    value -= decrement;
    return true;
}

[[nodiscard]] bool reserve_headroom_size(std::size_t size, std::size_t& output) noexcept {
    const auto extra = size / 16 + 64;
    if (size > (std::numeric_limits<std::size_t>::max)() - extra)
        return false;
    output = size + extra;
    return true;
}

template<class T>
[[nodiscard]] status reserve_with_headroom(std::vector<T>& values, std::size_t size) noexcept {
    std::size_t capacity = 0;
    if (!reserve_headroom_size(size, capacity))
        return {status_code::not_available};
    try {
        values.reserve(capacity);
        return {};
    } catch (const std::bad_alloc&) {
        return {status_code::initialization_failed};
    } catch (const std::length_error&) {
        return {status_code::not_available};
    } catch (...) {
        return {status_code::initialization_failed};
    }
}


template<class T>
[[nodiscard]] status reserve_with_headroom(mapped_vector<T>& values, std::size_t size) noexcept {
    std::size_t capacity = 0;
    if (!reserve_headroom_size(size, capacity))
        return {status_code::not_available};
    try {
        values.reserve(capacity);
        return {};
    } catch (const std::bad_alloc&) {
        return {status_code::initialization_failed};
    } catch (const std::length_error&) {
        return {status_code::not_available};
    } catch (...) {
        return {status_code::initialization_failed};
    }
}

} // namespace

source_contribution_cache::source_contribution_cache(
    const build_cache_image_view& baseline_cache_value) noexcept
    : baseline_cache(&baseline_cache_value),
      statistics_value(baseline_cache_value.contribution_statistics()),
      provenance_complete(baseline_cache_value.contributions_complete()) {

    const auto source_slots = baseline_cache_value.source_count() + 1;
    committed.sources.bind_baseline(
        &baseline_cache_value,
        source_slots,
        [](const void* context, std::size_t index, source_contribution_state& output) noexcept {
            output = {};
            if (index == 0)
                return status{};
            if (index > (std::numeric_limits<std::uint32_t>::max)())
                return status{status_code::artifact_corrupt};
            return static_cast<const build_cache_image_view*>(context)->contribution_state(
                source_id{static_cast<std::uint32_t>(index)}, output);
        });

    committed.types.bind_baseline(
        &baseline_cache_value,
        baseline_cache_value.contribution_type_count(),
        [](const void* context, std::size_t index, source_contribution_type& output) noexcept {
            return static_cast<const build_cache_image_view*>(context)->contribution_type(index, output);
        });
    committed.members.bind_baseline(
        &baseline_cache_value,
        baseline_cache_value.contribution_member_count(),
        [](const void* context, std::size_t index, source_contribution_member& output) noexcept {
            return static_cast<const build_cache_image_view*>(context)->contribution_member(index, output);
        });
    committed.modifiers.bind_baseline(
        &baseline_cache_value,
        baseline_cache_value.contribution_modifier_count(),
        [](const void* context, std::size_t index, source_type_modifier& output) noexcept {
            return static_cast<const build_cache_image_view*>(context)->contribution_modifier(index, output);
        });
    committed.enum_values.bind_baseline(
        &baseline_cache_value,
        baseline_cache_value.contribution_enum_value_count(),
        [](const void* context, std::size_t index, source_contribution_enum_value& output) noexcept {
            return static_cast<const build_cache_image_view*>(context)->contribution_enum_value(index, output);
        });
    committed.objects.bind_baseline(
        &baseline_cache_value,
        baseline_cache_value.contribution_object_count(),
        [](const void* context, std::size_t index, source_contribution_object& output) noexcept {
            return static_cast<const build_cache_image_view*>(context)->contribution_object(index, output);
        });
    committed.links.bind_baseline(
        &baseline_cache_value,
        baseline_cache_value.contribution_link_count(),
        [](const void* context, std::size_t index, source_contribution_link& output) noexcept {
            return static_cast<const build_cache_image_view*>(context)->contribution_link(index, output);
        });
    committed.construction.bind_baseline(
        &baseline_cache_value,
        baseline_cache_value.construction_slot_count() + 1,
        [](const void* context, std::size_t index, source_construction_state& output) noexcept {
            return static_cast<const build_cache_image_view*>(context)->construction_at_slot(
                index,
                output);
        });
}

void source_contribution_cache::storage::swap(storage& other) noexcept {
    sources.swap(other.sources);
    types.swap(other.types);
    members.swap(other.members);
    modifiers.swap(other.modifiers);
    enum_values.swap(other.enum_values);
    objects.swap(other.objects);
    links.swap(other.links);
    construction.swap(other.construction);
    std::swap(statistics, other.statistics);
}

source_contribution_storage_usage source_contribution_cache::storage_usage() const noexcept {
    source_contribution_storage_usage output;

    const auto add_vector = [&](const auto& values) noexcept {
        using value_type = typename std::remove_reference_t<decltype(values)>::value_type;
        const auto heap_records = values.heap_record_capacity();
        output.retained_bytes += heap_records * sizeof(value_type);
        if (values.local_capacity() > values.local_size())
            output.reserve_bytes += (values.local_capacity() - values.local_size()) * sizeof(value_type);
    };

    add_vector(committed.sources);
    add_vector(committed.types);
    add_vector(committed.members);
    add_vector(committed.modifiers);
    add_vector(committed.enum_values);
    add_vector(committed.objects);
    add_vector(committed.links);
    add_vector(committed.construction);

    const auto stale_count = [](std::size_t physical, std::size_t live) noexcept {
        return physical > live ? physical - live : std::size_t{0};
    };

    const auto source_slots = committed.sources.empty() ? 0 : committed.sources.size() - 1;
    output.construction_slots = committed.construction.empty() ? 0 : committed.construction.size() - 1;
    output.stale_bytes += stale_count(source_slots, statistics_value.sources) * sizeof(source_contribution_state);
    output.stale_bytes += stale_count(committed.types.size(), statistics_value.type_declarations) * sizeof(source_contribution_type);
    output.stale_bytes += stale_count(committed.members.size(), statistics_value.members) * sizeof(source_contribution_member);
    output.stale_bytes += stale_count(committed.modifiers.size(), statistics_value.modifiers) * sizeof(source_type_modifier);
    output.stale_bytes += stale_count(committed.enum_values.size(), statistics_value.enum_values) * sizeof(source_contribution_enum_value);
    output.stale_bytes += stale_count(committed.objects.size(), statistics_value.objects) * sizeof(source_contribution_object);
    output.stale_bytes += stale_count(committed.links.size(), statistics_value.links) * sizeof(source_contribution_link);

    // Construction slots track historical type handles. Their stale portion is
    // accounted by Project-level pressure where current Graph live type count is known.
    return output;
}

status source_contribution_cache::release_native_generation_storage(
    source_contribution_generation_storage& output) noexcept {

    output = {};

    if (baseline_cache != nullptr ||
        !provenance_complete) {
        return {status_code::invalid_state};
    }

    const auto native = native_generation();
    if (!native.complete ||
        native.sources.empty() ||
        native.construction.empty()) {
        return {status_code::initialization_failed};
    }

    output.sources = committed.sources.release_local_values();
    output.types = committed.types.release_local_values();
    output.members = committed.members.release_local_values();
    output.modifiers = committed.modifiers.release_local_values();
    output.enum_values = committed.enum_values.release_local_values();
    output.objects = committed.objects.release_local_values();
    output.links = committed.links.release_local_values();
    output.construction =
        committed.construction.release_local_values();
    output.complete = true;

    if (!output.valid())
        return {status_code::initialization_failed};

    statistics_value = {};
    provenance_complete = false;
    return {};
}

source_contribution_cache_update source_contribution_cache::begin_rebuild() noexcept {
    return source_contribution_cache_update{*this};
}

source_contribution_sparse_update source_contribution_cache::begin_incremental() noexcept {
    return source_contribution_sparse_update{*this};
}

const source_contribution_state* source_contribution_cache::state(source_id source) const noexcept {
    if (!source || source.value() >= committed.sources.size())
        return nullptr;
    const auto& value = committed.sources[source.value()];
    return value.source == source ? &value : nullptr;
}

std::span<const source_contribution_type> source_contribution_cache::types(source_id source) const noexcept {
    const auto* value = state(source);
    return value == nullptr ? std::span<const source_contribution_type>{} :
        checked_span(committed.types, value->types);
}

std::span<const source_contribution_member> source_contribution_cache::members(source_fact_range range) const noexcept {
    return checked_span(committed.members, range);
}

std::span<const source_type_modifier> source_contribution_cache::modifiers(source_fact_range range) const noexcept {
    return checked_span(committed.modifiers, range);
}

std::span<const source_contribution_enum_value> source_contribution_cache::enum_values(source_fact_range range) const noexcept {
    return checked_span(committed.enum_values, range);
}

std::span<const source_contribution_object> source_contribution_cache::objects(source_id source) const noexcept {
    const auto* value = state(source);
    return value == nullptr ? std::span<const source_contribution_object>{} :
        checked_span(committed.objects, value->objects);
}

std::span<const source_contribution_link> source_contribution_cache::links(source_id source) const noexcept {
    const auto* value = state(source);
    return value == nullptr ? std::span<const source_contribution_link>{} :
        checked_span(committed.links, value->links);
}

bool source_contribution_cache::equivalent(const source_facts& facts) const noexcept {
    const auto* source_state = state(facts.source());
    if (source_state == nullptr)
        return false;

    const auto cached_types = types(facts.source());
    const auto cached_members = members(source_state->members);
    const auto cached_modifiers = modifiers(source_state->modifiers);
    const auto cached_enum_values = enum_values(source_state->enum_values);
    const auto cached_objects = objects(facts.source());
    const auto cached_links = links(facts.source());

    if (cached_members.size() != facts.members().size() ||
        cached_modifiers.size() != facts.modifiers().size() ||
        cached_enum_values.size() != facts.enum_values().size() ||
        cached_objects.size() != facts.objects().size() ||
        cached_links.size() != facts.links().size()) {
        return false;
    }

    for (std::size_t index = 0; index < cached_modifiers.size(); ++index) {
        if (cached_modifiers[index].kind != facts.modifiers()[index].kind ||
            cached_modifiers[index].value != facts.modifiers()[index].value) {
            return false;
        }
    }

    for (std::size_t index = 0; index < cached_members.size(); ++index) {
        const auto& cached = cached_members[index];
        const auto& current = facts.members()[index];
        if (cached.type.identity != current.type.identity ||
            cached.type.intrinsic != current.type.intrinsic ||
            cached.type.modifiers.count != current.type.modifiers.count ||
            cached.access != current.access ||
            cached.name != current.name) {
            return false;
        }
        if (cached.type.modifiers.begin < source_state->modifiers.begin ||
            cached.type.modifiers.begin - source_state->modifiers.begin != current.type.modifiers.begin) {
            return false;
        }
    }

    for (std::size_t index = 0; index < cached_enum_values.size(); ++index) {
        const auto& cached = cached_enum_values[index];
        const auto& current = facts.enum_values()[index];
        if (cached.value.intrinsic != current.value.intrinsic ||
            cached.value.bits != current.value.bits ||
            cached.name != current.name) {
            return false;
        }
    }

    for (std::size_t index = 0; index < cached_objects.size(); ++index) {
        const auto& cached = cached_objects[index];
        const auto& current = facts.objects()[index];
        if (cached.identity != current.identity ||
            cached.type.identity != current.type.identity ||
            cached.type.intrinsic != current.type.intrinsic ||
            cached.type.modifiers.count != current.type.modifiers.count ||
            cached.type.modifiers.begin < source_state->modifiers.begin ||
            cached.type.modifiers.begin - source_state->modifiers.begin != current.type.modifiers.begin) {
            return false;
        }
    }

    for (std::size_t index = 0; index < cached_links.size(); ++index) {
        const auto& cached = cached_links[index];
        const auto& current = facts.links()[index];
        if (cached.source.object != current.source.object ||
            cached.source.member != current.source.member ||
            cached.target.object != current.target.object ||
            cached.target.member != current.target.member) {
            return false;
        }
    }

    std::size_t type_position = 0;
    const auto compare_record = [&](const source_record_fact& current) noexcept {
        if (type_position >= cached_types.size())
            return false;
        const auto& cached = cached_types[type_position++];
        if (cached.identity != current.identity ||
            cached.kind != source_contribution_type_kind::record ||
            cached.record_kind != current.record_kind ||
            cached.definition() !=
                (current.declaration_kind == source_record_declaration_kind::definition) ||
            cached.definition_items.count != current.members.count ||
            cached.definition_items.begin < source_state->members.begin ||
            cached.definition_items.begin - source_state->members.begin != current.members.begin) {
            return false;
        }
        return true;
    };

    const auto compare_enum = [&](const source_enum_fact& current) noexcept {
        if (type_position >= cached_types.size())
            return false;
        const auto& cached = cached_types[type_position++];
        if (cached.identity != current.identity ||
            cached.kind != source_contribution_type_kind::enumeration ||
            cached.explicit_underlying != current.explicit_underlying ||
            cached.enum_scoped() != current.scoped ||
            cached.definition() !=
                (current.declaration_kind == source_enum_declaration_kind::definition) ||
            cached.definition_items.count != current.enumerators.count ||
            cached.definition_items.begin < source_state->enum_values.begin ||
            cached.definition_items.begin - source_state->enum_values.begin != current.enumerators.begin) {
            return false;
        }
        return true;
    };

    if (!facts.declarations().empty()) {
        for (const auto& declaration : facts.declarations()) {
            switch (declaration.kind) {
            case source_declaration_kind::namespace_scope:
                continue;
            case source_declaration_kind::record_type:
                if (declaration.index >= facts.records().size() ||
                    !compare_record(facts.records()[declaration.index]))
                    return false;
                break;
            case source_declaration_kind::enum_type:
                if (declaration.index >= facts.enums().size() ||
                    !compare_enum(facts.enums()[declaration.index]))
                    return false;
                break;
            case source_declaration_kind::object:
                if (declaration.index >= facts.objects().size())
                    return false;
                break;
            case source_declaration_kind::link:
                if (declaration.index >= facts.links().size())
                    return false;
                break;
            case source_declaration_kind::alias:
                // Aliases are recorded and validated but not consumed yet.
                continue;
            }
        }
    } else {
        for (const auto& record : facts.records()) {
            if (!compare_record(record))
                return false;
        }
        for (const auto& enum_value : facts.enums()) {
            if (!compare_enum(enum_value))
                return false;
        }
    }

    return type_position == cached_types.size();
}

const source_construction_state* source_contribution_cache::construction(type_handle handle) const noexcept {
    if (!handle || handle.value() >= committed.construction.size())
        return nullptr;
    return &committed.construction[handle.value()];
}

status source_contribution_cache_update::reserve_rebuild(
    std::size_t max_source_id,
    std::size_t type_declarations,
    std::size_t member_count,
    std::size_t modifier_count,
    std::size_t enum_value_count,
    std::size_t object_count,
    std::size_t link_count) noexcept {

    if (!failure.ok())
        return failure;
    if (owner == nullptr || prepared || published || !candidate.sources.empty() || !replaced.empty())
        return {status_code::invalid_argument};

    if (max_source_id >= (std::numeric_limits<std::uint32_t>::max)() ||
        type_declarations >= (std::numeric_limits<std::uint32_t>::max)() ||
        member_count >= (std::numeric_limits<std::uint32_t>::max)() ||
        modifier_count >= (std::numeric_limits<std::uint32_t>::max)() ||
        enum_value_count >= (std::numeric_limits<std::uint32_t>::max)() ||
        object_count >= (std::numeric_limits<std::uint32_t>::max)() ||
        link_count >= (std::numeric_limits<std::uint32_t>::max)()) {
        failure = {status_code::not_available};
        return failure;
    }

    try {
        auto result = reserve_with_headroom(candidate.sources, max_source_id + 1);
        if (!result.ok()) return result;
        result = reserve_with_headroom(replaced, max_source_id + 1);
        if (!result.ok()) return result;
        result = reserve_with_headroom(candidate.types, type_declarations);
        if (!result.ok()) return result;
        result = reserve_with_headroom(candidate.members, member_count);
        if (!result.ok()) return result;
        result = reserve_with_headroom(candidate.modifiers, modifier_count);
        if (!result.ok()) return result;
        result = reserve_with_headroom(candidate.enum_values, enum_value_count);
        if (!result.ok()) return result;
        result = reserve_with_headroom(candidate.objects, object_count);
        if (!result.ok()) return result;
        result = reserve_with_headroom(candidate.links, link_count);
        if (!result.ok()) return result;
        result = reserve_with_headroom(candidate.construction, type_declarations + 1);
        if (!result.ok()) return result;
        candidate.sources.resize(max_source_id + 1);
        replaced.resize(max_source_id + 1, 0);
        return {};
    } catch (const std::bad_alloc&) {
        failure = {status_code::initialization_failed};
    } catch (const std::length_error&) {
        failure = {status_code::not_available};
    } catch (...) {
        failure = {status_code::initialization_failed};
    }
    return failure;
}

const source_contribution_state* source_contribution_cache_update::state(source_id source) const noexcept {
    if (!source || source.value() >= candidate.sources.size())
        return nullptr;
    const auto& value = candidate.sources[source.value()];
    return value.source == source ? &value : nullptr;
}

std::span<const source_contribution_type> source_contribution_cache_update::types(source_id source) const noexcept {
    const auto* value = state(source);
    return value == nullptr ? std::span<const source_contribution_type>{} :
        checked_span(candidate.types, value->types);
}

status source_contribution_cache_update::replace(
    const source_facts& facts,
    operation_id operation,
    diagnostic_buffer& diagnostics) noexcept {

    if (!failure.ok())
        return failure;
    if (owner == nullptr || prepared || published || !facts.source())
        return {status_code::invalid_argument};

    source_facts_validation_error validation;
    const auto validation_status = validate_source_facts(facts, validation);
    if (!validation_status.ok()) {
        try {
            emit_source_facts_validation_diagnostic(facts, validation, operation, diagnostics);
        } catch (...) {
        }
        failure = validation_status;
        return failure;
    }

    // Construction capability gate: inheritance is recognized by the Parser
    // and validated in facts, but Generation Builder/layout cannot consume
    // bases yet (ABI stage). Publication with bases must fail loudly rather
    // than build a silently incomplete Graph. Parser success != pipeline
    // acceptance.
    for (const auto& record : facts.records()) {
        if (record.bases.count != 0) {
            try {
                diagnostics.emit({
                    diagnostics::generation_build_failed.id,
                    diagnostics::generation_build_failed.default_severity,
                    operation,
                    source_range{facts.source(), record.declaration.offset, record.declaration.length},
                    "record has base classes: inheritance is not supported by ABI/layout",
                });
            } catch (...) {
            }
            failure = {status_code::not_available};
            return failure;
        }
    }

    try {
        const auto source_index = static_cast<std::size_t>(facts.source().value());
        if (candidate.sources.size() <= source_index) {
            candidate.sources.resize(source_index + 1);
            replaced.resize(source_index + 1, 0);
        }
        if (replaced[source_index] != 0) {
            failure = {status_code::invalid_argument};
            return failure;
        }

        if (candidate.types.size() > (std::numeric_limits<std::uint32_t>::max)() ||
            candidate.members.size() > (std::numeric_limits<std::uint32_t>::max)() ||
            candidate.modifiers.size() > (std::numeric_limits<std::uint32_t>::max)() ||
            candidate.enum_values.size() > (std::numeric_limits<std::uint32_t>::max)() ||
            candidate.objects.size() > (std::numeric_limits<std::uint32_t>::max)() ||
            candidate.links.size() > (std::numeric_limits<std::uint32_t>::max)()) {
            failure = {status_code::not_available};
            return failure;
        }

        const auto type_begin = candidate.types.size();
        const auto member_begin = candidate.members.size();
        const auto modifier_begin = candidate.modifiers.size();
        const auto enum_value_begin = candidate.enum_values.size();
        const auto object_begin = candidate.objects.size();
        const auto link_begin = candidate.links.size();

        candidate.modifiers.insert(
            candidate.modifiers.end(), facts.modifiers().begin(), facts.modifiers().end());

        for (const auto& item : facts.members()) {
            source_contribution_member value;
            value.type.identity = item.type.identity;
            value.type.intrinsic = item.type.intrinsic;
            value.type.modifiers.count = item.type.modifiers.count;
            if (!add_u32(modifier_begin, item.type.modifiers.begin, value.type.modifiers.begin)) {
                failure = {status_code::not_available};
                return failure;
            }
            value.name = item.name;
            value.access = item.access;
            candidate.members.push_back(value);
        }

        for (const auto& item : facts.enum_values()) {
            source_contribution_enum_value value;
            value.name = item.name;
            value.value = item.value;
            candidate.enum_values.push_back(value);
        }

        for (const auto& item : facts.objects()) {
            source_contribution_object value;
            value.identity = item.identity;
            value.type.identity = item.type.identity;
            value.type.intrinsic = item.type.intrinsic;
            value.type.modifiers.count = item.type.modifiers.count;
            if (!add_u32(modifier_begin, item.type.modifiers.begin, value.type.modifiers.begin)) {
                failure = {status_code::not_available};
                return failure;
            }
            candidate.objects.push_back(value);
        }

        for (const auto& item : facts.links())
            candidate.links.push_back({item.source, item.target});

        const auto append_record = [&](std::uint32_t index) -> status {
            if (index >= facts.records().size())
                return {status_code::invalid_argument};
            const auto& item = facts.records()[index];
            source_contribution_type value;
            value.identity = item.identity;
            value.kind = source_contribution_type_kind::record;
            value.record_kind = item.record_kind;
            value.flags = item.declaration_kind == source_record_declaration_kind::definition ? 0x01u : 0u;
            value.definition_items.count = item.members.count;
            if (!add_u32(member_begin, item.members.begin, value.definition_items.begin))
                return {status_code::not_available};
            candidate.types.push_back(value);
            return {};
        };

        const auto append_enum = [&](std::uint32_t index) -> status {
            if (index >= facts.enums().size())
                return {status_code::invalid_argument};
            const auto& item = facts.enums()[index];
            source_contribution_type value;
            value.identity = item.identity;
            value.kind = source_contribution_type_kind::enumeration;
            value.explicit_underlying = item.explicit_underlying;
            value.flags = item.declaration_kind == source_enum_declaration_kind::definition ? 0x01u : 0u;
            if (item.scoped)
                value.flags = static_cast<std::uint8_t>(value.flags | 0x02u);
            value.definition_items.count = item.enumerators.count;
            if (!add_u32(enum_value_begin, item.enumerators.begin, value.definition_items.begin))
                return {status_code::not_available};
            candidate.types.push_back(value);
            return {};
        };

        if (!facts.declarations().empty()) {
            for (const auto& declaration : facts.declarations()) {
                status result;
                switch (declaration.kind) {
                case source_declaration_kind::namespace_scope:
                    continue;
                case source_declaration_kind::record_type:
                    result = append_record(declaration.index);
                    break;
                case source_declaration_kind::enum_type:
                    result = append_enum(declaration.index);
                    break;
                case source_declaration_kind::object:
                    result = declaration.index < facts.objects().size() ? status{} : status{status_code::invalid_argument};
                    break;
                case source_declaration_kind::link:
                    result = declaration.index < facts.links().size() ? status{} : status{status_code::invalid_argument};
                    break;
                case source_declaration_kind::alias:
                    // Recorded and validated; not consumed by contributions yet.
                    continue;
                }
                if (!result.ok()) {
                    failure = result;
                    return failure;
                }
            }
        } else {
            for (std::uint32_t index = 0; index < facts.records().size(); ++index) {
                const auto result = append_record(index);
                if (!result.ok()) {
                    failure = result;
                    return failure;
                }
            }
            for (std::uint32_t index = 0; index < facts.enums().size(); ++index) {
                const auto result = append_enum(index);
                if (!result.ok()) {
                    failure = result;
                    return failure;
                }
            }
        }

        if (candidate.types.size() - type_begin > (std::numeric_limits<std::uint32_t>::max)() ||
            candidate.members.size() - member_begin > (std::numeric_limits<std::uint32_t>::max)() ||
            candidate.modifiers.size() - modifier_begin > (std::numeric_limits<std::uint32_t>::max)() ||
            candidate.enum_values.size() - enum_value_begin > (std::numeric_limits<std::uint32_t>::max)() ||
            candidate.objects.size() - object_begin > (std::numeric_limits<std::uint32_t>::max)() ||
            candidate.links.size() - link_begin > (std::numeric_limits<std::uint32_t>::max)()) {
            failure = {status_code::not_available};
            return failure;
        }

        source_contribution_state state_value;
        state_value.source = facts.source();
        state_value.types = {
            static_cast<std::uint32_t>(type_begin),
            static_cast<std::uint32_t>(candidate.types.size() - type_begin)};
        state_value.members = {
            static_cast<std::uint32_t>(member_begin),
            static_cast<std::uint32_t>(candidate.members.size() - member_begin)};
        state_value.modifiers = {
            static_cast<std::uint32_t>(modifier_begin),
            static_cast<std::uint32_t>(candidate.modifiers.size() - modifier_begin)};
        state_value.enum_values = {
            static_cast<std::uint32_t>(enum_value_begin),
            static_cast<std::uint32_t>(candidate.enum_values.size() - enum_value_begin)};
        state_value.objects = {
            static_cast<std::uint32_t>(object_begin),
            static_cast<std::uint32_t>(candidate.objects.size() - object_begin)};
        state_value.links = {
            static_cast<std::uint32_t>(link_begin),
            static_cast<std::uint32_t>(candidate.links.size() - link_begin)};

        candidate.sources[source_index] = state_value;
        replaced[source_index] = 1;
        ++candidate.statistics.sources;
        candidate.statistics.type_declarations = candidate.types.size();
        candidate.statistics.members = candidate.members.size();
        candidate.statistics.modifiers = candidate.modifiers.size();
        candidate.statistics.enum_values = candidate.enum_values.size();
        candidate.statistics.objects = candidate.objects.size();
        candidate.statistics.links = candidate.links.size();
        return {};
    } catch (const std::bad_alloc&) {
        failure = {status_code::initialization_failed};
    } catch (const std::length_error&) {
        failure = {status_code::not_available};
    } catch (...) {
        failure = {status_code::initialization_failed};
    }

    return failure;
}

status source_contribution_cache_update::prepare_publish() noexcept {
    if (!failure.ok())
        return failure;
    if (owner == nullptr || prepared || published)
        return {status_code::invalid_argument};
    prepared = true;
    return {};
}

void source_contribution_cache_update::publish_prepared() noexcept {
    if (owner == nullptr || !prepared || published)
        return;

    owner->committed.swap(candidate);
    owner->statistics_value = owner->committed.statistics;
    owner->provenance_complete = true;
    published = true;
}

source_contribution_sparse_update::source_contribution_sparse_update(
    source_contribution_cache& cache) noexcept
    : owner(&cache),
      type_base(cache.committed.types.size()),
      member_base(cache.committed.members.size()),
      modifier_base(cache.committed.modifiers.size()),
      enum_value_base(cache.committed.enum_values.size()),
      object_base(cache.committed.objects.size()),
      link_base(cache.committed.links.size()) {
}

status source_contribution_sparse_update::reserve_incremental(
    std::size_t source_changes,
    std::size_t touched_type_upper_bound,
    std::size_t type_declarations,
    std::size_t member_count,
    std::size_t modifier_count,
    std::size_t enum_value_count,
    std::size_t object_count,
    std::size_t link_count) noexcept {

    if (!failure.ok())
        return failure;
    if (owner == nullptr || prepared || published || !source_patches.empty() ||
        !construction_patches.empty() || !candidate.types.empty()) {
        return {status_code::invalid_argument};
    }

    if (type_declarations > (std::numeric_limits<std::uint32_t>::max)() - type_base ||
        member_count > (std::numeric_limits<std::uint32_t>::max)() - member_base ||
        modifier_count > (std::numeric_limits<std::uint32_t>::max)() - modifier_base ||
        enum_value_count > (std::numeric_limits<std::uint32_t>::max)() - enum_value_base ||
        object_count > (std::numeric_limits<std::uint32_t>::max)() - object_base ||
        link_count > (std::numeric_limits<std::uint32_t>::max)() - link_base) {
        failure = {status_code::not_available};
        return failure;
    }

    const auto source_capacity = patch_capacity(source_changes);
    const auto construction_capacity = patch_capacity(touched_type_upper_bound);
    if ((source_changes != 0 && source_capacity == 0) ||
        (touched_type_upper_bound != 0 && construction_capacity == 0)) {
        failure = {status_code::not_available};
        return failure;
    }

    try {
        candidate.types.reserve(type_declarations);
        candidate.members.reserve(member_count);
        candidate.modifiers.reserve(modifier_count);
        candidate.enum_values.reserve(enum_value_count);
        candidate.objects.reserve(object_count);
        candidate.links.reserve(link_count);
        source_patches.reserve(source_changes);
        construction_patches.reserve(touched_type_upper_bound);
        changed_source_ids.reserve(source_changes);
        source_patch_index.assign(source_capacity, {});
        construction_patch_index.assign(construction_capacity, {});
        return {};
    } catch (const std::bad_alloc&) {
        failure = {status_code::initialization_failed};
    } catch (const std::length_error&) {
        failure = {status_code::not_available};
    } catch (...) {
        failure = {status_code::initialization_failed};
    }
    return failure;
}

source_contribution_sparse_update::source_patch*
source_contribution_sparse_update::find_source_patch(source_id source) noexcept {
    return const_cast<source_patch*>(
        static_cast<const source_contribution_sparse_update&>(*this).find_source_patch(source));
}

const source_contribution_sparse_update::source_patch*
source_contribution_sparse_update::find_source_patch(source_id source) const noexcept {
    if (!source || source_patch_index.empty())
        return nullptr;
    const auto mask = source_patch_index.size() - 1;
    auto position = patch_hash(source.value()) & mask;
    for (std::size_t probe = 0; probe < source_patch_index.size(); ++probe) {
        const auto& slot = source_patch_index[position];
        if (slot.key == 0)
            return nullptr;
        if (slot.key == source.value()) {
            const auto index = static_cast<std::size_t>(slot.position - 1);
            return index < source_patches.size() ? &source_patches[index] : nullptr;
        }
        position = (position + 1) & mask;
    }
    return nullptr;
}

source_contribution_sparse_update::construction_patch*
source_contribution_sparse_update::find_construction_patch(type_handle handle) noexcept {
    return const_cast<construction_patch*>(
        static_cast<const source_contribution_sparse_update&>(*this).find_construction_patch(handle));
}

const source_contribution_sparse_update::construction_patch*
source_contribution_sparse_update::find_construction_patch(type_handle handle) const noexcept {
    if (!handle || construction_patch_index.empty())
        return nullptr;
    const auto mask = construction_patch_index.size() - 1;
    auto position = patch_hash(handle.value()) & mask;
    for (std::size_t probe = 0; probe < construction_patch_index.size(); ++probe) {
        const auto& slot = construction_patch_index[position];
        if (slot.key == 0)
            return nullptr;
        if (slot.key == handle.value()) {
            const auto index = static_cast<std::size_t>(slot.position - 1);
            return index < construction_patches.size() ? &construction_patches[index] : nullptr;
        }
        position = (position + 1) & mask;
    }
    return nullptr;
}

status source_contribution_sparse_update::add_source_patch(
    source_id source,
    source_patch*& output) noexcept {

    output = nullptr;
    if (!source || source_patch_index.empty())
        return {status_code::invalid_argument};
    if (find_source_patch(source) != nullptr)
        return {status_code::invalid_argument};

    const auto mask = source_patch_index.size() - 1;
    auto position = patch_hash(source.value()) & mask;
    for (std::size_t probe = 0; probe < source_patch_index.size(); ++probe) {
        auto& slot = source_patch_index[position];
        if (slot.key == 0) {
            try {
                source_patches.push_back(source_patch{});
                changed_source_ids.push_back(source);
            } catch (...) {
                return {status_code::initialization_failed};
            }
            auto& patch = source_patches.back();
            patch.source = source;
            slot.key = source.value();
            slot.position = static_cast<std::uint32_t>(source_patches.size());
            output = &patch;
            return {};
        }
        position = (position + 1) & mask;
    }
    return {status_code::not_available};
}

status source_contribution_sparse_update::add_construction_patch(
    type_handle handle,
    construction_patch*& output) noexcept {

    output = nullptr;
    if (!handle || construction_patch_index.empty())
        return {status_code::invalid_argument};
    if (auto* existing = find_construction_patch(handle)) {
        output = existing;
        return {};
    }

    const auto mask = construction_patch_index.size() - 1;
    auto position = patch_hash(handle.value()) & mask;
    for (std::size_t probe = 0; probe < construction_patch_index.size(); ++probe) {
        auto& slot = construction_patch_index[position];
        if (slot.key == 0) {
            try {
                construction_patches.push_back(construction_patch{});
            } catch (...) {
                return {status_code::initialization_failed};
            }
            auto& patch = construction_patches.back();
            patch.handle = handle;
            slot.key = handle.value();
            slot.position = static_cast<std::uint32_t>(construction_patches.size());
            output = &patch;
            return {};
        }
        position = (position + 1) & mask;
    }
    return {status_code::not_available};
}

status source_contribution_sparse_update::replace(
    const source_facts& facts,
    operation_id operation,
    diagnostic_buffer& diagnostics) noexcept {

    if (!failure.ok())
        return failure;
    if (owner == nullptr || prepared || published || !facts.source())
        return {status_code::invalid_argument};

    source_facts_validation_error validation;
    const auto validation_status = validate_source_facts(facts, validation);
    if (!validation_status.ok()) {
        try {
            emit_source_facts_validation_diagnostic(facts, validation, operation, diagnostics);
        } catch (...) {
        }
        failure = validation_status;
        return failure;
    }

    // Construction capability gate: inheritance is recognized by the Parser
    // and validated in facts, but Generation Builder/layout cannot consume
    // bases yet (ABI stage). Publication with bases must fail loudly rather
    // than build a silently incomplete Graph. Parser success != pipeline
    // acceptance.
    for (const auto& record : facts.records()) {
        if (record.bases.count != 0) {
            try {
                diagnostics.emit({
                    diagnostics::generation_build_failed.id,
                    diagnostics::generation_build_failed.default_severity,
                    operation,
                    source_range{facts.source(), record.declaration.offset, record.declaration.length},
                    "record has base classes: inheritance is not supported by ABI/layout",
                });
            } catch (...) {
            }
            failure = {status_code::not_available};
            return failure;
        }
    }

    source_patch* patch = nullptr;
    auto result = add_source_patch(facts.source(), patch);
    if (!result.ok()) {
        failure = result;
        return failure;
    }

    try {
        const auto type_local_begin = candidate.types.size();
        const auto member_local_begin = candidate.members.size();
        const auto modifier_local_begin = candidate.modifiers.size();
        const auto enum_value_local_begin = candidate.enum_values.size();
        const auto object_local_begin = candidate.objects.size();
        const auto link_local_begin = candidate.links.size();

        candidate.modifiers.insert(
            candidate.modifiers.end(), facts.modifiers().begin(), facts.modifiers().end());

        for (const auto& item : facts.members()) {
            source_contribution_member value;
            value.type.identity = item.type.identity;
            value.type.intrinsic = item.type.intrinsic;
            value.type.modifiers.count = item.type.modifiers.count;
            std::uint32_t global_modifier_base = 0;
            if (!absolute_u32(modifier_base, modifier_local_begin, global_modifier_base) ||
                !add_u32(global_modifier_base, item.type.modifiers.begin, value.type.modifiers.begin)) {
                failure = {status_code::not_available};
                return failure;
            }
            value.name = item.name;
            value.access = item.access;
            candidate.members.push_back(value);
        }

        for (const auto& item : facts.enum_values()) {
            source_contribution_enum_value value;
            value.name = item.name;
            value.value = item.value;
            candidate.enum_values.push_back(value);
        }

        for (const auto& item : facts.objects()) {
            source_contribution_object value;
            value.identity = item.identity;
            value.type.identity = item.type.identity;
            value.type.intrinsic = item.type.intrinsic;
            value.type.modifiers.count = item.type.modifiers.count;
            std::uint32_t global_modifier_base = 0;
            if (!absolute_u32(modifier_base, modifier_local_begin, global_modifier_base) ||
                !add_u32(global_modifier_base, item.type.modifiers.begin, value.type.modifiers.begin)) {
                failure = {status_code::not_available};
                return failure;
            }
            candidate.objects.push_back(value);
        }

        for (const auto& item : facts.links())
            candidate.links.push_back({item.source, item.target});

        std::uint32_t global_member_base = 0;
        std::uint32_t global_enum_base = 0;
        std::uint32_t global_object_base = 0;
        std::uint32_t global_link_base = 0;
        if (!absolute_u32(member_base, member_local_begin, global_member_base) ||
            !absolute_u32(enum_value_base, enum_value_local_begin, global_enum_base) ||
            !absolute_u32(object_base, object_local_begin, global_object_base) ||
            !absolute_u32(link_base, link_local_begin, global_link_base)) {
            failure = {status_code::not_available};
            return failure;
        }

        const auto append_record = [&](std::uint32_t index) -> status {
            if (index >= facts.records().size())
                return {status_code::invalid_argument};
            const auto& item = facts.records()[index];
            source_contribution_type value;
            value.identity = item.identity;
            value.kind = source_contribution_type_kind::record;
            value.record_kind = item.record_kind;
            value.flags = item.declaration_kind == source_record_declaration_kind::definition ? 0x01u : 0u;
            value.definition_items.count = item.members.count;
            if (!add_u32(global_member_base, item.members.begin, value.definition_items.begin))
                return {status_code::not_available};
            candidate.types.push_back(value);
            return {};
        };

        const auto append_enum = [&](std::uint32_t index) -> status {
            if (index >= facts.enums().size())
                return {status_code::invalid_argument};
            const auto& item = facts.enums()[index];
            source_contribution_type value;
            value.identity = item.identity;
            value.kind = source_contribution_type_kind::enumeration;
            value.explicit_underlying = item.explicit_underlying;
            value.flags = item.declaration_kind == source_enum_declaration_kind::definition ? 0x01u : 0u;
            if (item.scoped)
                value.flags = static_cast<std::uint8_t>(value.flags | 0x02u);
            value.definition_items.count = item.enumerators.count;
            if (!add_u32(global_enum_base, item.enumerators.begin, value.definition_items.begin))
                return {status_code::not_available};
            candidate.types.push_back(value);
            return {};
        };

        if (!facts.declarations().empty()) {
            for (const auto& declaration : facts.declarations()) {
                switch (declaration.kind) {
                case source_declaration_kind::namespace_scope:
                    continue;
                case source_declaration_kind::record_type:
                    result = append_record(declaration.index);
                    break;
                case source_declaration_kind::enum_type:
                    result = append_enum(declaration.index);
                    break;
                case source_declaration_kind::object:
                    result = declaration.index < facts.objects().size() ? status{} : status{status_code::invalid_argument};
                    break;
                case source_declaration_kind::link:
                    result = declaration.index < facts.links().size() ? status{} : status{status_code::invalid_argument};
                    break;
                case source_declaration_kind::alias:
                    // Recorded and validated; not consumed by contributions yet.
                    continue;
                }
                if (!result.ok()) {
                    failure = result;
                    return failure;
                }
            }
        } else {
            for (std::uint32_t index = 0; index < facts.records().size(); ++index) {
                result = append_record(index);
                if (!result.ok()) {
                    failure = result;
                    return failure;
                }
            }
            for (std::uint32_t index = 0; index < facts.enums().size(); ++index) {
                result = append_enum(index);
                if (!result.ok()) {
                    failure = result;
                    return failure;
                }
            }
        }

        const auto type_count = candidate.types.size() - type_local_begin;
        const auto member_count = candidate.members.size() - member_local_begin;
        const auto modifier_count = candidate.modifiers.size() - modifier_local_begin;
        const auto enum_count = candidate.enum_values.size() - enum_value_local_begin;
        const auto object_count = candidate.objects.size() - object_local_begin;
        const auto link_count = candidate.links.size() - link_local_begin;
        if (type_count > (std::numeric_limits<std::uint32_t>::max)() ||
            member_count > (std::numeric_limits<std::uint32_t>::max)() ||
            modifier_count > (std::numeric_limits<std::uint32_t>::max)() ||
            enum_count > (std::numeric_limits<std::uint32_t>::max)() ||
            object_count > (std::numeric_limits<std::uint32_t>::max)() ||
            link_count > (std::numeric_limits<std::uint32_t>::max)()) {
            failure = {status_code::not_available};
            return failure;
        }

        std::uint32_t global_type_begin = 0;
        if (!absolute_u32(type_base, type_local_begin, global_type_begin)) {
            failure = {status_code::not_available};
            return failure;
        }

        patch->state.source = facts.source();
        patch->state.types = {global_type_begin, static_cast<std::uint32_t>(type_count)};
        patch->state.members = {global_member_base, static_cast<std::uint32_t>(member_count)};
        patch->state.modifiers = {
            static_cast<std::uint32_t>(modifier_base + modifier_local_begin),
            static_cast<std::uint32_t>(modifier_count)};
        patch->state.enum_values = {global_enum_base, static_cast<std::uint32_t>(enum_count)};
        patch->state.objects = {global_object_base, static_cast<std::uint32_t>(object_count)};
        patch->state.links = {global_link_base, static_cast<std::uint32_t>(link_count)};
        patch->present = true;
        return {};
    } catch (const std::bad_alloc&) {
        failure = {status_code::initialization_failed};
    } catch (const std::length_error&) {
        failure = {status_code::not_available};
    } catch (...) {
        failure = {status_code::initialization_failed};
    }
    return failure;
}

status source_contribution_sparse_update::remove(source_id source) noexcept {
    if (!failure.ok())
        return failure;
    if (owner == nullptr || prepared || published || !source)
        return {status_code::invalid_argument};

    source_patch* patch = nullptr;
    const auto result = add_source_patch(source, patch);
    if (!result.ok()) {
        failure = result;
        return failure;
    }
    patch->state = {};
    patch->present = false;
    return {};
}

const source_contribution_state* source_contribution_sparse_update::previous_state(source_id source) const noexcept {
    return owner == nullptr ? nullptr : owner->state(source);
}

const source_contribution_state* source_contribution_sparse_update::replacement_state(source_id source) const noexcept {
    const auto* patch = find_source_patch(source);
    return patch != nullptr && patch->present ? &patch->state : nullptr;
}

std::span<const source_contribution_type> source_contribution_sparse_update::previous_types(source_id source) const noexcept {
    return owner == nullptr ? std::span<const source_contribution_type>{} : owner->types(source);
}

std::span<const source_contribution_type> source_contribution_sparse_update::replacement_types(source_id source) const noexcept {
    const auto* state_value = replacement_state(source);
    return state_value == nullptr || owner == nullptr ? std::span<const source_contribution_type>{} :
        combined_span(owner->committed.types, candidate.types, type_base, state_value->types);
}

const source_contribution_type* source_contribution_sparse_update::type(
    std::uint32_t zero_based_index) const noexcept {

    if (owner == nullptr)
        return nullptr;
    const auto index = static_cast<std::size_t>(zero_based_index);
    if (index < type_base)
        return index < owner->committed.types.size() ? &owner->committed.types[index] : nullptr;
    const auto local = index - type_base;
    return local < candidate.types.size() ? &candidate.types[local] : nullptr;
}

std::span<const source_contribution_member> source_contribution_sparse_update::members(
    source_fact_range range) const noexcept {

    return owner == nullptr ? std::span<const source_contribution_member>{} :
        combined_span(owner->committed.members, candidate.members, member_base, range);
}

std::span<const source_type_modifier> source_contribution_sparse_update::modifiers(
    source_fact_range range) const noexcept {

    return owner == nullptr ? std::span<const source_type_modifier>{} :
        combined_span(owner->committed.modifiers, candidate.modifiers, modifier_base, range);
}

std::span<const source_contribution_enum_value> source_contribution_sparse_update::enum_values(
    source_fact_range range) const noexcept {

    return owner == nullptr ? std::span<const source_contribution_enum_value>{} :
        combined_span(owner->committed.enum_values, candidate.enum_values, enum_value_base, range);
}

std::span<const source_contribution_object> source_contribution_sparse_update::previous_objects(
    source_id source) const noexcept {
    return owner == nullptr ? std::span<const source_contribution_object>{} : owner->objects(source);
}

std::span<const source_contribution_object> source_contribution_sparse_update::replacement_objects(
    source_id source) const noexcept {
    const auto* state_value = replacement_state(source);
    return state_value == nullptr || owner == nullptr ? std::span<const source_contribution_object>{} :
        combined_span(owner->committed.objects, candidate.objects, object_base, state_value->objects);
}

std::span<const source_contribution_link> source_contribution_sparse_update::previous_links(
    source_id source) const noexcept {
    return owner == nullptr ? std::span<const source_contribution_link>{} : owner->links(source);
}

std::span<const source_contribution_link> source_contribution_sparse_update::replacement_links(
    source_id source) const noexcept {
    const auto* state_value = replacement_state(source);
    return state_value == nullptr || owner == nullptr ? std::span<const source_contribution_link>{} :
        combined_span(owner->committed.links, candidate.links, link_base, state_value->links);
}

const source_construction_state* source_contribution_sparse_update::construction(type_handle handle) const noexcept {
    if (const auto* patch = find_construction_patch(handle))
        return &patch->state;
    return owner == nullptr ? nullptr : owner->construction(handle);
}

status source_contribution_sparse_update::set_construction(
    type_handle handle,
    const source_construction_state& state_value) noexcept {

    if (!failure.ok())
        return failure;
    if (owner == nullptr || prepared || published || !handle)
        return {status_code::invalid_argument};
    construction_patch* patch = nullptr;
    const auto result = add_construction_patch(handle, patch);
    if (!result.ok()) {
        failure = result;
        return failure;
    }
    patch->state = state_value;
    return {};
}

status source_contribution_sparse_update::prepare_publish() noexcept {
    if (!failure.ok())
        return failure;
    if (owner == nullptr || prepared || published || !owner->provenance_complete)
        return {status_code::invalid_argument};

    try {
        prepared_statistics = owner->statistics_value;
        std::size_t max_source = owner->committed.sources.empty() ? 0 : owner->committed.sources.size() - 1;
        std::size_t max_handle = owner->committed.construction.empty() ? 0 : owner->committed.construction.size() - 1;

        for (const auto& patch : source_patches) {
            max_source = std::max(max_source, static_cast<std::size_t>(patch.source.value()));
            const auto* old = owner->state(patch.source);
            if (old != nullptr) {
                if (prepared_statistics.sources == 0 ||
                    !subtract_count(prepared_statistics.type_declarations, old->types.count) ||
                    !subtract_count(prepared_statistics.members, old->members.count) ||
                    !subtract_count(prepared_statistics.modifiers, old->modifiers.count) ||
                    !subtract_count(prepared_statistics.enum_values, old->enum_values.count) ||
                    !subtract_count(prepared_statistics.objects, old->objects.count) ||
                    !subtract_count(prepared_statistics.links, old->links.count)) {
                    return {status_code::invalid_argument};
                }
                --prepared_statistics.sources;
            }
            if (patch.present) {
                ++prepared_statistics.sources;
                prepared_statistics.type_declarations += patch.state.types.count;
                prepared_statistics.members += patch.state.members.count;
                prepared_statistics.modifiers += patch.state.modifiers.count;
                prepared_statistics.enum_values += patch.state.enum_values.count;
                prepared_statistics.objects += patch.state.objects.count;
                prepared_statistics.links += patch.state.links.count;
            }
        }
        for (const auto& patch : construction_patches)
            max_handle = std::max(max_handle, static_cast<std::size_t>(patch.handle.value()));

        if (max_source >= (std::numeric_limits<std::uint32_t>::max)() ||
            max_handle >= (std::numeric_limits<std::uint32_t>::max)()) {
            return {status_code::not_available};
        }

        prepared_source_size = max_source + 1;
        prepared_construction_size = max_handle + 1;

        if (owner->baseline_backed()) {
            owner->committed.sources.reserve(prepared_source_size);
            owner->committed.construction.reserve(prepared_construction_size);
            owner->committed.types.reserve(type_base + candidate.types.size());
            owner->committed.members.reserve(member_base + candidate.members.size());
            owner->committed.modifiers.reserve(modifier_base + candidate.modifiers.size());
            owner->committed.enum_values.reserve(enum_value_base + candidate.enum_values.size());
            owner->committed.objects.reserve(object_base + candidate.objects.size());
            owner->committed.links.reserve(link_base + candidate.links.size());

            // Publication writes through mutable operator[]. Materialize every
            // touched persisted slot now so publish_prepared() remains allocation-
            // free/no-fail after the transaction's publication barrier.
            for (const auto& patch : source_patches) {
                if (patch.source.value() < owner->committed.sources.baseline_size())
                    (void)owner->committed.sources[patch.source.value()];
            }
            for (const auto& patch : construction_patches) {
                if (patch.handle.value() < owner->committed.construction.baseline_size())
                    (void)owner->committed.construction[patch.handle.value()];
            }

            if (!owner->committed.sources.read_status().ok())
                return owner->committed.sources.read_status();
            if (!owner->committed.construction.read_status().ok())
                return owner->committed.construction.read_status();
        }

        if (prepared_source_size > owner->committed.sources.capacity() ||
            prepared_construction_size > owner->committed.construction.capacity() ||
            type_base + candidate.types.size() > owner->committed.types.capacity() ||
            member_base + candidate.members.size() > owner->committed.members.capacity() ||
            modifier_base + candidate.modifiers.size() > owner->committed.modifiers.capacity() ||
            enum_value_base + candidate.enum_values.size() > owner->committed.enum_values.capacity() ||
            object_base + candidate.objects.size() > owner->committed.objects.capacity() ||
            link_base + candidate.links.size() > owner->committed.links.capacity()) {
            return {status_code::rebuild_required};
        }
        prepared = true;
        return {};
    } catch (const std::bad_alloc&) {
        failure = {status_code::initialization_failed};
    } catch (const std::length_error&) {
        failure = {status_code::not_available};
    } catch (...) {
        failure = {status_code::initialization_failed};
    }
    return failure;
}

void source_contribution_sparse_update::publish_prepared() noexcept {
    if (owner == nullptr || !prepared || published)
        return;

    owner->committed.sources.resize(prepared_source_size);
    owner->committed.construction.resize(prepared_construction_size);

    append_prepared(owner->committed.types, candidate.types);
    append_prepared(owner->committed.members, candidate.members);
    append_prepared(owner->committed.modifiers, candidate.modifiers);
    append_prepared(owner->committed.enum_values, candidate.enum_values);
    append_prepared(owner->committed.objects, candidate.objects);
    append_prepared(owner->committed.links, candidate.links);

    for (const auto& patch : source_patches) {
        auto& target = owner->committed.sources[patch.source.value()];
        target = patch.present ? patch.state : source_contribution_state{};
    }
    for (const auto& patch : construction_patches)
        owner->committed.construction[patch.handle.value()] = patch.state;

    owner->statistics_value = prepared_statistics;
    owner->committed.statistics = prepared_statistics;
    owner->provenance_complete = true;
    published = true;
}

} // namespace cw::server
