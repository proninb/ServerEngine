#pragma once

#include "source_environment.hpp"
#include "token.hpp"
#include "../frontend/parsed_source.hpp"
#include "../project_context.hpp"
#include "../../diagnostics/diagnostic_buffer.hpp"
#include "../../operation.hpp"
#include "../../status.hpp"

#include <span>

namespace cw::server {

// Native V3 Parser backend. It consumes already lexed immutable Source tokens,
// performs exactly one source-language lookup per referenced type, and emits direct
// project identity_ref values in source_facts. It owns no filesystem/preprocessor work.
class source_parser final {
public:
    explicit source_parser(project_semantic_services semantic_value) noexcept
        : semantic(semantic_value) {}

    [[nodiscard]] status parse(
        const source_snapshot& source,
        std::span<const parser_token> tokens,
        const source_environment& environment,
        operation_id operation,
        diagnostic_buffer& diagnostics,
        parsed_source& output) const noexcept;

private:
    project_semantic_services semantic;
};

} // namespace cw::server
