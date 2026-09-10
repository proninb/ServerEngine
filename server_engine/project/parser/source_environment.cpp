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
    value ^= value >> 31;
    return value;
}

[[nodiscard]] std::uint64_t binding_hash(identity_ref parent, std::string_view name) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char character : name) {
        const auto byte = static_cast<unsigned char>(character);
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return mix64(hash ^ mix64(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(parent))));
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
    std::span<const identity_ref> local_types,
    std::span<const source_interface* const> imports) noexcept {

    try {
        const auto capacity = capacity_for(local_types.size());
        if (capacity == 0)
            return {status_code::not_available};

        std::vector<identity_ref> new_types;
        std::vector<identity_ref> new_slots(capacity, nullptr);
        std::vector<const source_interface*> new_imports;
        new_types.reserve(local_types.size());
        new_imports.reserve(imports.size());

        const auto mask = new_slots.size() - 1;
        for (const auto identity : local_types) {
            if (identity == nullptr || identity->kind() != identity_kind::type || identity->parent() == nullptr)
                return {status_code::invalid_argument};
            const auto name = identity->name().view();
            auto position = static_cast<std::size_t>(binding_hash(identity->parent(), name)) & mask;
            for (;;) {
                const auto existing = new_slots[position];
                if (existing == nullptr) {
                    new_slots[position] = identity;
                    new_types.push_back(identity);
                    break;
                }
                if (existing->parent() == identity->parent() && existing->name().view() == name) {
                    if (existing != identity)
                        return {status_code::semantic_conflict};
                    break;
                }
                position = (position + 1) & mask;
            }
        }

        for (const auto* imported : imports) {
            if (imported == nullptr)
                return {status_code::invalid_argument};
            new_imports.push_back(imported);
        }

        local_type_values.swap(new_types);
        slots.swap(new_slots);
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

identity_ref source_interface::find_type(
    identity_ref scope,
    std::string_view name) const noexcept {

    return find_type_recursive(scope, name, 0);
}

identity_ref source_interface::find_type_recursive(
    identity_ref scope,
    std::string_view name,
    std::uint32_t depth) const noexcept {

    if (scope == nullptr || name.empty() || depth > 1024)
        return nullptr;

    if (!slots.empty()) {
        const auto mask = slots.size() - 1;
        auto position = static_cast<std::size_t>(binding_hash(scope, name)) & mask;
        for (std::size_t probe = 0; probe < slots.size(); ++probe) {
            const auto identity = slots[position];
            if (identity == nullptr)
                break;
            if (identity->parent() == scope && identity->name().view() == name)
                return identity;
            position = (position + 1) & mask;
        }
    }

    for (auto iterator = imported_interfaces.rbegin(); iterator != imported_interfaces.rend(); ++iterator) {
        if (const auto identity = (*iterator)->find_type_recursive(scope, name, depth + 1); identity != nullptr)
            return identity;
    }
    return nullptr;
}

identity_ref source_environment::find_type(
    identity_ref scope,
    std::string_view name,
    std::uint32_t source_offset) const noexcept {

    for (auto iterator = imports.rbegin(); iterator != imports.rend(); ++iterator) {
        if (iterator->visible_from > source_offset || iterator->interface == nullptr)
            continue;
        if (const auto identity = iterator->interface->find_type(scope, name); identity != nullptr)
            return identity;
    }
    return nullptr;
}

} // namespace cw::server
