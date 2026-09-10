#pragma once

#include "../../operation.hpp"
#include "../../source_id.hpp"
#include "../../status.hpp"
#include "../frontend/source_facts.hpp"

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

struct source_contribution_statistics final {
    std::size_t sources = 0;
    std::size_t type_declarations = 0;
    std::size_t members = 0;
    std::size_t modifiers = 0;
    std::size_t enum_values = 0;
    std::size_t name_bytes = 0;
};

class source_contribution_cache_update;

// Non-authoritative build provenance keyed by source_id. G0 storage is flat and
// dense; it exists outside Graph solely so later sparse Source replacement can
// subtract the old contribution without reconstructing unrelated Sources.
class source_contribution_cache final {
public:
    source_contribution_cache() = default;

    [[nodiscard]] source_contribution_cache_update begin_rebuild() noexcept;

    [[nodiscard]] const source_contribution_state* state(source_id source) const noexcept;
    [[nodiscard]] std::span<const source_contribution_type> types(source_id source) const noexcept;
    [[nodiscard]] std::span<const source_contribution_member> members(source_fact_range range) const noexcept;
    [[nodiscard]] std::span<const source_type_modifier> modifiers(source_fact_range range) const noexcept;
    [[nodiscard]] std::span<const source_contribution_enum_value> enum_values(source_fact_range range) const noexcept;
    [[nodiscard]] std::string_view name(source_contribution_name_ref value) const noexcept;
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
        source_contribution_statistics statistics{};

        void swap(storage& other) noexcept;
    };

    storage committed;
    source_contribution_statistics statistics_value{};
    bool provenance_complete = true;

    friend class source_contribution_cache_update;
    friend class generation_builder;
};

// Detached full-reconstruction candidate. replace() captures source_facts into
// owned flat arenas; prepare_publish() performs no semantic work and publication
// is an allocation-free/no-fail swap into SourceContribution cache.
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

} // namespace cw::server
