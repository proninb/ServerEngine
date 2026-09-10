#include "../server_engine/config/server_configuration_loader.hpp"
#include "../server_engine/project/project_configuration_loader.hpp"
#include "../server_engine/project/project_context.hpp"
#include "../server_engine/project/frontend/source_facts_validation.hpp"
#include "../server_engine/diagnostics/diagnostic_descriptor.hpp"

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


[[nodiscard]] source_span span_of(std::string_view text, std::string_view value) {
    const auto offset = text.find(value);
    if (offset == std::string_view::npos)
        return {};
    return source_span{static_cast<std::uint32_t>(offset), static_cast<std::uint32_t>(value.size())};
}

bool test_source_facts_contract() {
    project_configuration configuration;
    project_context context{std::move(configuration)};

    const auto root = context.identity_root();
    identity_ref n = nullptr;
    identity_ref a = nullptr;
    identity_ref b = nullptr;
    if (!context.resolve_declaration(root, "N", identity_kind::namespace_scope, n).ok() ||
        !context.resolve_declaration(n, "A", identity_kind::type, a).ok() ||
        !context.resolve_declaration(n, "B", identity_kind::type, b).ok()) {
        return false;
    }

    constexpr std::string_view text =
        "namespace N { struct B; struct A { const B* value; int count; }; }";

    const auto namespace_range = source_span{0, static_cast<std::uint32_t>(text.size())};
    const auto b_declaration = span_of(text, "struct B;");
    const auto a_declaration = span_of(text, "struct A { const B* value; int count; };");
    const auto value_declaration = span_of(text, "const B* value;");
    const auto count_declaration = span_of(text, "int count;");

    const std::array namespaces{
        source_namespace_fact{n, namespace_range},
    };

    const std::array modifiers{
        source_type_modifier{0, source_type_modifier_kind::const_qualified},
        source_type_modifier{0, source_type_modifier_kind::pointer},
    };

    const std::array members{
        source_member_fact{
            source_type_ref::semantic(b, source_fact_range{0, 2}, span_of(text, "const B*")),
            span_of(text, "value"),
            value_declaration,
            source_member_access::public_access,
        },
        source_member_fact{
            source_type_ref::builtin(
                intrinsic_type::signed_int, source_fact_range{2, 0}, span_of(text, "int")),
            span_of(text, "count"),
            count_declaration,
            source_member_access::public_access,
        },
    };

    const std::array records{
        source_record_fact{
            b,
            source_fact_range{0, 0},
            b_declaration,
            source_record_declaration_kind::declaration,
            source_record_kind::struct_type,
        },
        source_record_fact{
            a,
            source_fact_range{0, 2},
            a_declaration,
            source_record_declaration_kind::definition,
            source_record_kind::struct_type,
        },
    };

    const source_facts facts{
        source_id{17}, text, namespaces, records, members, modifiers};

    source_facts_validation_error error;
    if (!validate_source_facts(facts, error).ok() ||
        error.code != source_facts_error_code::none) {
        return false;
    }

    if (facts.records()[1].identity != a ||
        facts.members()[0].type.identity != b ||
        facts.members()[0].type.intrinsic != intrinsic_type::none ||
        facts.members()[1].type.identity != nullptr ||
        facts.members()[1].type.intrinsic != intrinsic_type::signed_int) {
        return false;
    }

    if (facts.text(facts.members()[0].name) != "value" ||
        facts.text(facts.members()[1].name) != "count") {
        return false;
    }

    const auto value_modifiers = facts.members()[0].type.modifiers;
    if (value_modifiers.begin != 0 || value_modifiers.count != 2)
        return false;

    return facts.modifiers()[0].kind == source_type_modifier_kind::const_qualified &&
           facts.modifiers()[1].kind == source_type_modifier_kind::pointer;
}

bool test_source_facts_validation() {
    project_configuration configuration;
    project_context context{std::move(configuration)};

    identity_ref a = nullptr;
    if (!context.resolve_declaration(
            context.identity_root(), "A", identity_kind::type, a).ok()) {
        return false;
    }

    constexpr std::string_view text = "struct A { Missing value; };";
    const auto record_range = source_span{0, static_cast<std::uint32_t>(text.size())};
    const auto member_range = span_of(text, "Missing value;");

    const std::array<source_namespace_fact, 0> namespaces{};
    const std::array<source_type_modifier, 0> modifiers{};
    const std::array members{
        source_member_fact{
            source_type_ref{nullptr, intrinsic_type::none, source_fact_range{0, 0}, span_of(text, "Missing")},
            span_of(text, "value"),
            member_range,
            source_member_access::public_access,
        },
    };
    const std::array records{
        source_record_fact{
            a,
            source_fact_range{0, 1},
            record_range,
            source_record_declaration_kind::definition,
            source_record_kind::struct_type,
        },
    };

    const source_facts facts{source_id{23}, text, namespaces, records, members, modifiers};
    source_facts_validation_error error;
    const auto result = validate_source_facts(facts, error);
    if (result.code != status_code::invalid_argument ||
        error.code != source_facts_error_code::unresolved_type_base ||
        error.category != source_fact_category::member_fact ||
        error.index != 0) {
        return false;
    }

    diagnostic_buffer diagnostic_output;
    emit_source_facts_validation_diagnostic(facts, error, operation_id{77}, diagnostic_output);
    if (diagnostic_output.records().size() != 1)
        return false;

    const auto& record = diagnostic_output.records().front();
    return record.id == diagnostics::parser_invalid_source_facts.id &&
           record.location.source == source_id{23} &&
           record.location.offset == span_of(text, "Missing").offset &&
           record.detail.find(source_facts_error_detail(error.code)) == 0 &&
           record.detail.find("fact=member[0]") != std::string::npos;
}


bool test_source_facts_order_contract() {
    project_configuration configuration;
    project_context context{std::move(configuration)};

    identity_ref a = nullptr;
    identity_ref b = nullptr;
    if (!context.resolve_declaration(
            context.identity_root(), "A", identity_kind::type, a).ok() ||
        !context.resolve_declaration(
            context.identity_root(), "B", identity_kind::type, b).ok()) {
        return false;
    }

    constexpr std::string_view text = "struct A; struct B;";
    const std::array<source_namespace_fact, 0> namespaces{};
    const std::array<source_member_fact, 0> members{};
    const std::array<source_type_modifier, 0> modifiers{};
    const std::array records{
        source_record_fact{
            b,
            source_fact_range{0, 0},
            span_of(text, "struct B;"),
            source_record_declaration_kind::declaration,
            source_record_kind::struct_type,
        },
        source_record_fact{
            a,
            source_fact_range{0, 0},
            span_of(text, "struct A;"),
            source_record_declaration_kind::declaration,
            source_record_kind::struct_type,
        },
    };

    const source_facts facts{source_id{24}, text, namespaces, records, members, modifiers};
    source_facts_validation_error error;
    const auto result = validate_source_facts(facts, error);

    return result.code == status_code::invalid_argument &&
           error.code == source_facts_error_code::record_order &&
           error.category == source_fact_category::record_fact &&
           error.index == 1;
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
    test_case{"source_facts_contract", &test_source_facts_contract},
    test_case{"source_facts_validation", &test_source_facts_validation},
    test_case{"source_facts_order_contract", &test_source_facts_order_contract},
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
