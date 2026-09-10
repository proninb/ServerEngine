#include "../server_engine/project/string/string_table.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace cw::server;
using clock_type = std::chrono::steady_clock;

struct result final {
    std::size_t count = 0;
    double intern_ms = 0.0;
    double hit_ms = 0.0;
    double direct_get_ms = 0.0;
    std::size_t bytes_reserved = 0;
    std::size_t occupied_buckets = 0;
    std::size_t collision_entries = 0;
    std::size_t max_chain = 0;
    bool pass = false;
};

[[nodiscard]] std::vector<std::string> make_names(std::size_t count) {
    std::vector<std::string> names;
    names.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
        names.push_back("String_" + std::to_string(index));
    return names;
}

[[nodiscard]] result run(std::size_t count) {
    result output;
    output.count = count;
    const auto names = make_names(count);
    std::vector<string_id> ids(count);
    string_table table;

    const auto intern_begin = clock_type::now();
    for (std::size_t index = 0; index < count; ++index) {
        if (!table.intern(names[index], ids[index]).ok() || !ids[index])
            return output;
    }
    const auto intern_end = clock_type::now();

    const auto hit_begin = clock_type::now();
    for (std::size_t index = count; index != 0; --index) {
        if (table.find(names[index - 1]) != ids[index - 1])
            return output;
    }
    const auto hit_end = clock_type::now();

    std::size_t observed_bytes = 0;
    const auto get_begin = clock_type::now();
    for (std::size_t index = 0; index < count; ++index) {
        const auto value = table.get(ids[index]);
        if (value != names[index])
            return output;
        observed_bytes += value.size();
    }
    const auto get_end = clock_type::now();
    if (observed_bytes == 0 && count != 0)
        return output;

    const auto statistics = table.statistics();
    output.intern_ms = std::chrono::duration<double, std::milli>(intern_end - intern_begin).count();
    output.hit_ms = std::chrono::duration<double, std::milli>(hit_end - hit_begin).count();
    output.direct_get_ms = std::chrono::duration<double, std::milli>(get_end - get_begin).count();
    output.bytes_reserved = statistics.bytes_reserved;
    output.occupied_buckets = statistics.occupied_buckets;
    output.collision_entries = statistics.collision_entries;
    output.max_chain = statistics.max_chain_length;
    output.pass = table.size() == count && statistics.strings == count && statistics.max_chain_length <= 32;
    return output;
}

void print(const result& value) {
    const auto divisor = value.count == 0 ? 1.0 : static_cast<double>(value.count);
    std::cout << value.count << ',' << value.intern_ms << ',' << value.hit_ms << ','
              << value.direct_get_ms << ',' << value.intern_ms * 1000000.0 / divisor << ','
              << value.hit_ms * 1000000.0 / divisor << ','
              << value.direct_get_ms * 1000000.0 / divisor << ',' << value.bytes_reserved << ','
              << (value.count == 0 ? 0.0 : static_cast<double>(value.bytes_reserved) / static_cast<double>(value.count)) << ','
              << value.occupied_buckets << ',' << value.collision_entries << ','
              << value.max_chain << ',' << (value.pass ? "PASS" : "FAIL") << '\n';
}

} // namespace

int main(int argc, char** argv) {
    const bool gate = argc == 2 && std::string_view{argv[1]} == "--gate";
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "count,intern_ms,hit_ms,direct_get_ms,intern_ns_per_op,hit_ns_per_op,direct_get_ns_per_op,bytes_reserved,bytes_per_string,occupied_buckets,collision_entries,max_chain,status\n";

    if (!gate) {
        const auto value = run(1000000);
        print(value);
        return value.pass ? 0 : 1;
    }

    const auto small = run(100000);
    const auto large = run(1000000);
    print(small);
    print(large);
    if (!small.pass || !large.pass || small.intern_ms <= 0.0 || large.intern_ms <= 0.0)
        return 1;

    const auto intern_exponent = std::log(large.intern_ms / small.intern_ms) / std::log(10.0);
    const auto hit_exponent = std::log(large.hit_ms / small.hit_ms) / std::log(10.0);
    const auto bytes_per_string = static_cast<double>(large.bytes_reserved) / static_cast<double>(large.count);
    const auto intern_ns = large.intern_ms * 1000000.0 / static_cast<double>(large.count);
    const auto hit_ns = large.hit_ms * 1000000.0 / static_cast<double>(large.count);
    const auto get_ns = large.direct_get_ms * 1000000.0 / static_cast<double>(large.count);
    const bool latency = intern_ns <= 500.0 && hit_ns <= 300.0 && get_ns <= 100.0;
    const bool memory = bytes_per_string <= 96.0;
    const bool structure = large.max_chain <= 32;

    std::cout << "STRING_TABLE_INTERN_SCALING_EXPONENT," << intern_exponent << '\n';
    std::cout << "STRING_TABLE_HIT_SCALING_EXPONENT," << hit_exponent << '\n';
    std::cout << "STRING_TABLE_LATENCY_GATE," << (latency ? "PASS" : "FAIL")
              << ",intern_ns=" << intern_ns << " <= 500,hit_ns=" << hit_ns
              << " <= 300,get_ns=" << get_ns << " <= 100\n";
    std::cout << "STRING_TABLE_MEMORY_GATE," << (memory ? "PASS" : "FAIL")
              << ",bytes_per_string=" << bytes_per_string << " <= 96.0\n";
    std::cout << "STRING_TABLE_STRUCTURE_GATE," << (structure ? "PASS" : "FAIL")
              << ",max_chain=" << large.max_chain << " <= 32\n";
    std::cout << "STRING_TABLE_GATE," << (latency && memory && structure ? "PASS" : "FAIL") << '\n';
    return latency && memory && structure ? 0 : 1;
}
