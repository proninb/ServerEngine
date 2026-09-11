#include "graph.hpp"

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

void insert_identity_index(
    std::vector<graph_identity_index_slot>& slots,
    std::span<const identity_ref> identities,
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

void insert_object_identity_index(
    std::vector<graph_object_identity_index_slot>& slots,
    std::span<const identity_ref> identities,
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

void insert_link_index(
    std::vector<graph_link_index_slot>& slots,
    std::span<const link_record> links,
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

void insert_derived_index(
    std::vector<graph_derived_index_slot>& slots,
    const std::vector<graph_canonical_type_record>& canonical_types,
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
void append_prepared(std::vector<T>& target, const std::vector<T>& source) noexcept {
    for (const auto& value : source)
        target.push_back(value);
}

} // namespace

type_handle graph::type_at(std::size_t index) const noexcept {
    if (index >= types.size() || index >= 0xffffffffu || !types[index].live())
        return {};
    return type_handle{static_cast<std::uint32_t>(index + 1)};
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
        return nullptr;
    return identities[handle.value() - 1];
}

identity_ref graph::identity(type_handle handle) const noexcept {
    return find(handle) == nullptr ? nullptr : identity_raw(handle);
}

type_handle graph::find_identity(identity_ref identity_value) const noexcept {
    if (identity_value == nullptr || identity_index.empty())
        return {};

    const auto hash = identity_hash(identity_value);
    const auto fingerprint = fold32(hash);
    const auto mask = identity_index.size() - 1;
    auto position = static_cast<std::size_t>(hash) & mask;
    for (std::size_t probe = 0; probe < identity_index.size(); ++probe) {
        const auto& slot = identity_index[position];
        if (slot.handle == 0)
            return {};
        if (slot.fingerprint == fingerprint && slot.handle <= identities.size() &&
            identities[slot.handle - 1] == identity_value) {
            return type_handle{slot.handle};
        }
        position = (position + 1) & mask;
    }
    return {};
}

type_handle graph::find_type(identity_ref identity_value) const noexcept {
    const auto handle = find_identity(identity_value);
    return find(handle) == nullptr ? type_handle{} : handle;
}

std::span<const member_record> graph::members(type_handle handle) const noexcept {
    const auto* entry = find(handle);
    if (entry == nullptr || entry->kind != graph_type_kind::record || !entry->definition)
        return {};

    const auto begin = static_cast<std::size_t>(entry->definition.begin - 1);
    const auto count = static_cast<std::size_t>(entry->definition.count);
    if (begin > member_records.size() || count > member_records.size() - begin || count == 0)
        return {};
    return {member_records.data() + begin, count};
}

std::span<const enum_value_record> graph::enum_values(type_handle handle) const noexcept {
    const auto* entry = find(handle);
    if (entry == nullptr || entry->kind != graph_type_kind::enumeration || !entry->definition)
        return {};

    const auto begin = static_cast<std::size_t>(entry->definition.begin - 1);
    const auto count = static_cast<std::size_t>(entry->definition.count);
    if (begin > enum_value_records.size() || count > enum_value_records.size() - begin || count == 0)
        return {};
    return {enum_value_records.data() + begin, count};
}

member_index graph::find_member(type_handle handle, string_id name) const noexcept {
    if (!name)
        return {};
    const auto values = members(handle);
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (values[index].name == name && index <= (std::numeric_limits<std::uint32_t>::max)())
            return member_index::from_zero_based(static_cast<std::uint32_t>(index));
    }
    return {};
}

object_handle graph::object_at(std::size_t index) const noexcept {
    if (index >= object_entries.size() || index >= 0xffffffffu || !object_entries[index].live())
        return {};
    return object_handle{static_cast<std::uint32_t>(index + 1)};
}

const object_entry* graph::find(object_handle handle) const noexcept {
    if (!handle || handle.value() > object_entries.size())
        return nullptr;
    const auto& entry = object_entries[handle.value() - 1];
    return entry.live() ? &entry : nullptr;
}

identity_ref graph::identity(object_handle handle) const noexcept {
    return find(handle) == nullptr || handle.value() > object_identities.size()
        ? nullptr
        : object_identities[handle.value() - 1];
}

object_handle graph::find_object_identity(identity_ref identity_value) const noexcept {
    if (identity_value == nullptr || object_identity_index.empty())
        return {};
    const auto hash = identity_hash(identity_value);
    const auto fingerprint = fold32(hash);
    const auto mask = object_identity_index.size() - 1;
    auto position = static_cast<std::size_t>(hash) & mask;
    for (std::size_t probe = 0; probe < object_identity_index.size(); ++probe) {
        const auto& slot = object_identity_index[position];
        if (slot.handle == 0)
            return {};
        if (slot.fingerprint == fingerprint && slot.handle <= object_identities.size() &&
            object_identities[slot.handle - 1] == identity_value) {
            return object_handle{slot.handle};
        }
        position = (position + 1) & mask;
    }
    return {};
}

object_handle graph::find_object(identity_ref identity_value) const noexcept {
    const auto handle = find_object_identity(identity_value);
    return find(handle) == nullptr ? object_handle{} : handle;
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
        const auto& slot = link_index[position];
        if (slot.handle == 0)
            return {};
        if (slot.fingerprint == fingerprint && slot.handle <= link_records.size() &&
            link_records[slot.handle - 1].target == target) {
            return link_handle{slot.handle};
        }
        position = (position + 1) & mask;
    }
    return {};
}

link_handle graph::find_link(object_endpoint target) const noexcept {
    const auto handle = find_link_raw(target);
    return find(handle) == nullptr ? link_handle{} : handle;
}

canonical_type_kind graph::kind(TypeRef type) const noexcept {
    if (!type || type.value() >= canonical_types.size())
        return canonical_type_kind::intrinsic;
    return canonical_types[type.value()].kind;
}

bool graph::intrinsic(TypeRef type, intrinsic_type& output) const noexcept {
    output = intrinsic_type::none;
    if (!type || type.value() >= canonical_types.size())
        return false;

    const auto& record = canonical_types[type.value()];
    if (record.kind != canonical_type_kind::intrinsic)
        return false;
    output = static_cast<intrinsic_type>(record.detail);
    return true;
}

bool graph::named_raw(TypeRef type, type_handle& output) const noexcept {
    output = {};
    if (!type || type.value() >= canonical_types.size())
        return false;

    const auto& record = canonical_types[type.value()];
    if (record.kind != canonical_type_kind::named || record.child_or_handle == 0 ||
        record.child_or_handle > types.size()) {
        return false;
    }
    output = type_handle{record.child_or_handle};
    return true;
}

bool graph::named(TypeRef type, type_handle& output) const noexcept {
    if (!named_raw(type, output))
        return false;
    if (find(output) == nullptr) {
        output = {};
        return false;
    }
    return true;
}

bool graph::derived(TypeRef type, derived_type_record& output) const noexcept {
    output = {};
    if (!type || type.value() >= canonical_types.size())
        return false;

    const auto& record = canonical_types[type.value()];
    if (record.kind != canonical_type_kind::derived || record.child_or_handle == 0 ||
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
        output.retained_bytes += values.capacity() * sizeof(value_type);
        output.reserve_bytes += (values.capacity() - values.size()) * sizeof(value_type);
    };
    const auto add_index = [&](const auto& values, std::size_t entries) noexcept {
        using value_type = typename std::remove_reference_t<decltype(values)>::value_type;
        output.retained_bytes += values.size() * sizeof(value_type);
        const auto safe_entries = values.size() / 2;
        if (safe_entries > entries)
            output.reserve_bytes += (safe_entries - entries) * sizeof(value_type);
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
