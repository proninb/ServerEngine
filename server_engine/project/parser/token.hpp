#pragma once

#include <cstdint>

namespace cw::server {

// Compact lexical categories produced from one immutable Source snapshot.
enum class parser_token_kind : std::uint8_t {
    eof,
    identifier,
    integer_literal,
    keyword_namespace,
    keyword_enum,
    keyword_class,
    keyword_struct,
    keyword_union,
    string_literal,
    punctuation,
};

enum class parser_punctuation : std::uint8_t {
    none,
    left_brace,
    right_brace,
    left_bracket,
    right_bracket,
    semicolon,
    colon,
    comma,
    dot,
    equal,
    plus,
    minus,
    hash,
    ampersand,
    ampersand_ampersand,
    asterisk,
    left_parenthesis,
    right_parenthesis,
};

// offset/length address the Source snapshot. flags bit 0 marks the first
// non-trivia token on a physical source line.
struct parser_token final {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    parser_token_kind kind = parser_token_kind::eof;
    parser_punctuation punctuation = parser_punctuation::none;
    std::uint8_t flags = 0;
};

static_assert(sizeof(parser_token) <= 12);

} // namespace cw::server
