#include "source_facts_validation.hpp"

#include "../../diagnostics/diagnostic_descriptor.hpp"

#include <limits>
#include <string>
#include <utility>

namespace cw::server {
namespace {

[[nodiscard]] constexpr bool valid_span(source_span range, std::size_t source_size) noexcept {
    return range.offset <= source_size && range.length <= source_size - range.offset;
}

[[nodiscard]] constexpr bool contains(source_span outer, source_span inner) noexcept {
    const auto outer_end = static_cast<std::uint64_t>(outer.offset) + outer.length;
    const auto inner_end = static_cast<std::uint64_t>(inner.offset) + inner.length;
    return inner.offset >= outer.offset && inner_end <= outer_end;
}

[[nodiscard]] constexpr bool valid_range(source_fact_range range, std::size_t size) noexcept {
    return range.begin <= size && range.count <= size - range.begin;
}

[[nodiscard]] constexpr bool valid_intrinsic(intrinsic_type value) noexcept {
    return value > intrinsic_type::none && value <= intrinsic_type::nullptr_type;
}

[[nodiscard]] constexpr bool valid_record_kind(source_record_kind value) noexcept {
    return value >= source_record_kind::struct_type && value <= source_record_kind::union_type;
}

[[nodiscard]] constexpr bool valid_member_access(source_member_access value) noexcept {
    return value >= source_member_access::public_access && value <= source_member_access::private_access;
}

[[nodiscard]] constexpr bool valid_modifier_kind(source_type_modifier_kind value) noexcept {
    return value >= source_type_modifier_kind::const_qualified &&
           value <= source_type_modifier_kind::unbounded_array;
}

[[nodiscard]] constexpr std::string_view category_name(source_fact_category category) noexcept {
    switch (category) {
    case source_fact_category::packet:
        return "packet";
    case source_fact_category::namespace_fact:
        return "namespace";
    case source_fact_category::record_fact:
        return "record";
    case source_fact_category::member_fact:
        return "member";
    case source_fact_category::modifier_fact:
        return "modifier";
    case source_fact_category::enum_fact:
        return "enum";
    case source_fact_category::enum_value_fact:
        return "enum_value";
    case source_fact_category::object_fact:
        return "object";
    case source_fact_category::link_fact:
        return "link";
    case source_fact_category::base_fact:
        return "base";
    case source_fact_category::alias_fact:
        return "alias";
    case source_fact_category::method_fact:
        return "method";
    case source_fact_category::declaration_ref:
        return "declaration";
    }
    return "unknown";
}

[[nodiscard]] status fail(
    source_facts_validation_error& error,
    source_facts_error_code code,
    source_fact_category category,
    std::size_t index,
    source_span location = {}) noexcept {

    error.code = code;
    error.category = category;
    error.index = index > std::numeric_limits<std::uint32_t>::max()
        ? std::numeric_limits<std::uint32_t>::max()
        : static_cast<std::uint32_t>(index);
    error.location = location;
    return status{status_code::invalid_argument};
}

} // namespace

status validate_source_facts(
    const source_facts& facts,
    source_facts_validation_error& error) noexcept {

    error = {};

    if (!facts.source())
        return fail(error, source_facts_error_code::invalid_source, source_fact_category::packet, 0);

    const auto source_size = facts.source_text().size();
    if (source_size > std::numeric_limits<std::uint32_t>::max())
        return fail(error, source_facts_error_code::source_too_large, source_fact_category::packet, 0);

    const auto namespaces = facts.namespaces();
    std::uint32_t previous_namespace_offset = 0;
    for (std::size_t index = 0; index < namespaces.size(); ++index) {
        const auto& item = namespaces[index];
        if (item.identity == nullptr) {
            return fail(error, source_facts_error_code::namespace_identity_missing,
                source_fact_category::namespace_fact, index, item.declaration);
        }
        if (item.identity.kind() != identity_kind::namespace_scope) {
            return fail(error, source_facts_error_code::namespace_identity_kind,
                source_fact_category::namespace_fact, index, item.declaration);
        }
        if (item.declaration.length == 0 || !valid_span(item.declaration, source_size)) {
            return fail(error, source_facts_error_code::namespace_range,
                source_fact_category::namespace_fact, index, item.declaration);
        }
        if (index != 0 && item.declaration.offset < previous_namespace_offset) {
            return fail(error, source_facts_error_code::namespace_order,
                source_fact_category::namespace_fact, index, item.declaration);
        }
        previous_namespace_offset = item.declaration.offset;
    }

    const auto records = facts.records();
    const auto members = facts.members();
    const auto bases = facts.bases();
    const auto methods = facts.methods();
    const auto method_modifiers = facts.method_modifiers();
    std::size_t expected_member_begin = 0;
    std::size_t expected_base_begin = 0;
    std::size_t expected_method_begin = 0;
    std::uint32_t previous_record_offset = 0;

    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto& item = records[index];
        if (item.identity == nullptr) {
            return fail(error, source_facts_error_code::record_identity_missing,
                source_fact_category::record_fact, index, item.declaration);
        }
        if (item.identity.kind() != identity_kind::type) {
            return fail(error, source_facts_error_code::record_identity_kind,
                source_fact_category::record_fact, index, item.declaration);
        }
        if (!valid_record_kind(item.record_kind)) {
            return fail(error, source_facts_error_code::record_kind,
                source_fact_category::record_fact, index, item.declaration);
        }
        if (item.declaration.length == 0 || !valid_span(item.declaration, source_size)) {
            return fail(error, source_facts_error_code::record_range,
                source_fact_category::record_fact, index, item.declaration);
        }
        if (index != 0 && item.declaration.offset < previous_record_offset) {
            return fail(error, source_facts_error_code::record_order,
                source_fact_category::record_fact, index, item.declaration);
        }
        previous_record_offset = item.declaration.offset;
        if (!valid_range(item.members, members.size())) {
            return fail(error, source_facts_error_code::record_member_range,
                source_fact_category::record_fact, index, item.declaration);
        }

        if (item.members.begin != expected_member_begin) {
            return fail(error, source_facts_error_code::member_partition,
                source_fact_category::record_fact, index, item.declaration);
        }
        if (!valid_range(item.bases, bases.size())) {
            return fail(error, source_facts_error_code::record_base_range,
                source_fact_category::record_fact, index, item.declaration);
        }
        if (item.bases.begin != expected_base_begin) {
            return fail(error, source_facts_error_code::base_partition,
                source_fact_category::record_fact, index, item.declaration);
        }
        if (!valid_range(item.methods, methods.size())) {
            return fail(error, source_facts_error_code::method_range,
                source_fact_category::record_fact, index, item.declaration);
        }
        if (item.methods.begin != expected_method_begin) {
            return fail(error, source_facts_error_code::method_partition,
                source_fact_category::record_fact, index, item.declaration);
        }

        if (item.declaration_kind == source_record_declaration_kind::declaration) {
            if (item.members.count != 0) {
                return fail(error, source_facts_error_code::declaration_has_members,
                    source_fact_category::record_fact, index, item.declaration);
            }
            if (item.bases.count != 0) {
                return fail(error, source_facts_error_code::declaration_has_bases,
                    source_fact_category::record_fact, index, item.declaration);
            }
            if (item.methods.count != 0) {
                return fail(error, source_facts_error_code::declaration_has_methods,
                    source_fact_category::record_fact, index, item.declaration);
            }
            continue;
        }

        if (item.declaration_kind != source_record_declaration_kind::definition) {
            return fail(error, source_facts_error_code::record_declaration_kind,
                source_fact_category::record_fact, index, item.declaration);
        }

        expected_member_begin += item.members.count;
        expected_base_begin += item.bases.count;
        expected_method_begin += item.methods.count;

        const auto member_end = static_cast<std::size_t>(item.members.begin) + item.members.count;
        std::uint32_t previous_member_offset = 0;
        for (std::size_t member_index = item.members.begin; member_index < member_end; ++member_index) {
            if (!contains(item.declaration, members[member_index].declaration)) {
                return fail(error, source_facts_error_code::member_range,
                    source_fact_category::member_fact, member_index, members[member_index].declaration);
            }
            if (member_index != item.members.begin &&
                members[member_index].declaration.offset < previous_member_offset) {
                return fail(error, source_facts_error_code::member_order,
                    source_fact_category::member_fact, member_index, members[member_index].declaration);
            }
            previous_member_offset = members[member_index].declaration.offset;
        }

        const auto method_end = static_cast<std::size_t>(item.methods.begin) + item.methods.count;
        for (std::size_t method_index = item.methods.begin; method_index < method_end; ++method_index) {
            if (!contains(item.declaration, methods[method_index].declaration)) {
                return fail(error, source_facts_error_code::method_range,
                    source_fact_category::method_fact, method_index, methods[method_index].declaration);
            }
        }
    }

    if (expected_member_begin != members.size()) {
        return fail(error, source_facts_error_code::member_partition,
            source_fact_category::packet, expected_member_begin);
    }
    if (expected_base_begin != bases.size()) {
        return fail(error, source_facts_error_code::base_partition,
            source_fact_category::packet, expected_base_begin);
    }
    if (expected_method_begin != methods.size()) {
        return fail(error, source_facts_error_code::method_partition,
            source_fact_category::packet, expected_method_begin);
    }

    std::size_t expected_method_modifier_begin = 0;
    std::uint32_t previous_method_offset = 0;
    for (std::size_t index = 0; index < methods.size(); ++index) {
        const auto& item = methods[index];
        if (!item.name) {
            return fail(error, source_facts_error_code::method_name_empty,
                source_fact_category::method_fact, index, item.declaration);
        }
        if (!valid_member_access(item.access)) {
            return fail(error, source_facts_error_code::method_access,
                source_fact_category::method_fact, index, item.declaration);
        }
        if (item.declaration.length == 0 || !valid_span(item.declaration, source_size)) {
            return fail(error, source_facts_error_code::method_range,
                source_fact_category::method_fact, index, item.declaration);
        }
        if (index != 0 && item.declaration.offset < previous_method_offset) {
            return fail(error, source_facts_error_code::method_order,
                source_fact_category::method_fact, index, item.declaration);
        }
        previous_method_offset = item.declaration.offset;
        const bool has_identity = item.return_type.identity != nullptr;
        const bool has_intrinsic = item.return_type.intrinsic != intrinsic_type::none;
        const bool no_return = !has_identity && !has_intrinsic;
        if (no_return && !item.is_constructor && !item.is_destructor) {
            return fail(error, source_facts_error_code::unresolved_type_base,
                source_fact_category::method_fact, index, item.declaration);
        }
        if (has_identity && has_intrinsic) {
            return fail(error, source_facts_error_code::ambiguous_type_base,
                source_fact_category::method_fact, index, item.declaration);
        }
        if (has_identity && item.return_type.identity.kind() != identity_kind::type) {
            return fail(error, source_facts_error_code::semantic_type_identity_kind,
                source_fact_category::method_fact, index, item.declaration);
        }
        if (has_intrinsic && !valid_intrinsic(item.return_type.intrinsic)) {
            return fail(error, source_facts_error_code::intrinsic_type_code,
                source_fact_category::method_fact, index, item.declaration);
        }
        if (!valid_range(item.return_type.modifiers, method_modifiers.size()) ||
            item.return_type.modifiers.begin != expected_method_modifier_begin) {
            return fail(error, source_facts_error_code::method_modifier_partition,
                source_fact_category::method_fact, index, item.declaration);
        }
        expected_method_modifier_begin += item.return_type.modifiers.count;
    }
    if (expected_method_modifier_begin != method_modifiers.size()) {
        return fail(error, source_facts_error_code::method_modifier_partition,
            source_fact_category::packet, expected_method_modifier_begin);
    }

    for (std::size_t index = 0; index < bases.size(); ++index) {
        const auto& item = bases[index];
        if (item.base == nullptr) {
            return fail(error, source_facts_error_code::base_identity_missing,
                source_fact_category::base_fact, index, item.declaration);
        }
        if (item.base.kind() != identity_kind::type) {
            return fail(error, source_facts_error_code::base_identity_kind,
                source_fact_category::base_fact, index, item.declaration);
        }
        if (!valid_member_access(item.access)) {
            return fail(error, source_facts_error_code::base_access,
                source_fact_category::base_fact, index, item.declaration);
        }
        if (item.declaration.length == 0 || !valid_span(item.declaration, source_size)) {
            return fail(error, source_facts_error_code::base_range,
                source_fact_category::base_fact, index, item.declaration);
        }
    }

    const auto modifiers = facts.modifiers();

    for (std::size_t index = 0; index < members.size(); ++index) {
        const auto& item = members[index];

        if (!valid_member_access(item.access)) {
            return fail(error, source_facts_error_code::member_access,
                source_fact_category::member_fact, index, item.declaration);
        }
        if (!item.name) {
            return fail(error, source_facts_error_code::member_name_empty,
                source_fact_category::member_fact, index, item.declaration);
        }
        if (item.declaration.length == 0 || !valid_span(item.declaration, source_size)) {
            return fail(error, source_facts_error_code::member_range,
                source_fact_category::member_fact, index, item.declaration);
        }
        if (item.initializer.length != 0 &&
            (!valid_span(item.initializer, source_size) || !contains(item.declaration, item.initializer))) {
            return fail(error, source_facts_error_code::member_initializer_range,
                source_fact_category::member_fact, index, item.initializer);
        }
        if (item.type.spelling.length == 0 || !valid_span(item.type.spelling, source_size)) {
            return fail(error, source_facts_error_code::type_spelling_range,
                source_fact_category::member_fact, index, item.type.spelling);
        }
        if (!contains(item.declaration, item.type.spelling)) {
            return fail(error, source_facts_error_code::type_spelling_outside_member,
                source_fact_category::member_fact, index, item.type.spelling);
        }

        const bool has_identity = item.type.identity != nullptr;
        const bool has_intrinsic = item.type.intrinsic != intrinsic_type::none;
        if (!has_identity && !has_intrinsic) {
            return fail(error, source_facts_error_code::unresolved_type_base,
                source_fact_category::member_fact, index, item.type.spelling);
        }
        if (has_identity && has_intrinsic) {
            return fail(error, source_facts_error_code::ambiguous_type_base,
                source_fact_category::member_fact, index, item.type.spelling);
        }
        if (has_identity && item.type.identity.kind() != identity_kind::type) {
            return fail(error, source_facts_error_code::semantic_type_identity_kind,
                source_fact_category::member_fact, index, item.type.spelling);
        }
        if (has_intrinsic && !valid_intrinsic(item.type.intrinsic)) {
            return fail(error, source_facts_error_code::intrinsic_type_code,
                source_fact_category::member_fact, index, item.type.spelling);
        }
        if (!valid_range(item.type.modifiers, modifiers.size())) {
            return fail(error, source_facts_error_code::modifier_partition,
                source_fact_category::member_fact, index, item.type.spelling);
        }
    }

    for (std::size_t index = 0; index < modifiers.size(); ++index) {
        const auto& item = modifiers[index];
        if (!valid_modifier_kind(item.kind)) {
            return fail(error, source_facts_error_code::modifier_kind,
                source_fact_category::modifier_fact, index);
        }

        const bool bounded_array = item.kind == source_type_modifier_kind::bounded_array;
        if ((bounded_array && item.value == 0) || (!bounded_array && item.value != 0)) {
            return fail(error, source_facts_error_code::modifier_value,
                source_fact_category::modifier_fact, index);
        }
    }


    const auto enum_values = facts.enum_values();
    const auto enums = facts.enums();
    std::size_t expected_enum_value_begin = 0;
    std::uint32_t previous_enum_offset = 0;
    for (std::size_t index = 0; index < enums.size(); ++index) {
        const auto& item = enums[index];
        if (item.identity == nullptr) {
            return fail(error, source_facts_error_code::enum_identity_missing,
                source_fact_category::enum_fact, index, item.declaration);
        }
        if (item.identity.kind() != identity_kind::type) {
            return fail(error, source_facts_error_code::enum_identity_kind,
                source_fact_category::enum_fact, index, item.declaration);
        }
        if (item.declaration.length == 0 || !valid_span(item.declaration, source_size)) {
            return fail(error, source_facts_error_code::enum_range,
                source_fact_category::enum_fact, index, item.declaration);
        }
        if (index != 0 && item.declaration.offset < previous_enum_offset) {
            return fail(error, source_facts_error_code::enum_order,
                source_fact_category::enum_fact, index, item.declaration);
        }
        previous_enum_offset = item.declaration.offset;
        if (!valid_range(item.enumerators, enum_values.size())) {
            return fail(error, source_facts_error_code::enum_enumerator_range,
                source_fact_category::enum_fact, index, item.declaration);
        }
        if (item.enumerators.begin != expected_enum_value_begin) {
            return fail(error, source_facts_error_code::enum_value_partition,
                source_fact_category::enum_fact, index, item.declaration);
        }
        if (item.explicit_underlying != intrinsic_type::none &&
            (!valid_intrinsic(item.explicit_underlying) ||
             item.explicit_underlying >= intrinsic_type::float_type)) {
            return fail(error, source_facts_error_code::enum_underlying_type,
                source_fact_category::enum_fact, index, item.underlying_spelling);
        }
        if (item.declaration_kind == source_enum_declaration_kind::declaration) {
            if (item.enumerators.count != 0) {
                return fail(error, source_facts_error_code::enum_declaration_has_values,
                    source_fact_category::enum_fact, index, item.declaration);
            }
            continue;
        }
        if (item.declaration_kind != source_enum_declaration_kind::definition) {
            return fail(error, source_facts_error_code::enum_declaration_kind,
                source_fact_category::enum_fact, index, item.declaration);
        }
        expected_enum_value_begin += item.enumerators.count;
        const auto end = static_cast<std::size_t>(item.enumerators.begin) + item.enumerators.count;
        for (std::size_t value_index = item.enumerators.begin; value_index < end; ++value_index) {
            const auto& value = enum_values[value_index];
            if (!value.name) {
                return fail(error, source_facts_error_code::enum_value_name_range,
                    source_fact_category::enum_value_fact, value_index, item.declaration);
            }
            if (value.expression.length != 0 &&
                (!valid_span(value.expression, source_size) || !contains(item.declaration, value.expression))) {
                return fail(error, source_facts_error_code::enum_value_expression_range,
                    source_fact_category::enum_value_fact, value_index, value.expression);
            }
            if (!valid_intrinsic(value.value.intrinsic) ||
                value.value.intrinsic >= intrinsic_type::float_type) {
                return fail(error, source_facts_error_code::enum_value_type,
                    source_fact_category::enum_value_fact, value_index, value.expression);
            }
        }
    }
    if (expected_enum_value_begin != enum_values.size()) {
        return fail(error, source_facts_error_code::enum_value_partition,
            source_fact_category::packet, expected_enum_value_begin);
    }

    const auto objects = facts.objects();
    std::uint32_t previous_object_offset = 0;
    for (std::size_t index = 0; index < objects.size(); ++index) {
        const auto& item = objects[index];
        if (item.identity == nullptr) {
            return fail(error, source_facts_error_code::object_identity_missing,
                source_fact_category::object_fact, index, item.declaration);
        }
        if (item.identity.kind() != identity_kind::object) {
            return fail(error, source_facts_error_code::object_identity_kind,
                source_fact_category::object_fact, index, item.declaration);
        }
        if (item.declaration.length == 0 || !valid_span(item.declaration, source_size)) {
            return fail(error, source_facts_error_code::object_range,
                source_fact_category::object_fact, index, item.declaration);
        }
        if (item.initializer.length != 0 &&
            (!valid_span(item.initializer, source_size) || !contains(item.declaration, item.initializer))) {
            return fail(error, source_facts_error_code::object_initializer_range,
                source_fact_category::object_fact, index, item.initializer);
        }
        if (index != 0 && item.declaration.offset < previous_object_offset) {
            return fail(error, source_facts_error_code::declaration_sequence_order,
                source_fact_category::object_fact, index, item.declaration);
        }
        previous_object_offset = item.declaration.offset;
        const bool semantic_type = item.type.identity != nullptr &&
            item.type.identity.kind() == identity_kind::type &&
            item.type.intrinsic == intrinsic_type::none;
        const bool intrinsic_type_value = item.type.identity == nullptr &&
            item.type.intrinsic != intrinsic_type::none;
        if (!semantic_type && !intrinsic_type_value) {
            return fail(error, source_facts_error_code::object_type,
                source_fact_category::object_fact, index, item.declaration);
        }
        if (item.type.modifiers.begin > facts.modifiers().size() ||
            item.type.modifiers.count > facts.modifiers().size() - item.type.modifiers.begin) {
            return fail(error, source_facts_error_code::object_type,
                source_fact_category::object_fact, index, item.declaration);
        }
    }

    // Member and object type modifiers share one dense array in declaration
    // order (objects historically appended alongside members). The merged
    // walk keeps the exact-partition guarantee across both arrays; ties
    // (multi-declarators share one offset) resolve in array order.
    {
        std::size_t member_position = 0;
        std::size_t object_position = 0;
        std::size_t expected_begin = 0;
        while (member_position < members.size() || object_position < objects.size()) {
            bool take_member = false;
            if (member_position < members.size() && object_position < objects.size()) {
                take_member = members[member_position].declaration.offset <=
                    objects[object_position].declaration.offset;
            }
            else {
                take_member = member_position < members.size();
            }
            const auto& range = take_member
                ? members[member_position].type.modifiers
                : objects[object_position].type.modifiers;
            const auto& declaration = take_member
                ? members[member_position].declaration
                : objects[object_position].declaration;
            if (range.begin != expected_begin) {
                return fail(error, source_facts_error_code::modifier_partition,
                    take_member ? source_fact_category::member_fact : source_fact_category::object_fact,
                    take_member ? member_position : object_position, declaration);
            }
            expected_begin += range.count;
            if (take_member)
                ++member_position;
            else
                ++object_position;
        }
        if (expected_begin != modifiers.size()) {
            return fail(error, source_facts_error_code::modifier_partition,
                source_fact_category::packet, expected_begin);
        }
    }

    const auto links = facts.links();
    std::uint32_t previous_link_offset = 0;
    for (std::size_t index = 0; index < links.size(); ++index) {
        const auto& item = links[index];
        if (item.declaration.length == 0 || !valid_span(item.declaration, source_size)) {
            return fail(error, source_facts_error_code::link_range,
                source_fact_category::link_fact, index, item.declaration);
        }
        if (index != 0 && item.declaration.offset < previous_link_offset) {
            return fail(error, source_facts_error_code::declaration_sequence_order,
                source_fact_category::link_fact, index, item.declaration);
        }
        previous_link_offset = item.declaration.offset;
        if (item.source.object == nullptr || item.source.object.kind() != identity_kind::object ||
            !item.source.member || item.target.object == nullptr ||
            item.target.object.kind() != identity_kind::object || !item.target.member) {
            return fail(error, source_facts_error_code::link_endpoint,
                source_fact_category::link_fact, index, item.declaration);
        }
    }

    const auto aliases = facts.aliases();
    const auto alias_modifiers = facts.alias_modifiers();
    std::size_t expected_alias_modifier_begin = 0;
    std::uint32_t previous_alias_offset = 0;
    for (std::size_t index = 0; index < aliases.size(); ++index) {
        const auto& item = aliases[index];
        if (item.identity == nullptr) {
            return fail(error, source_facts_error_code::alias_identity_missing,
                source_fact_category::alias_fact, index, item.declaration);
        }
        if (item.identity.kind() != identity_kind::type) {
            return fail(error, source_facts_error_code::alias_identity_kind,
                source_fact_category::alias_fact, index, item.declaration);
        }
        if (item.declaration.length == 0 || !valid_span(item.declaration, source_size)) {
            return fail(error, source_facts_error_code::alias_range,
                source_fact_category::alias_fact, index, item.declaration);
        }
        if (index != 0 && item.declaration.offset < previous_alias_offset) {
            return fail(error, source_facts_error_code::alias_order,
                source_fact_category::alias_fact, index, item.declaration);
        }
        previous_alias_offset = item.declaration.offset;
        const bool has_identity = item.target.identity != nullptr;
        const bool has_intrinsic = item.target.intrinsic != intrinsic_type::none;
        if (!has_identity && !has_intrinsic) {
            return fail(error, source_facts_error_code::unresolved_type_base,
                source_fact_category::alias_fact, index, item.declaration);
        }
        if (has_identity && has_intrinsic) {
            return fail(error, source_facts_error_code::ambiguous_type_base,
                source_fact_category::alias_fact, index, item.declaration);
        }
        if (has_identity && item.target.identity.kind() != identity_kind::type) {
            return fail(error, source_facts_error_code::semantic_type_identity_kind,
                source_fact_category::alias_fact, index, item.declaration);
        }
        if (has_intrinsic && !valid_intrinsic(item.target.intrinsic)) {
            return fail(error, source_facts_error_code::intrinsic_type_code,
                source_fact_category::alias_fact, index, item.declaration);
        }
        if (!valid_range(item.target.modifiers, alias_modifiers.size()) ||
            item.target.modifiers.begin != expected_alias_modifier_begin) {
            return fail(error, source_facts_error_code::alias_modifier_partition,
                source_fact_category::alias_fact, index, item.declaration);
        }
        expected_alias_modifier_begin += item.target.modifiers.count;
    }
    if (expected_alias_modifier_begin != alias_modifiers.size()) {
        return fail(error, source_facts_error_code::alias_modifier_partition,
            source_fact_category::packet, expected_alias_modifier_begin);
    }

    const auto declarations = facts.declarations();
    if (!declarations.empty()) {
        std::size_t next_namespace = 0;
        std::size_t next_record = 0;
        std::size_t next_enum = 0;
        std::size_t next_object = 0;
        std::size_t next_link = 0;
        std::size_t next_alias = 0;
        std::uint32_t previous_offset = 0;
        for (std::size_t index = 0; index < declarations.size(); ++index) {
            const auto& item = declarations[index];
            if (index != 0 && item.declaration.offset < previous_offset) {
                return fail(error, source_facts_error_code::declaration_sequence_order,
                    source_fact_category::declaration_ref, index, item.declaration);
            }
            previous_offset = item.declaration.offset;
            source_span expected{};
            switch (item.kind) {
            case source_declaration_kind::namespace_scope:
                if (item.index != next_namespace || next_namespace >= namespaces.size()) {
                    return fail(error, source_facts_error_code::declaration_sequence_index,
                        source_fact_category::declaration_ref, index, item.declaration);
                }
                expected = namespaces[next_namespace++].declaration;
                break;
            case source_declaration_kind::record_type:
                if (item.index != next_record || next_record >= records.size()) {
                    return fail(error, source_facts_error_code::declaration_sequence_index,
                        source_fact_category::declaration_ref, index, item.declaration);
                }
                expected = records[next_record++].declaration;
                break;
            case source_declaration_kind::enum_type:
                if (item.index != next_enum || next_enum >= enums.size()) {
                    return fail(error, source_facts_error_code::declaration_sequence_index,
                        source_fact_category::declaration_ref, index, item.declaration);
                }
                expected = enums[next_enum++].declaration;
                break;
            case source_declaration_kind::object:
                if (item.index != next_object || next_object >= objects.size()) {
                    return fail(error, source_facts_error_code::declaration_sequence_index,
                        source_fact_category::declaration_ref, index, item.declaration);
                }
                expected = objects[next_object++].declaration;
                break;
            case source_declaration_kind::link:
                if (item.index != next_link || next_link >= links.size()) {
                    return fail(error, source_facts_error_code::declaration_sequence_index,
                        source_fact_category::declaration_ref, index, item.declaration);
                }
                expected = links[next_link++].declaration;
                break;
            case source_declaration_kind::alias:
                if (item.index != next_alias || next_alias >= aliases.size()) {
                    return fail(error, source_facts_error_code::declaration_sequence_index,
                        source_fact_category::declaration_ref, index, item.declaration);
                }
                expected = aliases[next_alias++].declaration;
                break;
            }
            if (expected.offset != item.declaration.offset || expected.length != item.declaration.length) {
                return fail(error, source_facts_error_code::declaration_sequence_range,
                    source_fact_category::declaration_ref, index, item.declaration);
            }
        }
        if (next_namespace != namespaces.size() || next_record != records.size() || next_enum != enums.size() ||
            next_object != objects.size() || next_link != links.size() || next_alias != aliases.size()) {
            return fail(error, source_facts_error_code::declaration_sequence_index,
                source_fact_category::packet, declarations.size());
        }
    }

    return {};
}

void emit_source_facts_validation_diagnostic(
    const source_facts& facts,
    const source_facts_validation_error& error,
    operation_id operation,
    diagnostic_buffer& output) {

    std::string detail{source_facts_error_detail(error.code)};
    detail.append("; fact=");
    detail.append(category_name(error.category));
    detail.push_back('[');
    detail.append(std::to_string(error.index));
    detail.push_back(']');

    output.emit(diagnostic_record{
        diagnostics::parser_invalid_source_facts.id,
        diagnostics::parser_invalid_source_facts.default_severity,
        operation,
        source_range{facts.source(), error.location.offset, error.location.length},
        std::move(detail),
    });
}

} // namespace cw::server
