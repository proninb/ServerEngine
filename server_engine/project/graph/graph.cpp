#include "graph.hpp"

namespace cw::server {

type_handle graph::type_at(std::size_t index) const noexcept {
    if (index >= types.size() || index >= 0xffffffffu)
        return {};
    return type_handle{static_cast<std::uint32_t>(index + 1)};
}

const type_entry* graph::find(type_handle handle) const noexcept {
    if (!handle || handle.value() > types.size())
        return nullptr;
    return &types[handle.value() - 1];
}

identity_ref graph::identity(type_handle handle) const noexcept {
    if (!handle || handle.value() > identities.size())
        return nullptr;
    return identities[handle.value() - 1];
}

std::span<const member_record> graph::members(type_handle handle) const noexcept {
    const auto* entry = find(handle);
    if (entry == nullptr || entry->kind != graph_type_kind::record || !entry->definition)
        return {};

    const auto begin = static_cast<std::size_t>(entry->definition.begin - 1);
    const auto count = static_cast<std::size_t>(entry->definition.count);
    if (begin > member_records.size() || count > member_records.size() - begin)
        return {};
    if (count == 0)
        return {};
    return {member_records.data() + begin, count};
}

std::span<const enum_value_record> graph::enum_values(type_handle handle) const noexcept {
    const auto* entry = find(handle);
    if (entry == nullptr || entry->kind != graph_type_kind::enumeration || !entry->definition)
        return {};

    const auto begin = static_cast<std::size_t>(entry->definition.begin - 1);
    const auto count = static_cast<std::size_t>(entry->definition.count);
    if (begin > enum_value_records.size() || count > enum_value_records.size() - begin)
        return {};
    if (count == 0)
        return {};
    return {enum_value_records.data() + begin, count};
}

std::string_view graph::name(graph_name_ref value) const noexcept {
    const auto offset = static_cast<std::size_t>(value.offset);
    const auto length = static_cast<std::size_t>(value.length);
    if (offset > names.size() || length > names.size() - offset)
        return {};
    if (length == 0)
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

bool graph::named(TypeRef type, type_handle& output) const noexcept {
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
    generation_value = prepared.generation;
}

} // namespace cw::server
