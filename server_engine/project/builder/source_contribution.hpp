#pragma once

#include "../../operation.hpp"
#include "../../source_id.hpp"
#include "../../status.hpp"
#include "../frontend/source_facts.hpp"
#include "../graph/type_handle.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace cw::server {

class diagnostic_buffer;
class generation_builder;

struct source_contribution_name_ref final {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
};

enum class source_contribution_type_kind : std::uint8_t {
    record,
    enumeration,
};

struct source_contribution_type_ref final {
    identity_ref identity = nullptr;
    source_fact_range modifiers{};
    intrinsic_type intrinsic = intrinsic_type::none;
};

struct source_contribution_member final {
    source_contribution_type_ref type{};
    source_contribution_name_ref name{};
    source_member_access access = source_member_access::public_access;
};

struct source_contribution_enum_value final {
    source_contribution_name_ref name{};
    source_integral_constant value{};
};

// One Project type declaration/definition contributed by one Source. The record
// retains identity_ref directly and contains no canonical spelling or stable ID.
struct source_contribution_type final {
    identity_ref identity = nullptr;
    source_fact_range definition_items{};
    intrinsic_type explicit_underlying = intrinsic_type::none;
    source_record_kind record_kind = source_record_kind::struct_type;
    source_contribution_type_kind kind = source_contribution_type_kind::record;
    std::uint8_t flags = 0;

    [[nodiscard]] constexpr bool definition() const noexcept { return (flags & 0x01u) != 0; }
    [[nodiscard]] constexpr bool enum_scoped() const noexcept { return (flags & 0x02u) != 0; }
};

// Dense source_id slot into flat Project contribution arenas. No per-Source
// vector/allocation is required, including for a million one-type Sources.
struct source_contribution_state final {
    source_id source{};
    source_fact_range types{};
    source_fact_range members{};
    source_fact_range modifiers{};
    source_fact_range enum_values{};
};

// Build-side semantic aggregation for one generation-local type slot. It is
// derived from SourceContribution records, is never Runtime-visible, and lets a
// sparse Source replacement subtract/add declarations without scanning Project.
struct source_construction_state final {
    std::uint32_t declarations = 0;
    std::uint32_t definitions = 0;
    std::uint32_t definition_type = 0; // one-based SourceContribution type arena index
    std::uint32_t record_struct = 0;
    std::uint32_t record_class = 0;
    std::uint32_t record_union = 0;
    std::uint32_t enum_scoped = 0;
    std::uint32_t enum_unscoped = 0;
    std::uint32_t enum_fixed = 0;
    intrinsic_type fixed_underlying = intrinsic_type::none;
    source_contribution_type_kind kind = source_contribution_type_kind::record;
};

struct source_contribution_statistics final {
    std::size_t sources = 0;
    std::size_t type_declarations = 0;
    std::size_t members = 0;
    std::size_t modifiers = 0;
    std::size_t enum_values = 0;
    std::size_t name_bytes = 0;
};

class source_contribution_cache_update;
class source_contribution_sparse_update;

// Non-authoritative build provenance keyed by source_id. G0 storage is compact;
// incremental replacement appends new payload and patches only changed Source
// slots plus touched construction states. Obsolete append-only slices are
// reclaimed by the next explicit G0 rebuild.
class source_contribution_cache final {
public:
    source_contribution_cache() = default;

    [[nodiscard]] source_contribution_cache_update begin_rebuild() noexcept;
    [[nodiscard]] source_contribution_sparse_update begin_incremental() noexcept;

    [[nodiscard]] const source_contribution_state* state(source_id source) const noexcept;
    [[nodiscard]] std::span<const source_contribution_type> types(source_id source) const noexcept;
    [[nodiscard]] std::span<const source_contribution_member> members(source_fact_range range) const noexcept;
    [[nodiscard]] std::span<const source_type_modifier> modifiers(source_fact_range range) const noexcept;
    [[nodiscard]] std::span<const source_contribution_enum_value> enum_values(source_fact_range range) const noexcept;
    [[nodiscard]] std::string_view name(source_contribution_name_ref value) const noexcept;
    [[nodiscard]] const source_construction_state* construction(type_handle handle) const noexcept;
    [[nodiscard]] const source_contribution_statistics& statistics() const noexcept { return statistics_value; }
    [[nodiscard]] bool complete() const noexcept { return provenance_complete; }

    void invalidate() noexcept { provenance_complete = false; }

private:
    struct storage final {
        std::vector<source_contribution_state> sources;
        std::vector<source_contribution_type> types;
        std::vector<source_contribution_member> members;
        std::vector<source_type_modifier> modifiers;
        std::vector<source_contribution_enum_value> enum_values;
        std::vector<char> names;
        // One-based by type_handle; slot zero is the sentinel.
        std::vector<source_construction_state> construction;
        source_contribution_statistics statistics{};

        void swap(storage& other) noexcept;
    };

    storage committed;
    source_contribution_statistics statistics_value{};
    bool provenance_complete = true;

    friend class source_contribution_cache_update;
    friend class source_contribution_sparse_update;
    friend class generation_builder;
};

// Detached full-reconstruction candidate. replace() captures source_facts into
// owned flat arenas; publication is an allocation-free/no-fail storage swap.
class source_contribution_cache_update final {
public:
    source_contribution_cache_update() noexcept = default;

    source_contribution_cache_update(const source_contribution_cache_update&) = delete;
    source_contribution_cache_update& operator=(const source_contribution_cache_update&) = delete;
    source_contribution_cache_update(source_contribution_cache_update&&) noexcept = default;
    source_contribution_cache_update& operator=(source_contribution_cache_update&&) noexcept = default;

    [[nodiscard]] status replace(
        const source_facts& facts,
        operation_id operation,
        diagnostic_buffer& diagnostics) noexcept;

    [[nodiscard]] const source_contribution_state* state(source_id source) const noexcept;
    [[nodiscard]] std::span<const source_contribution_type> types(source_id source) const noexcept;
    [[nodiscard]] const source_contribution_statistics& statistics() const noexcept {
        return candidate.statistics;
    }

    [[nodiscard]] status prepare_publish() noexcept;
    void publish_prepared() noexcept;

private:
    explicit source_contribution_cache_update(source_contribution_cache& cache) noexcept
        : owner(&cache) {}

    [[nodiscard]] status reserve_rebuild(
        std::size_t max_source_id,
        std::size_t type_declarations,
        std::size_t members,
        std::size_t modifiers,
        std::size_t enum_values,
        std::size_t name_bytes) noexcept;

    source_contribution_cache* owner = nullptr;
    source_contribution_cache::storage candidate;
    std::vector<std::uint8_t> replaced;
    status failure{};
    bool prepared = false;
    bool published = false;

    friend class source_contribution_cache;
    friend class generation_builder;
};

// Sparse SourceContribution candidate. New payload is kept in transaction-local
// append arenas whose ranges are pre-adjusted to the committed global arenas.
// prepare_publish() reserves all owner growth; publish_prepared() only appends
// trivially-copyable payload and patches changed Source/construction slots.
class source_contribution_sparse_update final {
public:
    source_contribution_sparse_update() noexcept = default;

    source_contribution_sparse_update(const source_contribution_sparse_update&) = delete;
    source_contribution_sparse_update& operator=(const source_contribution_sparse_update&) = delete;
    source_contribution_sparse_update(source_contribution_sparse_update&&) noexcept = default;
    source_contribution_sparse_update& operator=(source_contribution_sparse_update&&) noexcept = default;

    [[nodiscard]] status replace(
        const source_facts& facts,
        operation_id operation,
        diagnostic_buffer& diagnostics) noexcept;

    [[nodiscard]] status remove(source_id source) noexcept;

    [[nodiscard]] const source_contribution_state* previous_state(source_id source) const noexcept;
    [[nodiscard]] const source_contribution_state* replacement_state(source_id source) const noexcept;
    [[nodiscard]] std::span<const source_contribution_type> previous_types(source_id source) const noexcept;
    [[nodiscard]] std::span<const source_contribution_type> replacement_types(source_id source) const noexcept;

    [[nodiscard]] const source_contribution_type* type(std::uint32_t zero_based_index) const noexcept;
    [[nodiscard]] std::span<const source_contribution_member> members(source_fact_range range) const noexcept;
    [[nodiscard]] std::span<const source_type_modifier> modifiers(source_fact_range range) const noexcept;
    [[nodiscard]] std::span<const source_contribution_enum_value> enum_values(source_fact_range range) const noexcept;
    [[nodiscard]] std::string_view name(source_contribution_name_ref value) const noexcept;

    [[nodiscard]] const source_construction_state* construction(type_handle handle) const noexcept;
    [[nodiscard]] status set_construction(type_handle handle, const source_construction_state& state) noexcept;

    [[nodiscard]] std::span<const source_id> changed_sources() const noexcept { return changed_source_ids; }
    [[nodiscard]] std::span<const char> appended_names() const noexcept { return candidate.names; }

    [[nodiscard]] status prepare_publish() noexcept;
    void publish_prepared() noexcept;

private:
    struct source_patch final {
        source_id source{};
        source_contribution_state state{};
        bool present = false;
    };

    struct construction_patch final {
        type_handle handle{};
        source_construction_state state{};
    };

    struct patch_slot final {
        std::uint32_t key = 0;
        std::uint32_t position = 0; // patch index + 1
    };

    explicit source_contribution_sparse_update(source_contribution_cache& cache) noexcept;

    [[nodiscard]] status reserve_incremental(
        std::size_t source_changes,
        std::size_t touched_type_upper_bound,
        std::size_t type_declarations,
        std::size_t members,
        std::size_t modifiers,
        std::size_t enum_values,
        std::size_t name_bytes) noexcept;

    [[nodiscard]] source_patch* find_source_patch(source_id source) noexcept;
    [[nodiscard]] const source_patch* find_source_patch(source_id source) const noexcept;
    [[nodiscard]] construction_patch* find_construction_patch(type_handle handle) noexcept;
    [[nodiscard]] const construction_patch* find_construction_patch(type_handle handle) const noexcept;
    [[nodiscard]] status add_source_patch(source_id source, source_patch*& output) noexcept;
    [[nodiscard]] status add_construction_patch(type_handle handle, construction_patch*& output) noexcept;

    source_contribution_cache* owner = nullptr;
    source_contribution_cache::storage candidate;
    std::vector<source_patch> source_patches;
    std::vector<construction_patch> construction_patches;
    std::vector<patch_slot> source_patch_index;
    std::vector<patch_slot> construction_patch_index;
    std::vector<source_id> changed_source_ids;
    source_contribution_statistics prepared_statistics{};
    std::size_t prepared_source_size = 0;
    std::size_t prepared_construction_size = 0;

    std::size_t type_base = 0;
    std::size_t member_base = 0;
    std::size_t modifier_base = 0;
    std::size_t enum_value_base = 0;
    std::size_t name_base = 0;
    status failure{};
    bool prepared = false;
    bool published = false;

    friend class source_contribution_cache;
    friend class generation_builder;
};

} // namespace cw::server
