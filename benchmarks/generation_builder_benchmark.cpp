#include "../server_engine/project/builder/generation_builder.hpp"
#include "../server_engine/project/project_context.hpp"
#include "../server_engine/diagnostics/diagnostic_buffer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace cw::server;
using clock_type = std::chrono::steady_clock;

struct synthetic_source final {
    std::string text;
    std::vector<source_record_fact> records;
    std::vector<source_member_fact> members;
    std::vector<source_type_modifier> modifiers;
    std::vector<source_declaration_ref> declarations;

    [[nodiscard]] source_facts facts() const noexcept {
        return source_facts{
            source_id{1}, text,
            std::span<const source_namespace_fact>{}, records, members, modifiers,
            std::span<const source_enum_fact>{},
            std::span<const source_enum_value_fact>{}, declarations};
    }
};

struct run_result final {
    double prepare_ms = 0.0;
    double publish_us = 0.0;
    generation_build_telemetry telemetry{};
    bool pass = false;
};

[[nodiscard]] double milliseconds(clock_type::duration value) noexcept {
    return std::chrono::duration<double, std::milli>(value).count();
}

[[nodiscard]] bool make_identity(
    project_context& context,
    std::size_t index,
    identity_ref& output) {

    const auto name = std::string{"Type_"} + std::to_string(index);
    return context.resolve_declaration(
        context.identity_root(), name, identity_kind::type, output).ok();
}

[[nodiscard]] bool make_defined_empty(
    std::size_t count,
    project_context& context,
    synthetic_source& output) {

    if (count == 0 || count > (std::numeric_limits<std::uint32_t>::max)())
        return false;

    try {
        output.text.assign(count, 'x');
        output.records.reserve(count);
        output.declarations.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            identity_ref identity = nullptr;
            if (!make_identity(context, index, identity))
                return false;
            const source_span declaration{
                static_cast<std::uint32_t>(index), 1};
            output.records.push_back(source_record_fact{
                identity,
                {}, {}, {},
                declaration,
                source_record_declaration_kind::definition,
                source_record_kind::struct_type});
            output.declarations.push_back(source_declaration_ref{
                static_cast<std::uint32_t>(index),
                source_declaration_kind::record_type,
                declaration});
        }
        return true;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool make_intrinsic_member_types(
    std::size_t count,
    project_context& context,
    synthetic_source& output) {

    if (count == 0 || count > (std::numeric_limits<std::uint32_t>::max)() / 4)
        return false;

    try {
        string_id member_name;
        if (!context.intern_string("x", member_name).ok())
            return false;
        output.text.assign(count * 4, 'x');
        output.records.reserve(count);
        output.members.reserve(count);
        output.declarations.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            identity_ref identity = nullptr;
            if (!make_identity(context, index, identity))
                return false;
            const auto base = static_cast<std::uint32_t>(index * 4);
            const source_span declaration{base, 4};
            output.members.push_back(source_member_fact{
                source_type_ref::builtin(
                    intrinsic_type::signed_int,
                    source_fact_range{0, 0},
                    source_span{base, 1}),
                member_name,
                source_span{base, 4},
                source_member_access::public_access});
            output.records.push_back(source_record_fact{
                identity,
                source_fact_range{static_cast<std::uint32_t>(index), 1}, {}, {},
                declaration,
                source_record_declaration_kind::definition,
                source_record_kind::struct_type});
            output.declarations.push_back(source_declaration_ref{
                static_cast<std::uint32_t>(index),
                source_declaration_kind::record_type,
                declaration});
        }
        return true;
    } catch (...) {
        return false;
    }
}


[[nodiscard]] bool make_semantic_pointer_types(
    std::size_t count,
    project_context& context,
    synthetic_source& output) {

    if (count == 0 || count > (std::numeric_limits<std::uint32_t>::max)() / 4)
        return false;

    try {
        string_id member_name;
        if (!context.intern_string("x", member_name).ok())
            return false;
        output.text.assign(count * 4, 'x');
        output.records.reserve(count);
        output.members.reserve(count);
        output.modifiers.reserve(count);
        output.declarations.reserve(count);
        std::vector<identity_ref> identities;
        identities.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            identity_ref identity = nullptr;
            if (!make_identity(context, index, identity))
                return false;
            identities.push_back(identity);
        }

        for (std::size_t index = 0; index < count; ++index) {
            const auto base = static_cast<std::uint32_t>(index * 4);
            const source_span declaration{base, 4};
            output.modifiers.push_back(source_type_modifier{0, source_type_modifier_kind::pointer});
            output.members.push_back(source_member_fact{
                source_type_ref::semantic(
                    identities[index],
                    source_fact_range{static_cast<std::uint32_t>(index), 1},
                    source_span{base, 1}),
                member_name,
                source_span{base, 4},
                source_member_access::public_access});
            output.records.push_back(source_record_fact{
                identities[index],
                source_fact_range{static_cast<std::uint32_t>(index), 1}, {}, {},
                declaration,
                source_record_declaration_kind::definition,
                source_record_kind::struct_type});
            output.declarations.push_back(source_declaration_ref{
                static_cast<std::uint32_t>(index),
                source_declaration_kind::record_type,
                declaration});
        }
        return true;
    } catch (...) {
        return false;
    }
}

run_result run_once(const source_facts& facts, std::size_t expected_types) {
    source_contribution_cache cache;
    graph graph_value;
    generation_builder builder{cache, graph_value};
    diagnostic_buffer diagnostics;

    const auto begin = clock_type::now();
    const auto result = builder.prepare_g0(
        std::span<const source_facts>{&facts, 1}, {}, operation_id{1000}, diagnostics);
    const auto prepared = clock_type::now();
    if (!result.ok() || diagnostics.has_errors())
        return {};

    builder.publish_prepared();
    const auto published = clock_type::now();

    run_result output;
    output.prepare_ms = milliseconds(prepared - begin);
    output.publish_us =
        std::chrono::duration<double, std::micro>(published - prepared).count();
    output.telemetry = builder.telemetry();
    output.pass = graph_value.type_count() == expected_types &&
        cache.statistics().type_declarations == expected_types &&
        builder.published();
    return output;
}

run_result median_run(const source_facts& facts, std::size_t expected_types, int samples) {
    std::vector<run_result> values;
    values.reserve(static_cast<std::size_t>(samples));
    for (int sample = 0; sample < samples; ++sample) {
        auto value = run_once(facts, expected_types);
        if (!value.pass)
            return {};
        values.push_back(value);
    }
    std::ranges::sort(values, {}, &run_result::prepare_ms);
    return values[values.size() / 2];
}

void print_result(std::size_t count, std::string_view scenario, const run_result& value) {
    const auto ns_per_type = value.prepare_ms * 1'000'000.0 / static_cast<double>(count);
    const auto& t = value.telemetry;
    std::cout << count << ',' << scenario << ','
              << value.prepare_ms << ',' << value.publish_us << ',' << ns_per_type << ','
              << static_cast<double>(t.contribution_capture_ns) / 1'000'000.0 << ','
              << static_cast<double>(t.identity_to_handle_ns) / 1'000'000.0 << ','
              << static_cast<double>(t.type_ref_materialization_ns) / 1'000'000.0 << ','
              << static_cast<double>(t.definition_materialization_ns) / 1'000'000.0 << ','
              << static_cast<double>(t.validation_ns) / 1'000'000.0 << ','
              << static_cast<double>(t.prepare_publish_ns) / 1'000'000.0 << ','
              << t.unique_types << ',' << t.members << ',' << t.canonical_type_refs << ','
              << t.derived_type_refs << ',' << (value.pass ? "PASS" : "FAIL") << '\n';
}

[[nodiscard]] run_result run_defined_empty(std::size_t count) {
    project_configuration configuration;
    project_context context{std::move(configuration)};
    synthetic_source source;
    if (!make_defined_empty(count, context, source))
        return {};
    const auto facts = source.facts();
    return median_run(facts, count, count >= 100000 ? 3 : 5);
}

[[nodiscard]] run_result run_intrinsic_members(std::size_t count) {
    project_configuration configuration;
    project_context context{std::move(configuration)};
    synthetic_source source;
    if (!make_intrinsic_member_types(count, context, source))
        return {};
    const auto facts = source.facts();
    return median_run(facts, count, count >= 100000 ? 3 : 5);
}


[[nodiscard]] run_result run_semantic_pointers(std::size_t count) {
    project_configuration configuration;
    project_context context{std::move(configuration)};
    synthetic_source source;
    if (!make_semantic_pointer_types(count, context, source))
        return {};
    const auto facts = source.facts();
    return median_run(facts, count, count >= 100000 ? 3 : 5);
}

} // namespace


struct incremental_fixture final {
    project_configuration configuration;
    project_context context{std::move(configuration)};
    std::string text{"x"};
    std::vector<identity_ref> identities;
    std::vector<source_record_fact> records;
    std::vector<source_facts> sources;

    incremental_fixture() = default;
    incremental_fixture(const incremental_fixture&) = delete;
    incremental_fixture& operator=(const incremental_fixture&) = delete;
};

[[nodiscard]] bool make_incremental_fixture(std::size_t count, incremental_fixture& output) {
    if (count == 0 || count >= (std::numeric_limits<std::uint32_t>::max)())
        return false;
    try {
        output.identities.reserve(count);
        output.records.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            identity_ref identity = nullptr;
            if (!make_identity(output.context, index, identity))
                return false;
            output.identities.push_back(identity);
            output.records.push_back(source_record_fact{
                identity, {}, {}, {}, {0, 1}, source_record_declaration_kind::definition,
                source_record_kind::struct_type});
        }
        output.sources.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            output.sources.emplace_back(
                source_id{static_cast<std::uint32_t>(index + 1)}, output.text,
                std::span<const source_namespace_fact>{},
                std::span<const source_record_fact>{&output.records[index], 1},
                std::span<const source_member_fact>{},
                std::span<const source_type_modifier>{},
                std::span<const source_enum_fact>{},
                std::span<const source_enum_value_fact>{},
                std::span<const source_declaration_ref>{});
        }
        return true;
    } catch (...) {
        return false;
    }
}

struct incremental_result final {
    double prepare_us = 0.0;
    double publish_us = 0.0;
    generation_build_telemetry telemetry{};
    bool pass = false;
};

[[nodiscard]] incremental_result run_incremental_once(
    generation_builder& builder,
    std::span<const source_facts> replacements,
    std::span<const source_id> removals,
    graph& graph_value,
    std::size_t expected_live,
    operation_id operation) {

    diagnostic_buffer diagnostics;
    const auto begin = clock_type::now();
    const auto result = builder.prepare_incremental(replacements, removals, {}, operation, diagnostics);
    const auto prepared = clock_type::now();
    if (!result.ok() || diagnostics.has_errors())
        return {};
    builder.publish_prepared();
    const auto published = clock_type::now();

    incremental_result output;
    output.prepare_us = std::chrono::duration<double, std::micro>(prepared - begin).count();
    output.publish_us = std::chrono::duration<double, std::micro>(published - prepared).count();
    output.telemetry = builder.telemetry();
    output.pass = builder.published() && graph_value.type_count() == expected_live;
    return output;
}

void print_incremental(std::size_t total, std::string_view scenario, const incremental_result& value) {
    const auto& t = value.telemetry;
    std::cout << total << ',' << scenario << ','
              << value.prepare_us << ',' << value.publish_us << ','
              << t.changed_sources << ',' << t.changed_types << ','
              << t.added_types << ',' << t.removed_types << ','
              << t.validation_visited_types << ',' << t.validation_visited_type_refs << ','
              << t.validation_dependency_edges << ',' << t.graph_full_scans << ','
              << t.contribution_full_scans << ',' << (value.pass ? "PASS" : "FAIL") << '\n';
}

[[nodiscard]] bool run_incremental_gate(std::size_t count) {
    incremental_fixture fixture;
    if (!make_incremental_fixture(count, fixture))
        return false;

    source_contribution_cache cache;
    graph graph_value;
    diagnostic_buffer diagnostics;
    generation_builder g0{cache, graph_value};
    if (!g0.prepare_g0(fixture.sources, {}, operation_id{2000}, diagnostics).ok() || diagnostics.has_errors())
        return false;
    g0.publish_prepared();
    if (graph_value.type_count() != count)
        return false;

    const auto target_index = count / 2;
    const auto target_source = source_id{static_cast<std::uint32_t>(target_index + 1)};
    const auto target_identity = fixture.identities[target_index];
    const auto original_handle = graph_value.type_at(target_index);
    if (!original_handle)
        return false;

    constexpr std::string_view member_text = "T* x;";
    string_id member_name;
    if (!fixture.context.intern_string("x", member_name).ok())
        return false;
    const std::array modifiers{
        source_type_modifier{0, source_type_modifier_kind::pointer},
    };
    const std::array members{
        source_member_fact{
            source_type_ref::semantic(target_identity, {0, 1}, {0, 2}),
            member_name, {0, 5}, source_member_access::public_access},
    };
    const std::array records{
        source_record_fact{target_identity, {0, 1}, {}, {}, {0, 5},
            source_record_declaration_kind::definition, source_record_kind::struct_type},
    };
    const source_facts modified{
        target_source, member_text, std::span<const source_namespace_fact>{}, records, members, modifiers,
        std::span<const source_enum_fact>{}, std::span<const source_enum_value_fact>{},
        std::span<const source_declaration_ref>{}};
    const std::array modify_replacements{modified};

    generation_builder g1{cache, graph_value};
    const auto modify = run_incremental_once(
        g1, modify_replacements, {}, graph_value, count, operation_id{2001});
    print_incremental(count, "g1_modify_one", modify);
    if (!modify.pass || graph_value.type_at(target_index) != original_handle ||
        graph_value.members(original_handle).size() != 1)
        return false;

    const std::array removals{target_source};
    generation_builder g2{cache, graph_value};
    const auto remove = run_incremental_once(
        g2, {}, removals, graph_value, count - 1, operation_id{2002});
    print_incremental(count, "g2_remove_one", remove);
    if (!remove.pass || graph_value.type_at(target_index))
        return false;

    const source_facts added{
        target_source, fixture.text, std::span<const source_namespace_fact>{},
        std::span<const source_record_fact>{&fixture.records[target_index], 1},
        std::span<const source_member_fact>{}, std::span<const source_type_modifier>{},
        std::span<const source_enum_fact>{}, std::span<const source_enum_value_fact>{},
        std::span<const source_declaration_ref>{}};
    const std::array add_replacements{added};
    generation_builder g3{cache, graph_value};
    const auto add = run_incremental_once(
        g3, add_replacements, {}, graph_value, count, operation_id{2003});
    print_incremental(count, "g3_add_one", add);
    if (!add.pass || graph_value.type_at(target_index) != original_handle)
        return false;

    constexpr double prepare_target_us = 250.0;
    constexpr double publish_target_us = 5.0;
    const auto sparse_gate = [&](const incremental_result& value) {
        return value.pass && value.prepare_us <= prepare_target_us &&
            value.publish_us <= publish_target_us &&
            value.telemetry.changed_sources == 1 && value.telemetry.changed_types == 1 &&
            value.telemetry.graph_full_scans == 0 &&
            value.telemetry.contribution_full_scans == 0;
    };
    const bool gate = sparse_gate(modify) && sparse_gate(remove) && sparse_gate(add);
    std::cout << "INCREMENTAL_1M_GATE," << (gate ? "PASS" : "FAIL")
              << ",prepare_us<=250,publish_us<=5,changed=1,full_scans=0\n";
    return gate;
}

int main(int argc, char** argv) {
    std::cout << std::fixed << std::setprecision(6);
    if (argc == 2 && std::string_view{argv[1]} == "--incremental-gate") {
        std::cout << "total_types,scenario,prepare_us,publish_us,changed_sources,changed_types,added_types,removed_types,validation_visited_types,validation_visited_type_refs,validation_dependency_edges,graph_full_scans,contribution_full_scans,status\n";
        return run_incremental_gate(1000000) ? 0 : 1;
    }

    std::cout << "count,scenario,prepare_ms,publish_us,ns_per_type,contribution_capture_ms,"
                 "identity_to_handle_ms,type_ref_materialization_ms,definition_materialization_ms,"
                 "validation_ms,prepare_publish_ms,unique_types,members,canonical_type_refs,derived_type_refs,status\n";

    if (argc == 2 && std::string_view{argv[1]} == "--gate") {
        constexpr std::size_t count = 1000000;
        constexpr double target_ms = 1000.0;
        const auto value = run_defined_empty(count);
        print_result(count, "g0_defined_empty", value);
        const bool gate = value.pass && value.prepare_ms <= target_ms;
        std::cout << "G0_1M_PREPARE_GATE," << (gate ? "PASS" : "FAIL")
                  << ",prepare_ms=" << value.prepare_ms << " <= " << target_ms << '\n';
        std::cout << "GENERATION_BUILDER_G0_GATE," << (gate ? "PASS" : "FAIL") << '\n';
        return gate ? 0 : 1;
    }

    if (argc == 3 && std::string_view{argv[1]} == "--semantic") {
        const auto count = std::strtoull(argv[2], nullptr, 10);
        if (count == 0 || count > (std::numeric_limits<std::uint32_t>::max)())
            return 2;
        const auto value = run_semantic_pointers(static_cast<std::size_t>(count));
        print_result(static_cast<std::size_t>(count), "g0_self_pointer_member", value);
        return value.pass ? 0 : 1;
    }

    if (argc == 2 && std::string_view{argv[1]} != "--gate") {
        const auto count = std::strtoull(argv[1], nullptr, 10);
        if (count == 0 || count > (std::numeric_limits<std::uint32_t>::max)())
            return 2;
        const auto value = run_defined_empty(static_cast<std::size_t>(count));
        print_result(static_cast<std::size_t>(count), "g0_defined_empty", value);
        return value.pass ? 0 : 1;
    }

    constexpr std::size_t counts[] = {128, 1024, 8192, 32768, 100000, 1000000};
    run_result million;
    for (const auto count : counts) {
        const auto value = run_defined_empty(count);
        print_result(count, "g0_defined_empty", value);
        if (!value.pass)
            return 1;
        if (count == 1000000)
            million = value;
    }

    constexpr std::size_t member_counts[] = {128, 1024, 8192, 32768, 100000};
    for (const auto count : member_counts) {
        const auto value = run_intrinsic_members(count);
        print_result(count, "g0_one_intrinsic_member", value);
        if (!value.pass)
            return 1;
    }

    constexpr std::size_t semantic_counts[] = {128, 1024, 8192, 32768, 100000};
    for (const auto count : semantic_counts) {
        const auto value = run_semantic_pointers(count);
        print_result(count, "g0_self_pointer_member", value);
        if (!value.pass)
            return 1;
    }

    constexpr double target_ms = 1000.0;
    const bool gate = million.pass && million.prepare_ms <= target_ms;
    std::cout << "G0_1M_PREPARE_GATE," << (gate ? "PASS" : "FAIL")
              << ",prepare_ms=" << million.prepare_ms << " <= " << target_ms << '\n';
    std::cout << "GENERATION_BUILDER_G0_GATE," << (gate ? "PASS" : "FAIL") << '\n';
    return gate ? 0 : 1;
}
