#include "identity_registry.hpp"

#include <functional>
#include <limits>
#include <new>

namespace cw::server {

identity_registry::identity_registry() noexcept = default;

std::size_t identity_registry::key_hash::operator()(const key& value) const noexcept {
    const auto parent_hash = std::hash<const void*>{}(value.parent);
    const auto name_hash = std::hash<std::uint32_t>{}(value.name.value());
    const auto kind_hash = std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(value.kind));
    auto result = parent_hash;
    result ^= name_hash + 0x9e3779b9u + (result << 6) + (result >> 2);
    result ^= kind_hash + 0x9e3779b9u + (result << 6) + (result >> 2);
    return result;
}

status identity_registry::intern(
    identity_ref parent,
    string_id local_name,
    identity_kind kind,
    identity_ref& output) noexcept {
    if (parent == nullptr || !local_name || kind == identity_kind::root)
        return {status_code::invalid_argument};

    try {
        std::scoped_lock lock{mutex};
        const key identity_key{parent, local_name, kind};
        const auto existing = index.find(identity_key);
        if (existing != index.end()) {
            output = existing->second;
            return {};
        }

        if (nodes.size() >= std::numeric_limits<std::uint32_t>::max())
            return {status_code::not_available};

        const auto slot = static_cast<std::uint32_t>(nodes.size() + 1);
        nodes.emplace_back(identity_node::construction_token{}, parent, local_name, kind, slot);
        const auto* created = &nodes.back();
        try {
            index.emplace(identity_key, created);
        }
        catch (...) {
            nodes.pop_back();
            throw;
        }
        output = created;
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::initialization_failed};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

std::size_t identity_registry::size() const noexcept {
    std::scoped_lock lock{mutex};
    return nodes.size() + 1;
}

} // namespace cw::server
