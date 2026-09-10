#pragma once

#include "../frontend/source_facts.hpp"
#include "../identity/identity_node.hpp"
#include "../../member_index.hpp"
#include "../../status.hpp"
#include "../../string_id.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cw::server {

struct source_interface_object final {
    identity_ref identity = nullptr;
    identity_ref named_type = nullptr;
};

struct source_interface_member final {
    identity_ref type = nullptr;
    string_id name{};
    member_index index{};
};

// Immutable Parser-visible interface exported by one parsed Source. It retains
// resolved types, objects and record-member positions using identity_ref/string_id;
// imports are referenced, so transitive visibility never copies declarations.
class source_interface final {
public:
    source_interface() = default;
    source_interface(const source_interface&) = delete;
    source_interface& operator=(const source_interface&) = delete;
    source_interface(source_interface&&) noexcept = default;
    source_interface& operator=(source_interface&&) noexcept = default;

    [[nodiscard]] status initialize(
        const source_facts& facts,
        std::span<const source_interface* const> imports = {}) noexcept;

    [[nodiscard]] identity_ref find_type(
        identity_ref scope,
        string_id name) const noexcept;

    [[nodiscard]] source_interface_object find_object(
        identity_ref scope,
        string_id name) const noexcept;

    [[nodiscard]] member_index find_member(
        identity_ref type,
        string_id name) const noexcept;

    [[nodiscard]] std::span<const identity_ref> local_types() const noexcept {
        return local_type_values;
    }

private:
    struct object_slot final {
        identity_ref identity = nullptr;
        identity_ref named_type = nullptr;
    };

    struct member_slot final {
        identity_ref type = nullptr;
        string_id name{};
        member_index index{};
    };

    [[nodiscard]] identity_ref find_type_recursive(
        identity_ref scope,
        string_id name,
        std::uint32_t depth) const noexcept;

    [[nodiscard]] source_interface_object find_object_recursive(
        identity_ref scope,
        string_id name,
        std::uint32_t depth) const noexcept;

    [[nodiscard]] member_index find_member_recursive(
        identity_ref type,
        string_id name,
        std::uint32_t depth) const noexcept;

    std::vector<identity_ref> local_type_values;
    std::vector<identity_ref> type_slots;
    std::vector<object_slot> object_slots;
    std::vector<member_slot> member_slots;
    std::vector<const source_interface*> imported_interfaces;
};

struct source_environment_import final {
    std::uint32_t visible_from = 0;
    const source_interface* interface = nullptr;
};

// Non-owning positional include environment for one Parser invocation.
class source_environment final {
public:
    source_environment() noexcept = default;
    explicit source_environment(std::span<const source_environment_import> import_values) noexcept
        : imports(import_values) {}

    [[nodiscard]] identity_ref find_type(
        identity_ref scope,
        string_id name,
        std::uint32_t source_offset) const noexcept;

    [[nodiscard]] source_interface_object find_object(
        identity_ref scope,
        string_id name,
        std::uint32_t source_offset) const noexcept;

    [[nodiscard]] member_index find_member(
        identity_ref type,
        string_id name,
        std::uint32_t source_offset) const noexcept;

private:
    std::span<const source_environment_import> imports;
};

} // namespace cw::server
