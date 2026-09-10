#include "json_parser.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <new>
#include <string>

namespace cw::server {
namespace {

constexpr std::size_t max_json_depth = 64;

[[nodiscard]] constexpr bool is_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

[[nodiscard]] constexpr bool is_digit(char value) noexcept {
    return value >= '0' && value <= '9';
}

[[nodiscard]] constexpr int hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

void append_utf8(std::string& output, std::uint32_t codepoint) {
    if (codepoint <= 0x7F) {
        output.push_back(static_cast<char>(codepoint));
    }
    else if (codepoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    else if (codepoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    else {
        output.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

class parser final {
public:
    parser(std::string_view text, json_event_handler& handler) noexcept
        : text(text), handler(handler) {}

    [[nodiscard]] json_parse_result run() {
        skip_space();
        if (!parse_value(0)) return result;
        skip_space();
        if (position != text.size()) static_cast<void>(fail(json_error_code::unexpected_token));
        return result;
    }

private:
    [[nodiscard]] bool parse_value(std::size_t depth) {
        skip_space();
        if (position >= text.size()) return fail(json_error_code::unexpected_end);
        handler.location(position);

        switch (text[position]) {
        case '{': return parse_object(depth);
        case '[': return parse_array(depth);
        case '"': {
            std::string value;
            if (!parse_string(value)) return false;
            handler.string(value);
            return true;
        }
        case 't': return parse_literal("true", [&] { handler.boolean(true); });
        case 'f': return parse_literal("false", [&] { handler.boolean(false); });
        case 'n': return parse_literal("null", [&] { handler.null(); });
        default:
            if (text[position] == '-' || is_digit(text[position])) return parse_number();
            return fail(json_error_code::unexpected_token);
        }
    }

    [[nodiscard]] bool parse_object(std::size_t depth) {
        if (depth >= max_json_depth) return fail(json_error_code::nesting_too_deep);
        ++position;
        handler.object_begin();
        skip_space();
        if (consume('}')) {
            handler.location(position - 1);
            handler.object_end();
            return true;
        }

        for (;;) {
            skip_space();
            if (position >= text.size()) return fail(json_error_code::unexpected_end);
            handler.location(position);
            std::string key_value;
            if (!parse_string(key_value)) return false;
            handler.key(key_value);
            skip_space();
            if (!consume(':')) return fail(json_error_code::unexpected_token);
            if (!parse_value(depth + 1)) return false;
            skip_space();
            if (consume('}')) {
                handler.location(position - 1);
                handler.object_end();
                return true;
            }
            if (!consume(',')) return fail(json_error_code::unexpected_token);
        }
    }

    [[nodiscard]] bool parse_array(std::size_t depth) {
        if (depth >= max_json_depth) return fail(json_error_code::nesting_too_deep);
        ++position;
        handler.array_begin();
        skip_space();
        if (consume(']')) {
            handler.location(position - 1);
            handler.array_end();
            return true;
        }

        for (;;) {
            if (!parse_value(depth + 1)) return false;
            skip_space();
            if (consume(']')) {
                handler.location(position - 1);
                handler.array_end();
                return true;
            }
            if (!consume(',')) return fail(json_error_code::unexpected_token);
        }
    }

    [[nodiscard]] bool parse_string(std::string& output) {
        if (!consume('"')) return fail(json_error_code::unexpected_token);
        while (position < text.size()) {
            const unsigned char value = static_cast<unsigned char>(text[position++]);
            if (value == '"') return true;
            if (value < 0x20) return fail(json_error_code::invalid_string, position - 1);
            if (value != '\\') {
                output.push_back(static_cast<char>(value));
                continue;
            }
            if (position >= text.size()) return fail(json_error_code::unexpected_end);
            const char escape = text[position++];
            switch (escape) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                std::uint32_t first = 0;
                if (!parse_hex4(first)) return false;
                std::uint32_t codepoint = first;
                if (first >= 0xD800 && first <= 0xDBFF) {
                    if (position + 2 > text.size() || text[position] != '\\' || text[position + 1] != 'u')
                        return fail(json_error_code::invalid_unicode);
                    position += 2;
                    std::uint32_t second = 0;
                    if (!parse_hex4(second)) return false;
                    if (second < 0xDC00 || second > 0xDFFF) return fail(json_error_code::invalid_unicode);
                    codepoint = 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
                }
                else if (first >= 0xDC00 && first <= 0xDFFF) {
                    return fail(json_error_code::invalid_unicode);
                }
                append_utf8(output, codepoint);
                break;
            }
            default: return fail(json_error_code::invalid_escape, position - 1);
            }
        }
        return fail(json_error_code::unexpected_end);
    }

    [[nodiscard]] bool parse_hex4(std::uint32_t& output) {
        if (position + 4 > text.size()) return fail(json_error_code::unexpected_end);
        output = 0;
        for (int i = 0; i < 4; ++i) {
            const int digit = hex_value(text[position++]);
            if (digit < 0) return fail(json_error_code::invalid_unicode, position - 1);
            output = (output << 4) | static_cast<std::uint32_t>(digit);
        }
        return true;
    }

    [[nodiscard]] bool parse_number() {
        const std::size_t begin = position;
        if (consume('-') && position >= text.size()) return fail(json_error_code::invalid_number);
        if (consume('0')) {
            if (position < text.size() && is_digit(text[position])) return fail(json_error_code::invalid_number);
        }
        else {
            if (position >= text.size() || !is_digit(text[position])) return fail(json_error_code::invalid_number);
            while (position < text.size() && is_digit(text[position])) ++position;
        }

        bool integral = true;
        if (consume('.')) {
            integral = false;
            if (position >= text.size() || !is_digit(text[position])) return fail(json_error_code::invalid_number);
            while (position < text.size() && is_digit(text[position])) ++position;
        }
        if (position < text.size() && (text[position] == 'e' || text[position] == 'E')) {
            integral = false;
            ++position;
            if (position < text.size() && (text[position] == '+' || text[position] == '-')) ++position;
            if (position >= text.size() || !is_digit(text[position])) return fail(json_error_code::invalid_number);
            while (position < text.size() && is_digit(text[position])) ++position;
        }

        const auto token = text.substr(begin, position - begin);
        if (integral) {
            std::int64_t value = 0;
            const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size())
                return fail(json_error_code::invalid_number, begin);
            handler.integer(value);
            return true;
        }

        double value = 0.0;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || !std::isfinite(value))
            return fail(json_error_code::invalid_number, begin);
        handler.number(value);
        return true;
    }

    template <class Callback>
    [[nodiscard]] bool parse_literal(std::string_view literal, Callback callback) {
        if (text.substr(position, literal.size()) != literal)
            return fail(json_error_code::unexpected_token);
        position += literal.size();
        callback();
        return true;
    }

    void skip_space() noexcept {
        while (position < text.size() && is_space(text[position])) ++position;
    }

    [[nodiscard]] bool consume(char value) noexcept {
        if (position < text.size() && text[position] == value) {
            ++position;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool fail(json_error_code code) noexcept {
        return fail(code, position);
    }

    [[nodiscard]] bool fail(json_error_code code, std::size_t offset) noexcept {
        if (result.code == json_error_code::none) result = {code, offset};
        return false;
    }

    std::string_view text;
    json_event_handler& handler;
    std::size_t position = 0;
    json_parse_result result;
};

} // namespace

json_parse_result parse_json(std::string_view text, json_event_handler& handler) noexcept {
    try {
        return parser{text, handler}.run();
    }
    catch (const std::bad_alloc&) {
        return {json_error_code::internal_failure, 0};
    }
    catch (...) {
        return {json_error_code::internal_failure, 0};
    }
}

} // namespace cw::server
