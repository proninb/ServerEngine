#include "byte_arena.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace cw::server {
namespace {

[[nodiscard]] constexpr std::size_t align_up(
    std::size_t value,
    std::size_t alignment) noexcept {

    return (value + alignment - 1) & ~(alignment - 1);
}

[[nodiscard]] constexpr bool valid_alignment(std::size_t alignment) noexcept {
    return alignment != 0 && (alignment & (alignment - 1)) == 0;
}

} // namespace

byte_arena::~byte_arena() noexcept {
    auto* item = owned_pages.load(std::memory_order_relaxed);
    while (item != nullptr) {
        auto* next = item->next_owned;
        delete item;
        item = next;
    }
}

byte_arena::page* byte_arena::make_page(
    std::size_t minimum_capacity) noexcept {

    const auto capacity = (std::max)(default_page_size, minimum_capacity);

    auto* result = new (std::nothrow) page{};
    if (result == nullptr)
        return nullptr;

    result->data =
        std::unique_ptr<std::byte[]>{new (std::nothrow) std::byte[capacity]};
    if (!result->data) {
        delete result;
        return nullptr;
    }

    result->capacity = capacity;
    return result;
}

void byte_arena::own_page(page* value) noexcept {
    auto* head = owned_pages.load(std::memory_order_relaxed);
    do {
        value->next_owned = head;
    } while (!owned_pages.compare_exchange_weak(
        head,
        value,
        std::memory_order_release,
        std::memory_order_relaxed));

    reserved_bytes.fetch_add(value->capacity, std::memory_order_relaxed);
    reserved_pages.fetch_add(1, std::memory_order_relaxed);
}

status byte_arena::allocate(
    std::size_t size,
    std::size_t alignment,
    void*& output) noexcept {

    output = nullptr;

    if (size == 0 || !valid_alignment(alignment))
        return {status_code::invalid_argument};

    if (size > (std::numeric_limits<std::size_t>::max)() - (alignment - 1))
        return {status_code::not_available};

    const auto reservation = size + alignment - 1;

    for (;;) {
        auto* page_value = current_page.load(std::memory_order_acquire);
        if (page_value == nullptr) {
            auto* candidate = make_page(reservation);
            if (candidate == nullptr)
                return {status_code::initialization_failed};

            page* expected = nullptr;
            if (!current_page.compare_exchange_strong(
                    expected,
                    candidate,
                    std::memory_order_release,
                    std::memory_order_acquire)) {
                delete candidate;
                continue;
            }

            own_page(candidate);
            page_value = candidate;
        }

        const auto offset =
            page_value->used.fetch_add(reservation, std::memory_order_relaxed);

        if (offset <= page_value->capacity) {
            const auto aligned = align_up(offset, alignment);
            if (aligned <= page_value->capacity &&
                size <= page_value->capacity - aligned) {
                output = page_value->data.get() + aligned;
                return {};
            }
        }

        auto* candidate = make_page(reservation);
        if (candidate == nullptr)
            return {status_code::initialization_failed};

        auto* expected = page_value;
        if (current_page.compare_exchange_strong(
                expected,
                candidate,
                std::memory_order_release,
                std::memory_order_acquire)) {
            own_page(candidate);
        }
        else {
            delete candidate;
        }
    }
}

} // namespace cw::server
