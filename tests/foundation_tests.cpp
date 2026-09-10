#include "../server_engine/config/server_configuration_loader.hpp"
#include "../server_engine/project/project_configuration_loader.hpp"
#include "../server_engine/project/project_context.hpp"
#include "../server_engine/project/frontend/source_facts_validation.hpp"
#include "../server_engine/project/frontend/include_discovery.hpp"
#include "../server_engine/project/frontend/source_frontend_generation.hpp"
#include "../server_engine/project/source/source_manager.hpp"
#include "../server_engine/project/source/source_hash.hpp"
#include "../server_engine/project/parser/lexer.hpp"
#include "../server_engine/project/parser/source_environment.hpp"
#include "../server_engine/project/parser/source_parser.hpp"
#include "../server_engine/project/builder/source_contribution.hpp"
#include "../server_engine/project/builder/generation_builder.hpp"
#include "../server_engine/project/graph/graph.hpp"
#include "../server_engine/diagnostics/diagnostic_descriptor.hpp"

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
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



bool test_source_facts_enum_validation() {
    project_configuration configuration;
    project_context context{std::move(configuration)};

    identity_ref e = nullptr;
    if (!context.resolve_declaration(
            context.identity_root(), "E", identity_kind::type, e).ok()) {
        return false;
    }

    constexpr std::string_view text = "enum E { A = 1 };";
    const std::array<source_namespace_fact, 0> namespaces{};
    const std::array<source_record_fact, 0> records{};
    const std::array<source_member_fact, 0> members{};
    const std::array<source_type_modifier, 0> modifiers{};
    const std::array values{
        source_enum_value_fact{
            span_of(text, "A"),
            source_integral_constant{intrinsic_type::signed_int, 1},
            span_of(text, "1"),
        },
    };
    const std::array enums{
        source_enum_fact{
            e,
            source_fact_range{0, 2},
            source_span{0, static_cast<std::uint32_t>(text.size())},
            {},
            intrinsic_type::none,
            source_enum_declaration_kind::definition,
            false,
        },
    };
    const std::array declarations{
        source_declaration_ref{
            0,
            source_declaration_kind::enum_type,
            source_span{0, static_cast<std::uint32_t>(text.size())},
        },
    };

    const source_facts facts{
        source_id{25}, text, namespaces, records, members, modifiers, enums, values, declarations};
    source_facts_validation_error error;
    const auto result = validate_source_facts(facts, error);

    return result.code == status_code::invalid_argument &&
           error.code == source_facts_error_code::enum_enumerator_range &&
           error.category == source_fact_category::enum_fact &&
           error.index == 0;
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


bool publish_test_source(
    source_manager& manager,
    std::string_view path,
    std::string_view text,
    source_snapshot& snapshot,
    source_id* source = nullptr) {

    return manager.publish_memory(path, text, snapshot, source).ok();
}

bool lex_and_parse(
    project_context& context,
    const source_snapshot& snapshot,
    const source_environment& environment,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    parsed_source& output) {

    std::vector<parser_token> tokens;
    std::vector<directive_span> directives;
    std::vector<source_include_directive> includes;
    if (!lex_source(snapshot, operation, diagnostics, tokens, &directives).ok())
        return false;
    if (!discover_source_includes(snapshot, tokens, directives, operation, diagnostics, includes).ok())
        return false;
    if (!includes.empty())
        return false;
    source_parser parser{context};
    return parser.parse(snapshot, tokens, environment, operation, diagnostics, output).ok();
}

bool test_source_hash_sha256() {
    const auto hash = hash_source_content("abc");
    constexpr std::array<std::uint8_t, 32> expected{
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad,
    };
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (static_cast<std::uint8_t>(hash.bytes[index]) != expected[index])
            return false;
    }
    return true;
}

bool test_source_snapshot_immutability() {
    source_manager manager;
    source_snapshot first;
    source_id source;
    std::string text = "struct A;";
    if (!publish_test_source(manager, "memory/a.hpp", text, first, &source))
        return false;

    text[7] = 'B';
    if (!first || !source || first.source() != source || first.text() != "struct A;")
        return false;

    source_snapshot second;
    source_id same_source;
    if (!publish_test_source(manager, "memory/a.hpp", "struct C;", second, &same_source))
        return false;

    const auto current = manager.current(source);
    return source == same_source && first.text() == "struct A;" && second.text() == "struct C;" &&
           current.text() == "struct C;" && current.source() == source;
}

bool test_source_manager_transactional_identity() {
    source_manager manager;
    auto update = manager.begin_update();
    source_id source;
    if (!update.resolve("transactional.hpp", source).ok() || !source)
        return false;
    const std::string normalized{update.path(source)};
    source_id found;
    if (manager.find(normalized, found).code != status_code::not_found)
        return false;
    if (!update.commit().ok())
        return false;
    if (!manager.find(normalized, found).ok() || found != source)
        return false;

    auto second = manager.begin_update();
    source_id again;
    return second.resolve("transactional.hpp", again).ok() && again == source;
}

bool test_source_manager_normalized_path_boundary() {
    std::string normalized;
    if (!normalize_source_path("hot/../hot/model.hpp", normalized).ok() || normalized.empty())
        return false;

    source_manager manager;
    auto update = manager.begin_update();
    source_id first;
    source_id repeated;
    if (!update.resolve_normalized(normalized, first).ok() || !first ||
        !update.resolve_normalized(normalized, repeated).ok() || repeated != first ||
        update.source_count() != 1) {
        return false;
    }

    if (!update.commit().ok() || manager.source_count() != 1 ||
        manager.path(first) != normalized || manager.path_index_bytes() == 0 ||
        manager.path_storage_bytes() != normalized.size()) {
        return false;
    }

    source_id found;
    if (!manager.find(normalized, found).ok() || found != first)
        return false;

    auto second = manager.begin_update();
    source_id existing;
    return second.resolve_normalized(normalized, existing).ok() &&
           existing == first && second.source_count() == 1;
}

bool test_source_manager_file_acquisition() {
    const auto path = std::filesystem::temp_directory_path() / "server_engine_v306r_source.hpp";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
            return false;
        output << "struct FileType;";
    }

    source_manager manager;
    auto update = manager.begin_update();
    source_id source;
    if (!update.resolve(path, source).ok())
        return false;
    source_acquire_job job;
    source_acquire_result acquired;
    if (!update.prepare_acquire(source, job).ok() ||
        !source_manager_update::execute_acquire(job, acquired).ok() ||
        acquired.kind != source_acquire_result_kind::present ||
        !update.apply_acquire(std::move(acquired)).ok() || !update.commit().ok()) {
        return false;
    }
    const auto snapshot = manager.current(source);
    if (!snapshot || snapshot.text() != "struct FileType;")
        return false;

    auto second = manager.begin_update();
    source_acquire_job second_job;
    source_acquire_result second_result;
    const bool unchanged = second.prepare_acquire(source, second_job).ok() &&
        source_manager_update::execute_acquire(second_job, second_result).ok() &&
        second_result.kind == source_acquire_result_kind::unchanged;

    std::error_code error;
    std::filesystem::remove(path, error);
    source_id invalid;
    const auto invalid_result = second.resolve({}, invalid);
    return unchanged && invalid_result.code == status_code::invalid_argument;
}

bool test_lexer_include_discovery() {
    source_manager manager;
    source_snapshot snapshot;
    if (!publish_test_source(manager, "memory/include.hpp",
            "#pragma once\n#include \"b.hpp\"\nstruct A;", snapshot))
        return false;

    diagnostic_buffer diagnostics;
    std::vector<parser_token> tokens;
    std::vector<directive_span> directives;
    std::vector<source_include_directive> includes;
    if (!lex_source(snapshot, operation_id{400}, diagnostics, tokens, &directives).ok() || directives.size() != 2)
        return false;
    if (!discover_source_includes(snapshot, tokens, directives, operation_id{400}, diagnostics, includes).ok())
        return false;
    if (diagnostics.has_errors() || includes.size() != 1 || snapshot.text().substr(includes[0].path.offset, includes[0].path.length) != "b.hpp")
        return false;
    for (const auto& token : tokens) {
        if (token.punctuation == parser_punctuation::hash)
            return false;
    }
    return tokens.size() >= 4 && tokens[0].kind == parser_token_kind::keyword_struct;
}

bool test_parser_source_facts_producer() {
    project_configuration configuration;
    project_context context{std::move(configuration)};
    source_manager manager;
    source_snapshot snapshot;
    constexpr std::string_view text =
        "namespace N { struct B; struct A { const B* value; unsigned long long count; int values[4]; }; }";
    if (!publish_test_source(manager, "memory/model.hpp", text, snapshot))
        return false;

    parsed_source parsed;
    diagnostic_buffer diagnostics;
    const source_environment environment;
    if (!lex_and_parse(context, snapshot, environment, operation_id{410}, diagnostics, parsed) || diagnostics.has_errors())
        return false;

    const auto facts = parsed.facts();
    source_facts_validation_error validation_error;
    if (!validate_source_facts(facts, validation_error).ok())
        return false;
    if (facts.namespaces().size() != 1 || facts.records().size() != 2 || facts.members().size() != 3 ||
        facts.declarations().size() != 3)
        return false;

    const auto n = facts.namespaces()[0].identity;
    const auto b = facts.records()[0].identity;
    const auto a = facts.records()[1].identity;
    if (n == nullptr || b == nullptr || a == nullptr || b->parent() != n || a->parent() != n)
        return false;

    const auto& value = facts.members()[0];
    const auto& count = facts.members()[1];
    const auto& values = facts.members()[2];
    if (value.type.identity != b || facts.text(value.name) != "value" ||
        count.type.intrinsic != intrinsic_type::unsigned_long_long ||
        values.type.intrinsic != intrinsic_type::signed_int)
        return false;
    const auto value_modifiers = value.type.modifiers;
    const auto array_modifiers = values.type.modifiers;
    return value_modifiers.count == 2 &&
           facts.modifiers()[value_modifiers.begin].kind == source_type_modifier_kind::const_qualified &&
           facts.modifiers()[value_modifiers.begin + 1].kind == source_type_modifier_kind::pointer &&
           array_modifiers.count == 1 && facts.modifiers()[array_modifiers.begin].value == 4;
}

bool test_parser_enum_facts() {
    project_configuration configuration;
    project_context context{std::move(configuration)};
    source_manager manager;
    source_snapshot snapshot;
    if (!publish_test_source(manager, "memory/enum.hpp",
            "enum class E : unsigned int { A = -1, B, C = 4294967296 };", snapshot))
        return false;
    parsed_source parsed;
    diagnostic_buffer diagnostics;
    const source_environment environment;
    if (!lex_and_parse(context, snapshot, environment, operation_id{420}, diagnostics, parsed))
        return false;
    const auto facts = parsed.facts();
    if (facts.enums().size() != 1 || facts.enum_values().size() != 3 || facts.declarations().size() != 1)
        return false;
    const auto& e = facts.enums()[0];
    return e.identity != nullptr && e.scoped && e.declaration_kind == source_enum_declaration_kind::definition &&
           e.explicit_underlying == intrinsic_type::unsigned_int &&
           static_cast<std::int64_t>(facts.enum_values()[0].value.bits) == -1 &&
           facts.enum_values()[1].value.bits == 0 && facts.enum_values()[2].value.bits == 4294967296ULL;
}

bool test_parser_cross_source_visibility() {
    project_configuration configuration;
    project_context context{std::move(configuration)};
    source_manager manager;
    diagnostic_buffer diagnostics;
    const source_environment empty_environment;

    source_snapshot first_snapshot;
    if (!publish_test_source(manager, "memory/b.hpp", "namespace N { struct B; }", first_snapshot))
        return false;
    parsed_source first;
    if (!lex_and_parse(context, first_snapshot, empty_environment, operation_id{510}, diagnostics, first))
        return false;
    const auto first_facts = first.facts();
    const auto b = first_facts.records()[0].identity;
    const std::array local_types{b};
    source_interface first_interface;
    if (!first_interface.initialize(local_types).ok())
        return false;

    source_snapshot second_snapshot;
    if (!publish_test_source(manager, "memory/a.hpp", "namespace N { struct A { B* link; }; }", second_snapshot))
        return false;
    const std::array imports{source_environment_import{0, &first_interface}};
    const source_environment environment{imports};
    parsed_source second;
    if (!lex_and_parse(context, second_snapshot, environment, operation_id{520}, diagnostics, second))
        return false;
    const auto second_facts = second.facts();
    return !diagnostics.has_errors() && second_facts.members().size() == 1 &&
           second_facts.members()[0].type.identity == b;
}

bool test_parser_positional_include_visibility() {
    project_configuration configuration;
    project_context context{std::move(configuration)};
    source_manager manager;
    diagnostic_buffer diagnostics;
    const source_environment empty_environment;

    source_snapshot b_snapshot;
    if (!publish_test_source(manager, "memory/pos_b.hpp", "struct B;", b_snapshot))
        return false;
    parsed_source b_parsed;
    if (!lex_and_parse(context, b_snapshot, empty_environment, operation_id{530}, diagnostics, b_parsed))
        return false;
    const auto b = b_parsed.facts().records()[0].identity;
    const std::array local_types{b};
    source_interface b_interface;
    if (!b_interface.initialize(local_types).ok())
        return false;

    source_snapshot a_snapshot;
    if (!publish_test_source(manager, "memory/pos_a.hpp",
            "struct A { B* before; };\n#include \"pos_b.hpp\"\nstruct C { B* after; };", a_snapshot))
        return false;
    std::vector<parser_token> tokens;
    std::vector<directive_span> directives;
    std::vector<source_include_directive> includes;
    if (!lex_source(a_snapshot, operation_id{531}, diagnostics, tokens, &directives).ok() ||
        !discover_source_includes(a_snapshot, tokens, directives, operation_id{531}, diagnostics, includes).ok() ||
        includes.size() != 1)
        return false;
    const std::array imports{source_environment_import{includes[0].visible_from, &b_interface}};
    const source_environment environment{imports};
    source_parser parser{context};
    parsed_source output;
    const auto result = parser.parse(a_snapshot, tokens, environment, operation_id{531}, diagnostics, output);
    return result.code == status_code::not_found && !output && !diagnostics.records().empty() &&
           diagnostics.records().back().id == diagnostics::parser_unresolved_type.id;
}

bool test_parser_unresolved_type() {
    project_configuration configuration;
    project_context context{std::move(configuration)};
    source_manager manager;
    source_snapshot snapshot;
    if (!publish_test_source(manager, "memory/invalid.hpp", "struct A { Missing value; };", snapshot))
        return false;
    diagnostic_buffer diagnostics;
    std::vector<parser_token> tokens;
    if (!lex_source(snapshot, operation_id{620}, diagnostics, tokens).ok())
        return false;
    source_parser parser{context};
    parsed_source output;
    const source_environment environment;
    const auto result = parser.parse(snapshot, tokens, environment, operation_id{620}, diagnostics, output);
    return result.code == status_code::not_found && !output && !diagnostics.records().empty() &&
           diagnostics.records().back().id == diagnostics::parser_unresolved_type.id;
}

bool test_parser_enclosing_namespace_lookup() {
    project_configuration configuration;
    project_context context{std::move(configuration)};
    source_manager manager;
    source_snapshot snapshot;
    constexpr std::string_view text =
        "namespace N { struct B; namespace M { struct A { B* parent_type; long double weight; }; } }";
    if (!publish_test_source(manager, "memory/nested.hpp", text, snapshot))
        return false;
    parsed_source parsed;
    diagnostic_buffer diagnostics;
    const source_environment environment;
    if (!lex_and_parse(context, snapshot, environment, operation_id{640}, diagnostics, parsed))
        return false;
    const auto facts = parsed.facts();
    return !diagnostics.has_errors() && facts.namespaces().size() == 2 && facts.records().size() == 2 &&
           facts.members().size() == 2 && facts.members()[0].type.identity == facts.records()[0].identity &&
           facts.members()[1].type.intrinsic == intrinsic_type::long_double_type;
}

bool test_parser_parallel_sources() {
    constexpr std::size_t worker_count = 8;
    project_configuration configuration;
    project_context context{std::move(configuration)};
    source_manager manager;
    std::array<source_snapshot, worker_count> snapshots{};
    for (std::size_t index = 0; index < worker_count; ++index) {
        const auto path = std::string{"memory/parallel_"} + std::to_string(index) + ".hpp";
        if (!publish_test_source(manager, path, "namespace N { struct Shared { int value; }; }", snapshots[index]))
            return false;
    }

    source_parser parser{context};
    std::array<parsed_source, worker_count> outputs{};
    std::array<diagnostic_buffer, worker_count> diagnostic_outputs{};
    std::array<std::vector<parser_token>, worker_count> tokens{};
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    for (std::size_t index = 0; index < worker_count; ++index) {
        workers.emplace_back([&, index] {
            if (!lex_source(snapshots[index], operation_id{static_cast<std::uint64_t>(700 + index)},
                    diagnostic_outputs[index], tokens[index]).ok()) {
                failed.store(true, std::memory_order_relaxed);
                return;
            }
            const source_environment environment;
            if (!parser.parse(snapshots[index], tokens[index], environment,
                    operation_id{static_cast<std::uint64_t>(700 + index)},
                    diagnostic_outputs[index], outputs[index]).ok()) {
                failed.store(true, std::memory_order_relaxed);
            }
        });
    }
    for (auto& worker : workers)
        worker.join();
    if (failed.load(std::memory_order_relaxed))
        return false;

    identity_ref shared = nullptr;
    for (const auto& output : outputs) {
        const auto facts = output.facts();
        if (facts.records().size() != 1)
            return false;
        const auto current = facts.records()[0].identity;
        if (shared == nullptr) shared = current;
        else if (shared != current) return false;
    }
    return context.identity_count() == 3;
}

bool test_frontend_include_pipeline() {
    const auto directory = std::filesystem::temp_directory_path() / "server_engine_v306r_include";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;
    const auto b_path = directory / "b.hpp";
    const auto a_path = directory / "a.hpp";
    {
        std::ofstream b(b_path); b << "namespace N { struct B; }";
        std::ofstream a(a_path); a << "#pragma once\n#include \"b.hpp\"\nnamespace N { struct A { B* link; }; }";
    }

    project_configuration configuration;
    project_context context{std::move(configuration)};
    source_manager manager;
    auto update = manager.begin_update();
    source_frontend_generation frontend{context, update, 4};
    source_frontend_result result;
    diagnostic_buffer diagnostics;
    const std::array roots{a_path};
    const auto build = frontend.build(roots, operation_id{800}, diagnostics, result);
    if (!build.ok() || diagnostics.has_errors() || result.sources().size() != 2 || result.summary().parsed != 2) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    identity_ref b_identity = nullptr;
    identity_ref linked = nullptr;
    for (const auto& entry : result.sources()) {
        const auto filename = std::filesystem::path{entry.parsed.source().normalized_path()}.filename().string();
        const auto facts = entry.parsed.facts();
        if (filename == "b.hpp")
            b_identity = facts.records()[0].identity;
        if (filename == "a.hpp")
            linked = facts.members()[0].type.identity;
    }
    const bool commit_ok = update.commit().ok();
    std::filesystem::remove_all(directory, error);
    return commit_ok && manager.source_count() == 2 && b_identity != nullptr && linked == b_identity;
}

bool test_frontend_include_cycle() {
    const auto directory = std::filesystem::temp_directory_path() / "server_engine_v306r_cycle";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;
    const auto a_path = directory / "a.hpp";
    const auto b_path = directory / "b.hpp";
    { std::ofstream a(a_path); a << "#include \"b.hpp\"\nstruct A;"; }
    { std::ofstream b(b_path); b << "#include \"a.hpp\"\nstruct B;"; }

    project_configuration configuration;
    project_context context{std::move(configuration)};
    source_manager manager;
    auto update = manager.begin_update();
    source_frontend_generation frontend{context, update, 4};
    source_frontend_result result;
    diagnostic_buffer diagnostics;
    const std::array roots{a_path};
    const auto build = frontend.build(roots, operation_id{810}, diagnostics, result);
    std::filesystem::remove_all(directory, error);
    return build.code == status_code::semantic_conflict && !diagnostics.records().empty() &&
           diagnostics.records().back().id == diagnostics::source_dependency_cycle.id;
}

bool test_frontend_parallel_roots() {
    constexpr std::size_t count = 24;
    const auto directory = std::filesystem::temp_directory_path() / "server_engine_v306r_parallel";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;
    std::vector<std::filesystem::path> roots;
    for (std::size_t index = 0; index < count; ++index) {
        const auto path = directory / ("r" + std::to_string(index) + ".hpp");
        std::ofstream file(path);
        file << "namespace N { struct Shared; }";
        roots.push_back(path);
    }

    project_configuration configuration;
    project_context context{std::move(configuration)};
    source_manager manager;
    auto update = manager.begin_update();
    source_frontend_generation frontend{context, update, 8};
    source_frontend_result result;
    diagnostic_buffer diagnostics;
    const auto build = frontend.build(roots, operation_id{820}, diagnostics, result);
    std::filesystem::remove_all(directory, error);
    if (!build.ok() || diagnostics.has_errors() || result.sources().size() != count ||
        result.summary().max_active_workers < 2)
        return false;
    identity_ref shared = nullptr;
    for (const auto& entry : result.sources()) {
        const auto facts = entry.parsed.facts();
        if (facts.records().size() != 1)
            return false;
        if (shared == nullptr) shared = facts.records()[0].identity;
        else if (shared != facts.records()[0].identity) return false;
    }
    return shared != nullptr;
}


bool test_frontend_transitive_include_pipeline() {
    const auto directory = std::filesystem::temp_directory_path() / "server_engine_v306r_transitive";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto c_path = directory / "c.inc";
    const auto b_path = directory / "b.hpp";
    const auto a_path = directory / "a.cpp";
    { std::ofstream c(c_path); c << "namespace N { struct C; }"; }
    { std::ofstream b(b_path); b << "#include \"c.inc\"\nnamespace N { struct B { C* link; }; }"; }
    { std::ofstream a(a_path); a << "#include \"b.hpp\"\nnamespace N { struct A { C* transitive; B* direct; }; }"; }

    project_configuration configuration;
    project_context context{std::move(configuration)};
    source_manager manager;
    auto update = manager.begin_update();
    source_frontend_generation frontend{context, update, 4};
    source_frontend_result result;
    diagnostic_buffer diagnostics;
    const std::array roots{a_path};
    const auto build = frontend.build(roots, operation_id{830}, diagnostics, result);

    bool valid = build.ok() && !diagnostics.has_errors() && result.sources().size() == 3;
    identity_ref c_identity = nullptr;
    identity_ref a_to_c = nullptr;
    if (valid) {
        for (const auto& entry : result.sources()) {
            const auto filename = std::filesystem::path{entry.parsed.source().normalized_path()}.filename().string();
            const auto facts = entry.parsed.facts();
            if (filename == "c.inc")
                c_identity = facts.records()[0].identity;
            else if (filename == "a.cpp")
                a_to_c = facts.members()[0].type.identity;
        }
        valid = c_identity != nullptr && a_to_c == c_identity && update.commit().ok() && manager.source_count() == 3;
    }

    std::filesystem::remove_all(directory, error);
    return valid;
}

bool test_frontend_shared_include_identity() {
    const auto directory = std::filesystem::temp_directory_path() / "server_engine_v306r_shared";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto common_path = directory / "common.hpp";
    const auto a_path = directory / "a.hpp";
    const auto b_path = directory / "b.hpp";
    { std::ofstream common(common_path); common << "namespace N { struct Shared; }"; }
    { std::ofstream a(a_path); a << "#include \"common.hpp\"\nnamespace N { struct A { Shared* value; }; }"; }
    { std::ofstream b(b_path); b << "#include \"common.hpp\"\nnamespace N { struct B { Shared* value; }; }"; }

    project_configuration configuration;
    project_context context{std::move(configuration)};
    source_manager manager;
    auto update = manager.begin_update();
    source_frontend_generation frontend{context, update, 4};
    source_frontend_result result;
    diagnostic_buffer diagnostics;
    const std::array roots{a_path, b_path};
    const auto build = frontend.build(roots, operation_id{840}, diagnostics, result);

    bool valid = build.ok() && !diagnostics.has_errors() && result.sources().size() == 3;
    identity_ref shared = nullptr;
    std::size_t consumers = 0;
    if (valid) {
        for (const auto& entry : result.sources()) {
            const auto filename = std::filesystem::path{entry.parsed.source().normalized_path()}.filename().string();
            const auto facts = entry.parsed.facts();
            if (filename == "common.hpp") {
                shared = facts.records()[0].identity;
            }
        }
        for (const auto& entry : result.sources()) {
            const auto filename = std::filesystem::path{entry.parsed.source().normalized_path()}.filename().string();
            if (filename == "a.hpp" || filename == "b.hpp") {
                const auto facts = entry.parsed.facts();
                if (facts.members().size() != 1 || facts.members()[0].type.identity != shared)
                    valid = false;
                ++consumers;
            }
        }
        valid = valid && shared != nullptr && consumers == 2 && update.commit().ok() && manager.source_count() == 3;
    }

    std::filesystem::remove_all(directory, error);
    return valid;
}

bool test_frontend_unsupported_directive() {
    const auto directory = std::filesystem::temp_directory_path() / "server_engine_v306r_directive";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto path = directory / "a.hpp";
    { std::ofstream file(path); file << "#define X 1\nstruct A;"; }

    project_configuration configuration;
    project_context context{std::move(configuration)};
    source_manager manager;
    auto update = manager.begin_update();
    source_frontend_generation frontend{context, update, 2};
    source_frontend_result result;
    diagnostic_buffer diagnostics;
    const std::array roots{path};
    const auto build = frontend.build(roots, operation_id{850}, diagnostics, result);
    std::filesystem::remove_all(directory, error);

    return !build.ok() && !diagnostics.records().empty() &&
           diagnostics.records().back().id == diagnostics::source_unsupported_directive.id;
}

bool test_frontend_include_inside_scope() {
    const auto directory = std::filesystem::temp_directory_path() / "server_engine_v306r_scope_include";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto b_path = directory / "b.hpp";
    const auto a_path = directory / "a.hpp";
    { std::ofstream b(b_path); b << "struct B;"; }
    { std::ofstream a(a_path); a << "namespace N {\n#include \"b.hpp\"\nstruct A;\n}"; }

    project_configuration configuration;
    project_context context{std::move(configuration)};
    source_manager manager;
    auto update = manager.begin_update();
    source_frontend_generation frontend{context, update, 2};
    source_frontend_result result;
    diagnostic_buffer diagnostics;
    const std::array roots{a_path};
    const auto build = frontend.build(roots, operation_id{860}, diagnostics, result);
    std::filesystem::remove_all(directory, error);

    return !build.ok() && !diagnostics.records().empty() &&
           diagnostics.records().back().id == diagnostics::source_unsupported_directive.id;
}


bool test_source_contribution_capture() {
    project_configuration configuration;
    project_context context{std::move(configuration)};

    identity_ref a = nullptr;
    identity_ref e = nullptr;
    if (!context.resolve_declaration(context.identity_root(), "A", identity_kind::type, a).ok() ||
        !context.resolve_declaration(context.identity_root(), "E", identity_kind::type, e).ok())
        return false;

    std::string text = "struct A { int value; }; enum E : unsigned int { X = 7 };";
    const std::array<source_namespace_fact, 0> namespaces{};
    const std::array<source_type_modifier, 0> modifiers{};
    const std::array members{
        source_member_fact{
            source_type_ref::builtin(intrinsic_type::signed_int, {}, span_of(text, "int")),
            span_of(text, "value"), span_of(text, "int value;"), source_member_access::public_access},
    };
    const std::array records{
        source_record_fact{a, {0, 1}, span_of(text, "struct A { int value; };"),
            source_record_declaration_kind::definition, source_record_kind::struct_type},
    };
    const std::array enum_values{
        source_enum_value_fact{span_of(text, "X"),
            source_integral_constant{intrinsic_type::signed_int, 7}, span_of(text, "7")},
    };
    const std::array enums{
        source_enum_fact{e, {0, 1}, span_of(text, "enum E : unsigned int { X = 7 };"),
            span_of(text, "unsigned int"), intrinsic_type::unsigned_int,
            source_enum_declaration_kind::definition, false},
    };
    const std::array declarations{
        source_declaration_ref{0, source_declaration_kind::record_type,
            span_of(text, "struct A { int value; };")},
        source_declaration_ref{0, source_declaration_kind::enum_type,
            span_of(text, "enum E : unsigned int { X = 7 };")},
    };

    const source_facts facts{source_id{1}, text, namespaces, records, members, modifiers,
        enums, enum_values, declarations};
    source_contribution_cache cache;
    auto update = cache.begin_rebuild();
    diagnostic_buffer diagnostics;
    if (!update.replace(facts, operation_id{900}, diagnostics).ok() ||
        update.statistics().sources != 1 || update.statistics().type_declarations != 2 ||
        update.statistics().members != 1 || update.statistics().enum_values != 1)
        return false;

    text.assign(text.size(), '?');
    if (!update.prepare_publish().ok())
        return false;
    update.publish_prepared();

    const auto* state = cache.state(source_id{1});
    if (state == nullptr || cache.types(source_id{1}).size() != 2)
        return false;
    const auto captured_members = cache.members(state->members);
    const auto captured_values = cache.enum_values(state->enum_values);
    return captured_members.size() == 1 && captured_values.size() == 1 &&
           cache.name(captured_members[0].name) == "value" &&
           cache.name(captured_values[0].name) == "X" &&
           cache.types(source_id{1})[0].identity == a &&
           cache.types(source_id{1})[1].identity == e;
}

bool test_generation_builder_g0() {
    project_configuration configuration;
    project_context context{std::move(configuration)};

    identity_ref b = nullptr;
    identity_ref a = nullptr;
    if (!context.resolve_declaration(context.identity_root(), "B", identity_kind::type, b).ok() ||
        !context.resolve_declaration(context.identity_root(), "A", identity_kind::type, a).ok())
        return false;

    constexpr std::string_view text = "struct B; struct A { B* value; };";
    const std::array<source_namespace_fact, 0> namespaces{};
    const std::array modifiers{
        source_type_modifier{0, source_type_modifier_kind::pointer},
    };
    const std::array members{
        source_member_fact{
            source_type_ref::semantic(b, {0, 1}, span_of(text, "B*")),
            span_of(text, "value"), span_of(text, "B* value;"),
            source_member_access::public_access},
    };
    const std::array records{
        source_record_fact{b, {0, 0}, span_of(text, "struct B;"),
            source_record_declaration_kind::declaration, source_record_kind::struct_type},
        source_record_fact{a, {0, 1}, span_of(text, "struct A { B* value; };"),
            source_record_declaration_kind::definition, source_record_kind::struct_type},
    };
    const std::array<source_enum_fact, 0> enums{};
    const std::array<source_enum_value_fact, 0> enum_values{};
    const std::array declarations{
        source_declaration_ref{0, source_declaration_kind::record_type, span_of(text, "struct B;")},
        source_declaration_ref{1, source_declaration_kind::record_type,
            span_of(text, "struct A { B* value; };")},
    };
    const source_facts facts{source_id{1}, text, namespaces, records, members, modifiers,
        enums, enum_values, declarations};
    const std::array sources{facts};

    source_contribution_cache cache;
    graph graph_value;
    generation_builder builder{cache, graph_value};
    diagnostic_buffer diagnostics;
    if (!builder.prepare_g0(sources, {}, operation_id{901}, diagnostics).ok() ||
        diagnostics.has_errors() || !builder.ready())
        return false;
    if (graph_value.type_count() != 0 || cache.statistics().sources != 0)
        return false;

    builder.publish_prepared();
    if (!builder.published() || graph_value.type_count() != 2 ||
        graph_value.identity(graph_value.type_at(0)) != b ||
        graph_value.identity(graph_value.type_at(1)) != a)
        return false;

    const auto a_handle = graph_value.type_at(1);
    const auto a_members = graph_value.members(a_handle);
    if (a_members.size() != 1 || graph_value.name(a_members[0].name) != "value")
        return false;

    derived_type_record pointer;
    if (!graph_value.derived(a_members[0].type, pointer) ||
        pointer.kind != derived_type_kind::pointer)
        return false;
    type_handle base;
    if (!graph_value.named(pointer.child, base) || base != graph_value.type_at(0))
        return false;

    return cache.statistics().sources == 1 && cache.statistics().type_declarations == 2 &&
           builder.telemetry().unique_types == 2 &&
           builder.telemetry().canonical_type_refs == 2;
}

bool test_generation_builder_enum() {
    project_configuration configuration;
    project_context context{std::move(configuration)};
    identity_ref e = nullptr;
    if (!context.resolve_declaration(context.identity_root(), "E", identity_kind::type, e).ok())
        return false;

    constexpr std::string_view text = "enum E : unsigned int { X = 7, Y = 9 };";
    const std::array<source_namespace_fact, 0> namespaces{};
    const std::array<source_record_fact, 0> records{};
    const std::array<source_member_fact, 0> members{};
    const std::array<source_type_modifier, 0> modifiers{};
    const std::array values{
        source_enum_value_fact{span_of(text, "X"),
            source_integral_constant{intrinsic_type::signed_int, 7}, span_of(text, "7")},
        source_enum_value_fact{span_of(text, "Y"),
            source_integral_constant{intrinsic_type::signed_int, 9}, span_of(text, "9")},
    };
    const std::array enums{
        source_enum_fact{e, {0, 2}, source_span{0, static_cast<std::uint32_t>(text.size())},
            span_of(text, "unsigned int"), intrinsic_type::unsigned_int,
            source_enum_declaration_kind::definition, false},
    };
    const std::array declarations{
        source_declaration_ref{0, source_declaration_kind::enum_type,
            source_span{0, static_cast<std::uint32_t>(text.size())}},
    };
    const source_facts facts{source_id{1}, text, namespaces, records, members, modifiers,
        enums, values, declarations};
    const std::array sources{facts};

    source_contribution_cache cache;
    graph graph_value;
    generation_builder builder{cache, graph_value};
    diagnostic_buffer diagnostics;
    if (!builder.prepare_g0(sources, {}, operation_id{902}, diagnostics).ok())
        return false;
    builder.publish_prepared();

    const auto handle = graph_value.type_at(0);
    const auto* entry = graph_value.find(handle);
    const auto materialized = graph_value.enum_values(handle);
    return entry != nullptr && entry->kind == graph_type_kind::enumeration && entry->defined() &&
           entry->enum_fixed_underlying() && entry->enum_underlying == intrinsic_type::unsigned_int &&
           materialized.size() == 2 && materialized[0].bits == 7 && materialized[1].bits == 9 &&
           graph_value.name(materialized[0].name) == "X" && graph_value.name(materialized[1].name) == "Y";
}

bool test_generation_builder_redeclaration() {
    project_configuration configuration;
    project_context context{std::move(configuration)};
    identity_ref a = nullptr;
    if (!context.resolve_declaration(context.identity_root(), "A", identity_kind::type, a).ok())
        return false;

    constexpr std::string_view text1 = "struct A;";
    constexpr std::string_view text2 = "class A {};";
    const std::array<source_namespace_fact, 0> namespaces{};
    const std::array<source_member_fact, 0> members{};
    const std::array<source_type_modifier, 0> modifiers{};
    const std::array<source_enum_fact, 0> enums{};
    const std::array<source_enum_value_fact, 0> values{};
    const std::array records1{
        source_record_fact{a, {0, 0}, source_span{0, static_cast<std::uint32_t>(text1.size())},
            source_record_declaration_kind::declaration, source_record_kind::struct_type},
    };
    const std::array records2{
        source_record_fact{a, {0, 0}, source_span{0, static_cast<std::uint32_t>(text2.size())},
            source_record_declaration_kind::definition, source_record_kind::class_type},
    };
    const std::array declarations1{
        source_declaration_ref{0, source_declaration_kind::record_type,
            source_span{0, static_cast<std::uint32_t>(text1.size())}},
    };
    const std::array declarations2{
        source_declaration_ref{0, source_declaration_kind::record_type,
            source_span{0, static_cast<std::uint32_t>(text2.size())}},
    };
    const source_facts facts1{source_id{1}, text1, namespaces, records1, members, modifiers,
        enums, values, declarations1};
    const source_facts facts2{source_id{2}, text2, namespaces, records2, members, modifiers,
        enums, values, declarations2};
    const std::array sources{facts1, facts2};

    source_contribution_cache cache;
    graph graph_value;
    generation_builder builder{cache, graph_value};
    diagnostic_buffer diagnostics;
    if (!builder.prepare_g0(sources, {}, operation_id{903}, diagnostics).ok())
        return false;
    builder.publish_prepared();
    const auto* entry = graph_value.find(graph_value.type_at(0));
    return graph_value.type_count() == 1 && entry != nullptr && entry->defined() &&
           entry->record_kind == source_record_kind::class_type;
}

bool test_generation_builder_definition_conflict() {
    project_configuration configuration;
    project_context context{std::move(configuration)};
    identity_ref a = nullptr;
    if (!context.resolve_declaration(context.identity_root(), "A", identity_kind::type, a).ok())
        return false;

    constexpr std::string_view text = "struct A {};";
    const std::array<source_namespace_fact, 0> namespaces{};
    const std::array<source_member_fact, 0> members{};
    const std::array<source_type_modifier, 0> modifiers{};
    const std::array<source_enum_fact, 0> enums{};
    const std::array<source_enum_value_fact, 0> values{};
    const std::array records{
        source_record_fact{a, {0, 0}, source_span{0, static_cast<std::uint32_t>(text.size())},
            source_record_declaration_kind::definition, source_record_kind::struct_type},
    };
    const std::array declarations{
        source_declaration_ref{0, source_declaration_kind::record_type,
            source_span{0, static_cast<std::uint32_t>(text.size())}},
    };
    const source_facts facts1{source_id{1}, text, namespaces, records, members, modifiers,
        enums, values, declarations};
    const source_facts facts2{source_id{2}, text, namespaces, records, members, modifiers,
        enums, values, declarations};
    const std::array sources{facts1, facts2};

    source_contribution_cache cache;
    graph graph_value;
    generation_builder builder{cache, graph_value};
    diagnostic_buffer diagnostics;
    const auto result = builder.prepare_g0(sources, {}, operation_id{904}, diagnostics);
    return result.code == status_code::semantic_conflict && diagnostics.has_errors() &&
           graph_value.type_count() == 0 && cache.statistics().sources == 0 && !builder.ready();
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
    test_case{"source_facts_enum_validation", &test_source_facts_enum_validation},
    test_case{"source_facts_order_contract", &test_source_facts_order_contract},
    test_case{"source_hash_sha256", &test_source_hash_sha256},
    test_case{"source_snapshot_immutability", &test_source_snapshot_immutability},
    test_case{"source_manager_transactional_identity", &test_source_manager_transactional_identity},
    test_case{"source_manager_normalized_path_boundary", &test_source_manager_normalized_path_boundary},
    test_case{"source_manager_file_acquisition", &test_source_manager_file_acquisition},
    test_case{"lexer_include_discovery", &test_lexer_include_discovery},
    test_case{"parser_source_facts_producer", &test_parser_source_facts_producer},
    test_case{"parser_enum_facts", &test_parser_enum_facts},
    test_case{"parser_cross_source_visibility", &test_parser_cross_source_visibility},
    test_case{"parser_positional_include_visibility", &test_parser_positional_include_visibility},
    test_case{"parser_unresolved_type", &test_parser_unresolved_type},
    test_case{"parser_enclosing_namespace_lookup", &test_parser_enclosing_namespace_lookup},
    test_case{"parser_parallel_sources", &test_parser_parallel_sources},
    test_case{"frontend_include_pipeline", &test_frontend_include_pipeline},
    test_case{"frontend_include_cycle", &test_frontend_include_cycle},
    test_case{"frontend_parallel_roots", &test_frontend_parallel_roots},
    test_case{"frontend_transitive_include_pipeline", &test_frontend_transitive_include_pipeline},
    test_case{"frontend_shared_include_identity", &test_frontend_shared_include_identity},
    test_case{"frontend_unsupported_directive", &test_frontend_unsupported_directive},
    test_case{"frontend_include_inside_scope", &test_frontend_include_inside_scope},
    test_case{"source_contribution_capture", &test_source_contribution_capture},
    test_case{"generation_builder_g0", &test_generation_builder_g0},
    test_case{"generation_builder_enum", &test_generation_builder_enum},
    test_case{"generation_builder_redeclaration", &test_generation_builder_redeclaration},
    test_case{"generation_builder_definition_conflict", &test_generation_builder_definition_conflict},
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
