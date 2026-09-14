#pragma once

#include "../persistence/change_state_image.hpp"

#include <cstddef>
#include <vector>

namespace cw::server {

// Generation freeze boundary for the Source change segment. The persisted
// change-state format is currently reused as the canonical immutable segment;
// callers do not depend on its physical persistence representation.
[[nodiscard]] inline status freeze_generation_change_segment(
    std::size_t source_count,
    const source_change_capture& capture,
    std::vector<std::byte>& output) noexcept {

    output.clear();

    return encode_change_state_image(
        source_count,
        capture.journal_anchor_path,
        capture,
        output);
}

} // namespace cw::server
