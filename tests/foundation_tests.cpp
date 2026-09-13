#include "../server_engine/config/server_configuration_loader.hpp"
#include "../server_engine/project/project_configuration_loader.hpp"
#include "../server_engine/project/project_context.hpp"
#include "../server_engine/project/project_build_orchestrator.hpp"
#include "../server_engine/project/project_manager.hpp"
#include "../server_engine/project/persistence/baseline_store.hpp"
#include "../server_engine/project/persistence/source_manager_image.hpp"
#include "../server_engine/project/persistence/compiled_image.hpp"
#include "../server_engine/project/persistence/build_cache_image.hpp"
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
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
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
    if (!root || context.identity_metadata().parent(root) || root.kind() != identity_kind::root)
        return false;

    identity_ref n1 = nullptr;
    identity_ref n2 = nullptr;
    if (!context.resolve_declaration(root, "N", identity_kind::namespace_scope, n1).ok())
        return false;
    if (!context.resolve_declaration(root, "N", identity_kind::namespace_scope, n2).ok())
        return false;
    if (!n1 || n1 != n2 || context.string(context.identity_metadata().name(n1)) != "N")
        return false;

    identity_ref a1 = nullptr;
    identity_ref a2 = nullptr;
    if (!context.resolve_declaration(n1, "A", identity_kind::type, a1).ok())
        return false;
    if (!context.resolve_declaration(n2, "A", identity_kind::type, a2).ok())
        return false;

    if (!a1 || a1 != a2 || context.identity_metadata().parent(a1) != n1 ||
        context.string(context.identity_metadata().name(a1)) != "A" ||
        a1.kind() != identity_kind::type)
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
    return identity && context.string(context.identity_metadata().name(identity)) == "Controller";
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



bool test_project_identity_compact_reference() {
    static_assert(sizeof(identity_ref) == 4);
    static_assert(std::is_trivially_copyable_v<identity_ref>);
    static_assert(std::is_standard_layout_v<identity_ref>);
    static_assert(!std::is_pointer_v<identity_ref>);

    project_configuration configuration;
    project_context context{std::move(configuration)};

    const auto metadata = context.identity_metadata();
    const auto root = context.identity_root();
    if (!root || root.kind() != identity_kind::root ||
        metadata.parent(root) || metadata.name(root)) {
        return false;
    }

    identity_ref ns;
    identity_ref type;
    if (!context.resolve_declaration(
            root, "N", identity_kind::namespace_scope, ns).ok() ||
        !context.resolve_declaration(
            ns, "A", identity_kind::type, type).ok()) {
        return false;
    }

    return ns && type &&
           ns.kind() == identity_kind::namespace_scope &&
           type.kind() == identity_kind::type &&
           metadata.parent(ns) == root &&
           metadata.parent(type) == ns &&
           context.string(metadata.name(ns)) == "N" &&
           context.string(metadata.name(type)) == "A" &&
           ns.value() != type.value();
}

[[nodiscard]] source_span span_of(std::string_view text, std::string_view value) {
    const auto offset = text.find(value);
    if (offset == std::string_view::npos)
        return {};
    return source_span{static_cast<std::uint32_t>(offset), static_cast<std::uint32_t>(value.size())};
}

[[nodiscard]] string_id test_string(project_context& context, std::string_view value) {
    string_id result;
    return context.intern_string(value, result).ok() ? result : string_id{};
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
            test_string(context, "value"),
            value_declaration,
            source_member_access::public_access,
        },
        source_member_fact{
            source_type_ref::builtin(
                intrinsic_type::signed_int, source_fact_range{2, 0}, span_of(text, "int")),
            test_string(context, "count"),
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

    if (context.string(facts.members()[0].name) != "value" ||
        context.string(facts.members()[1].name) != "count") {
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
            test_string(context, "value"),
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
            test_string(context, "A"),
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
    source_parser parser{context.parser_services()};
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
    if (!n || !b || !a || context.identity_metadata().parent(b) != n ||
        context.identity_metadata().parent(a) != n)
        return false;

    const auto& value = facts.members()[0];
    const auto& count = facts.members()[1];
    const auto& values = facts.members()[2];
    if (value.type.identity != b || context.string(value.name) != "value" ||
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
    source_interface first_interface;
    if (!first_interface.initialize(first_facts, context.identity_metadata()).ok())
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
    const auto b_facts = b_parsed.facts();
    source_interface b_interface;
    if (!b_interface.initialize(b_facts, context.identity_metadata()).ok())
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
    source_parser parser{context.parser_services()};
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
    source_parser parser{context.parser_services()};
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

    source_parser parser{context.parser_services()};
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


bool test_frontend_parallel_failure_safe() {
    constexpr std::size_t count = 32;
    const auto directory = std::filesystem::temp_directory_path() /
        "server_engine_v309_parallel_failure";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    std::vector<std::filesystem::path> roots;
    roots.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto path = directory / ("r" + std::to_string(index) + ".hpp");
        std::ofstream file(path);
        if (index == count / 2)
            file << "#define BROKEN 1\nstruct Broken;";
        else
            file << "struct T" << index << ";";
        roots.push_back(path);
    }

    project_configuration configuration;
    project_context context{std::move(configuration)};
    source_manager manager;
    auto update = manager.begin_update();
    source_frontend_generation frontend{context, update, 8};
    source_frontend_result result;
    diagnostic_buffer diagnostics;
    const auto build = frontend.build(
        roots, operation_id{825}, diagnostics, result);

    const bool pass = !build.ok() && !diagnostics.records().empty() &&
        manager.source_count() == 0 && result.sources().empty();
    std::filesystem::remove_all(directory, error);
    return pass;
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
            test_string(context, "value"), span_of(text, "int value;"), source_member_access::public_access},
    };
    const std::array records{
        source_record_fact{a, {0, 1}, span_of(text, "struct A { int value; };"),
            source_record_declaration_kind::definition, source_record_kind::struct_type},
    };
    const std::array enum_values{
        source_enum_value_fact{test_string(context, "X"),
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
           context.string(captured_members[0].name) == "value" &&
           context.string(captured_values[0].name) == "X" &&
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
            test_string(context, "value"), span_of(text, "B* value;"),
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
    if (a_members.size() != 1 || context.string(a_members[0].name) != "value")
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
        source_enum_value_fact{test_string(context, "X"),
            source_integral_constant{intrinsic_type::signed_int, 7}, span_of(text, "7")},
        source_enum_value_fact{test_string(context, "Y"),
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
           context.string(materialized[0].name) == "X" && context.string(materialized[1].name) == "Y";
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


bool test_generation_builder_incremental_modify() {
    project_configuration configuration;
    project_context context{std::move(configuration)};
    identity_ref a = nullptr;
    identity_ref b = nullptr;
    if (!context.resolve_declaration(context.identity_root(), "A", identity_kind::type, a).ok() ||
        !context.resolve_declaration(context.identity_root(), "B", identity_kind::type, b).ok())
        return false;

    constexpr std::string_view empty_text = "x";
    constexpr std::string_view member_text = "int x;";
    const std::array<source_namespace_fact, 0> namespaces{};
    const std::array<source_type_modifier, 0> modifiers{};
    const std::array<source_enum_fact, 0> enums{};
    const std::array<source_enum_value_fact, 0> values{};
    const std::array<source_member_fact, 0> no_members{};

    const std::array a0_records{
        source_record_fact{a, {}, {0, 1}, source_record_declaration_kind::definition, source_record_kind::struct_type},
    };
    const std::array b0_records{
        source_record_fact{b, {}, {0, 1}, source_record_declaration_kind::definition, source_record_kind::struct_type},
    };
    const std::array declarations{
        source_declaration_ref{0, source_declaration_kind::record_type, {0, 1}},
    };
    const source_facts a0{source_id{1}, empty_text, namespaces, a0_records, no_members, modifiers, enums, values, declarations};
    const source_facts b0{source_id{2}, empty_text, namespaces, b0_records, no_members, modifiers, enums, values, declarations};
    const std::array initial{a0, b0};

    source_contribution_cache cache;
    graph graph_value;
    diagnostic_buffer diagnostics;
    generation_builder g0{cache, graph_value};
    if (!g0.prepare_g0(initial, {}, operation_id{910}, diagnostics).ok())
        return false;
    g0.publish_prepared();
    const auto a_handle = graph_value.type_at(0);
    const auto b_handle = graph_value.type_at(1);
    if (!a_handle || !b_handle)
        return false;

    const std::array members{
        source_member_fact{
            source_type_ref::builtin(intrinsic_type::signed_int, {}, {0, 3}),
            test_string(context, "x"), {0, 6}, source_member_access::public_access},
    };
    const std::array a1_records{
        source_record_fact{a, {0, 1}, {0, 6}, source_record_declaration_kind::definition, source_record_kind::struct_type},
    };
    const std::array a1_declarations{
        source_declaration_ref{0, source_declaration_kind::record_type, {0, 6}},
    };
    const source_facts a1{source_id{1}, member_text, namespaces, a1_records, members, modifiers, enums, values, a1_declarations};
    const std::array replacements{a1};

    diagnostic_buffer incremental_diagnostics;
    generation_builder g1{cache, graph_value};
    if (!g1.prepare_incremental(replacements, {}, {}, operation_id{911}, incremental_diagnostics).ok())
        return false;
    const auto telemetry = g1.telemetry();
    if (telemetry.changed_sources != 1 || telemetry.changed_types != 1 ||
        telemetry.graph_full_scans != 0 || telemetry.contribution_full_scans != 0)
        return false;
    g1.publish_prepared();

    return graph_value.type_count() == 2 &&
        graph_value.type_at(0) == a_handle && graph_value.type_at(1) == b_handle &&
        graph_value.members(a_handle).size() == 1 &&
        context.string(graph_value.members(a_handle)[0].name) == "x";
}

bool test_generation_builder_incremental_remove_add() {
    project_configuration configuration;
    project_context context{std::move(configuration)};
    identity_ref a = nullptr;
    if (!context.resolve_declaration(context.identity_root(), "A", identity_kind::type, a).ok())
        return false;

    constexpr std::string_view text = "x";
    const std::array<source_namespace_fact, 0> namespaces{};
    const std::array<source_member_fact, 0> members{};
    const std::array<source_type_modifier, 0> modifiers{};
    const std::array<source_enum_fact, 0> enums{};
    const std::array<source_enum_value_fact, 0> values{};
    const std::array records{
        source_record_fact{a, {}, {0, 1}, source_record_declaration_kind::definition, source_record_kind::struct_type},
    };
    const std::array declarations{
        source_declaration_ref{0, source_declaration_kind::record_type, {0, 1}},
    };
    const source_facts facts1{source_id{1}, text, namespaces, records, members, modifiers, enums, values, declarations};

    source_contribution_cache cache;
    graph graph_value;
    diagnostic_buffer diagnostics;
    generation_builder g0{cache, graph_value};
    const std::array initial{facts1};
    if (!g0.prepare_g0(initial, {}, operation_id{912}, diagnostics).ok())
        return false;
    g0.publish_prepared();
    const auto original_handle = graph_value.type_at(0);
    if (!original_handle)
        return false;

    const std::array removals{source_id{1}};
    diagnostic_buffer remove_diagnostics;
    generation_builder g1{cache, graph_value};
    if (!g1.prepare_incremental({}, removals, {}, operation_id{913}, remove_diagnostics).ok())
        return false;
    g1.publish_prepared();
    if (graph_value.type_count() != 0 || graph_value.type_at(0))
        return false;

    const source_facts facts2{source_id{2}, text, namespaces, records, members, modifiers, enums, values, declarations};
    const std::array replacements{facts2};
    diagnostic_buffer add_diagnostics;
    generation_builder g2{cache, graph_value};
    if (!g2.prepare_incremental(replacements, {}, {}, operation_id{914}, add_diagnostics).ok())
        return false;
    if (g2.telemetry().graph_full_scans != 0 || g2.telemetry().added_types != 1)
        return false;
    g2.publish_prepared();

    return graph_value.type_count() == 1 &&
        graph_value.type_at(0) == original_handle && graph_value.type_slot_count() == 1;
}

bool test_generation_builder_incremental_dangling_guard() {
    project_configuration configuration;
    project_context context{std::move(configuration)};
    identity_ref a = nullptr;
    identity_ref b = nullptr;
    if (!context.resolve_declaration(context.identity_root(), "A", identity_kind::type, a).ok() ||
        !context.resolve_declaration(context.identity_root(), "B", identity_kind::type, b).ok())
        return false;

    constexpr std::string_view a_text = "x";
    constexpr std::string_view b_text = "A* b;";
    const std::array<source_namespace_fact, 0> namespaces{};
    const std::array<source_enum_fact, 0> enums{};
    const std::array<source_enum_value_fact, 0> values{};
    const std::array<source_member_fact, 0> no_members{};
    const std::array<source_type_modifier, 0> no_modifiers{};
    const std::array a_records{
        source_record_fact{a, {}, {0, 1}, source_record_declaration_kind::definition, source_record_kind::struct_type},
    };
    const std::array declarations{
        source_declaration_ref{0, source_declaration_kind::record_type, {0, 1}},
    };
    const source_facts a_facts{source_id{1}, a_text, namespaces, a_records, no_members, no_modifiers, enums, values, declarations};

    const std::array modifiers{
        source_type_modifier{0, source_type_modifier_kind::pointer},
    };
    const std::array b_members{
        source_member_fact{
            source_type_ref::semantic(a, {0, 1}, {0, 2}),
            test_string(context, "a"), {0, 5}, source_member_access::public_access},
    };
    const std::array b_records{
        source_record_fact{b, {0, 1}, {0, 5}, source_record_declaration_kind::definition, source_record_kind::struct_type},
    };
    const std::array b_declarations{
        source_declaration_ref{0, source_declaration_kind::record_type, {0, 5}},
    };
    const source_facts b_facts{source_id{2}, b_text, namespaces, b_records, b_members, modifiers, enums, values, b_declarations};
    const std::array initial{a_facts, b_facts};

    source_contribution_cache cache;
    graph graph_value;
    diagnostic_buffer diagnostics;
    generation_builder g0{cache, graph_value};
    if (!g0.prepare_g0(initial, {}, operation_id{915}, diagnostics).ok())
        return false;
    g0.publish_prepared();
    const auto before_types = graph_value.type_count();
    const auto before_sources = cache.statistics().sources;

    const std::array removals{source_id{1}};
    diagnostic_buffer incremental_diagnostics;
    generation_builder g1{cache, graph_value};
    const auto result = g1.prepare_incremental({}, removals, {}, operation_id{916}, incremental_diagnostics);
    return result.code == status_code::semantic_conflict && incremental_diagnostics.has_errors() &&
        !g1.ready() &&
        graph_value.type_count() == before_types && cache.statistics().sources == before_sources &&
        g1.telemetry().validation_visited_types == 2;
}

bool test_generation_builder_incremental_conflict_rollback() {
    project_configuration configuration;
    project_context context{std::move(configuration)};
    identity_ref a = nullptr;
    if (!context.resolve_declaration(context.identity_root(), "A", identity_kind::type, a).ok())
        return false;

    constexpr std::string_view declaration_text = "struct A;";
    constexpr std::string_view definition_text = "struct A{};";
    const std::array<source_namespace_fact, 0> namespaces{};
    const std::array<source_member_fact, 0> members{};
    const std::array<source_type_modifier, 0> modifiers{};
    const std::array<source_enum_fact, 0> enums{};
    const std::array<source_enum_value_fact, 0> values{};
    const std::array declaration_records{
        source_record_fact{a, {}, {0, 9}, source_record_declaration_kind::declaration, source_record_kind::struct_type},
    };
    const std::array definition_records{
        source_record_fact{a, {}, {0, 11}, source_record_declaration_kind::definition, source_record_kind::struct_type},
    };
    const std::array declaration_order{
        source_declaration_ref{0, source_declaration_kind::record_type, {0, 9}},
    };
    const std::array definition_order{
        source_declaration_ref{0, source_declaration_kind::record_type, {0, 11}},
    };
    const source_facts declaration{source_id{1}, declaration_text, namespaces, declaration_records, members, modifiers, enums, values, declaration_order};
    const source_facts definition{source_id{2}, definition_text, namespaces, definition_records, members, modifiers, enums, values, definition_order};
    const std::array initial{declaration, definition};

    source_contribution_cache cache;
    graph graph_value;
    diagnostic_buffer diagnostics;
    generation_builder g0{cache, graph_value};
    if (!g0.prepare_g0(initial, {}, operation_id{917}, diagnostics).ok())
        return false;
    g0.publish_prepared();

    const source_facts conflicting{source_id{1}, definition_text, namespaces, definition_records, members, modifiers, enums, values, definition_order};
    const std::array replacements{conflicting};
    diagnostic_buffer incremental_diagnostics;
    generation_builder g1{cache, graph_value};
    const auto result = g1.prepare_incremental(replacements, {}, {}, operation_id{918}, incremental_diagnostics);
    return result.code == status_code::semantic_conflict && incremental_diagnostics.has_errors() &&
        graph_value.type_count() == 1 &&
        cache.statistics().sources == 2 && !g1.ready();
}



[[nodiscard]] type_handle find_graph_type_by_name(
    const project_context& context,
    std::string_view name) {

    const auto& graph_value = context.compiled_graph();
    for (std::size_t index = 0; index < graph_value.type_slot_count(); ++index) {
        const auto handle = graph_value.type_at(index);
        if (!handle)
            continue;
        const auto identity = graph_value.identity(handle);
        if (identity && context.string(context.identity_metadata().name(identity)) == name)
            return handle;
    }
    return {};
}

bool test_project_build_orchestrator_full() {
    const auto directory = std::filesystem::temp_directory_path() / "server_engine_v308_full";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto b_path = directory / "b.hpp";
    const auto a_path = directory / "a.hpp";
    { std::ofstream b(b_path); b << "struct B;"; }
    { std::ofstream a(a_path); a << "#include \"b.hpp\"\nstruct A { B* value; };"; }

    project_configuration configuration;
    configuration.version = 1;
    configuration.name = "E2E";
    configuration.project.push_back(project_item_configuration{a_path, project_item_role::type});
    project_context context{std::move(configuration)};
    project_build_orchestrator orchestrator{context, 2};
    diagnostic_buffer diagnostics;
    project_build_result build;
    const auto result = orchestrator.rebuild(operation_id{1001}, diagnostics, build);
    if (!result.ok() || diagnostics.has_errors()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    std::string normalized_a;
    std::string normalized_b;
    source_id a_source;
    source_id b_source;
    const bool resolved = normalize_source_path(a_path, normalized_a).ok() &&
        normalize_source_path(b_path, normalized_b).ok() &&
        context.sources().find(normalized_a, a_source).ok() &&
        context.sources().find(normalized_b, b_source).ok();

    const auto a_handle = find_graph_type_by_name(context, "A");
    const auto b_handle = find_graph_type_by_name(context, "B");
    const auto b_dependents = context.sources().dependents(b_source);
    const bool pass = resolved && a_source && b_source && a_source != b_source &&
        context.sources().source_count() == 2 && context.compiled_graph().type_count() == 2 &&
        a_handle && b_handle && context.frontend_cache().complete() &&
        context.frontend_cache().interface(a_source) != nullptr &&
        context.frontend_cache().interface(b_source) != nullptr &&
        b_dependents.size() == 1 && b_dependents.front() == a_source &&
        build.changed && build.telemetry.frontend.parsed == 2;

    std::filesystem::remove_all(directory, error);
    return pass;
}

bool test_project_build_orchestrator_incremental() {
    const auto directory = std::filesystem::temp_directory_path() / "server_engine_v308_incremental";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto b_path = directory / "b.hpp";
    const auto a_path = directory / "a.hpp";
    { std::ofstream b(b_path); b << "struct B;"; }
    { std::ofstream a(a_path); a << "#include \"b.hpp\"\nstruct A { B* value; };"; }

    project_configuration configuration;
    configuration.version = 1;
    configuration.name = "E2E";
    configuration.project.push_back(project_item_configuration{a_path, project_item_role::type});
    project_context context{std::move(configuration)};
    project_build_orchestrator orchestrator{context, 2};
    diagnostic_buffer diagnostics;
    project_build_result full;
    if (!orchestrator.rebuild(operation_id{1002}, diagnostics, full).ok() || diagnostics.has_errors()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    std::string normalized_b;
    source_id b_source;
    if (!normalize_source_path(b_path, normalized_b).ok() ||
        !context.sources().find(normalized_b, b_source).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    const auto a_handle_before = find_graph_type_by_name(context, "A");
    const auto b_handle_before = find_graph_type_by_name(context, "B");
    { std::ofstream b(b_path, std::ios::trunc); b << "struct B { int count; };"; }

    diagnostics.clear();
    project_build_result update;
    const std::array dirty{b_source};
    const auto result = orchestrator.update(dirty, operation_id{1003}, diagnostics, update);
    const auto a_handle_after = find_graph_type_by_name(context, "A");
    const auto b_handle_after = find_graph_type_by_name(context, "B");
    const auto b_members = context.compiled_graph().members(b_handle_after);

    const bool pass = result.ok() && !diagnostics.has_errors() && update.changed &&
        update.telemetry.frontend.dirty == 1 && update.telemetry.frontend.changed == 1 &&
        update.telemetry.frontend.affected == 2 && update.telemetry.frontend.acquired == 1 &&
        update.telemetry.frontend.parsed == 2 &&
        update.telemetry.builder.graph_full_scans == 0 &&
        update.telemetry.builder.contribution_full_scans == 0 &&
        update.telemetry.sources.source_graph_full_scans == 0 &&
        update.telemetry.sources.path_index_full_rebuilds == 0 &&
        a_handle_before == a_handle_after && b_handle_before == b_handle_after &&
        b_members.size() == 1 && context.string(b_members.front().name) == "count";

    std::filesystem::remove_all(directory, error);
    return pass;
}

bool test_project_build_orchestrator_no_change() {
    const auto directory = std::filesystem::temp_directory_path() / "server_engine_v308_no_change";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto path = directory / "a.hpp";
    { std::ofstream file(path); file << "struct A;"; }

    project_configuration configuration;
    configuration.version = 1;
    configuration.name = "E2E";
    configuration.project.push_back(project_item_configuration{path, project_item_role::type});
    project_context context{std::move(configuration)};
    project_build_orchestrator orchestrator{context, 1};
    diagnostic_buffer diagnostics;
    project_build_result full;
    if (!orchestrator.rebuild(operation_id{1004}, diagnostics, full).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    std::string normalized;
    source_id source;
    if (!normalize_source_path(path, normalized).ok() || !context.sources().find(normalized, source).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    project_build_result update;
    const std::array dirty{source};
    const auto result = orchestrator.update(dirty, operation_id{1005}, diagnostics, update);
    const bool pass = result.ok() && !update.changed &&
        update.telemetry.frontend.changed == 0 && update.telemetry.frontend.parsed == 0 &&
        context.compiled_graph().type_count() == 1;
    std::filesystem::remove_all(directory, error);
    return pass;
}

bool test_project_build_orchestrator_failure_rollback() {
    const auto directory = std::filesystem::temp_directory_path() / "server_engine_v308_rollback";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto path = directory / "a.hpp";
    { std::ofstream file(path); file << "struct A { int value; };"; }

    project_configuration configuration;
    configuration.version = 1;
    configuration.name = "E2E";
    configuration.project.push_back(project_item_configuration{path, project_item_role::type});
    project_context context{std::move(configuration)};
    project_build_orchestrator orchestrator{context, 1};
    diagnostic_buffer diagnostics;
    project_build_result full;
    if (!orchestrator.rebuild(operation_id{1006}, diagnostics, full).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    std::string normalized;
    source_id source;
    if (!normalize_source_path(path, normalized).ok() || !context.sources().find(normalized, source).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    const auto committed_snapshot = context.sources().current(source);
    const auto handle = find_graph_type_by_name(context, "A");
    const auto before_members = context.compiled_graph().members(handle).size();
    const auto* before_interface = context.frontend_cache().interface(source);
    { std::ofstream file(path, std::ios::trunc); file << "#define X 1\nstruct A { int changed; };"; }

    diagnostics.clear();
    project_build_result update;
    const std::array dirty{source};
    const auto result = orchestrator.update(dirty, operation_id{1007}, diagnostics, update);
    const auto after_snapshot = context.sources().current(source);
    const bool pass = !result.ok() && diagnostics.has_errors() &&
        after_snapshot && committed_snapshot && after_snapshot.hash() == committed_snapshot.hash() &&
        context.compiled_graph().members(handle).size() == before_members &&
        context.frontend_cache().interface(source) == before_interface;

    std::filesystem::remove_all(directory, error);
    return pass;
}



bool test_project_build_orchestrator_new_include() {
    const auto directory = std::filesystem::temp_directory_path() / "server_engine_v308_new_include";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto a_path = directory / "a.hpp";
    const auto b_path = directory / "b.hpp";
    { std::ofstream a(a_path); a << "struct A;"; }
    { std::ofstream b(b_path); b << "struct B;"; }

    project_configuration configuration;
    configuration.version = 1;
    configuration.name = "E2E";
    configuration.project.push_back(project_item_configuration{a_path, project_item_role::type});
    project_context context{std::move(configuration)};
    project_build_orchestrator orchestrator{context, 2};
    diagnostic_buffer diagnostics;
    project_build_result full;
    if (!orchestrator.rebuild(operation_id{1008}, diagnostics, full).ok() ||
        context.sources().source_count() != 1) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    std::string normalized_a;
    source_id a_source;
    if (!normalize_source_path(a_path, normalized_a).ok() ||
        !context.sources().find(normalized_a, a_source).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    { std::ofstream a(a_path, std::ios::trunc); a << "#include \"b.hpp\"\nstruct A { B* value; };"; }
    diagnostics.clear();
    project_build_result update;
    const std::array dirty{a_source};
    const auto result = orchestrator.update(dirty, operation_id{1009}, diagnostics, update);

    std::string normalized_b;
    source_id b_source;
    const bool resolved_b = normalize_source_path(b_path, normalized_b).ok() &&
        context.sources().find(normalized_b, b_source).ok();
    const auto b_dependents = context.sources().dependents(b_source);
    const bool pass = result.ok() && !diagnostics.has_errors() && resolved_b &&
        context.sources().source_count() == 2 && context.compiled_graph().type_count() == 2 &&
        update.telemetry.frontend.acquired == 2 && update.telemetry.frontend.parsed == 2 &&
        update.telemetry.sources.path_index_full_rebuilds == 0 &&
        update.telemetry.sources.source_graph_full_scans == 0 &&
        b_dependents.size() == 1 && b_dependents.front() == a_source &&
        context.frontend_cache().interface(b_source) != nullptr;

    std::filesystem::remove_all(directory, error);
    return pass;
}

bool test_project_build_orchestrator_remove_leaf() {
    const auto directory = std::filesystem::temp_directory_path() / "server_engine_v308_remove_leaf";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto path = directory / "a.hpp";
    { std::ofstream file(path); file << "struct A;"; }
    project_configuration configuration;
    configuration.version = 1;
    configuration.name = "E2E";
    configuration.project.push_back(project_item_configuration{path, project_item_role::type});
    project_context context{std::move(configuration)};
    project_build_orchestrator orchestrator{context, 1};
    diagnostic_buffer diagnostics;
    project_build_result full;
    if (!orchestrator.rebuild(operation_id{1010}, diagnostics, full).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    std::string normalized;
    source_id source;
    if (!normalize_source_path(path, normalized).ok() || !context.sources().find(normalized, source).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }
    std::filesystem::remove(path, error);
    if (error) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    diagnostics.clear();
    project_build_result update;
    const std::array dirty{source};
    const auto result = orchestrator.update(dirty, operation_id{1011}, diagnostics, update);
    const bool pass = result.ok() && update.changed &&
        context.compiled_graph().type_count() == 0 && !context.sources().current(source) &&
        context.frontend_cache().interface(source) == nullptr &&
        update.telemetry.frontend.changed == 1 && update.telemetry.frontend.affected == 1 &&
        update.telemetry.sources.source_graph_full_scans == 0;
    std::filesystem::remove_all(directory, error);
    return pass;
}

bool test_project_build_orchestrator_builder_conflict_rollback() {
    const auto directory = std::filesystem::temp_directory_path() / "server_engine_v308_builder_rollback";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto a_path = directory / "a.hpp";
    const auto b_path = directory / "b.hpp";
    { std::ofstream b(b_path); b << "struct B {};"; }
    { std::ofstream a(a_path); a << "#include \"b.hpp\"\nstruct A;"; }

    project_configuration configuration;
    configuration.version = 1;
    configuration.name = "E2E";
    configuration.project.push_back(project_item_configuration{a_path, project_item_role::type});
    project_context context{std::move(configuration)};
    project_build_orchestrator orchestrator{context, 2};
    diagnostic_buffer diagnostics;
    project_build_result full;
    if (!orchestrator.rebuild(operation_id{1012}, diagnostics, full).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    std::string normalized_a;
    source_id a_source;
    if (!normalize_source_path(a_path, normalized_a).ok() ||
        !context.sources().find(normalized_a, a_source).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    const auto committed_snapshot = context.sources().current(a_source);
    const auto b_handle = find_graph_type_by_name(context, "B");
    const auto before_types = context.compiled_graph().type_count();
    const auto* before_interface = context.frontend_cache().interface(a_source);
    { std::ofstream a(a_path, std::ios::trunc); a << "#include \"b.hpp\"\nstruct B {};"; }

    diagnostics.clear();
    project_build_result update;
    const std::array dirty{a_source};
    const auto result = orchestrator.update(dirty, operation_id{1013}, diagnostics, update);
    const auto after_snapshot = context.sources().current(a_source);
    const bool pass = result.code == status_code::semantic_conflict && diagnostics.has_errors() &&
        committed_snapshot && after_snapshot && committed_snapshot.hash() == after_snapshot.hash() &&
        context.compiled_graph().type_count() == before_types &&
        find_graph_type_by_name(context, "B") == b_handle &&
        context.frontend_cache().interface(a_source) == before_interface;

    std::filesystem::remove_all(directory, error);
    return pass;
}



bool test_project_build_orchestrator_semantic_noop() {
    const auto directory = std::filesystem::temp_directory_path() / "server_engine_v308_semantic_noop";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto path = directory / "a.hpp";
    { std::ofstream file(path); file << "struct A;"; }
    project_configuration configuration;
    configuration.version = 1;
    configuration.name = "E2E";
    configuration.project.push_back(project_item_configuration{path, project_item_role::type});
    project_context context{std::move(configuration)};
    project_build_orchestrator orchestrator{context, 1};
    diagnostic_buffer diagnostics;
    project_build_result full;
    if (!orchestrator.rebuild(operation_id{1014}, diagnostics, full).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    std::string normalized;
    source_id source;
    if (!normalize_source_path(path, normalized).ok() || !context.sources().find(normalized, source).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }
    const auto handle = find_graph_type_by_name(context, "A");
    const auto before_hash = context.sources().current(source).hash();
    { std::ofstream file(path, std::ios::trunc); file << "\nstruct A;\n"; }

    diagnostics.clear();
    project_build_result update;
    const std::array dirty{source};
    const auto result = orchestrator.update(dirty, operation_id{1015}, diagnostics, update);
    const bool pass = result.ok() && !diagnostics.has_errors() && update.changed &&
        update.telemetry.frontend.changed == 1 && update.telemetry.frontend.parsed == 1 &&
        update.telemetry.builder.changed_sources == 0 && update.telemetry.builder.changed_types == 0 &&
        update.telemetry.builder_prepare_ns == 0 &&
        context.compiled_graph().type_count() == 1 &&
        find_graph_type_by_name(context, "A") == handle &&
        context.sources().current(source).hash() != before_hash;
    std::filesystem::remove_all(directory, error);
    return pass;
}

bool test_project_build_orchestrator_reverse_edge_replacement() {
    const auto directory = std::filesystem::temp_directory_path() / "server_engine_v308_edge_replace";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto a_path = directory / "a.hpp";
    const auto b_path = directory / "b.hpp";
    const auto c_path = directory / "c.hpp";
    { std::ofstream b(b_path); b << "struct B;"; }
    { std::ofstream c(c_path); c << "struct C;"; }
    { std::ofstream a(a_path); a << "#include \"b.hpp\"\nstruct A { B* value; };"; }

    project_configuration configuration;
    configuration.version = 1;
    configuration.name = "E2E";
    configuration.project.push_back(project_item_configuration{a_path, project_item_role::type});
    configuration.project.push_back(project_item_configuration{c_path, project_item_role::type});
    project_context context{std::move(configuration)};
    project_build_orchestrator orchestrator{context, 2};
    diagnostic_buffer diagnostics;
    project_build_result full;
    if (!orchestrator.rebuild(operation_id{1016}, diagnostics, full).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    std::string normalized_a;
    std::string normalized_b;
    std::string normalized_c;
    source_id a_source;
    source_id b_source;
    source_id c_source;
    if (!normalize_source_path(a_path, normalized_a).ok() ||
        !normalize_source_path(b_path, normalized_b).ok() ||
        !normalize_source_path(c_path, normalized_c).ok() ||
        !context.sources().find(normalized_a, a_source).ok() ||
        !context.sources().find(normalized_b, b_source).ok() ||
        !context.sources().find(normalized_c, c_source).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }
    if (context.sources().dependents(b_source).size() != 1 ||
        context.sources().dependents(c_source).size() != 0) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    { std::ofstream a(a_path, std::ios::trunc); a << "#include \"c.hpp\"\nstruct A { C* value; };"; }
    diagnostics.clear();
    project_build_result update;
    const std::array dirty{a_source};
    const auto result = orchestrator.update(dirty, operation_id{1017}, diagnostics, update);
    const auto b_dependents = context.sources().dependents(b_source);
    const auto c_dependents = context.sources().dependents(c_source);
    const bool pass = result.ok() && !diagnostics.has_errors() &&
        b_dependents.empty() && c_dependents.size() == 1 && c_dependents.front() == a_source &&
        update.telemetry.sources.reverse_edge_patches == 2 &&
        update.telemetry.sources.path_index_full_rebuilds == 0 &&
        update.telemetry.sources.source_graph_full_scans == 0;
    std::filesystem::remove_all(directory, error);
    return pass;
}


bool test_string_table_canonicalization() {
    project_configuration configuration;
    project_context context{std::move(configuration)};

    string_id a1;
    string_id a2;
    string_id b;
    if (!context.intern_string("A", a1).ok() ||
        !context.intern_string("A", a2).ok() ||
        !context.intern_string("B", b).ok()) {
        return false;
    }
    if (!a1 || a1 != a2 || !b || b == a1 ||
        context.string(a1) != "A" || context.string(b) != "B") {
        return false;
    }

    constexpr std::size_t thread_count = 8;
    std::array<string_id, thread_count> shared{};
    std::array<std::thread, thread_count> workers;
    for (std::size_t index = 0; index < thread_count; ++index) {
        workers[index] = std::thread([&context, &shared, index]() {
            string_id value;
            for (std::size_t pass = 0; pass < 128; ++pass) {
                if (!context.intern_string("Shared", value).ok()) {
                    shared[index] = {};
                    return;
                }
            }
            shared[index] = value;
        });
    }
    for (auto& worker : workers)
        worker.join();

    const auto canonical = context.find_string("Shared");
    if (!canonical || context.string(canonical) != "Shared")
        return false;
    for (const auto value : shared) {
        if (value != canonical)
            return false;
    }
    return true;
}

bool test_complete_graph_objects_links_query() {
    const auto directory = std::filesystem::temp_directory_path() / "server_engine_v309_objects_links";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto path = directory / "model.hpp";
    {
        std::ofstream file(path);
        file << "namespace N { struct IO { int IN; int OUT; }; IO A; IO B; B.IN = A.OUT; }";
    }

    project_configuration configuration;
    configuration.version = 1;
    configuration.name = "GraphObjectsLinks";
    configuration.project.push_back(project_item_configuration{path, project_item_role::type});
    project_context context{std::move(configuration)};
    project_build_orchestrator orchestrator{context, 1};
    diagnostic_buffer diagnostics;
    project_build_result build;
    const auto result = orchestrator.rebuild(operation_id{1100}, diagnostics, build);
    if (!result.ok() || diagnostics.has_errors()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    type_handle io;
    object_handle a;
    object_handle b;
    object_endpoint a_out;
    object_endpoint b_in;
    link_handle link;
    if (!context.find_type("N::IO", io).ok() ||
        !context.find_object("N::A", a).ok() ||
        !context.find_object("N::B", b).ok() ||
        !context.find_endpoint("N::A.OUT", a_out).ok() ||
        !context.find_endpoint("N::B.IN", b_in).ok() ||
        !context.find_link("N::B.IN", link).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    const auto& graph_value = context.compiled_graph();
    const auto* a_entry = graph_value.find(a);
    const auto* link_entry = graph_value.find(link);
    type_handle a_type;
    const bool pass = graph_value.type_count() == 1 && graph_value.object_count() == 2 &&
        graph_value.link_count() == 1 && a_entry != nullptr &&
        graph_value.named(a_entry->type, a_type) && a_type == io &&
        link_entry != nullptr && link_entry->source == a_out && link_entry->target == b_in &&
        graph_value.find_link(b_in) == link;

    std::filesystem::remove_all(directory, error);
    return pass;
}

bool test_complete_graph_incremental_link_retarget() {
    const auto directory = std::filesystem::temp_directory_path() / "server_engine_v309_link_retarget";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto path = directory / "model.hpp";
    {
        std::ofstream file(path);
        file << "struct IO { int IN; int OUT; }; IO A; IO B; IO C; B.IN = A.OUT;";
    }
    project_configuration configuration;
    configuration.version = 1;
    configuration.name = "LinkRetarget";
    configuration.project.push_back(project_item_configuration{path, project_item_role::type});
    project_context context{std::move(configuration)};
    project_build_orchestrator orchestrator{context, 1};
    diagnostic_buffer diagnostics;
    project_build_result full;
    if (!orchestrator.rebuild(operation_id{1110}, diagnostics, full).ok() || diagnostics.has_errors()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    object_handle a_before;
    object_handle b_before;
    object_handle c_before;
    object_endpoint b_in;
    link_handle link_before;
    if (!context.find_object("A", a_before).ok() || !context.find_object("B", b_before).ok() ||
        !context.find_object("C", c_before).ok() || !context.find_endpoint("B.IN", b_in).ok() ||
        !context.find_link("B.IN", link_before).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    std::string normalized;
    source_id source;
    if (!normalize_source_path(path, normalized).ok() || !context.sources().find(normalized, source).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }
    {
        std::ofstream file(path, std::ios::trunc);
        file << "struct IO { int IN; int OUT; }; IO A; IO B; IO C; B.IN = C.OUT;  ";
    }

    diagnostics.clear();
    project_build_result update;
    const std::array dirty{source};
    const auto result = orchestrator.update(dirty, operation_id{1111}, diagnostics, update);

    object_handle a_after;
    object_handle b_after;
    object_handle c_after;
    object_endpoint c_out;
    link_handle link_after;
    const bool resolved = context.find_object("A", a_after).ok() &&
        context.find_object("B", b_after).ok() && context.find_object("C", c_after).ok() &&
        context.find_endpoint("C.OUT", c_out).ok() && context.find_link("B.IN", link_after).ok();
    const auto* link_entry = resolved ? context.compiled_graph().find(link_after) : nullptr;
    const bool pass = result.ok() && !diagnostics.has_errors() && resolved &&
        a_before == a_after && b_before == b_after && c_before == c_after &&
        link_before == link_after && link_entry != nullptr && link_entry->source == c_out &&
        link_entry->target == b_in && context.compiled_graph().object_count() == 3 &&
        context.compiled_graph().link_count() == 1 &&
        update.telemetry.builder.graph_full_scans == 0 &&
        update.telemetry.builder.contribution_full_scans == 0;

    std::filesystem::remove_all(directory, error);
    return pass;
}

bool test_complete_graph_object_reactivation() {
    const auto directory = std::filesystem::temp_directory_path() / "server_engine_v309_object_reactivation";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto path = directory / "model.hpp";
    { std::ofstream file(path); file << "struct IO { int value; }; IO A;"; }
    project_configuration configuration;
    configuration.version = 1;
    configuration.name = "ObjectReactivation";
    configuration.project.push_back(project_item_configuration{path, project_item_role::type});
    project_context context{std::move(configuration)};
    project_build_orchestrator orchestrator{context, 1};
    diagnostic_buffer diagnostics;
    project_build_result full;
    if (!orchestrator.rebuild(operation_id{1120}, diagnostics, full).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    object_handle original;
    if (!context.find_object("A", original).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }
    std::string normalized;
    source_id source;
    if (!normalize_source_path(path, normalized).ok() || !context.sources().find(normalized, source).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    { std::ofstream file(path, std::ios::trunc); file << "struct IO { int value; };"; }
    diagnostics.clear();
    project_build_result removed;
    const std::array dirty{source};
    if (!orchestrator.update(dirty, operation_id{1121}, diagnostics, removed).ok() ||
        context.compiled_graph().object_count() != 0) {
        std::filesystem::remove_all(directory, error);
        return false;
    }
    object_handle absent;
    if (context.find_object("A", absent).code != status_code::not_found ||
        context.compiled_graph().object_at(original.value() - 1)) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    { std::ofstream file(path, std::ios::trunc); file << "struct IO { int value; }; IO A;  "; }
    diagnostics.clear();
    project_build_result added;
    const auto result = orchestrator.update(dirty, operation_id{1122}, diagnostics, added);
    object_handle reactivated;
    const bool pass = result.ok() && !diagnostics.has_errors() &&
        context.find_object("A", reactivated).ok() && reactivated == original &&
        context.compiled_graph().object_count() == 1 &&
        added.telemetry.builder.graph_full_scans == 0 &&
        added.telemetry.builder.contribution_full_scans == 0;

    std::filesystem::remove_all(directory, error);
    return pass;
}

bool test_complete_graph_duplicate_new_link_rollback() {
    const auto directory = std::filesystem::temp_directory_path() / "server_engine_v309_duplicate_link";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto path = directory / "model.hpp";
    {
        std::ofstream file(path);
        file << "struct IO { int IN; int OUT; }; IO A; IO B; IO C;";
    }
    project_configuration configuration;
    configuration.version = 1;
    configuration.name = "DuplicateLink";
    configuration.project.push_back(project_item_configuration{path, project_item_role::type});
    project_context context{std::move(configuration)};
    project_build_orchestrator orchestrator{context, 1};
    diagnostic_buffer diagnostics;
    project_build_result full;
    if (!orchestrator.rebuild(operation_id{1130}, diagnostics, full).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    std::string normalized;
    source_id source;
    if (!normalize_source_path(path, normalized).ok() || !context.sources().find(normalized, source).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }
    const auto committed = context.sources().current(source);
    const auto* committed_interface = context.frontend_cache().interface(source);
    object_handle a_before;
    object_handle b_before;
    object_handle c_before;
    if (!context.find_object("A", a_before).ok() || !context.find_object("B", b_before).ok() ||
        !context.find_object("C", c_before).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    {
        std::ofstream file(path, std::ios::trunc);
        file << "struct IO { int IN; int OUT; }; IO A; IO B; IO C; B.IN = A.OUT; B.IN = C.OUT;  ";
    }
    diagnostics.clear();
    project_build_result update;
    const std::array dirty{source};
    const auto result = orchestrator.update(dirty, operation_id{1131}, diagnostics, update);
    const auto after = context.sources().current(source);
    object_handle a_after;
    object_handle b_after;
    object_handle c_after;
    const bool pass = result.code == status_code::semantic_conflict && diagnostics.has_errors() &&
        committed && after && committed.hash() == after.hash() &&
        context.frontend_cache().interface(source) == committed_interface &&
        context.compiled_graph().link_count() == 0 && context.compiled_graph().object_count() == 3 &&
        context.find_object("A", a_after).ok() && context.find_object("B", b_after).ok() &&
        context.find_object("C", c_after).ok() &&
        a_before == a_after && b_before == b_after && c_before == c_after;

    std::filesystem::remove_all(directory, error);
    return pass;
}


bool test_project_manager_rebuild_ready_unload() {
    const auto directory = std::filesystem::temp_directory_path() /
        "server_engine_v310a_ready_lifecycle";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto model_path = directory / "model.hpp";
    const auto project_path = directory / "project.json";
    { std::ofstream file(model_path); file << "struct IO { int value; }; IO A;"; }
    {
        std::ofstream file(project_path);
        file << R"({"version":1,"name":"Ready","project":[{"path":"model.hpp","role":"type"}],"configuration":{"abi":{"target":"windows-x64","pack":8}}})";
    }

    project_manager manager;
    diagnostic_buffer diagnostics;
    project_build_result build;
    if (!manager.rebuild(project_path, operation_id{1140}, diagnostics, build, 2).ok() ||
        diagnostics.has_errors() || !build.rebuilt ||
        manager.state() != project_lifecycle_state::ready) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    project_access access;
    object_handle object;
    if (!manager.acquire(access).ok() || !access ||
        !access->find_object("A", object).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    project_build_result second;
    const auto invalid = manager.rebuild(
        project_path, operation_id{1141}, diagnostics, second, 1);
    if (invalid.code != status_code::invalid_state ||
        manager.state() != project_lifecycle_state::ready) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    access.reset();
    if (!manager.unload().ok() ||
        manager.state() != project_lifecycle_state::unloaded) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    project_access after;
    const bool pass =
        manager.acquire(after).code == status_code::not_found && !after;

    std::filesystem::remove_all(directory, error);
    return pass;
}

bool test_project_access_move_only() {
    static_assert(!std::is_copy_constructible_v<project_access>);
    static_assert(!std::is_copy_assignable_v<project_access>);
    static_assert(std::is_nothrow_move_constructible_v<project_access>);
    static_assert(std::is_nothrow_move_assignable_v<project_access>);

    const auto directory = std::filesystem::temp_directory_path() /
        "server_engine_v310a_access_move";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto path = directory / "model.hpp";
    { std::ofstream file(path); file << "struct A { int value; };"; }

    project_configuration configuration;
    configuration.version = 1;
    configuration.name = "AccessMove";
    configuration.project.push_back(
        project_item_configuration{path, project_item_role::type});

    project_manager manager;
    diagnostic_buffer diagnostics;
    project_build_result build;
    if (!manager.rebuild(
            std::move(configuration), operation_id{1150}, diagnostics, build, 1).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    project_access first;
    if (!manager.acquire(first).ok() || !first) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    project_access second{std::move(first)};
    if (first || !second) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    project_access third;
    third = std::move(second);
    if (second || !third) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    third.reset();
    const bool pass = manager.unload().ok() &&
        manager.state() == project_lifecycle_state::unloaded;

    std::filesystem::remove_all(directory, error);
    return pass;
}

bool test_project_unload_stop_before_wait() {
    const auto directory = std::filesystem::temp_directory_path() /
        "server_engine_v310a_stop_before_wait";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto path = directory / "model.hpp";
    { std::ofstream file(path); file << "struct A { int value; };"; }

    project_configuration configuration;
    configuration.version = 1;
    configuration.name = "StopBeforeWait";
    configuration.project.push_back(
        project_item_configuration{path, project_item_role::type});

    project_manager manager;
    diagnostic_buffer diagnostics;
    project_build_result build;
    if (!manager.rebuild(
            std::move(configuration), operation_id{1160}, diagnostics, build, 1).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    std::atomic_bool started{false};
    std::atomic_bool stop_requested{false};
    std::atomic_bool worker_failed{false};
    std::atomic_bool released{false};

    std::thread worker([&] {
        project_access access;
        if (!manager.acquire(access).ok() || !access) {
            worker_failed.store(true, std::memory_order_release);
            return;
        }

        started.store(true, std::memory_order_release);
        while (!stop_requested.load(std::memory_order_acquire))
            std::this_thread::yield();

        access.reset();
        released.store(true, std::memory_order_release);
    });

    while (!started.load(std::memory_order_acquire) &&
           !worker_failed.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    if (worker_failed.load(std::memory_order_acquire)) {
        worker.join();
        std::filesystem::remove_all(directory, error);
        return false;
    }

    project_stop_request stop{
        &stop_requested,
        [](void* context) noexcept {
            static_cast<std::atomic_bool*>(context)->store(
                true, std::memory_order_release);
        },
    };

    const auto unload_result = manager.unload(stop);
    worker.join();

    const bool pass = unload_result.ok() &&
        stop_requested.load(std::memory_order_acquire) &&
        released.load(std::memory_order_acquire) &&
        manager.state() == project_lifecycle_state::unloaded;

    std::filesystem::remove_all(directory, error);
    return pass;
}

bool test_project_rebuild_failure_returns_unloaded() {
    const auto directory = std::filesystem::temp_directory_path() /
        "server_engine_v310a_failed_construction";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto path = directory / "model.hpp";
    { std::ofstream file(path); file << "#define BROKEN 1\nstruct A { int value; };"; }

    project_configuration configuration;
    configuration.version = 1;
    configuration.name = "FailedConstruction";
    configuration.project.push_back(
        project_item_configuration{path, project_item_role::type});

    project_manager manager;
    diagnostic_buffer diagnostics;
    project_build_result build;
    const auto result = manager.rebuild(
        std::move(configuration), operation_id{1170}, diagnostics, build, 1);

    project_access access;
    const bool pass = !result.ok() &&
        diagnostics.has_errors() &&
        manager.state() == project_lifecycle_state::unloaded &&
        manager.acquire(access).code == status_code::not_found &&
        !access;

    std::filesystem::remove_all(directory, error);
    return pass;
}


template <std::size_t Size>
[[nodiscard]] std::span<const std::byte> baseline_test_bytes(
    const std::array<std::uint8_t, Size>& value) noexcept {
    return std::as_bytes(std::span{value});
}

[[nodiscard]] baseline_fingerprint baseline_test_fingerprint(std::uint8_t seed) noexcept {
    baseline_fingerprint output;
    for (std::size_t index = 0; index < output.bytes.size(); ++index)
        output.bytes[index] = static_cast<std::uint8_t>(seed + index);
    return output;
}

bool test_baseline_store_commit_open() {
    const auto directory = std::filesystem::temp_directory_path() /
        "server_engine_v310b_baseline_commit";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto project_path = directory / "project.json";
    { std::ofstream file(project_path); file << "{}"; }

    const auto fingerprint = baseline_test_fingerprint(11);
    const std::array<std::uint8_t, 4> compiled{1, 2, 3, 4};
    const std::array<std::uint8_t, 3> sources{5, 6, 7};
    const std::array<std::uint8_t, 2> cache{8, 9};

    baseline_store store{project_path};
    baseline_commit_result committed;
    if (!store.commit(
            fingerprint,
            baseline_test_bytes(compiled),
            baseline_test_bytes(sources),
            baseline_test_bytes(cache),
            committed).ok() ||
        committed.transaction.empty()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    baseline_snapshot snapshot;
    const auto result = store.open(fingerprint, snapshot);
    const auto compiled_view = snapshot.artifact(baseline_artifact_kind::compiled);
    const auto source_view = snapshot.artifact(baseline_artifact_kind::source_manager);
    const auto cache_view = snapshot.artifact(baseline_artifact_kind::build_cache);

    const bool pass = result.ok() && snapshot.valid() &&
        snapshot.transaction() == committed.transaction &&
        snapshot.fingerprint() == fingerprint &&
        compiled_view.size() == compiled.size() &&
        source_view.size() == sources.size() &&
        cache_view.size() == cache.size() &&
        std::to_integer<std::uint8_t>(compiled_view[2]) == 3 &&
        std::to_integer<std::uint8_t>(source_view[1]) == 6 &&
        std::to_integer<std::uint8_t>(cache_view[0]) == 8;

    snapshot = {};
    std::filesystem::remove_all(directory, error);
    return pass;
}

bool test_baseline_store_fingerprint_guard() {
    const auto directory = std::filesystem::temp_directory_path() /
        "server_engine_v310b_baseline_fingerprint";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto project_path = directory / "project.json";
    { std::ofstream file(project_path); file << "{}"; }

    const auto fingerprint = baseline_test_fingerprint(21);
    const auto wrong = baseline_test_fingerprint(22);
    const std::array<std::uint8_t, 1> data{42};

    baseline_store store{project_path};
    baseline_commit_result committed;
    if (!store.commit(
            fingerprint,
            baseline_test_bytes(data),
            baseline_test_bytes(data),
            baseline_test_bytes(data),
            committed).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    baseline_snapshot snapshot;
    const bool pass =
        store.open(wrong, snapshot).code == status_code::rebuild_required &&
        !snapshot.valid();

    std::filesystem::remove_all(directory, error);
    return pass;
}

bool test_baseline_store_pinned_gc() {
    const auto directory = std::filesystem::temp_directory_path() /
        "server_engine_v310b_baseline_gc";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto project_path = directory / "project.json";
    { std::ofstream file(project_path); file << "{}"; }

    const auto fingerprint = baseline_test_fingerprint(31);
    const std::array<std::uint8_t, 4> first_data{1, 2, 3, 4};
    const std::array<std::uint8_t, 5> second_data{10, 11, 12, 13, 14};

    baseline_store store{project_path};
    baseline_commit_result first_commit;
    if (!store.commit(
            fingerprint,
            baseline_test_bytes(first_data),
            {},
            {},
            first_commit).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    baseline_snapshot pinned;
    if (!store.open(fingerprint, pinned).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    baseline_commit_result second_commit;
    if (!store.commit(
            fingerprint,
            baseline_test_bytes(second_data),
            {},
            {},
            second_commit).ok() ||
        second_commit.transaction == first_commit.transaction) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    const auto baseline_root = directory / ".serverengine" / "project.json";
    const auto first_path = baseline_root / first_commit.transaction;
    const auto second_path = baseline_root / second_commit.transaction;

    if (!store.collect_garbage(pinned.transaction()).ok() ||
        !std::filesystem::exists(first_path) ||
        !std::filesystem::exists(second_path) ||
        std::to_integer<std::uint8_t>(
            pinned.artifact(baseline_artifact_kind::compiled)[0]) != 1) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    pinned = {};
    if (!store.collect_garbage().ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    baseline_snapshot current;
    const bool pass =
        !std::filesystem::exists(first_path) &&
        std::filesystem::exists(second_path) &&
        store.open(fingerprint, current).ok() &&
        current.transaction() == second_commit.transaction &&
        current.artifact(baseline_artifact_kind::compiled).size() ==
            second_data.size();

    current = {};
    std::filesystem::remove_all(directory, error);
    return pass;
}

bool test_baseline_store_manifest_corruption() {
    const auto directory = std::filesystem::temp_directory_path() /
        "server_engine_v310b_baseline_corrupt";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto project_path = directory / "project.json";
    { std::ofstream file(project_path); file << "{}"; }

    const auto fingerprint = baseline_test_fingerprint(41);
    const std::array<std::uint8_t, 2> data{90, 91};

    baseline_store store{project_path};
    baseline_commit_result committed;
    if (!store.commit(
            fingerprint,
            baseline_test_bytes(data),
            baseline_test_bytes(data),
            baseline_test_bytes(data),
            committed).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    const auto manifest_path =
        directory / ".serverengine" / "project.json" /
        committed.transaction / "manifest.bin";

    {
        std::fstream file(
            manifest_path,
            std::ios::binary | std::ios::in | std::ios::out);
        if (!file) {
            std::filesystem::remove_all(directory, error);
            return false;
        }

        file.seekg(24);
        char value = 0;
        file.read(&value, 1);
        if (!file) {
            std::filesystem::remove_all(directory, error);
            return false;
        }

        value ^= 0x5a;
        file.seekp(24);
        file.write(&value, 1);
        if (!file) {
            std::filesystem::remove_all(directory, error);
            return false;
        }
    }

    baseline_snapshot snapshot;
    const bool pass =
        store.open(fingerprint, snapshot).code == status_code::artifact_corrupt &&
        !snapshot.valid();

    std::filesystem::remove_all(directory, error);
    return pass;
}


[[nodiscard]] bool prepare_source_manager_image_fixture(
    const std::filesystem::path& directory,
    source_manager& manager,
    source_id& root_source,
    source_id& include_source,
    source_id& leaf_source) {

    source_snapshot snapshot;
    if (!manager.publish_memory(
            (directory / "root.hpp").generic_string(),
            "struct Root;",
            snapshot,
            &root_source).ok()) {
        return false;
    }

    if (!manager.publish_memory(
            (directory / "include.hpp").generic_string(),
            "struct Include;",
            snapshot,
            &include_source).ok()) {
        return false;
    }

    if (!manager.publish_memory(
            (directory / "leaf.hpp").generic_string(),
            "struct Leaf;",
            snapshot,
            &leaf_source).ok()) {
        return false;
    }

    auto update = manager.begin_update();
    const std::array root_dependencies{include_source, leaf_source};
    if (!update.set_includes(root_source, root_dependencies).ok())
        return false;

    const std::array include_dependencies{leaf_source};
    if (!update.set_includes(include_source, include_dependencies).ok())
        return false;

    return update.commit().ok();
}

bool test_source_manager_image_roundtrip() {
    const auto directory = std::filesystem::temp_directory_path() /
        "server_engine_v310c_source_manager_image";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    source_manager manager;
    source_id root_source;
    source_id include_source;
    source_id leaf_source;
    if (!prepare_source_manager_image_fixture(
            directory, manager, root_source, include_source, leaf_source)) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    const std::array roots{
        source_manager_image_root{root_source, project_item_role::type},
        source_manager_image_root{include_source, project_item_role::source},
    };

    std::vector<std::byte> image;
    if (!encode_source_manager_image(
            manager, source_manager_image_options{73, roots}, image).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    source_manager_image_view view;
    if (!view.bind(image).ok() || !view.valid() ||
        view.generation() != 73 ||
        view.source_count() != 3 ||
        view.root_count() != 2 ||
        !view.verify_contents().ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    if (view.path(root_source) != manager.path(root_source) ||
        view.path(include_source) != manager.path(include_source) ||
        view.path(leaf_source) != manager.path(leaf_source)) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    const auto root_includes = view.includes(root_source);
    const auto include_includes = view.includes(include_source);
    const auto leaf_dependents = view.dependents(leaf_source);
    if (root_includes.size() != 2 ||
        root_includes[0] != include_source ||
        root_includes[1] != leaf_source ||
        include_includes.size() != 1 ||
        include_includes[0] != leaf_source ||
        leaf_dependents.size() != 2) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    source_manager_image_physical_state physical;
    const auto current = manager.current(root_source);
    if (!view.physical(root_source, physical).ok() ||
        !physical.present ||
        physical.size != current.observation().size ||
        physical.hash != current.hash()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    source_manager_image_root first_root;
    source_manager_image_root second_root;
    if (!view.root(0, first_root).ok() ||
        !view.root(1, second_root).ok() ||
        first_root.source != root_source ||
        first_root.role != project_item_role::type ||
        second_root.source != include_source ||
        second_root.role != project_item_role::source) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    source_id found;
    const bool pass =
        view.find(manager.path(include_source), found).ok() &&
        found == include_source;

    std::filesystem::remove_all(directory, error);
    return pass;
}

bool test_source_manager_image_deterministic() {
    const auto directory = std::filesystem::temp_directory_path() /
        "server_engine_v310c_source_manager_deterministic";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    source_manager manager;
    source_id root_source;
    source_id include_source;
    source_id leaf_source;
    if (!prepare_source_manager_image_fixture(
            directory, manager, root_source, include_source, leaf_source)) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    const std::array roots{
        source_manager_image_root{root_source, project_item_role::type},
    };

    std::vector<std::byte> first;
    std::vector<std::byte> second;
    const bool pass =
        encode_source_manager_image(
            manager, source_manager_image_options{91, roots}, first).ok() &&
        encode_source_manager_image(
            manager, source_manager_image_options{91, roots}, second).ok() &&
        first == second;

    std::filesystem::remove_all(directory, error);
    return pass;
}

bool test_source_manager_image_mapped_baseline() {
    const auto directory = std::filesystem::temp_directory_path() /
        "server_engine_v310c_source_manager_mapped";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto project_path = directory / "project.json";
    { std::ofstream file(project_path); file << "{}"; }

    source_manager manager;
    source_id root_source;
    source_id include_source;
    source_id leaf_source;
    if (!prepare_source_manager_image_fixture(
            directory, manager, root_source, include_source, leaf_source)) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    const std::array roots{
        source_manager_image_root{root_source, project_item_role::type},
    };

    std::vector<std::byte> source_image;
    if (!encode_source_manager_image(
            manager, source_manager_image_options{105, roots}, source_image).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    baseline_store store{project_path};
    baseline_commit_result committed;
    const auto fingerprint = baseline_test_fingerprint(77);
    if (!store.commit(
            fingerprint,
            {},
            std::span<const std::byte>{source_image.data(), source_image.size()},
            {},
            committed).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    baseline_snapshot snapshot;
    if (!store.open(fingerprint, snapshot).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    source_manager_image_view view;
    const auto mapped =
        snapshot.artifact(baseline_artifact_kind::source_manager);
    source_id found;
    const bool pass =
        view.bind(mapped).ok() &&
        view.generation() == 105 &&
        view.source_count() == manager.source_count() &&
        view.find(manager.path(leaf_source), found).ok() &&
        found == leaf_source &&
        view.includes(root_source).size() == 2;

    std::filesystem::remove_all(directory, error);
    return pass;
}

bool test_source_manager_image_integrity() {
    const auto directory = std::filesystem::temp_directory_path() /
        "server_engine_v310c_source_manager_integrity";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    source_manager manager;
    source_id root_source;
    source_id include_source;
    source_id leaf_source;
    if (!prepare_source_manager_image_fixture(
            directory, manager, root_source, include_source, leaf_source)) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    const std::array roots{
        source_manager_image_root{root_source, project_item_role::type},
    };

    std::vector<std::byte> image;
    if (!encode_source_manager_image(
            manager, source_manager_image_options{119, roots}, image).ok() ||
        image.empty()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    source_manager_image_view before;
    if (!before.bind(image).ok() || !before.verify_contents().ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    const auto leaf_path = before.path(leaf_source);
    if (leaf_path.empty()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    const auto* image_begin = image.data();
    const auto* leaf_begin =
        reinterpret_cast<const std::byte*>(leaf_path.data());
    if (leaf_begin < image_begin ||
        leaf_begin >= image_begin + image.size()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    const auto corruption_offset =
        static_cast<std::size_t>(leaf_begin - image_begin);
    image[corruption_offset] ^= std::byte{0x01};

    source_manager_image_view after;
    const bool pass =
        after.bind(image).ok() &&
        after.verify_contents().code == status_code::artifact_corrupt;

    std::filesystem::remove_all(directory, error);
    return pass;
}


[[nodiscard]] bool prepare_compiled_image_fixture(
    const std::filesystem::path& directory,
    std::unique_ptr<project_context>& output) {

    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto model_path = directory / "model.hpp";
    {
        std::ofstream file(model_path);
        file << "namespace N { "
                "struct IO { int IN; int OUT; }; "
                "IO A; IO B; B.IN = A.OUT; "
                "}";
        if (!file)
            return false;
    }

    project_configuration configuration;
    configuration.version = 1;
    configuration.name = "CompiledImage";
    configuration.project.push_back(
        project_item_configuration{model_path, project_item_role::type});

    auto candidate =
        std::make_unique<project_context>(std::move(configuration));
    project_build_orchestrator orchestrator{*candidate, 1};
    diagnostic_buffer diagnostics;
    project_build_result build;

    if (!orchestrator.rebuild(
            operation_id{1200},
            diagnostics,
            build).ok() ||
        diagnostics.has_errors() ||
        !build.changed) {
        return false;
    }

    output = std::move(candidate);
    return true;
}

bool test_compiled_image_roundtrip() {
    static_assert(compiled_image_directory_count == 16);
    static_assert(
        static_cast<std::uint32_t>(
            compiled_image_section::graph_link_index) == 16);

    const auto directory =
        std::filesystem::temp_directory_path() /
        "server_engine_v310d1_compiled_roundtrip";

    std::unique_ptr<project_context> context;
    if (!prepare_compiled_image_fixture(directory, context))
        return false;

    type_handle io;
    object_handle a;
    object_handle b;
    object_endpoint a_out;
    object_endpoint b_in;
    link_handle link;

    if (!context->find_type("N::IO", io).ok() ||
        !context->find_object("N::A", a).ok() ||
        !context->find_object("N::B", b).ok() ||
        !context->find_endpoint("N::A.OUT", a_out).ok() ||
        !context->find_endpoint("N::B.IN", b_in).ok() ||
        !context->find_link("N::B.IN", link).ok()) {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        return false;
    }

    std::vector<std::byte> image;
    if (!encode_compiled_image(*context, image).ok() || image.empty()) {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        return false;
    }

    compiled_image_view view;
    if (!view.bind(image).ok() ||
        !view.verify_contents().ok() ||
        view.type_count() != 1 ||
        view.object_count() != 2 ||
        view.link_count() != 1 ||
        view.identity_count() != context->identity_count()) {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        return false;
    }

    string_id n_name;
    string_id io_name;
    string_id a_name;
    string_id b_name;
    string_id in_name;
    string_id out_name;

    if (!view.find_string("N", n_name).ok() ||
        !view.find_string("IO", io_name).ok() ||
        !view.find_string("A", a_name).ok() ||
        !view.find_string("B", b_name).ok() ||
        !view.find_string("IN", in_name).ok() ||
        !view.find_string("OUT", out_name).ok()) {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        return false;
    }

    identity_ref n_identity;
    identity_ref io_identity;
    identity_ref a_identity;
    identity_ref b_identity;

    const auto root = view.identity_root();
    if (!view.find_identity(
            root,
            n_name,
            identity_kind::namespace_scope,
            n_identity).ok() ||
        !view.find_identity(
            n_identity,
            io_name,
            identity_kind::type,
            io_identity).ok() ||
        !view.find_identity(
            n_identity,
            a_name,
            identity_kind::object,
            a_identity).ok() ||
        !view.find_identity(
            n_identity,
            b_name,
            identity_kind::object,
            b_identity).ok()) {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        return false;
    }

    const auto image_io = view.find_type(io_identity);
    const auto image_a = view.find_object(a_identity);
    const auto image_b = view.find_object(b_identity);

    const auto current_io_identity =
        context->compiled_graph().identity(io);
    const auto current_a_identity =
        context->compiled_graph().identity(a);
    const auto current_b_identity =
        context->compiled_graph().identity(b);

    if (n_name != context->find_string("N") ||
        io_name != context->find_string("IO") ||
        a_name != context->find_string("A") ||
        b_name != context->find_string("B") ||
        in_name != context->find_string("IN") ||
        out_name != context->find_string("OUT") ||
        io_identity != current_io_identity ||
        a_identity != current_a_identity ||
        b_identity != current_b_identity) {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        return false;
    }

    const auto image_in = view.find_member(image_io, in_name);
    const auto image_out = view.find_member(image_io, out_name);
    const object_endpoint image_a_out{image_a, image_out};
    const object_endpoint image_b_in{image_b, image_in};
    const auto image_link = view.find_link(image_b_in);

    compiled_image_object_record b_record;
    type_handle b_type;
    compiled_image_link_record link_record;
    const auto* current_b_record =
        context->compiled_graph().find(b);

    const bool pass =
        image_io == io &&
        image_a == a &&
        image_b == b &&
        image_in == b_in.member &&
        image_out == a_out.member &&
        image_link == link &&
        view.object(image_b, b_record).ok() &&
        current_b_record != nullptr &&
        b_record.type == current_b_record->type &&
        view.named(b_record.type, b_type) &&
        b_type == io &&
        view.link(image_link, link_record).ok() &&
        link_record.source == image_a_out &&
        link_record.target == image_b_in &&
        view.string(io_name) == "IO" &&
        view.identity_name(io_identity) == io_name;

    std::error_code error;
    std::filesystem::remove_all(directory, error);
    return pass;
}

bool test_compiled_image_deterministic() {
    const auto directory =
        std::filesystem::temp_directory_path() /
        "server_engine_v310d1_compiled_deterministic";

    std::unique_ptr<project_context> context;
    if (!prepare_compiled_image_fixture(directory, context))
        return false;

    std::vector<std::byte> first;
    std::vector<std::byte> second;

    const bool pass =
        encode_compiled_image(*context, first).ok() &&
        encode_compiled_image(*context, second).ok() &&
        !first.empty() &&
        first == second;

    std::error_code error;
    std::filesystem::remove_all(directory, error);
    return pass;
}

bool test_compiled_image_mapped_baseline() {
    const auto directory =
        std::filesystem::temp_directory_path() /
        "server_engine_v310d1_compiled_mapped";

    std::unique_ptr<project_context> context;
    if (!prepare_compiled_image_fixture(directory, context))
        return false;

    std::vector<std::byte> image;
    if (!encode_compiled_image(*context, image).ok()) {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        return false;
    }

    const auto project_path = directory / "project.json";
    {
        std::ofstream file(project_path);
        file << "{}";
    }

    baseline_store store{project_path};
    baseline_commit_result committed;
    const auto fingerprint = baseline_test_fingerprint(131);

    if (!store.commit(
            fingerprint,
            std::span<const std::byte>{image.data(), image.size()},
            {},
            {},
            committed).ok()) {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        return false;
    }

    baseline_snapshot snapshot;
    if (!store.open(fingerprint, snapshot).ok()) {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        return false;
    }

    compiled_image_view view;
    const auto mapped =
        snapshot.artifact(baseline_artifact_kind::compiled);

    string_id io_name;
    const bool pass =
        view.bind(mapped).ok() &&
        view.type_count() == 1 &&
        view.object_count() == 2 &&
        view.link_count() == 1 &&
        view.find_string("IO", io_name).ok() &&
        view.string(io_name) == "IO";

    snapshot = {};
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    return pass;
}


bool test_compiled_image_tombstone_preservation() {
    const auto directory =
        std::filesystem::temp_directory_path() /
        "server_engine_v310d1_compiled_tombstone";

    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    const auto model_path = directory / "model.hpp";
    {
        std::ofstream file(model_path);
        file << "struct IO { int value; }; IO A;";
        if (!file) {
            std::filesystem::remove_all(directory, error);
            return false;
        }
    }

    project_configuration configuration;
    configuration.version = 1;
    configuration.name = "CompiledTombstone";
    configuration.project.push_back(
        project_item_configuration{model_path, project_item_role::type});

    project_context context{std::move(configuration)};
    project_build_orchestrator orchestrator{context, 1};
    diagnostic_buffer diagnostics;
    project_build_result full;

    if (!orchestrator.rebuild(
            operation_id{1201},
            diagnostics,
            full).ok() ||
        diagnostics.has_errors()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    object_handle original;
    if (!context.find_object("A", original).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    std::string normalized;
    source_id source;
    if (!normalize_source_path(model_path, normalized).ok() ||
        !context.sources().find(normalized, source).ok()) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    {
        std::ofstream file(model_path, std::ios::trunc);
        file << "struct IO { int value; };";
        if (!file) {
            std::filesystem::remove_all(directory, error);
            return false;
        }
    }

    diagnostics.clear();
    project_build_result update;
    const std::array dirty{source};
    if (!orchestrator.update(
            dirty,
            operation_id{1202},
            diagnostics,
            update).ok() ||
        diagnostics.has_errors() ||
        context.compiled_graph().object_count() != 0 ||
        context.compiled_graph().object_slot_count() != 1) {
        std::filesystem::remove_all(directory, error);
        return false;
    }

    std::vector<std::byte> image;
    compiled_image_view view;

    const bool pass =
        encode_compiled_image(context, image).ok() &&
        view.bind(image).ok() &&
        view.verify_contents().ok() &&
        view.object_count() == 0 &&
        view.object_slot_count() == 1 &&
        !view.object_at(original.value() - 1);

    std::filesystem::remove_all(directory, error);
    return pass;
}

bool test_compiled_image_integrity() {
    const auto directory =
        std::filesystem::temp_directory_path() /
        "server_engine_v310d1_compiled_integrity";

    std::unique_ptr<project_context> context;
    if (!prepare_compiled_image_fixture(directory, context))
        return false;

    std::vector<std::byte> image;
    if (!encode_compiled_image(*context, image).ok() || image.empty()) {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        return false;
    }

    compiled_image_view before;
    if (!before.bind(image).ok() ||
        !before.verify_contents().ok()) {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        return false;
    }

    image.back() ^= std::byte{0x01};

    compiled_image_view after;
    const bool pass =
        after.bind(image).ok() &&
        after.verify_contents().code ==
            status_code::artifact_corrupt;

    std::error_code error;
    std::filesystem::remove_all(directory, error);
    return pass;
}


struct build_cache_fixture final {
    std::unique_ptr<project_context> context;
    std::filesystem::path directory;
    std::filesystem::path root_path;
    std::filesystem::path dependency_path;
    source_id root_source{};
    source_id dependency_source{};
};

[[nodiscard]] bool prepare_build_cache_fixture(
    build_cache_fixture& output) {

    output = {};
    output.directory =
        std::filesystem::temp_directory_path() /
        "server_engine_v310d2_build_cache_fixture";

    std::error_code error;
    std::filesystem::remove_all(output.directory, error);
    std::filesystem::create_directories(output.directory, error);
    if (error)
        return false;

    output.root_path = output.directory / "a.hpp";
    output.dependency_path = output.directory / "b.hpp";

    {
        std::ofstream file(output.dependency_path);
        file << "namespace N { "
                "struct Value {}; "
                "struct IO { Value* IN; Value* OUT; }; "
                "}";
        if (!file)
            return false;
    }

    {
        std::ofstream file(output.root_path);
        file << "#include \"b.hpp\"\n"
                "namespace N { "
                "IO A; IO B; B.IN = A.OUT; "
                "}";
        if (!file)
            return false;
    }

    project_configuration configuration;
    configuration.version = 1;
    configuration.name = "BuildCache";
    configuration.project.push_back(
        project_item_configuration{
            output.root_path,
            project_item_role::type});

    auto context =
        std::make_unique<project_context>(std::move(configuration));
    project_build_orchestrator orchestrator{*context, 2};
    diagnostic_buffer diagnostics;
    project_build_result build;

    if (!orchestrator.rebuild(
            operation_id{1300},
            diagnostics,
            build).ok() ||
        diagnostics.has_errors() ||
        !build.changed ||
        !context->frontend_cache().complete() ||
        !context->contributions().complete() ||
        context->sources().source_count() != 2) {
        return false;
    }

    std::string normalized;
    if (!normalize_source_path(output.root_path, normalized).ok() ||
        !context->sources().find(
            normalized,
            output.root_source).ok()) {
        return false;
    }

    if (!normalize_source_path(
            output.dependency_path,
            normalized).ok() ||
        !context->sources().find(
            normalized,
            output.dependency_source).ok()) {
        return false;
    }

    output.context = std::move(context);
    return true;
}

[[nodiscard]] bool encode_build_cache_correlated_images(
    const build_cache_fixture& fixture,
    std::vector<std::byte>& compiled,
    std::vector<std::byte>& sources,
    std::vector<std::byte>& build_cache) {

    compiled.clear();
    sources.clear();
    build_cache.clear();

    if (!fixture.context ||
        !fixture.root_source ||
        !encode_compiled_image(
            *fixture.context,
            compiled).ok()) {
        return false;
    }

    const std::array roots{
        source_manager_image_root{
            fixture.root_source,
            project_item_role::type},
    };
    source_manager_image_options options;
    options.generation = 1;
    options.roots = roots;

    return encode_source_manager_image(
               fixture.context->sources(),
               options,
               sources).ok() &&
           encode_build_cache_image(
               *fixture.context,
               build_cache).ok();
}

bool test_build_cache_image_roundtrip() {
    static_assert(build_cache_image_directory_count == 25);
    static_assert(
        static_cast<std::uint32_t>(
            build_cache_image_section::graph_dependency_edges) == 20);
    static_assert(
        static_cast<std::uint32_t>(
            build_cache_image_section::graph_link_target_index) == 23);
    static_assert(
        static_cast<std::uint32_t>(
            build_cache_image_section::source_file_identity_index) == 24);
    static_assert(
        static_cast<std::uint32_t>(
            build_cache_image_section::tracked_directory_identity_index) == 25);

    build_cache_fixture fixture;
    if (!prepare_build_cache_fixture(fixture))
        return false;

    source_change_capture change_capture;
    change_capture.checkpoint.backend =
        source_change_backend::windows_usn;
    change_capture.checkpoint.volume_serial = 0x1234u;
    change_capture.checkpoint.journal_id = 0x5678u;
    change_capture.checkpoint.next_usn = 42;

    change_capture.file_index.assign(
        16,
        source_change_file_index_slot{});

    const auto test_mix64 = [](std::uint64_t value) noexcept {
        value ^= value >> 30;
        value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27;
        value *= 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    };

    constexpr std::uint64_t root_file_reference = 101;
    constexpr std::uint64_t dependency_file_reference = 202;
    const auto file_mask =
        change_capture.file_index.size() - 1;

    auto insert_file = [&](std::uint64_t file_reference, source_id source) {
        auto position =
            static_cast<std::size_t>(
                test_mix64(file_reference)) &
            file_mask;

        while (change_capture.file_index[position].file_reference != 0)
            position = (position + 1) & file_mask;

        change_capture.file_index[position] = {
            file_reference,
            source,
            0,
        };
    };

    insert_file(
        root_file_reference,
        fixture.root_source);
    insert_file(
        dependency_file_reference,
        fixture.dependency_source);

    change_capture.directory_index.assign(
        16,
        source_change_directory_index_slot{});

    constexpr std::uint64_t directory_reference = 303;
    const auto directory_position =
        static_cast<std::size_t>(
            test_mix64(directory_reference)) &
        (change_capture.directory_index.size() - 1);
    change_capture.directory_index[directory_position].file_reference =
        directory_reference;
    change_capture.directory_index[directory_position].flags =
        source_change_directory_watch_topology |
        source_change_directory_watch_arrival;

    std::vector<std::byte> image;
    if (!encode_build_cache_image(
            *fixture.context,
            change_capture,
            image).ok() ||
        image.empty()) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    build_cache_image_view view;
    if (!view.bind(image).ok() ||
        !view.verify_contents().ok() ||
        !view.frontend_complete() ||
        !view.contributions_complete() ||
        view.source_count() != 2 ||
        view.frontend_count() != 2) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    const auto dependency_text =
        view.source_text(fixture.dependency_source);
    if (dependency_text.find("struct IO") == std::string_view::npos ||
        dependency_text.find("Value* IN") == std::string_view::npos) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    type_handle value_type;
    type_handle io_type;
    object_handle a_object;
    if (!fixture.context->find_type("N::Value", value_type).ok() ||
        !fixture.context->find_type("N::IO", io_type).ok() ||
        !fixture.context->find_object("N::A", a_object).ok()) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    const auto expected_io_identity =
        fixture.context->compiled_graph().identity(io_type);

    bool found_io = false;
    build_cache_source_record dependency_record;
    if (!view.source(
            fixture.dependency_source,
            dependency_record).ok()) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    for (std::size_t index = 0;
         index < dependency_record.local_types.count;
         ++index) {
        identity_ref identity;
        if (!view.frontend_local_type(
                fixture.dependency_source,
                index,
                identity).ok()) {
            std::error_code error;
            std::filesystem::remove_all(fixture.directory, error);
            return false;
        }
        if (identity == expected_io_identity)
            found_io = true;
    }

    source_contribution_state root_state;
    source_contribution_state dependency_state;
    const auto statistics = view.contribution_statistics();

    const auto* a_record =
        fixture.context->compiled_graph().find(a_object);
    const auto named_io = view.named_ref(io_type);

    const auto checkpoint = view.change_checkpoint();
    type_handle decoded_io;
    const bool pass =
        checkpoint.backend == source_change_backend::windows_usn &&
        checkpoint.volume_serial == 0x1234u &&
        checkpoint.journal_id == 0x5678u &&
        checkpoint.next_usn == 42 &&
        view.find_source_file(root_file_reference) == fixture.root_source &&
        view.find_source_file(dependency_file_reference) ==
            fixture.dependency_source &&
        view.directory_watch_flags(directory_reference) ==
            (source_change_directory_watch_topology |
             source_change_directory_watch_arrival) &&
        found_io &&
        view.contribution_state(
            fixture.root_source,
            root_state).ok() &&
        view.contribution_state(
            fixture.dependency_source,
            dependency_state).ok() &&
        root_state.objects.count == 2 &&
        root_state.links.count == 1 &&
        dependency_state.types.count == 2 &&
        dependency_state.members.count == 2 &&
        statistics.sources == 2 &&
        statistics.type_declarations == 2 &&
        statistics.members == 2 &&
        statistics.objects == 2 &&
        statistics.links == 1 &&
        view.construction_slot_count() ==
            fixture.context->compiled_graph().type_slot_count() &&
        view.dependency_version_count() ==
            fixture.context->compiled_graph().type_slot_count() &&
        view.derived_index_entries() != 0 &&
        view.dependency_edge_count() != 0 &&
        a_record != nullptr &&
        named_io &&
        named_io == a_record->type &&
        fixture.context->compiled_graph().named(
            named_io,
            decoded_io) &&
        decoded_io == io_type;

    std::error_code error;
    std::filesystem::remove_all(fixture.directory, error);
    return pass;
}

bool test_build_cache_image_deterministic() {
    build_cache_fixture fixture;
    if (!prepare_build_cache_fixture(fixture))
        return false;

    std::vector<std::byte> first;
    std::vector<std::byte> second;

    const bool pass =
        encode_build_cache_image(
            *fixture.context,
            first).ok() &&
        encode_build_cache_image(
            *fixture.context,
            second).ok() &&
        !first.empty() &&
        first == second;

    std::error_code error;
    std::filesystem::remove_all(fixture.directory, error);
    return pass;
}

bool test_build_cache_image_cross_artifact() {
    build_cache_fixture fixture;
    if (!prepare_build_cache_fixture(fixture))
        return false;

    std::vector<std::byte> compiled_bytes;
    std::vector<std::byte> source_bytes;
    std::vector<std::byte> cache_bytes;

    if (!encode_build_cache_correlated_images(
            fixture,
            compiled_bytes,
            source_bytes,
            cache_bytes)) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    compiled_image_view compiled;
    source_manager_image_view sources;
    build_cache_image_view cache;

    const bool pass =
        compiled.bind(compiled_bytes).ok() &&
        compiled.verify_contents().ok() &&
        sources.bind(source_bytes).ok() &&
        sources.verify_contents().ok() &&
        cache.bind(cache_bytes).ok() &&
        cache.verify_contents().ok() &&
        cache.verify_against(compiled, sources).ok();

    std::error_code error;
    std::filesystem::remove_all(fixture.directory, error);
    return pass;
}

bool test_build_cache_image_mapped_baseline() {
    build_cache_fixture fixture;
    if (!prepare_build_cache_fixture(fixture))
        return false;

    std::vector<std::byte> compiled_bytes;
    std::vector<std::byte> source_bytes;
    std::vector<std::byte> cache_bytes;

    if (!encode_build_cache_correlated_images(
            fixture,
            compiled_bytes,
            source_bytes,
            cache_bytes)) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    const auto project_path = fixture.directory / "project.json";
    {
        std::ofstream file(project_path);
        file << "{}";
    }

    baseline_store store{project_path};
    baseline_commit_result committed;
    const auto fingerprint = baseline_test_fingerprint(141);

    if (!store.commit(
            fingerprint,
            compiled_bytes,
            source_bytes,
            cache_bytes,
            committed).ok()) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    baseline_snapshot snapshot;
    if (!store.open(fingerprint, snapshot).ok()) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    compiled_image_view compiled;
    source_manager_image_view sources;
    build_cache_image_view cache;

    const bool pass =
        compiled.bind(
            snapshot.artifact(
                baseline_artifact_kind::compiled)).ok() &&
        sources.bind(
            snapshot.artifact(
                baseline_artifact_kind::source_manager)).ok() &&
        cache.bind(
            snapshot.artifact(
                baseline_artifact_kind::build_cache)).ok() &&
        cache.verify_contents().ok() &&
        cache.verify_against(compiled, sources).ok() &&
        cache.source_text(
            fixture.dependency_source).find(
                "Value* OUT") != std::string_view::npos;

    snapshot = {};
    std::error_code error;
    std::filesystem::remove_all(fixture.directory, error);
    return pass;
}

bool test_build_cache_image_incremental_lineage() {
    build_cache_fixture fixture;
    if (!prepare_build_cache_fixture(fixture))
        return false;

    {
        std::ofstream file(
            fixture.dependency_path,
            std::ios::trunc);
        file << "namespace N { "
                "struct Value {}; "
                "struct IO { "
                "Value* IN; Value* OUT; Value* AUX; "
                "}; "
                "}";
        if (!file)
            return false;
    }

    project_build_orchestrator orchestrator{
        *fixture.context,
        2};
    diagnostic_buffer diagnostics;
    project_build_result update;
    const std::array dirty{fixture.dependency_source};

    if (!orchestrator.update(
            dirty,
            operation_id{1301},
            diagnostics,
            update).ok() ||
        diagnostics.has_errors() ||
        !update.changed) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    const auto storage =
        fixture.context->contributions().storage_usage();
    const auto live =
        fixture.context->contributions().statistics();

    std::vector<std::byte> compiled_bytes;
    std::vector<std::byte> source_bytes;
    std::vector<std::byte> cache_bytes;

    if (!encode_build_cache_correlated_images(
            fixture,
            compiled_bytes,
            source_bytes,
            cache_bytes)) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    compiled_image_view compiled;
    source_manager_image_view sources;
    build_cache_image_view cache;

    const bool pass =
        storage.stale_bytes != 0 &&
        compiled.bind(compiled_bytes).ok() &&
        sources.bind(source_bytes).ok() &&
        cache.bind(cache_bytes).ok() &&
        cache.verify_contents().ok() &&
        cache.verify_against(compiled, sources).ok() &&
        cache.source_text(
            fixture.dependency_source).find(
                "Value* AUX") != std::string_view::npos &&
        cache.contribution_statistics().members ==
            live.members &&
        cache.contribution_member_count() >
            cache.contribution_statistics().members;

    std::error_code error;
    std::filesystem::remove_all(fixture.directory, error);
    return pass;
}

bool test_build_cache_image_integrity() {
    build_cache_fixture fixture;
    if (!prepare_build_cache_fixture(fixture))
        return false;

    std::vector<std::byte> image;
    if (!encode_build_cache_image(
            *fixture.context,
            image).ok()) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    build_cache_image_view before;
    if (!before.bind(image).ok() ||
        !before.verify_contents().ok()) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    auto damaged = image;
    damaged[damaged.size() / 2] ^= std::byte{0x01};

    build_cache_image_view after;
    const auto bind_result = after.bind(damaged);
    const bool pass =
        bind_result.code == status_code::artifact_corrupt ||
        (bind_result.ok() &&
         after.verify_contents().code ==
             status_code::artifact_corrupt);

    std::error_code error;
    std::filesystem::remove_all(fixture.directory, error);
    return pass;
}


struct persistent_lifecycle_fixture final {
    std::filesystem::path directory;
    std::filesystem::path configuration_path;
    std::filesystem::path model_path;
};

[[nodiscard]] bool prepare_persistent_lifecycle_fixture(
    std::string_view name,
    persistent_lifecycle_fixture& output) {

    output = {};
    output.directory =
        std::filesystem::temp_directory_path() /
        std::filesystem::path{std::string{name}};
    output.configuration_path =
        output.directory / "project.json";
    output.model_path =
        output.directory / "model.hpp";

    std::error_code error;
    std::filesystem::remove_all(output.directory, error);
    std::filesystem::create_directories(output.directory, error);
    if (error)
        return false;

    {
        std::ofstream file(output.model_path);
        file << "struct IO { int IN; int OUT; }; "
                "IO A; IO B; B.IN = A.OUT;";
        if (!file)
            return false;
    }

    {
        std::ofstream file(output.configuration_path);
        file <<
            R"({"version":1,"name":"Persistent","project":[{"path":"model.hpp","role":"type"}],"configuration":{"abi":{"target":"windows-x64","pack":8}}})";
        if (!file)
            return false;
    }

    return true;
}

[[nodiscard]] bool create_saved_persistent_project(
    const persistent_lifecycle_fixture& fixture,
    baseline_commit_result& committed) {

    project_manager manager;
    diagnostic_buffer diagnostics;
    project_build_result rebuild;

    if (!manager.rebuild(
            fixture.configuration_path,
            operation_id{1400},
            diagnostics,
            rebuild,
            1).ok() ||
        diagnostics.has_errors() ||
        !rebuild.changed ||
        !rebuild.rebuilt) {
        return false;
    }

    project_access before_save;
    if (!manager.acquire(before_save).ok() ||
        !before_save ||
        before_save->baseline_backed()) {
        return false;
    }
    before_save.reset();

    const auto initial_save_result =
        manager.save(committed);

    if (!initial_save_result.ok() ||
        committed.transaction.empty() ||
        committed.bytes_written == 0) {


        return false;
    }

    project_access after_save;
    if (!manager.acquire(after_save).ok() ||
        !after_save ||
        after_save->baseline_backed()) {
        return false;
    }
    after_save.reset();

    return manager.unload().ok();
}

bool test_project_persistence_save_load() {
    persistent_lifecycle_fixture fixture;
    if (!prepare_persistent_lifecycle_fixture(
            "server_engine_v310d3a_save_load",
            fixture)) {
        return false;
    }

    baseline_commit_result committed;
    if (!create_saved_persistent_project(
            fixture,
            committed)) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    project_manager manager;
    diagnostic_buffer diagnostics;
    project_load_result loaded;

    if (!manager.load(
            fixture.configuration_path,
            operation_id{1401},
            diagnostics,
            loaded).ok() ||
        diagnostics.has_errors() ||
        loaded.transaction != committed.transaction ||
        loaded.build_cache_mapped ||
        manager.state() != project_lifecycle_state::ready) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    project_access access;
    object_handle a;
    object_handle b;
    link_handle link;

    const bool pass =
        manager.acquire(access).ok() &&
        access &&
        access->baseline_backed() &&
        !access->build_cache_mapped() &&
        access->baseline_transaction() ==
            committed.transaction &&
        access->sources().source_count() == 1 &&
        access->compiled_graph().type_count() == 1 &&
        access->compiled_graph().object_count() == 2 &&
        access->compiled_graph().link_count() == 1 &&
        access->identity_count() != 0 &&
        access->find_object("A", a).ok() &&
        access->find_object("B", b).ok() &&
        access->find_link("B.IN", link).ok() &&
        a && b && link;

    access.reset();
    const bool unloaded = manager.unload().ok();

    std::error_code error;
    std::filesystem::remove_all(fixture.directory, error);
    return pass && unloaded;
}

bool test_project_load_does_not_map_build_cache() {
    persistent_lifecycle_fixture fixture;
    if (!prepare_persistent_lifecycle_fixture(
            "server_engine_v310d3a_load_no_build_cache",
            fixture)) {
        return false;
    }

    baseline_commit_result committed;
    if (!create_saved_persistent_project(
            fixture,
            committed)) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    const auto build_cache_path =
        fixture.directory /
        ".serverengine" /
        fixture.configuration_path.filename() /
        committed.transaction /
        "build_cache.bin";

    std::error_code error;
    const auto build_cache_exists =
        std::filesystem::exists(
            build_cache_path,
            error);

    if (error ||
        (build_cache_exists &&
         (!std::filesystem::remove(
              build_cache_path,
              error) ||
          error))) {
        std::filesystem::remove_all(
            fixture.directory,
            error);
        return false;
    }

    project_manager manager;
    diagnostic_buffer diagnostics;
    project_load_result loaded;
    const auto result = manager.load(
        fixture.configuration_path,
        operation_id{1402},
        diagnostics,
        loaded);

    project_access access;
    object_handle object;

    const bool pass =
        result.ok() &&
        !diagnostics.has_errors() &&
        !loaded.build_cache_mapped &&
        manager.acquire(access).ok() &&
        access &&
        access->baseline_backed() &&
        !access->build_cache_mapped() &&
        access->find_object("A", object).ok() &&
        object;

    access.reset();
    if (manager.ready())
        (void)manager.unload();

    std::filesystem::remove_all(fixture.directory, error);
    return pass;
}

bool test_project_load_fingerprint_guard() {
    persistent_lifecycle_fixture fixture;
    if (!prepare_persistent_lifecycle_fixture(
            "server_engine_v310d3a_fingerprint",
            fixture)) {
        return false;
    }

    baseline_commit_result committed;
    if (!create_saved_persistent_project(
            fixture,
            committed)) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    {
        std::ofstream file(
            fixture.configuration_path,
            std::ios::trunc);
        file <<
            R"({"version":1,"name":"Different","project":[{"path":"model.hpp","role":"type"}],"configuration":{"abi":{"target":"windows-x64","pack":8}}})";
        if (!file)
            return false;
    }

    project_manager manager;
    diagnostic_buffer diagnostics;
    project_load_result loaded;

    const auto result = manager.load(
        fixture.configuration_path,
        operation_id{1403},
        diagnostics,
        loaded);

    project_access access;
    const bool pass =
        result.code == status_code::rebuild_required &&
        manager.state() == project_lifecycle_state::unloaded &&
        manager.acquire(access).code ==
            status_code::not_found &&
        !access;

    std::error_code error;
    std::filesystem::remove_all(fixture.directory, error);
    return pass;
}

bool test_project_save_after_load_keeps_active_baseline() {
    persistent_lifecycle_fixture fixture;
    if (!prepare_persistent_lifecycle_fixture(
            "server_engine_v310d3a_save_after_load",
            fixture)) {
        return false;
    }

    baseline_commit_result first;
    if (!create_saved_persistent_project(
            fixture,
            first)) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    project_manager manager;
    diagnostic_buffer diagnostics;
    project_load_result loaded;
    if (!manager.load(
            fixture.configuration_path,
            operation_id{1404},
            diagnostics,
            loaded).ok()) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    baseline_commit_result second;
    if (!manager.save(second).ok() ||
        second.transaction.empty() ||
        second.transaction == first.transaction) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    project_access access;
    const bool pass =
        manager.acquire(access).ok() &&
        access &&
        access->baseline_backed() &&
        access->baseline_transaction() ==
            first.transaction &&
        access->baseline_transaction() !=
            second.transaction;

    access.reset();
    if (manager.ready())
        (void)manager.unload();

    std::error_code error;
    std::filesystem::remove_all(fixture.directory, error);
    return pass;
}

bool test_project_build_no_change_reuses_baseline() {
    persistent_lifecycle_fixture fixture;
    if (!prepare_persistent_lifecycle_fixture(
            "server_engine_v310d3a_build_no_change",
            fixture)) {
        return false;
    }

    baseline_commit_result committed;
    if (!create_saved_persistent_project(
            fixture,
            committed)) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    project_manager manager;
    diagnostic_buffer diagnostics;
    project_build_result build;

    const auto result = manager.build(
        fixture.configuration_path,
        operation_id{1405},
        diagnostics,
        build,
        1);

    project_access access;
    object_handle object;

    const bool pass =
        result.ok() &&
        !diagnostics.has_errors() &&
        !build.changed &&
        !build.rebuilt &&
        manager.state() == project_lifecycle_state::ready &&
        manager.acquire(access).ok() &&
        access &&
        access->baseline_backed() &&
        !access->build_cache_mapped() &&
        access->baseline_transaction() ==
            committed.transaction &&
        access->find_object("B", object).ok() &&
        object;

    access.reset();
    if (manager.ready())
        (void)manager.unload();

    std::error_code error;
    std::filesystem::remove_all(fixture.directory, error);
    return pass;
}

bool test_project_build_changed_baseline_sparse_save_load() {
    persistent_lifecycle_fixture fixture;
    if (!prepare_persistent_lifecycle_fixture(
            "server_engine_v310d3b_sparse_save_load",
            fixture)) {
        return false;
    }

    baseline_commit_result baseline_commit;
    if (!create_saved_persistent_project(
            fixture,
            baseline_commit)) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    object_handle initial_a;
    object_handle initial_b;
    link_handle initial_link;

    {
        project_manager manager;
        diagnostic_buffer diagnostics;
        project_load_result loaded;
        if (!manager.load(
                fixture.configuration_path,
                operation_id{1410},
                diagnostics,
                loaded).ok() ||
            diagnostics.has_errors() ||
            loaded.transaction != baseline_commit.transaction) {
            std::error_code error;
            std::filesystem::remove_all(fixture.directory, error);
            return false;
        }

        project_access access;
        const bool queried =
            manager.acquire(access).ok() &&
            access &&
            access->find_object("A", initial_a).ok() &&
            access->find_object("B", initial_b).ok() &&
            access->find_link("B.IN", initial_link).ok() &&
            initial_a && initial_b && initial_link;
        access.reset();
        if (!queried || !manager.unload().ok()) {
            std::error_code error;
            std::filesystem::remove_all(fixture.directory, error);
            return false;
        }
    }

    {
        std::ofstream file(
            fixture.model_path,
            std::ios::trunc);
        file <<
            "struct IO { int IN; int OUT; int AUX; }; "
            "IO A; IO B; B.IN = A.OUT;";
        if (!file)
            return false;
    }

    project_manager manager;
    diagnostic_buffer build_diagnostics;
    project_build_result build;
    const auto build_result = manager.build(
        fixture.configuration_path,
        operation_id{1411},
        build_diagnostics,
        build,
        2);

    project_access sparse;
    object_handle sparse_a;
    object_handle sparse_b;
    link_handle sparse_link;
    object_endpoint aux;

    if (!build_result.ok() ||
        build_diagnostics.has_errors() ||
        !build.changed ||
        build.rebuilt ||
        manager.state() != project_lifecycle_state::ready ||
        !manager.acquire(sparse).ok() ||
        !sparse ||
        !sparse->baseline_backed() ||
        !sparse->build_cache_mapped() ||
        sparse->baseline_transaction() != baseline_commit.transaction ||
        !sparse->find_object("A", sparse_a).ok() ||
        !sparse->find_object("B", sparse_b).ok() ||
        !sparse->find_link("B.IN", sparse_link).ok() ||
        !sparse->find_endpoint("A.AUX", aux).ok() ||
        sparse_a != initial_a ||
        sparse_b != initial_b ||
        sparse_link != initial_link) {
        sparse.reset();
        if (manager.ready())
            (void)manager.unload();
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }
    sparse.reset();

    baseline_commit_result sparse_commit;
    const auto sparse_save_result =
        manager.save(sparse_commit);

    if (!sparse_save_result.ok() ||
        sparse_commit.transaction.empty() ||
        sparse_commit.transaction == baseline_commit.transaction) {


        if (manager.ready())
            (void)manager.unload();
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    project_access after_save;
    if (!manager.acquire(after_save).ok() ||
        !after_save ||
        after_save->baseline_transaction() != baseline_commit.transaction ||
        !after_save->find_endpoint("A.AUX", aux).ok()) {
        after_save.reset();
        if (manager.ready())
            (void)manager.unload();
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }
    after_save.reset();

    if (!manager.unload().ok()) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    diagnostic_buffer load_diagnostics;
    project_load_result loaded;
    const auto load_result = manager.load(
        fixture.configuration_path,
        operation_id{1412},
        load_diagnostics,
        loaded);

    project_access reloaded;
    object_handle reloaded_a;
    object_handle reloaded_b;
    link_handle reloaded_link;

    const bool pass =
        load_result.ok() &&
        !load_diagnostics.has_errors() &&
        loaded.transaction == sparse_commit.transaction &&
        !loaded.build_cache_mapped &&
        manager.acquire(reloaded).ok() &&
        reloaded &&
        reloaded->baseline_backed() &&
        !reloaded->build_cache_mapped() &&
        reloaded->find_object("A", reloaded_a).ok() &&
        reloaded->find_object("B", reloaded_b).ok() &&
        reloaded->find_link("B.IN", reloaded_link).ok() &&
        reloaded->find_endpoint("A.AUX", aux).ok() &&
        reloaded_a == initial_a &&
        reloaded_b == initial_b &&
        reloaded_link == initial_link;

    reloaded.reset();
    if (manager.ready())
        (void)manager.unload();

    std::error_code error;
    std::filesystem::remove_all(fixture.directory, error);
    return pass;
}

bool test_project_build_link_tombstone_handle_restore() {
    persistent_lifecycle_fixture fixture;
    if (!prepare_persistent_lifecycle_fixture(
            "server_engine_v310d3b_link_tombstone",
            fixture)) {
        return false;
    }

    baseline_commit_result initial_commit;
    if (!create_saved_persistent_project(
            fixture,
            initial_commit)) {
        std::error_code error;
        std::filesystem::remove_all(fixture.directory, error);
        return false;
    }

    link_handle original_link;
    {
        project_manager manager;
        diagnostic_buffer diagnostics;
        project_load_result loaded;
        if (!manager.load(
                fixture.configuration_path,
                operation_id{1420},
                diagnostics,
                loaded).ok()) {
            std::error_code error;
            std::filesystem::remove_all(fixture.directory, error);
            return false;
        }

        project_access access;
        const bool queried =
            manager.acquire(access).ok() &&
            access &&
            access->find_link("B.IN", original_link).ok() &&
            original_link;
        access.reset();
        if (!queried || !manager.unload().ok()) {
            std::error_code error;
            std::filesystem::remove_all(fixture.directory, error);
            return false;
        }
    }

    {
        std::ofstream file(
            fixture.model_path,
            std::ios::trunc);
        file <<
            "struct IO { int IN; int OUT; }; "
            "IO A; IO B;";
        if (!file)
            return false;
    }

    baseline_commit_result tombstone_commit;
    {
        project_manager manager;
        diagnostic_buffer diagnostics;
        project_build_result build;
        const auto tombstone_build_result =
            manager.build(
                fixture.configuration_path,
                operation_id{1421},
                diagnostics,
                build,
                1);

        if (!tombstone_build_result.ok() ||
            diagnostics.has_errors() ||
            !build.changed ||
            build.rebuilt) {


            std::error_code error;
            std::filesystem::remove_all(fixture.directory, error);
            return false;
        }

        project_access access;
        link_handle removed;
        if (!manager.acquire(access).ok() ||
            !access ||
            access->compiled_graph().link_count() != 0 ||
            access->find_link("B.IN", removed).code != status_code::not_found ||
            removed) {
            access.reset();
            if (manager.ready())
                (void)manager.unload();
            std::error_code error;
            std::filesystem::remove_all(fixture.directory, error);
            return false;
        }
        access.reset();

        if (!manager.save(tombstone_commit).ok() ||
            tombstone_commit.transaction.empty() ||
            tombstone_commit.transaction == initial_commit.transaction ||
            !manager.unload().ok()) {
            std::error_code error;
            std::filesystem::remove_all(fixture.directory, error);
            return false;
        }
    }

    {
        std::ofstream file(
            fixture.model_path,
            std::ios::trunc);
        file <<
            "struct IO { int IN; int OUT; }; "
            "IO A; IO B; B.IN = A.OUT;";
        if (!file)
            return false;
    }

    project_manager manager;
    diagnostic_buffer diagnostics;
    project_build_result build;
    const auto result = manager.build(
        fixture.configuration_path,
        operation_id{1422},
        diagnostics,
        build,
        1);

    project_access access;
    link_handle restored_link;
    const bool pass =
        result.ok() &&
        !diagnostics.has_errors() &&
        build.changed &&
        !build.rebuilt &&
        manager.acquire(access).ok() &&
        access &&
        access->baseline_backed() &&
        access->build_cache_mapped() &&
        access->baseline_transaction() == tombstone_commit.transaction &&
        access->find_link("B.IN", restored_link).ok() &&
        restored_link == original_link;

    access.reset();
    if (manager.ready())
        (void)manager.unload();

    std::error_code error;
    std::filesystem::remove_all(fixture.directory, error);
    return pass;
}

bool test_project_build_without_baseline_full_no_save() {
    persistent_lifecycle_fixture fixture;
    if (!prepare_persistent_lifecycle_fixture(
            "server_engine_v310d3a_build_full",
            fixture)) {
        return false;
    }

    project_manager manager;
    diagnostic_buffer diagnostics;
    project_build_result build;

    const auto result = manager.build(
        fixture.configuration_path,
        operation_id{1407},
        diagnostics,
        build,
        1);

    const auto current_path =
        fixture.directory /
        ".serverengine" /
        fixture.configuration_path.filename() /
        "CURRENT";

    project_access access;
    object_handle object;

    std::error_code exists_error;
    const auto current_exists =
        std::filesystem::exists(
            current_path,
            exists_error);

    const bool pass =
        result.ok() &&
        !diagnostics.has_errors() &&
        build.changed &&
        !build.rebuilt &&
        manager.state() == project_lifecycle_state::ready &&
        manager.acquire(access).ok() &&
        access &&
        !access->baseline_backed() &&
        access->find_object("A", object).ok() &&
        object &&
        !exists_error &&
        !current_exists;

    access.reset();
    if (manager.ready())
        (void)manager.unload();

    std::error_code error;
    std::filesystem::remove_all(fixture.directory, error);
    return pass;
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
    test_case{"project_identity_compact_reference", &test_project_identity_compact_reference},
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
    test_case{"frontend_parallel_failure_safe", &test_frontend_parallel_failure_safe},
    test_case{"frontend_transitive_include_pipeline", &test_frontend_transitive_include_pipeline},
    test_case{"frontend_shared_include_identity", &test_frontend_shared_include_identity},
    test_case{"frontend_unsupported_directive", &test_frontend_unsupported_directive},
    test_case{"frontend_include_inside_scope", &test_frontend_include_inside_scope},
    test_case{"source_contribution_capture", &test_source_contribution_capture},
    test_case{"generation_builder_g0", &test_generation_builder_g0},
    test_case{"generation_builder_enum", &test_generation_builder_enum},
    test_case{"generation_builder_redeclaration", &test_generation_builder_redeclaration},
    test_case{"generation_builder_definition_conflict", &test_generation_builder_definition_conflict},
    test_case{"generation_builder_incremental_modify", &test_generation_builder_incremental_modify},
    test_case{"generation_builder_incremental_remove_add", &test_generation_builder_incremental_remove_add},
    test_case{"generation_builder_incremental_dangling_guard", &test_generation_builder_incremental_dangling_guard},
    test_case{"generation_builder_incremental_conflict_rollback", &test_generation_builder_incremental_conflict_rollback},
    test_case{"project_build_orchestrator_full", &test_project_build_orchestrator_full},
    test_case{"project_build_orchestrator_incremental", &test_project_build_orchestrator_incremental},
    test_case{"project_build_orchestrator_no_change", &test_project_build_orchestrator_no_change},
    test_case{"project_build_orchestrator_failure_rollback", &test_project_build_orchestrator_failure_rollback},
    test_case{"project_build_orchestrator_new_include", &test_project_build_orchestrator_new_include},
    test_case{"project_build_orchestrator_remove_leaf", &test_project_build_orchestrator_remove_leaf},
    test_case{"project_build_orchestrator_builder_conflict_rollback", &test_project_build_orchestrator_builder_conflict_rollback},
    test_case{"project_build_orchestrator_semantic_noop", &test_project_build_orchestrator_semantic_noop},
    test_case{"project_build_orchestrator_reverse_edge_replacement", &test_project_build_orchestrator_reverse_edge_replacement},
    test_case{"string_table_canonicalization", &test_string_table_canonicalization},
    test_case{"complete_graph_objects_links_query", &test_complete_graph_objects_links_query},
    test_case{"complete_graph_incremental_link_retarget", &test_complete_graph_incremental_link_retarget},
    test_case{"complete_graph_object_reactivation", &test_complete_graph_object_reactivation},
    test_case{"complete_graph_duplicate_new_link_rollback", &test_complete_graph_duplicate_new_link_rollback},
    test_case{"project_manager_rebuild_ready_unload", &test_project_manager_rebuild_ready_unload},
    test_case{"project_access_move_only", &test_project_access_move_only},
    test_case{"project_unload_stop_before_wait", &test_project_unload_stop_before_wait},
    test_case{"project_rebuild_failure_returns_unloaded", &test_project_rebuild_failure_returns_unloaded},
    test_case{"baseline_store_commit_open", &test_baseline_store_commit_open},
    test_case{"baseline_store_fingerprint_guard", &test_baseline_store_fingerprint_guard},
    test_case{"baseline_store_pinned_gc", &test_baseline_store_pinned_gc},
    test_case{"baseline_store_manifest_corruption", &test_baseline_store_manifest_corruption},
    test_case{"source_manager_image_roundtrip", &test_source_manager_image_roundtrip},
    test_case{"source_manager_image_deterministic", &test_source_manager_image_deterministic},
    test_case{"source_manager_image_mapped_baseline", &test_source_manager_image_mapped_baseline},
    test_case{"source_manager_image_integrity", &test_source_manager_image_integrity},
    test_case{"compiled_image_roundtrip", &test_compiled_image_roundtrip},
    test_case{"compiled_image_deterministic", &test_compiled_image_deterministic},
    test_case{"compiled_image_mapped_baseline", &test_compiled_image_mapped_baseline},
    test_case{"compiled_image_tombstone_preservation", &test_compiled_image_tombstone_preservation},
    test_case{"compiled_image_integrity", &test_compiled_image_integrity},
    test_case{"build_cache_image_roundtrip", &test_build_cache_image_roundtrip},
    test_case{"build_cache_image_deterministic", &test_build_cache_image_deterministic},
    test_case{"build_cache_image_cross_artifact", &test_build_cache_image_cross_artifact},
    test_case{"build_cache_image_mapped_baseline", &test_build_cache_image_mapped_baseline},
    test_case{"build_cache_image_incremental_lineage", &test_build_cache_image_incremental_lineage},
    test_case{"build_cache_image_integrity", &test_build_cache_image_integrity},
    test_case{"project_persistence_save_load", &test_project_persistence_save_load},
    test_case{"project_load_does_not_map_build_cache", &test_project_load_does_not_map_build_cache},
    test_case{"project_load_fingerprint_guard", &test_project_load_fingerprint_guard},
    test_case{"project_save_after_load_keeps_active_baseline", &test_project_save_after_load_keeps_active_baseline},
    test_case{"project_build_no_change_reuses_baseline", &test_project_build_no_change_reuses_baseline},
    test_case{"project_build_changed_baseline_sparse_save_load", &test_project_build_changed_baseline_sparse_save_load},
    test_case{"project_build_link_tombstone_handle_restore", &test_project_build_link_tombstone_handle_restore},
    test_case{"project_build_without_baseline_full_no_save", &test_project_build_without_baseline_full_no_save},
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
