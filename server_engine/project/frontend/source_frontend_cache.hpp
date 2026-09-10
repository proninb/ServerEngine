#pragma once

#include "../parser/source_environment.hpp"
#include "../../source_id.hpp"
#include "../../status.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace cw::server {

class source_frontend_cache_update;

// Retains Parser-visible Source interfaces between builds. This is reconstructable
// COLD acceleration state; semantic identity and compiled Graph state live elsewhere.
class source_frontend_cache final {
public:
    source_frontend_cache() = default;

    source_frontend_cache(const source_frontend_cache&) = delete;
    source_frontend_cache& operator=(const source_frontend_cache&) = delete;

    [[nodiscard]] bool complete() const noexcept { return complete_state; }
    [[nodiscard]] const source_interface* interface(source_id source) const noexcept;
    [[nodiscard]] std::size_t source_slots() const noexcept { return interfaces.size(); }

    [[nodiscard]] source_frontend_cache_update begin_update(bool full_reconstruction) noexcept;
    void invalidate() noexcept;

private:
    friend class source_frontend_cache_update;

    std::vector<std::unique_ptr<source_interface>> interfaces;
    bool complete_state = false;
};

// Prepares either a detached full cache replacement or sparse Source interface
// replacements. Publication performs only pre-reserved unique_ptr moves/swaps.
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
