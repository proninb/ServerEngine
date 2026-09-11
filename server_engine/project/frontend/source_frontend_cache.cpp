#include "source_frontend_cache.hpp"

#include "../persistence/build_cache_image.hpp"
#include "../persistence/source_manager_image.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <new>
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

} // namespace

source_frontend_cache::source_frontend_cache(
    const build_cache_image_view& baseline_cache_value,
    const source_manager_image_view& baseline_sources_value) noexcept
    : baseline_cache(&baseline_cache_value),
      baseline_sources(&baseline_sources_value),
      baseline_source_count(baseline_sources_value.source_count()),
      logical_source_count(baseline_sources_value.source_count()),
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

status source_frontend_cache::persistence_record(
    source_id source,
    source_frontend_persistence_record& output) const noexcept {

    output = {};
    if (!source || static_cast<std::size_t>(source.value()) > logical_source_count)
        return {status_code::not_found};

    const source_interface* value = nullptr;
    if (baseline_cache == nullptr) {
        const auto index = static_cast<std::size_t>(source.value() - 1);
        value = index < interfaces.size() ? interfaces[index].get() : nullptr;
    } else if (const auto* item = find_overlay(source); item != nullptr) {
        if (!item->resolved)
            return {status_code::invalid_state};
        value = item->interface.get();
    } else if (static_cast<std::size_t>(source.value()) <= baseline_source_count) {
        build_cache_source_record persisted;
        const auto result = baseline_cache->source(source, persisted);
        if (!result.ok())
            return result;
        if (!persisted.frontend_present)
            return {};

        output.present = true;
        output.local_types = persisted.local_types.count;
        output.type_slots = persisted.type_slots.count;
        output.object_slots = persisted.object_slots.count;
        output.member_slots = persisted.member_slots.count;
        return {};
    }

    if (value == nullptr)
        return {};

    const auto data = value->data_view();
    output.present = true;
    output.local_types = data.local_types.size();
    output.type_slots = data.type_slots.size();
    output.object_slots = data.object_slots.size();
    output.member_slots = data.member_slots.size();
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
    overlay.clear();
    overlay_index.clear();
    complete_state = false;
}

source_frontend_cache_update::~source_frontend_cache_update() {
    cancel();
}

source_frontend_cache_update::source_frontend_cache_update(
    source_frontend_cache_update&& other) noexcept
    : owner(std::exchange(other.owner, nullptr)),
      full_candidate(std::move(other.full_candidate)),
      replacements(std::move(other.replacements)),
      required_source_count(other.required_source_count),
      full_reconstruction(other.full_reconstruction),
      prepared(other.prepared),
      published(other.published),
      failure(other.failure) {}

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
            const auto required = static_cast<std::size_t>(source.value());
            if (full_candidate.size() < required)
                full_candidate.resize(required);
            full_candidate[required - 1] = std::move(interface_value);
        } else {
            replacements.push_back(replacement{source, std::move(interface_value)});
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
        if (full_reconstruction) {
            if (owner->baseline_backed())
                return {status_code::invalid_state};
            if (full_candidate.size() < required_source_count)
                full_candidate.resize(required_source_count);
            std::size_t capacity = 0;
            if (!cache_headroom(required_source_count, capacity))
                return {status_code::not_available};
            full_candidate.reserve(capacity);
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
        while (owner->interfaces.size() < required_source_count)
            owner->interfaces.emplace_back();
        for (auto& item : replacements)
            owner->interfaces[item.source.value() - 1] = std::move(item.interface);
        owner->logical_source_count = required_source_count;
    }

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
