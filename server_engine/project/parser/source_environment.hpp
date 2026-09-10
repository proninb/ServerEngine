#pragma once

#include "../identity/identity_node.hpp"
#include "../../status.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace cw::server {

// Immutable Parser-visible interface exported by one parsed Source. Local type
// bindings are indexed directly to identity_ref; imports are referenced, not copied,
// so transitive include visibility does not duplicate project declarations.
class source_interface final {
public:
    source_interface() = default;
    source_interface(const source_interface&) = delete;
    source_interface& operator=(const source_interface&) = delete;
    source_interface(source_interface&&) noexcept = default;
    source_interface& operator=(source_interface&&) noexcept = default;

    [[nodiscard]] status initialize(
        std::span<const identity_ref> local_types,
        std::span<const source_interface* const> imports = {}) noexcept;

    [[nodiscard]] identity_ref find_type(
        identity_ref scope,
        std::string_view name) const noexcept;

    [[nodiscard]] std::span<const identity_ref> local_types() const noexcept {
        return local_type_values;
    }

private:
    [[nodiscard]] identity_ref find_type_recursive(
        identity_ref scope,
        std::string_view name,
        std::uint32_t depth) const noexcept;

    std::vector<identity_ref> local_type_values;
    std::vector<identity_ref> slots;
    std::vector<const source_interface*> imported_interfaces;
};

struct source_environment_import final {
    std::uint32_t visible_from = 0;
    const source_interface* interface = nullptr;
};

// Non-owning positional include environment for one Parser invocation. An import
// participates in lookup only after its original #include source position.
class source_environment final {
public:
    source_environment() noexcept = default;
    explicit source_environment(std::span<const source_environment_import> import_values) noexcept
        : imports(import_values) {}

    [[nodiscard]] identity_ref find_type(
        identity_ref scope,
        std::string_view name,
        std::uint32_t source_offset) const noexcept;

private:
    std::span<const source_environment_import> imports;
};

} // namespace cw::server
