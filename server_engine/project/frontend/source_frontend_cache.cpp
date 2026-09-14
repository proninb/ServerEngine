#include "source_frontend_cache.hpp"

#include "../persistence/build_cache_image.hpp"
#include "../persistence/source_manager_image.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <new>
#include <type_traits>
#include <stdexcept>
#include <utility>

namespace cw::server {
namespace {

[[nodiscard]] bool cache_headroom(std::size_t size, std::size_t& output) noexcept {
    const auto extra = (std::max)(size / 16, std::size_t{64});
    if (size > (std::numeric_limits<std::size_t>::max)() - extra)
        return false;
    output = size + extra;
    return true;
}

[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

[[nodiscard]] std::size_t overlay_capacity(std::size_t count) noexcept {
    if (count == 0)
        return 0;
    if (count > (std::numeric_limits<std::size_t>::max)() / 2)
        return 0;
    const auto minimum = (std::max)(std::size_t{8}, count * 2);
    return std::bit_ceil(minimum);
}

[[nodiscard]] source_frontend_persistence_record
persistence_record_of(
    const source_interface* value) noexcept {

    source_frontend_persistence_record output;
    if (value == nullptr)
        return output;

    const auto counts =
        value->persistence_counts();

    output.present = true;
    output.local_types = counts.local_types;
    output.type_slots = counts.type_slots;
    output.object_slots = counts.object_slots;
    output.member_slots = counts.member_slots;
    return output;
}

[[nodiscard]] bool add_persistence_record(
    source_frontend_persistence_summary& summary,
    const source_frontend_persistence_record& value) noexcept {

    if (!value.present)
        return true;

    const auto maximum =
        (std::numeric_limits<std::size_t>::max)();

    if (summary.frontend_count == maximum ||
        summary.local_types > maximum - value.local_types ||
        summary.type_slots > maximum - value.type_slots ||
        summary.object_slots > maximum - value.object_slots ||
        summary.member_slots > maximum - value.member_slots) {
        return false;
    }

    ++summary.frontend_count;
    summary.local_types += value.local_types;
    summary.type_slots += value.type_slots;
    summary.object_slots += value.object_slots;
    summary.member_slots += value.member_slots;
    return true;
}

[[nodiscard]] bool subtract_persistence_record(
    source_frontend_persistence_summary& summary,
    const source_frontend_persistence_record& value) noexcept {

    if (!value.present)
        return true;

    if (summary.frontend_count == 0 ||
        summary.local_types < value.local_types ||
        summary.type_slots < value.type_slots ||
        summary.object_slots < value.object_slots ||
        summary.member_slots < value.member_slots) {
        return false;
    }

    --summary.frontend_count;
    summary.local_types -= value.local_types;
    summary.type_slots -= value.type_slots;
    summary.object_slots -= value.object_slots;
    summary.member_slots -= value.member_slots;
    return true;
}

[[nodiscard]] bool make_native_range(
    std::size_t begin,
    std::size_t count,
    source_frontend_native_persistence_range& output) noexcept {

    constexpr auto maximum =
        static_cast<std::size_t>(
            (std::numeric_limits<std::uint32_t>::max)());

    if (begin > maximum ||
        count > maximum ||
        count > maximum - begin) {
        return false;
    }

    output.begin = static_cast<std::uint32_t>(begin);
    output.count = static_cast<std::uint32_t>(count);
    return true;
}

[[nodiscard]] status build_native_persistence_storage(
    std::vector<std::unique_ptr<source_interface>>& interfaces,
    std::size_t source_count,
    const source_frontend_persistence_summary& summary,
    std::vector<source_frontend_native_persistence_record>& records,
    std::vector<identity_ref>& local_types,
    std::vector<source_interface_type_slot>& type_slots,
    std::vector<source_interface_object_slot>& object_slots,
    std::vector<source_interface_member_slot>& member_slots) noexcept {

    records.clear();
    local_types.clear();
    type_slots.clear();
    object_slots.clear();
    member_slots.clear();

    constexpr auto maximum =
        static_cast<std::size_t>(
            (std::numeric_limits<std::uint32_t>::max)());

    if (source_count > maximum ||
        summary.local_types > maximum ||
        summary.type_slots > maximum ||
        summary.object_slots > maximum ||
        summary.member_slots > maximum) {
        return {status_code::not_available};
    }

    try {
        // GEN-02C18.1: allocate the final arena sizes once. Full REBUILD
        // interfaces deliberately omit their SAVE-only compact mirrors.
        records.resize(source_count);
        local_types.resize(summary.local_types);
        type_slots.resize(summary.type_slots);
        object_slots.resize(summary.object_slots);
        member_slots.resize(summary.member_slots);

        std::size_t local_type_cursor = 0;
        std::size_t type_slot_cursor = 0;
        std::size_t object_slot_cursor = 0;
        std::size_t member_slot_cursor = 0;
        std::size_t observed_frontends = 0;

        for (std::size_t index = 0;
             index < source_count;
             ++index) {

            if (index >= interfaces.size())
                return {status_code::initialization_failed};

            auto* interface_value =
                interfaces[index].get();

            if (interface_value == nullptr)
                continue;

            const auto counts =
                interface_value->persistence_counts();

            auto& record = records[index];
            record.present = 1;

            if (!make_native_range(
                    local_type_cursor,
                    counts.local_types,
                    record.local_types) ||
                !make_native_range(
                    type_slot_cursor,
                    counts.type_slots,
                    record.type_slots) ||
                !make_native_range(
                    object_slot_cursor,
                    counts.object_slots,
                    record.object_slots) ||
                !make_native_range(
                    member_slot_cursor,
                    counts.member_slots,
                    record.member_slots)) {
                return {status_code::not_available};
            }

            const bool compact_available =
                interface_value->compact_persistence_available();

            const auto runtime =
                interface_value->runtime_data_view();

            auto owned =
                interface_value->release_persistence_data();

            if (owned.local_types.size() != counts.local_types)
                return {status_code::initialization_failed};

            if (local_type_cursor >
                    local_types.size() ||
                owned.local_types.size() >
                    local_types.size() - local_type_cursor) {
                return {status_code::initialization_failed};
            }

            std::copy(
                owned.local_types.begin(),
                owned.local_types.end(),
                local_types.begin() +
                    static_cast<std::ptrdiff_t>(local_type_cursor));

            local_type_cursor +=
                owned.local_types.size();

            if (compact_available) {
                if (owned.type_slots.size() != counts.type_slots ||
                    owned.object_slots.size() != counts.object_slots ||
                    owned.member_slots.size() != counts.member_slots) {
                    return {status_code::initialization_failed};
                }

                if (type_slot_cursor > type_slots.size() ||
                    owned.type_slots.size() >
                        type_slots.size() - type_slot_cursor ||
                    object_slot_cursor > object_slots.size() ||
                    owned.object_slots.size() >
                        object_slots.size() - object_slot_cursor ||
                    member_slot_cursor > member_slots.size() ||
                    owned.member_slots.size() >
                        member_slots.size() - member_slot_cursor) {
                    return {status_code::initialization_failed};
                }

                std::copy(
                    owned.type_slots.begin(),
                    owned.type_slots.end(),
                    type_slots.begin() +
                        static_cast<std::ptrdiff_t>(type_slot_cursor));
                std::copy(
                    owned.object_slots.begin(),
                    owned.object_slots.end(),
                    object_slots.begin() +
                        static_cast<std::ptrdiff_t>(object_slot_cursor));
                std::copy(
                    owned.member_slots.begin(),
                    owned.member_slots.end(),
                    member_slots.begin() +
                        static_cast<std::ptrdiff_t>(member_slot_cursor));

                type_slot_cursor += owned.type_slots.size();
                object_slot_cursor += owned.object_slots.size();
                member_slot_cursor += owned.member_slots.size();
            }
            else {
                if (!owned.type_slots.empty() ||
                    !owned.object_slots.empty() ||
                    !owned.member_slots.empty()) {
                    return {status_code::initialization_failed};
                }

                const auto type_begin = type_slot_cursor;

                // local_types keeps declaration insertion order. Match each
                // identity back to its occupied runtime hash slot so persisted
                // type-slot ordering remains canonical.
                for (const auto identity : owned.local_types) {
                    const source_interface_type_slot* found = nullptr;

                    for (const auto& slot : runtime.type_slots) {
                        if (slot.identity == identity) {
                            found = &slot;
                            break;
                        }
                    }

                    if (found == nullptr ||
                        type_slot_cursor >= type_slots.size()) {
                        return {status_code::initialization_failed};
                    }

                    type_slots[type_slot_cursor++] = *found;
                }

                if (type_slot_cursor - type_begin != counts.type_slots)
                    return {status_code::initialization_failed};

                const auto object_begin = object_slot_cursor;
                for (const auto& slot : runtime.object_slots) {
                    if (!slot.identity)
                        continue;
                    if (object_slot_cursor >= object_slots.size())
                        return {status_code::initialization_failed};
                    object_slots[object_slot_cursor++] = slot;
                }

                if (object_slot_cursor - object_begin != counts.object_slots)
                    return {status_code::initialization_failed};

                const auto member_begin = member_slot_cursor;
                for (const auto& slot : runtime.member_slots) {
                    if (!slot.type)
                        continue;
                    if (member_slot_cursor >= member_slots.size())
                        return {status_code::initialization_failed};
                    member_slots[member_slot_cursor++] = slot;
                }

                if (member_slot_cursor - member_begin != counts.member_slots)
                    return {status_code::initialization_failed};
            }

            ++observed_frontends;
        }

        if (observed_frontends != summary.frontend_count ||
            local_type_cursor != local_types.size() ||
            type_slot_cursor != type_slots.size() ||
            object_slot_cursor != object_slots.size() ||
            member_slot_cursor != member_slots.size()) {
            return {status_code::initialization_failed};
        }

        const auto bind_range =
            [](const auto& values,
               source_frontend_native_persistence_range range) noexcept {
                using vector_type =
                    std::remove_reference_t<decltype(values)>;
                using value_type =
                    typename vector_type::value_type;

                if (range.count == 0)
                    return std::span<const value_type>{};

                return std::span<const value_type>{values}.subspan(
                    static_cast<std::size_t>(range.begin),
                    static_cast<std::size_t>(range.count));
            };

        for (std::size_t index = 0;
             index < source_count;
             ++index) {

            const auto& record = records[index];
            if (record.present == 0)
                continue;

            auto* interface_value =
                interfaces[index].get();

            if (interface_value == nullptr)
                return {status_code::initialization_failed};

            interface_value->bind_persistence_data({
                bind_range(local_types, record.local_types),
                bind_range(type_slots, record.type_slots),
                bind_range(object_slots, record.object_slots),
                bind_range(member_slots, record.member_slots),
            });
        }

        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}


} // namespace

source_frontend_cache::source_frontend_cache(
    const build_cache_image_view& baseline_cache_value,
    const source_manager_image_view& baseline_sources_value) noexcept
    : baseline_cache(&baseline_cache_value),
      baseline_sources(&baseline_sources_value),
      baseline_source_count(baseline_sources_value.source_count()),
      logical_source_count(baseline_sources_value.source_count()),
      persistence_summary_value{
          baseline_cache_value.frontend_count(),
          baseline_cache_value.frontend_local_type_count(),
          baseline_cache_value.frontend_type_slot_count(),
          baseline_cache_value.frontend_object_slot_count(),
          baseline_cache_value.frontend_member_slot_count()},
      complete_state(
          baseline_cache_value.frontend_complete() &&
          baseline_cache_value.source_count() == baseline_sources_value.source_count()) {}

const source_frontend_cache::overlay_entry* source_frontend_cache::find_overlay(
    source_id source) const noexcept {

    if (!source || overlay_index.empty())
        return nullptr;

    const auto mask = overlay_index.size() - 1;
    auto position = static_cast<std::size_t>(mix64(source.value())) & mask;
    for (std::size_t probe = 0; probe < overlay_index.size(); ++probe) {
        const auto& slot = overlay_index[position];
        if (!slot.source)
            return nullptr;
        if (slot.source == source) {
            const auto index = static_cast<std::size_t>(slot.position - 1);
            return index < overlay.size() ? &overlay[index] : nullptr;
        }
        position = (position + 1) & mask;
    }
    return nullptr;
}

source_frontend_cache::overlay_entry* source_frontend_cache::find_overlay(
    source_id source) noexcept {

    return const_cast<overlay_entry*>(
        static_cast<const source_frontend_cache&>(*this).find_overlay(source));
}

status source_frontend_cache::prepare_overlay_capacity(
    std::size_t additional) noexcept {

    if (additional == 0)
        return {};
    if (overlay.size() >
        (std::numeric_limits<std::size_t>::max)() - additional) {
        return {status_code::not_available};
    }

    try {
        const auto required = overlay.size() + additional;
        overlay.reserve(required);
        if (!overlay_index.empty() && required * 2 < overlay_index.size())
            return {};

        const auto capacity = overlay_capacity(required);
        if (capacity == 0)
            return {status_code::not_available};

        std::vector<overlay_slot> replacement(capacity);
        const auto mask = capacity - 1;
        for (std::uint32_t index = 0; index < overlay.size(); ++index) {
            const auto source = overlay[index].source;
            auto position = static_cast<std::size_t>(mix64(source.value())) & mask;
            while (replacement[position].source)
                position = (position + 1) & mask;
            replacement[position] = overlay_slot{source, index + 1};
        }
        overlay_index.swap(replacement);
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

source_frontend_cache::overlay_entry* source_frontend_cache::publish_overlay(
    source_id source,
    std::unique_ptr<source_interface> interface_value,
    bool resolved_value) noexcept {

    if (auto* existing = find_overlay(source)) {
        existing->interface = std::move(interface_value);
        existing->resolved = resolved_value;
        return existing;
    }

    if (!source || overlay.size() == overlay.capacity() || overlay_index.empty())
        return nullptr;

    const auto mask = overlay_index.size() - 1;
    auto position = static_cast<std::size_t>(mix64(source.value())) & mask;
    while (overlay_index[position].source)
        position = (position + 1) & mask;

    overlay.push_back(overlay_entry{
        source,
        std::move(interface_value),
        resolved_value});
    overlay_index[position] = overlay_slot{
        source,
        static_cast<std::uint32_t>(overlay.size())};
    return &overlay.back();
}

const source_interface* source_frontend_cache::materialize_baseline(
    source_id source) const noexcept {

    if (baseline_cache == nullptr || baseline_sources == nullptr || !source ||
        static_cast<std::size_t>(source.value()) > baseline_source_count) {
        return nullptr;
    }

    if (const auto* existing = find_overlay(source); existing != nullptr)
        return existing->resolved ? existing->interface.get() : nullptr;

    build_cache_source_record persisted;
    if (!baseline_cache->source(source, persisted).ok() ||
        !persisted.frontend_present) {
        return nullptr;
    }

    auto* self = const_cast<source_frontend_cache*>(this);
    if (!self->prepare_overlay_capacity(1).ok())
        return nullptr;

    // Publish an unresolved sentinel before walking imports. It detects a
    // corrupted include cycle without an arbitrary recursion-depth contract and
    // prevents duplicate materialization when several Sources share one import.
    if (self->publish_overlay(source, nullptr, false) == nullptr)
        return nullptr;

    try {
        const auto includes = baseline_sources->includes(source);
        std::vector<const source_interface*> imports;
        imports.reserve(includes.size());
        for (std::size_t index = 0; index < includes.size(); ++index) {
            const auto* imported = materialize_baseline(includes[index]);
            if (imported == nullptr)
                return nullptr;
            imports.push_back(imported);
        }

        auto value = std::make_unique<source_interface>();
        if (!value->initialize_persisted(*baseline_cache, source, imports).ok())
            return nullptr;

        auto* published = self->find_overlay(source);
        if (published == nullptr || published->resolved)
            return nullptr;

        published->interface = std::move(value);
        published->resolved = true;
        return published->interface.get();
    }
    catch (const std::bad_alloc&) {
        return nullptr;
    }
    catch (const std::length_error&) {
        return nullptr;
    }
}

const source_interface* source_frontend_cache::interface(source_id source) const noexcept {
    if (!source || static_cast<std::size_t>(source.value()) > logical_source_count)
        return nullptr;

    if (baseline_cache == nullptr) {
        const auto index = static_cast<std::size_t>(source.value() - 1);
        return index < interfaces.size() ? interfaces[index].get() : nullptr;
    }

    if (const auto* item = find_overlay(source); item != nullptr)
        return item->resolved ? item->interface.get() : nullptr;

    return materialize_baseline(source);
}

status source_frontend_cache::persistence_view(
    source_id source,
    source_frontend_persistence_view& output) const noexcept {

    output = {};
    if (!source ||
        static_cast<std::size_t>(source.value()) > logical_source_count) {
        return {status_code::not_found};
    }

    const source_interface* value = nullptr;

    if (baseline_cache == nullptr) {
        const auto index =
            static_cast<std::size_t>(source.value() - 1);
        value = index < interfaces.size()
            ? interfaces[index].get()
            : nullptr;
    }
    else if (const auto* item = find_overlay(source);
             item != nullptr) {

        if (!item->resolved)
            return {status_code::invalid_state};

        value = item->interface.get();
    }
    else if (static_cast<std::size_t>(source.value()) <=
             baseline_source_count) {

        build_cache_source_record persisted;
        const auto result =
            baseline_cache->source(source, persisted);
        if (!result.ok())
            return result;

        if (!persisted.frontend_present)
            return {};

        output.storage =
            source_frontend_persistence_storage::persisted_baseline;
        output.record.present = true;
        output.record.local_types = persisted.local_types.count;
        output.record.type_slots = persisted.type_slots.count;
        output.record.object_slots = persisted.object_slots.count;
        output.record.member_slots = persisted.member_slots.count;
        return {};
    }

    if (value == nullptr)
        return {};

    const auto counts =
        value->persistence_counts();
    const auto data =
        value->persistence_data_view();

    if (data.local_types.size() != counts.local_types ||
        data.type_slots.size() != counts.type_slots ||
        data.object_slots.size() != counts.object_slots ||
        data.member_slots.size() != counts.member_slots) {
        return {status_code::invalid_state};
    }

    output.storage =
        source_frontend_persistence_storage::native_interface;
    output.record.present = true;
    output.record.local_types = counts.local_types;
    output.record.type_slots = counts.type_slots;
    output.record.object_slots = counts.object_slots;
    output.record.member_slots = counts.member_slots;
    output.data = data;
    return {};
}

status source_frontend_cache::persistence_record(
    source_id source,
    source_frontend_persistence_record& output) const noexcept {

    source_frontend_persistence_view view;
    const auto result = persistence_view(source, view);

    if (!result.ok()) {
        output = {};
        return result;
    }

    output = view.record;
    return {};
}

status source_frontend_cache::persistence_local_type(
    source_id source,
    std::size_t index,
    identity_ref& output) const noexcept {

    output = {};
    if (!source || static_cast<std::size_t>(source.value()) > logical_source_count)
        return {status_code::not_found};

    if (baseline_cache != nullptr) {
        if (const auto* item = find_overlay(source); item != nullptr) {
            if (!item->resolved || item->interface == nullptr)
                return {status_code::not_found};
            const auto values = item->interface->data_view().local_types;
            if (index >= values.size())
                return {status_code::not_found};
            output = values[index];
            return {};
        }
        if (static_cast<std::size_t>(source.value()) <= baseline_source_count)
            return baseline_cache->frontend_local_type(source, index, output);
    }

    const auto dense_index = static_cast<std::size_t>(source.value() - 1);
    if (dense_index >= interfaces.size() || interfaces[dense_index] == nullptr)
        return {status_code::not_found};
    const auto values = interfaces[dense_index]->data_view().local_types;
    if (index >= values.size())
        return {status_code::not_found};
    output = values[index];
    return {};
}

status source_frontend_cache::persistence_type_slot(
    source_id source,
    std::size_t index,
    source_interface_type_slot& output) const noexcept {

    output = {};
    if (!source || static_cast<std::size_t>(source.value()) > logical_source_count)
        return {status_code::not_found};

    if (baseline_cache != nullptr) {
        if (const auto* item = find_overlay(source); item != nullptr) {
            if (!item->resolved || item->interface == nullptr)
                return {status_code::not_found};
            const auto values = item->interface->data_view().type_slots;
            if (index >= values.size())
                return {status_code::not_found};
            output = values[index];
            return {};
        }
        if (static_cast<std::size_t>(source.value()) <= baseline_source_count)
            return baseline_cache->frontend_type_slot(source, index, output);
    }

    const auto dense_index = static_cast<std::size_t>(source.value() - 1);
    if (dense_index >= interfaces.size() || interfaces[dense_index] == nullptr)
        return {status_code::not_found};
    const auto values = interfaces[dense_index]->data_view().type_slots;
    if (index >= values.size())
        return {status_code::not_found};
    output = values[index];
    return {};
}

status source_frontend_cache::persistence_object_slot(
    source_id source,
    std::size_t index,
    source_interface_object_slot& output) const noexcept {

    output = {};
    if (!source || static_cast<std::size_t>(source.value()) > logical_source_count)
        return {status_code::not_found};

    if (baseline_cache != nullptr) {
        if (const auto* item = find_overlay(source); item != nullptr) {
            if (!item->resolved || item->interface == nullptr)
                return {status_code::not_found};
            const auto values = item->interface->data_view().object_slots;
            if (index >= values.size())
                return {status_code::not_found};
            output = values[index];
            return {};
        }
        if (static_cast<std::size_t>(source.value()) <= baseline_source_count)
            return baseline_cache->frontend_object_slot(source, index, output);
    }

    const auto dense_index = static_cast<std::size_t>(source.value() - 1);
    if (dense_index >= interfaces.size() || interfaces[dense_index] == nullptr)
        return {status_code::not_found};
    const auto values = interfaces[dense_index]->data_view().object_slots;
    if (index >= values.size())
        return {status_code::not_found};
    output = values[index];
    return {};
}

status source_frontend_cache::persistence_member_slot(
    source_id source,
    std::size_t index,
    source_interface_member_slot& output) const noexcept {

    output = {};
    if (!source || static_cast<std::size_t>(source.value()) > logical_source_count)
        return {status_code::not_found};

    if (baseline_cache != nullptr) {
        if (const auto* item = find_overlay(source); item != nullptr) {
            if (!item->resolved || item->interface == nullptr)
                return {status_code::not_found};
            const auto values = item->interface->data_view().member_slots;
            if (index >= values.size())
                return {status_code::not_found};
            output = values[index];
            return {};
        }
        if (static_cast<std::size_t>(source.value()) <= baseline_source_count)
            return baseline_cache->frontend_member_slot(source, index, output);
    }

    const auto dense_index = static_cast<std::size_t>(source.value() - 1);
    if (dense_index >= interfaces.size() || interfaces[dense_index] == nullptr)
        return {status_code::not_found};
    const auto values = interfaces[dense_index]->data_view().member_slots;
    if (index >= values.size())
        return {status_code::not_found};
    output = values[index];
    return {};
}

source_frontend_cache_update source_frontend_cache::begin_update(
    bool full_reconstruction) noexcept {

    return source_frontend_cache_update{*this, full_reconstruction};
}

void source_frontend_cache::invalidate() noexcept {
    interfaces.clear();
    persistence_records.clear();
    persistence_local_types.clear();
    persistence_type_slots.clear();
    persistence_object_slots.clear();
    persistence_member_slots.clear();
    native_persistence_complete_state = false;
    overlay.clear();
    overlay_index.clear();
    persistence_summary_value = {};
    complete_state = false;
}

source_frontend_cache_update::~source_frontend_cache_update() {
    cancel();
}

source_frontend_cache_update::source_frontend_cache_update(
    source_frontend_cache_update&& other) noexcept
    : owner(std::exchange(other.owner, nullptr)),
      full_candidate(std::move(other.full_candidate)),
      full_persistence_records(
          std::move(other.full_persistence_records)),
      full_persistence_local_types(
          std::move(other.full_persistence_local_types)),
      full_persistence_type_slots(
          std::move(other.full_persistence_type_slots)),
      full_persistence_object_slots(
          std::move(other.full_persistence_object_slots)),
      full_persistence_member_slots(
          std::move(other.full_persistence_member_slots)),
      replacements(std::move(other.replacements)),
      replacement_index(std::move(other.replacement_index)),
      candidate_summary(other.candidate_summary),
      required_source_count(other.required_source_count),
      full_reconstruction(other.full_reconstruction),
      prepared(other.prepared),
      published(other.published),
      failure(other.failure) {}

status source_frontend_cache_update::ensure_replacement_index(
    std::size_t required) noexcept {

    if (required == 0)
        return {};

    if (!replacement_index.empty() &&
        required <= replacement_index.size() / 2) {
        return {};
    }

    const auto capacity =
        overlay_capacity(required);
    if (capacity == 0)
        return {status_code::not_available};

    try {
        std::vector<replacement_slot> replacement_slots(
            capacity);

        const auto mask = capacity - 1;
        for (std::uint32_t index = 0;
             index < replacements.size();
             ++index) {

            const auto source =
                replacements[index].source;
            auto position =
                static_cast<std::size_t>(
                    mix64(source.value())) & mask;

            while (replacement_slots[position].source)
                position = (position + 1) & mask;

            replacement_slots[position] = {
                source,
                index + 1,
            };
        }

        replacement_index.swap(replacement_slots);
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

source_frontend_cache_update::replacement*
source_frontend_cache_update::find_replacement(
    source_id source) noexcept {

    if (!source || replacement_index.empty())
        return nullptr;

    const auto mask =
        replacement_index.size() - 1;
    auto position =
        static_cast<std::size_t>(
            mix64(source.value())) & mask;

    for (std::size_t probe = 0;
         probe < replacement_index.size();
         ++probe) {

        const auto& slot =
            replacement_index[position];

        if (!slot.source)
            return nullptr;

        if (slot.source == source) {
            const auto index =
                static_cast<std::size_t>(
                    slot.position - 1);
            return index < replacements.size()
                ? &replacements[index]
                : nullptr;
        }

        position = (position + 1) & mask;
    }

    return nullptr;
}

status source_frontend_cache_update::replace(
    source_id source,
    std::unique_ptr<source_interface> interface_value) noexcept {

    if (!failure.ok())
        return failure;
    if (owner == nullptr || !source || prepared || published)
        return {status_code::invalid_argument};

    try {
        if (full_reconstruction) {
            if (owner->baseline_backed())
                return {status_code::invalid_state};

            const auto required =
                static_cast<std::size_t>(source.value());

            if (full_candidate.size() < required)
                full_candidate.resize(required);

            auto next_summary = candidate_summary;

            if (!subtract_persistence_record(
                    next_summary,
                    persistence_record_of(
                        full_candidate[required - 1].get())) ||
                !add_persistence_record(
                    next_summary,
                    persistence_record_of(
                        interface_value.get()))) {
                failure = {status_code::not_available};
                return failure;
            }

            full_candidate[required - 1] =
                std::move(interface_value);
            candidate_summary = next_summary;
        }
        else {
            auto result =
                ensure_replacement_index(
                    replacements.size() + 1);
            if (!result.ok()) {
                failure = result;
                return failure;
            }

            if (auto* existing =
                    find_replacement(source);
                existing != nullptr) {

                existing->interface =
                    std::move(interface_value);
                return {};
            }

            replacements.push_back(
                replacement{
                    source,
                    std::move(interface_value)});

            const auto mask =
                replacement_index.size() - 1;
            auto position =
                static_cast<std::size_t>(
                    mix64(source.value())) & mask;

            while (replacement_index[position].source)
                position = (position + 1) & mask;

            replacement_index[position] = {
                source,
                static_cast<std::uint32_t>(
                    replacements.size()),
            };
        }
        return {};
    }
    catch (const std::bad_alloc&) {
        failure = {status_code::not_available};
    }
    catch (const std::length_error&) {
        failure = {status_code::not_available};
    }
    return failure;
}

status source_frontend_cache_update::prepare_publish(
    std::size_t required_count) noexcept {

    if (!failure.ok())
        return failure;
    if (owner == nullptr || prepared || published)
        return {status_code::invalid_argument};
    if (!full_reconstruction && !owner->complete_state)
        return {status_code::not_available};

    try {
        required_source_count = required_count;

        if (!full_reconstruction) {
            candidate_summary =
                owner->persistence_summary_value;

            for (const auto& item : replacements) {
                source_frontend_persistence_record previous;
                auto summary_result =
                    owner->persistence_record(
                        item.source,
                        previous);

                if (!summary_result.ok() &&
                    summary_result.code ==
                        status_code::not_found &&
                    static_cast<std::size_t>(
                        item.source.value()) >
                        owner->logical_source_count) {

                    summary_result = {};
                }

                if (!summary_result.ok() &&
                    summary_result.code ==
                        status_code::invalid_state &&
                    owner->baseline_cache != nullptr &&
                    static_cast<std::size_t>(
                        item.source.value()) <=
                        owner->baseline_source_count) {

                    build_cache_source_record persisted;
                    summary_result =
                        owner->baseline_cache->source(
                            item.source,
                            persisted);

                    if (summary_result.ok() &&
                        persisted.frontend_present) {
                        previous.present = true;
                        previous.local_types =
                            persisted.local_types.count;
                        previous.type_slots =
                            persisted.type_slots.count;
                        previous.object_slots =
                            persisted.object_slots.count;
                        previous.member_slots =
                            persisted.member_slots.count;
                    }
                }

                if (!summary_result.ok())
                    return summary_result;

                auto next_summary =
                    candidate_summary;

                if (!subtract_persistence_record(
                        next_summary,
                        previous)) {
                    return {
                        status_code::initialization_failed};
                }

                if (!add_persistence_record(
                        next_summary,
                        persistence_record_of(
                            item.interface.get()))) {
                    return {status_code::not_available};
                }

                candidate_summary = next_summary;
            }
        }

        if (full_reconstruction) {
            if (owner->baseline_backed())
                return {status_code::invalid_state};
            if (full_candidate.size() < required_source_count)
                full_candidate.resize(required_source_count);
            std::size_t capacity = 0;
            if (!cache_headroom(required_source_count, capacity))
                return {status_code::not_available};
            full_candidate.reserve(capacity);

            // GEN-02C18: full publication canonicalizes compact persistence
            // payload into cache-wide arenas before the Generation becomes
            // visible. SAVE can then serialize four dense spans directly.
            const auto arena_result =
                build_native_persistence_storage(
                    full_candidate,
                    required_source_count,
                    candidate_summary,
                    full_persistence_records,
                    full_persistence_local_types,
                    full_persistence_type_slots,
                    full_persistence_object_slots,
                    full_persistence_member_slots);

            if (!arena_result.ok()) {
                failure = arena_result;
                return failure;
            }
        } else if (owner->baseline_backed()) {
            if (required_source_count < owner->baseline_source_count)
                return {status_code::invalid_argument};
            auto result = owner->prepare_overlay_capacity(replacements.size());
            if (!result.ok())
                return result;
            for (const auto& item : replacements) {
                if (!item.source ||
                    static_cast<std::size_t>(item.source.value()) > required_source_count) {
                    return {status_code::invalid_argument};
                }
            }
        } else {
            if (required_source_count > owner->interfaces.capacity())
                return {status_code::rebuild_required};
            for (const auto& item : replacements) {
                if (!item.source ||
                    static_cast<std::size_t>(item.source.value()) > required_source_count) {
                    return {status_code::invalid_argument};
                }
            }
        }
        prepared = true;
        return {};
    }
    catch (const std::bad_alloc&) {
        failure = {status_code::not_available};
    }
    catch (const std::length_error&) {
        failure = {status_code::not_available};
    }
    return failure;
}

void source_frontend_cache_update::publish_prepared() noexcept {
    if (!prepared || published || owner == nullptr)
        return;

    if (full_reconstruction) {
        owner->interfaces.swap(full_candidate);

        owner->persistence_records.swap(
            full_persistence_records);
        owner->persistence_local_types.swap(
            full_persistence_local_types);
        owner->persistence_type_slots.swap(
            full_persistence_type_slots);
        owner->persistence_object_slots.swap(
            full_persistence_object_slots);
        owner->persistence_member_slots.swap(
            full_persistence_member_slots);

        owner->native_persistence_complete_state = true;
        owner->logical_source_count = required_source_count;
    } else if (owner->baseline_backed()) {
        for (auto& item : replacements) {
            if (owner->publish_overlay(
                    item.source,
                    std::move(item.interface),
                    true) == nullptr) {
                return;
            }
        }
        owner->logical_source_count = required_source_count;
    } else {
        const bool invalidates_bulk_persistence =
            !replacements.empty() ||
            required_source_count !=
                owner->logical_source_count;

        while (owner->interfaces.size() < required_source_count)
            owner->interfaces.emplace_back();
        for (auto& item : replacements)
            owner->interfaces[item.source.value() - 1] = std::move(item.interface);

        // Do not clear the arenas here. Unchanged interfaces may hold spans
        // into them. Only disable the bulk directory because replaced Source
        // ranges no longer describe the complete current Generation.
        if (invalidates_bulk_persistence)
            owner->native_persistence_complete_state = false;

        owner->logical_source_count = required_source_count;
    }

    owner->persistence_summary_value =
        candidate_summary;
    owner->complete_state = true;
    published = true;
    prepared = false;
    owner = nullptr;
}

void source_frontend_cache_update::cancel() noexcept {
    if (owner == nullptr || published)
        return;
    owner = nullptr;
    prepared = false;
}

} // namespace cw::server
