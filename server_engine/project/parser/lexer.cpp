#include "lexer.hpp"

#include "../../diagnostics/diagnostic_descriptor.hpp"

#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>

namespace cw::server {
namespace {

[[nodiscard]] constexpr bool identifier_start(char value) noexcept {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') || value == '_';
}

[[nodiscard]] constexpr bool decimal_digit(char value) noexcept {
    return value >= '0' && value <= '9';
}

[[nodiscard]] parser_token_kind identifier_kind(std::string_view text) noexcept {
    if (text == "namespace") return parser_token_kind::keyword_namespace;
    if (text == "enum") return parser_token_kind::keyword_enum;
    if (text == "class") return parser_token_kind::keyword_class;
    if (text == "struct") return parser_token_kind::keyword_struct;
    if (text == "union") return parser_token_kind::keyword_union;
    return parser_token_kind::identifier;
}

[[nodiscard]] status fail(
    const source_snapshot& source,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    std::size_t offset,
    std::size_t length,
    std::string detail = {}) noexcept {

    try {
        diagnostics.emit(diagnostic_record{
            diagnostics::parser_syntax_error.id,
            diagnostics::parser_syntax_error.default_severity,
            operation,
            source_range{
                source.source(),
                static_cast<std::uint32_t>(offset),
                static_cast<std::uint32_t>(length),
            },
            std::move(detail),
        });
    }
    catch (...) {
        return {status_code::not_available};
    }
    return {status_code::configuration_failed};
}

} // namespace

status lex_source(
    const source_snapshot& source,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    std::vector<parser_token>& output,
    std::vector<directive_span>* directives) noexcept {

    output.clear();
    if (directives != nullptr)
        directives->clear();
    if (!source)
        return {status_code::invalid_argument};

    const auto bytes = source.text();
    if (bytes.size() > (std::numeric_limits<std::uint32_t>::max)())
        return fail(source, operation, diagnostics, 0, 0, "Source exceeds 32-bit token range");

    try {
        bool line_start = true;
        bool directive_open = false;
        std::uint32_t directive_begin = 0;

        for (std::size_t index = 0; index < bytes.size();) {
            const auto character = bytes[index];
            if (character == ' ' || character == '\t' || character == '\r' ||
                character == '\f' || character == '\v') {
                ++index;
                continue;
            }
            if (character == '\n') {
                if (directive_open && directives != nullptr) {
                    directives->push_back(directive_span{
                        directive_begin,
                        static_cast<std::uint32_t>(output.size()),
                    });
                }
                directive_open = false;
                line_start = true;
                ++index;
                continue;
            }
            if (character == '/' && index + 1 < bytes.size() && bytes[index + 1] == '/') {
                index += 2;
                while (index < bytes.size() && bytes[index] != '\n')
                    ++index;
                continue;
            }
            if (character == '/' && index + 1 < bytes.size() && bytes[index + 1] == '*') {
                const auto start = index;
                index += 2;
                bool closed = false;
                while (index + 1 < bytes.size()) {
                    if (bytes[index] == '\n')
                        line_start = true;
                    if (bytes[index] == '*' && bytes[index + 1] == '/') {
                        index += 2;
                        closed = true;
                        break;
                    }
                    ++index;
                }
                if (!closed)
                    return fail(source, operation, diagnostics, start, bytes.size() - start, "unterminated block comment");
                continue;
            }

            const auto start = index;
            parser_token token;
            token.offset = static_cast<std::uint32_t>(start);
            token.flags = line_start ? 1U : 0U;
            line_start = false;

            if (identifier_start(character)) {
                ++index;
                while (index < bytes.size() &&
                       (identifier_start(bytes[index]) || decimal_digit(bytes[index]))) {
                    ++index;
                }
                token.kind = identifier_kind(bytes.substr(start, index - start));
            }
            else if (decimal_digit(character)) {
                ++index;
                while (index < bytes.size() && decimal_digit(bytes[index]))
                    ++index;
                token.kind = parser_token_kind::integer_literal;
            }
            else if (character == '"') {
                ++index;
                while (index < bytes.size() && bytes[index] != '"' && bytes[index] != '\n') {
                    if (bytes[index] == '\\' && index + 1 < bytes.size())
                        index += 2;
                    else
                        ++index;
                }
                if (index >= bytes.size() || bytes[index] != '"')
                    return fail(source, operation, diagnostics, start, index - start, "unterminated string literal");
                ++index;
                token.kind = parser_token_kind::string_literal;
            }
            else if (character == '\'') {
                // Character literal. Interpreted nowhere yet (initializers and
                // bodies only skip it); digit separators (`1'000`) and raw
                // strings stay unsupported and fail here loudly.
                ++index;
                while (index < bytes.size() && bytes[index] != '\'' && bytes[index] != '\n') {
                    if (bytes[index] == '\\' && index + 1 < bytes.size())
                        index += 2;
                    else
                        ++index;
                }
                if (index >= bytes.size() || bytes[index] != '\'')
                    return fail(source, operation, diagnostics, start, index - start, "unterminated character literal");
                ++index;
                token.kind = parser_token_kind::character_literal;
            }
            else {
                token.kind = parser_token_kind::punctuation;
                switch (character) {
                case '{': token.punctuation = parser_punctuation::left_brace; ++index; break;
                case '}': token.punctuation = parser_punctuation::right_brace; ++index; break;
                case '[': token.punctuation = parser_punctuation::left_bracket; ++index; break;
                case ']': token.punctuation = parser_punctuation::right_bracket; ++index; break;
                case ';': token.punctuation = parser_punctuation::semicolon; ++index; break;
                case ':': token.punctuation = parser_punctuation::colon; ++index; break;
                case ',': token.punctuation = parser_punctuation::comma; ++index; break;
                case '.': token.punctuation = parser_punctuation::dot; ++index; break;
                case '=': token.punctuation = parser_punctuation::equal; ++index; break;
                case '+': token.punctuation = parser_punctuation::plus; ++index; break;
                case '-': token.punctuation = parser_punctuation::minus; ++index; break;
                case '#': token.punctuation = parser_punctuation::hash; ++index; break;
                case '*': token.punctuation = parser_punctuation::asterisk; ++index; break;
                case '(': token.punctuation = parser_punctuation::left_parenthesis; ++index; break;
                case ')': token.punctuation = parser_punctuation::right_parenthesis; ++index; break;
                case '<': token.punctuation = parser_punctuation::less; ++index; break;
                case '>': token.punctuation = parser_punctuation::greater; ++index; break;
                case '!': token.punctuation = parser_punctuation::bang; ++index; break;
                case '%': token.punctuation = parser_punctuation::percent; ++index; break;
                case '^': token.punctuation = parser_punctuation::caret; ++index; break;
                case '|': token.punctuation = parser_punctuation::pipe; ++index; break;
                case '~': token.punctuation = parser_punctuation::tilde; ++index; break;
                case '?': token.punctuation = parser_punctuation::question; ++index; break;
                case '/': token.punctuation = parser_punctuation::slash; ++index; break;
                case '&':
                    ++index;
                    if (index < bytes.size() && bytes[index] == '&') {
                        ++index;
                        token.punctuation = parser_punctuation::ampersand_ampersand;
                    }
                    else {
                        token.punctuation = parser_punctuation::ampersand;
                    }
                    break;
                default:
                    return fail(source, operation, diagnostics, start, 1, "unsupported lexical character");
                }
            }

            token.length = static_cast<std::uint32_t>(index - start);
            if (token.punctuation == parser_punctuation::hash &&
                (token.flags & 1U) != 0 && directives != nullptr) {
                directive_begin = static_cast<std::uint32_t>(output.size());
                directive_open = true;
            }
            output.push_back(token);
        }

        if (directive_open && directives != nullptr) {
            directives->push_back(directive_span{
                directive_begin,
                static_cast<std::uint32_t>(output.size()),
            });
        }
        output.push_back(parser_token{
            static_cast<std::uint32_t>(bytes.size()),
            0,
            parser_token_kind::eof,
            parser_punctuation::none,
            0,
        });
        return {};
    }
    catch (const std::bad_alloc&) {
        output.clear();
        if (directives != nullptr)
            directives->clear();
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        output.clear();
        if (directives != nullptr)
            directives->clear();
        return {status_code::not_available};
    }
}

} // namespace cw::server
