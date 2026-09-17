#include "graph.hpp"

#include "../persistence/build_cache_image.hpp"
#include "../persistence/compiled_image.hpp"

#include <cstdint>
#include <type_traits>
#include <limits>

namespace cw::server {
namespace {

[[nodiscard]] std::uint64_t mix64(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

[[nodiscard]] std::uint32_t fold32(std::uint64_t value) noexcept {
    auto result = static_cast<std::uint32_t>(value ^ (value >> 32));
    return result == 0 ? 1u : result;
}

[[nodiscard]] std::uint64_t identity_hash(identity_ref identity) noexcept {
    return mix64(static_cast<std::uint64_t>(identity.value()));
}

[[nodiscard]] std::uint64_t derived_hash(
    derived_type_kind kind,
    std::uint32_t child,
    std::uint64_t payload) noexcept {

    return mix64(
        (static_cast<std::uint64_t>(static_cast<std::uint8_t>(kind)) << 56) ^
        (static_cast<std::uint64_t>(child) << 16) ^ mix64(payload));
}

template<class SlotContainer, class IdentityContainer>
void insert_identity_index(
    SlotContainer& slots,
    const IdentityContainer& identities,
    identity_ref identity,
    std::uint32_t handle) noexcept {

    const auto hash = identity_hash(identity);
    const auto fingerprint = fold32(hash);
    const auto mask = slots.size() - 1;
    auto position = static_cast<std::size_t>(hash) & mask;
    for (;;) {
        auto& slot = slots[position];
        if (slot.handle == 0) {
            slot.fingerprint = fingerprint;
            slot.handle = handle;
            return;
        }
        if (slot.fingerprint == fingerprint && slot.handle <= identities.size() &&
            identities[slot.handle - 1] == identity) {
            return;
        }
        position = (position + 1) & mask;
    }
}

template<class SlotContainer, class IdentityContainer>
void insert_object_identity_index(
    SlotContainer& slots,
    const IdentityContainer& identities,
    identity_ref identity,
    std::uint32_t handle) noexcept {

    const auto hash = identity_hash(identity);
    const auto fingerprint = fold32(hash);
    const auto mask = slots.size() - 1;
    auto position = static_cast<std::size_t>(hash) & mask;
    for (;;) {
        auto& slot = slots[position];
        if (slot.handle == 0) {
            slot.fingerprint = fingerprint;
            slot.handle = handle;
            return;
        }
        if (slot.fingerprint == fingerprint && slot.handle <= identities.size() &&
            identities[slot.handle - 1] == identity) {
            return;
        }
        position = (position + 1) & mask;
    }
}

[[nodiscard]] std::uint64_t endpoint_hash(object_endpoint endpoint) noexcept {
    return mix64(
        (static_cast<std::uint64_t>(endpoint.object.value()) << 32) ^
        static_cast<std::uint64_t>(endpoint.member.value()));
}

template<class SlotContainer, class LinkContainer>
void insert_link_index(
    SlotContainer& slots,
    const LinkContainer& links,
    object_endpoint target,
    std::uint32_t handle) noexcept {

    const auto hash = endpoint_hash(target);
    const auto fingerprint = fold32(hash);
    const auto mask = slots.size() - 1;
    auto position = static_cast<std::size_t>(hash) & mask;
    for (;;) {
        auto& slot = slots[position];
        if (slot.handle == 0) {
            slot.fingerprint = fingerprint;
            slot.handle = handle;
            return;
        }
        if (slot.fingerprint == fingerprint && slot.handle <= links.size() &&
            links[slot.handle - 1].target == target) {
            return;
        }
        position = (position + 1) & mask;
    }
}

template<class SlotContainer, class CanonicalContainer>
void insert_derived_index(
    SlotContainer& slots,
    const CanonicalContainer& canonical_types,
    std::uint32_t type_ref) noexcept {

    const auto& record = canonical_types[type_ref];
    const auto kind = static_cast<derived_type_kind>(record.detail);
    const auto hash = derived_hash(kind, record.child_or_handle, record.payload);
    const auto fingerprint = fold32(hash);
    const auto mask = slots.size() - 1;
    auto position = static_cast<std::size_t>(hash) & mask;
    for (;;) {
        auto& slot = slots[position];
        if (slot.type_ref == 0) {
            slot.fingerprint = fingerprint;
            slot.type_ref = type_ref;
            return;
        }
        position = (position + 1) & mask;
    }
}

template<class T>
void append_prepared(mapped_vector<T>& target, const std::vector<T>& source) noexcept {
    for (const auto& value : source)
        target.push_back(value);
}

} // namespace

graph::graph(
    const compiled_image_view& compiled,
    const build_cache_image_view& build_cache) noexcept
    : baseline_compiled(&compiled),
      baseline_build_cache(&build_cache),
      derived_index_entries(build_cache.derived_index_entries()),
      live_type_count(compiled.type_count()),
      live_object_count(compiled.object_count()),
      live_link_count(compiled.link_count()) {

    types.bind_baseline(
        &compiled, compiled.type_slot_count(),
        [](const void* context, std::size_t index, type_entry& output) noexcept {
            if (index >= (std::numeric_limits<std::uint32_t>::max)())
                return status{status_code::artifact_corrupt};
            compiled_image_type_record value;
            const auto result = static_cast<const compiled_image_view*>(context)->type_raw(
                type_handle{static_cast<std::uint32_t>(index + 1)}, value);
            if (!result.ok())
                return result;
            output.definition = value.definition;
            output.kind = value.kind;
            output.record_kind = value.record_kind;
            output.enum_underlying = value.enum_underlying;
            output.flags = value.flags;
            return status{};
        });
    identities.bind_baseline(
        &compiled, compiled.type_slot_count(),
        [](const void* context, std::size_t index, identity_ref& output) noexcept {
            output = static_cast<const compiled_image_view*>(context)->type_identity_at_slot(index);
            return output ? status{} : status{status_code::artifact_corrupt};
        });
    member_records.bind_baseline(
        &compiled, compiled.member_slot_count(),
        [](const void* context, std::size_t index, member_record& output) noexcept {
            compiled_image_member_record value;
            const auto result = static_cast<const compiled_image_view*>(context)->member_at_slot(index, value);
            if (!result.ok())
                return result;
            output = member_record{value.name, value.type, value.access};
            return status{};
        });
    enum_value_records.bind_baseline(
        &compiled, compiled.enum_value_slot_count(),
        [](const void* context, std::size_t index, enum_value_record& output) noexcept {
            compiled_image_enum_value_record value;
            const auto result = static_cast<const compiled_image_view*>(context)->enum_value_at_slot(index, value);
            if (!result.ok())
                return result;
            output = enum_value_record{value.bits, value.name, value.intrinsic};
            return status{};
        });
    object_entries.bind_baseline(
        &compiled, compiled.object_slot_count(),
        [](const void* context, std::size_t index, object_entry& output) noexcept {
            if (index >= (std::numeric_limits<std::uint32_t>::max)())
                return status{status_code::artifact_corrupt};
            compiled_image_object_record value;
            const auto result = static_cast<const compiled_image_view*>(context)->object_raw(
                object_handle{static_cast<std::uint32_t>(index + 1)}, value);
            if (!result.ok())
                return result;
            output = object_entry{value.type, value.flags};
            return status{};
        });
    object_identities.bind_baseline(
        &compiled, compiled.object_slot_count(),
        [](const void* context, std::size_t index, identity_ref& output) noexcept {
            output = static_cast<const compiled_image_view*>(context)->object_identity_at_slot(index);
            return output ? status{} : status{status_code::artifact_corrupt};
        });
    link_records.bind_baseline(
        &compiled, compiled.link_slot_count(),
        [](const void* context, std::size_t index, link_record& output) noexcept {
            if (index >= (std::numeric_limits<std::uint32_t>::max)())
                return status{status_code::artifact_corrupt};
            compiled_image_link_record value;
            const auto result = static_cast<const compiled_image_view*>(context)->link_raw(
                link_handle{static_cast<std::uint32_t>(index + 1)}, value);
            if (!result.ok())
                return result;
            output = link_record{value.source, value.target};
            return status{};
        });
    canonical_types.bind_baseline(
        &compiled, compiled.canonical_type_slot_count(),
        [](const void* context, std::size_t index, graph_canonical_type_record& output) noexcept {
            compiled_image_canonical_type_record value;
            const auto result = static_cast<const compiled_image_view*>(context)->canonical_type_at_slot(index, value);
            if (!result.ok())
                return result;
            output.payload = value.payload;
            output.child_or_handle = value.child_or_handle;
            output.kind = value.kind;
            output.detail = value.detail;
            output.reserved = 0;
            return status{};
        });

    identity_index.bind_baseline(
        &build_cache, build_cache.type_identity_index_slot_count(),
        [](const void* context, std::size_t index, graph_identity_index_slot& output) noexcept {
            return static_cast<const build_cache_image_view*>(context)->type_identity_index_slot(index, output);
        });
    object_identity_index.bind_baseline(
        &build_cache, build_cache.object_identity_index_slot_count(),
        [](const void* context, std::size_t index, graph_object_identity_index_slot& output) noexcept {
            return static_cast<const build_cache_image_view*>(context)->object_identity_index_slot(index, output);
        });
    link_index.bind_baseline(
        &build_cache, build_cache.link_target_index_slot_count(),
        [](const void* context, std::size_t index, graph_link_index_slot& output) noexcept {
            return static_cast<const build_cache_image_view*>(context)->link_target_index_slot(index, output);
        });

    named_refs.bind_baseline(
        &build_cache, build_cache.named_ref_count(),
        [](const void* context, std::size_t index, TypeRef& output) noexcept {
            if (index == 0) {
                output = {};
                return status{};
            }
            if (index > (std::numeric_limits<std::uint32_t>::max)())
                return status{status_code::artifact_corrupt};
            output = static_cast<const build_cache_image_view*>(context)->named_ref(
                type_handle{static_cast<std::uint32_t>(index)});
            return status{};
        });
    derived_index.bind_baseline(
        &build_cache, build_cache.derived_index_slot_count(),
        [](const void* context, std::size_t index, graph_derived_index_slot& output) noexcept {
            build_cache_derived_index_slot value;
            const auto result = static_cast<const build_cache_image_view*>(context)->derived_index_slot(index, value);
            if (!result.ok())
                return result;
            output = graph_derived_index_slot{value.fingerprint, value.type.value()};
            return status{};
        });
    dependency_versions.bind_baseline(
        &build_cache, build_cache.dependency_version_count(),
        [](const void* context, std::size_t index, std::uint32_t& output) noexcept {
            if (index >= (std::numeric_limits<std::uint32_t>::max)())
                return status{status_code::artifact_corrupt};
            output = static_cast<const build_cache_image_view*>(context)->dependency_version(
                type_handle{static_cast<std::uint32_t>(index + 1)});
            return status{};
        });
    reverse_dependency_heads.bind_baseline(
        &build_cache, build_cache.dependency_version_count(),
        [](const void* context, std::size_t index, std::uint32_t& output) noexcept {
            if (index >= (std::numeric_limits<std::uint32_t>::max)())
                return status{status_code::artifact_corrupt};
            output = static_cast<const build_cache_image_view*>(context)->reverse_dependency_head(
                type_handle{static_cast<std::uint32_t>(index + 1)});
            return status{};
        });
    dependency_edges.bind_baseline(
        &build_cache, build_cache.dependency_edge_count(),
        [](const void* context, std::size_t index, graph_dependency_edge& output) noexcept {
            return static_cast<const build_cache_image_view*>(context)->dependency_edge(index, output);
        });

    for (std::size_t index = 0; index < intrinsic_refs.size(); ++index)
        intrinsic_refs[index] = build_cache.intrinsic_ref(static_cast<intrinsic_type>(index));
}

status graph::release_compiled_generation_storage(
    compiled_graph_generation_storage& output) noexcept {

    output = {};

    if (baseline_compiled != nullptr ||
        baseline_build_cache != nullptr ||
        types.baseline_backed() ||
        identities.baseline_backed() ||
        member_records.baseline_backed() ||
        enum_value_records.baseline_backed() ||
        object_entries.baseline_backed() ||
        object_identities.baseline_backed() ||
        link_records.baseline_backed() ||
        canonical_types.baseline_backed() ||
        identity_index.baseline_backed() ||
        object_identity_index.baseline_backed() ||
        link_index.baseline_backed()) {
        return {status_code::invalid_state};
    }

    output.live_types = live_type_count;
    output.live_objects = live_object_count;
    output.live_links = live_link_count;

    output.types = types.release_local_values();
    output.type_identities =
        identities.release_local_values();
    output.members =
        member_records.release_local_values();
    output.enum_values =
        enum_value_records.release_local_values();
    output.objects =
        object_entries.release_local_values();
    output.object_identities =
        object_identities.release_local_values();
    output.links =
        link_records.release_local_values();
    output.canonical_types =
        canonical_types.release_local_values();
    output.type_index =
        identity_index.release_local_values();
    output.object_index =
        object_identity_index.release_local_values();
    output.link_index =
        link_index.release_local_values();

    live_type_count = 0;
    live_object_count = 0;
    live_link_count = 0;

    return {};
}


type_handle graph::type_at(std::size_t index) const noexcept {
    if (index >= types.size() || index >= 0xffffffffu)
        return {};

    type_entry entry;
    return types.read(index, entry).ok() && entry.live()
        ? type_handle{static_cast<std::uint32_t>(index + 1)}
        : type_handle{};
}

const type_entry* graph::find_raw(type_handle handle) const noexcept {
    if (!handle || handle.value() > types.size())
        return nullptr;
    return &types[handle.value() - 1];
}

const type_entry* graph::find(type_handle handle) const noexcept {
    const auto* entry = find_raw(handle);
    return entry != nullptr && entry->live() ? entry : nullptr;
}

identity_ref graph::identity_raw(type_handle handle) const noexcept {
    if (!handle || handle.value() > identities.size())
        return {};

    identity_ref output;
    return identities.read(handle.value() - 1, output).ok()
        ? output
        : identity_ref{};
}

identity_ref graph::identity(type_handle handle) const noexcept {
    if (!handle || handle.value() > types.size())
        return {};

    type_entry entry;
    if (!types.read(handle.value() - 1, entry).ok() || !entry.live())
        return {};
    return identity_raw(handle);
}

type_handle graph::find_identity(identity_ref identity_value) const noexcept {
    if (!identity_value || identity_index.empty())
        return {};

    const auto hash = identity_hash(identity_value);
    const auto fingerprint = fold32(hash);
    const auto mask = identity_index.size() - 1;
    auto position = static_cast<std::size_t>(hash) & mask;

    for (std::size_t probe = 0; probe < identity_index.size(); ++probe) {
        graph_identity_index_slot slot;
        if (!identity_index.read(position, slot).ok())
            return {};
        if (slot.handle == 0)
            return {};

        if (slot.fingerprint == fingerprint &&
            slot.handle <= identities.size()) {
            identity_ref candidate;
            if (identities.read(slot.handle - 1, candidate).ok() &&
                candidate == identity_value) {
                return type_handle{slot.handle};
            }
        }
        position = (position + 1) & mask;
    }
    return {};
}

type_handle graph::find_type(identity_ref identity_value) const noexcept {
    const auto handle = find_identity(identity_value);
    if (!handle)
        return {};

    type_entry entry;
    return types.read(handle.value() - 1, entry).ok() && entry.live()
        ? handle
        : type_handle{};
}

std::span<const member_record> graph::members(type_handle handle) const noexcept {
    const auto* entry = find(handle);
    if (entry == nullptr || entry->kind != graph_type_kind::record || !entry->definition)
        return {};

    const auto begin = static_cast<std::size_t>(entry->definition.begin - 1);
    const auto count = static_cast<std::size_t>(entry->definition.count);
    if (begin > member_records.size() || count > member_records.size() - begin || count == 0)
        return {};
    return member_records.span(begin, count);
}

std::span<const enum_value_record> graph::enum_values(type_handle handle) const noexcept {
    const auto* entry = find(handle);
    if (entry == nullptr || entry->kind != graph_type_kind::enumeration || !entry->definition)
        return {};

    const auto begin = static_cast<std::size_t>(entry->definition.begin - 1);
    const auto count = static_cast<std::size_t>(entry->definition.count);
    if (begin > enum_value_records.size() || count > enum_value_records.size() - begin || count == 0)
        return {};
    return enum_value_records.span(begin, count);
}

member_index graph::find_member(type_handle handle, string_id name) const noexcept {
    if (!handle || !name || handle.value() > types.size())
        return {};

    type_entry entry;
    if (!types.read(handle.value() - 1, entry).ok() ||
        !entry.live() ||
        entry.kind != graph_type_kind::record ||
        !entry.definition) {
        return {};
    }

    const auto begin = static_cast<std::size_t>(entry.definition.begin - 1);
    const auto count = static_cast<std::size_t>(entry.definition.count);
    if (begin > member_records.size() || count > member_records.size() - begin)
        return {};

    for (std::size_t index = 0; index < count; ++index) {
        member_record value;
        if (!member_records.read(begin + index, value).ok())
            return {};
        if (value.name == name &&
            index <= (std::numeric_limits<std::uint32_t>::max)()) {
            return member_index::from_zero_based(
                static_cast<std::uint32_t>(index));
        }
    }
    return {};
}

object_handle graph::object_at(std::size_t index) const noexcept {
    if (index >= object_entries.size() || index >= 0xffffffffu)
        return {};

    object_entry entry;
    return object_entries.read(index, entry).ok() && entry.live()
        ? object_handle{static_cast<std::uint32_t>(index + 1)}
        : object_handle{};
}

const object_entry* graph::find(object_handle handle) const noexcept {
    if (!handle || handle.value() > object_entries.size())
        return nullptr;
    const auto& entry = object_entries[handle.value() - 1];
    return entry.live() ? &entry : nullptr;
}

bool graph::object_type(
    object_handle handle,
    TypeRef& output) const noexcept {

    output = {};
    if (!handle || handle.value() > object_entries.size())
        return false;

    object_entry entry;
    if (!object_entries.read(handle.value() - 1, entry).ok() || !entry.live())
        return false;

    output = entry.type;
    return static_cast<bool>(output);
}

identity_ref graph::identity(object_handle handle) const noexcept {
    if (!handle || handle.value() > object_entries.size() ||
        handle.value() > object_identities.size()) {
        return {};
    }

    object_entry entry;
    if (!object_entries.read(handle.value() - 1, entry).ok() || !entry.live())
        return {};

    identity_ref output;
    return object_identities.read(handle.value() - 1, output).ok()
        ? output
        : identity_ref{};
}

object_handle graph::find_object_identity(identity_ref identity_value) const noexcept {
    if (!identity_value || object_identity_index.empty())
        return {};

    const auto hash = identity_hash(identity_value);
    const auto fingerprint = fold32(hash);
    const auto mask = object_identity_index.size() - 1;
    auto position = static_cast<std::size_t>(hash) & mask;

    for (std::size_t probe = 0; probe < object_identity_index.size(); ++probe) {
        graph_object_identity_index_slot slot;
        if (!object_identity_index.read(position, slot).ok())
            return {};
        if (slot.handle == 0)
            return {};

        if (slot.fingerprint == fingerprint &&
            slot.handle <= object_identities.size()) {
            identity_ref candidate;
            if (object_identities.read(slot.handle - 1, candidate).ok() &&
                candidate == identity_value) {
                return object_handle{slot.handle};
            }
        }
        position = (position + 1) & mask;
    }
    return {};
}

object_handle graph::find_object(identity_ref identity_value) const noexcept {
    const auto handle = find_object_identity(identity_value);
    if (!handle)
        return {};

    object_entry entry;
    return object_entries.read(handle.value() - 1, entry).ok() && entry.live()
        ? handle
        : object_handle{};
}

const link_record* graph::find(link_handle handle) const noexcept {
    if (!handle || handle.value() > link_records.size())
        return nullptr;
    const auto& value = link_records[handle.value() - 1];
    return value.live() ? &value : nullptr;
}

link_handle graph::find_link_raw(object_endpoint target) const noexcept {
    if (!target.object || !target.member || link_index.empty())
        return {};

    const auto hash = endpoint_hash(target);
    const auto fingerprint = fold32(hash);
    const auto mask = link_index.size() - 1;
    auto position = static_cast<std::size_t>(hash) & mask;

    for (std::size_t probe = 0; probe < link_index.size(); ++probe) {
        graph_link_index_slot slot;
        if (!link_index.read(position, slot).ok())
            return {};
        if (slot.handle == 0)
            return {};

        if (slot.fingerprint == fingerprint && slot.handle <= link_records.size()) {
            link_record value;
            if (link_records.read(slot.handle - 1, value).ok() &&
                value.target == target) {
                return link_handle{slot.handle};
            }
        }
        position = (position + 1) & mask;
    }
    return {};
}

link_handle graph::find_link(object_endpoint target) const noexcept {
    const auto handle = find_link_raw(target);
    if (!handle)
        return {};

    link_record value;
    return link_records.read(handle.value() - 1, value).ok() && value.live()
        ? handle
        : link_handle{};
}

canonical_type_kind graph::kind(TypeRef type) const noexcept {
    if (!type || type.value() >= canonical_types.size())
        return canonical_type_kind::intrinsic;

    graph_canonical_type_record record;
    return canonical_types.read(type.value(), record).ok()
        ? record.kind
        : canonical_type_kind::intrinsic;
}

bool graph::intrinsic(TypeRef type, intrinsic_type& output) const noexcept {
    output = intrinsic_type::none;
    if (!type || type.value() >= canonical_types.size())
        return false;

    graph_canonical_type_record record;
    if (!canonical_types.read(type.value(), record).ok() ||
        record.kind != canonical_type_kind::intrinsic) {
        return false;
    }

    output = static_cast<intrinsic_type>(record.detail);
    return true;
}

bool graph::named_raw(TypeRef type, type_handle& output) const noexcept {
    output = {};
    if (!type || type.value() >= canonical_types.size())
        return false;

    graph_canonical_type_record record;
    if (!canonical_types.read(type.value(), record).ok() ||
        record.kind != canonical_type_kind::named ||
        record.child_or_handle == 0 ||
        record.child_or_handle > types.size()) {
        return false;
    }

    output = type_handle{record.child_or_handle};
    return true;
}

bool graph::named(TypeRef type, type_handle& output) const noexcept {
    if (!named_raw(type, output))
        return false;

    type_entry entry;
    if (!types.read(output.value() - 1, entry).ok() || !entry.live()) {
        output = {};
        return false;
    }
    return true;
}

bool graph::derived(TypeRef type, derived_type_record& output) const noexcept {
    output = {};
    if (!type || type.value() >= canonical_types.size())
        return false;

    graph_canonical_type_record record;
    if (!canonical_types.read(type.value(), record).ok() ||
        record.kind != canonical_type_kind::derived ||
        record.child_or_handle == 0 ||
        record.child_or_handle >= canonical_types.size()) {
        return false;
    }

    output.payload = record.payload;
    output.child = TypeRef{record.child_or_handle};
    output.kind = static_cast<derived_type_kind>(record.detail);
    return true;
}

graph_storage_usage graph::storage_usage(
    std::size_t live_members,
    std::size_t live_enum_values) const noexcept {

    graph_storage_usage output;
    const auto add_vector = [&](const auto& values) noexcept {
        using value_type = typename std::remove_reference_t<decltype(values)>::value_type;
        output.retained_bytes += values.heap_record_capacity() * sizeof(value_type);
        if (values.local_capacity() > values.local_size())
            output.reserve_bytes += (values.local_capacity() - values.local_size()) * sizeof(value_type);
    };
    const auto add_index = [&](const auto& values, std::size_t entries) noexcept {
        using value_type = typename std::remove_reference_t<decltype(values)>::value_type;
        output.retained_bytes += values.heap_record_capacity() * sizeof(value_type);
        if (!values.baseline_backed()) {
            const auto safe_entries = values.size() / 2;
            if (safe_entries > entries)
                output.reserve_bytes += (safe_entries - entries) * sizeof(value_type);
        }
    };
    const auto stale_count = [](std::size_t physical, std::size_t live) noexcept {
        return physical > live ? physical - live : std::size_t{0};
    };

    add_vector(types);
    add_vector(identities);
    add_vector(member_records);
    add_vector(enum_value_records);
    add_vector(object_entries);
    add_vector(object_identities);
    add_vector(link_records);
    add_vector(canonical_types);
    add_vector(named_refs);
    add_vector(dependency_versions);
    add_vector(reverse_dependency_heads);
    add_vector(dependency_edges);

    add_index(identity_index, identities.size());
    add_index(object_identity_index, object_identities.size());
    add_index(link_index, link_records.size());
    add_index(derived_index, derived_index_entries);

    const auto stale_types = stale_count(types.size(), live_type_count);
    output.stale_bytes += stale_types * (
        sizeof(type_entry) + sizeof(identity_ref) + sizeof(TypeRef) +
        sizeof(std::uint32_t) + sizeof(std::uint32_t));
    output.stale_bytes += stale_count(member_records.size(), live_members) * sizeof(member_record);
    output.stale_bytes += stale_count(enum_value_records.size(), live_enum_values) * sizeof(enum_value_record);
    output.stale_bytes += stale_count(object_entries.size(), live_object_count) *
        (sizeof(object_entry) + sizeof(identity_ref));
    output.stale_bytes += stale_count(link_records.size(), live_link_count) * sizeof(link_record);

    // Every live record member can contribute at most one named-type dependency
    // edge. Anything beyond that bound is certainly stale append-only history.
    output.stale_bytes += stale_count(dependency_edges.size(), live_members) * sizeof(graph_dependency_edge);
    return output;
}

status graph::prepare_sparse_publication(
    prepared_graph_update& prepared) noexcept {

    try {
        if (!baseline_backed()) {
            if (member_records.size() + prepared.members.size() > member_records.capacity() ||
                enum_value_records.size() + prepared.enum_values.size() > enum_value_records.capacity() ||
                canonical_types.size() + prepared.canonical_types.size() > canonical_types.capacity() ||
                types.size() + prepared.new_types.size() > types.capacity() ||
                identities.size() + prepared.new_identities.size() > identities.capacity() ||
                object_entries.size() + prepared.new_objects.size() > object_entries.capacity() ||
                object_identities.size() + prepared.new_object_identities.size() > object_identities.capacity() ||
                link_records.size() + prepared.new_links.size() > link_records.capacity() ||
                named_refs.size() + prepared.new_types.size() > named_refs.capacity() ||
                dependency_versions.size() + prepared.new_types.size() > dependency_versions.capacity() ||
                reverse_dependency_heads.size() + prepared.new_types.size() > reverse_dependency_heads.capacity() ||
                dependency_edges.size() + prepared.dependency_edges.size() > dependency_edges.capacity()) {
                return {status_code::rebuild_required};
            }
            return {};
        }

        member_records.reserve(member_records.size() + prepared.members.size());
        enum_value_records.reserve(enum_value_records.size() + prepared.enum_values.size());
        canonical_types.reserve(canonical_types.size() + prepared.canonical_types.size());
        types.reserve(types.size() + prepared.new_types.size());
        identities.reserve(identities.size() + prepared.new_identities.size());
        object_entries.reserve(object_entries.size() + prepared.new_objects.size());
        object_identities.reserve(object_identities.size() + prepared.new_object_identities.size());
        link_records.reserve(link_records.size() + prepared.new_links.size());
        named_refs.reserve(named_refs.size() + prepared.new_types.size());
        dependency_versions.reserve(dependency_versions.size() + prepared.new_types.size());
        reverse_dependency_heads.reserve(reverse_dependency_heads.size() + prepared.new_types.size());
        dependency_edges.reserve(dependency_edges.size() + prepared.dependency_edges.size());

        const auto touch_identity_slot = [&](identity_ref identity) noexcept -> bool {
            if (!identity || identity_index.empty())
                return false;
            const auto hash = identity_hash(identity);
            const auto fingerprint = fold32(hash);
            const auto mask = identity_index.size() - 1;
            auto position = static_cast<std::size_t>(hash) & mask;
            for (std::size_t probe = 0; probe < identity_index.size(); ++probe) {
                graph_identity_index_slot slot;
                if (!identity_index.read(position, slot).ok())
                    return false;
                if (slot.handle == 0) {
                    (void)identity_index[position];
                    return true;
                }
                if (slot.fingerprint == fingerprint && slot.handle <= identities.size()) {
                    identity_ref candidate;
                    if (identities.read(slot.handle - 1, candidate).ok() &&
                        candidate == identity) {
                        return true;
                    }
                }
                position = (position + 1) & mask;
            }
            return false;
        };
        if (!prepared.replace_identity_index) {
            for (const auto identity : prepared.new_identities) {
                if (!touch_identity_slot(identity))
                    return {status_code::rebuild_required};
            }
        }

        const auto touch_object_slot = [&](identity_ref identity) noexcept -> bool {
            if (!identity || object_identity_index.empty())
                return false;
            const auto hash = identity_hash(identity);
            const auto fingerprint = fold32(hash);
            const auto mask = object_identity_index.size() - 1;
            auto position = static_cast<std::size_t>(hash) & mask;
            for (std::size_t probe = 0; probe < object_identity_index.size(); ++probe) {
                graph_object_identity_index_slot slot;
                if (!object_identity_index.read(position, slot).ok())
                    return false;
                if (slot.handle == 0) {
                    (void)object_identity_index[position];
                    return true;
                }
                if (slot.fingerprint == fingerprint && slot.handle <= object_identities.size()) {
                    identity_ref candidate;
                    if (object_identities.read(slot.handle - 1, candidate).ok() &&
                        candidate == identity) {
                        return true;
                    }
                }
                position = (position + 1) & mask;
            }
            return false;
        };
        if (!prepared.replace_object_identity_index) {
            for (const auto identity : prepared.new_object_identities) {
                if (!touch_object_slot(identity))
                    return {status_code::rebuild_required};
            }
        }

        if (!prepared.replace_link_index) {
            for (const auto& link : prepared.new_links) {
            const auto hash = endpoint_hash(link.target);
            const auto mask = link_index.size() - 1;
            auto position = static_cast<std::size_t>(hash) & mask;
            bool touched = false;
            for (std::size_t probe = 0; probe < link_index.size(); ++probe) {
                graph_link_index_slot slot;
                if (!link_index.read(position, slot).ok())
                    return {status_code::artifact_corrupt};
                if (slot.handle == 0) {
                    (void)link_index[position];
                    touched = true;
                    break;
                }
                position = (position + 1) & mask;
            }
                if (!touched)
                    return {status_code::rebuild_required};
            }
        }

        const auto canonical_base = canonical_types.size();
        if (!prepared.replace_derived_index) {
            for (std::size_t index = 0; index < prepared.canonical_types.size(); ++index) {
            const auto& record = prepared.canonical_types[index];
            if (record.kind != canonical_type_kind::derived)
                continue;
            if (derived_index.empty())
                return {status_code::rebuild_required};
            const auto hash = derived_hash(
                static_cast<derived_type_kind>(record.detail),
                record.child_or_handle,
                record.payload);
            const auto mask = derived_index.size() - 1;
            auto position = static_cast<std::size_t>(hash) & mask;
            bool touched = false;
            for (std::size_t probe = 0; probe < derived_index.size(); ++probe) {
                graph_derived_index_slot slot;
                if (!derived_index.read(position, slot).ok())
                    return {status_code::artifact_corrupt};
                if (slot.type_ref == 0) {
                    (void)derived_index[position];
                    touched = true;
                    break;
                }
                position = (position + 1) & mask;
            }
                if (!touched)
                    return {status_code::rebuild_required};
                (void)canonical_base;
            }
        }

        for (const auto& pending : prepared.dependency_edges) {
            if (pending.target_handle == 0 ||
                pending.target_handle > reverse_dependency_heads.size()) {
                return {status_code::invalid_argument};
            }
            (void)reverse_dependency_heads[pending.target_handle - 1];
        }

        if (!types.read_status().ok() || !identities.read_status().ok() ||
            !member_records.read_status().ok() || !enum_value_records.read_status().ok() ||
            !object_entries.read_status().ok() || !object_identities.read_status().ok() ||
            !link_records.read_status().ok() || !canonical_types.read_status().ok() ||
            !identity_index.read_status().ok() || !object_identity_index.read_status().ok() ||
            !link_index.read_status().ok() || !named_refs.read_status().ok() ||
            !derived_index.read_status().ok() || !dependency_versions.read_status().ok() ||
            !reverse_dependency_heads.read_status().ok() || !dependency_edges.read_status().ok()) {
            return {status_code::artifact_corrupt};
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

void graph::publish_prepared(prepared_graph_generation& prepared) noexcept {
    types.swap(prepared.types);
    identities.swap(prepared.identities);
    member_records.swap(prepared.members);
    enum_value_records.swap(prepared.enum_values);
    object_entries.swap(prepared.objects);
    object_identities.swap(prepared.object_identities);
    link_records.swap(prepared.links);
    canonical_types.swap(prepared.canonical_types);

    identity_index.swap(prepared.identity_index);
    object_identity_index.swap(prepared.object_identity_index);
    link_index.swap(prepared.link_index);
    intrinsic_refs = prepared.intrinsic_refs;
    named_refs.swap(prepared.named_refs);
    derived_index.swap(prepared.derived_index);
    derived_index_entries = prepared.derived_index_entries;

    dependency_versions.swap(prepared.dependency_versions);
    reverse_dependency_heads.swap(prepared.reverse_dependency_heads);
    dependency_edges.swap(prepared.dependency_edges);

    live_type_count = prepared.live_type_count;
    live_object_count = prepared.live_object_count;
    live_link_count = prepared.live_link_count;
}

void graph::publish_prepared(prepared_graph_update& prepared) noexcept {
    const auto old_type_slots = types.size();
    const auto old_object_slots = object_entries.size();
    const auto old_link_slots = link_records.size();
    const auto canonical_base = canonical_types.size();

    append_prepared(member_records, prepared.members);
    append_prepared(enum_value_records, prepared.enum_values);
    append_prepared(canonical_types, prepared.canonical_types);

    append_prepared(types, prepared.new_types);
    append_prepared(identities, prepared.new_identities);
    append_prepared(object_entries, prepared.new_objects);
    append_prepared(object_identities, prepared.new_object_identities);
    append_prepared(link_records, prepared.new_links);
    for (std::size_t index = 0; index < prepared.new_types.size(); ++index) {
        named_refs.push_back({});
        dependency_versions.push_back(1);
        reverse_dependency_heads.push_back(0);
    }

    for (const auto& patch : prepared.type_patches)
        types[patch.handle - 1] = patch.value;
    for (const auto& patch : prepared.object_patches)
        object_entries[patch.handle - 1] = patch.value;
    for (const auto& patch : prepared.link_patches)
        link_records[patch.handle - 1] = patch.value;

    intrinsic_refs = prepared.intrinsic_refs;
    for (const auto& patch : prepared.named_ref_patches)
        named_refs[patch.handle] = patch.value;

    if (prepared.replace_identity_index) {
        identity_index.swap(prepared.rebuilt_identity_index);
    } else {
        for (std::size_t index = 0; index < prepared.new_identities.size(); ++index) {
            const auto handle = static_cast<std::uint32_t>(old_type_slots + index + 1);
            insert_identity_index(identity_index, identities, prepared.new_identities[index], handle);
        }
    }

    if (prepared.replace_object_identity_index) {
        object_identity_index.swap(prepared.rebuilt_object_identity_index);
    } else {
        for (std::size_t index = 0; index < prepared.new_object_identities.size(); ++index) {
            const auto handle = static_cast<std::uint32_t>(old_object_slots + index + 1);
            insert_object_identity_index(
                object_identity_index, object_identities, prepared.new_object_identities[index], handle);
        }
    }

    if (prepared.replace_link_index) {
        link_index.swap(prepared.rebuilt_link_index);
    } else {
        for (std::size_t index = 0; index < prepared.new_links.size(); ++index) {
            const auto handle = static_cast<std::uint32_t>(old_link_slots + index + 1);
            insert_link_index(link_index, link_records, prepared.new_links[index].target, handle);
        }
    }

    if (prepared.replace_derived_index) {
        derived_index.swap(prepared.rebuilt_derived_index);
    } else if (!prepared.canonical_types.empty()) {
        for (std::size_t index = 0; index < prepared.canonical_types.size(); ++index) {
            if (prepared.canonical_types[index].kind != canonical_type_kind::derived)
                continue;
            insert_derived_index(
                derived_index,
                canonical_types,
                static_cast<std::uint32_t>(canonical_base + index));
        }
    }
    derived_index_entries = prepared.derived_index_entries;

    for (const auto& patch : prepared.dependency_version_patches)
        dependency_versions[patch.handle - 1] = patch.version;

    for (const auto& pending : prepared.dependency_edges) {
        const auto target_index = static_cast<std::size_t>(pending.target_handle - 1);
        graph_dependency_edge edge;
        edge.owner_handle = pending.owner_handle;
        edge.owner_version = pending.owner_version;
        edge.next_for_target = reverse_dependency_heads[target_index];
        dependency_edges.push_back(edge);
        reverse_dependency_heads[target_index] = static_cast<std::uint32_t>(dependency_edges.size());
    }

    live_type_count = prepared.live_type_count;
    live_object_count = prepared.live_object_count;
    live_link_count = prepared.live_link_count;
}

} // namespace cw::server
