#pragma once

#include "../parser/source_environment.hpp"
#include "../../source_id.hpp"
#include "../../status.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace cw::server {

class build_cache_image_view;
class source_manager_image_view;
class source_frontend_cache_update;

struct source_frontend_persistence_record final {
    bool present = false;
    std::size_t local_types = 0;
    std::size_t type_slots = 0;
    std::size_t object_slots = 0;
    std::size_t member_slots = 0;
};

// Retains Parser-visible Source interfaces between builds. A baseline-backed cache
// keeps persisted interfaces in mmap and materializes only interfaces actually
// reused by the affected parse closure; changed Sources live in a sparse overlay.
class source_frontend_cache final {
public:
    source_frontend_cache() = default;
    source_frontend_cache(
        const build_cache_image_view& baseline_cache,
        const source_manager_image_view& baseline_sources) noexcept;

    source_frontend_cache(const source_frontend_cache&) = delete;
    source_frontend_cache& operator=(const source_frontend_cache&) = delete;

    [[nodiscard]] bool complete() const noexcept { return complete_state; }
    [[nodiscard]] const source_interface* interface(source_id source) const noexcept;

    // Allocation-free SAVE boundary. Untouched baseline interface records are
    // read directly from build_cache.bin; changed/new Sources read sparse overlay.
    [[nodiscard]] status persistence_record(
        source_id source,
        source_frontend_persistence_record& output) const noexcept;

    [[nodiscard]] status persistence_local_type(
        source_id source,
        std::size_t index,
        identity_ref& output) const noexcept;

    [[nodiscard]] status persistence_type_slot(
        source_id source,
        std::size_t index,
        source_interface_type_slot& output) const noexcept;

    [[nodiscard]] status persistence_object_slot(
        source_id source,
        std::size_t index,
        source_interface_object_slot& output) const noexcept;

    [[nodiscard]] status persistence_member_slot(
        source_id source,
        std::size_t index,
        source_interface_member_slot& output) const noexcept;

    [[nodiscard]] std::size_t source_slots() const noexcept { return logical_source_count; }
    [[nodiscard]] bool baseline_backed() const noexcept { return baseline_cache != nullptr; }

    [[nodiscard]] source_frontend_cache_update begin_update(bool full_reconstruction) noexcept;
    void invalidate() noexcept;

private:
    struct overlay_entry final {
        source_id source{};
        std::unique_ptr<source_interface> interface;
        bool resolved = false;
    };

    struct overlay_slot final {
        source_id source{};
        std::uint32_t position = 0;
    };

    [[nodiscard]] const overlay_entry* find_overlay(source_id source) const noexcept;
    [[nodiscard]] overlay_entry* find_overlay(source_id source) noexcept;
    [[nodiscard]] status prepare_overlay_capacity(std::size_t additional) noexcept;
    [[nodiscard]] overlay_entry* publish_overlay(
        source_id source,
        std::unique_ptr<source_interface> interface_value,
        bool resolved_value) noexcept;
    [[nodiscard]] const source_interface* materialize_baseline(
        source_id source) const noexcept;

    // Detached/G0 storage remains dense and preserves the existing fast path.
    std::vector<std::unique_ptr<source_interface>> interfaces;

    const build_cache_image_view* baseline_cache = nullptr;
    const source_manager_image_view* baseline_sources = nullptr;
    std::size_t baseline_source_count = 0;
    std::size_t logical_source_count = 0;
    mutable std::vector<overlay_entry> overlay;
    mutable std::vector<overlay_slot> overlay_index;
    bool complete_state = false;

    friend class source_frontend_cache_update;
};

// Prepares either a detached full cache replacement or sparse Source interface
// replacements. Baseline publication mutates only the sparse overlay and performs
// no allocation after prepare_publish().
class source_frontend_cache_update final {
public:
    source_frontend_cache_update() noexcept = default;
    ~source_frontend_cache_update();

    source_frontend_cache_update(const source_frontend_cache_update&) = delete;
    source_frontend_cache_update& operator=(const source_frontend_cache_update&) = delete;
    source_frontend_cache_update(source_frontend_cache_update&& other) noexcept;
    source_frontend_cache_update& operator=(source_frontend_cache_update&&) = delete;

    [[nodiscard]] status replace(
        source_id source,
        std::unique_ptr<source_interface> interface_value) noexcept;

    [[nodiscard]] status prepare_publish(std::size_t required_source_count) noexcept;
    void publish_prepared() noexcept;
    void cancel() noexcept;

private:
    struct replacement final {
        source_id source{};
        std::unique_ptr<source_interface> interface;
    };

    source_frontend_cache_update(
        source_frontend_cache& owner_value,
        bool full_reconstruction_value) noexcept
        : owner(&owner_value), full_reconstruction(full_reconstruction_value) {}

    source_frontend_cache* owner = nullptr;
    std::vector<std::unique_ptr<source_interface>> full_candidate;
    std::vector<replacement> replacements;
    std::size_t required_source_count = 0;
    bool full_reconstruction = false;
    bool prepared = false;
    bool published = false;
    status failure{};

    friend class source_frontend_cache;
};

} // namespace cw::server
