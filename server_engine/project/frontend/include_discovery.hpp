#pragma once

#include "source_facts.hpp"
#include "../parser/lexer.hpp"
#include "../source/source_snapshot.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"
#include "../../operation.hpp"
#include "../../status.hpp"

#include <vector>

namespace cw::server {

struct source_include_directive final {
    source_span path{};
    std::uint32_t visible_from = 0;
};

// Interprets the intentionally small preprocessor subset owned by Source frontend:
// quoted #include and #pragma once. Directive tokens are compacted out in-place so
// Parser never receives preprocessing syntax.
[[nodiscard]] status discover_source_includes(
    const source_snapshot& source,
    std::vector<parser_token>& tokens,
    std::span<const directive_span> directives,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    std::vector<source_include_directive>& includes) noexcept;

} // namespace cw::server
