#pragma once

#include "../../member_index.hpp"
#include "../../source_id.hpp"
#include "../../string_id.hpp"
#include "../identity/identity_node.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace cw::server {

struct source_span final {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;

    [[nodiscard]] constexpr bool empty() const noexcept { return length == 0; }
};

struct source_fact_range final {
    std::uint32_t begin = 0;
    std::uint32_t count = 0;
};

enum class intrinsic_type : std::uint8_t {
    none = 0,
    void_type,
    bool_type,
    char_type,
    signed_char,
    unsigned_char,
    wchar_type,
    char8_type,
    char16_type,
    char32_type,
    signed_short,
    unsigned_short,
    signed_int,
    unsigned_int,
    signed_long,
    unsigned_long,
    signed_long_long,
    unsigned_long_long,
    float_type,
    double_type,
    long_double_type,
    nullptr_type,
};

enum class source_type_modifier_kind : std::uint8_t {
    const_qualified,
    volatile_qualified,
    pointer,
    lvalue_reference,
    rvalue_reference,
    bounded_array,
    unbounded_array,
};

// Declarator transformation applied from base type outward, left to right.
struct source_type_modifier final {
    std::uint64_t value = 0;
    source_type_modifier_kind kind = source_type_modifier_kind::pointer;
    std::uint8_t reserved[7]{};
};

// Fully resolved source-language type reference. Exactly one base representation
// is present: project semantic identity or intrinsic Language/ABI type code.
struct source_type_ref final {
    identity_ref identity = nullptr;
    intrinsic_type intrinsic = intrinsic_type::none;
    source_fact_range modifiers{};
    source_span spelling{};

    [[nodiscard]] static constexpr source_type_ref semantic(
        identity_ref base,
        source_fact_range modifier_range,
        source_span source_spelling) noexcept {
        return source_type_ref{base, intrinsic_type::none, modifier_range, source_spelling};
    }

    [[nodiscard]] static constexpr source_type_ref builtin(
        intrinsic_type base,
        source_fact_range modifier_range,
        source_span source_spelling) noexcept {
        return source_type_ref{nullptr, base, modifier_range, source_spelling};
    }
};

struct source_namespace_fact final {
    identity_ref identity = nullptr;
    source_span declaration{};
};

enum class source_record_declaration_kind : std::uint8_t {
    declaration,
    definition,
};

enum class source_record_kind : std::uint8_t {
    struct_type,
    class_type,
    union_type,
};

struct source_record_fact final {
    identity_ref identity = nullptr;
    source_fact_range members{};
    source_fact_range bases{};
    source_span declaration{};
    source_record_declaration_kind declaration_kind = source_record_declaration_kind::declaration;
    source_record_kind record_kind = source_record_kind::struct_type;
};

enum class source_member_access : std::uint8_t {
    public_access,
    protected_access,
    private_access,
};

struct source_member_fact final {
    source_type_ref type{};
    string_id name{};
    source_span declaration{};
    source_member_access access = source_member_access::public_access;
};

// One direct base-class specifier. Recorded by the Parser and validated, but
// not yet consumed by Generation Builder/layout (ABI stage): contributions
// intentionally exclude bases until layout consumes them.
struct source_base_fact final {
    identity_ref base = nullptr;
    source_member_access access = source_member_access::public_access;
    source_span declaration{};
    bool is_virtual = false;
};

// One `using Name = Type` / `typedef Type Name` alias. The target is fully
// resolved at parse time (identity or intrinsic plus modifiers); use sites
// copy the target modifiers into their own type ranges. Aliases resolve
// within the defining source only and are not exported through interfaces,
// consumed by the Builder, or stored in contributions (same slice rule as
// base classes: recorded and validated, consumed later).
struct source_alias_fact final {
    identity_ref identity = nullptr;
    source_type_ref target{};
    source_span declaration{};
};

// Parser-interpreted integer value. bits are the raw value representation and
// intrinsic identifies the source-language integer category used by later ABI work.
struct source_integral_constant final {
    intrinsic_type intrinsic = intrinsic_type::signed_int;
    std::uint64_t bits = 0;
};

struct source_enum_value_fact final {
    string_id name{};
    source_integral_constant value{};
    source_span expression{};
};

enum class source_enum_declaration_kind : std::uint8_t {
    declaration,
    definition,
};

struct source_enum_fact final {
    identity_ref identity = nullptr;
    source_fact_range enumerators{};
    source_span declaration{};
    source_span underlying_spelling{};
    intrinsic_type explicit_underlying = intrinsic_type::none;
    source_enum_declaration_kind declaration_kind = source_enum_declaration_kind::declaration;
    bool scoped = false;
};


// One named Project object. Object identity is semantic WHO; its type remains a
// fully resolved Parser TypeRef and is materialized to Graph TypeRef by Builder.
struct source_object_fact final {
    identity_ref identity = nullptr;
    source_type_ref type{};
    source_span declaration{};
};

struct source_object_endpoint_fact final {
    identity_ref object = nullptr;
    member_index member{};
};

// Assignment syntax `target.member = source.member;` becomes one directed semantic
// link from source endpoint to target endpoint. No textual lookup survives Parser.
struct source_link_fact final {
    source_object_endpoint_fact source{};
    source_object_endpoint_fact target{};
    source_span declaration{};
};

enum class source_declaration_kind : std::uint8_t {
    namespace_scope,
    record_type,
    enum_type,
    object,
    link,
    alias,
};

// Preserves total lexical declaration order across separate flat fact arrays so
// Generation Builder never sorts or reconstructs declaration ordering.
struct source_declaration_ref final {
    std::uint32_t index = 0;
    source_declaration_kind kind = source_declaration_kind::record_type;
    source_span declaration{};
};

// Immutable zero-allocation Parser -> Generation Builder boundary. Spans borrow
// immutable Source bytes; identity_ref values remain valid for Project lifetime.
class source_facts final {
public:
    constexpr source_facts(
        source_id source,
        std::string_view source_text,
        std::span<const source_namespace_fact> namespaces,
        std::span<const source_record_fact> records,
        std::span<const source_member_fact> members,
        std::span<const source_type_modifier> modifiers) noexcept
        : source_facts(source, source_text, namespaces, records, members, modifiers, {}, {}, {}, {}, {}, {}) {}

    constexpr source_facts(
        source_id source,
        std::string_view source_text,
        std::span<const source_namespace_fact> namespaces,
        std::span<const source_record_fact> records,
        std::span<const source_member_fact> members,
        std::span<const source_type_modifier> modifiers,
        std::span<const source_enum_fact> enums,
        std::span<const source_enum_value_fact> enum_values,
        std::span<const source_declaration_ref> declarations) noexcept
        : source_facts(source, source_text, namespaces, records, members, modifiers, enums,
              enum_values, declarations, {}, {}, {}) {}

    constexpr source_facts(
        source_id source,
        std::string_view source_text,
        std::span<const source_namespace_fact> namespaces,
        std::span<const source_record_fact> records,
        std::span<const source_member_fact> members,
        std::span<const source_type_modifier> modifiers,
        std::span<const source_enum_fact> enums,
        std::span<const source_enum_value_fact> enum_values,
        std::span<const source_declaration_ref> declarations,
        std::span<const source_object_fact> objects,
        std::span<const source_link_fact> links,
        std::span<const source_base_fact> bases = {},
        std::span<const source_alias_fact> aliases = {},
        std::span<const source_type_modifier> alias_modifiers = {}) noexcept
        : source_value(source),
          source_text_value(source_text),
          namespaces_value(namespaces),
          records_value(records),
          members_value(members),
          modifiers_value(modifiers),
          enums_value(enums),
          enum_values_value(enum_values),
          declarations_value(declarations),
          objects_value(objects),
          links_value(links),
          bases_value(bases),
          aliases_value(aliases),
          alias_modifiers_value(alias_modifiers) {}

    [[nodiscard]] constexpr source_id source() const noexcept { return source_value; }
    [[nodiscard]] constexpr std::string_view source_text() const noexcept { return source_text_value; }
    [[nodiscard]] constexpr std::span<const source_namespace_fact> namespaces() const noexcept { return namespaces_value; }
    [[nodiscard]] constexpr std::span<const source_record_fact> records() const noexcept { return records_value; }
    [[nodiscard]] constexpr std::span<const source_member_fact> members() const noexcept { return members_value; }
    [[nodiscard]] constexpr std::span<const source_type_modifier> modifiers() const noexcept { return modifiers_value; }
    [[nodiscard]] constexpr std::span<const source_enum_fact> enums() const noexcept { return enums_value; }
    [[nodiscard]] constexpr std::span<const source_enum_value_fact> enum_values() const noexcept { return enum_values_value; }
    [[nodiscard]] constexpr std::span<const source_declaration_ref> declarations() const noexcept { return declarations_value; }
    [[nodiscard]] constexpr std::span<const source_object_fact> objects() const noexcept { return objects_value; }
    [[nodiscard]] constexpr std::span<const source_link_fact> links() const noexcept { return links_value; }
    [[nodiscard]] constexpr std::span<const source_base_fact> bases() const noexcept { return bases_value; }
    [[nodiscard]] constexpr std::span<const source_alias_fact> aliases() const noexcept { return aliases_value; }
    [[nodiscard]] constexpr std::span<const source_type_modifier> alias_modifiers() const noexcept {
        return alias_modifiers_value;
    }

    [[nodiscard]] constexpr std::string_view text(source_span range) const noexcept {
        if (range.offset > source_text_value.size() ||
            range.length > source_text_value.size() - range.offset) {
            return {};
        }
        return source_text_value.substr(range.offset, range.length);
    }

private:
    source_id source_value;
    std::string_view source_text_value;
    std::span<const source_namespace_fact> namespaces_value;
    std::span<const source_record_fact> records_value;
    std::span<const source_member_fact> members_value;
    std::span<const source_type_modifier> modifiers_value;
    std::span<const source_enum_fact> enums_value;
    std::span<const source_enum_value_fact> enum_values_value;
    std::span<const source_declaration_ref> declarations_value;
    std::span<const source_object_fact> objects_value;
    std::span<const source_link_fact> links_value;
    std::span<const source_base_fact> bases_value;
    std::span<const source_alias_fact> aliases_value;
    std::span<const source_type_modifier> alias_modifiers_value;
};

static_assert(std::is_trivially_copyable_v<source_span>);
static_assert(std::is_trivially_copyable_v<source_fact_range>);
static_assert(std::is_trivially_copyable_v<source_type_modifier>);
static_assert(std::is_trivially_copyable_v<source_type_ref>);
static_assert(std::is_trivially_copyable_v<source_namespace_fact>);
static_assert(std::is_trivially_copyable_v<source_record_fact>);
static_assert(std::is_trivially_copyable_v<source_member_fact>);
static_assert(std::is_trivially_copyable_v<source_enum_fact>);
static_assert(std::is_trivially_copyable_v<source_enum_value_fact>);
static_assert(std::is_trivially_copyable_v<source_declaration_ref>);
static_assert(std::is_trivially_copyable_v<source_object_fact>);
static_assert(std::is_trivially_copyable_v<source_object_endpoint_fact>);
static_assert(std::is_trivially_copyable_v<source_link_fact>);
static_assert(std::is_trivially_copyable_v<source_base_fact>);
static_assert(std::is_trivially_copyable_v<source_alias_fact>);
static_assert(sizeof(source_span) == 8);
static_assert(sizeof(source_fact_range) == 8);

} // namespace cw::server
