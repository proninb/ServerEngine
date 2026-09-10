#include "string_table.hpp"

#include <cstring>
#include <limits>
#include <new>

namespace cw::server {
namespace {

[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

} // namespace

string_table::string_table() noexcept
    : buckets(new (std::nothrow) std::atomic<record*>[bucket_count]),
      pages(new (std::nothrow) std::atomic<record_page*>[page_count]) {

    if (buckets != nullptr) {
        for (std::size_t index = 0; index < bucket_count; ++index)
            buckets[index].store(nullptr, std::memory_order_relaxed);
    }
    if (pages != nullptr) {
        for (std::size_t index = 0; index < page_count; ++index)
            pages[index].store(nullptr, std::memory_order_relaxed);
    }
}

string_table::~string_table() noexcept {
    if (pages == nullptr)
        return;
    for (std::size_t index = 0; index < page_count; ++index)
        delete pages[index].load(std::memory_order_relaxed);
}

std::uint64_t string_table::hash_text(std::string_view value) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return mix64(hash);
}

string_table::record* string_table::find_record(
    std::string_view value,
    std::uint64_t hash) const noexcept {

    if (buckets == nullptr)
        return nullptr;

    auto* item = buckets[static_cast<std::size_t>(hash) & bucket_mask].load(
        std::memory_order_acquire);
    while (item != nullptr) {
        if (item->hash == hash && item->length == value.size() &&
            (value.empty() || std::memcmp(item->bytes(), value.data(), value.size()) == 0)) {
            return item;
        }
        item = item->next;
    }
    return nullptr;
}

string_table::record_page* string_table::ensure_page(std::uint32_t id) noexcept {
    if (pages == nullptr || id == 0)
        return nullptr;

    const auto page_index = static_cast<std::size_t>(id - 1) >> page_shift;
    if (page_index >= page_count)
        return nullptr;

    auto* existing = pages[page_index].load(std::memory_order_acquire);
    if (existing != nullptr)
        return existing;

    auto* candidate = new (std::nothrow) record_page{};
    if (candidate == nullptr)
        return nullptr;

    record_page* expected = nullptr;
    if (pages[page_index].compare_exchange_strong(
            expected,
            candidate,
            std::memory_order_release,
            std::memory_order_acquire)) {
        page_bytes.fetch_add(sizeof(record_page), std::memory_order_relaxed);
        return candidate;
    }

    delete candidate;
    return expected;
}

string_table::record_page* string_table::page(std::uint32_t id) const noexcept {
    if (pages == nullptr || id == 0)
        return nullptr;
    const auto page_index = static_cast<std::size_t>(id - 1) >> page_shift;
    if (page_index >= page_count)
        return nullptr;
    return pages[page_index].load(std::memory_order_acquire);
}

status string_table::intern(std::string_view value, string_id& output) noexcept {
    output = {};
    if (value.empty())
        return {status_code::invalid_argument};
    if (buckets == nullptr || pages == nullptr)
        return {status_code::not_available};
    if (value.size() > (std::numeric_limits<std::uint32_t>::max)())
        return {status_code::not_available};
    if (sizeof(record) > (std::numeric_limits<std::size_t>::max)() - value.size())
        return {status_code::not_available};

    const auto hash = hash_text(value);
    if (const auto* existing = find_record(value, hash); existing != nullptr) {
        output = string_id{existing->id};
        return {};
    }

    void* memory = nullptr;
    auto result = storage.allocate(sizeof(record) + value.size(), alignof(record), memory);
    if (!result.ok())
        return result;

    auto* candidate = ::new (memory) record{};
    candidate->hash = hash;
    candidate->length = static_cast<std::uint32_t>(value.size());
    std::memcpy(reinterpret_cast<char*>(candidate + 1), value.data(), value.size());

    const auto raw_id = next_id.fetch_add(1, std::memory_order_relaxed);
    if (raw_id == 0 || static_cast<std::uint64_t>(raw_id) > maximum_id)
        return {status_code::not_available};
    candidate->id = raw_id;

    auto* direct_page = ensure_page(raw_id);
    if (direct_page == nullptr)
        return {status_code::not_available};
    const auto slot_index = static_cast<std::size_t>(raw_id - 1) & page_mask;
    direct_page->slots[slot_index].store(candidate, std::memory_order_release);

    auto& bucket = buckets[static_cast<std::size_t>(hash) & bucket_mask];
    auto* head = bucket.load(std::memory_order_acquire);
    for (;;) {
        for (auto* item = head; item != nullptr; item = item->next) {
            if (item->hash == hash && item->length == value.size() &&
                std::memcmp(item->bytes(), value.data(), value.size()) == 0) {
                direct_page->slots[slot_index].store(nullptr, std::memory_order_release);
                output = string_id{item->id};
                return {};
            }
        }

        candidate->next = head;
        if (bucket.compare_exchange_weak(
                head,
                candidate,
                std::memory_order_release,
                std::memory_order_acquire)) {
            live_count.fetch_add(1, std::memory_order_relaxed);
            output = string_id{raw_id};
            return {};
        }
    }
}

string_id string_table::find(std::string_view value) const noexcept {
    if (value.empty())
        return {};
    const auto* found = find_record(value, hash_text(value));
    return found == nullptr ? string_id{} : string_id{found->id};
}

std::string_view string_table::get(string_id id) const noexcept {
    if (!id)
        return {};
    auto* direct_page = page(id.value());
    if (direct_page == nullptr)
        return {};
    const auto slot_index = static_cast<std::size_t>(id.value() - 1) & page_mask;
    const auto* value = direct_page->slots[slot_index].load(std::memory_order_acquire);
    if (value == nullptr)
        return {};
    return {value->bytes(), value->length};
}

string_table_statistics string_table::statistics() const noexcept {
    string_table_statistics output;
    output.strings = size();
    output.bytes_reserved = storage.bytes_reserved() +
        bucket_count * sizeof(std::atomic<record*>) +
        page_count * sizeof(std::atomic<record_page*>) +
        page_bytes.load(std::memory_order_relaxed);

    if (buckets == nullptr)
        return output;
    for (std::size_t index = 0; index < bucket_count; ++index) {
        const auto* item = buckets[index].load(std::memory_order_acquire);
        if (item == nullptr)
            continue;
        ++output.occupied_buckets;
        std::size_t chain = 0;
        while (item != nullptr) {
            ++chain;
            item = item->next;
        }
        if (chain > 1)
            output.collision_entries += chain - 1;
        if (chain > output.max_chain_length)
            output.max_chain_length = chain;
    }
    return output;
}

} // namespace cw::server
