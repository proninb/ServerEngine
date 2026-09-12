#pragma once

#include "project_root.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cw::server {

struct project_item_configuration {
    std::filesystem::path path;
    project_item_role role = project_item_role::source;

    // The JSON loader resolves roots against an absolute configuration path and
    // lexically normalizes them. Programmatic configurations leave this false.
    bool canonical_path = false;
};

enum class abi_target : std::uint8_t {
    windows_x64 = 0,
    posix_x64 = 1,
};

[[nodiscard]] constexpr bool is_supported_abi_target(abi_target target) noexcept {
    return target == abi_target::windows_x64 || target == abi_target::posix_x64;
}

[[nodiscard]] constexpr bool is_supported_abi_pack(std::uint32_t pack) noexcept {
    return pack == 1 || pack == 2 || pack == 4 || pack == 8 || pack == 16;
}

struct abi_configuration {
    abi_target target = abi_target::windows_x64;
    std::uint32_t pack = 8;
};

[[nodiscard]] constexpr bool is_supported_abi_configuration(
    const abi_configuration& abi) noexcept {
    return is_supported_abi_target(abi.target) && is_supported_abi_pack(abi.pack);
}

// Validated Project configuration consumed before Source Manager and identity construction exist.
struct project_configuration {
    std::uint32_t version = 0;
    std::string name;
    std::vector<project_item_configuration> project;
    abi_configuration abi;
};

inline constexpr std::uint32_t current_project_configuration_version = 1;

} // namespace cw::server
