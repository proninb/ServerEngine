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
    [[nodiscard]] const source_change_capture*
    source_change() const noexcept {
        return source_change_available
            ? &source_change_value
            : nullptr;
    }

    void publish_source_change(
        source_change_capture&& capture) noexcept {
        source_change_value = std::move(capture);
        source_change_available =
            static_cast<bool>(
                source_change_value.checkpoint);
    }

    void clear_source_change() noexcept {
        source_change_value.reset();
        source_change_available = false;
    }

private:
    source_change_capture source_change_value;
    bool source_change_available = false;
};

} // namespace cw::server
