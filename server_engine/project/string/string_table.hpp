#pragma once

#include "../../status.hpp"
#include "../../string_id.hpp"
#include "../storage/byte_arena.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace cw::server {

class compiled_image_view;

struct string_table_statistics final {
    std::size_t strings = 0;
    std::size_t occupied_buckets = 0;
    std::size_t collision_entries = 0;
    std::size_t max_chain_length = 0;
    std::size_t bytes_reserved = 0;
};

// Owns Project-lifetime text atoms created after the active baseline. Persisted
// IDs stay mmap-backed and immutable; only post-baseline atoms enter local pages.
class string_table final {
public:
    string_table() noexcept;
    explicit string_table(const compiled_image_view& baseline_value) noexcept;
    ~string_table() noexcept;

    string_table(const string_table&) = delete;
    string_table& operator=(const string_table&) = delete;

    [[nodiscard]] status intern(std::string_view value, string_id& output) noexcept;
    [[nodiscard]] string_id find(std::string_view value) const noexcept;
    [[nodiscard]] std::string_view get(string_id id) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept {
        return baseline_live_count + live_count.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t slot_count() const noexcept {
        const auto next = next_id.load(std::memory_order_relaxed);
        return next > 1 ? static_cast<std::size_t>(next - 1) : 0;
    }

    [[nodiscard]] string_id at_slot(std::size_t index) const noexcept;
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
    static constexpr std::size_t bucket_page_shift = 10;
    static constexpr std::size_t bucket_page_size =
        std::size_t{1} << bucket_page_shift;
    static constexpr std::size_t bucket_page_mask =
        bucket_page_size - 1;
    static constexpr std::size_t bucket_directory_count =
        bucket_count / bucket_page_size;

    struct bucket_page final {
        bucket_page() noexcept;
        std::atomic<record*> slots[bucket_page_size]{};
    };

    static constexpr std::size_t page_shift = 12;
    static constexpr std::size_t page_size = std::size_t{1} << page_shift;
    static constexpr std::size_t page_mask = page_size - 1;
    static constexpr std::size_t page_count = std::size_t{1} << 16;
    static constexpr std::uint64_t maximum_id =
        static_cast<std::uint64_t>(page_count) * page_size;

    struct record_page {
        std::atomic<record*> slots[page_size]{};
    };

    void initialize_indexes(bool dense_buckets) noexcept;

    [[nodiscard]] std::atomic<record*>* ensure_bucket_slot(
        std::size_t bucket_index) noexcept;
    [[nodiscard]] const std::atomic<record*>* bucket_slot(
        std::size_t bucket_index) const noexcept;

    [[nodiscard]] static std::uint64_t hash_text(std::string_view value) noexcept;
    [[nodiscard]] record* find_record(std::string_view value, std::uint64_t hash) const noexcept;
    [[nodiscard]] record_page* ensure_page(std::uint32_t id) noexcept;
    [[nodiscard]] record_page* page(std::uint32_t id) const noexcept;

    const compiled_image_view* baseline = nullptr;
    std::size_t baseline_slot_count = 0;
    std::size_t baseline_live_count = 0;

    byte_arena storage;
    std::unique_ptr<std::atomic<record*>[]> dense_buckets;
    std::array<std::atomic<bucket_page*>, bucket_directory_count>
        bucket_pages{};
    std::unique_ptr<std::atomic<record_page*>[]> pages;
    std::atomic<std::uint32_t> next_id{1};
    std::atomic<std::size_t> live_count{0};
    std::atomic<std::size_t> bucket_page_bytes{0};
    std::atomic<std::size_t> page_bytes{0};
    bool dense_bucket_mode = true;
};

} // namespace cw::server
