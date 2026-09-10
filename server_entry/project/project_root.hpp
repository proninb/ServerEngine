#pragma once

#include "../status.hpp"

#include <cstdint>
#include <filesystem>

namespace cw::server {

enum class project_item_role : std::uint8_t {
    type,
    source,
    project,
};

// Receives normalized Project composition roots from configuration/composition resolution.
class project_root_sink {
public:
    virtual ~project_root_sink() = default;

    [[nodiscard]] virtual status add(
        const std::filesystem::path& path,
        project_item_role role) noexcept = 0;
};

} // namespace cw::server
