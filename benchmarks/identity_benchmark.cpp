#include "../server_engine/project/project_context.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace cw::server;
using clock_type = std::chrono::steady_clock;

enum class scenario {
    create_unique,
    hit_existing,
    mixed_50,
};

enum class name_pattern {
    sequential,
    patterned,
    same_prefix,
    long_name,
};

struct benchmark_result {
    std::size_t count = 0;
    scenario scenario_value = scenario::create_unique;
    std::size_t threads = 1;
    double total_ms = 0.0;
    double ns_per_op = 0.0;
    double mops = 0.0;
    std::size_t identity_count = 0;
    std::size_t expected_identity_count = 0;
    std::size_t reserved_bytes = 0;
    std::size_t reserved_pages = 0;
    std::size_t semantic_buckets = 0;
    double bucket_load = 0.0;
    identity_index_statistics index_stats;
    bool passed = false;
};

struct hardening_result {
    std::size_t count = 0;
    name_pattern pattern = name_pattern::sequential;
    double create_ms = 0.0;
    double hit_ms = 0.0;
    std::size_t identity_count = 0;
    std::size_t reserved_bytes = 0;
    identity_index_statistics index_stats;
    bool canonical_pointer_pass = false;
    bool structural_pass = false;
};

[[nodiscard]] constexpr std::string_view scenario_name(scenario value) noexcept {
    switch (value) {
    case scenario::create_unique:
        return "create_unique";
    case scenario::hit_existing:
        return "hit_existing";
    case scenario::mixed_50:
        return "mixed_50";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view pattern_name(name_pattern value) noexcept {
    switch (value) {
    case name_pattern::sequential:
        return "sequential";
    case name_pattern::patterned:
        return "patterned";
    case name_pattern::same_prefix:
        return "same_prefix";
    case name_pattern::long_name:
        return "long_name";
    }
    return "unknown";
}

[[nodiscard]] bool parse_size(std::string_view text, std::size_t& output) noexcept {
    output = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, output);
    return result.ec == std::errc{} && result.ptr == end && output != 0;
}

[[nodiscard]] std::string make_name(std::size_t index, name_pattern pattern) {
    char suffix[48]{};

    switch (pattern) {
    case name_pattern::sequential: {
        const auto written = std::snprintf(suffix, sizeof(suffix), "Type_%010zu", index);
        if (written <= 0)
            return {};
        return std::string{suffix, static_cast<std::size_t>(written)};
    }
    case name_pattern::patterned: {
        const auto lane = (index * std::size_t{2654435761u}) & std::size_t{0xffffu};
        const auto written = std::snprintf(
            suffix,
            sizeof(suffix),
            "T_%04zu_%04zu_%010zu",
            index % 97u,
            lane,
            index);
        if (written <= 0)
            return {};
        return std::string{suffix, static_cast<std::size_t>(written)};
    }
    case name_pattern::same_prefix: {
        constexpr std::string_view prefix =
            "VeryLongSharedProjectNamespace_Component_Controller_Channel_";
        const auto written = std::snprintf(suffix, sizeof(suffix), "%010zu", index);
        if (written <= 0)
            return {};
        std::string result;
        result.reserve(prefix.size() + static_cast<std::size_t>(written));
        result.append(prefix);
        result.append(suffix, static_cast<std::size_t>(written));
        return result;
    }
    case name_pattern::long_name: {
        constexpr std::size_t body_size = 112;
        std::string result(body_size, 'A');
        auto value = static_cast<std::uint64_t>(index) + 0x9e3779b97f4a7c15ULL;
        for (std::size_t position = 0; position < body_size; ++position) {
            value ^= value >> 12;
            value ^= value << 25;
            value ^= value >> 27;
            result[position] = static_cast<char>('A' + (value % 26u));
        }
        const auto written = std::snprintf(suffix, sizeof(suffix), "_%010zu", index);
        if (written <= 0)
            return {};
        result.append(suffix, static_cast<std::size_t>(written));
        return result;
    }
    }
    return {};
}

[[nodiscard]] std::vector<std::string> make_names(
    std::size_t count,
    name_pattern pattern = name_pattern::sequential) {

    std::vector<std::string> names;
    names.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
        names.push_back(make_name(index, pattern));
    return names;
}

[[nodiscard]] bool resolve_range(
    project_context& context,
    const std::vector<std::string>& names,
    std::size_t begin,
    std::size_t end,
    std::atomic<bool>& failed) noexcept {

    const auto root = context.identity_root();
    for (std::size_t index = begin; index < end; ++index) {
        identity_ref identity = nullptr;
        if (!context.resolve_declaration(root, names[index], identity_kind::type, identity).ok() ||
            identity == nullptr) {
            failed.store(true, std::memory_order_relaxed);
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool prepare(
    project_context& context,
    const std::vector<std::string>& names,
    scenario scenario_value) noexcept {

    if (scenario_value == scenario::create_unique)
        return true;

    const auto root = context.identity_root();
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (scenario_value == scenario::mixed_50 && (index & 1u) != 0u)
            continue;

        identity_ref identity = nullptr;
        if (!context.resolve_declaration(root, names[index], identity_kind::type, identity).ok() ||
            identity == nullptr)
            return false;
    }
    return true;
}

[[nodiscard]] benchmark_result run_benchmark(
    std::size_t count,
    scenario scenario_value,
    std::size_t thread_count) {

    benchmark_result output;
    output.count = count;
    output.scenario_value = scenario_value;
    output.threads = thread_count;
    output.expected_identity_count = count + 1;

    auto names = make_names(count);
    if (names.size() != count)
        return output;

    project_configuration configuration;
    project_context context{std::move(configuration)};

    if (!prepare(context, names, scenario_value))
        return output;

    std::atomic<bool> failed{false};
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    const auto block = (count + thread_count - 1) / thread_count;
    for (std::size_t worker = 0; worker < thread_count; ++worker) {
        const auto begin = std::min(worker * block, count);
        const auto end = std::min(begin + block, count);
        workers.emplace_back([&, begin, end] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();

            static_cast<void>(resolve_range(context, names, begin, end, failed));
        });
    }

    while (ready.load(std::memory_order_acquire) != thread_count)
        std::this_thread::yield();

    const auto begin_time = clock_type::now();
    start.store(true, std::memory_order_release);

    for (auto& worker : workers)
        worker.join();

    const auto end_time = clock_type::now();
    output.total_ms = std::chrono::duration<double, std::milli>(end_time - begin_time).count();
    output.ns_per_op = output.total_ms * 1'000'000.0 / static_cast<double>(count);
    output.mops = output.total_ms == 0.0
        ? 0.0
        : static_cast<double>(count) / (output.total_ms * 1000.0);
    output.identity_count = context.identity_count();
    output.reserved_bytes = context.identity_bytes_reserved();
    output.reserved_pages = context.identity_pages_reserved();
    output.semantic_buckets = context.identity_bucket_count();
    output.bucket_load = output.semantic_buckets == 0
        ? 0.0
        : static_cast<double>(output.identity_count - 1) /
              static_cast<double>(output.semantic_buckets);
    output.index_stats = context.identity_index_stats();
    output.passed = !failed.load(std::memory_order_relaxed) &&
                    output.identity_count == output.expected_identity_count &&
                    output.index_stats.entry_count == count;
    return output;
}

void print_header() {
    std::puts(
        "count,scenario,threads,total_ms,ns_per_op,mops,identity_count,expected_identity_count,"
        "reserved_bytes,reserved_pages,semantic_buckets,bucket_load,occupied_buckets,"
        "collision_entries,collision_rate,max_chain,avg_chain,avg_success_comparisons,"
        "p95_success_comparisons,p99_success_comparisons,status");
}

void print_result(const benchmark_result& result) {
    const auto collision_rate = result.index_stats.entry_count == 0
        ? 0.0
        : static_cast<double>(result.index_stats.collision_entries) /
              static_cast<double>(result.index_stats.entry_count);

    std::printf(
        "%zu,%.*s,%zu,%.6f,%.3f,%.6f,%zu,%zu,%zu,%zu,%zu,%.6f,%zu,%zu,%.6f,%zu,%.6f,%.6f,%zu,%zu,%s\n",
        result.count,
        static_cast<int>(scenario_name(result.scenario_value).size()),
        scenario_name(result.scenario_value).data(),
        result.threads,
        result.total_ms,
        result.ns_per_op,
        result.mops,
        result.identity_count,
        result.expected_identity_count,
        result.reserved_bytes,
        result.reserved_pages,
        result.semantic_buckets,
        result.bucket_load,
        result.index_stats.occupied_buckets,
        result.index_stats.collision_entries,
        collision_rate,
        result.index_stats.max_chain_length,
        result.index_stats.average_chain_length,
        result.index_stats.average_successful_lookup_comparisons,
        result.index_stats.p95_successful_lookup_comparisons,
        result.index_stats.p99_successful_lookup_comparisons,
        result.passed ? "PASS" : "FAIL");
}

[[nodiscard]] bool run_scaling_gate() {
    constexpr std::size_t small_count = 100000;
    constexpr std::size_t large_count = 1000000;
    constexpr double maximum_exponent = 1.50;
    constexpr double maximum_memory_amplification = 1.10;

    const auto small = run_benchmark(small_count, scenario::create_unique, 1);
    const auto large = run_benchmark(large_count, scenario::create_unique, 1);
    const auto parallel = run_benchmark(large_count, scenario::create_unique, 16);

    print_header();
    print_result(small);
    print_result(large);
    print_result(parallel);

    if (!small.passed || !large.passed || !parallel.passed ||
        small.total_ms <= 0.0 || large.total_ms <= 0.0 ||
        large.reserved_bytes == 0) {
        std::puts("SCALING_GATE,FAIL,benchmark correctness failure");
        return false;
    }

    const auto size_ratio = static_cast<double>(large_count) / static_cast<double>(small_count);
    const auto time_ratio = large.total_ms / small.total_ms;
    const auto exponent = std::log(time_ratio) / std::log(size_ratio);
    const auto memory_amplification =
        static_cast<double>(parallel.reserved_bytes) / static_cast<double>(large.reserved_bytes);

    bool passed = true;

    std::printf("SCALING_EXPONENT,%.3f\n", exponent);
    if (exponent > maximum_exponent) {
        std::printf(
            "SCALING_GATE,FAIL,superlinear identity resolution; exponent %.3f > %.2f\n",
            exponent,
            maximum_exponent);
        passed = false;
    } else {
        std::printf("SCALING_GATE,PASS,exponent %.3f <= %.2f\n", exponent, maximum_exponent);
    }

    std::printf("MEMORY_AMPLIFICATION_16T,%.3f\n", memory_amplification);
    if (memory_amplification > maximum_memory_amplification) {
        std::printf(
            "MEMORY_GATE,FAIL,16-thread reserved-byte amplification %.3f > %.2f\n",
            memory_amplification,
            maximum_memory_amplification);
        passed = false;
    } else {
        std::printf(
            "MEMORY_GATE,PASS,16-thread reserved-byte amplification %.3f <= %.2f\n",
            memory_amplification,
            maximum_memory_amplification);
    }

    return passed;
}

[[nodiscard]] hardening_result run_hardening_workload(
    std::size_t count,
    name_pattern pattern) {

    hardening_result output;
    output.count = count;
    output.pattern = pattern;

    auto names = make_names(count, pattern);
    if (names.size() != count)
        return output;

    std::vector<identity_ref> first_resolution(count, nullptr);
    project_configuration configuration;
    project_context context{std::move(configuration)};
    const auto root = context.identity_root();

    const auto create_begin = clock_type::now();
    for (std::size_t index = 0; index < count; ++index) {
        if (!context.resolve_declaration(
                root,
                names[index],
                identity_kind::type,
                first_resolution[index]).ok() ||
            first_resolution[index] == nullptr) {
            return output;
        }
    }
    const auto create_end = clock_type::now();

    bool canonical = true;
    const auto hit_begin = clock_type::now();
    for (std::size_t index = 0; index < count; ++index) {
        identity_ref identity = nullptr;
        if (!context.resolve_declaration(root, names[index], identity_kind::type, identity).ok() ||
            identity != first_resolution[index]) {
            canonical = false;
            break;
        }
    }
    const auto hit_end = clock_type::now();

    output.create_ms =
        std::chrono::duration<double, std::milli>(create_end - create_begin).count();
    output.hit_ms = std::chrono::duration<double, std::milli>(hit_end - hit_begin).count();
    output.identity_count = context.identity_count();
    output.reserved_bytes = context.identity_bytes_reserved();
    output.index_stats = context.identity_index_stats();
    output.canonical_pointer_pass = canonical && output.identity_count == count + 1 &&
                                    output.index_stats.entry_count == count;

    constexpr double maximum_collision_rate = 0.45;
    constexpr std::size_t maximum_chain_length = 16;
    constexpr double maximum_average_successful_comparisons = 2.00;
    constexpr std::size_t maximum_p95_successful_comparisons = 4;
    constexpr std::size_t maximum_p99_successful_comparisons = 6;

    const auto collision_rate = output.index_stats.entry_count == 0
        ? 0.0
        : static_cast<double>(output.index_stats.collision_entries) /
              static_cast<double>(output.index_stats.entry_count);

    output.structural_pass =
        collision_rate <= maximum_collision_rate &&
        output.index_stats.max_chain_length <= maximum_chain_length &&
        output.index_stats.average_successful_lookup_comparisons <=
            maximum_average_successful_comparisons &&
        output.index_stats.p95_successful_lookup_comparisons <=
            maximum_p95_successful_comparisons &&
        output.index_stats.p99_successful_lookup_comparisons <=
            maximum_p99_successful_comparisons;
    return output;
}

void print_hardening_header() {
    std::puts(
        "count,pattern,create_ms,hit_ms,identity_count,reserved_bytes,occupied_buckets,"
        "collision_entries,collision_rate,max_chain,avg_chain,avg_success_comparisons,"
        "p95_success_comparisons,p99_success_comparisons,canonical_pointer,structural_gate,status");
}

void print_hardening_result(const hardening_result& result) {
    const auto collision_rate = result.index_stats.entry_count == 0
        ? 0.0
        : static_cast<double>(result.index_stats.collision_entries) /
              static_cast<double>(result.index_stats.entry_count);
    const auto passed = result.canonical_pointer_pass && result.structural_pass;

    std::printf(
        "%zu,%.*s,%.6f,%.6f,%zu,%zu,%zu,%zu,%.6f,%zu,%.6f,%.6f,%zu,%zu,%s,%s,%s\n",
        result.count,
        static_cast<int>(pattern_name(result.pattern).size()),
        pattern_name(result.pattern).data(),
        result.create_ms,
        result.hit_ms,
        result.identity_count,
        result.reserved_bytes,
        result.index_stats.occupied_buckets,
        result.index_stats.collision_entries,
        collision_rate,
        result.index_stats.max_chain_length,
        result.index_stats.average_chain_length,
        result.index_stats.average_successful_lookup_comparisons,
        result.index_stats.p95_successful_lookup_comparisons,
        result.index_stats.p99_successful_lookup_comparisons,
        result.canonical_pointer_pass ? "PASS" : "FAIL",
        result.structural_pass ? "PASS" : "FAIL",
        passed ? "PASS" : "FAIL");
}

[[nodiscard]] bool run_hardening_gate() {
    constexpr std::size_t count = 1000000;
    constexpr name_pattern patterns[]{
        name_pattern::sequential,
        name_pattern::patterned,
        name_pattern::same_prefix,
        name_pattern::long_name,
    };

    print_hardening_header();
    bool passed = true;
    for (const auto pattern : patterns) {
        const auto result = run_hardening_workload(count, pattern);
        print_hardening_result(result);
        passed = result.canonical_pointer_pass && result.structural_pass && passed;
    }

    if (passed) {
        std::puts(
            "IDENTITY_INDEX_HARDENING_GATE,PASS,1M canonical pointer and collision structure "
            "passed for all identifier patterns");
    } else {
        std::puts(
            "IDENTITY_INDEX_HARDENING_GATE,FAIL,canonical pointer or collision structure "
            "failed for at least one identifier pattern");
    }
    return passed;
}

[[nodiscard]] int run_full_matrix(bool force) {
    if (!force && !run_scaling_gate())
        return 3;

    constexpr std::size_t counts[]{10000, 100000, 1000000};
    constexpr std::size_t threads[]{1, 2, 4, 8, 16};
    constexpr scenario scenarios[]{
        scenario::create_unique,
        scenario::hit_existing,
        scenario::mixed_50,
    };

    print_header();
    bool passed = true;
    for (const auto count : counts) {
        for (const auto scenario_value : scenarios) {
            for (const auto thread_count : threads) {
                const auto result = run_benchmark(count, scenario_value, thread_count);
                print_result(result);
                passed = result.passed && passed;
            }
        }
    }
    return passed ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--gate")
        return run_scaling_gate() ? 0 : 3;

    if (argc == 2 && std::string_view{argv[1]} == "--hardening")
        return run_hardening_gate() ? 0 : 4;

    if (argc == 2 && std::string_view{argv[1]} == "--full")
        return run_full_matrix(false);

    if (argc == 2 && std::string_view{argv[1]} == "--force-full")
        return run_full_matrix(true);

    if (argc == 4 && std::string_view{argv[1]} == "--single") {
        std::size_t count = 0;
        std::size_t threads = 0;
        if (!parse_size(argv[2], count) || !parse_size(argv[3], threads))
            return 2;
        print_header();
        const auto result = run_benchmark(count, scenario::create_unique, threads);
        print_result(result);
        return result.passed ? 0 : 1;
    }

    std::fputs(
        "usage:\n"
        "  server_engine_identity_benchmark --gate\n"
        "  server_engine_identity_benchmark --hardening\n"
        "  server_engine_identity_benchmark --full\n"
        "  server_engine_identity_benchmark --force-full\n"
        "  server_engine_identity_benchmark --single <count> <threads>\n",
        stderr);
    return 2;
}
