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

identity_space::identity_space() noexcept
    : root_record(
          identity_node::construction_token{},
          nullptr,
          string_id{},
          identity_kind::root,
          0),
      buckets(new (std::nothrow) std::atomic<record*>[semantic_bucket_count]) {

    if (buckets == nullptr)
        return;

    for (std::size_t index = 0; index < semantic_bucket_count; ++index)
        buckets[index].store(nullptr, std::memory_order_relaxed);
}

std::uint64_t identity_space::semantic_hash(
    identity_ref parent,
    string_id local_name) noexcept {

    const auto parent_value = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(parent));
    return mix64(parent_value ^ (static_cast<std::uint64_t>(local_name.value()) << 32));
}

const identity_space::record* identity_space::find_record(
    identity_ref parent,
    string_id local_name,
    std::uint64_t hash) const noexcept {

    if (buckets == nullptr)
        return nullptr;

    auto* item = buckets[static_cast<std::size_t>(hash) & semantic_bucket_mask].load(
        std::memory_order_acquire);
    while (item != nullptr) {
        if (item->semantic_hash == hash &&
            item->identity.parent() == parent &&
            item->identity.name() == local_name) {
            return item;
        }
        item = item->next_bucket;
    }
    return nullptr;
}

status identity_space::make_candidate(
    identity_ref parent,
    string_id local_name,
    identity_kind kind,
    std::uint64_t hash,
    record*& output) noexcept {

    output = nullptr;
    void* memory = nullptr;
    const auto result = storage.allocate(sizeof(record), alignof(record), memory);
    if (!result.ok())
        return result;

    output = ::new (memory) record(
        identity_node::construction_token{},
        parent,
        local_name,
        kind,
        hash);
    return {};
}

status identity_space::resolve_declaration(
    identity_ref parent,
    string_id local_name,
    identity_kind kind,
    identity_ref& output) noexcept {

    output = nullptr;

    if (parent == nullptr || !local_name || kind == identity_kind::root)
        return {status_code::invalid_argument};

    if (buckets == nullptr)
        return {status_code::not_available};

    const auto hash = semantic_hash(parent, local_name);
    auto& bucket = buckets[static_cast<std::size_t>(hash) & semantic_bucket_mask];

    if (const auto* existing = find_record(parent, local_name, hash); existing != nullptr) {
        if (existing->identity.kind() != kind)
            return {status_code::semantic_conflict};
        output = &existing->identity;
        return {};
    }

    record* candidate = nullptr;
    const auto result = make_candidate(parent, local_name, kind, hash, candidate);
    if (!result.ok())
        return result;

    auto* head = bucket.load(std::memory_order_acquire);
    for (;;) {
        auto* item = head;
        while (item != nullptr) {
            if (item->semantic_hash == hash &&
                item->identity.parent() == parent &&
                item->identity.name() == local_name) {
                if (item->identity.kind() != kind)
                    return {status_code::semantic_conflict};
                output = &item->identity;
                return {};
            }
            item = item->next_bucket;
        }

        candidate->next_bucket = head;
        if (bucket.compare_exchange_weak(
                head,
                candidate,
                std::memory_order_release,
                std::memory_order_acquire)) {
            identity_count.fetch_add(1, std::memory_order_relaxed);
            output = &candidate->identity;
            return {};
        }
    }
}

identity_ref identity_space::find(
    identity_ref parent,
    string_id local_name,
    identity_kind kind) const noexcept {

    if (parent == nullptr || !local_name || kind == identity_kind::root)
        return nullptr;
    const auto hash = semantic_hash(parent, local_name);
    const auto* found = find_record(parent, local_name, hash);
    return found != nullptr && found->identity.kind() == kind
        ? &found->identity
        : nullptr;
}

identity_index_statistics identity_space::index_statistics() const noexcept {
    identity_index_statistics output;
    if (buckets == nullptr)
        return output;

    std::array<std::size_t, 256> comparison_histogram{};
    std::uint64_t total_comparisons = 0;

    for (std::size_t bucket_index = 0; bucket_index < semantic_bucket_count; ++bucket_index) {
        const auto* item = buckets[bucket_index].load(std::memory_order_acquire);
        if (item == nullptr)
            continue;

        ++output.occupied_buckets;
        std::size_t chain_length = 0;
        while (item != nullptr) {
            ++chain_length;
            ++output.entry_count;
            total_comparisons += chain_length;
            const auto histogram_index = chain_length < comparison_histogram.size()
                ? chain_length
                : comparison_histogram.size() - 1;
            ++comparison_histogram[histogram_index];
            item = item->next_bucket;
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
            percentile_from_histogram(comparison_histogram, output.entry_count, 95);
        output.p99_successful_lookup_comparisons =
            percentile_from_histogram(comparison_histogram, output.entry_count, 99);
    }

    return output;
}

} // namespace cw::server
