#include "source_frontend_block_store.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace cw::server {
namespace {

struct source_frontend_block_range final {
    std::uint32_t page = 0;
    std::uint32_t begin = 0;
    std::uint32_t logical_begin = 0;
    std::uint32_t count = 0;
};

template <typename T>
class typed_page_store final {
public:
    struct checkpoint final {
        std::size_t page_count = 0;
        std::size_t last_size = 0;
        std::size_t logical_size = 0;
        std::size_t next_page_capacity = 1;
    };

    explicit typed_page_store(
        std::size_t page_bytes_value) noexcept
        : initial_page_capacity(
              (std::max)(
                  std::size_t{1},
                  page_bytes_value / sizeof(T))),
          maximum_page_capacity(
              (std::max)(
                  initial_page_capacity,
                  (std::max)(
                      std::size_t{1},
                      source_frontend_block_maximum_chunk_bytes /
                          sizeof(T)))),
          next_page_capacity_value(
              initial_page_capacity) {}

    [[nodiscard]] checkpoint mark() const noexcept {
        return {
            pages.size(),
            pages.empty() ? 0 : pages.back().size(),
            logical_size_value,
            next_page_capacity_value,
        };
    }

    void restore(checkpoint state) noexcept {
        if (state.page_count == 0) {
            pages.clear();
            logical_size_value = state.logical_size;
            return;
        }

        if (pages.size() > state.page_count)
            pages.resize(state.page_count);

        if (!pages.empty() &&
            pages.back().size() > state.last_size) {
            pages.back().resize(state.last_size);
        }

        logical_size_value = state.logical_size;
        next_page_capacity_value =
            state.next_page_capacity;
    }

    [[nodiscard]] status append(
        std::span<const T> values,
        source_frontend_block_range& output) noexcept {

        output = {};

        constexpr auto maximum =
            static_cast<std::size_t>(
                (std::numeric_limits<std::uint32_t>::max)());

        if (logical_size_value > maximum)
            return {status_code::not_available};

        // Empty ranges still carry the current logical cursor. Build Cache v4
        // range geometry is canonical even when count == 0.
        output.logical_begin =
            static_cast<std::uint32_t>(logical_size_value);

        if (values.empty())
            return {};

        if (values.size() > maximum ||
            values.size() > maximum - logical_size_value) {
            return {status_code::not_available};
        }

        try {
            const bool needs_page =
                pages.empty() ||
                pages.back().capacity() - pages.back().size() <
                    values.size();

            if (needs_page) {
                const auto capacity =
                    (std::max)(
                        next_page_capacity_value,
                        values.size());

                pages.emplace_back();
                pages.back().reserve(capacity);

                advance_page_capacity(capacity);
            }

            auto& page = pages.back();

            if (pages.size() >
                    static_cast<std::size_t>(
                        (std::numeric_limits<std::uint32_t>::max)()) ||
                page.size() >
                    static_cast<std::size_t>(
                        (std::numeric_limits<std::uint32_t>::max)()) ||
                values.size() >
                    static_cast<std::size_t>(
                        (std::numeric_limits<std::uint32_t>::max)()) -
                        page.size()) {
                return {status_code::not_available};
            }

            output.page =
                static_cast<std::uint32_t>(pages.size());
            output.begin =
                static_cast<std::uint32_t>(page.size());
            output.logical_begin =
                static_cast<std::uint32_t>(logical_size_value);
            output.count =
                static_cast<std::uint32_t>(values.size());

            page.insert(
                page.end(),
                values.begin(),
                values.end());

            logical_size_value += values.size();
            return {};
        }
        catch (const std::bad_alloc&) {
            return {status_code::not_available};
        }
        catch (const std::length_error&) {
            return {status_code::not_available};
        }
    }

    [[nodiscard]] std::span<const T> view(
        source_frontend_block_range range) const noexcept {

        if (range.count == 0)
            return {};

        if (range.page == 0)
            return {};

        const auto page_index =
            static_cast<std::size_t>(range.page - 1);

        if (page_index >= pages.size())
            return {};

        const auto& page = pages[page_index];
        const auto begin =
            static_cast<std::size_t>(range.begin);
        const auto count =
            static_cast<std::size_t>(range.count);

        if (begin > page.size() ||
            count > page.size() - begin) {
            return {};
        }

        return std::span<const T>{page}.subspan(
            begin,
            count);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return logical_size_value;
    }

    [[nodiscard]] std::size_t page_count() const noexcept {
        return pages.size();
    }

    [[nodiscard]] std::span<const T> page(
        std::size_t index) const noexcept {

        if (index >= pages.size())
            return {};

        return pages[index];
    }

    void clear() noexcept {
        pages.clear();
        logical_size_value = 0;
        next_page_capacity_value =
            initial_page_capacity;
    }

private:
    void advance_page_capacity(
        std::size_t reserved_capacity) noexcept {

        const auto growth_base =
            (std::min)(
                maximum_page_capacity,
                (std::max)(
                    next_page_capacity_value,
                    reserved_capacity));

        if (growth_base >=
            maximum_page_capacity) {
            next_page_capacity_value =
                maximum_page_capacity;
            return;
        }

        next_page_capacity_value =
            growth_base >
                maximum_page_capacity / 2
            ? maximum_page_capacity
            : growth_base * 2;
    }

    std::size_t initial_page_capacity = 1;
    std::size_t maximum_page_capacity = 1;
    std::size_t next_page_capacity_value = 1;
    std::size_t logical_size_value = 0;
    std::vector<std::vector<T>> pages;
};

struct source_frontend_block_record final {
    source_id source{};
    source_frontend_block_range local_types{};
    source_frontend_block_range type_slots{};
    source_frontend_block_range object_slots{};
    source_frontend_block_range member_slots{};
};

} // namespace

class source_frontend_block_store::implementation final {
public:
    explicit implementation(
        std::size_t page_bytes) noexcept
        : local_types(page_bytes),
          type_slots(page_bytes),
          object_slots(page_bytes),
          member_slots(page_bytes) {}

    [[nodiscard]] status append(
        source_id source,
        source_interface_data_view data,
        source_frontend_block_ref& output) noexcept {

        output = {};

        if (!source)
            return {status_code::invalid_argument};

        if (records.size() >=
            static_cast<std::size_t>(
                (std::numeric_limits<std::uint32_t>::max)())) {
            return {status_code::not_available};
        }

        const auto local_checkpoint =
            local_types.mark();
        const auto type_checkpoint =
            type_slots.mark();
        const auto object_checkpoint =
            object_slots.mark();
        const auto member_checkpoint =
            member_slots.mark();

        source_frontend_block_record record;
        record.source = source;

        auto result =
            local_types.append(
                data.local_types,
                record.local_types);

        if (result.ok()) {
            result =
                type_slots.append(
                    data.type_slots,
                    record.type_slots);
        }

        if (result.ok()) {
            result =
                object_slots.append(
                    data.object_slots,
                    record.object_slots);
        }

        if (result.ok()) {
            result =
                member_slots.append(
                    data.member_slots,
                    record.member_slots);
        }

        if (!result.ok()) {
            local_types.restore(local_checkpoint);
            type_slots.restore(type_checkpoint);
            object_slots.restore(object_checkpoint);
            member_slots.restore(member_checkpoint);
            return result;
        }

        try {
            records.push_back(record);
        }
        catch (const std::bad_alloc&) {
            local_types.restore(local_checkpoint);
            type_slots.restore(type_checkpoint);
            object_slots.restore(object_checkpoint);
            member_slots.restore(member_checkpoint);
            return {status_code::not_available};
        }
        catch (const std::length_error&) {
            local_types.restore(local_checkpoint);
            type_slots.restore(type_checkpoint);
            object_slots.restore(object_checkpoint);
            member_slots.restore(member_checkpoint);
            return {status_code::not_available};
        }

        output =
            source_frontend_block_ref{
                static_cast<std::uint32_t>(
                    records.size())};

        return {};
    }

    [[nodiscard]] status view(
        source_frontend_block_ref block,
        source_interface_data_view& output) const noexcept {

        output = {};

        if (!block ||
            static_cast<std::size_t>(block.value()) >
                records.size()) {
            return {status_code::not_found};
        }

        const auto& record =
            records[
                static_cast<std::size_t>(
                    block.value() - 1)];

        const auto local =
            local_types.view(record.local_types);
        const auto types =
            type_slots.view(record.type_slots);
        const auto objects =
            object_slots.view(record.object_slots);
        const auto members =
            member_slots.view(record.member_slots);

        if ((record.local_types.count != 0 &&
                local.size() != record.local_types.count) ||
            (record.type_slots.count != 0 &&
                types.size() != record.type_slots.count) ||
            (record.object_slots.count != 0 &&
                objects.size() != record.object_slots.count) ||
            (record.member_slots.count != 0 &&
                members.size() != record.member_slots.count)) {
            return {status_code::invalid_state};
        }

        output = {
            local,
            types,
            objects,
            members,
        };

        return {};
    }

    [[nodiscard]] source_id source(
        source_frontend_block_ref block) const noexcept {

        if (!block ||
            static_cast<std::size_t>(block.value()) >
                records.size()) {
            return {};
        }

        return records[
            static_cast<std::size_t>(
                block.value() - 1)].source;
    }

    [[nodiscard]] status layout(
        source_frontend_block_ref block,
        source_frontend_block_layout& output) const noexcept {

        output = {};

        if (!block ||
            static_cast<std::size_t>(block.value()) >
                records.size()) {
            return {status_code::not_found};
        }

        const auto& record =
            records[
                static_cast<std::size_t>(
                    block.value() - 1)];

        output.source = record.source;
        output.local_types = {
            record.local_types.logical_begin,
            record.local_types.count,
        };
        output.type_slots = {
            record.type_slots.logical_begin,
            record.type_slots.count,
        };
        output.object_slots = {
            record.object_slots.logical_begin,
            record.object_slots.count,
        };
        output.member_slots = {
            record.member_slots.logical_begin,
            record.member_slots.count,
        };
        return {};
    }

    [[nodiscard]] std::array<std::size_t, 17>
    mark() const noexcept {
        const auto local = local_types.mark();
        const auto types = type_slots.mark();
        const auto objects = object_slots.mark();
        const auto members = member_slots.mark();

        return {
            records.size(),
            local.page_count,
            local.last_size,
            local.logical_size,
            local.next_page_capacity,
            types.page_count,
            types.last_size,
            types.logical_size,
            types.next_page_capacity,
            objects.page_count,
            objects.last_size,
            objects.logical_size,
            objects.next_page_capacity,
            members.page_count,
            members.last_size,
            members.logical_size,
            members.next_page_capacity,
        };
    }

    void restore(
        const std::array<std::size_t, 17>& state) noexcept {

        if (records.size() > state[0])
            records.resize(state[0]);

        local_types.restore({
            state[1],
            state[2],
            state[3],
            state[4],
        });
        type_slots.restore({
            state[5],
            state[6],
            state[7],
            state[8],
        });
        object_slots.restore({
            state[9],
            state[10],
            state[11],
            state[12],
        });
        member_slots.restore({
            state[13],
            state[14],
            state[15],
            state[16],
        });
    }

    void clear() noexcept {
        records.clear();
        local_types.clear();
        type_slots.clear();
        object_slots.clear();
        member_slots.clear();
    }

    std::vector<source_frontend_block_record> records;
    typed_page_store<identity_ref> local_types;
    typed_page_store<source_interface_type_slot> type_slots;
    typed_page_store<source_interface_object_slot> object_slots;
    typed_page_store<source_interface_member_slot> member_slots;
};

source_frontend_block_store::source_frontend_block_store(
    std::size_t page_bytes) noexcept {

    try {
        value =
            std::make_shared<implementation>(
                (std::max)(std::size_t{1}, page_bytes));
    }
    catch (const std::bad_alloc&) {
        value.reset();
    }
}

source_frontend_block_store::source_frontend_block_store(
    source_frontend_block_store&&) noexcept = default;

source_frontend_block_store&
source_frontend_block_store::operator=(
    source_frontend_block_store&&) noexcept = default;

source_frontend_block_store::~source_frontend_block_store() = default;

status source_frontend_block_store::append(
    source_id source,
    source_interface_data_view data,
    source_frontend_block_ref& output) noexcept {

    output = {};

    if (!value)
        return {status_code::not_available};

    return value->append(
        source,
        data,
        output);
}

status source_frontend_block_store::view(
    source_frontend_block_ref block,
    source_interface_data_view& output) const noexcept {

    output = {};

    if (!value)
        return {status_code::not_available};

    return value->view(
        block,
        output);
}

source_id source_frontend_block_store::source(
    source_frontend_block_ref block) const noexcept {

    return value
        ? value->source(block)
        : source_id{};
}

status source_frontend_block_store::layout(
    source_frontend_block_ref block,
    source_frontend_block_layout& output) const noexcept {

    output = {};

    if (!value)
        return {status_code::not_available};

    return value->layout(block, output);
}

std::size_t source_frontend_block_store::local_type_count() const noexcept {
    return value ? value->local_types.size() : 0;
}

std::size_t source_frontend_block_store::type_slot_count() const noexcept {
    return value ? value->type_slots.size() : 0;
}

std::size_t source_frontend_block_store::object_slot_count() const noexcept {
    return value ? value->object_slots.size() : 0;
}

std::size_t source_frontend_block_store::member_slot_count() const noexcept {
    return value ? value->member_slots.size() : 0;
}

std::size_t source_frontend_block_store::local_type_page_count() const noexcept {
    return value ? value->local_types.page_count() : 0;
}

std::size_t source_frontend_block_store::type_slot_page_count() const noexcept {
    return value ? value->type_slots.page_count() : 0;
}

std::size_t source_frontend_block_store::object_slot_page_count() const noexcept {
    return value ? value->object_slots.page_count() : 0;
}

std::size_t source_frontend_block_store::member_slot_page_count() const noexcept {
    return value ? value->member_slots.page_count() : 0;
}

std::span<const identity_ref>
source_frontend_block_store::local_type_page(
    std::size_t index) const noexcept {

    return value
        ? value->local_types.page(index)
        : std::span<const identity_ref>{};
}

std::span<const source_interface_type_slot>
source_frontend_block_store::type_slot_page(
    std::size_t index) const noexcept {

    return value
        ? value->type_slots.page(index)
        : std::span<const source_interface_type_slot>{};
}

std::span<const source_interface_object_slot>
source_frontend_block_store::object_slot_page(
    std::size_t index) const noexcept {

    return value
        ? value->object_slots.page(index)
        : std::span<const source_interface_object_slot>{};
}

std::span<const source_interface_member_slot>
source_frontend_block_store::member_slot_page(
    std::size_t index) const noexcept {

    return value
        ? value->member_slots.page(index)
        : std::span<const source_interface_member_slot>{};
}

std::size_t source_frontend_block_store::block_count() const noexcept {
    return value
        ? value->records.size()
        : 0;
}

source_frontend_block_store_lifetime
source_frontend_block_store::pin_lifetime() const noexcept {

    source_frontend_block_store_lifetime output;
    output.owner = value;
    return output;
}

source_frontend_block_store::checkpoint
source_frontend_block_store::mark() const noexcept {
    checkpoint output;

    if (value)
        output.state = value->mark();

    return output;
}

void source_frontend_block_store::restore(
    checkpoint state) noexcept {

    if (value)
        value->restore(state.state);
}

void source_frontend_block_store::clear() noexcept {
    if (value)
        value->clear();
}

} // namespace cw::server
