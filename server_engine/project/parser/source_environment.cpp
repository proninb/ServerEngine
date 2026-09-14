#include "source_environment.hpp"

#include "../persistence/build_cache_image.hpp"

#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

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
    return mix64(
        static_cast<std::uint64_t>(owner.value()) ^
        (static_cast<std::uint64_t>(name.value()) << 32));
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
    identity_view identities,
    std::span<const source_interface* const> imports,
    bool retain_compact_persistence) noexcept {

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
        std::vector<type_slot> new_type_slots(type_capacity);
        std::vector<object_slot> new_object_slots(object_capacity);
        std::vector<member_slot> new_member_slots(member_capacity);

        std::vector<type_slot> new_persistence_type_slots;
        std::vector<object_slot> new_persistence_object_slots;
        std::vector<member_slot> new_persistence_member_slots;
        std::vector<const source_interface*> new_imports;

        std::size_t new_persistence_type_count = 0;
        std::size_t new_persistence_object_count = 0;
        std::size_t new_persistence_member_count = 0;

        new_types.reserve(type_count);

        if (retain_compact_persistence) {
            new_persistence_type_slots.reserve(type_count);
            new_persistence_object_slots.reserve(
                facts.objects().size());
            new_persistence_member_slots.reserve(member_count);
        }

        new_imports.reserve(imports.size());

        const auto insert_type = [&](identity_ref identity) -> status {
            if (!identity || identity.kind() != identity_kind::type)
                return {status_code::invalid_argument};

            const auto parent = identities.parent(identity);
            const auto name = identities.name(identity);
            if (!parent || !name)
                return {status_code::invalid_argument};

            const auto mask = new_type_slots.size() - 1;
            auto position =
                static_cast<std::size_t>(binding_hash(parent, name)) & mask;

            for (;;) {
                auto& slot = new_type_slots[position];
                if (!slot.identity) {
                    const type_slot value{
                        parent,
                        name,
                        identity,
                    };
                    slot = value;
                    new_types.push_back(identity);
                    ++new_persistence_type_count;

                    if (retain_compact_persistence)
                        new_persistence_type_slots.push_back(value);

                    return {};
                }

                if (slot.parent == parent && slot.name == name) {
                    return slot.identity == identity
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
            if (!object.identity || object.identity.kind() != identity_kind::object)
                return {status_code::invalid_argument};

            const auto parent = identities.parent(object.identity);
            const auto name = identities.name(object.identity);
            if (!parent || !name)
                return {status_code::invalid_argument};

            auto position =
                static_cast<std::size_t>(binding_hash(parent, name)) & object_mask;

            const auto named_type = object.type.identity && object.type.modifiers.count == 0
                ? object.type.identity
                : identity_ref{};

            for (;;) {
                auto& slot = new_object_slots[position];
                if (!slot.identity) {
                    const object_slot value{
                        parent,
                        name,
                        object.identity,
                        named_type,
                    };
                    slot = value;
                    ++new_persistence_object_count;

                    if (retain_compact_persistence)
                        new_persistence_object_slots.push_back(value);

                    break;
                }

                if (slot.parent == parent && slot.name == name) {
                    if (slot.identity != object.identity ||
                        slot.named_type != named_type) {
                        return {status_code::semantic_conflict};
                    }
                    break;
                }

                position = (position + 1) & object_mask;
            }
        }

        const auto member_mask = new_member_slots.size() - 1;
        for (const auto& record : facts.records()) {
            if (record.declaration_kind != source_record_declaration_kind::definition)
                continue;

            if (!record.identity ||
                record.identity.kind() != identity_kind::type ||
                record.members.begin > facts.members().size() ||
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
                    if (!slot.type) {
                        const member_slot value{
                            record.identity,
                            member.name,
                            member_index::from_zero_based(index),
                        };
                        slot = value;
                        ++new_persistence_member_count;

                        if (retain_compact_persistence)
                            new_persistence_member_slots.push_back(value);

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

        persistence_type_slots.swap(
            new_persistence_type_slots);
        persistence_object_slots.swap(
            new_persistence_object_slots);
        persistence_member_slots.swap(
            new_persistence_member_slots);

        persistence_local_type_count =
            local_type_values.size();
        persistence_type_slot_count =
            new_persistence_type_count;
        persistence_object_slot_count =
            new_persistence_object_count;
        persistence_member_slot_count =
            new_persistence_member_count;
        persistence_compact_state =
            retain_compact_persistence;

        external_persistence_data = {};
        persistence_externalized_state = false;
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

status source_interface::initialize_persisted(
    const build_cache_image_view& cache,
    source_id source,
    std::span<const source_interface* const> imports) noexcept {

    if (!source)
        return {status_code::invalid_argument};

    build_cache_source_record persisted;
    auto result = cache.source(source, persisted);
    if (!result.ok())
        return result;
    if (!persisted.frontend_present)
        return {status_code::not_found};

    try {
        const auto type_capacity =
            capacity_for(persisted.type_slots.count);
        const auto object_capacity =
            capacity_for(persisted.object_slots.count);
        const auto member_capacity =
            capacity_for(persisted.member_slots.count);

        if (type_capacity == 0 ||
            object_capacity == 0 ||
            member_capacity == 0) {
            return {status_code::not_available};
        }

        std::vector<identity_ref> new_types(
            persisted.local_types.count);
        std::vector<type_slot> new_type_slots(type_capacity);
        std::vector<object_slot> new_object_slots(object_capacity);
        std::vector<member_slot> new_member_slots(member_capacity);

        std::vector<type_slot> new_persistence_type_slots;
        std::vector<object_slot> new_persistence_object_slots;
        std::vector<member_slot> new_persistence_member_slots;
        std::vector<const source_interface*> new_imports;

        new_persistence_type_slots.reserve(
            persisted.type_slots.count);
        new_persistence_object_slots.reserve(
            persisted.object_slots.count);
        new_persistence_member_slots.reserve(
            persisted.member_slots.count);
        new_imports.reserve(imports.size());

        for (std::size_t index = 0; index < new_types.size(); ++index) {
            result = cache.frontend_local_type(
                source,
                index,
                new_types[index]);
            if (!result.ok() || !new_types[index])
                return {status_code::artifact_corrupt};
        }

        const auto type_mask = new_type_slots.size() - 1;
        for (std::size_t index = 0;
             index < persisted.type_slots.count;
             ++index) {

            type_slot value;
            result = cache.frontend_type_slot(
                source,
                index,
                value);
            if (!result.ok() ||
                !value.parent ||
                !value.name ||
                !value.identity) {
                return {status_code::artifact_corrupt};
            }

            auto position =
                static_cast<std::size_t>(
                    binding_hash(value.parent, value.name)) &
                type_mask;

            for (;;) {
                auto& slot = new_type_slots[position];
                if (!slot.identity) {
                    slot = value;
                    new_persistence_type_slots.push_back(value);
                    break;
                }

                if (slot.parent == value.parent &&
                    slot.name == value.name) {
                    return {status_code::artifact_corrupt};
                }

                position = (position + 1) & type_mask;
            }
        }

        const auto object_mask = new_object_slots.size() - 1;
        for (std::size_t index = 0;
             index < persisted.object_slots.count;
             ++index) {

            object_slot value;
            result = cache.frontend_object_slot(
                source,
                index,
                value);
            if (!result.ok() ||
                !value.parent ||
                !value.name ||
                !value.identity) {
                return {status_code::artifact_corrupt};
            }

            auto position =
                static_cast<std::size_t>(
                    binding_hash(value.parent, value.name)) &
                object_mask;

            for (;;) {
                auto& slot = new_object_slots[position];
                if (!slot.identity) {
                    slot = value;
                    new_persistence_object_slots.push_back(value);
                    break;
                }

                if (slot.parent == value.parent &&
                    slot.name == value.name) {
                    return {status_code::artifact_corrupt};
                }

                position = (position + 1) & object_mask;
            }
        }

        const auto member_mask = new_member_slots.size() - 1;
        for (std::size_t index = 0;
             index < persisted.member_slots.count;
             ++index) {

            member_slot value;
            result = cache.frontend_member_slot(
                source,
                index,
                value);
            if (!result.ok() ||
                !value.type ||
                !value.name ||
                !value.index) {
                return {status_code::artifact_corrupt};
            }

            auto position =
                static_cast<std::size_t>(
                    binding_hash(value.type, value.name)) &
                member_mask;

            for (;;) {
                auto& slot = new_member_slots[position];
                if (!slot.type) {
                    slot = value;
                    new_persistence_member_slots.push_back(value);
                    break;
                }

                if (slot.type == value.type &&
                    slot.name == value.name) {
                    return {status_code::artifact_corrupt};
                }

                position = (position + 1) & member_mask;
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

        persistence_type_slots.swap(
            new_persistence_type_slots);
        persistence_object_slots.swap(
            new_persistence_object_slots);
        persistence_member_slots.swap(
            new_persistence_member_slots);

        persistence_local_type_count =
            local_type_values.size();
        persistence_type_slot_count =
            persistence_type_slots.size();
        persistence_object_slot_count =
            persistence_object_slots.size();
        persistence_member_slot_count =
            persistence_member_slots.size();
        persistence_compact_state = true;

        external_persistence_data = {};
        persistence_externalized_state = false;
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

source_interface_owned_persistence_data
source_interface::release_persistence_data() noexcept {

    source_interface_owned_persistence_data output;
    if (persistence_externalized_state)
        return output;

    output.local_types = std::move(local_type_values);
    output.type_slots = std::move(persistence_type_slots);
    output.object_slots = std::move(persistence_object_slots);
    output.member_slots = std::move(persistence_member_slots);

    external_persistence_data = {};
    return output;
}

void source_interface::bind_persistence_data(
    source_interface_data_view data) noexcept {

    external_persistence_data = data;
    persistence_externalized_state = true;
}


identity_ref source_interface::find_type(
    identity_ref scope,
    string_id name) const noexcept {

    return find_type_recursive(scope, name, 0);
}

identity_ref source_interface::find_type_recursive(
    identity_ref scope,
    string_id name,
    std::uint32_t depth) const noexcept {

    if (!scope || !name || depth > 1024)
        return {};

    if (!type_slots.empty()) {
        const auto mask = type_slots.size() - 1;
        auto position =
            static_cast<std::size_t>(binding_hash(scope, name)) & mask;

        for (std::size_t probe = 0; probe < type_slots.size(); ++probe) {
            const auto& slot = type_slots[position];
            if (!slot.identity)
                break;

            if (slot.parent == scope && slot.name == name)
                return slot.identity;

            position = (position + 1) & mask;
        }
    }

    for (auto iterator = imported_interfaces.rbegin();
         iterator != imported_interfaces.rend();
         ++iterator) {

        if (const auto identity =
                (*iterator)->find_type_recursive(scope, name, depth + 1);
            identity) {
            return identity;
        }
    }

    return {};
}

source_interface_object source_interface::find_object(
    identity_ref scope,
    string_id name) const noexcept {

    return find_object_recursive(scope, name, 0);
}

source_interface_object source_interface::find_object_recursive(
    identity_ref scope,
    string_id name,
    std::uint32_t depth) const noexcept {

    if (!scope || !name || depth > 1024)
        return {};

    if (!object_slots.empty()) {
        const auto mask = object_slots.size() - 1;
        auto position =
            static_cast<std::size_t>(binding_hash(scope, name)) & mask;

        for (std::size_t probe = 0; probe < object_slots.size(); ++probe) {
            const auto& slot = object_slots[position];
            if (!slot.identity)
                break;

            if (slot.parent == scope && slot.name == name)
                return {slot.identity, slot.named_type};

            position = (position + 1) & mask;
        }
    }

    for (auto iterator = imported_interfaces.rbegin();
         iterator != imported_interfaces.rend();
         ++iterator) {

        const auto found =
            (*iterator)->find_object_recursive(scope, name, depth + 1);
        if (found.identity)
            return found;
    }

    return {};
}

member_index source_interface::find_member(
    identity_ref type,
    string_id name) const noexcept {

    return find_member_recursive(type, name, 0);
}

member_index source_interface::find_member_recursive(
    identity_ref type,
    string_id name,
    std::uint32_t depth) const noexcept {

    if (!type || !name || depth > 1024)
        return {};

    if (!member_slots.empty()) {
        const auto mask = member_slots.size() - 1;
        auto position =
            static_cast<std::size_t>(binding_hash(type, name)) & mask;

        for (std::size_t probe = 0; probe < member_slots.size(); ++probe) {
            const auto& slot = member_slots[position];
            if (!slot.type)
                break;

            if (slot.type == type && slot.name == name)
                return slot.index;

            position = (position + 1) & mask;
        }
    }

    for (auto iterator = imported_interfaces.rbegin();
         iterator != imported_interfaces.rend();
         ++iterator) {

        const auto found =
            (*iterator)->find_member_recursive(type, name, depth + 1);
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

        if (const auto identity = iterator->interface->find_type(scope, name);
            identity) {
            return identity;
        }
    }

    return {};
}

source_interface_object source_environment::find_object(
    identity_ref scope,
    string_id name,
    std::uint32_t source_offset) const noexcept {

    for (auto iterator = imports.rbegin(); iterator != imports.rend(); ++iterator) {
        if (iterator->visible_from > source_offset || iterator->interface == nullptr)
            continue;

        const auto found = iterator->interface->find_object(scope, name);
        if (found.identity)
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
