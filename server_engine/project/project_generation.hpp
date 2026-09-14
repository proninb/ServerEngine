#pragma once

#include "project_root.hpp"
#include "source/file_snapshot.hpp"
#include "source/source_change_tracker.hpp"

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

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

// GEN-02C19: configuration proof belongs to the Generation that was built
// from project.json. SAVE consumes this proof but never creates a new one.
struct project_generation_configuration_proof final {
    file_snapshot_observation observation{};
    source_content_hash content_hash{};
    file_change_token change_token{};
    bool content_hash_available = false;
    bool change_token_available = false;
};

struct project_generation_root final {
    source_id source{};
    project_item_role role = project_item_role::type;
};

// Provenance owned by one published Project generation. It records the exact
// configuration and Source filesystem epochs used to construct that generation.
class project_generation_provenance final {
public:
    [[nodiscard]] std::size_t root_count() const noexcept {
        return roots_value.size() == root_roles_value.size()
            ? roots_value.size()
            : 0;
    }

    [[nodiscard]] project_generation_root
    root(std::size_t index) const noexcept {
        if (index >= root_count())
            return {};

        return {
            roots_value[index],
            root_roles_value[index],
        };
    }

    // Full REBUILD moves the frontend-resolved Source identities without
    // copying them. Roles are collected in the same configuration traversal.
    void publish_roots(
        std::vector<source_id>&& roots,
        std::vector<project_item_role>&& roles) noexcept {

        if (roots.size() != roles.size()) {
            roots_value.clear();
            root_roles_value.clear();
            return;
        }

        roots_value = std::move(roots);
        root_roles_value = std::move(roles);
    }

    [[nodiscard]] const project_generation_configuration_proof*
    configuration() const noexcept {
        return configuration_available
            ? &configuration_value
            : nullptr;
    }

    void publish_configuration(
        const project_generation_configuration_proof& proof) noexcept {
        configuration_value = proof;
        configuration_available = true;
    }

    void clear_configuration() noexcept {
        configuration_value = {};
        configuration_available = false;
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
    std::vector<project_item_role> root_roles_value;
    project_generation_configuration_proof configuration_value{};
    source_change_capture source_change_value;
    bool configuration_available = false;
    bool source_change_available = false;
};

} // namespace cw::server
