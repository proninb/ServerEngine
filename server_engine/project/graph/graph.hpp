#pragma once

#include "type_handle.hpp"
#include "object_handle.hpp"
#include "link_handle.hpp"
#include "../../member_index.hpp"
#include "type_ref.hpp"
#include "../frontend/source_facts.hpp"
#include "../identity/identity_node.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace cw::server {

class generation_builder;

inline constexpr std::size_t graph_intrinsic_type_count =
    static_cast<std::size_t>(intrinsic_type::nullptr_type) + 1;

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

// Graph-local non-static instance member. string_id names are Project-lifetime
// textual atoms; TypeRef is interpreted only in this same Graph.
struct member_record final {
    string_id name{};
    TypeRef type{};
    source_member_access access = source_member_access::public_access;
};

static_assert(sizeof(member_record) == 12);

struct enum_value_record final {
    std::uint64_t bits = 0;
    string_id name{};
    intrinsic_type intrinsic = intrinsic_type::signed_int;
};

static_assert(sizeof(enum_value_record) == 16);

// Graph-local static Project object. The type is already resolved to a Graph
// handle; identity/name remain outside the hot record.
struct object_entry final {
    TypeRef type{};
    std::uint32_t flags = 0;

    [[nodiscard]] constexpr bool live() const noexcept { return (flags & 0x80000000u) != 0; }
};

static_assert(sizeof(object_entry) == 8);

struct object_endpoint final {
    object_handle object{};
    member_index member{};

    friend constexpr bool operator==(
        const object_endpoint&, const object_endpoint&) noexcept = default;
};

static_assert(sizeof(object_endpoint) == 8);

// Directed static Project connection. target identifies the unique binding
// destination; Runtime later materializes this semantic edge into an address.
struct link_record final {
    object_endpoint source{};
    object_endpoint target{};

    [[nodiscard]] constexpr bool live() const noexcept {
        return static_cast<bool>(source.object) && static_cast<bool>(target.object);
    }
};

static_assert(sizeof(link_record) == 16);

struct graph_object_identity_index_slot final {
    std::uint32_t fingerprint = 0;
    std::uint32_t handle = 0;
};

static_assert(sizeof(graph_object_identity_index_slot) == 8);

struct graph_link_index_slot final {
    std::uint32_t fingerprint = 0;
    std::uint32_t handle = 0;
};

static_assert(sizeof(graph_link_index_slot) == 8);

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

struct graph_storage_usage final {
    std::size_t retained_bytes = 0;
    std::size_t reserve_bytes = 0;
    std::size_t stale_bytes = 0;
};

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

// Read-only physical semantic arrays of the current Graph. Builder-only
// acceleration indexes and dependency caches are intentionally excluded.
struct graph_data_view final {
    std::span<const type_entry> types;
    std::span<const identity_ref> type_identities;
    std::span<const member_record> members;
    std::span<const enum_value_record> enum_values;
    std::span<const object_entry> objects;
    std::span<const identity_ref> object_identities;
    std::span<const link_record> links;
    std::span<const graph_canonical_type_record> canonical_types;
    std::size_t live_types = 0;
    std::size_t live_objects = 0;
    std::size_t live_links = 0;
};

// Read-only persistence boundary for incremental Builder lineage that is not
// Runtime/READY semantic ownership. The query indexes remain in compiled.bin.
struct graph_build_data_view final {
    std::span<const TypeRef> intrinsic_refs;
    std::span<const TypeRef> named_refs;
    std::span<const graph_derived_index_slot> derived_index;
    std::size_t derived_index_entries = 0;
    std::span<const std::uint32_t> dependency_versions;
    std::span<const std::uint32_t> reverse_dependency_heads;
    std::span<const graph_dependency_edge> dependency_edges;
};

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
    std::vector<object_entry> objects;
    std::vector<identity_ref> object_identities;
    std::vector<link_record> links;
    std::vector<graph_canonical_type_record> canonical_types;

    std::vector<graph_identity_index_slot> identity_index;
    std::vector<graph_object_identity_index_slot> object_identity_index;
    std::vector<graph_link_index_slot> link_index;
    std::array<TypeRef, graph_intrinsic_type_count> intrinsic_refs{};
    std::vector<TypeRef> named_refs;
    std::vector<graph_derived_index_slot> derived_index;
    std::size_t derived_index_entries = 0;

    std::vector<std::uint32_t> dependency_versions;
    std::vector<std::uint32_t> reverse_dependency_heads;
    std::vector<graph_dependency_edge> dependency_edges;

    std::size_t live_type_count = 0;
    std::size_t live_object_count = 0;
    std::size_t live_link_count = 0;

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

    struct object_patch final {
        std::uint32_t handle = 0;
        object_entry value{};
    };

    struct link_patch final {
        std::uint32_t handle = 0;
        link_record value{};
    };

    std::vector<type_patch> type_patches;
    std::vector<type_entry> new_types;
    std::vector<identity_ref> new_identities;

    std::vector<object_patch> object_patches;
    std::vector<object_entry> new_objects;
    std::vector<identity_ref> new_object_identities;
    std::vector<link_patch> link_patches;
    std::vector<link_record> new_links;

    std::vector<member_record> members;
    std::vector<enum_value_record> enum_values;
    std::vector<graph_canonical_type_record> canonical_types;

    std::array<TypeRef, graph_intrinsic_type_count> intrinsic_refs{};
    std::vector<named_ref_patch> named_ref_patches;

    std::vector<dependency_version_patch> dependency_version_patches;
    std::vector<pending_dependency_edge> dependency_edges;

    std::vector<graph_identity_index_slot> rebuilt_identity_index;
    std::vector<graph_object_identity_index_slot> rebuilt_object_identity_index;
    std::vector<graph_link_index_slot> rebuilt_link_index;
    std::vector<graph_derived_index_slot> rebuilt_derived_index;
    std::size_t derived_index_entries = 0;

    std::size_t live_type_count = 0;
    std::size_t live_object_count = 0;
    std::size_t live_link_count = 0;
    bool replace_identity_index = false;
    bool replace_object_identity_index = false;
    bool replace_link_index = false;
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
    [[nodiscard]] std::size_t object_count() const noexcept { return live_object_count; }
    [[nodiscard]] std::size_t object_slot_count() const noexcept { return object_entries.size(); }
    [[nodiscard]] std::size_t link_count() const noexcept { return live_link_count; }
    [[nodiscard]] std::size_t link_slot_count() const noexcept { return link_records.size(); }
    [[nodiscard]] std::size_t canonical_type_count() const noexcept {
        return canonical_types.empty() ? 0 : canonical_types.size() - 1;
    }

    [[nodiscard]] graph_data_view data_view() const noexcept {
        return {
            types,
            identities,
            member_records,
            enum_value_records,
            object_entries,
            object_identities,
            link_records,
            canonical_types,
            live_type_count,
            live_object_count,
            live_link_count,
        };
    }

    [[nodiscard]] graph_build_data_view build_data_view() const noexcept {
        return {
            intrinsic_refs,
            named_refs,
            derived_index,
            derived_index_entries,
            dependency_versions,
            reverse_dependency_heads,
            dependency_edges,
        };
    }

    [[nodiscard]] type_handle type_at(std::size_t index) const noexcept;
    [[nodiscard]] const type_entry* find(type_handle handle) const noexcept;
    [[nodiscard]] identity_ref identity(type_handle handle) const noexcept;
    [[nodiscard]] type_handle find_type(identity_ref identity) const noexcept;

    [[nodiscard]] std::span<const member_record> members(type_handle handle) const noexcept;
    [[nodiscard]] std::span<const enum_value_record> enum_values(type_handle handle) const noexcept;
    [[nodiscard]] member_index find_member(type_handle handle, string_id name) const noexcept;

    [[nodiscard]] object_handle object_at(std::size_t index) const noexcept;
    [[nodiscard]] const object_entry* find(object_handle handle) const noexcept;
    [[nodiscard]] identity_ref identity(object_handle handle) const noexcept;
    [[nodiscard]] object_handle find_object(identity_ref identity) const noexcept;

    [[nodiscard]] const link_record* find(link_handle handle) const noexcept;
    [[nodiscard]] link_handle find_link(object_endpoint target) const noexcept;

    [[nodiscard]] canonical_type_kind kind(TypeRef type) const noexcept;
    [[nodiscard]] bool intrinsic(TypeRef type, intrinsic_type& output) const noexcept;
    [[nodiscard]] bool named(TypeRef type, type_handle& output) const noexcept;
    [[nodiscard]] bool derived(TypeRef type, derived_type_record& output) const noexcept;

    [[nodiscard]] graph_storage_usage storage_usage(
        std::size_t live_members,
        std::size_t live_enum_values) const noexcept;

private:
    void publish_prepared(prepared_graph_generation& prepared) noexcept;
    void publish_prepared(prepared_graph_update& prepared) noexcept;

    [[nodiscard]] type_handle find_identity(identity_ref identity) const noexcept;
    [[nodiscard]] object_handle find_object_identity(identity_ref identity) const noexcept;
    [[nodiscard]] link_handle find_link_raw(object_endpoint target) const noexcept;
    [[nodiscard]] identity_ref identity_raw(type_handle handle) const noexcept;
    [[nodiscard]] const type_entry* find_raw(type_handle handle) const noexcept;
    [[nodiscard]] bool named_raw(TypeRef type, type_handle& output) const noexcept;

    std::vector<type_entry> types;
    std::vector<identity_ref> identities;
    std::vector<member_record> member_records;
    std::vector<enum_value_record> enum_value_records;
    std::vector<object_entry> object_entries;
    std::vector<identity_ref> object_identities;
    std::vector<link_record> link_records;
    std::vector<graph_canonical_type_record> canonical_types;

    std::vector<graph_identity_index_slot> identity_index;
    std::vector<graph_object_identity_index_slot> object_identity_index;
    std::vector<graph_link_index_slot> link_index;
    std::array<TypeRef, graph_intrinsic_type_count> intrinsic_refs{};
    std::vector<TypeRef> named_refs;
    std::vector<graph_derived_index_slot> derived_index;
    std::size_t derived_index_entries = 0;

    std::vector<std::uint32_t> dependency_versions;
    std::vector<std::uint32_t> reverse_dependency_heads;
    std::vector<graph_dependency_edge> dependency_edges;

    std::size_t live_type_count = 0;
    std::size_t live_object_count = 0;
    std::size_t live_link_count = 0;

    friend class generation_builder;
};

static_assert(std::is_trivially_copyable_v<definition_range>);

} // namespace cw::server
