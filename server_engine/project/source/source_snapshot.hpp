#pragma once

#include "file_snapshot.hpp"
#include "../../source_id.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace cw::server {

class source_manager;
class source_manager_update;

// Immutable project Source revision. Construction snapshots own bytes through
// shared state; persisted snapshots borrow immutable mmap bytes pinned by Project.
class source_snapshot final {
public:
    source_snapshot() noexcept = default;

    [[nodiscard]] source_id source() const noexcept;
    [[nodiscard]] std::string_view normalized_path() const noexcept;
    [[nodiscard]] std::string_view text() const noexcept;
    [[nodiscard]] file_snapshot_observation observation() const noexcept;
    [[nodiscard]] const source_content_hash& hash() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept {
        return state != nullptr || static_cast<bool>(borrowed_source);
    }

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

    source_snapshot(
        source_id source_value,
        std::string_view path_value,
        std::string_view text_value,
        file_snapshot_observation observation_value,
        const source_content_hash& hash_value) noexcept
        : borrowed_source(source_value),
          borrowed_path(path_value),
          borrowed_text(text_value),
          borrowed_observation(observation_value),
          borrowed_hash(hash_value) {}

    friend class source_manager;
    friend class source_manager_update;

    std::shared_ptr<const storage> state;
    source_id borrowed_source{};
    std::string_view borrowed_path;
    std::string_view borrowed_text;
    file_snapshot_observation borrowed_observation{};
    source_content_hash borrowed_hash{};
};

inline source_id source_snapshot::source() const noexcept {
    return state != nullptr ? state->source : borrowed_source;
}

inline std::string_view source_snapshot::normalized_path() const noexcept {
    return state != nullptr
        ? std::string_view{state->normalized_path}
        : borrowed_path;
}

inline std::string_view source_snapshot::text() const noexcept {
    return state != nullptr
        ? std::string_view{state->text}
        : borrowed_text;
}

inline file_snapshot_observation source_snapshot::observation() const noexcept {
    return state != nullptr ? state->observation : borrowed_observation;
}

inline const source_content_hash& source_snapshot::hash() const noexcept {
    static const source_content_hash empty{};
    if (state != nullptr)
        return state->hash;
    return borrowed_source ? borrowed_hash : empty;
}

} // namespace cw::server
