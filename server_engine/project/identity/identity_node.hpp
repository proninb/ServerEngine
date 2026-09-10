#pragma once

#include "../../string_id.hpp"

#include <cstdint>

namespace cw::server {

enum class identity_kind : std::uint8_t {
    root,
    namespace_scope,
    type,
};

// Project-lifetime semantic identity atom. It never contains generation-specific state or a Graph reference.
class identity_node final {
public:
    [[nodiscard]] constexpr const identity_node* parent() const noexcept { return parent_identity; }
    [[nodiscard]] constexpr string_id name() const noexcept { return local_name; }
    [[nodiscard]] constexpr identity_kind kind() const noexcept { return semantic_kind; }
    [[nodiscard]] constexpr std::uint32_t slot() const noexcept { return acceleration_slot; }

private:
    struct construction_token {};
    friend class identity_registry;

public:
    constexpr identity_node(
        construction_token,
        const identity_node* parent,
        string_id name,
        identity_kind kind,
        std::uint32_t slot) noexcept
        : parent_identity(parent), local_name(name), semantic_kind(kind), acceleration_slot(slot) {}

private:
    const identity_node* parent_identity = nullptr;
    string_id local_name;
    identity_kind semantic_kind = identity_kind::root;
    std::uint32_t acceleration_slot = 0;
};

using identity_ref = const identity_node*;

} // namespace cw::server
