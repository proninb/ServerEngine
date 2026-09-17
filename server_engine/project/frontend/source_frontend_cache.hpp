#pragma once

#include "source_frontend_block_store.hpp"
#include "../parser/source_environment.hpp"
#include "../../source_id.hpp"
#include "../../status.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
#include <span>
#include <stdexcept>
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

enum class source_frontend_persistence_storage : std::uint8_t {
    none = 0,
    native_interface = 1,
    persisted_baseline = 2,
};

enum class source_frontend_block_origin : std::uint8_t {
    none = 0,
    generation_owned = 1,
    baseline_borrowed = 2,
};

// Storage-neutral identity of one current Source frontend block. Baseline-backed
// unchanged Sources are represented implicitly by source_id and require no
// dense O(N) block-ref directory.
struct source_frontend_block_descriptor final {
    source_frontend_block_origin origin =
        source_frontend_block_origin::none;
    source_id source{};
    source_frontend_block_ref block{};
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

// Owns dense construction-time Source interfaces for one full Frontend build.
// One heap owner keeps all interface object addresses stable across context moves;
// individual Sources are identities, never heap owners.
class source_frontend_context final {
public:
    source_frontend_context() noexcept = default;

    source_frontend_context(
        const source_frontend_context&) = delete;
    source_frontend_context& operator=(
        const source_frontend_context&) = delete;
    source_frontend_context(
        source_frontend_context&&) noexcept = default;
    source_frontend_context& operator=(
        source_frontend_context&&) noexcept = default;

    [[nodiscard]] status initialize(
        std::size_t source_slots) noexcept {

        if (source_slots == 0)
            return {status_code::invalid_argument};

        try {
            auto candidate =
                std::make_unique<storage>();

            candidate->base.resize(source_slots);
            candidate->current.assign(
                source_slots,
                nullptr);
            candidate->overlay.resize(
                source_slots);

            value = std::move(candidate);
            return {};
        }
        catch (const std::bad_alloc&) {
            return {status_code::not_available};
        }
        catch (const std::length_error&) {
            return {status_code::not_available};
        }
    }

    [[nodiscard]] bool active() const noexcept {
        return value != nullptr;
    }

    [[nodiscard]] std::size_t source_slots() const noexcept {
        return value != nullptr
            ? value->current.size()
            : 0;
    }

    [[nodiscard]] source_interface* prepare(
        source_id source) noexcept {

        if (value == nullptr || !source)
            return nullptr;

        const auto index =
            static_cast<std::size_t>(
                source.value() - 1);

        return index < value->base.size()
            ? &value->base[index]
            : nullptr;
    }

    void publish(source_id source) noexcept {
        if (value == nullptr || !source)
            return;

        const auto index =
            static_cast<std::size_t>(
                source.value() - 1);

        if (index < value->base.size() &&
            index < value->current.size()) {
            value->current[index] =
                &value->base[index];
        }
    }

    [[nodiscard]] status ensure_source_slots(
        std::size_t required) noexcept {

        if (value == nullptr)
            return {status_code::invalid_state};

        if (required <= value->current.size())
            return {};

        try {
            value->current.resize(
                required,
                nullptr);
            value->overlay.resize(
                required);
            return {};
        }
        catch (const std::bad_alloc&) {
            return {status_code::not_available};
        }
        catch (const std::length_error&) {
            return {status_code::not_available};
        }
    }

    void adopt(
        source_id source,
        std::unique_ptr<source_interface> replacement) noexcept {

        if (value == nullptr || !source)
            return;

        const auto index =
            static_cast<std::size_t>(
                source.value() - 1);

        if (index >= value->current.size() ||
            index >= value->overlay.size()) {
            return;
        }

        value->overlay[index] =
            std::move(replacement);

        value->current[index] =
            value->overlay[index]
            ? value->overlay[index].get()
            : nullptr;
    }

    [[nodiscard]] const source_interface* interface(
        source_id source) const noexcept {

        if (value == nullptr || !source)
            return nullptr;

        const auto index =
            static_cast<std::size_t>(
                source.value() - 1);

        return index < value->current.size()
            ? value->current[index]
            : nullptr;
    }

    [[nodiscard]] source_interface* mutable_interface(
        source_id source) noexcept {

        return const_cast<source_interface*>(
            static_cast<
                const source_frontend_context&>(
                    *this).interface(source));
    }

private:
    struct storage final {
        std::vector<source_interface> base;
        std::vector<source_interface*> current;

        // Sparse Gn replacements only. G0 does not allocate one interface
        // object per Source.
        std::vector<
            std::unique_ptr<source_interface>>
                overlay;
    };

    std::unique_ptr<storage> value;
};

// Dense G0 persistence traversal. The view keeps ownership private while
// allowing SAVE to walk native Source interfaces directly by source_id order.
class source_frontend_native_persistence_view final {
public:
    source_frontend_native_persistence_view() noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return context != nullptr || values != nullptr;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        if (context != nullptr)
            return context->source_slots();

        return values != nullptr
            ? values->size()
            : 0;
    }

    [[nodiscard]] const source_interface* operator[](
        std::size_t index) const noexcept {

        if (context != nullptr) {
            if (index >= context->source_slots() ||
                index >= static_cast<std::size_t>(
                    (std::numeric_limits<std::uint32_t>::max)())) {
                return nullptr;
            }

            return context->interface(
                source_id{
                    static_cast<std::uint32_t>(
                        index + 1)});
        }

        if (values == nullptr ||
            index >= values->size()) {
            return nullptr;
        }

        return (*values)[index].get();
    }

private:
    explicit source_frontend_native_persistence_view(
        const source_frontend_context& value) noexcept
        : context(&value) {}

    explicit source_frontend_native_persistence_view(
        const std::vector<
            std::unique_ptr<source_interface>>&
            values_value) noexcept
        : values(&values_value) {}

    const source_frontend_context* context = nullptr;
    const std::vector<
        std::unique_ptr<source_interface>>* values =
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

    [[nodiscard]] std::size_t
    dense_context_sources() const noexcept {
        return dense_context.active()
            ? dense_context.source_slots()
            : 0;
    }

    [[nodiscard]] const source_interface* interface(source_id source) const noexcept;

    // Allocation-free SAVE boundary. Untouched baseline interface records are
    // read directly from the persisted Build Cache image; changed/new Sources
    // read the sparse overlay.
    [[nodiscard]] source_frontend_native_persistence_view
    native_persistence_view() const noexcept {
        if (baseline_cache != nullptr)
            return {};

        return dense_context.active()
            ? source_frontend_native_persistence_view{
                dense_context}
            : source_frontend_native_persistence_view{
                interfaces};
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

    [[nodiscard]] bool
    native_frontend_block_storage_complete() const noexcept {
        return native_frontend_block_complete_state &&
            frontend_block_refs.size() == logical_source_count;
    }

    // Bulk page capability exists only for a canonical full publication.
    // Sparse replacement keeps block refs valid but drops this capability
    // because prior immutable blocks remain append-only in the store.
    [[nodiscard]] const source_frontend_block_store*
    native_frontend_block_bulk_storage() const noexcept {
        return native_frontend_block_storage_complete() &&
            native_frontend_block_bulk_complete_state
            ? &frontend_blocks
            : nullptr;
    }

    // Lifetime capability for the exact same native Frontend pages exposed by
    // native_frontend_block_bulk_storage(). The committed Generation may retain
    // this token and therefore keep Build Cache frontend spans valid after
    // construction-only source_interface objects are destroyed.
    [[nodiscard]] source_frontend_block_store_lifetime
    native_frontend_block_lifetime() const noexcept {
        return native_frontend_block_storage_complete() &&
            native_frontend_block_bulk_complete_state
            ? frontend_blocks.pin_lifetime()
            : source_frontend_block_store_lifetime{};
    }

    // Construction-to-Generation ownership handoff. Valid only for a complete
    // native Frontend after all Parser/Builder consumers have finished.
    [[nodiscard]] status release_native_frontend_block_storage(
        source_frontend_block_store& output) noexcept;

    [[nodiscard]] status native_frontend_block(
        source_id source,
        source_frontend_block_ref& output) const noexcept;

    [[nodiscard]] status native_frontend_block_descriptor(
        source_id source,
        source_frontend_block_descriptor& output) const noexcept;

    [[nodiscard]] status native_frontend_block_view(
        source_id source,
        source_interface_data_view& output) const noexcept;

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

    [[nodiscard]] std::size_t persistence_overlay_count() const noexcept {
        return baseline_cache != nullptr
            ? overlay.size()
            : 0;
    }

    // Enumerates only sparse Frontend replacements retained over a mapped
    // baseline. The returned data spans remain owned by this cache.
    [[nodiscard]] status persistence_overlay(
        std::size_t index,
        source_id& source,
        source_frontend_persistence_view& output) const noexcept;

    [[nodiscard]] source_frontend_cache_update begin_update(bool full_reconstruction) noexcept;
    void invalidate() noexcept;

private:
    struct overlay_entry final {
        source_id source{};
        std::unique_ptr<source_interface> interface;
        source_frontend_block_ref native_block{};
        bool resolved = false;
        bool native_block_overrides_baseline = false;
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

    // Full G0 construction is owned by one dense Frontend context. The legacy
    // vector remains only as a compatibility fallback for pre-context callers.
    source_frontend_context dense_context;
    std::vector<std::unique_ptr<source_interface>> interfaces;

    source_frontend_block_store frontend_blocks;
    std::vector<source_frontend_block_ref> frontend_block_refs;
    bool native_frontend_block_complete_state = false;
    bool native_frontend_block_bulk_complete_state = false;

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

    [[nodiscard]] status replace_full_context(
        source_frontend_context&& context) noexcept;

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

    struct frontend_block_update final {
        source_id source{};
        source_frontend_block_ref block{};
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
    source_frontend_context full_context_candidate;
    bool full_context_candidate_active = false;

    source_frontend_block_store full_frontend_blocks;
    std::vector<source_frontend_block_ref>
        full_frontend_block_refs;

    std::vector<replacement> replacements;
    std::vector<replacement_slot> replacement_index;

    std::vector<frontend_block_update>
        frontend_block_updates;
    source_frontend_block_store::checkpoint
        frontend_block_checkpoint{};
    bool frontend_block_sparse_prepared = false;

    source_frontend_persistence_summary candidate_summary{};
    std::size_t required_source_count = 0;
    bool full_reconstruction = false;
    bool prepared = false;
    bool published = false;
    status failure{};

    friend class source_frontend_cache;
};

} // namespace cw::server
