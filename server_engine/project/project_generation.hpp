#pragma once

#include "source/source_change_tracker.hpp"

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include <utility>

namespace cw::server {

// Owns persistence-native immutable segments already frozen for the current
// Generation. GEN-02B starts with change; compiled/sources/build migrate later.
class project_generation_native_segments final {
public:
    project_generation_native_segments() noexcept = default;

    project_generation_native_segments(
        const project_generation_native_segments&) = delete;
    project_generation_native_segments& operator=(
        const project_generation_native_segments&) = delete;
    project_generation_native_segments(
        project_generation_native_segments&&) noexcept = default;
    project_generation_native_segments& operator=(
        project_generation_native_segments&&) noexcept = default;

    [[nodiscard]] std::span<const std::byte>
    change() const noexcept {
        return {
            change_value.data(),
            change_value.size(),
        };
    }

    void publish_change(
        std::vector<std::byte>&& value) noexcept {
        change_value = std::move(value);
    }

    void clear_change() noexcept {
        change_value.clear();
    }

private:
    std::vector<std::byte> change_value;
};

// Provenance owned by one published Project generation. It records the filesystem
// epoch from which the generation was constructed; persistence consumes it but
// does not create or own it.
class project_generation_provenance final {
public:
    // Configuration roots resolved to stable Source identities for this
    // Generation. Order exactly matches project_configuration::project.
    [[nodiscard]] std::span<const source_id>
    roots() const noexcept {
        return roots_value;
    }

    void publish_roots(
        std::vector<source_id>&& roots) noexcept {
        roots_value = std::move(roots);
    }

    [[nodiscard]] const source_change_capture*
    source_change() const noexcept {
        return source_change_available
            ? &source_change_value
            : nullptr;
    }

    void publish_source_change(
        source_change_capture&& capture) noexcept {
        source_change_value = std::move(capture);
        // Availability means the Generation owns a prepared persistence
        // boundary. An empty checkpoint is a valid fail-closed state: SAVE
        // persists it and the next BUILD falls back to a full Source scan.
        source_change_available = true;
    }

    void clear_source_change() noexcept {
        source_change_value.reset();
        source_change_available = false;
    }

private:
    std::vector<source_id> roots_value;
    source_change_capture source_change_value;
    bool source_change_available = false;
};

} // namespace cw::server
