#include "source_contribution.hpp"

#include "../frontend/source_facts_validation.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"

#include <bit>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace cw::server {
namespace {

template<class T>
[[nodiscard]] std::span<const T> checked_span(
    const std::vector<T>& values,
    source_fact_range range) noexcept {

    const auto begin = static_cast<std::size_t>(range.begin);
    const auto count = static_cast<std::size_t>(range.count);
    if (begin > values.size() || count > values.size() - begin || count == 0)
        return {};
    return {values.data() + begin, count};
}

template<class T>
[[nodiscard]] std::span<const T> combined_span(
    const std::vector<T>& committed,
    const std::vector<T>& appended,
    std::size_t base,
    source_fact_range range) noexcept {

    const auto begin = static_cast<std::size_t>(range.begin);
    const auto count = static_cast<std::size_t>(range.count);
    if (count == 0)
        return {};
    if (begin < base) {
        if (begin > committed.size() || count > committed.size() - begin || begin + count > base)
            return {};
        return {committed.data() + begin, count};
    }
    const auto local = begin - base;
    if (local > appended.size() || count > appended.size() - local)
        return {};
    return {appended.data() + local, count};
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

[[nodiscard]] status append_name(
    const source_facts& facts,
    source_span span,
    std::vector<char>& names,
    std::size_t base,
    source_contribution_name_ref& output) {

    const auto text = facts.text(span);
    if (text.size() > (std::numeric_limits<std::uint32_t>::max)() ||
        names.size() > (std::numeric_limits<std::size_t>::max)() - text.size()) {
        return {status_code::not_available};
    }

    if (!absolute_u32(base, names.size(), output.offset))
        return {status_code::not_available};
    output.length = static_cast<std::uint32_t>(text.size());
    names.insert(names.end(), text.begin(), text.end());
    return {};
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
void append_prepared(std::vector<T>& target, const std::vector<T>& source) noexcept {
    for (const auto& value : source)
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

} // namespace

void source_contribution_cache::storage::swap(storage& other) noexcept {
    sources.swap(other.sources);
    types.swap(other.types);
    members.swap(other.members);
    modifiers.swap(other.modifiers);
    enum_values.swap(other.enum_values);
    names.swap(other.names);
    construction.swap(other.construction);
    std::swap(statistics, other.statistics);
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

std::string_view source_contribution_cache::name(source_contribution_name_ref value) const noexcept {
    const auto offset = static_cast<std::size_t>(value.offset);
    const auto length = static_cast<std::size_t>(value.length);
    if (offset > committed.names.size() || length > committed.names.size() - offset)
        return {};
    return {committed.names.data() + offset, length};
}

bool source_contribution_cache::equivalent(const source_facts& facts) const noexcept {
    const auto* source_state = state(facts.source());
    if (source_state == nullptr)
        return false;

    const auto cached_types = types(facts.source());
    const auto cached_members = members(source_state->members);
    const auto cached_modifiers = modifiers(source_state->modifiers);
    const auto cached_enum_values = enum_values(source_state->enum_values);

    if (cached_members.size() != facts.members().size() ||
        cached_modifiers.size() != facts.modifiers().size() ||
        cached_enum_values.size() != facts.enum_values().size()) {
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
            name(cached.name) != facts.text(current.name)) {
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
            name(cached.name) != facts.text(current.name)) {
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
    std::size_t name_bytes) noexcept {

    if (!failure.ok())
        return failure;
    if (owner == nullptr || prepared || published || !candidate.sources.empty() || !replaced.empty())
        return {status_code::invalid_argument};

    if (max_source_id >= (std::numeric_limits<std::uint32_t>::max)() ||
        type_declarations >= (std::numeric_limits<std::uint32_t>::max)() ||
        member_count >= (std::numeric_limits<std::uint32_t>::max)() ||
        modifier_count >= (std::numeric_limits<std::uint32_t>::max)() ||
        enum_value_count >= (std::numeric_limits<std::uint32_t>::max)() ||
        name_bytes > (std::numeric_limits<std::uint32_t>::max)()) {
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
        result = reserve_with_headroom(candidate.names, name_bytes);
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
            candidate.enum_values.size() > (std::numeric_limits<std::uint32_t>::max)()) {
            failure = {status_code::not_available};
            return failure;
        }

        const auto type_begin = candidate.types.size();
        const auto member_begin = candidate.members.size();
        const auto modifier_begin = candidate.modifiers.size();
        const auto enum_value_begin = candidate.enum_values.size();

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
            auto result = append_name(facts, item.name, candidate.names, 0, value.name);
            if (!result.ok()) {
                failure = result;
                return failure;
            }
            value.access = item.access;
            candidate.members.push_back(value);
        }

        for (const auto& item : facts.enum_values()) {
            source_contribution_enum_value value;
            auto result = append_name(facts, item.name, candidate.names, 0, value.name);
            if (!result.ok()) {
                failure = result;
                return failure;
            }
            value.value = item.value;
            candidate.enum_values.push_back(value);
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
            candidate.enum_values.size() - enum_value_begin > (std::numeric_limits<std::uint32_t>::max)()) {
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

        candidate.sources[source_index] = state_value;
        replaced[source_index] = 1;
        ++candidate.statistics.sources;
        candidate.statistics.type_declarations = candidate.types.size();
        candidate.statistics.members = candidate.members.size();
        candidate.statistics.modifiers = candidate.modifiers.size();
        candidate.statistics.enum_values = candidate.enum_values.size();
        candidate.statistics.name_bytes = candidate.names.size();
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
      name_base(cache.committed.names.size()) {
}

status source_contribution_sparse_update::reserve_incremental(
    std::size_t source_changes,
    std::size_t touched_type_upper_bound,
    std::size_t type_declarations,
    std::size_t member_count,
    std::size_t modifier_count,
    std::size_t enum_value_count,
    std::size_t name_bytes) noexcept {

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
        name_bytes > (std::numeric_limits<std::uint32_t>::max)() - name_base) {
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
        candidate.names.reserve(name_bytes);
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
            result = append_name(facts, item.name, candidate.names, name_base, value.name);
            if (!result.ok()) {
                failure = result;
                return failure;
            }
            value.access = item.access;
            candidate.members.push_back(value);
        }

        for (const auto& item : facts.enum_values()) {
            source_contribution_enum_value value;
            result = append_name(facts, item.name, candidate.names, name_base, value.name);
            if (!result.ok()) {
                failure = result;
                return failure;
            }
            value.value = item.value;
            candidate.enum_values.push_back(value);
        }

        std::uint32_t global_member_base = 0;
        std::uint32_t global_enum_base = 0;
        if (!absolute_u32(member_base, member_local_begin, global_member_base) ||
            !absolute_u32(enum_value_base, enum_value_local_begin, global_enum_base)) {
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
        if (type_count > (std::numeric_limits<std::uint32_t>::max)() ||
            member_count > (std::numeric_limits<std::uint32_t>::max)() ||
            modifier_count > (std::numeric_limits<std::uint32_t>::max)() ||
            enum_count > (std::numeric_limits<std::uint32_t>::max)()) {
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

std::string_view source_contribution_sparse_update::name(source_contribution_name_ref value) const noexcept {
    if (owner == nullptr)
        return {};
    const auto offset = static_cast<std::size_t>(value.offset);
    const auto length = static_cast<std::size_t>(value.length);
    if (offset < name_base) {
        if (offset > owner->committed.names.size() || length > owner->committed.names.size() - offset ||
            offset + length > name_base) {
            return {};
        }
        return {owner->committed.names.data() + offset, length};
    }
    const auto local = offset - name_base;
    if (local > candidate.names.size() || length > candidate.names.size() - local)
        return {};
    return {candidate.names.data() + local, length};
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
                    !subtract_count(prepared_statistics.enum_values, old->enum_values.count)) {
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
        if (prepared_source_size > owner->committed.sources.capacity() ||
            prepared_construction_size > owner->committed.construction.capacity() ||
            type_base + candidate.types.size() > owner->committed.types.capacity() ||
            member_base + candidate.members.size() > owner->committed.members.capacity() ||
            modifier_base + candidate.modifiers.size() > owner->committed.modifiers.capacity() ||
            enum_value_base + candidate.enum_values.size() > owner->committed.enum_values.capacity() ||
            name_base + candidate.names.size() > owner->committed.names.capacity()) {
            return {status_code::not_available};
        }
        prepared_statistics.name_bytes = name_base + candidate.names.size();
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
    append_prepared(owner->committed.names, candidate.names);

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
