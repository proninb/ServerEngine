#include "string_table.hpp"

#include "../persistence/compiled_image.hpp"

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

string_table::bucket_page::bucket_page() noexcept {
    for (auto& slot : slots)
        slot.store(nullptr, std::memory_order_relaxed);
}

string_table::string_table() noexcept {
    initialize_indexes(true);
}

string_table::string_table(
    const compiled_image_view& baseline_value) noexcept
    : baseline(&baseline_value),
      baseline_slot_count(baseline_value.string_slot_count()),
      baseline_live_count(baseline_value.string_count()) {

    if (baseline_slot_count < (std::numeric_limits<std::uint32_t>::max)()) {
        next_id.store(
            static_cast<std::uint32_t>(baseline_slot_count + 1),
            std::memory_order_relaxed);
    } else {
        next_id.store(0, std::memory_order_relaxed);
    }

    initialize_indexes(false);
}

void string_table::initialize_indexes(
    bool dense_buckets_value) noexcept {

    dense_bucket_mode = dense_buckets_value;

    for (auto& page : bucket_pages)
        page.store(nullptr, std::memory_order_relaxed);

    if (dense_bucket_mode) {
        dense_buckets.reset(
            new (std::nothrow)
                std::atomic<record*>[bucket_count]);

        if (dense_buckets != nullptr) {
            for (std::size_t index = 0;
                 index < bucket_count;
                 ++index) {
                dense_buckets[index].store(
                    nullptr,
                    std::memory_order_relaxed);
            }
        }
    }
    else {
        dense_buckets.reset();
    }

    pages.reset(
        new (std::nothrow)
            std::atomic<record_page*>[page_count]);

    if (pages != nullptr) {
        for (std::size_t index = 0;
             index < page_count;
             ++index) {
            pages[index].store(
                nullptr,
                std::memory_order_relaxed);
        }
    }
}

std::atomic<string_table::record*>*
string_table::ensure_bucket_slot(
    std::size_t bucket_index) noexcept {

    if (bucket_index >= bucket_count)
        return nullptr;

    if (dense_bucket_mode) {
        return dense_buckets != nullptr
            ? &dense_buckets[bucket_index]
            : nullptr;
    }

    const auto directory_index =
        bucket_index >> bucket_page_shift;
    const auto slot_index =
        bucket_index & bucket_page_mask;

    auto* page =
        bucket_pages[directory_index].load(
            std::memory_order_acquire);

    if (page == nullptr) {
        auto* candidate =
            new (std::nothrow) bucket_page;
        if (candidate == nullptr)
            return nullptr;

        bucket_page* expected = nullptr;
        if (bucket_pages[directory_index].
                compare_exchange_strong(
                    expected,
                    candidate,
                    std::memory_order_release,
                    std::memory_order_acquire)) {
            page = candidate;
            bucket_page_bytes.fetch_add(
                sizeof(bucket_page),
                std::memory_order_relaxed);
        }
        else {
            delete candidate;
            page = expected;
        }
    }

    return &page->slots[slot_index];
}

const std::atomic<string_table::record*>*
string_table::bucket_slot(
    std::size_t bucket_index) const noexcept {

    if (bucket_index >= bucket_count)
        return nullptr;

    if (dense_bucket_mode) {
        return dense_buckets != nullptr
            ? &dense_buckets[bucket_index]
            : nullptr;
    }

    const auto directory_index =
        bucket_index >> bucket_page_shift;
    const auto slot_index =
        bucket_index & bucket_page_mask;

    auto* page =
        bucket_pages[directory_index].load(
            std::memory_order_acquire);

    return page == nullptr
        ? nullptr
        : &page->slots[slot_index];
}

string_table::~string_table() noexcept {
    for (auto& page_slot : bucket_pages)
        delete page_slot.load(std::memory_order_relaxed);

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

    const auto* bucket = bucket_slot(
        static_cast<std::size_t>(hash) &
            bucket_mask);

    if (bucket == nullptr)
        return nullptr;

    auto* item =
        bucket->load(std::memory_order_acquire);
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
    if (pages == nullptr)
        return {status_code::not_available};
    if (value.size() > (std::numeric_limits<std::uint32_t>::max)())
        return {status_code::not_available};
    if (sizeof(record) > (std::numeric_limits<std::size_t>::max)() - value.size())
        return {status_code::not_available};

    if (const auto existing = find(value); existing) {
        output = existing;
        return {};
    }

    const auto hash = hash_text(value);
    auto* bucket = ensure_bucket_slot(
        static_cast<std::size_t>(hash) &
            bucket_mask);
    if (bucket == nullptr)
        return {status_code::not_available};

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

    auto* head =
        bucket->load(std::memory_order_acquire);
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
        if (bucket->compare_exchange_weak(
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

    if (const auto* found = find_record(value, hash_text(value)); found != nullptr)
        return string_id{found->id};

    if (baseline != nullptr) {
        string_id output;
        if (baseline->find_string(value, output).ok())
            return output;
    }

    return {};
}

std::string_view string_table::get(string_id id) const noexcept {
    if (!id)
        return {};

    if (baseline != nullptr && id.value() <= baseline_slot_count)
        return baseline->string(id);

    auto* direct_page = page(id.value());
    if (direct_page == nullptr)
        return {};
    const auto slot_index = static_cast<std::size_t>(id.value() - 1) & page_mask;
    const auto* value = direct_page->slots[slot_index].load(std::memory_order_acquire);
    if (value == nullptr)
        return {};
    return {value->bytes(), value->length};
}

string_id string_table::at_slot(std::size_t index) const noexcept {
    if (index >= maximum_id)
        return {};

    if (baseline != nullptr && index < baseline_slot_count)
        return baseline->string_at_slot(index);

    const auto raw_id = static_cast<std::uint32_t>(index + 1);
    auto* direct_page = page(raw_id);
    if (direct_page == nullptr)
        return {};

    const auto slot_index =
        static_cast<std::size_t>(raw_id - 1) & page_mask;
    const auto* value =
        direct_page->slots[slot_index].load(std::memory_order_acquire);

    return value != nullptr && value->id == raw_id
        ? string_id{raw_id}
        : string_id{};
}

string_table_statistics string_table::statistics() const noexcept {
    string_table_statistics output;
    output.strings = size();
    output.bytes_reserved =
        storage.bytes_reserved() +
        (dense_bucket_mode
            ? bucket_count *
                sizeof(std::atomic<record*>)
            : sizeof(bucket_pages) +
                bucket_page_bytes.load(
                    std::memory_order_relaxed)) +
        page_count *
            sizeof(std::atomic<record_page*>) +
        page_bytes.load(std::memory_order_relaxed);

    const auto account_chain =
        [&](const record* item) noexcept {
            if (item == nullptr)
                return;

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
        };

    if (dense_bucket_mode) {
        if (dense_buckets != nullptr) {
            for (std::size_t index = 0;
                 index < bucket_count;
                 ++index) {
                account_chain(
                    dense_buckets[index].load(
                        std::memory_order_acquire));
            }
        }
    }
    else {
        for (const auto& page_slot : bucket_pages) {
            const auto* page =
                page_slot.load(
                    std::memory_order_acquire);
            if (page == nullptr)
                continue;

            for (const auto& bucket : page->slots) {
                account_chain(
                    bucket.load(
                        std::memory_order_acquire));
            }
        }
    }
    return output;
}

} // namespace cw::server
