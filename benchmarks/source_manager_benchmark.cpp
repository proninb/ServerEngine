#include "project/source/source_manager.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;
using cw::server::normalize_source_path;
using cw::server::source_id;
using cw::server::source_manager;

volatile std::uint64_t benchmark_sink = 0;

struct measurement final {
    std::size_t count = 0;
    double resolve_unique_ms = 0.0;
    double commit_ms = 0.0;
    double resolve_existing_ms = 0.0;
    double hit_sequential_ms = 0.0;
    double hit_random_ms = 0.0;
    double miss_random_ms = 0.0;
    double old_reference_random_ms = 0.0;
    double old_production_insert_ms = 0.0;
    double old_production_random_ms = 0.0;
    std::size_t path_index_bytes = 0;
    std::size_t path_storage_bytes = 0;
    bool pass = false;
};

[[nodiscard]] std::uint64_t read64(const char* data) noexcept {
    std::uint64_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

[[nodiscard]] std::uint32_t read32(const char* data) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

[[nodiscard]] constexpr std::uint64_t xxh64_round(
    std::uint64_t accumulator,
    std::uint64_t input) noexcept {

    accumulator += input * 14029467366897019727ULL;
    accumulator = std::rotl(accumulator, 31);
    return accumulator * 11400714785074694791ULL;
}

[[nodiscard]] std::uint64_t xxh64(std::string_view value) noexcept {
    constexpr std::uint64_t prime1 = 11400714785074694791ULL;
    constexpr std::uint64_t prime2 = 14029467366897019727ULL;
    constexpr std::uint64_t prime3 = 1609587929392839161ULL;
    constexpr std::uint64_t prime4 = 9650029242287828579ULL;
    constexpr std::uint64_t prime5 = 2870177450012600261ULL;

    const char* position = value.data();
    const char* const end = position + value.size();
    std::uint64_t hash = 0;

    if (value.size() >= 32) {
        std::uint64_t lane1 = prime1 + prime2;
        std::uint64_t lane2 = prime2;
        std::uint64_t lane3 = 0;
        std::uint64_t lane4 = 0 - prime1;
        const char* const limit = end - 32;
        do {
            lane1 = xxh64_round(lane1, read64(position));
            position += 8;
            lane2 = xxh64_round(lane2, read64(position));
            position += 8;
            lane3 = xxh64_round(lane3, read64(position));
            position += 8;
            lane4 = xxh64_round(lane4, read64(position));
            position += 8;
        } while (position <= limit);

        hash =
            std::rotl(lane1, 1) +
            std::rotl(lane2, 7) +
            std::rotl(lane3, 12) +
            std::rotl(lane4, 18);

        const std::uint64_t lanes[] = {lane1, lane2, lane3, lane4};
        for (const auto lane : lanes) {
            hash ^= xxh64_round(0, lane);
            hash = hash * prime1 + prime4;
        }
    }
    else {
        hash = prime5;
    }

    hash += value.size();
    while (position + 8 <= end) {
        const auto lane = xxh64_round(0, read64(position));
        hash ^= lane;
        hash = std::rotl(hash, 27) * prime1 + prime4;
        position += 8;
    }
    if (position + 4 <= end) {
        hash ^= static_cast<std::uint64_t>(read32(position)) * prime1;
        hash = std::rotl(hash, 23) * prime2 + prime3;
        position += 4;
    }
    while (position < end) {
        hash ^= static_cast<unsigned char>(*position++) * prime5;
        hash = std::rotl(hash, 11) * prime1;
    }

    hash ^= hash >> 33;
    hash *= prime2;
    hash ^= hash >> 29;
    hash *= prime3;
    return hash ^ (hash >> 32);
}

[[nodiscard]] constexpr std::uint32_t fingerprint(std::uint64_t hash) noexcept {
    const auto value = static_cast<std::uint32_t>(hash ^ (hash >> 32));
    return value == 0 ? 1U : value;
}

struct reference_bucket final {
    std::uint32_t hash = 0;
    std::uint32_t source = 0;
};

static_assert(sizeof(reference_bucket) == 8);

// Exact reference shape from the Server-Entry_OLD source-manager layout study:
// XXH64 position hash, folded 32-bit fingerprint, 8-byte linear-probing bucket.
class old_reference_index final {
public:
    explicit old_reference_index(std::span<const std::string> paths) {
        const auto required = (paths.size() * 10 + 6) / 7;
        const auto capacity = std::bit_ceil(std::max<std::size_t>(required, 16));
        buckets.resize(capacity);
        mask = capacity - 1;

        for (std::size_t index = 0; index < paths.size(); ++index) {
            const auto hash = xxh64(paths[index]);
            auto position = static_cast<std::size_t>(hash) & mask;
            while (buckets[position].source != 0)
                position = (position + 1) & mask;
            buckets[position] = {
                fingerprint(hash),
                static_cast<std::uint32_t>(index + 1),
            };
        }
    }

    [[nodiscard]] std::uint32_t find(
        std::string_view value,
        std::span<const std::string> paths) const noexcept {

        const auto hash = xxh64(value);
        const auto stored_hash = fingerprint(hash);
        auto position = static_cast<std::size_t>(hash) & mask;

        for (;;) {
            const auto& bucket = buckets[position];
            if (bucket.source == 0)
                return 0;
            if (bucket.hash == stored_hash && paths[bucket.source - 1] == value)
                return bucket.source;
            position = (position + 1) & mask;
        }
    }

private:
    std::vector<reference_bucket> buckets;
    std::size_t mask = 0;
};

[[nodiscard]] std::vector<std::string> make_normalized_paths(std::size_t count) {
    std::string root;
    if (!normalize_source_path("benchmark_sources", root).ok())
        std::abort();

    std::vector<std::string> output;
    output.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        output.push_back(
            root + "/generated/common/prefix/module_" +
            std::to_string(index / 1000) +
            "/source_" + std::to_string(index % 1000) + ".hpp");
    }
    return output;
}

[[nodiscard]] std::vector<std::uint32_t> make_random_order(std::size_t count) {
    std::vector<std::uint32_t> output(count);
    std::iota(output.begin(), output.end(), 0U);
    std::mt19937 generator{0x534D5033U};
    std::shuffle(output.begin(), output.end(), generator);
    return output;
}

template<class Function>
[[nodiscard]] double median_ms(Function&& function, int samples = 5) {
    std::vector<double> values;
    values.reserve(static_cast<std::size_t>(samples));
    for (int sample = 0; sample < samples; ++sample) {
        const auto begin = clock_type::now();
        benchmark_sink ^= function();
        const auto end = clock_type::now();
        values.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
    }
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

[[nodiscard]] measurement run(std::size_t count) {
    const auto paths = make_normalized_paths(count);
    const auto queries = paths;
    const auto random_order = make_random_order(count);

    source_manager manager;
    double resolve_unique_ms = 0.0;
    double commit_ms = 0.0;

    {
        auto update = manager.begin_update();
        const auto begin = clock_type::now();
        for (std::size_t index = 0; index < count; ++index) {
            source_id source;
            const auto result = update.resolve_normalized(paths[index], source);
            if (!result.ok() || source.value() != index + 1)
                return measurement{count};
        }
        const auto resolved = clock_type::now();
        const auto commit_result = update.commit();
        const auto committed = clock_type::now();
        if (!commit_result.ok())
            return measurement{count};

        resolve_unique_ms = std::chrono::duration<double, std::milli>(resolved - begin).count();
        commit_ms = std::chrono::duration<double, std::milli>(committed - resolved).count();
    }

    if (manager.source_count() != count)
        return measurement{count};

    double resolve_existing_ms = 0.0;
    {
        auto update = manager.begin_update();
        const auto begin = clock_type::now();
        for (std::size_t index = 0; index < count; ++index) {
            source_id source;
            const auto result = update.resolve_normalized(paths[index], source);
            if (!result.ok() || source.value() != index + 1)
                return measurement{count};
        }
        const auto end = clock_type::now();
        resolve_existing_ms = std::chrono::duration<double, std::milli>(end - begin).count();
    }

    const auto hit_sequential_ms = median_ms([&]() noexcept {
        std::uint64_t total = 0;
        for (std::size_t index = 0; index < count; ++index) {
            source_id source;
            total += manager.find(queries[index], source).ok() ? source.value() : 0U;
        }
        return total;
    });

    const auto hit_random_ms = median_ms([&]() noexcept {
        std::uint64_t total = 0;
        for (const auto index : random_order) {
            source_id source;
            total += manager.find(queries[index], source).ok() ? source.value() : 0U;
        }
        return total;
    });

    double miss_random_ms = 0.0;
    {
        std::vector<std::string> misses;
        misses.reserve(count);
        for (const auto& path : queries)
            misses.push_back(path + ".missing");

        miss_random_ms = median_ms([&]() noexcept {
            std::uint64_t total = 0;
            for (const auto index : random_order) {
                source_id source;
                total += manager.find(misses[index], source).ok() ? source.value() : 1U;
            }
            return total;
        });
    }

    double old_reference_random_ms = 0.0;
    {
        const old_reference_index old_reference{paths};
        old_reference_random_ms = median_ms([&]() noexcept {
            std::uint64_t total = 0;
            for (const auto index : random_order)
                total += old_reference.find(queries[index], paths);
            return total;
        });
    }

    double old_production_insert_ms = 0.0;
    double old_production_random_ms = 0.0;
    {
        std::vector<std::filesystem::path> old_stored_paths;
        old_stored_paths.reserve(count);
        for (const auto& path : paths)
            old_stored_paths.emplace_back(path);

        std::unordered_map<std::filesystem::path, std::uint32_t> old_by_path;
        old_by_path.reserve(count);
        const auto insert_begin = clock_type::now();
        for (std::size_t index = 0; index < count; ++index)
            old_by_path.emplace(old_stored_paths[index], static_cast<std::uint32_t>(index + 1));
        const auto insert_end = clock_type::now();
        old_production_insert_ms =
            std::chrono::duration<double, std::milli>(insert_end - insert_begin).count();

        std::vector<std::filesystem::path> old_queries;
        old_queries.reserve(count);
        for (const auto& query : queries)
            old_queries.emplace_back(query);

        old_production_random_ms = median_ms([&]() noexcept {
            std::uint64_t total = 0;
            for (const auto index : random_order) {
                const auto found = old_by_path.find(old_queries[index]);
                if (found != old_by_path.end())
                    total += found->second;
            }
            return total;
        });
    }

    return {
        count,
        resolve_unique_ms,
        commit_ms,
        resolve_existing_ms,
        hit_sequential_ms,
        hit_random_ms,
        miss_random_ms,
        old_reference_random_ms,
        old_production_insert_ms,
        old_production_random_ms,
        manager.path_index_bytes(),
        manager.path_storage_bytes(),
        true,
    };
}

void print(const measurement& value) {
    const auto operations = static_cast<double>(value.count);
    const auto ns = [operations](double milliseconds) {
        return milliseconds * 1'000'000.0 / operations;
    };

    std::cout << value.count << ','
              << std::fixed << std::setprecision(6)
              << value.resolve_unique_ms << ','
              << value.commit_ms << ','
              << value.resolve_existing_ms << ','
              << ns(value.resolve_unique_ms) << ','
              << ns(value.resolve_existing_ms) << ','
              << ns(value.hit_sequential_ms) << ','
              << ns(value.hit_random_ms) << ','
              << ns(value.miss_random_ms) << ','
              << ns(value.old_reference_random_ms) << ','
              << ns(value.old_production_insert_ms) << ','
              << ns(value.old_production_random_ms) << ','
              << value.path_index_bytes << ','
              << value.path_storage_bytes << ','
              << (value.pass ? "PASS" : "FAIL") << '\n';
}

} // namespace

int main(int argc, char** argv) {
    const bool quick = argc == 2 && std::string_view{argv[1]} == "--quick";
    if (argc > 2 || (argc == 2 && !quick))
        return 2;

    std::cout
        << "count,resolve_unique_normalized_ms,commit_ms,resolve_existing_normalized_ms,"
        << "resolve_unique_normalized_ns,resolve_existing_normalized_ns,"
        << "find_hit_sequential_ns,find_hit_random_ns,find_miss_random_ns,"
        << "old_xxh32_bucket8_hit_random_ns,old_production_unordered_path_insert_ns,"
        << "old_production_unordered_path_hit_random_ns,"
        << "path_index_bytes,path_storage_bytes,status\n";

    const auto small = run(quick ? 10'000 : 100'000);
    const auto large = run(quick ? 100'000 : 1'000'000);
    print(small);
    print(large);
    if (!small.pass || !large.pass)
        return 1;

    const auto source_ratio = static_cast<double>(large.count) / static_cast<double>(small.count);
    const auto construction_exponent =
        std::log(large.resolve_unique_ms / small.resolve_unique_ms) / std::log(source_ratio);
    const auto hot_exponent =
        std::log(large.hit_random_ms / small.hit_random_ms) / std::log(source_ratio);
    const auto old_ns = large.old_reference_random_ms * 1'000'000.0 / static_cast<double>(large.count);
    const auto v3_ns = large.hit_random_ms * 1'000'000.0 / static_cast<double>(large.count);
    const auto ratio = old_ns > 0.0 ? v3_ns / old_ns : 0.0;
    const auto old_production_insert_ns =
        large.old_production_insert_ms * 1'000'000.0 / static_cast<double>(large.count);
    const auto old_production_ns =
        large.old_production_random_ms * 1'000'000.0 / static_cast<double>(large.count);
    const auto production_construction_ratio = old_production_insert_ns > 0.0
        ? (large.resolve_unique_ms * 1'000'000.0 / static_cast<double>(large.count)) / old_production_insert_ns
        : 0.0;
    const auto production_ratio = old_production_ns > 0.0 ? v3_ns / old_production_ns : 0.0;

    std::cout << "CONSTRUCTION_SCALING_EXPONENT," << std::fixed << std::setprecision(3)
              << construction_exponent << '\n';
    std::cout << "HOT_LOOKUP_SCALING_EXPONENT," << hot_exponent << '\n';
    std::cout << "V3_TO_OLD_LAYOUT_CANDIDATE_RATIO," << ratio << '\n';
    std::cout << "V3_TO_OLD_PRODUCTION_INSERT_RATIO," << production_construction_ratio << '\n';
    std::cout << "V3_TO_OLD_PRODUCTION_LOOKUP_RATIO," << production_ratio << '\n';

    bool gate = true;
    if (construction_exponent > 1.50) {
        std::cout << "SOURCE_MANAGER_CONSTRUCTION_GATE,FAIL,exponent "
                  << construction_exponent << " > 1.50\n";
        gate = false;
    }
    else {
        std::cout << "SOURCE_MANAGER_CONSTRUCTION_GATE,PASS,exponent "
                  << construction_exponent << " <= 1.50\n";
    }

    // Power-of-two capacity at <= 0.5 load is always below 4 * count buckets.
    // With an 8-byte bucket this bounds index memory below 32 bytes per Source.
    if (large.path_index_bytes > large.count * 32ULL) {
        std::cout << "SOURCE_MANAGER_INDEX_MEMORY_GATE,FAIL,index exceeds compact 8-byte bucket budget\n";
        gate = false;
    }
    else {
        std::cout << "SOURCE_MANAGER_INDEX_MEMORY_GATE,PASS,compact 8-byte bucket index\n";
    }

    // OLD production used unordered_map<filesystem::path, source_id>. V3 must
    // beat that actual repository implementation for both insertion and lookup.
    if (production_construction_ratio >= 1.0 || production_ratio >= 1.0) {
        std::cout << "SOURCE_MANAGER_OLD_PRODUCTION_GATE,FAIL,insert="
                  << production_construction_ratio << ",lookup=" << production_ratio
                  << ",expected both < 1.0\n";
        gate = false;
    }
    else {
        std::cout << "SOURCE_MANAGER_OLD_PRODUCTION_GATE,PASS,insert="
                  << production_construction_ratio << ",lookup=" << production_ratio
                  << ",both < 1.0\n";
    }

    // The separate OLD layout benchmark contained a better 8-byte XXH candidate
    // that was not integrated into Source Manager. Near-parity is acceptable here;
    // this gate prevents the V3 production index from losing its design advantage.
    if (ratio > 1.20) {
        std::cout << "SOURCE_MANAGER_OLD_LAYOUT_GATE,FAIL,V3/OLD-layout "
                  << ratio << " > 1.20\n";
        gate = false;
    }
    else {
        std::cout << "SOURCE_MANAGER_OLD_LAYOUT_GATE,PASS,V3/OLD-layout "
                  << ratio << " <= 1.20\n";
    }

    std::cout << "SOURCE_MANAGER_PERFORMANCE_GATE," << (gate ? "PASS" : "FAIL") << '\n';
    return gate ? 0 : 1;
}
