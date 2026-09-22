#include "project/parser/source_parser.hpp"
#include "project/parser/lexer.hpp"

#include <iostream>
#include <string_view>

using namespace cw::server;

namespace {
bool check(std::string_view text, bool accepted, std::size_t members = 0,
    std::size_t methods = 0, std::size_t objects = 0) {
    project_configuration configuration;
    project_context context{std::move(configuration)};
    source_manager manager;
    source_snapshot snapshot;
    diagnostic_buffer diagnostics;
    if (!manager.publish_memory("memory/regression.hpp", text, snapshot).ok()) return false;
    std::vector<parser_token> tokens;
    if (!lex_source(snapshot, operation_id{1}, diagnostics, tokens).ok()) return false;
    parsed_source output;
    const source_environment environment;
    const auto result = source_parser{context.parser_services()}.parse(
        snapshot, tokens, environment, operation_id{1}, diagnostics, output);
    const auto facts = output.facts();
    const bool correct = result.ok() == accepted &&
        (accepted ? !diagnostics.has_errors() && facts.members().size() == members &&
            facts.methods().size() == methods && facts.objects().size() == objects :
            diagnostics.has_errors() && facts.members().empty() && facts.methods().empty());
    if (!correct) std::cerr << "Unexpected parse result: " << text << '\n';
    return correct;
}

bool check_ranges() {
    constexpr std::string_view text =
        "struct S { int* f(); const int& g(); int x = (1); int y{2}; int z; };"
        " S a(5), b{6};";
    project_configuration configuration;
    project_context context{std::move(configuration)};
    source_manager manager;
    source_snapshot snapshot;
    diagnostic_buffer diagnostics;
    if (!manager.publish_memory("memory/ranges.hpp", text, snapshot).ok()) return false;
    std::vector<parser_token> tokens;
    if (!lex_source(snapshot, operation_id{1}, diagnostics, tokens).ok()) return false;
    parsed_source output;
    const source_environment environment;
    if (!source_parser{context.parser_services()}.parse(snapshot, tokens, environment,
        operation_id{1}, diagnostics, output).ok()) return false;
    const auto facts = output.facts();
    if (facts.methods().size() != 2 || facts.members().size() != 3 || facts.objects().size() != 2)
        return false;
    const auto first = facts.methods()[0].return_type.modifiers;
    const auto second = facts.methods()[1].return_type.modifiers;
    if (first.begin != 0 || first.count != 1 || second.begin != 1 || second.count != 2)
        return false;
    const auto mods = facts.method_modifiers();
    if (mods[0].kind != source_type_modifier_kind::pointer ||
        mods[1].kind != source_type_modifier_kind::const_qualified ||
        mods[2].kind != source_type_modifier_kind::lvalue_reference) return false;
    const auto spelling = [&](source_span span) { return text.substr(span.offset, span.length); };
    return spelling(facts.members()[0].initializer) == "= (1)" &&
        spelling(facts.members()[1].initializer) == "{2}" &&
        facts.members()[2].initializer.length == 0 &&
        spelling(facts.objects()[0].initializer) == "(5)" &&
        spelling(facts.objects()[1].initializer) == "{6}";
}
}

int main() {
    bool passed = check_ranges();
    passed &= check("struct S { int f(int x = (1)); };", true, 0, 1);
    passed &= check("struct S { int f(int x = call(1, (2)), int y = 3); };", true, 0, 1);
    passed &= check("struct S { int f(int x = make({1, 2})); };", true, 0, 1);
    passed &= check("struct S { int x = call(1, (2)), y{}; };", true, 2);
    passed &= check("struct S {}; S a(-5); S b{call(1, 2)};", true, 0, 0, 2);
    passed &= check("struct S {}; S f(int x = (1));", true);
    for (const auto text : {
        "struct S { int x(5); };", "struct S { int f(Unknown); };",
        "struct S { int x = ; };", "struct S { int x = , y; };",
        "struct S { int x = (1]; };", "struct S { int x{(1]}; };",
        "struct S { int f(int x = ); };", "struct S { int f(int x = (1]); };",
        "struct S {}; S x = ;", "struct S {}; S x(1];",
        "struct S {}; S x{(1]};", "struct S {}; S f(Unknown);"})
        passed &= check(text, false);
    return passed ? 0 : 1;
}
