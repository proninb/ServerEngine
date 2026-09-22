#pragma once

#include "construction_value.hpp"
#include <charconv>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace cw::server {
// Restricted initializer grammar, shared by normalization and managed execution.
// No names, calls or arbitrary C++ expressions are evaluated here.
struct aggregate_initializer {
    construction_value value{};
    bool list = false;
    std::vector<aggregate_initializer> children;
};
inline bool parse_aggregate_initializer(std::string_view text, aggregate_initializer& output) {
    std::size_t position = 0, nodes = 0;
    const auto skip = [&] {
        while (position < text.size() && (text[position] == ' ' || text[position] == '\t' ||
            text[position] == '\n' || text[position] == '\r')) ++position;
    };
    const auto parse = [&](auto&& self, aggregate_initializer& out, unsigned depth) -> bool {
        if (depth > 64 || ++nodes > 65536) return false;
        skip();
        if (position == text.size()) return false;
        if (text[position] == '{') {
            out.list = true; ++position; skip();
            if (position < text.size() && text[position] == '}') { ++position; return true; }
            for (;;) {
                out.children.emplace_back();
                if (!self(self, out.children.back(), depth + 1)) return false;
                skip();
                if (position == text.size()) return false;
                if (text[position] == '}') { ++position; return true; }
                if (text[position++] != ',') return false;
                skip();
                if (position < text.size() && text[position] == '}') { ++position; return true; }
            }
        }
        if (text[position] == '(') {
            ++position;
            if (!self(self, out, depth + 1)) return false;
            skip();
            return position < text.size() && text[position++] == ')';
        }
        const auto begin = position;
        while (position < text.size() && text[position] != ',' && text[position] != '}' && text[position] != ')') ++position;
        auto token = text.substr(begin, position - begin);
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t' || token.back() == '\n' || token.back() == '\r')) token.remove_suffix(1);
        if (token == "false" || token == "nullptr") { out.value = {}; return true; }
        if (token == "true") { out.value = construction_value::constant(construction_kind::unsigned_integer, 1); return true; }
        if (token.starts_with('+')) token.remove_prefix(1);
        if (token.empty()) return false;
        if (token.find_first_of(".eE") != std::string_view::npos) {
            double number;
            const auto result = std::from_chars(token.data(), token.data() + token.size(), number);
            if (result.ec != std::errc{} || result.ptr != token.data() + token.size() || !std::isfinite(number)) return false;
            out.value = construction_value::constant(construction_kind::real, std::bit_cast<std::uint64_t>(number));
        } else if (token.starts_with('-')) {
            std::int64_t number;
            const auto result = std::from_chars(token.data(), token.data() + token.size(), number);
            if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) return false;
            out.value = construction_value::constant(construction_kind::signed_integer, std::bit_cast<std::uint64_t>(number));
        } else {
            std::uint64_t number;
            const auto result = std::from_chars(token.data(), token.data() + token.size(), number);
            if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) return false;
            out.value = construction_value::constant(construction_kind::unsigned_integer, number);
        }
        return true;
    };
    output = {};
    if (!parse(parse, output, 0)) return false;
    skip(); return position == text.size();
}
inline void encode_aggregate_initializer(const aggregate_initializer& value, std::string& output) {
    if (value.list) {
        output += '{';
        for (std::size_t i = 0; i < value.children.size(); ++i) {
            if (i) output += ',';
            encode_aggregate_initializer(value.children[i], output);
        }
        output += '}'; return;
    }
    char buffer[64];
    std::to_chars_result result;
    if (value.value.kind == construction_kind::real) {
        result = std::to_chars(buffer, buffer + sizeof(buffer), std::bit_cast<double>(value.value.bits()));
        const std::string_view number(buffer, result.ptr);
        output += number;
        if (number.find_first_of(".eE") == std::string_view::npos) output += ".0";
        return;
    }
    if (value.value.kind == construction_kind::signed_integer)
        result = std::to_chars(buffer, buffer + sizeof(buffer), std::bit_cast<std::int64_t>(value.value.bits()));
    else result = std::to_chars(buffer, buffer + sizeof(buffer), value.value.bits());
    output.append(buffer, result.ptr);
}
} // namespace cw::server
