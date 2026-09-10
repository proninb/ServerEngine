#pragma once

#include "source_facts.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"
#include "../../operation.hpp"
#include "../../status.hpp"

#include <cstdint>
#include <string_view>

namespace cw::server {

enum class source_facts_error_code : std::uint8_t {
    none = 0,
    invalid_source,
    source_too_large,
    namespace_identity_missing,
    namespace_identity_kind,
    namespace_range,
    namespace_order,
    record_identity_missing,
    record_identity_kind,
    record_declaration_kind,
    record_kind,
    record_range,
    record_order,
    record_member_range,
    declaration_has_members,
    member_partition,
    member_access,
    member_name_empty,
    member_name_range,
    member_range,
    member_order,
    member_name_outside_declaration,
    type_spelling_range,
    type_spelling_outside_member,
    unresolved_type_base,
    ambiguous_type_base,
    semantic_type_identity_kind,
    intrinsic_type_code,
    modifier_partition,
    modifier_kind,
    modifier_value,
    enum_identity_missing,
    enum_identity_kind,
    enum_declaration_kind,
    enum_range,
    enum_order,
    enum_enumerator_range,
    enum_declaration_has_values,
    enum_value_partition,
    enum_underlying_type,
    enum_value_name_range,
    enum_value_expression_range,
    enum_value_type,
    object_identity_missing,
    object_identity_kind,
    object_range,
    object_type,
    link_endpoint,
    link_range,
    declaration_sequence_index,
    declaration_sequence_order,
    declaration_sequence_range,
};

enum class source_fact_category : std::uint8_t {
    packet,
    namespace_fact,
    record_fact,
    member_fact,
    modifier_fact,
    enum_fact,
    enum_value_fact,
    object_fact,
    link_fact,
    declaration_ref,
};

// Pinpoints one malformed Parser -> Builder contract record without carrying strings
// or performing semantic resolution. index is relative to the indicated flat fact array.
struct source_facts_validation_error {
    source_facts_error_code code = source_facts_error_code::none;
    source_fact_category category = source_fact_category::packet;
    std::uint32_t index = 0;
    source_span location{};
};

[[nodiscard]] status validate_source_facts(
    const source_facts& facts,
    source_facts_validation_error& error) noexcept;

[[nodiscard]] constexpr std::string_view source_facts_error_detail(
    source_facts_error_code code) noexcept {

    switch (code) {
    case source_facts_error_code::none:
        return "source facts are valid";
    case source_facts_error_code::invalid_source:
        return "source_facts carries an invalid source_id";
    case source_facts_error_code::source_too_large:
        return "source text exceeds the 32-bit source-range contract";
    case source_facts_error_code::namespace_identity_missing:
        return "namespace fact has no resolved identity";
    case source_facts_error_code::namespace_identity_kind:
        return "namespace fact identity is not a namespace identity";
    case source_facts_error_code::namespace_range:
        return "namespace declaration range is outside the Source snapshot";
    case source_facts_error_code::namespace_order:
        return "namespace facts are not in source declaration order";
    case source_facts_error_code::record_identity_missing:
        return "record fact has no resolved identity";
    case source_facts_error_code::record_identity_kind:
        return "record fact identity is not a type identity";
    case source_facts_error_code::record_declaration_kind:
        return "record fact has an unsupported declaration kind";
    case source_facts_error_code::record_kind:
        return "record fact has an unsupported record kind";
    case source_facts_error_code::record_range:
        return "record declaration range is outside the Source snapshot";
    case source_facts_error_code::record_order:
        return "record facts are not in source declaration order";
    case source_facts_error_code::record_member_range:
        return "record member range is outside the flat member array";
    case source_facts_error_code::declaration_has_members:
        return "record declaration carries definition members";
    case source_facts_error_code::member_partition:
        return "record definitions do not form an exact lexical partition of members";
    case source_facts_error_code::member_access:
        return "member fact has an unsupported access value";
    case source_facts_error_code::member_name_empty:
        return "member fact has an empty source name";
    case source_facts_error_code::member_name_range:
        return "member name range is outside the Source snapshot";
    case source_facts_error_code::member_range:
        return "member declaration range is outside the Source snapshot";
    case source_facts_error_code::member_order:
        return "record members are not in source declaration order";
    case source_facts_error_code::member_name_outside_declaration:
        return "member name is outside its declaration range";
    case source_facts_error_code::type_spelling_range:
        return "member type spelling range is outside the Source snapshot";
    case source_facts_error_code::type_spelling_outside_member:
        return "member type spelling is outside its declaration range";
    case source_facts_error_code::unresolved_type_base:
        return "type reference has no resolved identity or intrinsic type";
    case source_facts_error_code::ambiguous_type_base:
        return "type reference carries both semantic identity and intrinsic type";
    case source_facts_error_code::semantic_type_identity_kind:
        return "type reference identity is not a type identity";
    case source_facts_error_code::intrinsic_type_code:
        return "type reference has an invalid intrinsic type code";
    case source_facts_error_code::modifier_partition:
        return "member type modifiers do not form an exact dense partition";
    case source_facts_error_code::modifier_kind:
        return "type modifier has an unsupported modifier kind";
    case source_facts_error_code::modifier_value:
        return "type modifier payload violates its structural contract";
    case source_facts_error_code::enum_identity_missing:
        return "enum fact has no resolved identity";
    case source_facts_error_code::enum_identity_kind:
        return "enum fact identity is not a type identity";
    case source_facts_error_code::enum_declaration_kind:
        return "enum fact has an unsupported declaration kind";
    case source_facts_error_code::enum_range:
        return "enum declaration range is outside the Source snapshot";
    case source_facts_error_code::enum_order:
        return "enum facts are not in source declaration order";
    case source_facts_error_code::enum_enumerator_range:
        return "enum enumerator range is outside the flat enum-value array";
    case source_facts_error_code::enum_declaration_has_values:
        return "enum declaration carries definition enumerators";
    case source_facts_error_code::enum_value_partition:
        return "enum definitions do not form an exact lexical partition of enum values";
    case source_facts_error_code::enum_underlying_type:
        return "enum explicit underlying type is not an integral intrinsic type";
    case source_facts_error_code::enum_value_name_range:
        return "enum value name range is outside the Source snapshot";
    case source_facts_error_code::enum_value_expression_range:
        return "enum value expression range is outside the Source snapshot";
    case source_facts_error_code::enum_value_type:
        return "enum value carries a non-integral intrinsic type";
    case source_facts_error_code::object_identity_missing:
        return "object fact has no resolved identity";
    case source_facts_error_code::object_identity_kind:
        return "object fact identity is not an object identity";
    case source_facts_error_code::object_range:
        return "object declaration range is outside the Source snapshot";
    case source_facts_error_code::object_type:
        return "object type must be one resolved unmodified named Project type";
    case source_facts_error_code::link_endpoint:
        return "link endpoint is not a resolved object/member pair";
    case source_facts_error_code::link_range:
        return "link declaration range is outside the Source snapshot";
    case source_facts_error_code::declaration_sequence_index:
        return "declaration sequence does not reference each fact exactly once in per-kind order";
    case source_facts_error_code::declaration_sequence_order:
        return "declaration sequence is not in lexical source order";
    case source_facts_error_code::declaration_sequence_range:
        return "declaration sequence range does not match the referenced fact";
    }
    return "source facts validation failed";
}

void emit_source_facts_validation_diagnostic(
    const source_facts& facts,
    const source_facts_validation_error& error,
    operation_id operation,
    diagnostic_buffer& diagnostics);

} // namespace cw::server
