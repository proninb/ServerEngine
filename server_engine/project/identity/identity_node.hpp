#pragma once

#include "name_ref.hpp"

#include <cstdint>

namespace cw::server {

class identity_space;

enum class identity_kind : std::uint8_t {
    root,
    namespace_scope,
    type,
};

// Project-lifetime semantic identity. It stores only WHO an entity is;
// definition, layout, handles, and compiled state live only in the current Graph.
class identity_node final {
public:
    [[nodiscard]] constexpr const identity_node* parent() const noexcept { return parent_identity; }
    [[nodiscard]] constexpr name_ref name() const noexcept { return local_name; }
    [[nodiscard]] constexpr identity_kind kind() const noexcept { return semantic_kind; }

private:
    struct construction_token {};
    friend class identity_space;

    constexpr identity_node(
        construction_token,
        const identity_node* parent,
        name_ref name,
        identity_kind kind) noexcept
        : parent_identity(parent), local_name(name), semantic_kind(kind) {}

    const identity_node* parent_identity = nullptr;
    name_ref local_name{};
    identity_kind semantic_kind = identity_kind::root;
};

using identity_ref = const identity_node*;

} // namespace cw::server
