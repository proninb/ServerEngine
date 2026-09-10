#pragma once

#include "type_handle.hpp"
#include "type_ref.hpp"
#include "../frontend/source_facts.hpp"
#include "../identity/identity_node.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace cw::server {

class generation_builder;

inline constexpr std::size_t graph_intrinsic_type_count =
    static_cast<std::size_t>(intrinsic_type::nullptr_type) + 1;

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

// Graph-local hot semantic state. A tombstone retains its historical
// handle/identity mapping but is not a live type in the current Graph.
struct type_entry final {
    definition_range definition{};
    graph_type_kind kind = graph_type_kind::record;
    source_record_kind record_kind = source_record_kind::struct_type;
    intrinsic_type enum_underlying = intrinsic_type::none;
    std::uint8_t flags = 0;

    [[nodiscard]] constexpr bool defined() const noexcept { return definition.valid(); }
    [[nodiscard]] constexpr bool enum_scoped() const noexcept { return (flags & 0x01u) != 0; }
    [[nodiscard]] constexpr bool enum_fixed_underlying() const noexcept { return (flags & 0x02u) != 0; }
    [[nodiscard]] constexpr bool live() const noexcept { return (flags & 0x80u) != 0; }
};

static_assert(sizeof(type_entry) == 12);

// Generation-local non-static instance member. Names live in the Graph name
// arena; TypeRef is interpreted only in this same Graph.
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

// Eight-byte acceleration slots retained by Graph so incremental Builder can
// map already-resolved identity_ref directly to a historical type_handle.
struct graph_identity_index_slot final {
    std::uint32_t fingerprint = 0;
    std::uint32_t handle = 0;
};

static_assert(sizeof(graph_identity_index_slot) == 8);

struct graph_derived_index_slot final {
    std::uint32_t fingerprint = 0;
    std::uint32_t type_ref = 0;
};

static_assert(sizeof(graph_derived_index_slot) == 8);

// Append-only reverse dependency edge. owner_version makes prior outgoing edges
// stale in O(1) when a type definition changes; G0 reclaims stale history.
struct graph_dependency_edge final {
    std::uint32_t owner_handle = 0;
    std::uint32_t next_for_target = 0;
    std::uint32_t owner_version = 0;
};

static_assert(sizeof(graph_dependency_edge) == 12);

// Canonical TypeRef payload shared by detached G0 and sparse incremental
// candidates. Named records hold a type_handle; derived records hold child ref.
struct graph_canonical_type_record final {
    std::uint64_t payload = 0;
    std::uint32_t child_or_handle = 0;
    canonical_type_kind kind = canonical_type_kind::intrinsic;
    std::uint8_t detail = 0;
    std::uint16_t reserved = 0;
};

static_assert(sizeof(graph_canonical_type_record) == 16);

// Detached complete Graph storage. Generation Builder performs all
// allocation and validation here; Graph publication is a no-fail swap only.
class prepared_graph_generation final {
public:
    prepared_graph_generation() = default;
    prepared_graph_generation(const prepared_graph_generation&) = delete;
    prepared_graph_generation& operator=(const prepared_graph_generation&) = delete;
    prepared_graph_generation(prepared_graph_generation&&) noexcept = default;
    prepared_graph_generation& operator=(prepared_graph_generation&&) noexcept = default;

private:
    std::vector<type_entry> types;
    std::vector<identity_ref> identities;
    std::vector<member_record> members;
    std::vector<enum_value_record> enum_values;
    std::vector<char> names;
    std::vector<graph_canonical_type_record> canonical_types;

    std::vector<graph_identity_index_slot> identity_index;
    std::array<TypeRef, graph_intrinsic_type_count> intrinsic_refs{};
    std::vector<TypeRef> named_refs;
    std::vector<graph_derived_index_slot> derived_index;
    std::size_t derived_index_entries = 0;

    std::vector<std::uint32_t> dependency_versions;
    std::vector<std::uint32_t> reverse_dependency_heads;
    std::vector<graph_dependency_edge> dependency_edges;

    std::size_t live_type_count = 0;

    friend class graph;
    friend class generation_builder;
};

// Sparse prepared mutation of one committed Graph lineage. Only touched type
// slots and append-only arenas are retained here. All owner capacity growth and
// optional index rehashing are completed before publication.
class prepared_graph_update final {
public:
    prepared_graph_update() = default;
    prepared_graph_update(const prepared_graph_update&) = delete;
    prepared_graph_update& operator=(const prepared_graph_update&) = delete;
    prepared_graph_update(prepared_graph_update&&) noexcept = default;
    prepared_graph_update& operator=(prepared_graph_update&&) noexcept = default;

private:
    struct type_patch final {
        std::uint32_t handle = 0;
        type_entry value{};
    };

    struct named_ref_patch final {
        std::uint32_t handle = 0;
        TypeRef value{};
    };

    struct dependency_version_patch final {
        std::uint32_t handle = 0;
        std::uint32_t version = 0;
    };

    struct pending_dependency_edge final {
        std::uint32_t target_handle = 0;
        std::uint32_t owner_handle = 0;
        std::uint32_t owner_version = 0;
    };

    std::vector<type_patch> type_patches;
    std::vector<type_entry> new_types;
    std::vector<identity_ref> new_identities;

    std::vector<member_record> members;
    std::vector<enum_value_record> enum_values;
    std::vector<char> names;
    std::vector<graph_canonical_type_record> canonical_types;

    std::array<TypeRef, graph_intrinsic_type_count> intrinsic_refs{};
    std::vector<named_ref_patch> named_ref_patches;

    std::vector<dependency_version_patch> dependency_version_patches;
    std::vector<pending_dependency_edge> dependency_edges;

    std::vector<graph_identity_index_slot> rebuilt_identity_index;
    std::vector<graph_derived_index_slot> rebuilt_derived_index;
    std::size_t derived_index_entries = 0;

    std::size_t live_type_count = 0;
    bool replace_identity_index = false;
    bool replace_derived_index = false;

    friend class graph;
    friend class generation_builder;
};

// Owns the one current committed semantic Graph. Graph owns only compiled
// state plus private acceleration indexes; Project semantic identity
// remains owned by Project Context and has no reverse pointer into Graph.
class graph final {
public:
    graph() noexcept = default;

    graph(const graph&) = delete;
    graph& operator=(const graph&) = delete;
    graph(graph&&) = delete;
    graph& operator=(graph&&) = delete;

    [[nodiscard]] std::size_t type_count() const noexcept { return live_type_count; }
    [[nodiscard]] std::size_t type_slot_count() const noexcept { return types.size(); }
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
    void publish_prepared(prepared_graph_generation& prepared) noexcept;
    void publish_prepared(prepared_graph_update& prepared) noexcept;

    [[nodiscard]] type_handle find_identity(identity_ref identity) const noexcept;
    [[nodiscard]] identity_ref identity_raw(type_handle handle) const noexcept;
    [[nodiscard]] const type_entry* find_raw(type_handle handle) const noexcept;
    [[nodiscard]] bool named_raw(TypeRef type, type_handle& output) const noexcept;

    std::vector<type_entry> types;
    std::vector<identity_ref> identities;
    std::vector<member_record> member_records;
    std::vector<enum_value_record> enum_value_records;
    std::vector<char> names;
    std::vector<graph_canonical_type_record> canonical_types;

    std::vector<graph_identity_index_slot> identity_index;
    std::array<TypeRef, graph_intrinsic_type_count> intrinsic_refs{};
    std::vector<TypeRef> named_refs;
    std::vector<graph_derived_index_slot> derived_index;
    std::size_t derived_index_entries = 0;

    std::vector<std::uint32_t> dependency_versions;
    std::vector<std::uint32_t> reverse_dependency_heads;
    std::vector<graph_dependency_edge> dependency_edges;

    std::size_t live_type_count = 0;

    friend class generation_builder;
};

static_assert(std::is_trivially_copyable_v<graph_name_ref>);
static_assert(std::is_trivially_copyable_v<definition_range>);

} // namespace cw::server
