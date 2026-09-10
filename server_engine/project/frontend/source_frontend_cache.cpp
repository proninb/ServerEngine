#include "source_frontend_cache.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace cw::server {
namespace {

[[nodiscard]] bool cache_headroom(std::size_t size, std::size_t& output) noexcept {
    const auto extra = (std::max)(size / 16, std::size_t{64});
    if (size > (std::numeric_limits<std::size_t>::max)() - extra)
        return false;
    output = size + extra;
    return true;
}

} // namespace

const source_interface* source_frontend_cache::interface(source_id source) const noexcept {
    if (!source)
        return nullptr;
    const auto index = static_cast<std::size_t>(source.value() - 1);
    return index < interfaces.size() ? interfaces[index].get() : nullptr;
}

source_frontend_cache_update source_frontend_cache::begin_update(
    bool full_reconstruction) noexcept {

    return source_frontend_cache_update{*this, full_reconstruction};
}

void source_frontend_cache::invalidate() noexcept {
    interfaces.clear();
    complete_state = false;
}

source_frontend_cache_update::~source_frontend_cache_update() {
    cancel();
}

source_frontend_cache_update::source_frontend_cache_update(
    source_frontend_cache_update&& other) noexcept
    : owner(std::exchange(other.owner, nullptr)),
      full_candidate(std::move(other.full_candidate)),
      replacements(std::move(other.replacements)),
      required_source_count(other.required_source_count),
      full_reconstruction(other.full_reconstruction),
      prepared(other.prepared),
      published(other.published),
      failure(other.failure) {}

status source_frontend_cache_update::replace(
    source_id source,
    std::unique_ptr<source_interface> interface_value) noexcept {

    if (!failure.ok())
        return failure;
    if (owner == nullptr || !source || prepared || published)
        return {status_code::invalid_argument};

    try {
        if (full_reconstruction) {
            const auto required = static_cast<std::size_t>(source.value());
            if (full_candidate.size() < required)
                full_candidate.resize(required);
            full_candidate[required - 1] = std::move(interface_value);
        } else {
            replacements.push_back(replacement{source, std::move(interface_value)});
        }
        return {};
    }
    catch (const std::bad_alloc&) {
        failure = {status_code::not_available};
    }
    catch (const std::length_error&) {
        failure = {status_code::not_available};
    }
    return failure;
}

status source_frontend_cache_update::prepare_publish(
    std::size_t required_count) noexcept {

    if (!failure.ok())
        return failure;
    if (owner == nullptr || prepared || published)
        return {status_code::invalid_argument};
    if (!full_reconstruction && !owner->complete_state)
        return {status_code::not_available};

    try {
        required_source_count = required_count;
        if (full_reconstruction) {
            if (full_candidate.size() < required_source_count)
                full_candidate.resize(required_source_count);
            std::size_t capacity = 0;
            if (!cache_headroom(required_source_count, capacity))
                return {status_code::not_available};
            full_candidate.reserve(capacity);
        } else {
            if (required_source_count > owner->interfaces.capacity())
                return {status_code::rebuild_required};
            for (const auto& item : replacements) {
                if (!item.source || static_cast<std::size_t>(item.source.value()) > required_source_count)
                    return {status_code::invalid_argument};
            }
        }
        prepared = true;
        return {};
    }
    catch (const std::bad_alloc&) {
        failure = {status_code::not_available};
    }
    catch (const std::length_error&) {
        failure = {status_code::not_available};
    }
    return failure;
}

void source_frontend_cache_update::publish_prepared() noexcept {
    if (!prepared || published || owner == nullptr)
        return;

    if (full_reconstruction) {
        owner->interfaces.swap(full_candidate);
    } else {
        while (owner->interfaces.size() < required_source_count)
            owner->interfaces.emplace_back();
        for (auto& item : replacements)
            owner->interfaces[item.source.value() - 1] = std::move(item.interface);
    }

    owner->complete_state = true;
    published = true;
    prepared = false;
    owner = nullptr;
}

void source_frontend_cache_update::cancel() noexcept {
    if (owner == nullptr || published)
        return;
    owner = nullptr;
    prepared = false;
}

} // namespace cw::server
