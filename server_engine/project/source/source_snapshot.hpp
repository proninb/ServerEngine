#pragma once

#include "file_snapshot.hpp"
#include "../../source_id.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace cw::server {

class source_manager;
class source_manager_update;

// Immutable project Source revision. Snapshots own their bytes through shared state,
// so Parser/Builder readers remain valid after a newer revision is committed.
class source_snapshot final {
public:
    source_snapshot() noexcept = default;

    [[nodiscard]] source_id source() const noexcept;
    [[nodiscard]] std::string_view normalized_path() const noexcept;
    [[nodiscard]] std::string_view text() const noexcept;
    [[nodiscard]] file_snapshot_observation observation() const noexcept;
    [[nodiscard]] const source_content_hash& hash() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept { return state != nullptr; }

private:
    struct storage final {
        source_id source{};
        std::string normalized_path;
        std::string text;
        file_snapshot_observation observation{};
        source_content_hash hash{};
    };

    explicit source_snapshot(std::shared_ptr<const storage> storage_value) noexcept
        : state(std::move(storage_value)) {}

    friend class source_manager;
    friend class source_manager_update;
    std::shared_ptr<const storage> state;
};

inline source_id source_snapshot::source() const noexcept {
    return state != nullptr ? state->source : source_id{};
}

inline std::string_view source_snapshot::normalized_path() const noexcept {
    return state != nullptr ? std::string_view{state->normalized_path} : std::string_view{};
}

inline std::string_view source_snapshot::text() const noexcept {
    return state != nullptr ? std::string_view{state->text} : std::string_view{};
}

inline file_snapshot_observation source_snapshot::observation() const noexcept {
    return state != nullptr ? state->observation : file_snapshot_observation{};
}

inline const source_content_hash& source_snapshot::hash() const noexcept {
    static const source_content_hash empty{};
    return state != nullptr ? state->hash : empty;
}

} // namespace cw::server
