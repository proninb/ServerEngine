#pragma once

#include "../../string_id.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace cw::server {

class identity_space;

enum class identity_kind : std::uint8_t {
    root,
    namespace_scope,
    type,
    object,
};

// Four-byte Project-local semantic reference. The top two bits carry identity
// kind; the remaining 30 bits address one identity slot. Values are valid only
// inside one compiled Project baseline/current build and are not stable across
// REBUILD/UNLOAD.
class identity_ref final {
public:
    constexpr identity_ref() noexcept = default;
    constexpr identity_ref(std::nullptr_t) noexcept {}

    [[nodiscard]] constexpr std::uint32_t value() const noexcept {
        return value_storage;
    }

    [[nodiscard]] constexpr std::uint32_t slot() const noexcept {
        return value_storage & slot_mask;
    }

    [[nodiscard]] constexpr identity_kind kind() const noexcept {
        return static_cast<identity_kind>(value_storage >> kind_shift);
    }

    [[nodiscard]] constexpr bool valid() const noexcept {
        return slot() != 0;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return valid();
    }

    friend constexpr bool operator==(identity_ref, identity_ref) noexcept = default;

private:
    static constexpr std::uint32_t kind_shift = 30;
    static constexpr std::uint32_t slot_mask = (std::uint32_t{1} << kind_shift) - 1;
    static constexpr std::uint32_t maximum_slot = slot_mask;

    [[nodiscard]] static constexpr identity_ref make(
        std::uint32_t slot,
        identity_kind kind) noexcept {

        identity_ref output;
        output.value_storage =
            (static_cast<std::uint32_t>(kind) << kind_shift) | slot;
        return output;
    }

    std::uint32_t value_storage = 0;

    friend class identity_space;
};

static_assert(sizeof(identity_ref) == 4);
static_assert(std::is_trivially_copyable_v<identity_ref>);
static_assert(std::is_standard_layout_v<identity_ref>);
static_assert(!std::is_pointer_v<identity_ref>);

// Immutable metadata stored at one identity slot. Kind lives in identity_ref,
// leaving the record to carry only the hierarchy edge and canonical local atom.
class identity_node final {
public:
    constexpr identity_node() noexcept = default;

    [[nodiscard]] constexpr identity_ref parent() const noexcept {
        return parent_identity;
    }

    [[nodiscard]] constexpr string_id name() const noexcept {
        return local_name;
    }

private:
    struct construction_token {};
    friend class identity_space;

    constexpr identity_node(
        construction_token,
        identity_ref parent,
        string_id name) noexcept
        : parent_identity(parent),
          local_name(name) {}

    identity_ref parent_identity{};
    string_id local_name{};
};

static_assert(sizeof(identity_node) == 8);
static_assert(std::is_trivially_copyable_v<identity_node>);
static_assert(std::is_standard_layout_v<identity_node>);

} // namespace cw::server
