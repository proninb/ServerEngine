#pragma once

#include "type_handle.hpp"
#include "type_ref.hpp"
#include "../frontend/source_facts.hpp"
#include "../identity/identity_node.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace cw::server {

class generation_builder;

struct graph_name_ref final {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
};

struct definition_range final {
    std::uint32_t begin = 0;
    std::uint32_t count = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return begin != 0; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }
};

enum class graph_type_kind : std::uint8_t {
    record,
    enumeration,
};

// Generation-local hot semantic state. identity_ref is intentionally stored in a
// parallel cold array so execution-facing scans do not pull identity pointers.
struct type_entry final {
    definition_range definition{};
    graph_type_kind kind = graph_type_kind::record;
    source_record_kind record_kind = source_record_kind::struct_type;
    intrinsic_type enum_underlying = intrinsic_type::none;
    std::uint8_t flags = 0;

    [[nodiscard]] constexpr bool defined() const noexcept { return definition.valid(); }
    [[nodiscard]] constexpr bool enum_scoped() const noexcept { return (flags & 0x01u) != 0; }
    [[nodiscard]] constexpr bool enum_fixed_underlying() const noexcept { return (flags & 0x02u) != 0; }
};

static_assert(sizeof(type_entry) == 12);

// Generation-local non-static instance member. Names live in the Graph name
// arena; TypeRef is interpreted only in this same Graph generation.
struct member_record final {
    graph_name_ref name{};
    TypeRef type{};
    source_member_access access = source_member_access::public_access;
};

static_assert(sizeof(member_record) == 16);

struct enum_value_record final {
    std::uint64_t bits = 0;
    graph_name_ref name{};
    intrinsic_type intrinsic = intrinsic_type::signed_int;
};

static_assert(sizeof(enum_value_record) == 24);

// Detached complete storage for one generation. Generation Builder performs all
// allocation and validation here; Graph publication is a no-fail swap only.
class prepared_graph_generation final {
public:
    prepared_graph_generation() = default;
    prepared_graph_generation(const prepared_graph_generation&) = delete;
    prepared_graph_generation& operator=(const prepared_graph_generation&) = delete;
    prepared_graph_generation(prepared_graph_generation&&) noexcept = default;
    prepared_graph_generation& operator=(prepared_graph_generation&&) noexcept = default;

private:
    struct canonical_type_record final {
        std::uint64_t payload = 0;
        std::uint32_t child_or_handle = 0;
        canonical_type_kind kind = canonical_type_kind::intrinsic;
        std::uint8_t detail = 0;
        std::uint16_t reserved = 0;
    };

    static_assert(sizeof(canonical_type_record) == 16);

    std::vector<type_entry> types;
    std::vector<identity_ref> identities;
    std::vector<member_record> members;
    std::vector<enum_value_record> enum_values;
    std::vector<char> names;
    std::vector<canonical_type_record> canonical_types;
    std::uint64_t generation = 0;

    friend class graph;
    friend class generation_builder;
};

// Owns one immutable committed semantic Graph generation. Graph maps generation-
// local handles to Project-lifetime identity_ref; it never provides reverse
// identity->generation lookup and never owns Project semantic identity.
class graph final {
public:
    graph() noexcept = default;

    graph(const graph&) = delete;
    graph& operator=(const graph&) = delete;
    graph(graph&&) = delete;
    graph& operator=(graph&&) = delete;

    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_value; }
    [[nodiscard]] std::size_t type_count() const noexcept { return types.size(); }
    [[nodiscard]] std::size_t member_record_count() const noexcept { return member_records.size(); }
    [[nodiscard]] std::size_t enum_value_record_count() const noexcept { return enum_value_records.size(); }
    [[nodiscard]] std::size_t canonical_type_count() const noexcept {
        return canonical_types.empty() ? 0 : canonical_types.size() - 1;
    }
    [[nodiscard]] std::size_t name_bytes() const noexcept { return names.size(); }

    [[nodiscard]] type_handle type_at(std::size_t index) const noexcept;
    [[nodiscard]] const type_entry* find(type_handle handle) const noexcept;
    [[nodiscard]] identity_ref identity(type_handle handle) const noexcept;

    [[nodiscard]] std::span<const member_record> members(type_handle handle) const noexcept;
    [[nodiscard]] std::span<const enum_value_record> enum_values(type_handle handle) const noexcept;
    [[nodiscard]] std::string_view name(graph_name_ref value) const noexcept;

    [[nodiscard]] canonical_type_kind kind(TypeRef type) const noexcept;
    [[nodiscard]] bool intrinsic(TypeRef type, intrinsic_type& output) const noexcept;
    [[nodiscard]] bool named(TypeRef type, type_handle& output) const noexcept;
    [[nodiscard]] bool derived(TypeRef type, derived_type_record& output) const noexcept;

private:
    using canonical_type_record = prepared_graph_generation::canonical_type_record;

    void publish_prepared(prepared_graph_generation& prepared) noexcept;

    std::vector<type_entry> types;
    std::vector<identity_ref> identities;
    std::vector<member_record> member_records;
    std::vector<enum_value_record> enum_value_records;
    std::vector<char> names;
    std::vector<canonical_type_record> canonical_types;
    std::uint64_t generation_value = 0;

    friend class generation_builder;
};

static_assert(std::is_trivially_copyable_v<graph_name_ref>);
static_assert(std::is_trivially_copyable_v<definition_range>);

} // namespace cw::server
