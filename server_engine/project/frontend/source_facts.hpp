#pragma once

#include "../../source_id.hpp"
#include "../identity/identity_node.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace cw::server {

// Byte range inside the immutable Source snapshot referenced by one source_facts packet.
struct source_span {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;

    [[nodiscard]] constexpr bool empty() const noexcept {
        return length == 0;
    }
};

// Dense range inside one of the flat arrays owned by the producer of source_facts.
struct source_fact_range {
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

// One declarator transformation. Modifiers are applied from the base type outward,
// left to right. value is used only by bounded_array and contains its element count.
struct source_type_modifier {
    std::uint64_t value = 0;
    source_type_modifier_kind kind = source_type_modifier_kind::pointer;
};

// Fully resolved source-language type reference. A base is either one Project
// identity or one intrinsic Language/ABI code; unresolved identifier text is forbidden.
struct source_type_ref {
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

// One namespace contribution in this Source. identity is already canonical Project identity.
struct source_namespace_fact {
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

// One C++ record declaration or definition. Definition members occupy one contiguous
// lexical-order range in source_facts::members(); declarations have no members.
struct source_record_fact {
    identity_ref identity = nullptr;
    source_fact_range members{};
    source_span declaration{};
    source_record_declaration_kind declaration_kind = source_record_declaration_kind::declaration;
    source_record_kind record_kind = source_record_kind::struct_type;
};

enum class source_member_access : std::uint8_t {
    public_access,
    protected_access,
    private_access,
};

// One non-static instance data member. name and declaration refer to the immutable
// Source snapshot; type already contains a resolved semantic identity or intrinsic type.
struct source_member_fact {
    source_type_ref type{};
    source_span name{};
    source_span declaration{};
    source_member_access access = source_member_access::public_access;
};

// Immutable zero-allocation Parser -> Generation Builder boundary. All spans and source
// text are borrowed from build-lifetime storage; referenced identities remain valid for
// the Project lifetime. This packet is transient process memory and is never serialized.
class source_facts final {
public:
    constexpr source_facts(
        source_id source,
        std::string_view source_text,
        std::span<const source_namespace_fact> namespaces,
        std::span<const source_record_fact> records,
        std::span<const source_member_fact> members,
        std::span<const source_type_modifier> modifiers) noexcept
        : source_value(source),
          source_text_value(source_text),
          namespaces_value(namespaces),
          records_value(records),
          members_value(members),
          modifiers_value(modifiers) {}

    [[nodiscard]] constexpr source_id source() const noexcept { return source_value; }
    [[nodiscard]] constexpr std::string_view source_text() const noexcept { return source_text_value; }
    [[nodiscard]] constexpr std::span<const source_namespace_fact> namespaces() const noexcept { return namespaces_value; }
    [[nodiscard]] constexpr std::span<const source_record_fact> records() const noexcept { return records_value; }
    [[nodiscard]] constexpr std::span<const source_member_fact> members() const noexcept { return members_value; }
    [[nodiscard]] constexpr std::span<const source_type_modifier> modifiers() const noexcept { return modifiers_value; }

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
};

static_assert(std::is_trivially_copyable_v<source_span>);
static_assert(std::is_trivially_copyable_v<source_fact_range>);
static_assert(std::is_trivially_copyable_v<source_type_modifier>);
static_assert(std::is_trivially_copyable_v<source_type_ref>);
static_assert(std::is_trivially_copyable_v<source_namespace_fact>);
static_assert(std::is_trivially_copyable_v<source_record_fact>);
static_assert(std::is_trivially_copyable_v<source_member_fact>);
static_assert(sizeof(source_span) == 8);
static_assert(sizeof(source_fact_range) == 8);

} // namespace cw::server
