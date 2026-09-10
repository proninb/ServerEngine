#include "project/source/source_manager.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

using clock_type = std::chrono::steady_clock;
using cw::server::source_id;
using cw::server::source_manager;

struct measurement final {
    std::size_t count = 0;
    double resolve_ms = 0.0;
    double commit_ms = 0.0;
    double find_ms = 0.0;
    bool pass = false;
};

[[nodiscard]] std::filesystem::path source_path(std::size_t index) {
    return std::filesystem::path{"benchmark_sources"} /
        ("source_" + std::to_string(index) + ".hpp");
}

[[nodiscard]] measurement run(std::size_t count) {
    source_manager manager;
    auto update = manager.begin_update();

    const auto resolve_begin = clock_type::now();
    for (std::size_t index = 0; index < count; ++index) {
        source_id source;
        const auto result = update.resolve(source_path(index), source);
        if (!result.ok() || source.value() != index + 1)
            return measurement{count};
    }
    const auto resolve_end = clock_type::now();

    const auto commit_begin = clock_type::now();
    const auto commit_result = update.commit();
    const auto commit_end = clock_type::now();
    if (!commit_result.ok() || manager.source_count() != count)
        return measurement{count};

    auto verify = manager.begin_update();
    const auto find_begin = clock_type::now();
    for (std::size_t index = 0; index < count; ++index) {
        source_id source;
        const auto result = verify.resolve(source_path(index), source);
        if (!result.ok() || source.value() != index + 1)
            return measurement{count};
    }
    const auto find_end = clock_type::now();

    const auto milliseconds = [](auto begin, auto end) {
        return std::chrono::duration<double, std::milli>(end - begin).count();
    };

    return measurement{
        count,
        milliseconds(resolve_begin, resolve_end),
        milliseconds(commit_begin, commit_end),
        milliseconds(find_begin, find_end),
        true,
    };
}

void print(const measurement& value) {
    const auto operations = static_cast<double>(value.count);
    std::cout << value.count << ','
              << std::fixed << std::setprecision(6)
              << value.resolve_ms << ','
              << value.commit_ms << ','
              << value.find_ms << ','
              << (value.resolve_ms * 1'000'000.0 / operations) << ','
              << (value.find_ms * 1'000'000.0 / operations) << ','
              << (value.pass ? "PASS" : "FAIL") << '\n';
}

} // namespace

int main(int argc, char** argv) {
    const bool full = argc == 2 && std::string_view{argv[1]} == "--full";
    if (argc > 2 || (argc == 2 && !full))
        return 2;

    std::cout << "count,resolve_unique_ms,commit_ms,resolve_existing_ms,resolve_unique_ns_per_op,resolve_existing_ns_per_op,status\n";

    const auto small = run(full ? 10'000 : 100'000);
    const auto medium = run(full ? 100'000 : 1'000'000);
    print(small);
    print(medium);
    if (!small.pass || !medium.pass)
        return 1;

    if (!full) {
        const auto exponent = std::log(medium.resolve_ms / small.resolve_ms) /
            std::log(static_cast<double>(medium.count) / static_cast<double>(small.count));
        std::cout << "SCALING_EXPONENT," << std::fixed << std::setprecision(3) << exponent << '\n';
        if (exponent > 1.50) {
            std::cout << "SOURCE_MANAGER_SCALE_GATE,FAIL,exponent " << exponent << " > 1.50\n";
            return 1;
        }
        std::cout << "SOURCE_MANAGER_SCALE_GATE,PASS,exponent " << exponent << " <= 1.50\n";
    }

    return 0;
}
