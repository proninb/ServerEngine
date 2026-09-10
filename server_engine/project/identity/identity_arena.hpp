#pragma once

#include "../../status.hpp"

#include <atomic>
#include <cstddef>
#include <memory>

namespace cw::server {

// Concurrent Project-lifetime bump storage. It owns bytes only and performs no
// identity lookup, canonicalization, hashing, ordering, or synchronization lock.
class identity_arena final {
public:
    identity_arena() noexcept = default;
    ~identity_arena() noexcept;

    identity_arena(const identity_arena&) = delete;
    identity_arena& operator=(const identity_arena&) = delete;

    [[nodiscard]] status allocate(
        std::size_t size,
        std::size_t alignment,
        void*& output) noexcept;

    [[nodiscard]] std::size_t bytes_reserved() const noexcept {
        return reserved_bytes.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t pages_reserved() const noexcept {
        return reserved_pages.load(std::memory_order_relaxed);
    }

private:
    static constexpr std::size_t default_page_size = 64u * 1024u;

    struct page {
        std::unique_ptr<std::byte[]> data;
        std::atomic<std::size_t> used{0};
        std::size_t capacity = 0;
        page* next_owned = nullptr;
    };

    [[nodiscard]] page* make_page(std::size_t minimum_capacity) noexcept;
    void own_page(page* value) noexcept;

    std::atomic<page*> current_page{nullptr};
    std::atomic<page*> owned_pages{nullptr};
    std::atomic<std::size_t> reserved_bytes{0};
    std::atomic<std::size_t> reserved_pages{0};
};

} // namespace cw::server
