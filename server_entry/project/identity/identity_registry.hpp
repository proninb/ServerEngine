#pragma once

#include "identity_node.hpp"
#include "../../status.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace cw::server {

// Owns all semantic identity atoms for one Project Context and guarantees stable addresses until Project teardown.
class identity_registry final {
public:
    identity_registry() noexcept;

    identity_registry(const identity_registry&) = delete;
    identity_registry& operator=(const identity_registry&) = delete;

    [[nodiscard]] identity_ref root() const noexcept { return &root_identity; }

    [[nodiscard]] status intern(
        identity_ref parent,
        string_id local_name,
        identity_kind kind,
        identity_ref& output) noexcept;

    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct key {
        identity_ref parent = nullptr;
        string_id name;
        identity_kind kind = identity_kind::type;

        friend constexpr bool operator==(const key&, const key&) noexcept = default;
    };

    struct key_hash {
        [[nodiscard]] std::size_t operator()(const key& value) const noexcept;
    };

    identity_node root_identity{identity_node::construction_token{}, nullptr, string_id{}, identity_kind::root, 0};
    mutable std::mutex mutex;
    std::deque<identity_node> nodes;
    std::unordered_map<key, identity_ref, key_hash> index;
};

} // namespace cw::server
