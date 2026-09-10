#include "../server_engine/project/builder/generation_builder.hpp"
#include "../server_engine/project/project_context.hpp"
#include "../server_engine/diagnostics/diagnostic_buffer.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace cw::server;
using clock_type = std::chrono::steady_clock;

struct full_fixture final {
    project_configuration configuration;
    project_context context{std::move(configuration)};
    std::string text{"x"};
    identity_ref type_identity = nullptr;
    string_id in_name{};
    string_id out_name{};
    std::array<source_member_fact, 2> members{};
    std::array<source_record_fact, 1> records{};
    std::vector<identity_ref> object_identities;
    std::vector<source_object_fact> objects;
    std::vector<source_link_fact> links;
};

[[nodiscard]] bool build_full_fixture(std::size_t count, full_fixture& fixture) {
    if (count == 0 || count >= (std::numeric_limits<std::uint32_t>::max)())
        return false;
    if (!fixture.context.resolve_declaration(
            fixture.context.identity_root(), "IO", identity_kind::type, fixture.type_identity).ok() ||
        !fixture.context.intern_string("IN", fixture.in_name).ok() ||
        !fixture.context.intern_string("OUT", fixture.out_name).ok()) {
        return false;
    }

    fixture.members[0] = source_member_fact{
        source_type_ref::builtin(intrinsic_type::signed_int, {}, {0, 1}),
        fixture.in_name, {0, 1}, source_member_access::public_access};
    fixture.members[1] = source_member_fact{
        source_type_ref::builtin(intrinsic_type::signed_int, {}, {0, 1}),
        fixture.out_name, {0, 1}, source_member_access::public_access};
    fixture.records[0] = source_record_fact{
        fixture.type_identity, {0, 2}, {0, 1},
        source_record_declaration_kind::definition, source_record_kind::struct_type};

    try {
        fixture.object_identities.reserve(count);
        fixture.objects.reserve(count);
        fixture.links.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            identity_ref identity = nullptr;
            const auto name = std::string{"O_"} + std::to_string(index);
            if (!fixture.context.resolve_declaration(
                    fixture.context.identity_root(), name, identity_kind::object, identity).ok()) {
                return false;
            }
            fixture.object_identities.push_back(identity);
            fixture.objects.push_back(source_object_fact{
                identity,
                source_type_ref::semantic(fixture.type_identity, {}, {0, 1}),
                {0, 1}});
            fixture.links.push_back(source_link_fact{
                {identity, member_index::from_zero_based(1)},
                {identity, member_index::from_zero_based(0)},
                {0, 1}});
        }
        return true;
    }
    catch (...) {
        return false;
    }
}

struct full_result final {
    double prepare_ms = 0.0;
    double publish_us = 0.0;
    generation_build_telemetry telemetry{};
    bool pass = false;
};

[[nodiscard]] full_result run_full(std::size_t count) {
    full_fixture fixture;
    if (!build_full_fixture(count, fixture))
        return {};

    const source_facts facts{
        source_id{1}, fixture.text, std::span<const source_namespace_fact>{},
        fixture.records, fixture.members, std::span<const source_type_modifier>{},
        std::span<const source_enum_fact>{}, std::span<const source_enum_value_fact>{},
        std::span<const source_declaration_ref>{}, fixture.objects, fixture.links};

    source_contribution_cache cache;
    graph graph_value;
    generation_builder builder{cache, graph_value};
    diagnostic_buffer diagnostics;
    const auto begin = clock_type::now();
    const auto result = builder.prepare_g0(
        std::span<const source_facts>{&facts, 1}, {}, operation_id{1200}, diagnostics);
    const auto prepared = clock_type::now();
    if (!result.ok() || diagnostics.has_errors())
        return {};
    builder.publish_prepared();
    const auto published = clock_type::now();

    full_result output;
    output.prepare_ms = std::chrono::duration<double, std::milli>(prepared - begin).count();
    output.publish_us = std::chrono::duration<double, std::micro>(published - prepared).count();
    output.telemetry = builder.telemetry();
    output.pass = builder.published() && graph_value.type_count() == 1 &&
        graph_value.object_count() == count && graph_value.link_count() == count &&
        output.telemetry.objects == count && output.telemetry.links == count;
    return output;
}

struct sparse_fixture final {
    project_configuration configuration;
    project_context context{std::move(configuration)};
    std::string text{"x"};
    identity_ref type_identity = nullptr;
    std::array<source_member_fact, 3> members{};
    std::array<source_record_fact, 1> records{};
    std::vector<identity_ref> object_identities;
    std::vector<source_object_fact> objects;
    std::vector<source_link_fact> links;
    std::vector<source_facts> facts;
};

[[nodiscard]] bool build_sparse_fixture(std::size_t count, sparse_fixture& fixture) {
    string_id in_name;
    string_id out_name;
    string_id alt_name;
    if (!fixture.context.resolve_declaration(
            fixture.context.identity_root(), "IO", identity_kind::type, fixture.type_identity).ok() ||
        !fixture.context.intern_string("IN", in_name).ok() ||
        !fixture.context.intern_string("OUT", out_name).ok() ||
        !fixture.context.intern_string("ALT", alt_name).ok()) {
        return false;
    }
    fixture.members = {
        source_member_fact{source_type_ref::builtin(intrinsic_type::signed_int, {}, {0, 1}),
            in_name, {0, 1}, source_member_access::public_access},
        source_member_fact{source_type_ref::builtin(intrinsic_type::signed_int, {}, {0, 1}),
            out_name, {0, 1}, source_member_access::public_access},
        source_member_fact{source_type_ref::builtin(intrinsic_type::signed_int, {}, {0, 1}),
            alt_name, {0, 1}, source_member_access::public_access},
    };
    fixture.records[0] = source_record_fact{
        fixture.type_identity, {0, 3}, {0, 1},
        source_record_declaration_kind::definition, source_record_kind::struct_type};

    try {
        fixture.object_identities.reserve(count);
        fixture.objects.reserve(count);
        fixture.links.reserve(count);
        fixture.facts.reserve(count + 1);
        fixture.facts.emplace_back(
            source_id{1}, fixture.text, std::span<const source_namespace_fact>{}, fixture.records,
            fixture.members, std::span<const source_type_modifier>{}, std::span<const source_enum_fact>{},
            std::span<const source_enum_value_fact>{}, std::span<const source_declaration_ref>{});

        for (std::size_t index = 0; index < count; ++index) {
            identity_ref identity = nullptr;
            const auto name = std::string{"O_"} + std::to_string(index);
            if (!fixture.context.resolve_declaration(
                    fixture.context.identity_root(), name, identity_kind::object, identity).ok()) {
                return false;
            }
            fixture.object_identities.push_back(identity);
            fixture.objects.push_back(source_object_fact{
                identity, source_type_ref::semantic(fixture.type_identity, {}, {0, 1}), {0, 1}});
            fixture.links.push_back(source_link_fact{
                {identity, member_index::from_zero_based(1)},
                {identity, member_index::from_zero_based(0)}, {0, 1}});
        }
        for (std::size_t index = 0; index < count; ++index) {
            fixture.facts.emplace_back(
                source_id{static_cast<std::uint32_t>(index + 2)}, fixture.text,
                std::span<const source_namespace_fact>{}, std::span<const source_record_fact>{},
                std::span<const source_member_fact>{}, std::span<const source_type_modifier>{},
                std::span<const source_enum_fact>{}, std::span<const source_enum_value_fact>{},
                std::span<const source_declaration_ref>{},
                std::span<const source_object_fact>{&fixture.objects[index], 1},
                std::span<const source_link_fact>{&fixture.links[index], 1});
        }
        return true;
    }
    catch (...) {
        return false;
    }
}

struct sparse_result final {
    double prepare_us = 0.0;
    double publish_us = 0.0;
    generation_build_telemetry telemetry{};
    bool pass = false;
};

[[nodiscard]] sparse_result run_sparse(std::size_t count) {
    sparse_fixture fixture;
    if (!build_sparse_fixture(count, fixture))
        return {};

    source_contribution_cache cache;
    graph graph_value;
    diagnostic_buffer diagnostics;
    generation_builder full{cache, graph_value};
    if (!full.prepare_g0(fixture.facts, {}, operation_id{1210}, diagnostics).ok() ||
        diagnostics.has_errors()) {
        return {};
    }
    full.publish_prepared();

    const auto target = count / 2;
    const auto identity = fixture.object_identities[target];
    const auto object = graph_value.find_object(identity);
    if (!object)
        return {};
    const object_endpoint target_endpoint{object, member_index::from_zero_based(0)};
    const auto original_link = graph_value.find_link(target_endpoint);
    if (!original_link)
        return {};

    const source_object_fact replacement_object{
        identity, source_type_ref::semantic(fixture.type_identity, {}, {0, 1}), {0, 1}};
    const source_link_fact corrected_link{
        {identity, member_index::from_zero_based(2)},
        {identity, member_index::from_zero_based(0)}, {0, 1}};
    const source_facts replacement{
        source_id{static_cast<std::uint32_t>(target + 2)}, fixture.text,
        std::span<const source_namespace_fact>{}, std::span<const source_record_fact>{},
        std::span<const source_member_fact>{}, std::span<const source_type_modifier>{},
        std::span<const source_enum_fact>{}, std::span<const source_enum_value_fact>{},
        std::span<const source_declaration_ref>{},
        std::span<const source_object_fact>{&replacement_object, 1},
        std::span<const source_link_fact>{&corrected_link, 1}};

    generation_builder incremental{cache, graph_value};
    diagnostics.clear();
    const auto begin = clock_type::now();
    const auto result = incremental.prepare_incremental(
        std::span<const source_facts>{&replacement, 1}, {}, {}, operation_id{1211}, diagnostics);
    const auto prepared = clock_type::now();
    if (!result.ok() || diagnostics.has_errors())
        return {};
    incremental.publish_prepared();
    const auto published = clock_type::now();

    const auto link_after = graph_value.find_link(target_endpoint);
    const auto* record = graph_value.find(link_after);
    sparse_result output;
    output.prepare_us = std::chrono::duration<double, std::micro>(prepared - begin).count();
    output.publish_us = std::chrono::duration<double, std::micro>(published - prepared).count();
    output.telemetry = incremental.telemetry();
    output.pass = incremental.published() && link_after == original_link && record != nullptr &&
        record->source.object == object && record->source.member == member_index::from_zero_based(2) &&
        graph_value.object_count() == count && graph_value.link_count() == count &&
        output.telemetry.changed_sources == 1 && output.telemetry.graph_full_scans == 0 &&
        output.telemetry.contribution_full_scans == 0;
    return output;
}

} // namespace

int main(int argc, char** argv) {
    const bool gate = argc == 2 && std::string_view{argv[1]} == "--gate";
    std::cout << std::fixed << std::setprecision(6);
    if (!gate) {
        const auto value = run_full(100000);
        std::cout << "objects,links,prepare_ms,publish_us,status\n";
        std::cout << 100000 << ',' << 100000 << ',' << value.prepare_ms << ',' << value.publish_us
                  << ',' << (value.pass ? "PASS" : "FAIL") << '\n';
        return value.pass ? 0 : 1;
    }

    const auto full = run_full(1000000);
    std::cout << "objects,links,scenario,prepare_ms,publish_us,graph_full_scans,contribution_full_scans,status\n";
    std::cout << 1000000 << ',' << 1000000 << ",full," << full.prepare_ms << ',' << full.publish_us
              << ',' << full.telemetry.graph_full_scans << ',' << full.telemetry.contribution_full_scans
              << ',' << (full.pass ? "PASS" : "FAIL") << '\n';

    const auto sparse = run_sparse(100000);
    std::cout << 100000 << ',' << 100000 << ",incremental_retarget_one,"
              << sparse.prepare_us / 1000.0 << ',' << sparse.publish_us << ','
              << sparse.telemetry.graph_full_scans << ',' << sparse.telemetry.contribution_full_scans
              << ',' << (sparse.pass ? "PASS" : "FAIL") << '\n';

    const bool performance = full.prepare_ms <= 2000.0 && full.publish_us <= 10.0 &&
        sparse.prepare_us <= 500.0 && sparse.publish_us <= 5.0;
    std::cout << "COMPLETE_GRAPH_GATE," << (full.pass && sparse.pass && performance ? "PASS" : "FAIL")
              << ",full_1M<=2000ms,sparse_100K_retarget<=500us,publish<=5us\n";
    return full.pass && sparse.pass && performance ? 0 : 1;
}
