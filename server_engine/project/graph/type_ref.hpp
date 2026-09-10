#pragma once

#include "type_handle.hpp"

#include <cstdint>
#include <type_traits>

namespace cw::server {

class graph;
class generation_builder;

// Identifies one canonical type expression inside a committed Graph generation.
// TypeRef is compact and generation-local; index zero is the invalid sentinel.
class TypeRef final {
public:
    constexpr TypeRef() noexcept = default;

    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return index; }
    [[nodiscard]] constexpr bool valid() const noexcept { return index != 0; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

    friend constexpr bool operator==(TypeRef, TypeRef) noexcept = default;

private:
    explicit constexpr TypeRef(std::uint32_t value) noexcept : index(value) {}

    std::uint32_t index = 0;

    friend class graph;
    friend class generation_builder;
};

static_assert(sizeof(TypeRef) == 4);
static_assert(std::is_trivially_copyable_v<TypeRef>);
static_assert(std::is_standard_layout_v<TypeRef>);

enum class canonical_type_kind : std::uint8_t {
    intrinsic,
    named,
    derived,
};

enum class derived_type_kind : std::uint8_t {
    const_qualified,
    volatile_qualified,
    pointer,
    lvalue_reference,
    rvalue_reference,
    bounded_array,
    unbounded_array,
};

// Describes one modifier wrapped around an immediate child TypeRef.
struct derived_type_record final {
    std::uint64_t payload = 0;
    TypeRef child{};
    derived_type_kind kind = derived_type_kind::pointer;
};

static_assert(sizeof(derived_type_record) == 16);

} // namespace cw::server
