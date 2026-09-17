#pragma once

#include "../frontend/source_facts.hpp"
#include "../identity/identity_space.hpp"
#include "../../member_index.hpp"
#include "../../status.hpp"
#include "../../string_id.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>


namespace cw::server {

class build_cache_image_view;

struct source_interface_object final {
    identity_ref identity{};
    identity_ref named_type{};
};

struct source_interface_member final {
    identity_ref type{};
    string_id name{};
    member_index index{};
};

struct source_interface_type_slot final {
    identity_ref parent{};
    string_id name{};
    identity_ref identity{};
};

struct source_interface_object_slot final {
    identity_ref parent{};
    string_id name{};
    identity_ref identity{};
    identity_ref named_type{};
};

struct source_interface_member_slot final {
    identity_ref type{};
    string_id name{};
    member_index index{};
};

// Read-only persistence boundary for Source-local Parser acceleration state.
// imported_interfaces is intentionally excluded because it contains process pointers.
struct source_interface_data_view final {
    std::span<const identity_ref> local_types;
    std::span<const source_interface_type_slot> type_slots;
    std::span<const source_interface_object_slot> object_slots;
    std::span<const source_interface_member_slot> member_slots;
};

// Move boundary used when a full Frontend generation consolidates per-Source
// persistence payload into cache-wide arenas. Parser lookup tables are excluded.
struct source_interface_owned_persistence_data final {
    std::vector<identity_ref> local_types;
    std::vector<source_interface_type_slot> type_slots;
    std::vector<source_interface_object_slot> object_slots;
    std::vector<source_interface_member_slot> member_slots;
};

// GEN-02C18.1: logical compact persistence cardinality is retained even when
// full REBUILD omits the per-Source SAVE-only compact vectors.
struct source_interface_persistence_counts final {
    std::size_t local_types = 0;
    std::size_t type_slots = 0;
    std::size_t object_slots = 0;
    std::size_t member_slots = 0;
};

// Immutable Parser-visible interface exported by one parsed Source. Lookup keys
// retain parent/name explicitly, so Parser lookup uses only numeric identity_ref
// equality and never dereferences semantic identity records on the probe path.
class source_interface final {
public:
    source_interface() = default;
    source_interface(const source_interface&) = delete;
    source_interface& operator=(const source_interface&) = delete;
    source_interface(source_interface&&) noexcept = default;
    source_interface& operator=(source_interface&&) noexcept = default;

    [[nodiscard]] status initialize(
        const source_facts& facts,
        identity_view identities,
        std::span<const source_interface* const> imports = {},
        bool retain_compact_persistence = true) noexcept;

    // Restores one immutable Parser interface from the persisted Build Cache
    // image without reparsing its Source. Import pointers are rebound process-locally.
    [[nodiscard]] status initialize_persisted(
        const build_cache_image_view& cache,
        source_id source,
        std::span<const source_interface* const> imports = {}) noexcept;

    [[nodiscard]] identity_ref find_type(
        identity_ref scope,
        string_id name) const noexcept;

    [[nodiscard]] source_interface_object find_object(
        identity_ref scope,
        string_id name) const noexcept;

    [[nodiscard]] member_index find_member(
        identity_ref type,
        string_id name) const noexcept;

    [[nodiscard]] std::span<const identity_ref> local_types() const noexcept {
        return persistence_externalized_state
            ? external_persistence_data.local_types
            : std::span<const identity_ref>{local_type_values};
    }

    [[nodiscard]] source_interface_data_view data_view() const noexcept {
        return {
            local_types(),
            type_slots,
            object_slots,
            member_slots,
        };
    }

    // Compact SAVE representation. A fully published native Frontend may bind
    // this interface to cache-owned arenas; sparse/new interfaces retain local
    // owned vectors and expose the same view contract.
    [[nodiscard]] source_interface_data_view
    persistence_data_view() const noexcept {
        if (persistence_externalized_state)
            return external_persistence_data;

        return {
            local_type_values,
            persistence_type_slots,
            persistence_object_slots,
            persistence_member_slots,
        };
    }

    [[nodiscard]] source_interface_persistence_counts
    persistence_counts() const noexcept {
        return {
            persistence_local_type_count,
            persistence_type_slot_count,
            persistence_object_slot_count,
            persistence_member_slot_count,
        };
    }

    [[nodiscard]] bool compact_persistence_available() const noexcept {
        return persistence_externalized_state ||
            persistence_compact_state;
    }

    [[nodiscard]] source_interface_data_view
    runtime_data_view() const noexcept {
        return {
            local_type_values,
            type_slots,
            object_slots,
            member_slots,
        };
    }

    // Transfers only persistence payload ownership. Runtime lookup hash tables
    // and import pointers remain inside the interface.
    [[nodiscard]] source_interface_owned_persistence_data
    release_persistence_data() noexcept;

    // Binds this immutable interface to cache-owned persistence arenas after all
    // arenas have reached their final addresses.
    void bind_persistence_data(
        source_interface_data_view data) noexcept;

private:
    using type_slot = source_interface_type_slot;
    using object_slot = source_interface_object_slot;
    using member_slot = source_interface_member_slot;

    [[nodiscard]] identity_ref find_type_recursive(
        identity_ref scope,
        string_id name,
        std::uint32_t depth) const noexcept;

    [[nodiscard]] source_interface_object find_object_recursive(
        identity_ref scope,
        string_id name,
        std::uint32_t depth) const noexcept;

    [[nodiscard]] member_index find_member_recursive(
        identity_ref type,
        string_id name,
        std::uint32_t depth) const noexcept;

    std::vector<identity_ref> local_type_values;

    // Parser lookup tables.
    std::vector<type_slot> type_slots;
    std::vector<object_slot> object_slots;
    std::vector<member_slot> member_slots;

    // Compact occupied entries used only by persistence.
    std::vector<type_slot> persistence_type_slots;
    std::vector<object_slot> persistence_object_slots;
    std::vector<member_slot> persistence_member_slots;

    source_interface_data_view external_persistence_data{};
    std::size_t persistence_local_type_count = 0;
    std::size_t persistence_type_slot_count = 0;
    std::size_t persistence_object_slot_count = 0;
    std::size_t persistence_member_slot_count = 0;
    bool persistence_compact_state = true;
    bool persistence_externalized_state = false;

    std::vector<const source_interface*> imported_interfaces;
};

struct source_environment_import final {
    std::uint32_t visible_from = 0;
    const source_interface* interface = nullptr;
};

// Non-owning positional include environment for one Parser invocation.
class source_environment final {
public:
    source_environment() noexcept = default;
    explicit source_environment(std::span<const source_environment_import> import_values) noexcept
        : imports(import_values) {}

    [[nodiscard]] identity_ref find_type(
        identity_ref scope,
        string_id name,
        std::uint32_t source_offset) const noexcept;

    [[nodiscard]] source_interface_object find_object(
        identity_ref scope,
        string_id name,
        std::uint32_t source_offset) const noexcept;

    [[nodiscard]] member_index find_member(
        identity_ref type,
        string_id name,
        std::uint32_t source_offset) const noexcept;

private:
    std::span<const source_environment_import> imports;
};

} // namespace cw::server
