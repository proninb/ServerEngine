#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace cw::server {

enum class json_error_code : std::uint8_t {
    none = 0,
    unexpected_end,
    unexpected_token,
    invalid_string,
    invalid_escape,
    invalid_unicode,
    invalid_number,
    nesting_too_deep,
    internal_failure,
};

struct json_parse_result {
    json_error_code code = json_error_code::none;
    std::size_t offset = 0;

    [[nodiscard]] constexpr bool ok() const noexcept {
        return code == json_error_code::none;
    }
};

// Receives JSON events directly from the parser so configuration loaders do not need a DOM.
class json_event_handler {
public:
    virtual ~json_event_handler() = default;

    virtual void location(std::size_t) noexcept {}
    virtual void object_begin() noexcept = 0;
    virtual void object_end() noexcept = 0;
    virtual void array_begin() noexcept = 0;
    virtual void array_end() noexcept = 0;
    virtual void key(std::string_view value) noexcept = 0;
    virtual void string(std::string_view value) noexcept = 0;
    virtual void integer(std::int64_t value) noexcept = 0;
    virtual void number(double value) noexcept = 0;
    virtual void boolean(bool value) noexcept = 0;
    virtual void null() noexcept = 0;
};

[[nodiscard]] json_parse_result parse_json(
    std::string_view text,
    json_event_handler& handler) noexcept;

} // namespace cw::server
