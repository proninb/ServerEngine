#include "../server_engine/config/server_configuration_loader.hpp"
#include "../server_engine/project/project_configuration_loader.hpp"
#include "../server_engine/project/project_context.hpp"

#include <array>
#include <atomic>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace cw::server;

bool test_server_configuration() {
    constexpr std::string_view text = R"({"version":1,"server":{"endpoints":[],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":"project.json"}})";
    diagnostic_buffer diagnostics;
    server_configuration configuration;
    if (!load_server_configuration(text, "/tmp/server.json", operation_id{1}, diagnostics, configuration).ok())
        return false;
    return configuration.version == 1 && configuration.project.path.filename() == "project.json";
}

bool test_project_configuration() {
    constexpr std::string_view text = R"({"version":1,"name":"Example","project":[{"path":"types/a.hpp","role":"type"}],"configuration":{"abi":{"target":"windows-x64","pack":8}}})";
    diagnostic_buffer diagnostics;
    project_configuration configuration;
    if (!load_project_configuration(text, "/tmp/project.json", operation_id{2}, diagnostics, configuration).ok())
        return false;
    return configuration.name == "Example" && configuration.project.size() == 1 &&
           configuration.project.front().role == project_item_role::type;
}

bool test_project_identity_resolution() {
    project_configuration configuration;
    configuration.version = 1;
    configuration.name = "IdentityTest";
    project_context context{std::move(configuration)};

    const auto root = context.identity_root();
    if (root == nullptr || root->parent() != nullptr || root->kind() != identity_kind::root)
        return false;

    identity_ref n1 = nullptr;
    identity_ref n2 = nullptr;
    if (!context.resolve_declaration(root, "N", identity_kind::namespace_scope, n1).ok())
        return false;
    if (!context.resolve_declaration(root, "N", identity_kind::namespace_scope, n2).ok())
        return false;
    if (n1 == nullptr || n1 != n2 || n1->name().view() != "N")
        return false;

    identity_ref a1 = nullptr;
    identity_ref a2 = nullptr;
    if (!context.resolve_declaration(n1, "A", identity_kind::type, a1).ok())
        return false;
    if (!context.resolve_declaration(n2, "A", identity_kind::type, a2).ok())
        return false;

    if (a1 == nullptr || a1 != a2 || a1->parent() != n1 ||
        a1->name().view() != "A" || a1->kind() != identity_kind::type)
        return false;

    identity_ref conflict = nullptr;
    const auto conflict_result = context.resolve_declaration(
        n1, "A", identity_kind::namespace_scope, conflict);
    if (conflict_result.code != status_code::semantic_conflict || conflict != nullptr)
        return false;

    return context.identity_count() == 3;
}

bool test_project_identity_name_lifetime() {
    project_configuration configuration;
    project_context context{std::move(configuration)};

    std::string source_name = "Controller";
    identity_ref identity = nullptr;
    if (!context.resolve_declaration(
            context.identity_root(), source_name, identity_kind::type, identity).ok())
        return false;

    source_name.assign("Modified");
    return identity != nullptr && identity->name().view() == "Controller";
}

bool test_project_identity_concurrency() {
    project_configuration configuration;
    project_context context{std::move(configuration)};

    constexpr std::size_t thread_count = 8;
    constexpr std::size_t iterations = 10000;
    std::array<identity_ref, thread_count> results{};
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (std::size_t worker = 0; worker < thread_count; ++worker) {
        workers.emplace_back([&, worker] {
            identity_ref last = nullptr;
            for (std::size_t index = 0; index < iterations; ++index) {
                identity_ref current = nullptr;
                if (!context.resolve_declaration(
                        context.identity_root(), "Shared", identity_kind::type, current).ok()) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
                if (last != nullptr && current != last) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
                last = current;
            }
            results[worker] = last;
        });
    }

    for (auto& worker : workers)
        worker.join();

    if (failed.load(std::memory_order_relaxed) || results[0] == nullptr)
        return false;

    for (const auto result : results) {
        if (result != results[0])
            return false;
    }

    return context.identity_count() == 2;
}

bool test_project_identity_index_statistics() {
    project_configuration configuration;
    project_context context{std::move(configuration)};

    const auto root = context.identity_root();
    for (std::size_t index = 0; index < 1000; ++index) {
        const auto name = std::string{"Stats_"} + std::to_string(index);
        identity_ref identity = nullptr;
        if (!context.resolve_declaration(root, name, identity_kind::type, identity).ok() ||
            identity == nullptr)
            return false;
    }

    const auto before = context.identity_count();
    const auto statistics = context.identity_index_stats();
    const auto after = context.identity_count();

    return before == 1001 && after == before &&
           statistics.entry_count == 1000 &&
           statistics.occupied_buckets != 0 &&
           statistics.occupied_buckets <= statistics.entry_count &&
           statistics.collision_entries ==
               statistics.entry_count - statistics.occupied_buckets &&
           statistics.max_chain_length >= 1 &&
           statistics.average_chain_length >= 1.0 &&
           statistics.average_successful_lookup_comparisons >= 1.0 &&
           statistics.p95_successful_lookup_comparisons >= 1 &&
           statistics.p99_successful_lookup_comparisons >=
               statistics.p95_successful_lookup_comparisons;
}

using test_function = bool (*)();

struct test_case {
    std::string_view name;
    test_function function;
};

constexpr std::array tests{
    test_case{"server_configuration", &test_server_configuration},
    test_case{"project_configuration", &test_project_configuration},
    test_case{"project_identity_resolution", &test_project_identity_resolution},
    test_case{"project_identity_name_lifetime", &test_project_identity_name_lifetime},
    test_case{"project_identity_concurrency", &test_project_identity_concurrency},
    test_case{"project_identity_index_statistics", &test_project_identity_index_statistics},
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 2)
        return 2;

    const std::string_view requested{argv[1]};
    for (const auto& item : tests) {
        if (item.name == requested)
            return item.function() ? 0 : 1;
    }

    return 2;
}
