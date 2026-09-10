#pragma once

#include "identity_arena.hpp"
#include "identity_node.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace cw::server {

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

// Project-lifetime semantic identity tree and its single source-language scope index.
// The index is an acceleration structure for resolve_declaration(); it is not a
// second identity registry and is never used by Generation Builder.
class identity_space final {
public:
    identity_space() noexcept;

    identity_space(const identity_space&) = delete;
    identity_space& operator=(const identity_space&) = delete;

    [[nodiscard]] identity_ref root() const noexcept { return &root_record.identity; }

    [[nodiscard]] status resolve_declaration(
        identity_ref parent,
        std::string_view local_name,
        identity_kind kind,
        identity_ref& output) noexcept;

    [[nodiscard]] std::size_t size() const noexcept {
        return identity_count.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t bytes_reserved() const noexcept {
        return storage.bytes_reserved() + semantic_bucket_bytes;
    }

    [[nodiscard]] std::size_t pages_reserved() const noexcept {
        return storage.pages_reserved();
    }

    [[nodiscard]] constexpr std::size_t bucket_count() const noexcept {
        return semantic_bucket_count;
    }

    // Diagnostic/benchmark snapshot of the immutable bucket chains. It performs
    // no sorting and does not participate in semantic resolution.
    [[nodiscard]] identity_index_statistics index_statistics() const noexcept;

private:
    struct record {
        identity_node identity;
        std::uint64_t semantic_hash = 0;
        record* next_bucket = nullptr;

        record(
            identity_node::construction_token token,
            identity_ref parent,
            name_ref name,
            identity_kind kind,
            std::uint64_t hash) noexcept
            : identity(token, parent, name, kind), semantic_hash(hash) {}
    };

    static constexpr std::size_t semantic_bucket_count = std::size_t{1} << 20;
    static constexpr std::size_t semantic_bucket_mask = semantic_bucket_count - 1;
    static constexpr std::size_t semantic_bucket_bytes =
        semantic_bucket_count * sizeof(std::atomic<record*>);

    [[nodiscard]] static std::uint64_t semantic_hash(
        identity_ref parent,
        std::string_view local_name) noexcept;

    [[nodiscard]] const record* find(
        identity_ref parent,
        std::string_view local_name,
        std::uint64_t hash) const noexcept;

    [[nodiscard]] status make_candidate(
        identity_ref parent,
        std::string_view local_name,
        identity_kind kind,
        std::uint64_t hash,
        record*& output) noexcept;

    record root_record;
    identity_arena storage;
    std::unique_ptr<std::atomic<record*>[]> buckets;
    std::atomic<std::size_t> identity_count{1};
};

} // namespace cw::server
