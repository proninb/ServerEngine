#pragma once

#include "../../status.hpp"
#include "identity_node.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace cw::server {

class compiled_image_view;

struct identity_index_statistics {
    std::size_t entry_count = 0;
    std::size_t occupied_buckets = 0;
    std::size_t collision_entries = 0;
    std::size_t max_chain_length = 0;
    double average_chain_length = 0.0;
    double average_successful_lookup_comparisons = 0.0;
    std::size_t p95_successful_lookup_comparisons = 0;
    std::size_t p99_successful_lookup_comparisons = 0;
};

class identity_space;

// Narrow immutable identity metadata accessor. It carries the owning space
// explicitly; identity_ref itself remains a four-byte relocatable value.
class identity_view final {
public:
    identity_view() noexcept = default;

    [[nodiscard]] bool valid(identity_ref identity) const noexcept;
    [[nodiscard]] identity_ref parent(identity_ref identity) const noexcept;
    [[nodiscard]] string_id name(identity_ref identity) const noexcept;

private:
    explicit identity_view(const identity_space& owner_value) noexcept
        : owner(&owner_value) {}

    const identity_space* owner = nullptr;

    friend class identity_space;
};

// Project-lifetime semantic identity tree. identity_ref is a compact Project-local
// slot; the concurrent index canonicalizes (parent, local name) and rejects a
// same-name/different-kind declaration. Metadata storage is lazy page-addressed.
class identity_space final {
public:
    identity_space() noexcept;
    explicit identity_space(const compiled_image_view& baseline_value) noexcept;
    ~identity_space() noexcept;

    identity_space(const identity_space&) = delete;
    identity_space& operator=(const identity_space&) = delete;

    [[nodiscard]] identity_ref root() const noexcept {
        return identity_ref::make(1, identity_kind::root);
    }

    [[nodiscard]] identity_view view() const noexcept {
        return identity_view{*this};
    }

    [[nodiscard]] status resolve_declaration(
        identity_ref parent,
        string_id local_name,
        identity_kind kind,
        identity_ref& output) noexcept;

    [[nodiscard]] identity_ref find(
        identity_ref parent,
        string_id local_name,
        identity_kind kind) const noexcept;

    [[nodiscard]] bool valid(identity_ref identity) const noexcept;
    [[nodiscard]] identity_ref parent(identity_ref identity) const noexcept;
    [[nodiscard]] string_id name(identity_ref identity) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept {
        return baseline_identity_count + identity_count.load(std::memory_order_relaxed);
    }

    // Includes rare unpublished holes left by losing concurrent candidates.
    // They are construction pressure only and disappear on the next REBUILD.
    [[nodiscard]] std::size_t slot_count() const noexcept {
        const auto next = next_slot.load(std::memory_order_relaxed);
        return next > 1 ? static_cast<std::size_t>(next - 1) : 1;
    }

    [[nodiscard]] identity_ref at_slot(std::size_t index) const noexcept;

    [[nodiscard]] std::size_t bytes_reserved() const noexcept {
        return semantic_bucket_bytes + sizeof(directories) +
            allocated_directory_bytes.load(std::memory_order_relaxed) +
            allocated_page_bytes.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t pages_reserved() const noexcept {
        return allocated_pages.load(std::memory_order_relaxed);
    }

    [[nodiscard]] constexpr std::size_t bucket_count() const noexcept {
        return semantic_bucket_count;
    }

    [[nodiscard]] identity_index_statistics index_statistics() const noexcept;

private:
    // 16-byte construction/runtime record. fingerprint==0 means the slot is not
    // published. next_bucket stores an encoded identity_ref value.
    struct record final {
        identity_node identity{};
        std::uint32_t fingerprint = 0;
        std::uint32_t next_bucket = 0;
    };

    static_assert(sizeof(record) == 16);

    static constexpr std::size_t semantic_bucket_count = std::size_t{1} << 21;
    static constexpr std::size_t semantic_bucket_mask = semantic_bucket_count - 1;
    static constexpr std::size_t semantic_bucket_bytes =
        semantic_bucket_count * sizeof(std::atomic<std::uint32_t>);

    static constexpr std::size_t page_shift = 12;
    static constexpr std::size_t page_size = std::size_t{1} << page_shift;
    static constexpr std::size_t page_mask = page_size - 1;

    static constexpr std::size_t directory_page_bits = 9;
    static constexpr std::size_t directory_page_count =
        std::size_t{1} << directory_page_bits;
    static constexpr std::size_t directory_page_mask =
        directory_page_count - 1;

    // 30-bit slot -> 18-bit page number. Split that page number 9+9 so the
    // initial directory costs only 4 KiB on a 64-bit host.
    static constexpr std::size_t directory_count =
        std::size_t{1} << (identity_ref::kind_shift - page_shift - directory_page_bits);

    struct record_page final {
        record slots[page_size]{};
    };

    struct page_directory final {
        std::array<std::atomic<record_page*>, directory_page_count> pages{};

        page_directory() noexcept;
    };

    [[nodiscard]] static std::uint64_t semantic_hash_value(
        identity_ref parent,
        string_id local_name) noexcept;

    [[nodiscard]] static std::uint32_t semantic_fingerprint(
        std::uint64_t hash) noexcept;

    [[nodiscard]] static constexpr std::uint32_t packed_fingerprint(
        std::uint32_t fingerprint,
        identity_kind kind) noexcept {
        return fingerprint |
            (static_cast<std::uint32_t>(kind) << identity_ref::kind_shift);
    }

    [[nodiscard]] page_directory* ensure_directory(
        std::size_t directory_index) noexcept;

    [[nodiscard]] record_page* ensure_page(std::uint32_t slot) noexcept;

    [[nodiscard]] record* slot_record(std::uint32_t slot) noexcept;
    [[nodiscard]] const record* slot_record(std::uint32_t slot) const noexcept;
    [[nodiscard]] const record* published_record(identity_ref identity) const noexcept;

    [[nodiscard]] identity_ref find_record(
        identity_ref parent,
        string_id local_name,
        std::uint64_t hash,
        std::uint32_t fingerprint) const noexcept;

    [[nodiscard]] status make_candidate(
        identity_ref parent,
        string_id local_name,
        identity_kind kind,
        std::uint32_t fingerprint,
        identity_ref& reference,
        record*& output) noexcept;

    void discard_candidate(record& candidate) noexcept;

    const compiled_image_view* baseline = nullptr;
    std::size_t baseline_slot_count = 0;
    std::size_t baseline_identity_count = 0;

    record root_record{};
    std::unique_ptr<std::atomic<std::uint32_t>[]> buckets;
    std::array<std::atomic<page_directory*>, directory_count> directories{};
    std::atomic<std::uint32_t> next_slot{2};
    std::atomic<std::size_t> identity_count{1};
    std::atomic<std::size_t> allocated_pages{0};
    std::atomic<std::size_t> allocated_directory_bytes{0};
    std::atomic<std::size_t> allocated_page_bytes{0};

    friend class identity_view;
};

} // namespace cw::server
