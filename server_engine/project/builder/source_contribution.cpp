#include "source_contribution.hpp"

#include "../frontend/source_facts_validation.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"

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
    if (begin > values.size() || count > values.size() - begin)
        return {};
    if (count == 0)
        return {};
    return {values.data() + begin, count};
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

[[nodiscard]] status append_name(
    const source_facts& facts,
    source_span span,
    std::vector<char>& names,
    source_contribution_name_ref& output) {

    const auto text = facts.text(span);
    if (text.size() > (std::numeric_limits<std::uint32_t>::max)() ||
        names.size() > (std::numeric_limits<std::uint32_t>::max)() - text.size()) {
        return {status_code::not_available};
    }

    output.offset = static_cast<std::uint32_t>(names.size());
    output.length = static_cast<std::uint32_t>(text.size());
    names.insert(names.end(), text.begin(), text.end());
    return {};
}

} // namespace

void source_contribution_cache::storage::swap(storage& other) noexcept {
    sources.swap(other.sources);
    types.swap(other.types);
    members.swap(other.members);
    modifiers.swap(other.modifiers);
    enum_values.swap(other.enum_values);
    names.swap(other.names);
    std::swap(statistics, other.statistics);
}

source_contribution_cache_update source_contribution_cache::begin_rebuild() noexcept {
    return source_contribution_cache_update{*this};
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
        candidate.sources.resize(max_source_id + 1);
        replaced.resize(max_source_id + 1, 0);
        candidate.types.reserve(type_declarations);
        candidate.members.reserve(member_count);
        candidate.modifiers.reserve(modifier_count);
        candidate.enum_values.reserve(enum_value_count);
        candidate.names.reserve(name_bytes);
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
            auto result = append_name(facts, item.name, candidate.names, value.name);
            if (!result.ok()) {
                failure = result;
                return failure;
            }
            value.access = item.access;
            candidate.members.push_back(value);
        }

        for (const auto& item : facts.enum_values()) {
            source_contribution_enum_value value;
            auto result = append_name(facts, item.name, candidate.names, value.name);
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

} // namespace cw::server
