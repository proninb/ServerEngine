#pragma once

#include "../parser/source_environment.hpp"
#include "../../source_id.hpp"
#include "../../status.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
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

struct source_frontend_persistence_summary final {
    std::size_t frontend_count = 0;
    std::size_t local_types = 0;
    std::size_t type_slots = 0;
    std::size_t object_slots = 0;
    std::size_t member_slots = 0;
};

struct source_frontend_native_persistence_range final {
    std::uint32_t begin = 0;
    std::uint32_t count = 0;
};

// Dense Source-indexed directory into cache-wide compact Frontend arenas.
struct source_frontend_native_persistence_record final {
    std::uint32_t present = 0;
    source_frontend_native_persistence_range local_types{};
    source_frontend_native_persistence_range type_slots{};
    source_frontend_native_persistence_range object_slots{};
    source_frontend_native_persistence_range member_slots{};
};

// Read-only bulk SAVE view. It is exposed only while all Source records still
// match the canonical native arenas. Sparse publication disables bulk mode but
// keeps the arenas alive because unchanged interfaces may still reference them.
struct source_frontend_native_persistence_storage_view final {
    std::span<const source_frontend_native_persistence_record> records;
    std::span<const identity_ref> local_types;
    std::span<const source_interface_type_slot> type_slots;
    std::span<const source_interface_object_slot> object_slots;
    std::span<const source_interface_member_slot> member_slots;
    bool complete = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return complete;
    }
};

enum class source_frontend_persistence_storage : std::uint8_t {
    none = 0,
    native_interface = 1,
    persisted_baseline = 2,
};

// Allocation-free SAVE view. Native and sparse-overlay interfaces expose
// contiguous typed spans; untouched mmap baseline state keeps its persisted
// representation and uses the canonical persisted accessors as a fallback.
struct source_frontend_persistence_view final {
    source_frontend_persistence_storage storage =
        source_frontend_persistence_storage::none;
    source_frontend_persistence_record record{};
    source_interface_data_view data{};
};

// Dense G0 persistence traversal. The view keeps ownership private while
// allowing SAVE to walk native Source interfaces directly by source_id order.
class source_frontend_native_persistence_view final {
public:
    source_frontend_native_persistence_view() noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return values != nullptr;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return values != nullptr ? values->size() : 0;
    }

    [[nodiscard]] const source_interface* operator[](
        std::size_t index) const noexcept {

        if (values == nullptr || index >= values->size())
            return nullptr;

        return (*values)[index].get();
    }

private:
    explicit source_frontend_native_persistence_view(
        const std::vector<std::unique_ptr<source_interface>>&
            values_value) noexcept
        : values(&values_value) {}

    const std::vector<std::unique_ptr<source_interface>>* values =
        nullptr;

    friend class source_frontend_cache;
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
    // read directly from the persisted Build Cache image; changed/new Sources
    // read the sparse overlay.
    [[nodiscard]] source_frontend_native_persistence_view
    native_persistence_view() const noexcept {
        if (baseline_cache != nullptr)
            return {};

        return source_frontend_native_persistence_view{
            interfaces};
    }

    [[nodiscard]] source_frontend_native_persistence_storage_view
    native_persistence_storage() const noexcept {
        if (baseline_cache != nullptr ||
            !native_persistence_complete_state ||
            persistence_records.size() != logical_source_count) {
            return {};
        }

        return {
            std::span<const source_frontend_native_persistence_record>{
                persistence_records},
            std::span<const identity_ref>{persistence_local_types},
            std::span<const source_interface_type_slot>{
                persistence_type_slots},
            std::span<const source_interface_object_slot>{
                persistence_object_slots},
            std::span<const source_interface_member_slot>{
                persistence_member_slots},
            true,
        };
    }

    [[nodiscard]] status persistence_view(
        source_id source,
        source_frontend_persistence_view& output) const noexcept;

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

    [[nodiscard]] const source_frontend_persistence_summary&
    persistence_summary() const noexcept {
        return persistence_summary_value;
    }

    [[nodiscard]] bool baseline_backed() const noexcept { return baseline_cache != nullptr; }

    [[nodiscard]] const build_cache_image_view*
    baseline_persistence_image() const noexcept {
        return baseline_cache;
    }

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

    std::vector<source_frontend_native_persistence_record>
        persistence_records;
    std::vector<identity_ref> persistence_local_types;
    std::vector<source_interface_type_slot>
        persistence_type_slots;
    std::vector<source_interface_object_slot>
        persistence_object_slots;
    std::vector<source_interface_member_slot>
        persistence_member_slots;
    bool native_persistence_complete_state = false;

    const build_cache_image_view* baseline_cache = nullptr;
    const source_manager_image_view* baseline_sources = nullptr;
    std::size_t baseline_source_count = 0;
    std::size_t logical_source_count = 0;
    mutable std::vector<overlay_entry> overlay;
    mutable std::vector<overlay_slot> overlay_index;
    source_frontend_persistence_summary persistence_summary_value{};
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

    struct replacement_slot final {
        source_id source{};
        std::uint32_t position = 0;
    };

    [[nodiscard]] status ensure_replacement_index(
        std::size_t required) noexcept;
    [[nodiscard]] replacement* find_replacement(
        source_id source) noexcept;

    source_frontend_cache_update(
        source_frontend_cache& owner_value,
        bool full_reconstruction_value) noexcept
        : owner(&owner_value), full_reconstruction(full_reconstruction_value) {}

    source_frontend_cache* owner = nullptr;
    std::vector<std::unique_ptr<source_interface>> full_candidate;

    std::vector<source_frontend_native_persistence_record>
        full_persistence_records;
    std::vector<identity_ref> full_persistence_local_types;
    std::vector<source_interface_type_slot>
        full_persistence_type_slots;
    std::vector<source_interface_object_slot>
        full_persistence_object_slots;
    std::vector<source_interface_member_slot>
        full_persistence_member_slots;

    std::vector<replacement> replacements;
    std::vector<replacement_slot> replacement_index;
    source_frontend_persistence_summary candidate_summary{};
    std::size_t required_source_count = 0;
    bool full_reconstruction = false;
    bool prepared = false;
    bool published = false;
    status failure{};

    friend class source_frontend_cache;
};

} // namespace cw::server
