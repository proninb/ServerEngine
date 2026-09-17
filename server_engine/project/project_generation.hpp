#pragma once

#include "project_root.hpp"
#include "source/file_snapshot.hpp"
#include "source/source_change_tracker.hpp"

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace cw::server {

// Persistence-native Build Cache source_directory record produced by BUILD.
// Explicit fields make the 56-byte v4 representation independent of implicit
// compiler padding. The section is Generation-local and ordered by source_id.
struct project_generation_build_cache_source_directory_record final {
    static constexpr std::uint32_t snapshot_present = 0x00000001u;
    static constexpr std::uint32_t frontend_present = 0x00000002u;

    source_id source{};
    std::uint32_t flags = 0;
    std::uint64_t text_offset = 0;
    std::uint32_t text_length = 0;
    std::uint32_t reserved = 0;
    std::uint32_t local_types_begin = 0;
    std::uint32_t local_types_count = 0;
    std::uint32_t type_slots_begin = 0;
    std::uint32_t type_slots_count = 0;
    std::uint32_t object_slots_begin = 0;
    std::uint32_t object_slots_count = 0;
    std::uint32_t member_slots_begin = 0;
    std::uint32_t member_slots_count = 0;
};

static_assert(
    sizeof(project_generation_build_cache_source_directory_record) == 56);

// Owns persistence-native immutable segments produced by BUILD for the current
// Generation. FINALIZE may bind these sections directly and transfer ownership
// without reconstructing their payload.
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

    [[nodiscard]] std::span<
        const project_generation_build_cache_source_directory_record>
    build_cache_source_directory() const noexcept {
        return build_cache_source_directory_value;
    }

    void publish_build_cache_source_directory(
        std::vector<
            project_generation_build_cache_source_directory_record>&&
                value) noexcept {

        build_cache_source_directory_value =
            std::move(value);
    }

    void clear_build_cache_source_directory() noexcept {
        build_cache_source_directory_value.clear();
    }

    [[nodiscard]] std::vector<
        project_generation_build_cache_source_directory_record>
    release_build_cache_source_directory() noexcept {
        return std::move(
            build_cache_source_directory_value);
    }

private:
    std::vector<std::byte> change_value;
    std::vector<
        project_generation_build_cache_source_directory_record>
            build_cache_source_directory_value;
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

    void clear_roots() noexcept {
        roots_value.clear();
        root_roles_value.clear();
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
