#include "generation_builder.hpp"

#include "../../diagnostics/diagnostic_buffer.hpp"
#include "../../diagnostics/diagnostic_descriptor.hpp"

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cw::server {
namespace {

using clock_type = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t elapsed_ns(
    clock_type::time_point begin,
    clock_type::time_point end) noexcept {

    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
}

void emit_failure(
    status result,
    source_id source,
    operation_id operation,
    std::string detail,
    diagnostic_buffer& diagnostics) noexcept {

    try {
        detail.append("; status=");
        detail.append(std::to_string(static_cast<std::uint32_t>(result.code)));
        diagnostics.emit({
            diagnostics::generation_build_failed.id,
            diagnostics::generation_build_failed.default_severity,
            operation,
            source_range{source, 0, 0},
            std::move(detail),
        });
    } catch (...) {
    }
}

[[nodiscard]] std::uint64_t mix64(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

[[nodiscard]] std::uint32_t fold32(std::uint64_t value) noexcept {
    auto result = static_cast<std::uint32_t>(value ^ (value >> 32));
    return result == 0 ? 1u : result;
}

[[nodiscard]] bool checked_index_capacity(
    std::size_t count,
    std::size_t& output) noexcept {

    if (count == 0) {
        output = 0;
        return true;
    }
    if (count > (std::numeric_limits<std::size_t>::max)() / 2)
        return false;

    const auto minimum = count * 2;
    output = std::bit_ceil(minimum < 8 ? std::size_t{8} : minimum);
    return output >= minimum;
}

[[nodiscard]] bool checked_add_size(
    std::size_t& target,
    std::size_t value) noexcept {

    if (value > (std::numeric_limits<std::size_t>::max)() - target)
        return false;
    target += value;
    return true;
}

[[nodiscard]] bool is_union(source_record_kind kind) noexcept {
    return kind == source_record_kind::union_type;
}

[[nodiscard]] bool compatible_record_kind(
    source_record_kind left,
    source_record_kind right) noexcept {

    return is_union(left) == is_union(right);
}

[[nodiscard]] derived_type_kind derived_kind(source_type_modifier_kind kind) noexcept {
    switch (kind) {
    case source_type_modifier_kind::const_qualified:
        return derived_type_kind::const_qualified;
    case source_type_modifier_kind::volatile_qualified:
        return derived_type_kind::volatile_qualified;
    case source_type_modifier_kind::pointer:
        return derived_type_kind::pointer;
    case source_type_modifier_kind::lvalue_reference:
        return derived_type_kind::lvalue_reference;
    case source_type_modifier_kind::rvalue_reference:
        return derived_type_kind::rvalue_reference;
    case source_type_modifier_kind::bounded_array:
        return derived_type_kind::bounded_array;
    case source_type_modifier_kind::unbounded_array:
        return derived_type_kind::unbounded_array;
    }
    return derived_type_kind::pointer;
}

[[nodiscard]] std::uint8_t intrinsic_width(
    intrinsic_type type,
    const abi_configuration& abi) noexcept {

    switch (type) {
    case intrinsic_type::bool_type:
        return 1;
    case intrinsic_type::char_type:
    case intrinsic_type::signed_char:
    case intrinsic_type::unsigned_char:
    case intrinsic_type::char8_type:
        return 8;
    case intrinsic_type::wchar_type:
        return abi.target == abi_target::windows_x64 ? 16 : 32;
    case intrinsic_type::char16_type:
    case intrinsic_type::signed_short:
    case intrinsic_type::unsigned_short:
        return 16;
    case intrinsic_type::char32_type:
    case intrinsic_type::signed_int:
    case intrinsic_type::unsigned_int:
        return 32;
    case intrinsic_type::signed_long:
    case intrinsic_type::unsigned_long:
        return abi.target == abi_target::windows_x64 ? 32 : 64;
    case intrinsic_type::signed_long_long:
    case intrinsic_type::unsigned_long_long:
        return 64;
    default:
        return 0;
    }
}

[[nodiscard]] bool intrinsic_signed(intrinsic_type type) noexcept {
    switch (type) {
    case intrinsic_type::char_type:
    case intrinsic_type::signed_char:
    case intrinsic_type::wchar_type:
    case intrinsic_type::signed_short:
    case intrinsic_type::signed_int:
    case intrinsic_type::signed_long:
    case intrinsic_type::signed_long_long:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] std::int64_t signed_value(
    source_integral_constant value,
    const abi_configuration& abi) noexcept {

    const auto width = intrinsic_width(value.intrinsic, abi);
    if (width == 64)
        return static_cast<std::int64_t>(value.bits);
    if (width == 0)
        return 0;
    return static_cast<std::int64_t>(value.bits << (64 - width)) >> (64 - width);
}

[[nodiscard]] status select_enum_underlying(
    std::span<const source_contribution_enum_value> values,
    const abi_configuration& abi,
    intrinsic_type& output) noexcept {

    std::int64_t minimum = 0;
    std::uint64_t maximum = 0;
    bool negative = false;

    for (const auto& item : values) {
        const auto width = intrinsic_width(item.value.intrinsic, abi);
        if (width == 0)
            return {status_code::invalid_argument};

        if (intrinsic_signed(item.value.intrinsic)) {
            const auto value = signed_value(item.value, abi);
            if (value < 0) {
                negative = true;
                if (value < minimum)
                    minimum = value;
            } else if (static_cast<std::uint64_t>(value) > maximum) {
                maximum = static_cast<std::uint64_t>(value);
            }
        } else if (item.value.bits > maximum) {
            maximum = item.value.bits;
        }
    }

    if (minimum >= (std::numeric_limits<std::int32_t>::min)() &&
        maximum <= static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())) {
        output = intrinsic_type::signed_int;
        return {};
    }
    if (!negative && maximum <= (std::numeric_limits<std::uint32_t>::max)()) {
        output = intrinsic_type::unsigned_int;
        return {};
    }
    if (maximum <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        output = abi.target == abi_target::windows_x64 ?
            intrinsic_type::signed_long_long : intrinsic_type::signed_long;
        return {};
    }
    if (!negative) {
        output = abi.target == abi_target::windows_x64 ?
            intrinsic_type::unsigned_long_long : intrinsic_type::unsigned_long;
        return {};
    }
    return {status_code::semantic_conflict};
}

} // namespace

generation_builder::generation_builder(
    source_contribution_cache& contribution_cache,
    graph& target_graph) noexcept
    : contributions(contribution_cache.begin_rebuild()), target(target_graph) {
}

status generation_builder::prepare_g0(
    std::span<const source_facts> sources,
    const abi_configuration& abi,
    operation_id operation,
    diagnostic_buffer& diagnostics) noexcept {

    if (prepared || published_value || !is_supported_abi_configuration(abi))
        return {status_code::invalid_argument};

    telemetry_value = {};
    const auto total_begin = clock_type::now();
    const auto capture_begin = total_begin;

    std::size_t max_source_id = 0;
    std::size_t total_types = 0;
    std::size_t total_members = 0;
    std::size_t total_modifiers = 0;
    std::size_t total_enum_values = 0;
    std::size_t total_name_bytes = 0;
    for (const auto& facts : sources) {
        max_source_id = std::max(
            max_source_id, static_cast<std::size_t>(facts.source().value()));
        if (!checked_add_size(total_types, facts.records().size()) ||
            !checked_add_size(total_types, facts.enums().size()) ||
            !checked_add_size(total_members, facts.members().size()) ||
            !checked_add_size(total_modifiers, facts.modifiers().size()) ||
            !checked_add_size(total_enum_values, facts.enum_values().size())) {
            const status result{status_code::not_available};
            emit_failure(result, facts.source(), operation, "G0 contribution counts overflow", diagnostics);
            return result;
        }
        for (const auto& member : facts.members()) {
            if (!checked_add_size(total_name_bytes, member.name.length)) {
                const status result{status_code::not_available};
                emit_failure(result, facts.source(), operation, "G0 member-name bytes overflow", diagnostics);
                return result;
            }
        }
        for (const auto& value : facts.enum_values()) {
            if (!checked_add_size(total_name_bytes, value.name.length)) {
                const status result{status_code::not_available};
                emit_failure(result, facts.source(), operation, "G0 enum-name bytes overflow", diagnostics);
                return result;
            }
        }
    }

    auto reserve_result = contributions.reserve_rebuild(
        max_source_id, total_types, total_members, total_modifiers,
        total_enum_values, total_name_bytes);
    if (!reserve_result.ok()) {
        emit_failure(reserve_result, {}, operation, "SourceContribution G0 reserve failed", diagnostics);
        return reserve_result;
    }

    for (const auto& facts : sources) {
        const auto result = contributions.replace(facts, operation, diagnostics);
        if (!result.ok())
            return result;
    }

    const auto capture_end = clock_type::now();
    telemetry_value.contribution_capture_ns = elapsed_ns(capture_begin, capture_end);

    const auto graph_result = prepare_graph(abi, operation, diagnostics);
    if (!graph_result.ok())
        return graph_result;

    const auto publish_prepare_begin = clock_type::now();
    const auto contribution_result = contributions.prepare_publish();
    if (!contribution_result.ok()) {
        emit_failure(contribution_result, {}, operation, "SourceContribution prepare failed", diagnostics);
        return contribution_result;
    }
    const auto publish_prepare_end = clock_type::now();
    telemetry_value.prepare_publish_ns = elapsed_ns(publish_prepare_begin, publish_prepare_end);
    telemetry_value.total_prepare_ns = elapsed_ns(total_begin, publish_prepare_end);

    const auto statistics = contributions.statistics();
    telemetry_value.sources = statistics.sources;
    telemetry_value.type_declarations = statistics.type_declarations;
    telemetry_value.members = statistics.members;
    telemetry_value.enum_values = statistics.enum_values;
    telemetry_value.unique_types = prepared_graph.types.size();
    telemetry_value.canonical_type_refs =
        prepared_graph.canonical_types.empty() ? 0 : prepared_graph.canonical_types.size() - 1;

    prepared = true;
    return {};
}

status generation_builder::prepare_graph(
    const abi_configuration& abi,
    operation_id operation,
    diagnostic_buffer& diagnostics) noexcept {

    const auto& storage = contributions.candidate;
    if (storage.types.size() >= (std::numeric_limits<std::uint32_t>::max)()) {
        const status result{status_code::not_available};
        emit_failure(result, {}, operation, "Too many type declarations", diagnostics);
        return result;
    }

    struct identity_slot final {
        identity_ref identity = nullptr;
        std::uint32_t handle = 0;
    };

    class local_identity_index final {
    public:
        [[nodiscard]] status initialize(std::size_t count) noexcept {
            std::size_t capacity = 0;
            if (!checked_index_capacity(count, capacity))
                return {status_code::not_available};
            try {
                slots.assign(capacity, {});
                mask = capacity == 0 ? 0 : capacity - 1;
                return {};
            } catch (const std::bad_alloc&) {
                return {status_code::initialization_failed};
            } catch (const std::length_error&) {
                return {status_code::not_available};
            } catch (...) {
                return {status_code::initialization_failed};
            }
        }

        [[nodiscard]] status insert_or_find(
            identity_ref identity,
            std::uint32_t proposed,
            std::uint32_t& output,
            bool& inserted) noexcept {

            output = 0;
            inserted = false;
            if (identity == nullptr || proposed == 0 || slots.empty())
                return {status_code::invalid_argument};
            auto position = position_for(identity);
            for (std::size_t probe = 0; probe < slots.size(); ++probe) {
                auto& slot = slots[position];
                if (slot.identity == nullptr) {
                    slot.identity = identity;
                    slot.handle = proposed;
                    output = proposed;
                    inserted = true;
                    return {};
                }
                if (slot.identity == identity) {
                    output = slot.handle;
                    return {};
                }
                position = (position + 1) & mask;
            }
            return {status_code::not_available};
        }

        [[nodiscard]] std::uint32_t find(identity_ref identity) const noexcept {
            if (identity == nullptr || slots.empty())
                return 0;
            auto position = position_for(identity);
            for (std::size_t probe = 0; probe < slots.size(); ++probe) {
                const auto& slot = slots[position];
                if (slot.identity == nullptr)
                    return 0;
                if (slot.identity == identity)
                    return slot.handle;
                position = (position + 1) & mask;
            }
            return 0;
        }

    private:
        [[nodiscard]] std::size_t position_for(identity_ref identity) const noexcept {
            const auto value = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(identity));
            return static_cast<std::size_t>(mix64(value >> 3)) & mask;
        }

        std::vector<identity_slot> slots;
        std::size_t mask = 0;
    };

    local_identity_index identity_index;
    auto result = identity_index.initialize(storage.types.size());
    if (!result.ok()) {
        emit_failure(result, {}, operation, "Generation identity->handle index allocation failed", diagnostics);
        return result;
    }

    try {
        prepared_graph = {};
        prepared_graph.generation = 0;
        prepared_graph.types.reserve(storage.types.size());
        prepared_graph.identities.reserve(storage.types.size());
        prepared_graph.members.reserve(storage.members.size());
        prepared_graph.enum_values.reserve(storage.enum_values.size());
        prepared_graph.names = storage.names;
        prepared_graph.canonical_types.reserve(
            1 + storage.members.size() + storage.modifiers.size());
        prepared_graph.canonical_types.push_back({});

        std::vector<std::uint8_t> definition_seen;
        std::vector<std::uint32_t> declaration_handles(storage.types.size(), 0);
        std::vector<std::uint32_t> member_base_handles(storage.members.size(), 0);
        std::vector<TypeRef> member_type_refs(storage.members.size());
        definition_seen.reserve(storage.types.size());

        const auto identity_begin = clock_type::now();

        for (std::size_t source_index = 1; source_index < storage.sources.size(); ++source_index) {
            const auto& source_state = storage.sources[source_index];
            if (!source_state.source)
                continue;

            const auto begin = static_cast<std::size_t>(source_state.types.begin);
            const auto count = static_cast<std::size_t>(source_state.types.count);
            if (begin > storage.types.size() || count > storage.types.size() - begin) {
                result = {status_code::invalid_argument};
                emit_failure(result, source_state.source, operation, "SourceContribution type range invalid", diagnostics);
                return result;
            }

            for (std::size_t offset = 0; offset < count; ++offset) {
                const auto global_index = begin + offset;
                const auto& contribution = storage.types[global_index];
                const auto proposed = static_cast<std::uint32_t>(prepared_graph.types.size() + 1);
                std::uint32_t handle_value = 0;
                bool inserted = false;
                result = identity_index.insert_or_find(
                    contribution.identity, proposed, handle_value, inserted);
                if (!result.ok()) {
                    emit_failure(result, source_state.source, operation, "identity_ref materialization failed", diagnostics);
                    return result;
                }
                declaration_handles[global_index] = handle_value;

                if (inserted) {
                    type_entry entry;
                    if (contribution.kind == source_contribution_type_kind::record) {
                        entry.kind = graph_type_kind::record;
                        entry.record_kind = contribution.record_kind;
                    } else {
                        entry.kind = graph_type_kind::enumeration;
                        entry.enum_underlying = contribution.explicit_underlying == intrinsic_type::none ?
                            intrinsic_type::signed_int : contribution.explicit_underlying;
                        if (contribution.enum_scoped())
                            entry.flags = static_cast<std::uint8_t>(entry.flags | 0x01u);
                        if (contribution.explicit_underlying != intrinsic_type::none)
                            entry.flags = static_cast<std::uint8_t>(entry.flags | 0x02u);
                    }
                    prepared_graph.types.push_back(entry);
                    prepared_graph.identities.push_back(contribution.identity);
                    definition_seen.push_back(0);
                    continue;
                }

                auto& entry = prepared_graph.types[handle_value - 1];
                if ((entry.kind == graph_type_kind::record) !=
                    (contribution.kind == source_contribution_type_kind::record)) {
                    result = {status_code::semantic_conflict};
                    emit_failure(result, source_state.source, operation, "record/enum identity kind conflict", diagnostics);
                    return result;
                }

                if (entry.kind == graph_type_kind::record) {
                    if (!compatible_record_kind(entry.record_kind, contribution.record_kind)) {
                        result = {status_code::semantic_conflict};
                        emit_failure(result, source_state.source, operation, "union/non-union redeclaration conflict", diagnostics);
                        return result;
                    }
                    if (contribution.definition())
                        entry.record_kind = contribution.record_kind;
                } else {
                    if (entry.enum_scoped() != contribution.enum_scoped()) {
                        result = {status_code::semantic_conflict};
                        emit_failure(result, source_state.source, operation, "scoped enum redeclaration conflict", diagnostics);
                        return result;
                    }
                    if (contribution.explicit_underlying != intrinsic_type::none) {
                        if (entry.enum_fixed_underlying() &&
                            entry.enum_underlying != contribution.explicit_underlying) {
                            result = {status_code::semantic_conflict};
                            emit_failure(result, source_state.source, operation, "enum underlying type conflict", diagnostics);
                            return result;
                        }
                        entry.enum_underlying = contribution.explicit_underlying;
                        entry.flags = static_cast<std::uint8_t>(entry.flags | 0x02u);
                    }
                }
            }
        }

        for (std::size_t index = 0; index < storage.members.size(); ++index) {
            const auto& member = storage.members[index];
            if (member.type.identity == nullptr)
                continue;
            const auto handle = identity_index.find(member.type.identity);
            if (handle == 0) {
                result = {status_code::semantic_conflict};
                emit_failure(result, {}, operation, "member identity_ref has no Project type declaration", diagnostics);
                return result;
            }
            member_base_handles[index] = handle;
        }

        const auto identity_end = clock_type::now();
        telemetry_value.identity_to_handle_ns = elapsed_ns(identity_begin, identity_end);

        constexpr auto intrinsic_count =
            static_cast<std::size_t>(intrinsic_type::nullptr_type) + 1;
        std::array<TypeRef, intrinsic_count> intrinsic_refs{};
        std::vector<TypeRef> named_refs(prepared_graph.types.size() + 1);

        struct derived_slot final {
            std::uint32_t fingerprint = 0;
            std::uint32_t type_ref = 0;
        };
        std::vector<derived_slot> derived_slots;
        std::size_t derived_mask = 0;
        if (!storage.modifiers.empty()) {
            std::size_t capacity = 0;
            if (!checked_index_capacity(storage.modifiers.size(), capacity)) {
                result = {status_code::not_available};
                emit_failure(result, {}, operation, "Derived TypeRef index too large", diagnostics);
                return result;
            }
            derived_slots.assign(capacity, {});
            derived_mask = capacity - 1;
        }

        const auto append_canonical = [&](prepared_graph_generation::canonical_type_record record, TypeRef& output) -> status {
            if (prepared_graph.canonical_types.size() >= (std::numeric_limits<std::uint32_t>::max)())
                return {status_code::not_available};
            const auto index = static_cast<std::uint32_t>(prepared_graph.canonical_types.size());
            prepared_graph.canonical_types.push_back(record);
            output = TypeRef{index};
            return {};
        };

        const auto get_intrinsic = [&](intrinsic_type value, TypeRef& output) -> status {
            const auto index = static_cast<std::size_t>(value);
            if (value == intrinsic_type::none || index >= intrinsic_refs.size())
                return {status_code::invalid_argument};
            if (intrinsic_refs[index]) {
                output = intrinsic_refs[index];
                return {};
            }
            prepared_graph_generation::canonical_type_record record;
            record.kind = canonical_type_kind::intrinsic;
            record.detail = static_cast<std::uint8_t>(value);
            auto local = append_canonical(record, output);
            if (local.ok())
                intrinsic_refs[index] = output;
            return local;
        };

        const auto get_named = [&](std::uint32_t handle, TypeRef& output) -> status {
            if (handle == 0 || handle >= named_refs.size())
                return {status_code::invalid_argument};
            if (named_refs[handle]) {
                output = named_refs[handle];
                return {};
            }
            prepared_graph_generation::canonical_type_record record;
            record.kind = canonical_type_kind::named;
            record.child_or_handle = handle;
            auto local = append_canonical(record, output);
            if (local.ok())
                named_refs[handle] = output;
            return local;
        };

        std::uint64_t derived_count = 0;
        const auto get_derived = [&](derived_type_kind kind, TypeRef child, std::uint64_t payload, TypeRef& output) -> status {
            if (!child || derived_slots.empty())
                return {status_code::invalid_argument};

            const auto hash = mix64(
                (static_cast<std::uint64_t>(static_cast<std::uint8_t>(kind)) << 56) ^
                (static_cast<std::uint64_t>(child.value()) << 16) ^ mix64(payload));
            const auto fingerprint = fold32(hash);
            auto position = static_cast<std::size_t>(hash) & derived_mask;

            for (std::size_t probe = 0; probe < derived_slots.size(); ++probe) {
                auto& slot = derived_slots[position];
                if (slot.type_ref == 0) {
                    prepared_graph_generation::canonical_type_record record;
                    record.kind = canonical_type_kind::derived;
                    record.detail = static_cast<std::uint8_t>(kind);
                    record.child_or_handle = child.value();
                    record.payload = payload;
                    auto local = append_canonical(record, output);
                    if (!local.ok())
                        return local;
                    slot.fingerprint = fingerprint;
                    slot.type_ref = output.value();
                    ++derived_count;
                    return {};
                }
                if (slot.fingerprint == fingerprint) {
                    const auto& record = prepared_graph.canonical_types[slot.type_ref];
                    if (record.kind == canonical_type_kind::derived &&
                        record.detail == static_cast<std::uint8_t>(kind) &&
                        record.child_or_handle == child.value() &&
                        record.payload == payload) {
                        output = TypeRef{slot.type_ref};
                        return {};
                    }
                }
                position = (position + 1) & derived_mask;
            }
            return {status_code::not_available};
        };

        const auto type_ref_begin = clock_type::now();
        for (std::size_t index = 0; index < storage.members.size(); ++index) {
            const auto& member = storage.members[index];
            TypeRef current;
            if (member.type.identity != nullptr)
                result = get_named(member_base_handles[index], current);
            else
                result = get_intrinsic(member.type.intrinsic, current);
            if (!result.ok()) {
                emit_failure(result, {}, operation, "member base TypeRef materialization failed", diagnostics);
                return result;
            }

            const auto modifier_begin = static_cast<std::size_t>(member.type.modifiers.begin);
            const auto modifier_count = static_cast<std::size_t>(member.type.modifiers.count);
            if (modifier_begin > storage.modifiers.size() ||
                modifier_count > storage.modifiers.size() - modifier_begin) {
                result = {status_code::invalid_argument};
                emit_failure(result, {}, operation, "member modifier range invalid", diagnostics);
                return result;
            }

            for (std::size_t modifier_index = 0; modifier_index < modifier_count; ++modifier_index) {
                const auto& modifier = storage.modifiers[modifier_begin + modifier_index];
                TypeRef wrapped;
                result = get_derived(derived_kind(modifier.kind), current, modifier.value, wrapped);
                if (!result.ok()) {
                    emit_failure(result, {}, operation, "derived TypeRef materialization failed", diagnostics);
                    return result;
                }
                current = wrapped;
            }
            member_type_refs[index] = current;
        }
        const auto type_ref_end = clock_type::now();
        telemetry_value.type_ref_materialization_ns = elapsed_ns(type_ref_begin, type_ref_end);
        telemetry_value.derived_type_refs = derived_count;

        const auto definition_begin = clock_type::now();
        for (std::size_t source_index = 1; source_index < storage.sources.size(); ++source_index) {
            const auto& source_state = storage.sources[source_index];
            if (!source_state.source)
                continue;

            const auto begin = static_cast<std::size_t>(source_state.types.begin);
            const auto count = static_cast<std::size_t>(source_state.types.count);
            for (std::size_t offset = 0; offset < count; ++offset) {
                const auto global_index = begin + offset;
                const auto& contribution = storage.types[global_index];
                if (!contribution.definition())
                    continue;

                const auto handle_value = declaration_handles[global_index];
                auto& seen = definition_seen[handle_value - 1];
                auto& entry = prepared_graph.types[handle_value - 1];
                if (seen != 0) {
                    result = {status_code::semantic_conflict};
                    emit_failure(result, source_state.source, operation, "multiple type definitions", diagnostics);
                    return result;
                }
                seen = 1;

                if (contribution.kind == source_contribution_type_kind::record) {
                    const auto item_begin = static_cast<std::size_t>(contribution.definition_items.begin);
                    const auto item_count = static_cast<std::size_t>(contribution.definition_items.count);
                    if (item_begin > storage.members.size() || item_count > storage.members.size() - item_begin ||
                        prepared_graph.members.size() >= (std::numeric_limits<std::uint32_t>::max)() ||
                        item_count > (std::numeric_limits<std::uint32_t>::max)()) {
                        result = {status_code::not_available};
                        emit_failure(result, source_state.source, operation, "record definition range too large", diagnostics);
                        return result;
                    }
                    entry.definition.begin = static_cast<std::uint32_t>(prepared_graph.members.size() + 1);
                    entry.definition.count = static_cast<std::uint32_t>(item_count);
                    for (std::size_t item = 0; item < item_count; ++item) {
                        const auto member_index = item_begin + item;
                        const auto& contribution_member = storage.members[member_index];
                        member_record materialized;
                        materialized.name = {
                            contribution_member.name.offset,
                            contribution_member.name.length};
                        materialized.type = member_type_refs[member_index];
                        materialized.access = contribution_member.access;
                        prepared_graph.members.push_back(materialized);
                    }
                } else {
                    const auto item_begin = static_cast<std::size_t>(contribution.definition_items.begin);
                    const auto item_count = static_cast<std::size_t>(contribution.definition_items.count);
                    if (item_begin > storage.enum_values.size() || item_count > storage.enum_values.size() - item_begin ||
                        prepared_graph.enum_values.size() >= (std::numeric_limits<std::uint32_t>::max)() ||
                        item_count > (std::numeric_limits<std::uint32_t>::max)()) {
                        result = {status_code::not_available};
                        emit_failure(result, source_state.source, operation, "enum definition range too large", diagnostics);
                        return result;
                    }

                    if (entry.enum_fixed_underlying()) {
                        // A previous compatible redeclaration may have fixed the
                        // underlying type before this defining declaration.
                    } else if (contribution.explicit_underlying != intrinsic_type::none) {
                        entry.enum_underlying = contribution.explicit_underlying;
                        entry.flags = static_cast<std::uint8_t>(entry.flags | 0x02u);
                    } else if (contribution.enum_scoped()) {
                        entry.enum_underlying = intrinsic_type::signed_int;
                    } else {
                        const auto values = item_count == 0 ?
                            std::span<const source_contribution_enum_value>{} :
                            std::span<const source_contribution_enum_value>{
                                storage.enum_values.data() + item_begin, item_count};
                        result = select_enum_underlying(values, abi, entry.enum_underlying);
                        if (!result.ok()) {
                            emit_failure(result, source_state.source, operation, "enum underlying selection failed", diagnostics);
                            return result;
                        }
                    }

                    entry.definition.begin = static_cast<std::uint32_t>(prepared_graph.enum_values.size() + 1);
                    entry.definition.count = static_cast<std::uint32_t>(item_count);
                    for (std::size_t item = 0; item < item_count; ++item) {
                        const auto& value = storage.enum_values[item_begin + item];
                        enum_value_record materialized;
                        materialized.bits = value.value.bits;
                        materialized.name = {value.name.offset, value.name.length};
                        materialized.intrinsic = value.value.intrinsic;
                        prepared_graph.enum_values.push_back(materialized);
                    }
                }
            }
        }
        const auto definition_end = clock_type::now();
        telemetry_value.definition_materialization_ns = elapsed_ns(definition_begin, definition_end);

        const auto validation_begin = clock_type::now();
        if (prepared_graph.types.size() != prepared_graph.identities.size() ||
            prepared_graph.types.size() != definition_seen.size()) {
            result = {status_code::invalid_argument};
            emit_failure(result, {}, operation, "detached Graph type arrays diverged", diagnostics);
            return result;
        }
        for (std::size_t index = 0; index < prepared_graph.types.size(); ++index) {
            if (prepared_graph.identities[index] == nullptr) {
                result = {status_code::invalid_argument};
                emit_failure(result, {}, operation, "detached Graph contains null identity_ref", diagnostics);
                return result;
            }
            const auto& entry = prepared_graph.types[index];
            if (!entry.definition)
                continue;
            const auto range_begin = static_cast<std::size_t>(entry.definition.begin - 1);
            const auto range_count = static_cast<std::size_t>(entry.definition.count);
            const auto arena_size = entry.kind == graph_type_kind::record ?
                prepared_graph.members.size() : prepared_graph.enum_values.size();
            if (range_begin > arena_size || range_count > arena_size - range_begin) {
                result = {status_code::invalid_argument};
                emit_failure(result, {}, operation, "detached Graph definition range invalid", diagnostics);
                return result;
            }
        }
        for (std::size_t index = 1; index < prepared_graph.canonical_types.size(); ++index) {
            const auto& type = prepared_graph.canonical_types[index];
            if (type.kind == canonical_type_kind::named) {
                if (type.child_or_handle == 0 || type.child_or_handle > prepared_graph.types.size()) {
                    result = {status_code::invalid_argument};
                    emit_failure(result, {}, operation, "named TypeRef handle invalid", diagnostics);
                    return result;
                }
            } else if (type.kind == canonical_type_kind::derived) {
                if (type.child_or_handle == 0 || type.child_or_handle >= prepared_graph.canonical_types.size()) {
                    result = {status_code::invalid_argument};
                    emit_failure(result, {}, operation, "derived TypeRef child invalid", diagnostics);
                    return result;
                }
            }
        }
        const auto validation_end = clock_type::now();
        telemetry_value.validation_ns = elapsed_ns(validation_begin, validation_end);
        return {};
    } catch (const std::bad_alloc&) {
        result = {status_code::initialization_failed};
    } catch (const std::length_error&) {
        result = {status_code::not_available};
    } catch (...) {
        result = {status_code::initialization_failed};
    }

    emit_failure(result, {}, operation, "Generation G0 preparation failed", diagnostics);
    return result;
}

void generation_builder::publish_prepared() noexcept {
    if (!prepared || published_value)
        return;

    const auto begin = clock_type::now();
    target.publish_prepared(prepared_graph);
    contributions.publish_prepared();
    const auto end = clock_type::now();
    telemetry_value.publish_ns = elapsed_ns(begin, end);
    published_value = true;
}

} // namespace cw::server
