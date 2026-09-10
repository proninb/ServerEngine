#include "include_discovery.hpp"

#include "../../diagnostics/diagnostic_descriptor.hpp"

#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cw::server {
namespace {

[[nodiscard]] std::string_view token_text(
    const source_snapshot& source,
    const parser_token& token) noexcept {

    const auto text = source.text();
    if (token.offset > text.size() || token.length > text.size() - token.offset)
        return {};
    return text.substr(token.offset, token.length);
}

[[nodiscard]] status fail(
    const source_snapshot& source,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    const parser_token& token,
    std::string_view detail) noexcept {

    try {
        diagnostics.emit(diagnostic_record{
            diagnostics::source_unsupported_directive.id,
            diagnostics::source_unsupported_directive.default_severity,
            operation,
            source_range{source.source(), token.offset, token.length},
            std::string{detail},
        });
    }
    catch (...) {
    }
    return {status_code::not_available};
}

[[nodiscard]] bool punctuation(const parser_token& token, parser_punctuation value) noexcept {
    return token.kind == parser_token_kind::punctuation && token.punctuation == value;
}

} // namespace

status discover_source_includes(
    const source_snapshot& source,
    std::vector<parser_token>& tokens,
    std::span<const directive_span> directives,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    std::vector<source_include_directive>& includes) noexcept {

    includes.clear();
    if (!source || tokens.empty() || tokens.back().kind != parser_token_kind::eof)
        return {status_code::invalid_argument};

    try {
        std::size_t scan = 0;
        std::uint32_t brace_depth = 0;
        for (const auto directive : directives) {
            if (directive.token_begin >= directive.token_end ||
                directive.token_end > tokens.size() - 1 || directive.token_begin < scan) {
                return {status_code::invalid_argument};
            }

            while (scan < directive.token_begin) {
                const auto& token = tokens[scan++];
                if (punctuation(token, parser_punctuation::left_brace))
                    ++brace_depth;
                else if (punctuation(token, parser_punctuation::right_brace)) {
                    if (brace_depth == 0)
                        return {status_code::invalid_argument};
                    --brace_depth;
                }
            }

            const auto count = directive.token_end - directive.token_begin;
            const auto& hash = tokens[directive.token_begin];
            if (!punctuation(hash, parser_punctuation::hash) || (hash.flags & 1U) == 0)
                return {status_code::invalid_argument};
            if (count < 2)
                return fail(source, operation, diagnostics, hash, "empty preprocessing directive is unsupported");

            const auto& name = tokens[directive.token_begin + 1];
            const auto name_text = token_text(source, name);
            if (name.kind != parser_token_kind::identifier)
                return fail(source, operation, diagnostics, name, "expected preprocessing directive name");

            if (name_text == "include") {
                if (brace_depth != 0)
                    return fail(source, operation, diagnostics, hash, "#include inside a brace/namespace scope is forbidden");
                if (count != 3)
                    return fail(source, operation, diagnostics, hash, "only quoted #include with one header-name is supported");
                const auto& header = tokens[directive.token_begin + 2];
                if (header.kind != parser_token_kind::string_literal || header.length < 2)
                    return fail(source, operation, diagnostics, header, "only quoted #include \"file.hpp\" is supported");
                includes.push_back(source_include_directive{
                    source_span{header.offset + 1, header.length - 2},
                    header.offset + header.length,
                });
            }
            else if (name_text == "pragma") {
                if (count != 3 || token_text(source, tokens[directive.token_begin + 2]) != "once")
                    return fail(source, operation, diagnostics, hash, "only #pragma once is supported");
            }
            else {
                return fail(source, operation, diagnostics, name,
                    "supported preprocessing directives are #include \"...\" and #pragma once only");
            }
            scan = directive.token_end;
        }

        std::size_t write = 0;
        std::size_t directive_index = 0;
        for (std::size_t read = 0; read + 1 < tokens.size();) {
            if (directive_index < directives.size() && read == directives[directive_index].token_begin) {
                read = directives[directive_index].token_end;
                ++directive_index;
                continue;
            }
            tokens[write++] = tokens[read++];
        }
        tokens[write++] = tokens.back();
        tokens.resize(write);
        return {};
    }
    catch (const std::bad_alloc&) {
        includes.clear();
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        includes.clear();
        return {status_code::not_available};
    }
}

} // namespace cw::server
