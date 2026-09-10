#include "source_environment.hpp"

#include <limits>
#include <new>
#include <stdexcept>

namespace cw::server {
namespace {

[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

[[nodiscard]] std::uint64_t binding_hash(identity_ref owner, string_id name) noexcept {
    const auto pointer = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(owner));
    return mix64(pointer ^ (static_cast<std::uint64_t>(name.value()) << 32));
}

[[nodiscard]] std::size_t capacity_for(std::size_t count) noexcept {
    std::size_t capacity = 16;
    const auto target = count > (std::numeric_limits<std::size_t>::max)() / 2
        ? (std::numeric_limits<std::size_t>::max)()
        : count * 2 + 1;
    while (capacity < target) {
        if (capacity > (std::numeric_limits<std::size_t>::max)() / 2)
            return 0;
        capacity *= 2;
    }
    return capacity;
}

} // namespace

status source_interface::initialize(
    const source_facts& facts,
    std::span<const source_interface* const> imports) noexcept {

    try {
        const auto type_count = facts.records().size() + facts.enums().size();
        std::size_t member_count = 0;
        for (const auto& record : facts.records()) {
            if (record.declaration_kind == source_record_declaration_kind::definition)
                member_count += record.members.count;
        }

        const auto type_capacity = capacity_for(type_count);
        const auto object_capacity = capacity_for(facts.objects().size());
        const auto member_capacity = capacity_for(member_count);
        if (type_capacity == 0 || object_capacity == 0 || member_capacity == 0)
            return {status_code::not_available};

        std::vector<identity_ref> new_types;
        std::vector<identity_ref> new_type_slots(type_capacity, nullptr);
        std::vector<object_slot> new_object_slots(object_capacity);
        std::vector<member_slot> new_member_slots(member_capacity);
        std::vector<const source_interface*> new_imports;
        new_types.reserve(type_count);
        new_imports.reserve(imports.size());

        const auto insert_type = [&](identity_ref identity) -> status {
            if (identity == nullptr || identity->kind() != identity_kind::type ||
                identity->parent() == nullptr || !identity->name()) {
                return {status_code::invalid_argument};
            }
            const auto mask = new_type_slots.size() - 1;
            auto position = static_cast<std::size_t>(
                binding_hash(identity->parent(), identity->name())) & mask;
            for (;;) {
                const auto existing = new_type_slots[position];
                if (existing == nullptr) {
                    new_type_slots[position] = identity;
                    new_types.push_back(identity);
                    return {};
                }
                if (existing->parent() == identity->parent() &&
                    existing->name() == identity->name()) {
                    return existing == identity
                        ? status{}
                        : status{status_code::semantic_conflict};
                }
                position = (position + 1) & mask;
            }
        };

        for (const auto& record : facts.records()) {
            const auto result = insert_type(record.identity);
            if (!result.ok())
                return result;
        }
        for (const auto& enum_fact : facts.enums()) {
            const auto result = insert_type(enum_fact.identity);
            if (!result.ok())
                return result;
        }

        const auto object_mask = new_object_slots.size() - 1;
        for (const auto& object : facts.objects()) {
            if (object.identity == nullptr || object.identity->kind() != identity_kind::object ||
                object.identity->parent() == nullptr || !object.identity->name()) {
                return {status_code::invalid_argument};
            }
            auto position = static_cast<std::size_t>(
                binding_hash(object.identity->parent(), object.identity->name())) & object_mask;
            const auto named_type = object.type.identity != nullptr && object.type.modifiers.count == 0
                ? object.type.identity
                : nullptr;
            for (;;) {
                auto& slot = new_object_slots[position];
                if (slot.identity == nullptr) {
                    slot.identity = object.identity;
                    slot.named_type = named_type;
                    break;
                }
                if (slot.identity->parent() == object.identity->parent() &&
                    slot.identity->name() == object.identity->name()) {
                    if (slot.identity != object.identity || slot.named_type != named_type)
                        return {status_code::semantic_conflict};
                    break;
                }
                position = (position + 1) & object_mask;
            }
        }

        const auto member_mask = new_member_slots.size() - 1;
        for (const auto& record : facts.records()) {
            if (record.declaration_kind != source_record_declaration_kind::definition)
                continue;
            if (record.members.begin > facts.members().size() ||
                record.members.count > facts.members().size() - record.members.begin) {
                return {status_code::invalid_argument};
            }
            for (std::uint32_t index = 0; index < record.members.count; ++index) {
                const auto& member = facts.members()[record.members.begin + index];
                if (!member.name)
                    return {status_code::invalid_argument};
                auto position = static_cast<std::size_t>(
                    binding_hash(record.identity, member.name)) & member_mask;
                for (;;) {
                    auto& slot = new_member_slots[position];
                    if (slot.type == nullptr) {
                        slot.type = record.identity;
                        slot.name = member.name;
                        slot.index = member_index::from_zero_based(index);
                        break;
                    }
                    if (slot.type == record.identity && slot.name == member.name)
                        return {status_code::semantic_conflict};
                    position = (position + 1) & member_mask;
                }
            }
        }

        for (const auto* imported : imports) {
            if (imported == nullptr)
                return {status_code::invalid_argument};
            new_imports.push_back(imported);
        }

        local_type_values.swap(new_types);
        type_slots.swap(new_type_slots);
        object_slots.swap(new_object_slots);
        member_slots.swap(new_member_slots);
        imported_interfaces.swap(new_imports);
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

identity_ref source_interface::find_type(identity_ref scope, string_id name) const noexcept {
    return find_type_recursive(scope, name, 0);
}

identity_ref source_interface::find_type_recursive(
    identity_ref scope,
    string_id name,
    std::uint32_t depth) const noexcept {

    if (scope == nullptr || !name || depth > 1024)
        return nullptr;

    if (!type_slots.empty()) {
        const auto mask = type_slots.size() - 1;
        auto position = static_cast<std::size_t>(binding_hash(scope, name)) & mask;
        for (std::size_t probe = 0; probe < type_slots.size(); ++probe) {
            const auto identity = type_slots[position];
            if (identity == nullptr)
                break;
            if (identity->parent() == scope && identity->name() == name)
                return identity;
            position = (position + 1) & mask;
        }
    }

    for (auto iterator = imported_interfaces.rbegin(); iterator != imported_interfaces.rend(); ++iterator) {
        if (const auto identity = (*iterator)->find_type_recursive(scope, name, depth + 1);
            identity != nullptr) {
            return identity;
        }
    }
    return nullptr;
}

source_interface_object source_interface::find_object(identity_ref scope, string_id name) const noexcept {
    return find_object_recursive(scope, name, 0);
}

source_interface_object source_interface::find_object_recursive(
    identity_ref scope,
    string_id name,
    std::uint32_t depth) const noexcept {

    if (scope == nullptr || !name || depth > 1024)
        return {};

    if (!object_slots.empty()) {
        const auto mask = object_slots.size() - 1;
        auto position = static_cast<std::size_t>(binding_hash(scope, name)) & mask;
        for (std::size_t probe = 0; probe < object_slots.size(); ++probe) {
            const auto& slot = object_slots[position];
            if (slot.identity == nullptr)
                break;
            if (slot.identity->parent() == scope && slot.identity->name() == name)
                return {slot.identity, slot.named_type};
            position = (position + 1) & mask;
        }
    }

    for (auto iterator = imported_interfaces.rbegin(); iterator != imported_interfaces.rend(); ++iterator) {
        const auto found = (*iterator)->find_object_recursive(scope, name, depth + 1);
        if (found.identity != nullptr)
            return found;
    }
    return {};
}

member_index source_interface::find_member(identity_ref type, string_id name) const noexcept {
    return find_member_recursive(type, name, 0);
}

member_index source_interface::find_member_recursive(
    identity_ref type,
    string_id name,
    std::uint32_t depth) const noexcept {

    if (type == nullptr || !name || depth > 1024)
        return {};

    if (!member_slots.empty()) {
        const auto mask = member_slots.size() - 1;
        auto position = static_cast<std::size_t>(binding_hash(type, name)) & mask;
        for (std::size_t probe = 0; probe < member_slots.size(); ++probe) {
            const auto& slot = member_slots[position];
            if (slot.type == nullptr)
                break;
            if (slot.type == type && slot.name == name)
                return slot.index;
            position = (position + 1) & mask;
        }
    }

    for (auto iterator = imported_interfaces.rbegin(); iterator != imported_interfaces.rend(); ++iterator) {
        const auto found = (*iterator)->find_member_recursive(type, name, depth + 1);
        if (found)
            return found;
    }
    return {};
}

identity_ref source_environment::find_type(
    identity_ref scope,
    string_id name,
    std::uint32_t source_offset) const noexcept {

    for (auto iterator = imports.rbegin(); iterator != imports.rend(); ++iterator) {
        if (iterator->visible_from > source_offset || iterator->interface == nullptr)
            continue;
        if (const auto identity = iterator->interface->find_type(scope, name); identity != nullptr)
            return identity;
    }
    return nullptr;
}

source_interface_object source_environment::find_object(
    identity_ref scope,
    string_id name,
    std::uint32_t source_offset) const noexcept {

    for (auto iterator = imports.rbegin(); iterator != imports.rend(); ++iterator) {
        if (iterator->visible_from > source_offset || iterator->interface == nullptr)
            continue;
        const auto found = iterator->interface->find_object(scope, name);
        if (found.identity != nullptr)
            return found;
    }
    return {};
}

member_index source_environment::find_member(
    identity_ref type,
    string_id name,
    std::uint32_t source_offset) const noexcept {

    for (auto iterator = imports.rbegin(); iterator != imports.rend(); ++iterator) {
        if (iterator->visible_from > source_offset || iterator->interface == nullptr)
            continue;
        const auto found = iterator->interface->find_member(type, name);
        if (found)
            return found;
    }
    return {};
}

} // namespace cw::server
