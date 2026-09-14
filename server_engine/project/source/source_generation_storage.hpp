#pragma once

#include "../../source_id.hpp"
#include "../../status.hpp"
#include "../storage/mapped_vector.hpp"
#include "source_snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace cw::server {

struct source_edge_range final {
    std::uint64_t offset = 0;
    std::uint32_t count = 0;
    std::uint32_t reserved = 0;
};

static_assert(sizeof(source_edge_range) == 16);


inline constexpr std::uint32_t source_generation_physical_present =
    0x00000001u;

// Persistence-native physical Source state. The field order matches the
// Source Manager image record and is populated at Source publication time.
struct source_generation_physical_record final {
    std::uint32_t flags = 0;
    std::uint32_t reserved = 0;
    std::int64_t write_time_ticks = 0;
    std::uint64_t size = 0;
    source_content_hash hash{};

    [[nodiscard]] constexpr bool present() const noexcept {
        return (flags & source_generation_physical_present) != 0;
    }
};

static_assert(sizeof(source_generation_physical_record) == 56);

enum class source_generation_edge_kind : std::uint8_t {
    includes,
    dependents,
};

// Generation-local Source graph record. Edge replacements append to immutable
// arenas; one source record selects the current range. Baseline records remain
// logically mapped and are materialized only for sparse replacements.
struct source_generation_record final {
    source_edge_range includes;
    source_edge_range dependents;
    std::uint32_t flags = 0;
    std::uint32_t reserved = 0;
};

static_assert(sizeof(source_generation_record) == 40);

// Construction storage for Source dependency topology. Fresh G0 uses dense local
// records; Gn sparse BUILD materializes only replaced baseline records. Edge
// arenas are append-only until the Generation is compacted by REBUILD.
class source_generation_storage final {
public:
    source_generation_storage() = default;

    source_generation_storage(
        const source_generation_storage&) = delete;
    source_generation_storage& operator=(
        const source_generation_storage&) = delete;
    source_generation_storage(
        source_generation_storage&&) noexcept = default;
    source_generation_storage& operator=(
        source_generation_storage&&) noexcept = default;

    void bind_baseline(std::size_t count) noexcept {
        records.bind_baseline(
            nullptr,
            count,
            &source_generation_storage::read_baseline_record);
        physical_records.bind_baseline(
            nullptr,
            count,
            &source_generation_storage::read_baseline_physical);
    }

    [[nodiscard]] std::size_t source_count() const noexcept {
        return records.size();
    }

    [[nodiscard]] std::size_t forward_edge_count() const noexcept {
        return forward_edges.size();
    }

    [[nodiscard]] std::size_t reverse_edge_count() const noexcept {
        return reverse_edges.size();
    }

    [[nodiscard]] std::span<const source_generation_record>
    local_records() const noexcept {
        return records.local_values();
    }

    [[nodiscard]] std::span<const source_generation_physical_record>
    local_physical_records() const noexcept {
        return physical_records.local_values();
    }

    [[nodiscard]] std::span<const source_id>
    forward_edge_arena() const noexcept {
        return forward_edges;
    }

    [[nodiscard]] std::span<const source_id>
    reverse_edge_arena() const noexcept {
        return reverse_edges;
    }

    // Preparation only reserves capacity/materializes sparse metadata. Logical
    // edge ranges are unchanged until publish_*() executes.
    [[nodiscard]] status prepare_publish(
        std::size_t additional_sources,
        std::size_t additional_forward_edges,
        std::size_t additional_reverse_edges) noexcept {

        if (additional_sources >
                (std::numeric_limits<std::size_t>::max)() -
                    records.size() ||
            additional_forward_edges >
                (std::numeric_limits<std::size_t>::max)() -
                    forward_edges.size() ||
            additional_reverse_edges >
                (std::numeric_limits<std::size_t>::max)() -
                    reverse_edges.size()) {
            return {status_code::not_available};
        }

        try {
            records.reserve(
                records.size() + additional_sources);
            physical_records.reserve(
                physical_records.size() + additional_sources);
            forward_edges.reserve(
                forward_edges.size() + additional_forward_edges);
            reverse_edges.reserve(
                reverse_edges.size() + additional_reverse_edges);
            return {};
        }
        catch (const std::bad_alloc&) {
            return {status_code::not_available};
        }
        catch (const std::length_error&) {
            return {status_code::not_available};
        }
    }

    [[nodiscard]] status prepare_source(
        source_id source) noexcept {

        if (!source ||
            static_cast<std::size_t>(source.value()) >
                records.size()) {
            return {status_code::invalid_argument};
        }

        const auto index =
            static_cast<std::size_t>(source.value() - 1);

        (void)records[index];
        (void)physical_records[index];

        if (!records.read_status().ok())
            return records.read_status();

        return physical_records.read_status().ok()
            ? status{}
            : physical_records.read_status();
    }

    // Must be preceded by prepare_publish(additional_sources >= 1).
    void publish_source() noexcept {
        records.emplace_back();
        physical_records.emplace_back();
    }

    // Must be preceded by prepare_source() for a baseline Source. Fresh Sources
    // already own a dense local physical record after publish_source().
    void publish_snapshot(
        source_id source,
        const source_snapshot& snapshot) noexcept {

        if (!source)
            return;

        const auto index =
            static_cast<std::size_t>(source.value() - 1);

        if (index >= physical_records.size())
            return;

        auto& output = physical_records[index];
        output = {};

        if (!snapshot)
            return;

        const auto observation = snapshot.observation();
        output.flags = source_generation_physical_present;
        output.write_time_ticks = observation.write_time_ticks;
        output.size = static_cast<std::uint64_t>(observation.size);
        output.hash = snapshot.hash();
    }

    // Must be preceded by prepare_publish() and, for a baseline source,
    // prepare_source(). Publication is allocation-free under that contract.
    void publish_includes(
        source_id source,
        std::span<const source_id> values) noexcept {

        publish_edges(
            source,
            source_generation_edge_kind::includes,
            values);
    }

    void publish_dependents(
        source_id source,
        std::span<const source_id> values) noexcept {

        publish_edges(
            source,
            source_generation_edge_kind::dependents,
            values);
    }

    [[nodiscard]] bool has_includes(
        source_id source) const noexcept {
        const auto* record = materialized(source);
        return record != nullptr &&
            (record->flags & includes_present) != 0;
    }

    [[nodiscard]] bool has_dependents(
        source_id source) const noexcept {
        const auto* record = materialized(source);
        return record != nullptr &&
            (record->flags & dependents_present) != 0;
    }

    [[nodiscard]] std::span<const source_id> includes(
        source_id source) const noexcept {

        const auto* record = materialized(source);
        return record != nullptr &&
            (record->flags & includes_present) != 0
            ? range(forward_edges, record->includes)
            : std::span<const source_id>{};
    }

    [[nodiscard]] std::span<const source_id> dependents(
        source_id source) const noexcept {

        const auto* record = materialized(source);
        return record != nullptr &&
            (record->flags & dependents_present) != 0
            ? range(reverse_edges, record->dependents)
            : std::span<const source_id>{};
    }

private:
    static constexpr std::uint32_t includes_present = 0x00000001u;
    static constexpr std::uint32_t dependents_present = 0x00000002u;

    [[nodiscard]] static status read_baseline_record(
        const void*,
        std::size_t,
        source_generation_record& output) noexcept {

        // Untouched baseline edges are read directly from the mapped Source
        // image. A sparse replacement materializes this empty overlay record.
        output = {};
        return {};
    }

    [[nodiscard]] static status read_baseline_physical(
        const void*,
        std::size_t,
        source_generation_physical_record& output) noexcept {

        // Untouched baseline physical state remains mmap-backed. This overlay is
        // materialized only when a sparse BUILD replaces the Source snapshot.
        output = {};
        return {};
    }

    [[nodiscard]] const source_generation_record* materialized(
        source_id source) const noexcept {

        if (!source)
            return nullptr;

        const auto index =
            static_cast<std::size_t>(source.value() - 1);

        return index < records.size()
            ? records.materialized(index)
            : nullptr;
    }

    [[nodiscard]] static std::span<const source_id> range(
        const std::vector<source_id>& arena,
        source_edge_range value) noexcept {

        if (value.offset > arena.size() ||
            value.count > arena.size() - value.offset) {
            return {};
        }

        if (value.count == 0)
            return {};

        return {
            arena.data() + static_cast<std::size_t>(value.offset),
            static_cast<std::size_t>(value.count),
        };
    }

    void publish_edges(
        source_id source,
        source_generation_edge_kind kind,
        std::span<const source_id> values) noexcept {

        if (!source ||
            values.size() >
                (std::numeric_limits<std::uint32_t>::max)()) {
            return;
        }

        const auto index =
            static_cast<std::size_t>(source.value() - 1);

        auto* record =
            index < records.size()
            ? const_cast<source_generation_record*>(
                records.materialized(index))
            : nullptr;

        if (record == nullptr)
            return;

        auto& arena =
            kind == source_generation_edge_kind::includes
            ? forward_edges
            : reverse_edges;

        if (arena.size() >
            (std::numeric_limits<std::uint64_t>::max)()) {
            return;
        }

        const source_edge_range replacement{
            static_cast<std::uint64_t>(arena.size()),
            static_cast<std::uint32_t>(values.size()),
            0,
        };

        arena.insert(
            arena.end(),
            values.begin(),
            values.end());

        if (kind == source_generation_edge_kind::includes) {
            record->includes = replacement;
            record->flags |= includes_present;
        }
        else {
            record->dependents = replacement;
            record->flags |= dependents_present;
        }
    }

    mapped_vector<source_generation_record> records;
    mapped_vector<source_generation_physical_record> physical_records;
    std::vector<source_id> forward_edges;
    std::vector<source_id> reverse_edges;
};

} // namespace cw::server
