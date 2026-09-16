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
        std::array<std::size_t, 9> state{};
        friend class source_frontend_block_store;
    };

    explicit source_frontend_block_store(
        std::size_t page_bytes = 64 * 1024) noexcept;

    source_frontend_block_store(
        const source_frontend_block_store&) = delete;
    source_frontend_block_store& operator=(
        const source_frontend_block_store&) = delete;
    source_frontend_block_store(
        source_frontend_block_store&&) noexcept;
    source_frontend_block_store& operator=(
        source_frontend_block_store&&) noexcept;
    ~source_frontend_block_store();

    [[nodiscard]] status append(
        source_id source,
        source_interface_data_view data,
        source_frontend_block_ref& output) noexcept;

    [[nodiscard]] status view(
        source_frontend_block_ref block,
        source_interface_data_view& output) const noexcept;

    [[nodiscard]] source_id source(
        source_frontend_block_ref block) const noexcept;

    [[nodiscard]] std::size_t block_count() const noexcept;

    [[nodiscard]] checkpoint mark() const noexcept;
    void restore(checkpoint state) noexcept;

    void clear() noexcept;

private:
    class implementation;
    std::unique_ptr<implementation> value;
};

} // namespace cw::server
