#pragma once

#include "../../status.hpp"
#include "../../string_id.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cw::server {

// Interns Project-lifetime strings and provides compact process-local string handles.
class string_registry final {
public:
    string_registry();

    string_registry(const string_registry&) = delete;
    string_registry& operator=(const string_registry&) = delete;

    [[nodiscard]] status intern(std::string_view value, string_id& output) noexcept;
    [[nodiscard]] std::string_view value(string_id id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct string_view_hash {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
    };

    struct string_view_equal {
        using is_transparent = void;
        [[nodiscard]] bool operator()(std::string_view left, std::string_view right) const noexcept {
            return left == right;
        }
    };

    mutable std::mutex mutex;
    std::deque<std::string> strings;
    std::unordered_map<std::string_view, string_id, string_view_hash, string_view_equal> index;
};

} // namespace cw::server
