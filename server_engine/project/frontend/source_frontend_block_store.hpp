#pragma once

#include "../parser/source_environment.hpp"
#include "../../source_id.hpp"
#include "../../status.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace cw::server {

inline constexpr std::size_t
source_frontend_block_initial_chunk_bytes =
    64 * 1024;

inline constexpr std::size_t
source_frontend_block_maximum_chunk_bytes =
    4 * 1024 * 1024;

// Generation-local handle to one immutable Source frontend block. The handle
// identifies storage inside one source_frontend_block_store and is not stable
// across REBUILD/UNLOAD or between independent stores.
class source_frontend_block_ref final {
public:
    constexpr source_frontend_block_ref() noexcept = default;

    [[nodiscard]] constexpr std::uint32_t value() const noexcept {
        return slot;
    }

    [[nodiscard]] constexpr bool valid() const noexcept {
        return slot != 0;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return valid();
    }

    friend constexpr bool operator==(
        source_frontend_block_ref,
        source_frontend_block_ref) noexcept = default;

private:
    explicit constexpr source_frontend_block_ref(
        std::uint32_t value) noexcept
        : slot(value) {}

    std::uint32_t slot = 0;

    friend class source_frontend_block_store;
};

static_assert(sizeof(source_frontend_block_ref) == 4);
static_assert(std::is_trivially_copyable_v<source_frontend_block_ref>);

struct source_frontend_block_logical_range final {
    std::uint32_t begin = 0;
    std::uint32_t count = 0;
};

// Flat logical Build Cache-compatible layout of one immutable frontend block.
// Payload bytes remain page-owned by source_frontend_block_store.
struct source_frontend_block_layout final {
    source_id source{};
    source_frontend_block_logical_range local_types{};
    source_frontend_block_logical_range type_slots{};
    source_frontend_block_logical_range object_slots{};
    source_frontend_block_logical_range member_slots{};
};

// Pins the immutable page owner of one Frontend Generation. The token contains
// no semantic copy: it only extends the lifetime of page storage already built
// by source_frontend_block_store until the committed Generation is released.
class source_frontend_block_store_lifetime final {
public:
    source_frontend_block_store_lifetime() noexcept = default;

    [[nodiscard]] bool valid() const noexcept {
        return owner != nullptr;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return valid();
    }

private:
    std::shared_ptr<const void> owner;
    friend class source_frontend_block_store;
};

// Append-only Generation-owned storage for immutable Source frontend blocks.
// Typed pages keep previously published spans stable while new/changed Sources
// append blocks without per-Source heap allocations or cascading offsets.
class source_frontend_block_store final {
public:
    // Opaque rollback token for unpublished append work. Restoring a token
    // never changes any block that existed when mark() was called.
    class checkpoint final {
    public:
        checkpoint() noexcept = default;

    private:
        std::array<std::size_t, 17> state{};
        friend class source_frontend_block_store;
    };

    explicit source_frontend_block_store(
        std::size_t page_bytes =
            source_frontend_block_initial_chunk_bytes) noexcept;

    source_frontend_block_store(
        const source_frontend_block_store&) = delete;
    source_frontend_block_store& operator=(
        const source_frontend_block_store&) = delete;
    source_frontend_block_store(
        source_frontend_block_store&&) noexcept;
    source_frontend_block_store& operator=(
        source_frontend_block_store&&) noexcept;
    ~source_frontend_block_store();

    [[nodiscard]] bool valid() const noexcept {
        return value != nullptr;
    }

    [[nodiscard]] status append(
        source_id source,
        source_interface_data_view data,
        source_frontend_block_ref& output) noexcept;

    [[nodiscard]] status view(
        source_frontend_block_ref block,
        source_interface_data_view& output) const noexcept;

    [[nodiscard]] source_id source(
        source_frontend_block_ref block) const noexcept;

    [[nodiscard]] status layout(
        source_frontend_block_ref block,
        source_frontend_block_layout& output) const noexcept;

    [[nodiscard]] std::size_t local_type_count() const noexcept;
    [[nodiscard]] std::size_t type_slot_count() const noexcept;
    [[nodiscard]] std::size_t object_slot_count() const noexcept;
    [[nodiscard]] std::size_t member_slot_count() const noexcept;

    [[nodiscard]] std::size_t local_type_page_count() const noexcept;
    [[nodiscard]] std::size_t type_slot_page_count() const noexcept;
    [[nodiscard]] std::size_t object_slot_page_count() const noexcept;
    [[nodiscard]] std::size_t member_slot_page_count() const noexcept;

    [[nodiscard]] std::span<const identity_ref> local_type_page(
        std::size_t index) const noexcept;
    [[nodiscard]] std::span<const source_interface_type_slot> type_slot_page(
        std::size_t index) const noexcept;
    [[nodiscard]] std::span<const source_interface_object_slot> object_slot_page(
        std::size_t index) const noexcept;
    [[nodiscard]] std::span<const source_interface_member_slot> member_slot_page(
        std::size_t index) const noexcept;

    [[nodiscard]] std::size_t block_count() const noexcept;

    // Returns a small ownership token for the existing immutable pages. No page
    // data is copied and all previously published spans remain address-stable.
    [[nodiscard]] source_frontend_block_store_lifetime
    pin_lifetime() const noexcept;

    [[nodiscard]] checkpoint mark() const noexcept;
    void restore(checkpoint state) noexcept;

    void clear() noexcept;

private:
    class implementation;
    std::shared_ptr<implementation> value;
};

} // namespace cw::server
