#pragma once

#include "source_facts.hpp"
#include "../source/source_snapshot.hpp"

#include <utility>
#include <vector>

namespace cw::server {

class source_parser;
class source_parser_state;

// Owns one Parser result and the immutable Source snapshot backing every source
// range. facts() exposes only the frozen flat Parser -> Builder view.
class parsed_source final {
public:
    parsed_source() = default;
    parsed_source(const parsed_source&) = delete;
    parsed_source& operator=(const parsed_source&) = delete;
    parsed_source(parsed_source&&) noexcept = default;
    parsed_source& operator=(parsed_source&&) noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(snapshot); }

    [[nodiscard]] source_facts facts() const noexcept {
        return source_facts{
            snapshot.source(), snapshot.text(), namespaces, records, members, modifiers,
            enums, enum_values, declarations,
        };
    }

    [[nodiscard]] const source_snapshot& source() const noexcept { return snapshot; }

private:
    friend class source_parser;
    friend class source_parser_state;

    source_snapshot snapshot;
    std::vector<source_namespace_fact> namespaces;
    std::vector<source_record_fact> records;
    std::vector<source_member_fact> members;
    std::vector<source_type_modifier> modifiers;
    std::vector<source_enum_fact> enums;
    std::vector<source_enum_value_fact> enum_values;
    std::vector<source_declaration_ref> declarations;
};

} // namespace cw::server
