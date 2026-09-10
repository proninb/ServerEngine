#include "string_registry.hpp"

#include <limits>
#include <new>

namespace cw::server {

string_registry::string_registry() {
    strings.emplace_back();
    index.emplace(std::string_view{strings.front()}, string_id{});
}

status string_registry::intern(std::string_view value, string_id& output) noexcept {
    try {
        std::scoped_lock lock{mutex};
        const auto existing = index.find(value);
        if (existing != index.end()) {
            output = existing->second;
            return {};
        }
        if (strings.size() > std::numeric_limits<std::uint32_t>::max())
            return {status_code::not_available};

        strings.emplace_back(value);
        const auto id = string_id{static_cast<std::uint32_t>(strings.size() - 1)};
        const std::string_view stable_view{strings.back()};
        try {
            index.emplace(stable_view, id);
        }
        catch (...) {
            strings.pop_back();
            throw;
        }
        output = id;
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::initialization_failed};
    }
    catch (...) {
        return {status_code::initialization_failed};
    }
}

std::string_view string_registry::value(string_id id) const noexcept {
    std::scoped_lock lock{mutex};
    if (id.value() >= strings.size()) return {};
    return strings[id.value()];
}

std::size_t string_registry::size() const noexcept {
    std::scoped_lock lock{mutex};
    return strings.size();
}

} // namespace cw::server
