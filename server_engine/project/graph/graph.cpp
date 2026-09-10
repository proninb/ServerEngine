#include "graph.hpp"

#include <cstdint>

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
    const auto value = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(identity));
    return mix64(value >> 3);
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

std::string_view graph::name(graph_name_ref value) const noexcept {
    const auto offset = static_cast<std::size_t>(value.offset);
    const auto length = static_cast<std::size_t>(value.length);
    if (offset > names.size() || length > names.size() - offset || length == 0)
        return {};
    return {names.data() + offset, length};
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

void graph::publish_prepared(prepared_graph_generation& prepared) noexcept {
    types.swap(prepared.types);
    identities.swap(prepared.identities);
    member_records.swap(prepared.members);
    enum_value_records.swap(prepared.enum_values);
    names.swap(prepared.names);
    canonical_types.swap(prepared.canonical_types);

    identity_index.swap(prepared.identity_index);
    intrinsic_refs = prepared.intrinsic_refs;
    named_refs.swap(prepared.named_refs);
    derived_index.swap(prepared.derived_index);
    derived_index_entries = prepared.derived_index_entries;

    dependency_versions.swap(prepared.dependency_versions);
    reverse_dependency_heads.swap(prepared.reverse_dependency_heads);
    dependency_edges.swap(prepared.dependency_edges);

    live_type_count = prepared.live_type_count;
}

void graph::publish_prepared(prepared_graph_update& prepared) noexcept {
    const auto old_type_slots = types.size();
    const auto canonical_base = canonical_types.size();

    append_prepared(names, prepared.names);
    append_prepared(member_records, prepared.members);
    append_prepared(enum_value_records, prepared.enum_values);
    append_prepared(canonical_types, prepared.canonical_types);

    append_prepared(types, prepared.new_types);
    append_prepared(identities, prepared.new_identities);
    for (std::size_t index = 0; index < prepared.new_types.size(); ++index) {
        named_refs.push_back({});
        dependency_versions.push_back(1);
        reverse_dependency_heads.push_back(0);
    }

    for (const auto& patch : prepared.type_patches)
        types[patch.handle - 1] = patch.value;

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
}

} // namespace cw::server
