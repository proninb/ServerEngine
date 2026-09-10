#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace cw::server {

[[nodiscard]] inline std::filesystem::path path_from_utf8(std::string_view text) {
#ifdef _WIN32
    std::u8string utf8;
    utf8.reserve(text.size());
    for (const unsigned char value : text) utf8.push_back(static_cast<char8_t>(value));
    return std::filesystem::path{utf8};
#else
    return std::filesystem::path{std::string{text}};
#endif
}

[[nodiscard]] inline std::filesystem::path resolve_configuration_path(
    const std::filesystem::path& configuration_path,
    std::string_view configured_path) {
    auto path = path_from_utf8(configured_path);
    if (path.is_relative()) path = configuration_path.parent_path() / path;
    return path.lexically_normal();
}

} // namespace cw::server
