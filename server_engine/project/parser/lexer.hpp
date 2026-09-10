#pragma once

#include "token.hpp"
#include "../source/source_snapshot.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"
#include "../../operation.hpp"
#include "../../status.hpp"

#include <vector>

namespace cw::server {

// Half-open token range occupied by one preprocessing directive.
struct directive_span final {
    std::uint32_t token_begin = 0;
    std::uint32_t token_end = 0;
};

// Lexes immutable Source bytes. Directive spans are reported but not interpreted;
// Source frontend owns preprocessing/include discovery and removes them before Parser.
[[nodiscard]] status lex_source(
    const source_snapshot& source,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    std::vector<parser_token>& output,
    std::vector<directive_span>* directives = nullptr) noexcept;

} // namespace cw::server
