#include "identity_space.hpp"

#include <array>
#include <limits>
#include <new>

namespace cw::server {

namespace {

[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}

[[nodiscard]] std::size_t percentile_from_histogram(
    const std::array<std::size_t, 256>& histogram,
    std::size_t entry_count,
    std::size_t numerator) noexcept {

    if (entry_count == 0)
        return 0;

    const auto target =
        (entry_count / 100u) * numerator +
        ((entry_count % 100u) * numerator + 99u) / 100u;

    std::size_t cumulative = 0;
    for (std::size_t comparisons = 1; comparisons < histogram.size(); ++comparisons) {
        cumulative += histogram[comparisons];
        if (cumulative >= target)
            return comparisons;
    }
    return histogram.size() - 1;
}

} // namespace

identity_space::page_directory::page_directory() noexcept {
    for (auto& page : pages)
        page.store(nullptr, std::memory_order_relaxed);
}

identity_space::identity_space() noexcept
    : buckets(new (std::nothrow) std::atomic<std::uint32_t>[semantic_bucket_count]) {

    root_record.identity = identity_node{
        identity_node::construction_token{}, identity_ref{}, string_id{}};
    root_record.fingerprint = 1;

    for (auto& directory : directories)
        directory.store(nullptr, std::memory_order_relaxed);

    if (buckets == nullptr)
        return;

    for (std::size_t index = 0; index < semantic_bucket_count; ++index)
        buckets[index].store(0, std::memory_order_relaxed);
}

identity_space::~identity_space() noexcept {
    for (auto& directory_slot : directories) {
        auto* directory = directory_slot.load(std::memory_order_relaxed);
        if (directory == nullptr)
            continue;

        for (auto& page_slot : directory->pages)
            delete page_slot.load(std::memory_order_relaxed);

        delete directory;
    }
}

bool identity_view::valid(identity_ref identity) const noexcept {
    return owner != nullptr && owner->valid(identity);
}

identity_ref identity_view::parent(identity_ref identity) const noexcept {
    return owner == nullptr ? identity_ref{} : owner->parent(identity);
}

string_id identity_view::name(identity_ref identity) const noexcept {
    return owner == nullptr ? string_id{} : owner->name(identity);
}

std::uint64_t identity_space::semantic_hash_value(
    identity_ref parent,
    string_id local_name) noexcept {

    return mix64(
        static_cast<std::uint64_t>(parent.value()) ^
        (static_cast<std::uint64_t>(local_name.value()) << 32));
}

std::uint32_t identity_space::semantic_fingerprint(std::uint64_t hash) noexcept {
    auto result =
        static_cast<std::uint32_t>(hash ^ (hash >> 32)) &
        identity_ref::slot_mask;
    return result == 0 ? 1u : result;
}

identity_space::page_directory* identity_space::ensure_directory(
    std::size_t directory_index) noexcept {

    if (directory_index >= directories.size())
        return nullptr;

    auto* existing = directories[directory_index].load(std::memory_order_acquire);
    if (existing != nullptr)
        return existing;

    auto* candidate = new (std::nothrow) page_directory;
    if (candidate == nullptr)
        return nullptr;

    page_directory* expected = nullptr;
    if (directories[directory_index].compare_exchange_strong(
            expected,
            candidate,
            std::memory_order_release,
            std::memory_order_acquire)) {
        allocated_directory_bytes.fetch_add(
            sizeof(page_directory), std::memory_order_relaxed);
        return candidate;
    }

    delete candidate;
    return expected;
}

identity_space::record_page* identity_space::ensure_page(std::uint32_t slot) noexcept {
    if (slot <= 1 || slot > identity_ref::maximum_slot)
        return nullptr;

    const auto page_number = static_cast<std::size_t>(slot) >> page_shift;
    const auto directory_index = page_number >> directory_page_bits;
    const auto page_index = page_number & directory_page_mask;

    auto* directory = ensure_directory(directory_index);
    if (directory == nullptr)
        return nullptr;

    auto* existing = directory->pages[page_index].load(std::memory_order_acquire);
    if (existing != nullptr)
        return existing;

    auto* candidate = new (std::nothrow) record_page;
    if (candidate == nullptr)
        return nullptr;

    record_page* expected = nullptr;
    if (directory->pages[page_index].compare_exchange_strong(
            expected,
            candidate,
            std::memory_order_release,
            std::memory_order_acquire)) {
        allocated_pages.fetch_add(1, std::memory_order_relaxed);
        allocated_page_bytes.fetch_add(
            sizeof(record_page), std::memory_order_relaxed);
        return candidate;
    }

    delete candidate;
    return expected;
}

identity_space::record* identity_space::slot_record(std::uint32_t slot) noexcept {
    if (slot == 1)
        return &root_record;

    if (slot == 0 || slot > identity_ref::maximum_slot)
        return nullptr;

    const auto page_number = static_cast<std::size_t>(slot) >> page_shift;
    const auto directory_index = page_number >> directory_page_bits;
    const auto page_index = page_number & directory_page_mask;

    if (directory_index >= directories.size())
        return nullptr;

    auto* directory =
        directories[directory_index].load(std::memory_order_acquire);
    if (directory == nullptr)
        return nullptr;

    auto* page = directory->pages[page_index].load(std::memory_order_acquire);
    return page == nullptr ? nullptr : &page->slots[slot & page_mask];
}

const identity_space::record* identity_space::slot_record(
    std::uint32_t slot) const noexcept {

    return const_cast<identity_space*>(this)->slot_record(slot);
}

const identity_space::record* identity_space::published_record(
    identity_ref identity) const noexcept {

    if (!identity)
        return nullptr;

    const auto* record = slot_record(identity.slot());
    if (record == nullptr)
        return nullptr;

    if (identity.slot() == 1)
        return identity.kind() == identity_kind::root ? record : nullptr;

    const auto fingerprint = record->fingerprint & identity_ref::slot_mask;
    const auto kind = static_cast<identity_kind>(
        record->fingerprint >> identity_ref::kind_shift);

    return fingerprint != 0 &&
           kind == identity.kind() &&
           record->identity.name()
        ? record
        : nullptr;
}

bool identity_space::valid(identity_ref identity) const noexcept {
    return published_record(identity) != nullptr;
}

identity_ref identity_space::parent(identity_ref identity) const noexcept {
    const auto* record = published_record(identity);
    return record == nullptr ? identity_ref{} : record->identity.parent();
}

string_id identity_space::name(identity_ref identity) const noexcept {
    const auto* record = published_record(identity);
    return record == nullptr ? string_id{} : record->identity.name();
}

identity_ref identity_space::find_record(
    identity_ref parent,
    string_id local_name,
    std::uint64_t hash,
    std::uint32_t fingerprint) const noexcept {

    if (buckets == nullptr)
        return {};

    auto current_value =
        buckets[static_cast<std::size_t>(hash) & semantic_bucket_mask].load(
            std::memory_order_acquire);

    while (current_value != 0) {
        const auto slot = current_value & identity_ref::slot_mask;
        const auto kind = static_cast<identity_kind>(
            current_value >> identity_ref::kind_shift);
        const auto current = identity_ref::make(slot, kind);

        const auto* item = published_record(current);
        if (item == nullptr)
            return {};

        if ((item->fingerprint & identity_ref::slot_mask) == fingerprint &&
            item->identity.parent() == parent &&
            item->identity.name() == local_name) {
            return current;
        }

        current_value = item->next_bucket;
    }

    return {};
}

status identity_space::make_candidate(
    identity_ref parent,
    string_id local_name,
    identity_kind kind,
    std::uint32_t fingerprint,
    identity_ref& reference,
    record*& output) noexcept {

    reference = {};
    output = nullptr;

    auto slot = next_slot.load(std::memory_order_relaxed);
    for (;;) {
        if (slot == 0 || slot > identity_ref::maximum_slot)
            return {status_code::not_available};

        if (next_slot.compare_exchange_weak(
                slot,
                slot + 1,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            break;
        }
    }

    auto* page = ensure_page(slot);
    if (page == nullptr)
        return {status_code::not_available};

    reference = identity_ref::make(slot, kind);
    output = &page->slots[slot & page_mask];
    output->identity = identity_node{
        identity_node::construction_token{}, parent, local_name};
    output->fingerprint = packed_fingerprint(fingerprint, kind);
    output->next_bucket = 0;
    return {};
}

void identity_space::discard_candidate(record& candidate) noexcept {
    candidate.next_bucket = 0;
    candidate.fingerprint = 0;
    candidate.identity = {};
}

status identity_space::resolve_declaration(
    identity_ref parent,
    string_id local_name,
    identity_kind kind,
    identity_ref& output) noexcept {

    output = {};

    if (!parent || !local_name || kind == identity_kind::root ||
        published_record(parent) == nullptr) {
        return {status_code::invalid_argument};
    }

    if (buckets == nullptr)
        return {status_code::not_available};

    const auto hash = semantic_hash_value(parent, local_name);
    const auto fingerprint = semantic_fingerprint(hash);
    auto& bucket =
        buckets[static_cast<std::size_t>(hash) & semantic_bucket_mask];

    if (const auto existing =
            find_record(parent, local_name, hash, fingerprint);
        existing) {

        if (existing.kind() != kind)
            return {status_code::semantic_conflict};

        output = existing;
        return {};
    }

    identity_ref candidate_ref;
    record* candidate = nullptr;
    const auto result = make_candidate(
        parent,
        local_name,
        kind,
        fingerprint,
        candidate_ref,
        candidate);
    if (!result.ok())
        return result;

    auto head = bucket.load(std::memory_order_acquire);

    for (;;) {
        auto current_value = head;

        while (current_value != 0) {
            const auto slot = current_value & identity_ref::slot_mask;
            const auto current_kind = static_cast<identity_kind>(
                current_value >> identity_ref::kind_shift);
            const auto current = identity_ref::make(slot, current_kind);

            const auto* item = published_record(current);
            if (item == nullptr) {
                discard_candidate(*candidate);
                return {status_code::initialization_failed};
            }

            if ((item->fingerprint & identity_ref::slot_mask) == fingerprint &&
                item->identity.parent() == parent &&
                item->identity.name() == local_name) {

                discard_candidate(*candidate);

                if (current.kind() != kind)
                    return {status_code::semantic_conflict};

                output = current;
                return {};
            }

            current_value = item->next_bucket;
        }

        candidate->next_bucket = head;

        if (bucket.compare_exchange_weak(
                head,
                candidate_ref.value(),
                std::memory_order_release,
                std::memory_order_acquire)) {
            identity_count.fetch_add(1, std::memory_order_relaxed);
            output = candidate_ref;
            return {};
        }
    }
}

identity_ref identity_space::find(
    identity_ref parent,
    string_id local_name,
    identity_kind kind) const noexcept {

    if (!parent || !local_name || kind == identity_kind::root ||
        published_record(parent) == nullptr) {
        return {};
    }

    const auto hash = semantic_hash_value(parent, local_name);
    const auto fingerprint = semantic_fingerprint(hash);
    const auto found = find_record(parent, local_name, hash, fingerprint);

    return found && found.kind() == kind ? found : identity_ref{};
}

identity_index_statistics identity_space::index_statistics() const noexcept {
    identity_index_statistics output;
    if (buckets == nullptr)
        return output;

    std::array<std::size_t, 256> comparison_histogram{};
    std::uint64_t total_comparisons = 0;

    for (std::size_t bucket_index = 0;
         bucket_index < semantic_bucket_count;
         ++bucket_index) {

        auto current_value =
            buckets[bucket_index].load(std::memory_order_acquire);
        if (current_value == 0)
            continue;

        ++output.occupied_buckets;
        std::size_t chain_length = 0;

        while (current_value != 0) {
            const auto slot = current_value & identity_ref::slot_mask;
            const auto kind = static_cast<identity_kind>(
                current_value >> identity_ref::kind_shift);
            const auto current = identity_ref::make(slot, kind);
            const auto* item = published_record(current);
            if (item == nullptr)
                break;

            ++chain_length;
            ++output.entry_count;
            total_comparisons += chain_length;

            const auto histogram_index =
                chain_length < comparison_histogram.size()
                    ? chain_length
                    : comparison_histogram.size() - 1;
            ++comparison_histogram[histogram_index];

            current_value = item->next_bucket;
        }

        if (chain_length > 1)
            output.collision_entries += chain_length - 1;
        if (chain_length > output.max_chain_length)
            output.max_chain_length = chain_length;
    }

    if (output.occupied_buckets != 0) {
        output.average_chain_length =
            static_cast<double>(output.entry_count) /
            static_cast<double>(output.occupied_buckets);
    }

    if (output.entry_count != 0) {
        output.average_successful_lookup_comparisons =
            static_cast<double>(total_comparisons) /
            static_cast<double>(output.entry_count);
        output.p95_successful_lookup_comparisons =
            percentile_from_histogram(
                comparison_histogram, output.entry_count, 95);
        output.p99_successful_lookup_comparisons =
            percentile_from_histogram(
                comparison_histogram, output.entry_count, 99);
    }

    return output;
}

} // namespace cw::server
