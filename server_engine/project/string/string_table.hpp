#pragma once

#include "../../status.hpp"
#include "../../string_id.hpp"
#include "../identity/identity_arena.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace cw::server {

struct string_table_statistics final {
    std::size_t strings = 0;
    std::size_t occupied_buckets = 0;
    std::size_t collision_entries = 0;
    std::size_t max_chain_length = 0;
    std::size_t bytes_reserved = 0;
};

// Owns canonical Project-lifetime identifier/text atoms. Bytes are interned once;
// all semantic layers thereafter carry 32-bit string_id values. The table is safe
// for concurrent Parser identity resolution and performs no semantic lookup.
class string_table final {
public:
    string_table() noexcept;
    ~string_table() noexcept;

    string_table(const string_table&) = delete;
    string_table& operator=(const string_table&) = delete;

    [[nodiscard]] status intern(std::string_view value, string_id& output) noexcept;
    [[nodiscard]] string_id find(std::string_view value) const noexcept;
    [[nodiscard]] std::string_view get(string_id id) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept {
        return live_count.load(std::memory_order_relaxed);
    }

    [[nodiscard]] string_table_statistics statistics() const noexcept;

private:
    struct record {
        std::uint64_t hash = 0;
        record* next = nullptr;
        std::uint32_t id = 0;
        std::uint32_t length = 0;

        [[nodiscard]] const char* bytes() const noexcept {
            return reinterpret_cast<const char*>(this + 1);
        }
    };

    static constexpr std::size_t bucket_count = std::size_t{1} << 20;
    static constexpr std::size_t bucket_mask = bucket_count - 1;
    static constexpr std::size_t page_shift = 12;
    static constexpr std::size_t page_size = std::size_t{1} << page_shift;
    static constexpr std::size_t page_mask = page_size - 1;
    static constexpr std::size_t page_count = std::size_t{1} << 16;
    static constexpr std::uint64_t maximum_id =
        static_cast<std::uint64_t>(page_count) * page_size;

    struct record_page {
        std::atomic<record*> slots[page_size]{};
    };

    [[nodiscard]] static std::uint64_t hash_text(std::string_view value) noexcept;
    [[nodiscard]] record* find_record(std::string_view value, std::uint64_t hash) const noexcept;
    [[nodiscard]] record_page* ensure_page(std::uint32_t id) noexcept;
    [[nodiscard]] record_page* page(std::uint32_t id) const noexcept;

    identity_arena storage;
    std::unique_ptr<std::atomic<record*>[]> buckets;
    std::unique_ptr<std::atomic<record_page*>[]> pages;
    std::atomic<std::uint32_t> next_id{1};
    std::atomic<std::size_t> live_count{0};
    std::atomic<std::size_t> page_bytes{0};
};

} // namespace cw::server
