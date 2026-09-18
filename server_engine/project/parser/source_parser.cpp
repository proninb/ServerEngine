#include "source_parser.hpp"

#include "../frontend/source_facts_validation.hpp"
#include "../../diagnostics/diagnostic_descriptor.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cw::server {
namespace {

[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}

[[nodiscard]] std::uint64_t binding_hash(identity_ref parent, string_id name) noexcept {
    return mix64(
        static_cast<std::uint64_t>(parent.value()) ^
        (static_cast<std::uint64_t>(name.value()) << 32));
}

// Per-parse local declaration index. Lookup keys are stored directly so the
// parser probe path never dereferences Identity Space metadata.
class binding_index final {
public:
    explicit binding_index(identity_view identities_value) noexcept
        : identities(identities_value) {}

    [[nodiscard]] status reserve(std::size_t expected) noexcept {
        try {
            std::size_t capacity = 16;
            const auto target = expected > (std::numeric_limits<std::size_t>::max)() / 2
                ? (std::numeric_limits<std::size_t>::max)()
                : expected * 2 + 1;
            while (capacity < target) {
                if (capacity > (std::numeric_limits<std::size_t>::max)() / 2)
                    return {status_code::not_available};
                capacity *= 2;
            }
            slots.assign(capacity, {});
            count = 0;
            return {};
        }
        catch (...) {
            return {status_code::not_available};
        }
    }

    [[nodiscard]] status insert(identity_ref identity) noexcept {
        if (!identity || identity.kind() != identity_kind::type)
            return {status_code::invalid_argument};

        const auto parent = identities.parent(identity);
        const auto name = identities.name(identity);
        if (!parent || !name)
            return {status_code::invalid_argument};

        if (slots.empty()) {
            const auto result = reserve(16);
            if (!result.ok())
                return result;
        }

        if ((count + 1) * 10 >= slots.size() * 7) {
            const auto result = grow();
            if (!result.ok())
                return result;
        }

        return insert_into(slots, parent, name, identity, count);
    }

    [[nodiscard]] identity_ref find(identity_ref parent, string_id name) const noexcept {
        if (slots.empty() || !parent || !name)
            return {};

        const auto mask = slots.size() - 1;
        auto position = static_cast<std::size_t>(binding_hash(parent, name)) & mask;

        for (std::size_t probe = 0; probe < slots.size(); ++probe) {
            const auto& current = slots[position];
            if (!current.identity)
                return {};

            if (current.parent == parent && current.name == name)
                return current.identity;

            position = (position + 1) & mask;
        }

        return {};
    }

private:
    struct slot final {
        identity_ref parent{};
        string_id name{};
        identity_ref identity{};
    };

    [[nodiscard]] static status insert_into(
        std::vector<slot>& target,
        identity_ref parent,
        string_id name,
        identity_ref identity,
        std::size_t& target_count) noexcept {

        const auto mask = target.size() - 1;
        auto position = static_cast<std::size_t>(binding_hash(parent, name)) & mask;

        for (std::size_t probe = 0; probe < target.size(); ++probe) {
            auto& current = target[position];
            if (!current.identity) {
                current = slot{parent, name, identity};
                ++target_count;
                return {};
            }

            if (current.parent == parent && current.name == name) {
                return current.identity == identity
                    ? status{}
                    : status{status_code::semantic_conflict};
            }

            position = (position + 1) & mask;
        }

        return {status_code::not_available};
    }

    [[nodiscard]] status grow() noexcept {
        try {
            if (slots.size() > (std::numeric_limits<std::size_t>::max)() / 2)
                return {status_code::not_available};

            std::vector<slot> replacement(slots.size() * 2);
            std::size_t replacement_count = 0;

            for (const auto& current : slots) {
                if (!current.identity)
                    continue;

                const auto result = insert_into(
                    replacement,
                    current.parent,
                    current.name,
                    current.identity,
                    replacement_count);
                if (!result.ok())
                    return result;
            }

            slots.swap(replacement);
            count = replacement_count;
            return {};
        }
        catch (...) {
            return {status_code::not_available};
        }
    }

    identity_view identities;
    std::vector<slot> slots;
    std::size_t count = 0;
};

class object_binding_index final {
public:
    explicit object_binding_index(identity_view identities_value) noexcept
        : identities(identities_value) {}

    [[nodiscard]] status reserve(std::size_t expected) noexcept {
        try {
            std::size_t capacity = 16;
            const auto target = expected > (std::numeric_limits<std::size_t>::max)() / 2
                ? (std::numeric_limits<std::size_t>::max)()
                : expected * 2 + 1;
            while (capacity < target) {
                if (capacity > (std::numeric_limits<std::size_t>::max)() / 2)
                    return {status_code::not_available};
                capacity *= 2;
            }
            slots.assign(capacity, {});
            count = 0;
            return {};
        }
        catch (...) {
            return {status_code::not_available};
        }
    }

    [[nodiscard]] status insert(
        identity_ref identity,
        identity_ref named_type) noexcept {

        if (!identity ||
            identity.kind() != identity_kind::object) {
            return {status_code::invalid_argument};
        }

        const auto parent = identities.parent(identity);
        const auto name = identities.name(identity);
        if (!parent || !name)
            return {status_code::invalid_argument};

        if (slots.empty()) {
            const auto result = reserve(16);
            if (!result.ok())
                return result;
        }

        if ((count + 1) * 10 >= slots.size() * 7) {
            const auto result = grow();
            if (!result.ok())
                return result;
        }

        return insert_into(
            slots, parent, name, identity, named_type, count);
    }

    [[nodiscard]] source_interface_object find(
        identity_ref parent,
        string_id name) const noexcept {

        if (slots.empty() || !parent || !name)
            return {};

        const auto mask = slots.size() - 1;
        auto position = static_cast<std::size_t>(binding_hash(parent, name)) & mask;

        for (std::size_t probe = 0; probe < slots.size(); ++probe) {
            const auto& current = slots[position];
            if (!current.identity)
                return {};

            if (current.parent == parent && current.name == name)
                return {current.identity, current.named_type};

            position = (position + 1) & mask;
        }

        return {};
    }

private:
    struct slot final {
        identity_ref parent{};
        string_id name{};
        identity_ref identity{};
        identity_ref named_type{};
    };

    [[nodiscard]] static status insert_into(
        std::vector<slot>& target,
        identity_ref parent,
        string_id name,
        identity_ref identity,
        identity_ref named_type,
        std::size_t& target_count) noexcept {

        const auto mask = target.size() - 1;
        auto position = static_cast<std::size_t>(binding_hash(parent, name)) & mask;

        for (std::size_t probe = 0; probe < target.size(); ++probe) {
            auto& current = target[position];
            if (!current.identity) {
                current = slot{parent, name, identity, named_type};
                ++target_count;
                return {};
            }

            if (current.parent == parent && current.name == name)
                return {status_code::semantic_conflict};

            position = (position + 1) & mask;
        }

        return {status_code::not_available};
    }

    [[nodiscard]] status grow() noexcept {
        try {
            if (slots.size() > (std::numeric_limits<std::size_t>::max)() / 2)
                return {status_code::not_available};

            std::vector<slot> replacement(slots.size() * 2);
            std::size_t replacement_count = 0;

            for (const auto& current : slots) {
                if (!current.identity)
                    continue;

                const auto result = insert_into(
                    replacement,
                    current.parent,
                    current.name,
                    current.identity,
                    current.named_type,
                    replacement_count);
                if (!result.ok())
                    return result;
            }

            slots.swap(replacement);
            count = replacement_count;
            return {};
        }
        catch (...) {
            return {status_code::not_available};
        }
    }

    identity_view identities;
    std::vector<slot> slots;
    std::size_t count = 0;
};

class member_binding_index final {
public:
    [[nodiscard]] status reserve(std::size_t expected) noexcept {
        try {
            std::size_t capacity = 16;
            const auto target = expected > (std::numeric_limits<std::size_t>::max)() / 2
                ? (std::numeric_limits<std::size_t>::max)()
                : expected * 2 + 1;
            while (capacity < target) {
                if (capacity > (std::numeric_limits<std::size_t>::max)() / 2)
                    return {status_code::not_available};
                capacity *= 2;
            }
            slots.assign(capacity, {});
            count = 0;
            return {};
        }
        catch (...) {
            return {status_code::not_available};
        }
    }

    [[nodiscard]] status insert(
        identity_ref type,
        string_id name,
        member_index index) noexcept {

        if (!type || type.kind() != identity_kind::type || !name || !index)
            return {status_code::invalid_argument};

        if (slots.empty()) {
            const auto result = reserve(16);
            if (!result.ok())
                return result;
        }

        if ((count + 1) * 10 >= slots.size() * 7) {
            const auto result = grow();
            if (!result.ok())
                return result;
        }

        return insert_into(slots, type, name, index, count);
    }

    [[nodiscard]] member_index find(
        identity_ref type,
        string_id name) const noexcept {

        if (slots.empty() || !type || !name)
            return {};

        const auto mask = slots.size() - 1;
        auto position = static_cast<std::size_t>(binding_hash(type, name)) & mask;

        for (std::size_t probe = 0; probe < slots.size(); ++probe) {
            const auto& current = slots[position];
            if (!current.type)
                return {};

            if (current.type == type && current.name == name)
                return current.index;

            position = (position + 1) & mask;
        }

        return {};
    }

private:
    struct slot final {
        identity_ref type{};
        string_id name{};
        member_index index{};
    };

    [[nodiscard]] static status insert_into(
        std::vector<slot>& target,
        identity_ref type,
        string_id name,
        member_index index,
        std::size_t& target_count) noexcept {

        const auto mask = target.size() - 1;
        auto position = static_cast<std::size_t>(binding_hash(type, name)) & mask;

        for (std::size_t probe = 0; probe < target.size(); ++probe) {
            auto& current = target[position];
            if (!current.type) {
                current = slot{type, name, index};
                ++target_count;
                return {};
            }

            if (current.type == type && current.name == name)
                return {status_code::semantic_conflict};

            position = (position + 1) & mask;
        }

        return {status_code::not_available};
    }

    [[nodiscard]] status grow() noexcept {
        try {
            if (slots.size() > (std::numeric_limits<std::size_t>::max)() / 2)
                return {status_code::not_available};

            std::vector<slot> replacement(slots.size() * 2);
            std::size_t replacement_count = 0;

            for (const auto& current : slots) {
                if (!current.type)
                    continue;

                const auto result = insert_into(
                    replacement,
                    current.type,
                    current.name,
                    current.index,
                    replacement_count);
                if (!result.ok())
                    return result;
            }

            slots.swap(replacement);
            count = replacement_count;
            return {};
        }
        catch (...) {
            return {status_code::not_available};
        }
    }

    std::vector<slot> slots;
    std::size_t count = 0;
};


[[nodiscard]] constexpr bool integral_intrinsic(intrinsic_type value) noexcept {
    return value >= intrinsic_type::bool_type && value <= intrinsic_type::unsigned_long_long;
}

} // namespace

class source_parser_state final {
public:
    source_parser_state(
        project_semantic_services semantic_value,
        const source_snapshot& source_value,
        std::span<const parser_token> token_values,
        const source_environment& environment_value,
        operation_id operation_value,
        diagnostic_buffer& diagnostics_value) noexcept
        : semantic(semantic_value), source(source_value), text(source_value.text()), tokens(token_values),
          environment(environment_value), operation(operation_value), diagnostics(diagnostics_value),
          bindings(semantic.identities()), object_bindings(semantic.identities()) {}

    [[nodiscard]] status run(parsed_source& output) {
        if (!source || !source.source() || tokens.empty() ||
            tokens.back().kind != parser_token_kind::eof ||
            text.size() > (std::numeric_limits<std::uint32_t>::max)()) {
            return {status_code::invalid_argument};
        }

        auto result = bindings.reserve(64);
        if (!result.ok())
            return result;
        result = object_bindings.reserve(64);
        if (!result.ok())
            return result;
        result = member_bindings.reserve(128);
        if (!result.ok())
            return result;
        candidate.snapshot = source;
        result = parse_scope(semantic.identity_root(), false, 0);
        if (!result.ok())
            return result;
        if (current().kind != parser_token_kind::eof)
            return fail_syntax(span(current()), "unexpected token after translation unit");

        source_facts_validation_error validation_error;
        const auto validation = validate_source_facts(candidate.facts(), validation_error);
        if (!validation.ok()) {
            try {
                emit_source_facts_validation_diagnostic(candidate.facts(), validation_error, operation, diagnostics);
            }
            catch (...) {
            }
            return validation;
        }
        output = std::move(candidate);
        return {};
    }

private:
    [[nodiscard]] const parser_token& current() const noexcept {
        return tokens[cursor < tokens.size() ? cursor : tokens.size() - 1];
    }

    [[nodiscard]] source_span span(const parser_token& token) const noexcept {
        return source_span{token.offset, token.length};
    }

    [[nodiscard]] std::string_view token_text(const parser_token& token) const noexcept {
        if (token.offset > text.size() || token.length > text.size() - token.offset)
            return {};
        return text.substr(token.offset, token.length);
    }

    [[nodiscard]] bool identifier(std::string_view value) const noexcept {
        return current().kind == parser_token_kind::identifier && token_text(current()) == value;
    }

    [[nodiscard]] bool punctuation(parser_punctuation value) const noexcept {
        return current().kind == parser_token_kind::punctuation && current().punctuation == value;
    }

    void advance() noexcept {
        if (cursor + 1 < tokens.size())
            ++cursor;
    }

    [[nodiscard]] status fail(
        const diagnostic_descriptor& descriptor,
        source_span location,
        std::string_view detail,
        status_code code) noexcept {

        try {
            diagnostics.emit(diagnostic_record{
                descriptor.id,
                descriptor.default_severity,
                operation,
                source_range{source.source(), location.offset, location.length},
                std::string{detail},
            });
        }
        catch (...) {
        }
        return {code};
    }

    [[nodiscard]] status fail_syntax(source_span location, std::string_view detail) noexcept {
        return fail(diagnostics::parser_syntax_error, location, detail, status_code::invalid_argument);
    }

    [[nodiscard]] status fail_unsupported(source_span location, std::string_view detail) noexcept {
        return fail(diagnostics::parser_unsupported_construct, location, detail, status_code::not_available);
    }

    void append_declaration(
        source_declaration_kind kind,
        std::uint32_t index,
        source_span declaration) {
        candidate.declarations.push_back(source_declaration_ref{index, kind, declaration});
    }

    // Skips an initializer up to the next ',' or ';' at nesting depth 0,
    // consuming nested (), {}, []. The terminator is left current.
    // Returns false on EOF or an unmatched closing bracket.
    //
    // Explicit metadata-model contract: initializers are intentionally
    // outside the model. They are skipped as lexical regions and never
    // interpreted, so no value/expression semantics reach facts, the Graph,
    // or runtime. If runtime ever needs values, this contract (not just the
    // parser) must change: facts, contributions, and images would all gain
    // initializer representation.
    [[nodiscard]] bool skip_declarator_initializer(bool& comma) noexcept {
        std::size_t depth = 0;
        comma = false;
        for (;;) {
            const auto& token = current();
            if (token.kind == parser_token_kind::eof)
                return false;
            if (token.kind == parser_token_kind::punctuation) {
                switch (token.punctuation) {
                case parser_punctuation::left_brace:
                case parser_punctuation::left_bracket:
                case parser_punctuation::left_parenthesis:
                    ++depth;
                    break;
                case parser_punctuation::right_brace:
                case parser_punctuation::right_bracket:
                case parser_punctuation::right_parenthesis:
                    if (depth == 0)
                        return false;
                    --depth;
                    break;
                case parser_punctuation::comma:
                    if (depth == 0) {
                        comma = true;
                        return true;
                    }
                    break;
                case parser_punctuation::semicolon:
                    if (depth == 0)
                        return true;
                    break;
                default:
                    break;
                }
            }
            advance();
        }
    }

    [[nodiscard]] status parse_using(identity_ref scope) {
        const auto start = current().offset;
        advance();
        if (current().kind != parser_token_kind::identifier)
            return fail_syntax(span(current()), "expected alias identifier after 'using'");
        const auto name_text = token_text(current());
        advance();
        if (!punctuation(parser_punctuation::equal))
            return fail_syntax(span(current()), "expected '=' in alias declaration");
        advance();

        const auto shared_begin = candidate.alias_modifiers.size();
        identity_ref target_identity = nullptr;
        intrinsic_type target_intrinsic = intrinsic_type::none;
        auto result = parse_alias_target(scope, start, target_identity, target_intrinsic);
        if (!result.ok())
            return result;
        result = parse_array_suffix(candidate.alias_modifiers, false);
        if (!result.ok())
            return result;
        if (!punctuation(parser_punctuation::semicolon))
            return fail_syntax(span(current()), "expected ';' after alias declaration");
        const auto end = current().offset + current().length;
        advance();

        result = check_alias_conflict(scope, name_text);
        if (!result.ok())
            return result;
        string_id alias_name;
        result = semantic.intern_string(name_text, alias_name);
        if (!result.ok())
            return result;
        identity_ref identity = nullptr;
        result = semantic.resolve_declaration(scope, name_text, identity_kind::type, identity);
        if (!result.ok()) {
            return fail(diagnostics::parser_semantic_resolution_failed, span(current()),
                "failed to resolve alias declaration", result.code);
        }

        const auto modifier_count = candidate.alias_modifiers.size() - shared_begin;
        if (shared_begin > (std::numeric_limits<std::uint32_t>::max)() ||
            modifier_count > (std::numeric_limits<std::uint32_t>::max)())
            return {status_code::not_available};
        const auto declaration = source_span{start, end - start};
        const auto alias_index = static_cast<std::uint32_t>(candidate.aliases.size());
        candidate.aliases.push_back(source_alias_fact{
            identity,
            source_type_ref{
                target_identity,
                target_intrinsic,
                source_fact_range{
                    static_cast<std::uint32_t>(shared_begin),
                    static_cast<std::uint32_t>(modifier_count),
                },
                {},
            },
            declaration,
        });
        append_declaration(source_declaration_kind::alias, alias_index, declaration);
        aliases.push_back(alias_entry{
            scope,
            alias_name,
            target_identity,
            target_intrinsic,
            source_fact_range{
                static_cast<std::uint32_t>(shared_begin),
                static_cast<std::uint32_t>(modifier_count),
            },
        });
        return {};
    }

    [[nodiscard]] status parse_typedef(identity_ref scope) {
        const auto start = current().offset;
        advance();
        const auto shared_begin = candidate.alias_modifiers.size();
        identity_ref target_identity = nullptr;
        intrinsic_type target_intrinsic = intrinsic_type::none;
        auto result = parse_alias_target(scope, start, target_identity, target_intrinsic);
        if (!result.ok())
            return result;
        const auto shared_end = candidate.alias_modifiers.size();

        bool first_declarator = true;
        for (;;) {
            if (current().kind != parser_token_kind::identifier)
                return fail_syntax(span(current()), "expected typedef declarator name");
            const auto name_text = token_text(current());
            advance();

            std::size_t declarator_begin = shared_begin;
            if (!first_declarator) {
                try {
                    candidate.alias_modifiers.reserve(shared_end + (shared_end - shared_begin));
                    declarator_begin = candidate.alias_modifiers.size();
                    for (std::size_t index = shared_begin; index < shared_end; ++index)
                        candidate.alias_modifiers.push_back(candidate.alias_modifiers[index]);
                }
                catch (const std::bad_alloc&) {
                    return {status_code::not_available};
                }
                catch (const std::length_error&) {
                    return {status_code::not_available};
                }
            }

            result = parse_array_suffix(candidate.alias_modifiers, false);
            if (!result.ok())
                return result;

            const bool comma = punctuation(parser_punctuation::comma);
            if (!comma && !punctuation(parser_punctuation::semicolon))
                return fail_syntax(span(current()), "expected ',' or ';' after typedef declarator");

            const auto end = current().offset + current().length;
            const auto modifier_count = candidate.alias_modifiers.size() - declarator_begin;
            if (declarator_begin > (std::numeric_limits<std::uint32_t>::max)() ||
                modifier_count > (std::numeric_limits<std::uint32_t>::max)())
                return {status_code::not_available};

            result = check_alias_conflict(scope, name_text);
            if (!result.ok())
                return result;
            string_id alias_name;
            result = semantic.intern_string(name_text, alias_name);
            if (!result.ok())
                return result;
            identity_ref identity = nullptr;
            result = semantic.resolve_declaration(scope, name_text, identity_kind::type, identity);
            if (!result.ok()) {
                return fail(diagnostics::parser_semantic_resolution_failed, span(current()),
                    "failed to resolve alias declaration", result.code);
            }

            const auto declaration = source_span{start, end - start};
            const auto alias_index = static_cast<std::uint32_t>(candidate.aliases.size());
            candidate.aliases.push_back(source_alias_fact{
                identity,
                source_type_ref{
                    target_identity,
                    target_intrinsic,
                    source_fact_range{
                        static_cast<std::uint32_t>(declarator_begin),
                        static_cast<std::uint32_t>(modifier_count),
                    },
                    {},
                },
                declaration,
            });
            append_declaration(source_declaration_kind::alias, alias_index, declaration);
            aliases.push_back(alias_entry{
                scope,
                alias_name,
                target_identity,
                target_intrinsic,
                source_fact_range{
                    static_cast<std::uint32_t>(declarator_begin),
                    static_cast<std::uint32_t>(modifier_count),
                },
            });

            advance();
            if (!comma)
                return {};
            first_declarator = false;
        }
    }

    // Parses `: [virtual] [access] Base (, ...)` after the record name.
    // Only single identifiers visible in scope resolve; qualified `A::B`
    // names stay unsupported. Bases are recorded, not consumed: Generation
    // Builder and layout do not use them yet (ABI stage).
    [[nodiscard]] status parse_base_clause(
        identity_ref scope,
        source_record_kind kind,
        std::uint32_t lookup_offset) {
        for (;;) {
            bool is_virtual = false;
            auto access = kind == source_record_kind::class_type
                ? source_member_access::private_access
                : source_member_access::public_access;
            bool progress = true;
            while (progress) {
                progress = false;
                if (identifier("virtual")) {
                    is_virtual = true;
                    advance();
                    progress = true;
                }
                else if (identifier("public")) {
                    access = source_member_access::public_access;
                    advance();
                    progress = true;
                }
                else if (identifier("protected")) {
                    access = source_member_access::protected_access;
                    advance();
                    progress = true;
                }
                else if (identifier("private")) {
                    access = source_member_access::private_access;
                    advance();
                    progress = true;
                }
            }
            if (current().kind != parser_token_kind::identifier)
                return fail_syntax(span(current()), "expected base type name");
            const auto base_span = span(current());
            const auto base_name = semantic.find_string(token_text(current()));
            identity_ref base = base_name ? lookup_visible_type(scope, base_name, lookup_offset) : nullptr;
            if (base == nullptr) {
                return fail(diagnostics::parser_unresolved_type, base_span,
                    "base type name is not visible in the Source semantic environment", status_code::not_found);
            }
            advance();
            if (punctuation(parser_punctuation::colon)) {
                const auto* second = cursor + 1 < tokens.size() ? &tokens[cursor + 1] : nullptr;
                if (second != nullptr && second->kind == parser_token_kind::punctuation &&
                    second->punctuation == parser_punctuation::colon) {
                    return fail_unsupported(span(current()), "qualified base type names are not implemented");
                }
                return fail_syntax(span(current()), "expected ',' or '{' after base type name");
            }
            candidate.bases.push_back(source_base_fact{base, access, base_span, is_virtual});
            if (punctuation(parser_punctuation::comma)) {
                advance();
                continue;
            }
            return {};
        }
    }

    [[nodiscard]] status parse_array_suffix(
        std::vector<source_type_modifier>& out,
        bool member_context) {
        while (punctuation(parser_punctuation::left_bracket)) {
            advance();
            if (punctuation(parser_punctuation::right_bracket)) {
                out.push_back(source_type_modifier{0, source_type_modifier_kind::unbounded_array});
                advance();
                continue;
            }
            if (current().kind != parser_token_kind::integer_literal) {
                return fail_syntax(span(current()), member_context
                    ? "expected positive member array bound or ']'"
                    : "expected positive object array bound or ']'");
            }
            std::uint64_t bound = 0;
            const auto number = token_text(current());
            const auto conversion = std::from_chars(number.data(), number.data() + number.size(), bound);
            if (conversion.ec != std::errc{} || conversion.ptr != number.data() + number.size() || bound == 0) {
                return fail_syntax(span(current()), member_context
                    ? "member array bound must be a positive decimal integer"
                    : "object array bound must be a positive decimal integer");
            }
            advance();
            if (!punctuation(parser_punctuation::right_bracket)) {
                return fail_syntax(span(current()), member_context
                    ? "expected ']' after member array bound"
                    : "expected ']' after object array bound");
            }
            out.push_back(source_type_modifier{bound, source_type_modifier_kind::bounded_array});
            advance();
        }
        return {};
    }

    // One source-local alias. Entries stay visible through nested scopes via
    // the scope ancestry chain; no truncation bookkeeping is required.
    struct alias_entry final {
        identity_ref scope = nullptr;
        string_id name{};
        identity_ref target_identity = nullptr;
        intrinsic_type target_intrinsic = intrinsic_type::none;
        source_fact_range target_modifiers{};
    };

    [[nodiscard]] const alias_entry* find_visible_alias(
        identity_ref scope,
        string_id name) const noexcept {
        for (std::size_t index = aliases.size(); index > 0; --index) {
            const auto& entry = aliases[index - 1];
            if (entry.name != name)
                continue;
            auto current_scope = scope;
            while (current_scope != nullptr) {
                if (current_scope == entry.scope)
                    return &entry;
                current_scope = semantic.identities().parent(current_scope);
            }
        }
        return nullptr;
    }

    [[nodiscard]] status check_alias_conflict(identity_ref scope, std::string_view name) {
        const auto existing = semantic.find_string(name);
        if (existing) {
            for (const auto& entry : aliases) {
                if (entry.scope == scope && entry.name == existing) {
                    return fail(diagnostics::parser_semantic_resolution_failed, span(current()),
                        "alias name conflicts with a visible declaration", status_code::semantic_conflict);
                }
            }
            if (bindings.find(scope, existing) != nullptr) {
                return fail(diagnostics::parser_semantic_resolution_failed, span(current()),
                    "alias name conflicts with a visible declaration", status_code::semantic_conflict);
            }
        }
        return {};
    }

    [[nodiscard]] bool using_alias_ahead() const noexcept {
        if (cursor + 2 >= tokens.size())
            return false;
        const auto& name = tokens[cursor + 1];
        const auto& equal = tokens[cursor + 2];
        return name.kind == parser_token_kind::identifier &&
            equal.kind == parser_token_kind::punctuation &&
            equal.punctuation == parser_punctuation::equal;
    }

    // Parses an aliased type (`using U = <here>` / `typedef <here> Name`)
    // into candidate.alias_modifiers: leading cv, base, trailing cv,
    // pointer/reference suffixes. Array suffixes stay with the caller so
    // typedef multi-declarators can extend each name separately.
    [[nodiscard]] status parse_alias_target(
        identity_ref scope,
        std::uint32_t lookup_offset,
        identity_ref& target_identity,
        intrinsic_type& target_intrinsic) {
        while (identifier("const") || identifier("volatile")) {
            candidate.alias_modifiers.push_back(source_type_modifier{
                0,
                identifier("const") ? source_type_modifier_kind::const_qualified
                                    : source_type_modifier_kind::volatile_qualified,
            });
            advance();
        }
        target_identity = nullptr;
        target_intrinsic = intrinsic_type::none;
        std::uint32_t type_end = current().offset;
        auto result = parse_type_base(
            scope, target_identity, target_intrinsic, type_end, lookup_offset, candidate.alias_modifiers);
        if (!result.ok())
            return result;
        while (identifier("const") || identifier("volatile")) {
            candidate.alias_modifiers.push_back(source_type_modifier{
                0,
                identifier("const") ? source_type_modifier_kind::const_qualified
                                    : source_type_modifier_kind::volatile_qualified,
            });
            advance();
        }
        for (;;) {
            if (punctuation(parser_punctuation::asterisk)) {
                candidate.alias_modifiers.push_back(
                    source_type_modifier{0, source_type_modifier_kind::pointer});
                advance();
                while (identifier("const") || identifier("volatile")) {
                    candidate.alias_modifiers.push_back(source_type_modifier{
                        0,
                        identifier("const") ? source_type_modifier_kind::const_qualified
                                            : source_type_modifier_kind::volatile_qualified,
                    });
                    advance();
                }
                continue;
            }
            if (punctuation(parser_punctuation::ampersand)) {
                candidate.alias_modifiers.push_back(
                    source_type_modifier{0, source_type_modifier_kind::lvalue_reference});
                advance();
                continue;
            }
            if (punctuation(parser_punctuation::ampersand_ampersand)) {
                candidate.alias_modifiers.push_back(
                    source_type_modifier{0, source_type_modifier_kind::rvalue_reference});
                advance();
                continue;
            }
            break;
        }
        return {};
    }

    [[nodiscard]] status parse_scope(identity_ref scope, bool expect_close, std::size_t depth) {
        if (depth > 128)
            return fail_unsupported(span(current()), "semantic scope nesting exceeds 128 levels");

        while (current().kind != parser_token_kind::eof) {
            if (punctuation(parser_punctuation::right_brace))
                return expect_close ? status{} : fail_syntax(span(current()), "unexpected closing brace");
            if (punctuation(parser_punctuation::semicolon)) {
                advance();
                continue;
            }
            if (current().kind == parser_token_kind::keyword_namespace) {
                const auto result = parse_namespace(scope, depth);
                if (!result.ok())
                    return result;
                continue;
            }
            if (current().kind == parser_token_kind::keyword_struct ||
                current().kind == parser_token_kind::keyword_class ||
                current().kind == parser_token_kind::keyword_union) {
                const auto result = parse_record(scope);
                if (!result.ok())
                    return result;
                continue;
            }
            if (current().kind == parser_token_kind::keyword_enum) {
                const auto result = parse_enum(scope);
                if (!result.ok())
                    return result;
                continue;
            }
            if (identifier("typedef")) {
                const auto result = parse_typedef(scope);
                if (!result.ok())
                    return result;
                continue;
            }
            if (identifier("using") && using_alias_ahead()) {
                const auto result = parse_using(scope);
                if (!result.ok())
                    return result;
                continue;
            }
            if (current().kind == parser_token_kind::identifier) {
                const auto next = cursor + 1 < tokens.size() ? &tokens[cursor + 1] : nullptr;
                const auto result = next != nullptr && next->kind == parser_token_kind::punctuation &&
                    next->punctuation == parser_punctuation::dot
                    ? parse_link(scope)
                    : parse_object(scope);
                if (!result.ok())
                    return result;
                continue;
            }
            if (punctuation(parser_punctuation::hash))
                return fail_unsupported(span(current()), "preprocessing directives must be removed by Source frontend");
            return fail_unsupported(span(current()), "unsupported declaration at semantic scope");
        }
        return expect_close
            ? fail_syntax(source_span{static_cast<std::uint32_t>(text.size()), 0}, "expected closing brace")
            : status{};
    }

    [[nodiscard]] status parse_namespace(identity_ref parent, std::size_t depth) {
        const auto start = current().offset;
        advance();
        if (current().kind != parser_token_kind::identifier)
            return fail_syntax(span(current()), "expected namespace identifier");

        // Nested-name `namespace A::B::C`: one fact/declaration per level,
        // each nested under the previous, sharing a single brace body.
        // `::` arrives as two consecutive colon tokens.
        struct namespace_level final {
            std::uint32_t namespace_index = 0;
            std::uint32_t sequence_index = 0;
        };
        std::vector<namespace_level> levels;
        identity_ref scope = parent;
        for (;;) {
            const auto alias_conflict = check_alias_conflict(scope, token_text(current()));
            if (!alias_conflict.ok())
                return alias_conflict;
            const auto name_span = span(current());
            identity_ref identity = nullptr;
            auto result = semantic.resolve_declaration(
                scope, token_text(current()), identity_kind::namespace_scope, identity);
            if (!result.ok()) {
                return fail(diagnostics::parser_semantic_resolution_failed, name_span,
                    "failed to resolve namespace declaration", result.code);
            }
            advance();
            if (levels.size() >= 128)
                return fail_unsupported(span(current()), "namespace name nesting exceeds 128 levels");
            const auto namespace_index = static_cast<std::uint32_t>(candidate.namespaces.size());
            candidate.namespaces.push_back(source_namespace_fact{identity, {}});
            const auto sequence_index = static_cast<std::uint32_t>(candidate.declarations.size());
            append_declaration(source_declaration_kind::namespace_scope, namespace_index, {});
            levels.push_back(namespace_level{namespace_index, sequence_index});
            scope = identity;
            if (!punctuation(parser_punctuation::colon))
                break;
            const auto* second = cursor + 1 < tokens.size() ? &tokens[cursor + 1] : nullptr;
            if (second == nullptr || second->kind != parser_token_kind::punctuation ||
                second->punctuation != parser_punctuation::colon)
                break;
            advance();
            advance();
            if (current().kind != parser_token_kind::identifier)
                return fail_syntax(span(current()), "expected namespace identifier after '::'");
        }
        if (!punctuation(parser_punctuation::left_brace))
            return fail_unsupported(span(current()), "namespace aliases are not implemented");

        advance();
        const auto body = parse_scope(scope, true, depth + 1);
        if (!body.ok())
            return body;
        const auto end = current().offset + current().length;
        const auto declaration = source_span{start, end - start};
        for (const auto& level : levels) {
            candidate.namespaces[level.namespace_index].declaration = declaration;
            candidate.declarations[level.sequence_index].declaration = declaration;
        }
        advance();
        return {};
    }

    [[nodiscard]] status parse_record(identity_ref scope) {
        const auto start = current().offset;
        source_record_kind kind = source_record_kind::struct_type;
        source_member_access access = source_member_access::public_access;
        if (current().kind == parser_token_kind::keyword_class) {
            kind = source_record_kind::class_type;
            access = source_member_access::private_access;
        }
        else if (current().kind == parser_token_kind::keyword_union) {
            kind = source_record_kind::union_type;
        }
        advance();
        if (current().kind != parser_token_kind::identifier)
            return fail_unsupported(span(current()), "anonymous record definitions are not implemented");

        {
            const auto alias_conflict = check_alias_conflict(scope, token_text(current()));
            if (!alias_conflict.ok())
                return alias_conflict;
        }
        const auto name_span = span(current());
        identity_ref identity = nullptr;
        auto result = semantic.resolve_declaration(scope, token_text(current()), identity_kind::type, identity);
        if (!result.ok()) {
            return fail(diagnostics::parser_semantic_resolution_failed, name_span,
                "failed to resolve record declaration", result.code);
        }
        result = bindings.insert(identity);
        if (!result.ok())
            return fail(diagnostics::parser_semantic_resolution_failed, name_span,
                "local type binding conflicts with canonical project identity", result.code);

        const auto record_index = static_cast<std::uint32_t>(candidate.records.size());
        candidate.records.push_back(source_record_fact{identity, {}, {}, {}, source_record_declaration_kind::declaration, kind});
        const auto sequence_index = candidate.declarations.size();
        append_declaration(source_declaration_kind::record_type, record_index, {});

        advance();
        const auto base_begin = candidate.bases.size();
        if (punctuation(parser_punctuation::colon)) {
            advance();
            result = parse_base_clause(scope, kind, start);
            if (!result.ok())
                return result;
        }
        if (punctuation(parser_punctuation::semicolon)) {
            if (candidate.bases.size() != base_begin)
                return fail_syntax(span(current()), "base clause requires a record definition");
            const auto end = current().offset + current().length;
            const auto declaration = source_span{start, end - start};
            candidate.records[record_index].declaration = declaration;
            candidate.declarations[sequence_index].declaration = declaration;
            advance();
            return {};
        }
        if (!punctuation(parser_punctuation::left_brace))
            return fail_unsupported(span(current()), "attributes and record declarator suffixes are not implemented");

        const auto member_begin = candidate.members.size();
        advance();
        while (current().kind != parser_token_kind::eof &&
               !punctuation(parser_punctuation::right_brace)) {
            if (current().kind == parser_token_kind::identifier &&
                (token_text(current()) == "public" || token_text(current()) == "protected" || token_text(current()) == "private")) {
                const auto text_value = token_text(current());
                advance();
                if (!punctuation(parser_punctuation::colon))
                    return fail_syntax(span(current()), "expected ':' after access specifier");
                if (text_value == "public") access = source_member_access::public_access;
                else if (text_value == "protected") access = source_member_access::protected_access;
                else access = source_member_access::private_access;
                advance();
                continue;
            }
            result = parse_member(scope, access);
            if (!result.ok())
                return result;
        }
        if (!punctuation(parser_punctuation::right_brace))
            return fail_syntax(span(current()), "expected '}' after record definition");
        advance();
        if (!punctuation(parser_punctuation::semicolon))
            return fail_unsupported(span(current()), "record variables and declarator suffixes are not implemented");
        const auto end = current().offset + current().length;
        advance();

        const auto member_count = candidate.members.size() - member_begin;
        if (member_begin > (std::numeric_limits<std::uint32_t>::max)() ||
            member_count > (std::numeric_limits<std::uint32_t>::max)())
            return {status_code::not_available};
        const auto base_count = candidate.bases.size() - base_begin;
        if (base_begin > (std::numeric_limits<std::uint32_t>::max)() ||
            base_count > (std::numeric_limits<std::uint32_t>::max)())
            return {status_code::not_available};
        const auto declaration = source_span{start, end - start};
        auto& fact = candidate.records[record_index];
        fact.members = source_fact_range{static_cast<std::uint32_t>(member_begin), static_cast<std::uint32_t>(member_count)};
        fact.bases = source_fact_range{static_cast<std::uint32_t>(base_begin), static_cast<std::uint32_t>(base_count)};
        fact.declaration = declaration;
        fact.declaration_kind = source_record_declaration_kind::definition;
        candidate.declarations[sequence_index].declaration = declaration;
        for (std::uint32_t index = 0; index < fact.members.count; ++index) {
            const auto& member = candidate.members[fact.members.begin + index];
            result = member_bindings.insert(
                identity, member.name, member_index::from_zero_based(index));
            if (!result.ok())
                return fail(diagnostics::parser_semantic_resolution_failed, declaration,
                    "record member name conflicts within type", result.code);
        }
        return {};
    }

    [[nodiscard]] status parse_object(identity_ref scope) {
        const auto declaration_start = current().offset;
        const auto type_start = current().offset;
        const auto modifier_begin = candidate.modifiers.size();
        std::uint32_t type_end = type_start;

        while (identifier("const") || identifier("volatile")) {
            candidate.modifiers.push_back(source_type_modifier{
                0,
                identifier("const") ? source_type_modifier_kind::const_qualified
                                    : source_type_modifier_kind::volatile_qualified,
            });
            type_end = current().offset + current().length;
            advance();
        }

        identity_ref semantic_type = nullptr;
        intrinsic_type intrinsic = intrinsic_type::none;
        auto result = parse_type_base(scope, semantic_type, intrinsic, type_end, declaration_start, candidate.modifiers);
        if (!result.ok())
            return result;

        while (identifier("const") || identifier("volatile")) {
            candidate.modifiers.push_back(source_type_modifier{
                0,
                identifier("const") ? source_type_modifier_kind::const_qualified
                                    : source_type_modifier_kind::volatile_qualified,
            });
            type_end = current().offset + current().length;
            advance();
        }

        for (;;) {
            if (punctuation(parser_punctuation::asterisk)) {
                candidate.modifiers.push_back(source_type_modifier{0, source_type_modifier_kind::pointer});
                type_end = current().offset + current().length;
                advance();
                while (identifier("const") || identifier("volatile")) {
                    candidate.modifiers.push_back(source_type_modifier{
                        0,
                        identifier("const") ? source_type_modifier_kind::const_qualified
                                            : source_type_modifier_kind::volatile_qualified,
                    });
                    type_end = current().offset + current().length;
                    advance();
                }
                continue;
            }
            if (punctuation(parser_punctuation::ampersand)) {
                candidate.modifiers.push_back(source_type_modifier{0, source_type_modifier_kind::lvalue_reference});
                type_end = current().offset + current().length;
                advance();
                continue;
            }
            if (punctuation(parser_punctuation::ampersand_ampersand)) {
                candidate.modifiers.push_back(source_type_modifier{0, source_type_modifier_kind::rvalue_reference});
                type_end = current().offset + current().length;
                advance();
                continue;
            }
            break;
        }

        // Declarator list: `name [arrays] [= init] (, ...)? ;`
        // Shared type modifiers are duplicated per declarator (see parse_member).
        const auto object_shared_end = candidate.modifiers.size();
        bool object_first = true;
        for (;;) {
            if (current().kind != parser_token_kind::identifier)
                return fail_syntax(span(current()), "expected object identifier");

            const auto object_name_span = span(current());
            identity_ref object_identity = nullptr;
            result = semantic.resolve_declaration(
                scope, token_text(current()), identity_kind::object, object_identity);
            if (!result.ok()) {
                return fail(diagnostics::parser_semantic_resolution_failed, object_name_span,
                    "failed to resolve object declaration", result.code);
            }
            advance();

            std::size_t object_declarator_begin = modifier_begin;
            if (!object_first) {
                try {
                    candidate.modifiers.reserve(object_shared_end + (object_shared_end - modifier_begin));
                    object_declarator_begin = candidate.modifiers.size();
                    for (std::size_t index = modifier_begin; index < object_shared_end; ++index)
                        candidate.modifiers.push_back(candidate.modifiers[index]);
                }
                catch (const std::bad_alloc&) {
                    return {status_code::not_available};
                }
                catch (const std::length_error&) {
                    return {status_code::not_available};
                }
            }

            result = parse_array_suffix(candidate.modifiers, false);
            if (!result.ok())
                return result;

            bool object_comma = false;
            if (punctuation(parser_punctuation::equal)) {
                advance();
                if (!skip_declarator_initializer(object_comma))
                    return fail_syntax(span(current()), "expected ',' or ';' after object initializer");
            }
            else if (punctuation(parser_punctuation::comma)) {
                object_comma = true;
            }
            else if (!punctuation(parser_punctuation::semicolon)) {
                return fail_unsupported(span(current()), "functions and function-style declarators are not implemented");
            }

            const auto object_declaration_end = current().offset + current().length;

            const auto object_modifier_count = candidate.modifiers.size() - object_declarator_begin;
            if (object_declarator_begin > (std::numeric_limits<std::uint32_t>::max)() ||
                object_modifier_count > (std::numeric_limits<std::uint32_t>::max)()) {
                return {status_code::not_available};
            }

            const auto object_modifier_range = source_fact_range{
                static_cast<std::uint32_t>(object_declarator_begin),
                static_cast<std::uint32_t>(object_modifier_count),
            };
            const auto spelling = source_span{type_start, type_end - type_start};
            const auto type = semantic_type != nullptr
                ? source_type_ref::semantic(semantic_type, object_modifier_range, spelling)
                : source_type_ref::builtin(intrinsic, object_modifier_range, spelling);
            const auto object_index = static_cast<std::uint32_t>(candidate.objects.size());
            candidate.objects.push_back(source_object_fact{
                object_identity,
                type,
                source_span{declaration_start, object_declaration_end - declaration_start},
            });
            append_declaration(source_declaration_kind::object, object_index,
                source_span{declaration_start, object_declaration_end - declaration_start});

            const auto direct_named_type = semantic_type != nullptr && object_modifier_count == 0
                ? semantic_type
                : nullptr;
            result = object_bindings.insert(object_identity, direct_named_type);
            if (!result.ok()) {
                return fail(diagnostics::parser_semantic_resolution_failed, object_name_span,
                    "object name conflicts within scope", result.code);
            }

            if (!object_comma) {
                advance();
                return {};
            }
            advance();
            object_first = false;
        }
    }

    [[nodiscard]] source_interface_object lookup_visible_object(
        identity_ref scope,
        string_id name,
        std::uint32_t source_offset) const noexcept {

        auto current_scope = scope;
        while (current_scope != nullptr) {
            const auto local = object_bindings.find(current_scope, name);
            if (local.identity != nullptr)
                return local;
            const auto imported = environment.find_object(current_scope, name, source_offset);
            if (imported.identity != nullptr)
                return imported;
            current_scope = semantic.identities().parent(current_scope);
        }
        return {};
    }

    [[nodiscard]] member_index lookup_visible_member(
        identity_ref type,
        string_id name,
        std::uint32_t source_offset) const noexcept {

        const auto local = member_bindings.find(type, name);
        if (local)
            return local;
        return environment.find_member(type, name, source_offset);
    }

    [[nodiscard]] status parse_endpoint(
        identity_ref scope,
        std::uint32_t lookup_offset,
        source_object_endpoint_fact& output) {

        output = {};
        if (current().kind != parser_token_kind::identifier)
            return fail_syntax(span(current()), "expected object identifier in link endpoint");
        const auto object_span = span(current());
        const auto object_name = semantic.find_string(token_text(current()));
        const auto object = object_name
            ? lookup_visible_object(scope, object_name, lookup_offset)
            : source_interface_object{};
        if (object.identity == nullptr) {
            return fail(diagnostics::parser_semantic_resolution_failed, object_span,
                "link endpoint object is not visible", status_code::not_found);
        }
        if (object.named_type == nullptr) {
            return fail_unsupported(object_span,
                "link endpoints require an object of direct named record type");
        }
        advance();
        if (!punctuation(parser_punctuation::dot))
            return fail_syntax(span(current()), "expected '.' in link endpoint");
        advance();
        if (current().kind != parser_token_kind::identifier)
            return fail_syntax(span(current()), "expected member identifier in link endpoint");
        const auto member_span = span(current());
        const auto member_name = semantic.find_string(token_text(current()));
        const auto index = member_name
            ? lookup_visible_member(object.named_type, member_name, lookup_offset)
            : member_index{};
        if (!index) {
            return fail(diagnostics::parser_semantic_resolution_failed, member_span,
                "link endpoint member is not visible", status_code::not_found);
        }
        output = {object.identity, index};
        advance();
        return {};
    }

    [[nodiscard]] status parse_link(identity_ref scope) {
        const auto start = current().offset;
        const auto lookup_offset = current().offset;
        source_object_endpoint_fact target_endpoint;
        auto result = parse_endpoint(scope, lookup_offset, target_endpoint);
        if (!result.ok())
            return result;
        if (!punctuation(parser_punctuation::equal))
            return fail_syntax(span(current()), "expected '=' between link endpoints");
        advance();
        source_object_endpoint_fact source_endpoint;
        result = parse_endpoint(scope, lookup_offset, source_endpoint);
        if (!result.ok())
            return result;
        if (!punctuation(parser_punctuation::semicolon))
            return fail_syntax(span(current()), "expected ';' after link");
        const auto end = current().offset + current().length;
        advance();

        const auto link_index = static_cast<std::uint32_t>(candidate.links.size());
        const auto declaration = source_span{start, end - start};
        candidate.links.push_back(source_link_fact{source_endpoint, target_endpoint, declaration});
        append_declaration(source_declaration_kind::link, link_index, declaration);
        return {};
    }

    [[nodiscard]] status parse_enum(identity_ref scope) {
        const auto start = current().offset;
        advance();
        bool scoped = false;
        if (current().kind == parser_token_kind::keyword_class || current().kind == parser_token_kind::keyword_struct) {
            scoped = true;
            advance();
        }
        if (current().kind != parser_token_kind::identifier)
            return fail_unsupported(span(current()), "anonymous enums are not implemented in the V3 identity slice");

        {
            const auto alias_conflict = check_alias_conflict(scope, token_text(current()));
            if (!alias_conflict.ok())
                return alias_conflict;
        }
        const auto name_span = span(current());
        identity_ref identity = nullptr;
        auto result = semantic.resolve_declaration(scope, token_text(current()), identity_kind::type, identity);
        if (!result.ok()) {
            return fail(diagnostics::parser_semantic_resolution_failed, name_span,
                "failed to resolve enum declaration", result.code);
        }
        result = bindings.insert(identity);
        if (!result.ok())
            return fail(diagnostics::parser_semantic_resolution_failed, name_span,
                "local enum binding conflicts with canonical project identity", result.code);

        const auto enum_index = static_cast<std::uint32_t>(candidate.enums.size());
        candidate.enums.push_back(source_enum_fact{identity});
        const auto sequence_index = candidate.declarations.size();
        append_declaration(source_declaration_kind::enum_type, enum_index, {});
        advance();

        intrinsic_type underlying = intrinsic_type::none;
        source_span underlying_span{};
        if (punctuation(parser_punctuation::colon)) {
            advance();
            const auto underlying_start = current().offset;
            std::uint32_t underlying_end = underlying_start;
            identity_ref semantic_type = nullptr;
            result = parse_type_base(scope, semantic_type, underlying, underlying_end, current().offset, candidate.modifiers);
            if (!result.ok())
                return result;
            if (semantic_type != nullptr || !integral_intrinsic(underlying))
                return fail_unsupported(source_span{underlying_start, underlying_end - underlying_start},
                    "enum underlying type must be an intrinsic integral type");
            underlying_span = source_span{underlying_start, underlying_end - underlying_start};
        }

        if (punctuation(parser_punctuation::semicolon)) {
            if (!scoped && underlying == intrinsic_type::none)
                return fail_syntax(span(current()), "unscoped enum forward declaration requires an explicit underlying type");
            const auto end = current().offset + current().length;
            const auto declaration = source_span{start, end - start};
            auto& fact = candidate.enums[enum_index];
            fact.declaration = declaration;
            fact.underlying_spelling = underlying_span;
            fact.explicit_underlying = underlying;
            fact.scoped = scoped;
            candidate.declarations[sequence_index].declaration = declaration;
            advance();
            return {};
        }
        if (!punctuation(parser_punctuation::left_brace))
            return fail_syntax(span(current()), "expected '{' or ';' after enum declaration");

        const auto value_begin = candidate.enum_values.size();
        advance();
        std::int64_t implicit_value = 0;
        bool have_value = false;
        while (current().kind != parser_token_kind::eof &&
               !punctuation(parser_punctuation::right_brace)) {
            if (current().kind != parser_token_kind::identifier)
                return fail_syntax(span(current()), "expected enumerator identifier");
            const auto enumerator_span = span(current());
            string_id enumerator_name;
            result = semantic.intern_string(token_text(current()), enumerator_name);
            if (!result.ok())
                return result;
            advance();

            source_integral_constant value{};
            source_span expression{};
            if (punctuation(parser_punctuation::equal)) {
                advance();
                bool negative = false;
                const auto expression_start = current().offset;
                if (punctuation(parser_punctuation::plus) || punctuation(parser_punctuation::minus)) {
                    negative = punctuation(parser_punctuation::minus);
                    advance();
                }
                if (current().kind != parser_token_kind::integer_literal)
                    return fail_unsupported(span(current()), "enum expressions currently support decimal integer literals only");
                std::uint64_t magnitude = 0;
                const auto number = token_text(current());
                const auto conversion = std::from_chars(number.data(), number.data() + number.size(), magnitude);
                if (conversion.ec != std::errc{} || conversion.ptr != number.data() + number.size())
                    return fail_syntax(span(current()), "invalid decimal enum literal");
                const auto expression_end = current().offset + current().length;
                expression = source_span{expression_start, expression_end - expression_start};
                if (negative) {
                    if (magnitude > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1ULL)
                        return fail_unsupported(expression, "negative enum literal exceeds signed 64-bit range");
                    const auto signed_value = magnitude == (1ULL << 63)
                        ? (std::numeric_limits<std::int64_t>::min)()
                        : -static_cast<std::int64_t>(magnitude);
                    value.intrinsic = (signed_value >= (std::numeric_limits<std::int32_t>::min)() &&
                                       signed_value <= (std::numeric_limits<std::int32_t>::max)())
                        ? intrinsic_type::signed_int
                        : intrinsic_type::signed_long_long;
                    value.bits = static_cast<std::uint64_t>(signed_value);
                    implicit_value = signed_value;
                }
                else {
                    if (magnitude <= static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())) {
                        value.intrinsic = intrinsic_type::signed_int;
                        implicit_value = static_cast<std::int64_t>(magnitude);
                    }
                    else if (magnitude <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
                        value.intrinsic = intrinsic_type::signed_long_long;
                        implicit_value = static_cast<std::int64_t>(magnitude);
                    }
                    else {
                        value.intrinsic = intrinsic_type::unsigned_long_long;
                        implicit_value = 0;
                    }
                    value.bits = magnitude;
                }
                have_value = true;
                advance();
            }
            else {
                if (have_value && implicit_value == (std::numeric_limits<std::int64_t>::max)())
                    return fail_unsupported(enumerator_span, "implicit enum value exceeds signed 64-bit range");
                if (have_value)
                    ++implicit_value;
                else
                    implicit_value = 0;
                have_value = true;
                value.intrinsic = (implicit_value >= (std::numeric_limits<std::int32_t>::min)() &&
                                   implicit_value <= (std::numeric_limits<std::int32_t>::max)())
                    ? intrinsic_type::signed_int
                    : intrinsic_type::signed_long_long;
                value.bits = static_cast<std::uint64_t>(implicit_value);
            }

            candidate.enum_values.push_back(source_enum_value_fact{enumerator_name, value, expression});
            if (punctuation(parser_punctuation::comma)) {
                advance();
                continue;
            }
            if (!punctuation(parser_punctuation::right_brace))
                return fail_syntax(span(current()), "expected ',' or '}' after enumerator");
        }

        if (!punctuation(parser_punctuation::right_brace))
            return fail_syntax(span(current()), "expected '}' after enum definition");
        advance();
        if (!punctuation(parser_punctuation::semicolon))
            return fail_syntax(span(current()), "expected ';' after enum definition");
        const auto end = current().offset + current().length;
        advance();

        const auto value_count = candidate.enum_values.size() - value_begin;
        if (value_begin > (std::numeric_limits<std::uint32_t>::max)() ||
            value_count > (std::numeric_limits<std::uint32_t>::max)())
            return {status_code::not_available};
        const auto declaration = source_span{start, end - start};
        auto& fact = candidate.enums[enum_index];
        fact.enumerators = source_fact_range{static_cast<std::uint32_t>(value_begin), static_cast<std::uint32_t>(value_count)};
        fact.declaration = declaration;
        fact.underlying_spelling = underlying_span;
        fact.explicit_underlying = underlying;
        fact.declaration_kind = source_enum_declaration_kind::definition;
        fact.scoped = scoped;
        candidate.declarations[sequence_index].declaration = declaration;
        return {};
    }

    [[nodiscard]] status parse_member(identity_ref scope, source_member_access access) {
        const auto declaration_start = current().offset;
        const auto type_start = current().offset;
        const auto modifier_begin = candidate.modifiers.size();
        std::uint32_t type_end = type_start;

        while (identifier("const") || identifier("volatile")) {
            candidate.modifiers.push_back(source_type_modifier{
                0,
                identifier("const") ? source_type_modifier_kind::const_qualified
                                    : source_type_modifier_kind::volatile_qualified,
            });
            type_end = current().offset + current().length;
            advance();
        }

        identity_ref semantic_type = nullptr;
        intrinsic_type intrinsic = intrinsic_type::none;
        auto result = parse_type_base(scope, semantic_type, intrinsic, type_end, declaration_start, candidate.modifiers);
        if (!result.ok())
            return result;

        while (identifier("const") || identifier("volatile")) {
            candidate.modifiers.push_back(source_type_modifier{
                0,
                identifier("const") ? source_type_modifier_kind::const_qualified
                                    : source_type_modifier_kind::volatile_qualified,
            });
            type_end = current().offset + current().length;
            advance();
        }

        for (;;) {
            if (punctuation(parser_punctuation::asterisk)) {
                candidate.modifiers.push_back(source_type_modifier{0, source_type_modifier_kind::pointer});
                type_end = current().offset + current().length;
                advance();
                while (identifier("const") || identifier("volatile")) {
                    candidate.modifiers.push_back(source_type_modifier{
                        0,
                        identifier("const") ? source_type_modifier_kind::const_qualified
                                            : source_type_modifier_kind::volatile_qualified,
                    });
                    type_end = current().offset + current().length;
                    advance();
                }
                continue;
            }
            if (punctuation(parser_punctuation::ampersand)) {
                candidate.modifiers.push_back(source_type_modifier{0, source_type_modifier_kind::lvalue_reference});
                type_end = current().offset + current().length;
                advance();
                continue;
            }
            if (punctuation(parser_punctuation::ampersand_ampersand)) {
                candidate.modifiers.push_back(source_type_modifier{0, source_type_modifier_kind::rvalue_reference});
                type_end = current().offset + current().length;
                advance();
                continue;
            }
            break;
        }

        // Declarator list: `name [arrays] [= init] (, ...)? ;`
        // The shared type/pointer modifiers are duplicated for every declarator
        // after the first so each member owns a contiguous modifier range
        // excluding its siblings' array suffixes. Bit-fields are rejected
        // explicitly: width is layout semantics, never dropped silently.
        const auto shared_end = candidate.modifiers.size();
        bool first_declarator = true;
        for (;;) {
            if (punctuation(parser_punctuation::colon)) {
                return fail_unsupported(span(current()),
                    "bit-fields are not implemented: width is layout semantics");
            }

            if (current().kind != parser_token_kind::identifier)
                return fail_syntax(span(current()), "expected non-static data member identifier");
            string_id member_name;
            result = semantic.intern_string(token_text(current()), member_name);
            if (!result.ok())
                return result;
            advance();

            std::size_t declarator_begin = modifier_begin;
            if (!first_declarator) {
                try {
                    candidate.modifiers.reserve(shared_end + (shared_end - modifier_begin));
                    declarator_begin = candidate.modifiers.size();
                    for (std::size_t index = modifier_begin; index < shared_end; ++index)
                        candidate.modifiers.push_back(candidate.modifiers[index]);
                }
                catch (const std::bad_alloc&) {
                    return {status_code::not_available};
                }
                catch (const std::length_error&) {
                    return {status_code::not_available};
                }
            }

            result = parse_array_suffix(candidate.modifiers, true);
            if (!result.ok())
                return result;

            if (punctuation(parser_punctuation::colon)) {
                return fail_unsupported(span(current()),
                    "bit-fields are not implemented: width is layout semantics");
            }

            bool comma = false;
            if (punctuation(parser_punctuation::equal)) {
                advance();
                if (!skip_declarator_initializer(comma))
                    return fail_syntax(span(current()), "expected ',' or ';' after member initializer");
            }
            else if (punctuation(parser_punctuation::comma)) {
                comma = true;
            }
            else if (!punctuation(parser_punctuation::semicolon)) {
                return fail_unsupported(span(current()), "methods, constructors, and function-style declarators are not implemented");
            }

            const auto declaration_end = current().offset + current().length;

            const auto modifier_count = candidate.modifiers.size() - declarator_begin;
            if (declarator_begin > (std::numeric_limits<std::uint32_t>::max)() ||
                modifier_count > (std::numeric_limits<std::uint32_t>::max)())
                return {status_code::not_available};

            const auto spelling = source_span{type_start, type_end - type_start};
            const auto modifier_range = source_fact_range{
                static_cast<std::uint32_t>(declarator_begin),
                static_cast<std::uint32_t>(modifier_count),
            };
            const auto type = semantic_type != nullptr
                ? source_type_ref::semantic(semantic_type, modifier_range, spelling)
                : source_type_ref::builtin(intrinsic, modifier_range, spelling);

            candidate.members.push_back(source_member_fact{
                type,
                member_name,
                source_span{declaration_start, declaration_end - declaration_start},
                access,
            });

            if (!comma) {
                advance();
                return {};
            }
            advance();
            first_declarator = false;
        }
    }

    [[nodiscard]] identity_ref lookup_visible_type(
        identity_ref scope,
        string_id name,
        std::uint32_t source_offset) const noexcept {

        auto current_scope = scope;
        while (current_scope != nullptr) {
            if (const auto identity = bindings.find(current_scope, name); identity != nullptr)
                return identity;
            if (const auto identity = environment.find_type(current_scope, name, source_offset); identity != nullptr)
                return identity;
            current_scope = semantic.identities().parent(current_scope);
        }
        return nullptr;
    }

    [[nodiscard]] status parse_type_base(
        identity_ref scope,
        identity_ref& semantic_type,
        intrinsic_type& intrinsic,
        std::uint32_t& type_end,
        std::uint32_t lookup_offset,
        std::vector<source_type_modifier>& modifier_out) {

        semantic_type = nullptr;
        intrinsic = intrinsic_type::none;

        if (current().kind == parser_token_kind::keyword_struct ||
            current().kind == parser_token_kind::keyword_class ||
            current().kind == parser_token_kind::keyword_union ||
            current().kind == parser_token_kind::keyword_enum) {
            advance();
            if (current().kind != parser_token_kind::identifier)
                return fail_syntax(span(current()), "expected type identifier after elaborated type specifier");
            const auto name = semantic.find_string(token_text(current()));
            semantic_type = name ? lookup_visible_type(scope, name, lookup_offset) : nullptr;
            if (semantic_type == nullptr)
                return fail(diagnostics::parser_unresolved_type, span(current()),
                    "elaborated type name is not visible", status_code::not_found);
            type_end = current().offset + current().length;
            advance();
            return {};
        }

        if (current().kind != parser_token_kind::identifier)
            return fail_syntax(span(current()), "expected member type");

        const auto first = token_text(current());
        type_end = current().offset + current().length;

        if (first == "signed" || first == "unsigned") {
            const bool unsigned_value = first == "unsigned";
            advance();
            if (identifier("char")) {
                intrinsic = unsigned_value ? intrinsic_type::unsigned_char : intrinsic_type::signed_char;
                type_end = current().offset + current().length;
                advance();
                return {};
            }
            if (identifier("short")) {
                intrinsic = unsigned_value ? intrinsic_type::unsigned_short : intrinsic_type::signed_short;
                type_end = current().offset + current().length;
                advance();
                if (identifier("int")) { type_end = current().offset + current().length; advance(); }
                return {};
            }
            if (identifier("long")) {
                intrinsic = unsigned_value ? intrinsic_type::unsigned_long : intrinsic_type::signed_long;
                type_end = current().offset + current().length;
                advance();
                if (identifier("long")) {
                    intrinsic = unsigned_value ? intrinsic_type::unsigned_long_long : intrinsic_type::signed_long_long;
                    type_end = current().offset + current().length;
                    advance();
                }
                if (identifier("int")) { type_end = current().offset + current().length; advance(); }
                return {};
            }
            intrinsic = unsigned_value ? intrinsic_type::unsigned_int : intrinsic_type::signed_int;
            if (identifier("int")) { type_end = current().offset + current().length; advance(); }
            return {};
        }

        if (first == "void") intrinsic = intrinsic_type::void_type;
        else if (first == "bool") intrinsic = intrinsic_type::bool_type;
        else if (first == "char") intrinsic = intrinsic_type::char_type;
        else if (first == "wchar_t") intrinsic = intrinsic_type::wchar_type;
        else if (first == "char8_t") intrinsic = intrinsic_type::char8_type;
        else if (first == "char16_t") intrinsic = intrinsic_type::char16_type;
        else if (first == "char32_t") intrinsic = intrinsic_type::char32_type;
        else if (first == "float") intrinsic = intrinsic_type::float_type;
        else if (first == "double") intrinsic = intrinsic_type::double_type;
        else if (first == "int") intrinsic = intrinsic_type::signed_int;
        else if (first == "short") intrinsic = intrinsic_type::signed_short;
        else if (first == "long") intrinsic = intrinsic_type::signed_long;
        else {
            const auto name = semantic.find_string(first);
            if (name) {
                if (const auto* alias = find_visible_alias(scope, name)) {
                    // A source-local alias resolves through to its target.
                    // Target modifiers are copied into the caller's own type
                    // range, preserving the dense member-modifier partition.
                    semantic_type = alias->target_identity;
                    intrinsic = alias->target_intrinsic;
                    try {
                        const auto& range = alias->target_modifiers;
                        modifier_out.reserve(
                            modifier_out.size() + range.count);
                        for (std::uint32_t index = 0; index < range.count; ++index)
                            modifier_out.push_back(
                                candidate.alias_modifiers[range.begin + index]);
                    }
                    catch (const std::bad_alloc&) {
                        return {status_code::not_available};
                    }
                    catch (const std::length_error&) {
                        return {status_code::not_available};
                    }
                    type_end = current().offset + current().length;
                    advance();
                    return {};
                }
            }
            semantic_type = name ? lookup_visible_type(scope, name, lookup_offset) : nullptr;
            if (semantic_type == nullptr)
                return fail(diagnostics::parser_unresolved_type, span(current()),
                    "type name is not visible in the Source semantic environment", status_code::not_found);
            advance();
            return {};
        }

        advance();
        if (intrinsic == intrinsic_type::signed_short && identifier("int")) {
            type_end = current().offset + current().length;
            advance();
        }
        else if (intrinsic == intrinsic_type::signed_long) {
            if (identifier("double")) {
                intrinsic = intrinsic_type::long_double_type;
                type_end = current().offset + current().length;
                advance();
            }
            else {
                if (identifier("long")) {
                    intrinsic = intrinsic_type::signed_long_long;
                    type_end = current().offset + current().length;
                    advance();
                }
                if (identifier("int")) {
                    type_end = current().offset + current().length;
                    advance();
                }
            }
        }
        return {};
    }

    project_semantic_services semantic;
    const source_snapshot& source;
    std::string_view text;
    std::span<const parser_token> tokens;
    const source_environment& environment;
    operation_id operation;
    diagnostic_buffer& diagnostics;
    parsed_source candidate;
    binding_index bindings;
    object_binding_index object_bindings;
    member_binding_index member_bindings;
    std::vector<alias_entry> aliases;
    std::size_t cursor = 0;
};

status source_parser::parse(
    const source_snapshot& source,
    std::span<const parser_token> tokens,
    const source_environment& environment,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    parsed_source& output) const noexcept {

    try {
        source_parser_state state{semantic, source, tokens, environment, operation, diagnostics};
        return state.run(output);
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

} // namespace cw::server
