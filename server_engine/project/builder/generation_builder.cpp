#include "generation_builder.hpp"

#include "../../diagnostics/diagnostic_buffer.hpp"
#include "../../diagnostics/diagnostic_descriptor.hpp"

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cw::server {
namespace {

using clock_type = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t elapsed_ns(
    clock_type::time_point begin,
    clock_type::time_point end) noexcept {

    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
}

void emit_failure(
    status result,
    source_id source,
    operation_id operation,
    std::string detail,
    diagnostic_buffer& diagnostics) noexcept {

    try {
        detail.append("; status=");
        detail.append(std::to_string(static_cast<std::uint32_t>(result.code)));
        diagnostics.emit({
            diagnostics::generation_build_failed.id,
            diagnostics::generation_build_failed.default_severity,
            operation,
            source_range{source, 0, 0},
            std::move(detail),
        });
    } catch (...) {
    }
}

[[nodiscard]] std::uint64_t mix64(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

[[nodiscard]] std::uint32_t fold32(std::uint64_t value) noexcept {
    auto result = static_cast<std::uint32_t>(value ^ (value >> 32));
    return result == 0 ? 1u : result;
}

[[nodiscard]] bool checked_index_capacity(
    std::size_t count,
    std::size_t& output) noexcept {

    if (count == 0) {
        output = 0;
        return true;
    }
    if (count > (std::numeric_limits<std::size_t>::max)() / 2)
        return false;

    const auto minimum = count * 2;
    output = std::bit_ceil(minimum < 8 ? std::size_t{8} : minimum);
    return output >= minimum;
}

[[nodiscard]] bool checked_add_size(
    std::size_t& target,
    std::size_t value) noexcept {

    if (value > (std::numeric_limits<std::size_t>::max)() - target)
        return false;
    target += value;
    return true;
}

[[nodiscard]] bool headroom_capacity(std::size_t size, std::size_t& output) noexcept {
    const auto extra = size / 16 + 64;
    if (size > (std::numeric_limits<std::size_t>::max)() - extra)
        return false;
    output = size + extra;
    return true;
}

template<class T>
[[nodiscard]] status reserve_headroom(std::vector<T>& values) noexcept {
    std::size_t capacity = 0;
    if (!headroom_capacity(values.size(), capacity))
        return {status_code::not_available};
    try {
        values.reserve(capacity);
        return {};
    } catch (const std::bad_alloc&) {
        return {status_code::initialization_failed};
    } catch (const std::length_error&) {
        return {status_code::not_available};
    } catch (...) {
        return {status_code::initialization_failed};
    }
}

[[nodiscard]] bool is_union(source_record_kind kind) noexcept {
    return kind == source_record_kind::union_type;
}

[[nodiscard]] bool compatible_record_kind(
    source_record_kind left,
    source_record_kind right) noexcept {

    return is_union(left) == is_union(right);
}


[[nodiscard]] bool increment_count(std::uint32_t& value) noexcept {
    if (value == (std::numeric_limits<std::uint32_t>::max)())
        return false;
    ++value;
    return true;
}

[[nodiscard]] bool decrement_count(std::uint32_t& value) noexcept {
    if (value == 0)
        return false;
    --value;
    return true;
}

[[nodiscard]] status add_construction(
    source_construction_state& state,
    const source_contribution_type& contribution,
    std::uint32_t one_based_type_index) noexcept {

    if (contribution.identity == nullptr || one_based_type_index == 0)
        return {status_code::invalid_argument};

    if (state.declarations == 0) {
        state.kind = contribution.kind;
    } else if (state.kind != contribution.kind) {
        return {status_code::semantic_conflict};
    }

    if (!increment_count(state.declarations))
        return {status_code::not_available};

    if (contribution.kind == source_contribution_type_kind::record) {
        auto* count = &state.record_struct;
        if (contribution.record_kind == source_record_kind::class_type)
            count = &state.record_class;
        else if (contribution.record_kind == source_record_kind::union_type)
            count = &state.record_union;
        if (!increment_count(*count))
            return {status_code::not_available};
        if (state.record_union != 0 &&
            (state.record_struct != 0 || state.record_class != 0)) {
            return {status_code::semantic_conflict};
        }
    } else {
        auto& scoped_count = contribution.enum_scoped() ? state.enum_scoped : state.enum_unscoped;
        if (!increment_count(scoped_count))
            return {status_code::not_available};
        if (state.enum_scoped != 0 && state.enum_unscoped != 0)
            return {status_code::semantic_conflict};

        if (contribution.explicit_underlying != intrinsic_type::none) {
            if (state.enum_fixed != 0 &&
                state.fixed_underlying != contribution.explicit_underlying) {
                return {status_code::semantic_conflict};
            }
            if (!increment_count(state.enum_fixed))
                return {status_code::not_available};
            state.fixed_underlying = contribution.explicit_underlying;
        }
    }

    if (contribution.definition()) {
        if (state.definitions != 0)
            return {status_code::semantic_conflict};
        state.definitions = 1;
        state.definition_type = one_based_type_index;
    }
    return {};
}

[[nodiscard]] status remove_construction(
    source_construction_state& state,
    const source_contribution_type& contribution,
    std::uint32_t one_based_type_index) noexcept {

    if (state.declarations == 0 || state.kind != contribution.kind)
        return {status_code::invalid_argument};

    if (contribution.definition()) {
        if (state.definitions != 1 || state.definition_type != one_based_type_index)
            return {status_code::invalid_argument};
        state.definitions = 0;
        state.definition_type = 0;
    }

    if (contribution.kind == source_contribution_type_kind::record) {
        auto* count = &state.record_struct;
        if (contribution.record_kind == source_record_kind::class_type)
            count = &state.record_class;
        else if (contribution.record_kind == source_record_kind::union_type)
            count = &state.record_union;
        if (!decrement_count(*count))
            return {status_code::invalid_argument};
    } else {
        auto& scoped_count = contribution.enum_scoped() ? state.enum_scoped : state.enum_unscoped;
        if (!decrement_count(scoped_count))
            return {status_code::invalid_argument};
        if (contribution.explicit_underlying != intrinsic_type::none) {
            if (state.enum_fixed == 0 || state.fixed_underlying != contribution.explicit_underlying)
                return {status_code::invalid_argument};
            --state.enum_fixed;
            if (state.enum_fixed == 0)
                state.fixed_underlying = intrinsic_type::none;
        }
    }

    if (!decrement_count(state.declarations))
        return {status_code::invalid_argument};
    if (state.declarations == 0)
        state = {};
    return {};
}

[[nodiscard]] std::uint64_t identity_hash(identity_ref identity) noexcept {
    return mix64(static_cast<std::uint64_t>(identity.value()));
}

[[nodiscard]] std::uint64_t derived_hash(
    derived_type_kind kind,
    std::uint32_t child,
    std::uint64_t payload) noexcept {

    return mix64(
        (static_cast<std::uint64_t>(static_cast<std::uint8_t>(kind)) << 56) ^
        (static_cast<std::uint64_t>(child) << 16) ^ mix64(payload));
}

class sparse_u32_map final {
public:
    [[nodiscard]] status reserve(std::size_t expected) noexcept {
        std::size_t capacity = 0;
        if (!checked_index_capacity(expected == 0 ? 1 : expected, capacity))
            return {status_code::not_available};
        try {
            slots.assign(capacity, {});
            return {};
        } catch (...) {
            return {status_code::initialization_failed};
        }
    }

    [[nodiscard]] std::uint32_t find(std::uint32_t key) const noexcept {
        if (key == 0 || slots.empty())
            return 0;
        const auto hash = mix64(key);
        const auto mask = slots.size() - 1;
        auto position = static_cast<std::size_t>(hash) & mask;
        for (std::size_t probe = 0; probe < slots.size(); ++probe) {
            const auto& current_slot = slots[position];
            if (current_slot.key == 0)
                return 0;
            if (current_slot.key == key)
                return current_slot.value;
            position = (position + 1) & mask;
        }
        return 0;
    }

    [[nodiscard]] status insert(std::uint32_t key, std::uint32_t value) noexcept {
        if (key == 0 || value == 0)
            return {status_code::invalid_argument};
        if (slots.empty()) {
            const auto result = reserve(4);
            if (!result.ok())
                return result;
        }
        if ((count + 1) * 2 > slots.size()) {
            const auto result = grow();
            if (!result.ok())
                return result;
        }
        return insert_no_grow(key, value);
    }

private:
    struct slot final {
        std::uint32_t key = 0;
        std::uint32_t value = 0;
    };

    [[nodiscard]] status insert_no_grow(std::uint32_t key, std::uint32_t value) noexcept {
        const auto hash = mix64(key);
        const auto mask = slots.size() - 1;
        auto position = static_cast<std::size_t>(hash) & mask;
        for (std::size_t probe = 0; probe < slots.size(); ++probe) {
            auto& current = slots[position];
            if (current.key == 0) {
                current = {key, value};
                ++count;
                return {};
            }
            if (current.key == key) {
                current.value = value;
                return {};
            }
            position = (position + 1) & mask;
        }
        return {status_code::not_available};
    }

    [[nodiscard]] status grow() noexcept {
        if (slots.size() > (std::numeric_limits<std::size_t>::max)() / 2)
            return {status_code::not_available};
        try {
            auto old = std::move(slots);
            slots.assign(old.size() * 2, {});
            count = 0;
            for (const auto& current : old) {
                if (current.key != 0) {
                    const auto result = insert_no_grow(current.key, current.value);
                    if (!result.ok())
                        return result;
                }
            }
            return {};
        } catch (...) {
            return {status_code::initialization_failed};
        }
    }

    std::vector<slot> slots;
    std::size_t count = 0;
};

class sparse_handle_set final {
public:
    [[nodiscard]] status reserve(std::size_t expected) noexcept {
        std::size_t capacity = 0;
        if (!checked_index_capacity(expected == 0 ? 1 : expected, capacity))
            return {status_code::not_available};
        try {
            slots.assign(capacity, 0);
            return {};
        } catch (...) {
            return {status_code::initialization_failed};
        }
    }

    [[nodiscard]] status insert(std::uint32_t value, bool& inserted) noexcept {
        inserted = false;
        if (value == 0)
            return {status_code::invalid_argument};
        if (slots.empty()) {
            const auto result = reserve(4);
            if (!result.ok())
                return result;
        }
        if ((count + 1) * 2 > slots.size()) {
            const auto result = grow();
            if (!result.ok())
                return result;
        }
        const auto mask = slots.size() - 1;
        auto position = static_cast<std::size_t>(mix64(value)) & mask;
        for (std::size_t probe = 0; probe < slots.size(); ++probe) {
            auto& current = slots[position];
            if (current == 0) {
                current = value;
                ++count;
                inserted = true;
                return {};
            }
            if (current == value)
                return {};
            position = (position + 1) & mask;
        }
        return {status_code::not_available};
    }

    [[nodiscard]] bool contains(std::uint32_t value) const noexcept {
        if (value == 0 || slots.empty())
            return false;
        const auto mask = slots.size() - 1;
        auto position = static_cast<std::size_t>(mix64(value)) & mask;
        for (std::size_t probe = 0; probe < slots.size(); ++probe) {
            const auto current = slots[position];
            if (current == 0)
                return false;
            if (current == value)
                return true;
            position = (position + 1) & mask;
        }
        return false;
    }

private:
    [[nodiscard]] status grow() noexcept {
        if (slots.size() > (std::numeric_limits<std::size_t>::max)() / 2)
            return {status_code::not_available};
        try {
            auto old = std::move(slots);
            slots.assign(old.size() * 2, 0);
            count = 0;
            for (const auto value : old) {
                if (value == 0)
                    continue;
                bool inserted = false;
                const auto result = insert(value, inserted);
                if (!result.ok())
                    return result;
            }
            return {};
        } catch (...) {
            return {status_code::initialization_failed};
        }
    }

    std::vector<std::uint32_t> slots;
    std::size_t count = 0;
};

class sparse_identity_map final {
public:
    [[nodiscard]] status reserve(std::size_t expected) noexcept {
        std::size_t capacity = 0;
        if (!checked_index_capacity(expected == 0 ? 1 : expected, capacity))
            return {status_code::not_available};
        try {
            slots.assign(capacity, {});
            return {};
        } catch (...) {
            return {status_code::initialization_failed};
        }
    }

    [[nodiscard]] std::uint32_t find(identity_ref key) const noexcept {
        if (key == nullptr || slots.empty())
            return 0;
        const auto hash = identity_hash(key);
        const auto mask = slots.size() - 1;
        auto position = static_cast<std::size_t>(hash) & mask;
        for (std::size_t probe = 0; probe < slots.size(); ++probe) {
            const auto& current_slot = slots[position];
            if (current_slot.identity == nullptr)
                return 0;
            if (current_slot.identity == key)
                return current_slot.handle;
            position = (position + 1) & mask;
        }
        return 0;
    }

    [[nodiscard]] status insert(identity_ref key, std::uint32_t handle) noexcept {
        if (key == nullptr || handle == 0 || slots.empty())
            return {status_code::invalid_argument};
        const auto hash = identity_hash(key);
        const auto mask = slots.size() - 1;
        auto position = static_cast<std::size_t>(hash) & mask;
        for (std::size_t probe = 0; probe < slots.size(); ++probe) {
            auto& current_slot = slots[position];
            if (current_slot.identity == nullptr) {
                current_slot = {key, handle};
                return {};
            }
            if (current_slot.identity == key) {
                current_slot.handle = handle;
                return {};
            }
            position = (position + 1) & mask;
        }
        return {status_code::not_available};
    }

private:
    struct slot final {
        identity_ref identity = nullptr;
        std::uint32_t handle = 0;
    };
    std::vector<slot> slots;
};

void insert_identity_slot(
    std::vector<graph_identity_index_slot>& slots,
    std::span<const identity_ref> identities,
    identity_ref identity,
    std::uint32_t handle) noexcept {

    const auto hash = identity_hash(identity);
    const auto fingerprint = fold32(hash);
    const auto mask = slots.size() - 1;
    auto position = static_cast<std::size_t>(hash) & mask;
    for (;;) {
        auto& slot = slots[position];
        if (slot.handle == 0) {
            slot.fingerprint = fingerprint;
            slot.handle = handle;
            return;
        }
        if (slot.fingerprint == fingerprint && slot.handle <= identities.size() &&
            identities[slot.handle - 1] == identity) {
            return;
        }
        position = (position + 1) & mask;
    }
}

void insert_object_identity_slot(
    std::vector<graph_object_identity_index_slot>& slots,
    std::span<const identity_ref> identities,
    identity_ref identity,
    std::uint32_t handle) noexcept {

    const auto hash = identity_hash(identity);
    const auto fingerprint = fold32(hash);
    const auto mask = slots.size() - 1;
    auto position = static_cast<std::size_t>(hash) & mask;
    for (;;) {
        auto& slot = slots[position];
        if (slot.handle == 0) {
            slot.fingerprint = fingerprint;
            slot.handle = handle;
            return;
        }
        if (slot.fingerprint == fingerprint && slot.handle <= identities.size() &&
            identities[slot.handle - 1] == identity) {
            return;
        }
        position = (position + 1) & mask;
    }
}

[[nodiscard]] std::uint64_t endpoint_hash(object_endpoint endpoint) noexcept {
    return mix64(
        (static_cast<std::uint64_t>(endpoint.object.value()) << 32) ^
        static_cast<std::uint64_t>(endpoint.member.value()));
}

class sparse_endpoint_set final {
public:
    [[nodiscard]] status reserve(std::size_t expected) noexcept {
        std::size_t capacity = 0;
        if (!checked_index_capacity(expected == 0 ? 1 : expected, capacity))
            return {status_code::not_available};
        try {
            slots.assign(capacity, {});
            return {};
        } catch (...) {
            return {status_code::initialization_failed};
        }
    }

    [[nodiscard]] status insert(object_endpoint value, bool& inserted) noexcept {
        inserted = false;
        if (!value.object || !value.member)
            return {status_code::invalid_argument};
        if (slots.empty()) {
            const auto result = reserve(4);
            if (!result.ok())
                return result;
        }
        if ((count + 1) * 2 > slots.size()) {
            const auto result = grow();
            if (!result.ok())
                return result;
        }
        return insert_no_grow(value, inserted);
    }

private:
    struct slot final {
        object_endpoint endpoint{};
        bool occupied = false;
    };

    [[nodiscard]] status insert_no_grow(object_endpoint value, bool& inserted) noexcept {
        inserted = false;
        const auto mask = slots.size() - 1;
        auto position = static_cast<std::size_t>(endpoint_hash(value)) & mask;
        for (std::size_t probe = 0; probe < slots.size(); ++probe) {
            auto& current = slots[position];
            if (!current.occupied) {
                current.endpoint = value;
                current.occupied = true;
                ++count;
                inserted = true;
                return {};
            }
            if (current.endpoint == value)
                return {};
            position = (position + 1) & mask;
        }
        return {status_code::not_available};
    }

    [[nodiscard]] status grow() noexcept {
        if (slots.size() > (std::numeric_limits<std::size_t>::max)() / 2)
            return {status_code::not_available};
        try {
            auto old = std::move(slots);
            slots.assign(old.size() * 2, {});
            count = 0;
            for (const auto& current : old) {
                if (!current.occupied)
                    continue;
                bool inserted = false;
                const auto result = insert_no_grow(current.endpoint, inserted);
                if (!result.ok())
                    return result;
            }
            return {};
        } catch (...) {
            return {status_code::initialization_failed};
        }
    }

    std::vector<slot> slots;
    std::size_t count = 0;
};

[[nodiscard]] bool insert_link_slot(
    std::vector<graph_link_index_slot>& slots,
    std::span<const link_record> links,
    object_endpoint target,
    std::uint32_t handle) noexcept {

    if (slots.empty() || !target.object || !target.member || handle == 0)
        return false;
    const auto hash = endpoint_hash(target);
    const auto fingerprint = fold32(hash);
    const auto mask = slots.size() - 1;
    auto position = static_cast<std::size_t>(hash) & mask;
    for (std::size_t probe = 0; probe < slots.size(); ++probe) {
        auto& slot = slots[position];
        if (slot.handle == 0) {
            slot.fingerprint = fingerprint;
            slot.handle = handle;
            return true;
        }
        if (slot.fingerprint == fingerprint && slot.handle <= links.size() &&
            links[slot.handle - 1].target == target) {
            return false;
        }
        position = (position + 1) & mask;
    }
    return false;
}

[[nodiscard]] derived_type_kind derived_kind(source_type_modifier_kind kind) noexcept {
    switch (kind) {
    case source_type_modifier_kind::const_qualified:
        return derived_type_kind::const_qualified;
    case source_type_modifier_kind::volatile_qualified:
        return derived_type_kind::volatile_qualified;
    case source_type_modifier_kind::pointer:
        return derived_type_kind::pointer;
    case source_type_modifier_kind::lvalue_reference:
        return derived_type_kind::lvalue_reference;
    case source_type_modifier_kind::rvalue_reference:
        return derived_type_kind::rvalue_reference;
    case source_type_modifier_kind::bounded_array:
        return derived_type_kind::bounded_array;
    case source_type_modifier_kind::unbounded_array:
        return derived_type_kind::unbounded_array;
    }
    return derived_type_kind::pointer;
}

[[nodiscard]] std::uint8_t intrinsic_width(
    intrinsic_type type,
    const abi_configuration& abi) noexcept {

    switch (type) {
    case intrinsic_type::bool_type:
        return 1;
    case intrinsic_type::char_type:
    case intrinsic_type::signed_char:
    case intrinsic_type::unsigned_char:
    case intrinsic_type::char8_type:
        return 8;
    case intrinsic_type::wchar_type:
        return abi.target == abi_target::windows_x64 ? 16 : 32;
    case intrinsic_type::char16_type:
    case intrinsic_type::signed_short:
    case intrinsic_type::unsigned_short:
        return 16;
    case intrinsic_type::char32_type:
    case intrinsic_type::signed_int:
    case intrinsic_type::unsigned_int:
        return 32;
    case intrinsic_type::signed_long:
    case intrinsic_type::unsigned_long:
        return abi.target == abi_target::windows_x64 ? 32 : 64;
    case intrinsic_type::signed_long_long:
    case intrinsic_type::unsigned_long_long:
        return 64;
    default:
        return 0;
    }
}

[[nodiscard]] bool intrinsic_signed(intrinsic_type type) noexcept {
    switch (type) {
    case intrinsic_type::char_type:
    case intrinsic_type::signed_char:
    case intrinsic_type::wchar_type:
    case intrinsic_type::signed_short:
    case intrinsic_type::signed_int:
    case intrinsic_type::signed_long:
    case intrinsic_type::signed_long_long:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] std::int64_t signed_value(
    source_integral_constant value,
    const abi_configuration& abi) noexcept {

    const auto width = intrinsic_width(value.intrinsic, abi);
    if (width == 64)
        return static_cast<std::int64_t>(value.bits);
    if (width == 0)
        return 0;
    return static_cast<std::int64_t>(value.bits << (64 - width)) >> (64 - width);
}

[[nodiscard]] status select_enum_underlying(
    std::span<const source_contribution_enum_value> values,
    const abi_configuration& abi,
    intrinsic_type& output) noexcept {

    std::int64_t minimum = 0;
    std::uint64_t maximum = 0;
    bool negative = false;

    for (const auto& item : values) {
        const auto width = intrinsic_width(item.value.intrinsic, abi);
        if (width == 0)
            return {status_code::invalid_argument};

        if (intrinsic_signed(item.value.intrinsic)) {
            const auto value = signed_value(item.value, abi);
            if (value < 0) {
                negative = true;
                if (value < minimum)
                    minimum = value;
            } else if (static_cast<std::uint64_t>(value) > maximum) {
                maximum = static_cast<std::uint64_t>(value);
            }
        } else if (item.value.bits > maximum) {
            maximum = item.value.bits;
        }
    }

    if (minimum >= (std::numeric_limits<std::int32_t>::min)() &&
        maximum <= static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())) {
        output = intrinsic_type::signed_int;
        return {};
    }
    if (!negative && maximum <= (std::numeric_limits<std::uint32_t>::max)()) {
        output = intrinsic_type::unsigned_int;
        return {};
    }
    if (maximum <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        output = abi.target == abi_target::windows_x64 ?
            intrinsic_type::signed_long_long : intrinsic_type::signed_long;
        return {};
    }
    if (!negative) {
        output = abi.target == abi_target::windows_x64 ?
            intrinsic_type::unsigned_long_long : intrinsic_type::unsigned_long;
        return {};
    }
    return {status_code::semantic_conflict};
}

} // namespace

generation_builder::generation_builder(
    source_contribution_cache& contribution_cache_value,
    graph& target_graph) noexcept
    : contribution_cache(contribution_cache_value), target(target_graph) {
}

status generation_builder::prepare_g0(
    std::span<const source_facts> sources,
    const abi_configuration& abi,
    operation_id operation,
    diagnostic_buffer& diagnostics) noexcept {

    if (prepared || published_value || !is_supported_abi_configuration(abi))
        return {status_code::invalid_argument};

    contributions = contribution_cache.begin_rebuild();
    mode = build_mode::rebuild;
    telemetry_value = {};
    const auto total_begin = clock_type::now();
    const auto capture_begin = total_begin;

    std::size_t max_source_id = 0;
    std::size_t total_types = 0;
    std::size_t total_members = 0;
    std::size_t total_modifiers = 0;
    std::size_t total_enum_values = 0;
    std::size_t total_objects = 0;
    std::size_t total_links = 0;
    for (const auto& facts : sources) {
        max_source_id = std::max(
            max_source_id, static_cast<std::size_t>(facts.source().value()));
        if (!checked_add_size(total_types, facts.records().size()) ||
            !checked_add_size(total_types, facts.enums().size()) ||
            !checked_add_size(total_members, facts.members().size()) ||
            !checked_add_size(total_modifiers, facts.modifiers().size()) ||
            !checked_add_size(total_enum_values, facts.enum_values().size()) ||
            !checked_add_size(total_objects, facts.objects().size()) ||
            !checked_add_size(total_links, facts.links().size())) {
            const status result{status_code::not_available};
            emit_failure(result, facts.source(), operation, "G0 contribution counts overflow", diagnostics);
            return result;
        }
    }

    auto reserve_result = contributions.reserve_rebuild(
        max_source_id, total_types, total_members, total_modifiers,
        total_enum_values, total_objects, total_links);
    if (!reserve_result.ok()) {
        emit_failure(reserve_result, {}, operation, "SourceContribution G0 reserve failed", diagnostics);
        return reserve_result;
    }

    for (const auto& facts : sources) {
        const auto result = contributions.replace(facts, operation, diagnostics);
        if (!result.ok())
            return result;
    }

    const auto capture_end = clock_type::now();
    telemetry_value.contribution_capture_ns = elapsed_ns(capture_begin, capture_end);

    const auto graph_result = prepare_graph(abi, operation, diagnostics);
    if (!graph_result.ok())
        return graph_result;

    const auto publish_prepare_begin = clock_type::now();
    const auto contribution_result = contributions.prepare_publish();
    if (!contribution_result.ok()) {
        emit_failure(contribution_result, {}, operation, "SourceContribution prepare failed", diagnostics);
        return contribution_result;
    }
    const auto publish_prepare_end = clock_type::now();
    telemetry_value.prepare_publish_ns = elapsed_ns(publish_prepare_begin, publish_prepare_end);
    telemetry_value.total_prepare_ns = elapsed_ns(total_begin, publish_prepare_end);

    const auto statistics = contributions.statistics();
    telemetry_value.sources = statistics.sources;
    telemetry_value.type_declarations = statistics.type_declarations;
    telemetry_value.members = statistics.members;
    telemetry_value.enum_values = statistics.enum_values;
    telemetry_value.objects = statistics.objects;
    telemetry_value.links = statistics.links;
    telemetry_value.unique_types = prepared_graph.types.size();
    telemetry_value.canonical_type_refs =
        prepared_graph.canonical_types.empty() ? 0 : prepared_graph.canonical_types.size() - 1;

    prepared = true;
    return {};
}

status generation_builder::prepare_incremental(
    std::span<const source_facts> replacements,
    std::span<const source_id> removals,
    const abi_configuration& abi,
    operation_id operation,
    diagnostic_buffer& diagnostics) noexcept {

    if (prepared || published_value || !is_supported_abi_configuration(abi) ||
        !contribution_cache.complete()) {
        return {status_code::invalid_argument};
    }

    sparse_contributions = contribution_cache.begin_incremental();
    mode = build_mode::incremental;
    telemetry_value = {};
    const auto total_begin = clock_type::now();
    const auto capture_begin = total_begin;

    std::size_t total_types = 0;
    std::size_t total_members = 0;
    std::size_t total_modifiers = 0;
    std::size_t total_enum_values = 0;
    std::size_t total_objects = 0;
    std::size_t total_links = 0;
    std::size_t touched_upper = 0;

    for (const auto& facts : replacements) {
        if (!facts.source())
            return {status_code::invalid_argument};
        if (!checked_add_size(total_types, facts.records().size()) ||
            !checked_add_size(total_types, facts.enums().size()) ||
            !checked_add_size(total_members, facts.members().size()) ||
            !checked_add_size(total_modifiers, facts.modifiers().size()) ||
            !checked_add_size(total_enum_values, facts.enum_values().size()) ||
            !checked_add_size(total_objects, facts.objects().size()) ||
            !checked_add_size(total_links, facts.links().size())) {
            const status result{status_code::not_available};
            emit_failure(result, facts.source(), operation, "incremental contribution counts overflow", diagnostics);
            return result;
        }
        const auto* previous = contribution_cache.state(facts.source());
        if (previous != nullptr && !checked_add_size(touched_upper, previous->types.count)) {
            const status result{status_code::not_available};
            emit_failure(result, facts.source(), operation, "incremental touched-type count overflow", diagnostics);
            return result;
        }
        if (!checked_add_size(touched_upper, facts.records().size()) ||
            !checked_add_size(touched_upper, facts.enums().size())) {
            const status result{status_code::not_available};
            emit_failure(result, facts.source(), operation, "incremental touched-type count overflow", diagnostics);
            return result;
        }
    }

    for (const auto source : removals) {
        if (!source)
            return {status_code::invalid_argument};
        const auto* previous = contribution_cache.state(source);
        if (previous != nullptr && !checked_add_size(touched_upper, previous->types.count)) {
            const status result{status_code::not_available};
            emit_failure(result, source, operation, "incremental touched-type count overflow", diagnostics);
            return result;
        }
    }

    std::size_t source_changes = replacements.size();
    if (!checked_add_size(source_changes, removals.size())) {
        const status result{status_code::not_available};
        emit_failure(result, {}, operation, "incremental source-change count overflow", diagnostics);
        return result;
    }

    auto result = sparse_contributions.reserve_incremental(
        source_changes, touched_upper, total_types, total_members, total_modifiers,
        total_enum_values, total_objects, total_links);
    if (!result.ok()) {
        emit_failure(result, {}, operation, "SourceContribution incremental reserve failed", diagnostics);
        return result;
    }

    for (const auto& facts : replacements) {
        result = sparse_contributions.replace(facts, operation, diagnostics);
        if (!result.ok())
            return result;
    }
    for (const auto source : removals) {
        result = sparse_contributions.remove(source);
        if (!result.ok()) {
            emit_failure(result, source, operation, "SourceContribution removal failed", diagnostics);
            return result;
        }
    }

    const auto capture_end = clock_type::now();
    telemetry_value.contribution_capture_ns = elapsed_ns(capture_begin, capture_end);
    telemetry_value.changed_sources = sparse_contributions.changed_sources().size();

    result = prepare_incremental_graph(abi, operation, diagnostics);
    if (!result.ok())
        return result;

    const auto publish_prepare_begin = clock_type::now();
    result = sparse_contributions.prepare_publish();
    if (!result.ok()) {
        if (result.code != status_code::rebuild_required)
            emit_failure(result, {}, operation, "SourceContribution incremental prepare failed", diagnostics);
        return result;
    }
    const auto publish_prepare_end = clock_type::now();
    telemetry_value.prepare_publish_ns = elapsed_ns(publish_prepare_begin, publish_prepare_end);
    telemetry_value.total_prepare_ns = elapsed_ns(total_begin, publish_prepare_end);

    const auto& statistics = sparse_contributions.prepared_statistics;
    telemetry_value.sources = statistics.sources;
    telemetry_value.type_declarations = statistics.type_declarations;
    telemetry_value.members = statistics.members;
    telemetry_value.enum_values = statistics.enum_values;
    telemetry_value.objects = statistics.objects;
    telemetry_value.links = statistics.links;
    telemetry_value.unique_types = prepared_update.live_type_count;
    telemetry_value.canonical_type_refs =
        target.canonical_types.size() + prepared_update.canonical_types.size() - 1;

    prepared = true;
    return {};
}

status generation_builder::prepare_incremental_graph(
    const abi_configuration& abi,
    operation_id operation,
    diagnostic_buffer& diagnostics) noexcept {

    status result;
    const auto identity_begin = clock_type::now();

    try {
        if (target.types.size() != target.identities.size() ||
            target.types.size() + 1 != target.named_refs.size() ||
            target.types.size() != target.dependency_versions.size() ||
            target.types.size() != target.reverse_dependency_heads.size()) {
            result = {status_code::invalid_argument};
            emit_failure(result, {}, operation, "incremental Graph/cache lineage mismatch", diagnostics);
            return result;
        }

        prepared_update = {};
        prepared_update.live_type_count = target.live_type_count;
        prepared_update.live_object_count = target.live_object_count;
        prepared_update.live_link_count = target.live_link_count;
        prepared_update.intrinsic_refs = target.intrinsic_refs;

        const auto changed = sparse_contributions.changed_sources();
        std::size_t touched_upper = 0;
        std::size_t touched_object_upper = 0;
        std::size_t touched_link_upper = 0;
        for (const auto source : changed) {
            if (!checked_add_size(touched_upper, sparse_contributions.previous_types(source).size()) ||
                !checked_add_size(touched_upper, sparse_contributions.replacement_types(source).size()) ||
                !checked_add_size(touched_object_upper, sparse_contributions.previous_objects(source).size()) ||
                !checked_add_size(touched_object_upper, sparse_contributions.replacement_objects(source).size()) ||
                !checked_add_size(touched_link_upper, sparse_contributions.previous_links(source).size()) ||
                !checked_add_size(touched_link_upper, sparse_contributions.replacement_links(source).size())) {
                return {status_code::not_available};
            }
        }

        prepared_update.type_patches.reserve(touched_upper);
        prepared_update.new_types.reserve(touched_upper);
        prepared_update.new_identities.reserve(touched_upper);
        prepared_update.named_ref_patches.reserve(touched_upper + touched_object_upper);
        prepared_update.dependency_version_patches.reserve(touched_upper);
        prepared_update.object_patches.reserve(touched_object_upper);
        prepared_update.new_objects.reserve(touched_object_upper);
        prepared_update.new_object_identities.reserve(touched_object_upper);
        prepared_update.link_patches.reserve(touched_link_upper);
        prepared_update.new_links.reserve(touched_link_upper);

        sparse_u32_map type_patch_map;
        result = type_patch_map.reserve(touched_upper + 1);
        if (!result.ok())
            return result;
        sparse_handle_set touched_set;
        result = touched_set.reserve(touched_upper + 1);
        if (!result.ok())
            return result;
        std::vector<std::uint32_t> touched_handles;
        touched_handles.reserve(touched_upper);
        sparse_identity_map new_identity_map;
        result = new_identity_map.reserve(touched_upper + touched_object_upper + 1);
        if (!result.ok())
            return result;
        sparse_u32_map object_patch_map;
        result = object_patch_map.reserve(touched_object_upper + 1);
        if (!result.ok())
            return result;
        sparse_u32_map link_patch_map;
        result = link_patch_map.reserve(touched_link_upper + 1);
        if (!result.ok())
            return result;
        sparse_identity_map new_object_identity_map;
        result = new_object_identity_map.reserve(touched_object_upper + 1);
        if (!result.ok())
            return result;
        sparse_handle_set retired_objects;
        result = retired_objects.reserve(touched_object_upper + 1);
        if (!result.ok())
            return result;
        sparse_handle_set claimed_objects;
        result = claimed_objects.reserve(touched_object_upper + 1);
        if (!result.ok())
            return result;
        sparse_handle_set retired_links;
        result = retired_links.reserve(touched_link_upper + 1);
        if (!result.ok())
            return result;
        sparse_handle_set claimed_links;
        result = claimed_links.reserve(touched_link_upper + 1);
        if (!result.ok())
            return result;
        sparse_endpoint_set claimed_link_targets;
        result = claimed_link_targets.reserve(touched_link_upper + 1);
        if (!result.ok())
            return result;

        const auto touch_handle = [&](std::uint32_t handle) -> status {
            bool inserted = false;
            auto local = touched_set.insert(handle, inserted);
            if (!local.ok())
                return local;
            if (inserted)
                touched_handles.push_back(handle);
            return {};
        };

        const auto find_or_create_handle = [&](identity_ref identity, std::uint32_t& output) -> status {
            output = 0;
            if (identity == nullptr)
                return {status_code::invalid_argument};
            const auto existing = target.find_identity(identity);
            if (existing) {
                output = existing.value();
                return {};
            }
            output = new_identity_map.find(identity);
            if (output != 0)
                return {};
            if (target.types.size() + prepared_update.new_types.size() >=
                (std::numeric_limits<std::uint32_t>::max)()) {
                return {status_code::not_available};
            }
            output = static_cast<std::uint32_t>(target.types.size() + prepared_update.new_types.size() + 1);
            prepared_update.new_types.push_back({});
            prepared_update.new_identities.push_back(identity);
            return new_identity_map.insert(identity, output);
        };

        const auto get_mutable_entry = [&](std::uint32_t handle) -> type_entry* {
            if (handle == 0)
                return nullptr;
            if (handle > target.types.size()) {
                const auto local = static_cast<std::size_t>(handle) - target.types.size() - 1;
                return local < prepared_update.new_types.size() ? &prepared_update.new_types[local] : nullptr;
            }
            const auto existing = type_patch_map.find(handle);
            if (existing != 0)
                return &prepared_update.type_patches[existing - 1].value;
            prepared_graph_update::type_patch patch;
            patch.handle = handle;
            patch.value = target.types[handle - 1];
            prepared_update.type_patches.push_back(patch);
            const auto position = static_cast<std::uint32_t>(prepared_update.type_patches.size());
            if (!type_patch_map.insert(handle, position).ok())
                return nullptr;
            return &prepared_update.type_patches[position - 1].value;
        };

        const auto find_or_create_object_handle = [&](identity_ref identity, std::uint32_t& output) -> status {
            output = 0;
            if (!identity || identity.kind() != identity_kind::object)
                return {status_code::invalid_argument};
            const auto existing = target.find_object_identity(identity);
            if (existing) {
                output = existing.value();
                return {};
            }
            output = new_object_identity_map.find(identity);
            if (output != 0)
                return {};
            if (target.object_entries.size() + prepared_update.new_objects.size() >=
                (std::numeric_limits<std::uint32_t>::max)()) {
                return {status_code::not_available};
            }
            output = static_cast<std::uint32_t>(
                target.object_entries.size() + prepared_update.new_objects.size() + 1);
            prepared_update.new_objects.push_back({});
            prepared_update.new_object_identities.push_back(identity);
            return new_object_identity_map.insert(identity, output);
        };

        const auto get_mutable_object = [&](std::uint32_t handle) -> object_entry* {
            if (handle == 0)
                return nullptr;
            if (handle > target.object_entries.size()) {
                const auto local = static_cast<std::size_t>(handle) - target.object_entries.size() - 1;
                return local < prepared_update.new_objects.size() ? &prepared_update.new_objects[local] : nullptr;
            }
            const auto existing = object_patch_map.find(handle);
            if (existing != 0)
                return &prepared_update.object_patches[existing - 1].value;
            prepared_graph_update::object_patch patch;
            patch.handle = handle;
            patch.value = target.object_entries[handle - 1];
            prepared_update.object_patches.push_back(patch);
            const auto position = static_cast<std::uint32_t>(prepared_update.object_patches.size());
            if (!object_patch_map.insert(handle, position).ok())
                return nullptr;
            return &prepared_update.object_patches[position - 1].value;
        };

        const auto get_mutable_link = [&](std::uint32_t handle) -> link_record* {
            if (handle == 0 || handle > target.link_records.size())
                return nullptr;
            const auto existing = link_patch_map.find(handle);
            if (existing != 0)
                return &prepared_update.link_patches[existing - 1].value;
            prepared_graph_update::link_patch patch;
            patch.handle = handle;
            patch.value = target.link_records[handle - 1];
            prepared_update.link_patches.push_back(patch);
            const auto position = static_cast<std::uint32_t>(prepared_update.link_patches.size());
            if (!link_patch_map.insert(handle, position).ok())
                return nullptr;
            return &prepared_update.link_patches[position - 1].value;
        };

        for (const auto source : changed) {
            const auto* old_state = sparse_contributions.previous_state(source);
            const auto old_types = sparse_contributions.previous_types(source);
            for (std::size_t index = 0; index < old_types.size(); ++index) {
                const auto& contribution = old_types[index];
                const auto handle = target.find_identity(contribution.identity);
                if (!handle || old_state == nullptr) {
                    result = {status_code::invalid_argument};
                    emit_failure(result, source, operation, "previous SourceContribution identity absent from Graph", diagnostics);
                    return result;
                }
                auto state = sparse_contributions.construction(handle);
                if (state == nullptr) {
                    result = {status_code::invalid_argument};
                    emit_failure(result, source, operation, "previous construction state absent", diagnostics);
                    return result;
                }
                auto next = *state;
                result = remove_construction(
                    next, contribution,
                    static_cast<std::uint32_t>(old_state->types.begin + index + 1));
                if (!result.ok()) {
                    emit_failure(result, source, operation, "SourceContribution subtraction failed", diagnostics);
                    return result;
                }
                result = sparse_contributions.set_construction(handle, next);
                if (!result.ok())
                    return result;
                result = touch_handle(handle.value());
                if (!result.ok())
                    return result;
            }

            const auto* new_state = sparse_contributions.replacement_state(source);
            const auto new_types = sparse_contributions.replacement_types(source);
            for (std::size_t index = 0; index < new_types.size(); ++index) {
                const auto& contribution = new_types[index];
                std::uint32_t handle_value = 0;
                result = find_or_create_handle(contribution.identity, handle_value);
                if (!result.ok()) {
                    emit_failure(result, source, operation, "incremental identity-to-handle mapping failed", diagnostics);
                    return result;
                }
                const type_handle handle{handle_value};
                source_construction_state next{};
                if (const auto* current = sparse_contributions.construction(handle))
                    next = *current;
                if (new_state == nullptr) {
                    result = {status_code::invalid_argument};
                    emit_failure(result, source, operation, "replacement contribution state absent", diagnostics);
                    return result;
                }
                result = add_construction(
                    next, contribution,
                    static_cast<std::uint32_t>(new_state->types.begin + index + 1));
                if (!result.ok()) {
                    emit_failure(result, source, operation, "SourceContribution addition failed", diagnostics);
                    return result;
                }
                result = sparse_contributions.set_construction(handle, next);
                if (!result.ok())
                    return result;
                result = touch_handle(handle_value);
                if (!result.ok())
                    return result;
            }
        }

        const auto identity_end = clock_type::now();
        telemetry_value.identity_to_handle_ns = elapsed_ns(identity_begin, identity_end);
        telemetry_value.changed_types = touched_handles.size();

        std::size_t total_new_modifiers = 0;
        std::size_t total_new_members = 0;
        for (const auto handle_value : touched_handles) {
            const auto* construction = sparse_contributions.construction(type_handle{handle_value});
            if (construction == nullptr || construction->definitions == 0)
                continue;
            const auto* definition = sparse_contributions.type(construction->definition_type - 1);
            if (definition == nullptr)
                return {status_code::invalid_argument};
            if (definition->kind != source_contribution_type_kind::record)
                continue;
            const auto members = sparse_contributions.members(definition->definition_items);
            if (!checked_add_size(total_new_members, members.size()))
                return {status_code::not_available};
            for (const auto& member : members) {
                if (!checked_add_size(total_new_modifiers, member.type.modifiers.count))
                    return {status_code::not_available};
            }
        }

        for (const auto source : changed) {
            for (const auto& object : sparse_contributions.replacement_objects(source)) {
                if (!checked_add_size(total_new_modifiers, object.type.modifiers.count))
                    return {status_code::not_available};
            }
        }

        prepared_update.members.reserve(total_new_members);
        prepared_update.canonical_types.reserve(total_new_members + total_new_modifiers);
        prepared_update.dependency_edges.reserve(total_new_members);

        sparse_u32_map named_patch_map;
        result = named_patch_map.reserve(touched_upper + total_new_members + touched_object_upper + 1);
        if (!result.ok())
            return result;

        std::vector<graph_derived_index_slot> local_derived_index;
        std::size_t local_derived_entries = 0;
        if (total_new_modifiers != 0) {
            std::size_t capacity = 0;
            if (!checked_index_capacity(total_new_modifiers, capacity))
                return {status_code::not_available};
            local_derived_index.assign(capacity, {});
        }

        const auto canonical_record = [&](TypeRef ref) -> const graph_canonical_type_record* {
            if (!ref)
                return nullptr;
            const auto value = static_cast<std::size_t>(ref.value());
            if (value < target.canonical_types.size())
                return &target.canonical_types[value];
            const auto local = value - target.canonical_types.size();
            return local < prepared_update.canonical_types.size() ? &prepared_update.canonical_types[local] : nullptr;
        };

        const auto append_canonical = [&](graph_canonical_type_record record, TypeRef& output) -> status {
            const auto absolute = target.canonical_types.size() + prepared_update.canonical_types.size();
            if (absolute >= (std::numeric_limits<std::uint32_t>::max)())
                return {status_code::not_available};
            prepared_update.canonical_types.push_back(record);
            output = TypeRef{static_cast<std::uint32_t>(absolute)};
            return {};
        };

        const auto get_intrinsic = [&](intrinsic_type value, TypeRef& output) -> status {
            const auto index = static_cast<std::size_t>(value);
            if (value == intrinsic_type::none || index >= prepared_update.intrinsic_refs.size())
                return {status_code::invalid_argument};
            if (prepared_update.intrinsic_refs[index]) {
                output = prepared_update.intrinsic_refs[index];
                return {};
            }
            graph_canonical_type_record record;
            record.kind = canonical_type_kind::intrinsic;
            record.detail = static_cast<std::uint8_t>(value);
            auto local = append_canonical(record, output);
            if (local.ok())
                prepared_update.intrinsic_refs[index] = output;
            return local;
        };

        const auto get_named = [&](std::uint32_t handle, TypeRef& output) -> status {
            if (handle == 0 || handle > target.types.size() + prepared_update.new_types.size())
                return {status_code::invalid_argument};
            if (handle < target.named_refs.size() && target.named_refs[handle]) {
                output = target.named_refs[handle];
                return {};
            }
            const auto pending = named_patch_map.find(handle);
            if (pending != 0) {
                output = TypeRef{pending};
                return {};
            }
            graph_canonical_type_record record;
            record.kind = canonical_type_kind::named;
            record.child_or_handle = handle;
            auto local = append_canonical(record, output);
            if (!local.ok())
                return local;
            prepared_update.named_ref_patches.push_back({handle, output});
            return named_patch_map.insert(handle, output.value());
        };

        std::uint64_t new_derived_count = 0;
        const auto get_derived = [&](derived_type_kind kind, TypeRef child, std::uint64_t payload, TypeRef& output) -> status {
            if (!child)
                return {status_code::invalid_argument};
            const auto hash = derived_hash(kind, child.value(), payload);
            const auto fingerprint = fold32(hash);

            if (!target.derived_index.empty()) {
                const auto mask = target.derived_index.size() - 1;
                auto position = static_cast<std::size_t>(hash) & mask;
                for (std::size_t probe = 0; probe < target.derived_index.size(); ++probe) {
                    const auto& slot = target.derived_index[position];
                    if (slot.type_ref == 0)
                        break;
                    if (slot.fingerprint == fingerprint) {
                        const auto* record = canonical_record(TypeRef{slot.type_ref});
                        if (record != nullptr && record->kind == canonical_type_kind::derived &&
                            record->detail == static_cast<std::uint8_t>(kind) &&
                            record->child_or_handle == child.value() && record->payload == payload) {
                            output = TypeRef{slot.type_ref};
                            return {};
                        }
                    }
                    position = (position + 1) & mask;
                }
            }

            if (!local_derived_index.empty()) {
                const auto mask = local_derived_index.size() - 1;
                auto position = static_cast<std::size_t>(hash) & mask;
                for (std::size_t probe = 0; probe < local_derived_index.size(); ++probe) {
                    auto& slot = local_derived_index[position];
                    if (slot.type_ref == 0) {
                        graph_canonical_type_record record;
                        record.kind = canonical_type_kind::derived;
                        record.detail = static_cast<std::uint8_t>(kind);
                        record.child_or_handle = child.value();
                        record.payload = payload;
                        auto local = append_canonical(record, output);
                        if (!local.ok())
                            return local;
                        slot.fingerprint = fingerprint;
                        slot.type_ref = output.value();
                        ++local_derived_entries;
                        ++new_derived_count;
                        return {};
                    }
                    if (slot.fingerprint == fingerprint) {
                        const auto* record = canonical_record(TypeRef{slot.type_ref});
                        if (record != nullptr && record->kind == canonical_type_kind::derived &&
                            record->detail == static_cast<std::uint8_t>(kind) &&
                            record->child_or_handle == child.value() && record->payload == payload) {
                            output = TypeRef{slot.type_ref};
                            return {};
                        }
                    }
                    position = (position + 1) & mask;
                }
            }
            return {status_code::not_available};
        };

        const auto candidate_entry = [&](std::uint32_t handle) -> const type_entry* {
            if (handle == 0)
                return nullptr;
            if (handle > target.types.size()) {
                const auto local = static_cast<std::size_t>(handle) - target.types.size() - 1;
                return local < prepared_update.new_types.size() ? &prepared_update.new_types[local] : nullptr;
            }
            const auto position = type_patch_map.find(handle);
            if (position != 0)
                return &prepared_update.type_patches[position - 1].value;
            return &target.types[handle - 1];
        };

        const auto type_ref_begin = clock_type::now();
        const auto definition_begin = type_ref_begin;
        for (const auto handle_value : touched_handles) {
            const type_handle handle{handle_value};
            const auto* construction = sparse_contributions.construction(handle);
            if (construction == nullptr)
                return {status_code::invalid_argument};

            auto* entry = get_mutable_entry(handle_value);
            if (entry == nullptr)
                return {status_code::initialization_failed};
            const bool old_live = handle_value <= target.types.size() && target.types[handle_value - 1].live();
            const bool new_live = construction->declarations != 0;

            if (!new_live) {
                *entry = {};
                if (old_live) {
                    --prepared_update.live_type_count;
                    ++telemetry_value.removed_types;
                }
            } else {
                type_entry next{};
                next.flags = 0x80u;
                if (construction->kind == source_contribution_type_kind::record) {
                    next.kind = graph_type_kind::record;
                    if (construction->record_union != 0) {
                        next.record_kind = source_record_kind::union_type;
                    } else if (old_live && target.types[handle_value - 1].kind == graph_type_kind::record &&
                        target.types[handle_value - 1].record_kind != source_record_kind::union_type) {
                        next.record_kind = target.types[handle_value - 1].record_kind;
                    } else if (construction->record_class != 0 && construction->record_struct == 0) {
                        next.record_kind = source_record_kind::class_type;
                    } else {
                        next.record_kind = source_record_kind::struct_type;
                    }
                } else {
                    next.kind = graph_type_kind::enumeration;
                    if (construction->enum_scoped != 0)
                        next.flags = static_cast<std::uint8_t>(next.flags | 0x01u);
                    if (construction->enum_fixed != 0) {
                        next.flags = static_cast<std::uint8_t>(next.flags | 0x02u);
                        next.enum_underlying = construction->fixed_underlying;
                    } else if (construction->enum_scoped != 0) {
                        next.enum_underlying = intrinsic_type::signed_int;
                    }
                }

                if (!old_live) {
                    ++prepared_update.live_type_count;
                    ++telemetry_value.added_types;
                }

                if (construction->definitions != 0) {
                    const auto* definition = sparse_contributions.type(construction->definition_type - 1);
                    if (definition == nullptr || definition->identity !=
                        (handle_value <= target.identities.size() ? target.identities[handle_value - 1] :
                         prepared_update.new_identities[handle_value - target.types.size() - 1])) {
                        result = {status_code::invalid_argument};
                        emit_failure(result, {}, operation, "active definition contribution invalid", diagnostics);
                        return result;
                    }

                    if (definition->kind == source_contribution_type_kind::record) {
                        const auto members = sparse_contributions.members(definition->definition_items);
                        if (target.member_records.size() + prepared_update.members.size() >=
                            (std::numeric_limits<std::uint32_t>::max)() ||
                            members.size() > (std::numeric_limits<std::uint32_t>::max)()) {
                            return {status_code::not_available};
                        }
                        next.definition.begin = static_cast<std::uint32_t>(
                            target.member_records.size() + prepared_update.members.size() + 1);
                        next.definition.count = static_cast<std::uint32_t>(members.size());

                        for (const auto& member : members) {
                            TypeRef current;
                            if (member.type.identity != nullptr) {
                                std::uint32_t base_handle = 0;
                                result = find_or_create_handle(member.type.identity, base_handle);
                                if (!result.ok())
                                    return result;
                                result = get_named(base_handle, current);
                            } else {
                                result = get_intrinsic(member.type.intrinsic, current);
                            }
                            if (!result.ok())
                                return result;

                            const auto modifiers = sparse_contributions.modifiers(member.type.modifiers);
                            for (const auto& modifier : modifiers) {
                                TypeRef wrapped;
                                result = get_derived(derived_kind(modifier.kind), current, modifier.value, wrapped);
                                if (!result.ok())
                                    return result;
                                current = wrapped;
                            }

                            prepared_update.members.push_back(member_record{
                                member.name, current, member.access, {}, member.construction});

                            if (member.type.identity != nullptr) {
                                std::uint32_t target_handle_value = 0;
                                result = find_or_create_handle(member.type.identity, target_handle_value);
                                if (!result.ok())
                                    return result;
                                prepared_graph_update::pending_dependency_edge edge;
                                edge.target_handle = target_handle_value;
                                edge.owner_handle = handle_value;
                                prepared_update.dependency_edges.push_back(edge);
                            }
                        }
                    } else {
                        const auto values = sparse_contributions.enum_values(definition->definition_items);
                        if (target.enum_value_records.size() + prepared_update.enum_values.size() >=
                            (std::numeric_limits<std::uint32_t>::max)() ||
                            values.size() > (std::numeric_limits<std::uint32_t>::max)()) {
                            return {status_code::not_available};
                        }
                        if (!next.enum_fixed_underlying() && !next.enum_scoped()) {
                            result = select_enum_underlying(values, abi, next.enum_underlying);
                            if (!result.ok())
                                return result;
                        }
                        next.definition.begin = static_cast<std::uint32_t>(
                            target.enum_value_records.size() + prepared_update.enum_values.size() + 1);
                        next.definition.count = static_cast<std::uint32_t>(values.size());
                        for (const auto& value : values) {
                            prepared_update.enum_values.push_back(enum_value_record{
                                value.value.bits, value.name, value.value.intrinsic});
                        }
                    }
                }
                *entry = next;
            }

            std::uint32_t version = 1;
            if (handle_value <= target.dependency_versions.size()) {
                if (target.dependency_versions[handle_value - 1] ==
                    (std::numeric_limits<std::uint32_t>::max)()) {
                    return {status_code::not_available};
                }
                version = target.dependency_versions[handle_value - 1] + 1;
            }
            prepared_update.dependency_version_patches.push_back({handle_value, version});
            for (auto& edge : prepared_update.dependency_edges) {
                if (edge.owner_handle == handle_value && edge.owner_version == 0)
                    edge.owner_version = version;
            }
        }
        for (const auto source : changed) {
            for (const auto& old_object : sparse_contributions.previous_objects(source)) {
                const auto handle = target.find_object_identity(old_object.identity);
                if (!handle) {
                    result = {status_code::invalid_argument};
                    emit_failure(result, source, operation, "previous object identity absent from Graph", diagnostics);
                    return result;
                }
                auto* entry = get_mutable_object(handle.value());
                if (entry == nullptr)
                    return {status_code::initialization_failed};
                if (entry->live()) {
                    *entry = {};
                    --prepared_update.live_object_count;
                }
                bool inserted = false;
                result = retired_objects.insert(handle.value(), inserted);
                if (!result.ok())
                    return result;
            }

            for (const auto& old_link : sparse_contributions.previous_links(source)) {
                const auto source_object = target.find_object_identity(old_link.source.object);
                const auto target_object = target.find_object_identity(old_link.target.object);
                if (!source_object || !target_object) {
                    result = {status_code::invalid_argument};
                    emit_failure(result, source, operation, "previous link object identity absent from Graph", diagnostics);
                    return result;
                }
                const object_endpoint old_target{target_object, old_link.target.member};
                const auto handle = target.find_link_raw(old_target);
                if (!handle) {
                    result = {status_code::invalid_argument};
                    emit_failure(result, source, operation, "previous link absent from Graph", diagnostics);
                    return result;
                }
                auto* link = get_mutable_link(handle.value());
                if (link == nullptr)
                    return {status_code::initialization_failed};
                if (link->live()) {
                    link->source = {};
                    --prepared_update.live_link_count;
                }
                bool inserted = false;
                result = retired_links.insert(handle.value(), inserted);
                if (!result.ok())
                    return result;
            }
        }

        const auto candidate_object_entry = [&](std::uint32_t handle) -> const object_entry* {
            if (handle == 0)
                return nullptr;
            if (handle > target.object_entries.size()) {
                const auto local = static_cast<std::size_t>(handle) - target.object_entries.size() - 1;
                return local < prepared_update.new_objects.size() ? &prepared_update.new_objects[local] : nullptr;
            }
            const auto position = object_patch_map.find(handle);
            if (position != 0)
                return &prepared_update.object_patches[position - 1].value;
            return &target.object_entries[handle - 1];
        };

        const auto materialize_object_type = [&](
            const source_contribution_object& object, TypeRef& output) -> status {
            TypeRef current;
            status local;
            if (object.type.identity != nullptr) {
                std::uint32_t base_handle = 0;
                local = find_or_create_handle(object.type.identity, base_handle);
                if (local.ok())
                    local = get_named(base_handle, current);
            } else {
                local = get_intrinsic(object.type.intrinsic, current);
            }
            if (!local.ok())
                return local;
            for (const auto& modifier : sparse_contributions.modifiers(object.type.modifiers)) {
                TypeRef wrapped;
                local = get_derived(derived_kind(modifier.kind), current, modifier.value, wrapped);
                if (!local.ok())
                    return local;
                current = wrapped;
            }
            output = current;
            return {};
        };

        for (const auto source : changed) {
            for (const auto& object : sparse_contributions.replacement_objects(source)) {
                std::uint32_t handle_value = 0;
                result = find_or_create_object_handle(object.identity, handle_value);
                if (!result.ok())
                    return result;

                bool claimed = false;
                result = claimed_objects.insert(handle_value, claimed);
                if (!result.ok())
                    return result;
                if (!claimed) {
                    result = {status_code::semantic_conflict};
                    emit_failure(result, source, operation, "multiple Project object declarations", diagnostics);
                    return result;
                }

                auto* entry = get_mutable_object(handle_value);
                if (entry == nullptr)
                    return {status_code::initialization_failed};
                const bool historical_live =
                    handle_value <= target.object_entries.size() && target.object_entries[handle_value - 1].live();
                const bool was_retired = retired_objects.contains(handle_value);
                if (entry->live() && (!historical_live || !was_retired)) {
                    result = {status_code::semantic_conflict};
                    emit_failure(result, source, operation, "multiple Project object declarations", diagnostics);
                    return result;
                }

                TypeRef object_type;
                result = materialize_object_type(object, object_type);
                if (!result.ok()) {
                    emit_failure(result, source, operation, "object TypeRef materialization failed", diagnostics);
                    return result;
                }
                const bool was_live = entry->live();
                *entry = object_entry{object_type, 0x80000000u | object.construction_flags};
                if (!was_live)
                    ++prepared_update.live_object_count;
            }
        }

        const auto validate_object_endpoint = [&](object_endpoint endpoint) noexcept -> bool {
            const auto* object = candidate_object_entry(endpoint.object.value());
            if (object == nullptr || !object->live() || !endpoint.member || !object->type)
                return false;
            const auto* type_ref = canonical_record(object->type);
            if (type_ref == nullptr || type_ref->kind != canonical_type_kind::named)
                return false;
            const auto* type = candidate_entry(type_ref->child_or_handle);
            return type != nullptr && type->live() && type->kind == graph_type_kind::record &&
                type->defined() && endpoint.member.value() < type->definition.count;
        };

        for (const auto source : changed) {
            for (const auto& link : sparse_contributions.replacement_links(source)) {
                std::uint32_t source_object_value = 0;
                std::uint32_t target_object_value = 0;
                result = find_or_create_object_handle(link.source.object, source_object_value);
                if (!result.ok())
                    return result;
                result = find_or_create_object_handle(link.target.object, target_object_value);
                if (!result.ok())
                    return result;
                const object_endpoint source_endpoint{object_handle{source_object_value}, link.source.member};
                const object_endpoint target_endpoint{object_handle{target_object_value}, link.target.member};
                if (!validate_object_endpoint(source_endpoint) || !validate_object_endpoint(target_endpoint)) {
                    result = {status_code::semantic_conflict};
                    emit_failure(result, source, operation, "link endpoint is not a live record member", diagnostics);
                    return result;
                }

                bool target_inserted = false;
                result = claimed_link_targets.insert(target_endpoint, target_inserted);
                if (!result.ok())
                    return result;
                if (!target_inserted) {
                    result = {status_code::semantic_conflict};
                    emit_failure(result, source, operation, "duplicate link target in replacement set", diagnostics);
                    return result;
                }

                const auto historical = target.find_link_raw(target_endpoint);
                if (historical) {
                    const bool was_retired = retired_links.contains(historical.value());
                    const bool historical_live = target.link_records[historical.value() - 1].live();
                    if (historical_live && !was_retired) {
                        result = {status_code::semantic_conflict};
                        emit_failure(result, source, operation, "multiple links target the same endpoint", diagnostics);
                        return result;
                    }
                    bool inserted = false;
                    result = claimed_links.insert(historical.value(), inserted);
                    if (!result.ok())
                        return result;
                    if (!inserted) {
                        result = {status_code::semantic_conflict};
                        emit_failure(result, source, operation, "duplicate link target in replacement set", diagnostics);
                        return result;
                    }
                    auto* value = get_mutable_link(historical.value());
                    if (value == nullptr)
                        return {status_code::initialization_failed};
                    const bool was_live = value->live();
                    *value = {source_endpoint, target_endpoint};
                    if (!was_live)
                        ++prepared_update.live_link_count;
                } else {
                    if (target.link_records.size() + prepared_update.new_links.size() >=
                        (std::numeric_limits<std::uint32_t>::max)())
                        return {status_code::not_available};
                    prepared_update.new_links.push_back({source_endpoint, target_endpoint});
                    ++prepared_update.live_link_count;
                }
            }
        }

        const auto definition_end = clock_type::now();
        telemetry_value.type_ref_materialization_ns = elapsed_ns(type_ref_begin, definition_end);
        telemetry_value.definition_materialization_ns = elapsed_ns(definition_begin, definition_end);
        telemetry_value.derived_type_refs = new_derived_count;

        const auto validation_begin = clock_type::now();
        sparse_handle_set closure_set;
        result = closure_set.reserve(touched_handles.size() + 1);
        if (!result.ok())
            return result;
        std::vector<std::uint32_t> closure;
        closure.reserve(touched_handles.size());
        for (const auto handle : touched_handles) {
            bool inserted = false;
            result = closure_set.insert(handle, inserted);
            if (!result.ok())
                return result;
            if (inserted)
                closure.push_back(handle);
        }

        for (std::size_t cursor = 0; cursor < closure.size(); ++cursor) {
            const auto changed_handle = closure[cursor];
            if (changed_handle > target.reverse_dependency_heads.size())
                continue;
            auto edge_index = target.reverse_dependency_heads[changed_handle - 1];
            while (edge_index != 0) {
                if (edge_index > target.dependency_edges.size())
                    return {status_code::invalid_argument};
                const auto& edge = target.dependency_edges[edge_index - 1];
                ++telemetry_value.validation_dependency_edges;
                if (edge.owner_handle == 0 || edge.owner_handle > target.dependency_versions.size())
                    return {status_code::invalid_argument};
                if (edge.owner_version == target.dependency_versions[edge.owner_handle - 1]) {
                    bool inserted = false;
                    result = closure_set.insert(edge.owner_handle, inserted);
                    if (!result.ok())
                        return result;
                    if (inserted)
                        closure.push_back(edge.owner_handle);
                }
                edge_index = edge.next_for_target;
            }
        }

        const auto get_member = [&](std::size_t absolute) -> const member_record* {
            if (absolute < target.member_records.size())
                return &target.member_records[absolute];
            const auto local = absolute - target.member_records.size();
            return local < prepared_update.members.size() ? &prepared_update.members[local] : nullptr;
        };

        for (const auto handle_value : closure) {
            ++telemetry_value.validation_visited_types;
            const auto* entry = candidate_entry(handle_value);
            if (entry == nullptr || !entry->live())
                continue;
            if (entry->kind != graph_type_kind::record || !entry->definition)
                continue;

            const auto begin = static_cast<std::size_t>(entry->definition.begin - 1);
            const auto count = static_cast<std::size_t>(entry->definition.count);
            for (std::size_t member_index = 0; member_index < count; ++member_index) {
                const auto* member = get_member(begin + member_index);
                if (member == nullptr)
                    return {status_code::invalid_argument};
                TypeRef current = member->type;
                for (;;) {
                    ++telemetry_value.validation_visited_type_refs;
                    const auto* record = canonical_record(current);
                    if (record == nullptr)
                        return {status_code::invalid_argument};
                    if (record->kind == canonical_type_kind::intrinsic)
                        break;
                    if (record->kind == canonical_type_kind::named) {
                        const auto* named_entry = candidate_entry(record->child_or_handle);
                        if (named_entry == nullptr || !named_entry->live()) {
                            result = {status_code::semantic_conflict};
                            emit_failure(result, {}, operation, "member TypeRef targets removed type", diagnostics);
                            return result;
                        }
                        break;
                    }
                    if (record->child_or_handle == 0) {
                        return {status_code::invalid_argument};
                    }
                    current = TypeRef{record->child_or_handle};
                }
            }
        }
        const auto validation_end = clock_type::now();
        telemetry_value.validation_ns = elapsed_ns(validation_begin, validation_end);

        for (const auto handle_value : touched_handles) {
            const auto* entry = candidate_entry(handle_value);
            if (entry == nullptr)
                return {status_code::invalid_argument};
        }

        const auto total_identity_count = target.identities.size() + prepared_update.new_identities.size();
        if (!prepared_update.new_identities.empty() &&
            (target.identity_index.empty() ||
             total_identity_count > target.identity_index.size() - target.identity_index.size() / 4)) {
            return {status_code::rebuild_required};
        }

        const auto total_object_count = target.object_identities.size() + prepared_update.new_object_identities.size();
        if (!prepared_update.new_object_identities.empty() &&
            (target.object_identity_index.empty() ||
             total_object_count > target.object_identity_index.size() - target.object_identity_index.size() / 4)) {
            return {status_code::rebuild_required};
        }
        const auto total_link_slots = target.link_records.size() + prepared_update.new_links.size();
        if (!prepared_update.new_links.empty() &&
            (target.link_index.empty() ||
             total_link_slots > target.link_index.size() - target.link_index.size() / 4)) {
            return {status_code::rebuild_required};
        }

        prepared_update.derived_index_entries = target.derived_index_entries + static_cast<std::size_t>(new_derived_count);
        if (new_derived_count != 0 &&
            (target.derived_index.empty() ||
             prepared_update.derived_index_entries >
                 target.derived_index.size() - target.derived_index.size() / 4)) {
            return {status_code::rebuild_required};
        }

        result = target.prepare_sparse_publication(prepared_update);
        if (!result.ok())
            return result;

        return {};
    } catch (const std::bad_alloc&) {
        result = {status_code::initialization_failed};
    } catch (const std::length_error&) {
        result = {status_code::not_available};
    } catch (...) {
        result = {status_code::initialization_failed};
    }

    emit_failure(result, {}, operation, "incremental Generation preparation failed", diagnostics);
    return result;
}

status generation_builder::prepare_graph(
    const abi_configuration& abi,
    operation_id operation,
    diagnostic_buffer& diagnostics) noexcept {

    auto& storage = contributions.candidate;
    if (storage.types.size() >= (std::numeric_limits<std::uint32_t>::max)() ||
        storage.objects.size() >= (std::numeric_limits<std::uint32_t>::max)() ||
        storage.links.size() >= (std::numeric_limits<std::uint32_t>::max)()) {
        const status result{status_code::not_available};
        emit_failure(result, {}, operation, "Too many Graph declarations", diagnostics);
        return result;
    }

    struct identity_slot final {
        identity_ref identity = nullptr;
        std::uint32_t handle = 0;
    };

    class local_identity_index final {
    public:
        [[nodiscard]] status initialize(std::size_t count) noexcept {
            std::size_t capacity = 0;
            if (!checked_index_capacity(count, capacity))
                return {status_code::not_available};
            try {
                slots.assign(capacity, {});
                mask = capacity == 0 ? 0 : capacity - 1;
                return {};
            } catch (const std::bad_alloc&) {
                return {status_code::initialization_failed};
            } catch (const std::length_error&) {
                return {status_code::not_available};
            } catch (...) {
                return {status_code::initialization_failed};
            }
        }

        [[nodiscard]] status insert_or_find(
            identity_ref identity,
            std::uint32_t proposed,
            std::uint32_t& output,
            bool& inserted) noexcept {

            output = 0;
            inserted = false;
            if (identity == nullptr || proposed == 0 || slots.empty())
                return {status_code::invalid_argument};
            auto position = position_for(identity);
            for (std::size_t probe = 0; probe < slots.size(); ++probe) {
                auto& slot = slots[position];
                if (slot.identity == nullptr) {
                    slot.identity = identity;
                    slot.handle = proposed;
                    output = proposed;
                    inserted = true;
                    return {};
                }
                if (slot.identity == identity) {
                    output = slot.handle;
                    return {};
                }
                position = (position + 1) & mask;
            }
            return {status_code::not_available};
        }

        [[nodiscard]] std::uint32_t find(identity_ref identity) const noexcept {
            if (identity == nullptr || slots.empty())
                return 0;
            auto position = position_for(identity);
            for (std::size_t probe = 0; probe < slots.size(); ++probe) {
                const auto& slot = slots[position];
                if (slot.identity == nullptr)
                    return 0;
                if (slot.identity == identity)
                    return slot.handle;
                position = (position + 1) & mask;
            }
            return 0;
        }

    private:
        [[nodiscard]] std::size_t position_for(identity_ref identity) const noexcept {
            return static_cast<std::size_t>(
                mix64(static_cast<std::uint64_t>(identity.value()))) & mask;
        }

        std::vector<identity_slot> slots;
        std::size_t mask = 0;
    };

    local_identity_index identity_index;
    auto result = identity_index.initialize(storage.types.size());
    if (!result.ok()) {
        emit_failure(result, {}, operation, "Graph type identity index allocation failed", diagnostics);
        return result;
    }
    local_identity_index object_identity_index;
    result = object_identity_index.initialize(storage.objects.size());
    if (!result.ok()) {
        emit_failure(result, {}, operation, "Graph object identity index allocation failed", diagnostics);
        return result;
    }

    try {
        prepared_graph = {};
        prepared_graph.types.reserve(storage.types.size());
        prepared_graph.identities.reserve(storage.types.size());
        prepared_graph.members.reserve(storage.members.size());
        prepared_graph.enum_values.reserve(storage.enum_values.size());
        prepared_graph.objects.reserve(storage.objects.size());
        prepared_graph.object_identities.reserve(storage.objects.size());
        prepared_graph.links.reserve(storage.links.size());
        prepared_graph.canonical_types.reserve(
            1 + storage.members.size() + storage.objects.size() + storage.modifiers.size());
        prepared_graph.canonical_types.push_back({});
        storage.construction.resize(storage.types.size() + 1);

        std::vector<std::uint8_t> definition_seen;
        std::vector<std::uint32_t> declaration_handles(storage.types.size(), 0);
        std::vector<std::uint32_t> member_base_handles(storage.members.size(), 0);
        std::vector<TypeRef> member_type_refs(storage.members.size());
        std::vector<std::uint32_t> object_base_handles(storage.objects.size(), 0);
        std::vector<TypeRef> object_type_refs(storage.objects.size());
        definition_seen.reserve(storage.types.size());

        const auto identity_begin = clock_type::now();

        for (std::size_t source_index = 1; source_index < storage.sources.size(); ++source_index) {
            const auto& source_state = storage.sources[source_index];
            if (!source_state.source)
                continue;

            const auto begin = static_cast<std::size_t>(source_state.types.begin);
            const auto count = static_cast<std::size_t>(source_state.types.count);
            if (begin > storage.types.size() || count > storage.types.size() - begin) {
                result = {status_code::invalid_argument};
                emit_failure(result, source_state.source, operation, "SourceContribution type range invalid", diagnostics);
                return result;
            }

            for (std::size_t offset = 0; offset < count; ++offset) {
                const auto global_index = begin + offset;
                const auto& contribution = storage.types[global_index];
                const auto proposed = static_cast<std::uint32_t>(prepared_graph.types.size() + 1);
                std::uint32_t handle_value = 0;
                bool inserted = false;
                result = identity_index.insert_or_find(
                    contribution.identity, proposed, handle_value, inserted);
                if (!result.ok()) {
                    emit_failure(result, source_state.source, operation, "identity_ref materialization failed", diagnostics);
                    return result;
                }
                declaration_handles[global_index] = handle_value;

                result = add_construction(
                    storage.construction[handle_value],
                    contribution,
                    static_cast<std::uint32_t>(global_index + 1));
                if (!result.ok()) {
                    emit_failure(result, source_state.source, operation,
                        "SourceContribution construction aggregation failed", diagnostics);
                    return result;
                }

                if (inserted) {
                    type_entry entry;
                    entry.flags = 0x80u;
                    if (contribution.kind == source_contribution_type_kind::record) {
                        entry.kind = graph_type_kind::record;
                        entry.record_kind = contribution.record_kind;
                    } else {
                        entry.kind = graph_type_kind::enumeration;
                        entry.enum_underlying = contribution.explicit_underlying == intrinsic_type::none ?
                            intrinsic_type::signed_int : contribution.explicit_underlying;
                        if (contribution.enum_scoped())
                            entry.flags = static_cast<std::uint8_t>(entry.flags | 0x01u);
                        if (contribution.explicit_underlying != intrinsic_type::none)
                            entry.flags = static_cast<std::uint8_t>(entry.flags | 0x02u);
                    }
                    prepared_graph.types.push_back(entry);
                    prepared_graph.identities.push_back(contribution.identity);
                    definition_seen.push_back(0);
                    continue;
                }

                auto& entry = prepared_graph.types[handle_value - 1];
                if ((entry.kind == graph_type_kind::record) !=
                    (contribution.kind == source_contribution_type_kind::record)) {
                    result = {status_code::semantic_conflict};
                    emit_failure(result, source_state.source, operation, "record/enum identity kind conflict", diagnostics);
                    return result;
                }

                if (entry.kind == graph_type_kind::record) {
                    if (!compatible_record_kind(entry.record_kind, contribution.record_kind)) {
                        result = {status_code::semantic_conflict};
                        emit_failure(result, source_state.source, operation, "union/non-union redeclaration conflict", diagnostics);
                        return result;
                    }
                    if (contribution.definition())
                        entry.record_kind = contribution.record_kind;
                } else {
                    if (entry.enum_scoped() != contribution.enum_scoped()) {
                        result = {status_code::semantic_conflict};
                        emit_failure(result, source_state.source, operation, "scoped enum redeclaration conflict", diagnostics);
                        return result;
                    }
                    if (contribution.explicit_underlying != intrinsic_type::none) {
                        if (entry.enum_fixed_underlying() &&
                            entry.enum_underlying != contribution.explicit_underlying) {
                            result = {status_code::semantic_conflict};
                            emit_failure(result, source_state.source, operation, "enum underlying type conflict", diagnostics);
                            return result;
                        }
                        entry.enum_underlying = contribution.explicit_underlying;
                        entry.flags = static_cast<std::uint8_t>(entry.flags | 0x02u);
                    }
                }
            }
        }

        storage.construction.resize(prepared_graph.types.size() + 1);
        {
            std::size_t capacity = 0;
            if (!checked_index_capacity(prepared_graph.types.size(), capacity)) {
                result = {status_code::not_available};
                emit_failure(result, {}, operation, "Generation identity index too large", diagnostics);
                return result;
            }
            prepared_graph.identity_index.assign(capacity, {});
            for (std::size_t index = 0; index < prepared_graph.identities.size(); ++index) {
                insert_identity_slot(
                    prepared_graph.identity_index, prepared_graph.identities,
                    prepared_graph.identities[index],
                    static_cast<std::uint32_t>(index + 1));
            }
        }

        for (std::size_t index = 0; index < storage.members.size(); ++index) {
            const auto& member = storage.members[index];
            if (member.type.identity == nullptr)
                continue;
            const auto handle = identity_index.find(member.type.identity);
            if (handle == 0) {
                result = {status_code::semantic_conflict};
                emit_failure(result, {}, operation, "member identity_ref has no Project type declaration", diagnostics);
                return result;
            }
            member_base_handles[index] = handle;
        }


        for (std::size_t index = 0; index < storage.objects.size(); ++index) {
            const auto& object = storage.objects[index];
            const auto proposed = static_cast<std::uint32_t>(prepared_graph.objects.size() + 1);
            std::uint32_t handle_value = 0;
            bool inserted = false;
            result = object_identity_index.insert_or_find(
                object.identity, proposed, handle_value, inserted);
            if (!result.ok()) {
                emit_failure(result, {}, operation, "object identity_ref materialization failed", diagnostics);
                return result;
            }
            if (!inserted) {
                result = {status_code::semantic_conflict};
                emit_failure(result, {}, operation, "multiple Project object declarations", diagnostics);
                return result;
            }
            prepared_graph.objects.push_back({});
            prepared_graph.object_identities.push_back(object.identity);

            if (object.type.identity != nullptr) {
                const auto type_handle_value = identity_index.find(object.type.identity);
                if (type_handle_value == 0) {
                    result = {status_code::semantic_conflict};
                    emit_failure(result, {}, operation, "object identity_ref has no Project type declaration", diagnostics);
                    return result;
                }
                object_base_handles[index] = type_handle_value;
            }
        }

        const auto identity_end = clock_type::now();
        telemetry_value.identity_to_handle_ns = elapsed_ns(identity_begin, identity_end);

        std::array<TypeRef, graph_intrinsic_type_count> intrinsic_refs{};
        std::vector<TypeRef> named_refs(prepared_graph.types.size() + 1);
        std::vector<graph_derived_index_slot> derived_slots;
        std::size_t derived_capacity = 8;
        if (!storage.modifiers.empty() &&
            !checked_index_capacity(storage.modifiers.size(), derived_capacity)) {
            result = {status_code::not_available};
            emit_failure(result, {}, operation, "Derived TypeRef index too large", diagnostics);
            return result;
        }
        derived_slots.assign(derived_capacity, {});
        const auto derived_mask = derived_capacity - 1;

        const auto append_canonical = [&](graph_canonical_type_record record, TypeRef& output) -> status {
            if (prepared_graph.canonical_types.size() >= (std::numeric_limits<std::uint32_t>::max)())
                return {status_code::not_available};
            const auto index = static_cast<std::uint32_t>(prepared_graph.canonical_types.size());
            prepared_graph.canonical_types.push_back(record);
            output = TypeRef{index};
            return {};
        };

        const auto get_intrinsic = [&](intrinsic_type value, TypeRef& output) -> status {
            const auto index = static_cast<std::size_t>(value);
            if (value == intrinsic_type::none || index >= intrinsic_refs.size())
                return {status_code::invalid_argument};
            if (intrinsic_refs[index]) {
                output = intrinsic_refs[index];
                return {};
            }
            graph_canonical_type_record record;
            record.kind = canonical_type_kind::intrinsic;
            record.detail = static_cast<std::uint8_t>(value);
            auto local = append_canonical(record, output);
            if (local.ok())
                intrinsic_refs[index] = output;
            return local;
        };

        const auto get_named = [&](std::uint32_t handle, TypeRef& output) -> status {
            if (handle == 0 || handle >= named_refs.size())
                return {status_code::invalid_argument};
            if (named_refs[handle]) {
                output = named_refs[handle];
                return {};
            }
            graph_canonical_type_record record;
            record.kind = canonical_type_kind::named;
            record.child_or_handle = handle;
            auto local = append_canonical(record, output);
            if (local.ok())
                named_refs[handle] = output;
            return local;
        };

        std::uint64_t derived_count = 0;
        const auto get_derived = [&](derived_type_kind kind, TypeRef child, std::uint64_t payload, TypeRef& output) -> status {
            if (!child || derived_slots.empty())
                return {status_code::invalid_argument};

            const auto hash = mix64(
                (static_cast<std::uint64_t>(static_cast<std::uint8_t>(kind)) << 56) ^
                (static_cast<std::uint64_t>(child.value()) << 16) ^ mix64(payload));
            const auto fingerprint = fold32(hash);
            auto position = static_cast<std::size_t>(hash) & derived_mask;

            for (std::size_t probe = 0; probe < derived_slots.size(); ++probe) {
                auto& slot = derived_slots[position];
                if (slot.type_ref == 0) {
                    graph_canonical_type_record record;
                    record.kind = canonical_type_kind::derived;
                    record.detail = static_cast<std::uint8_t>(kind);
                    record.child_or_handle = child.value();
                    record.payload = payload;
                    auto local = append_canonical(record, output);
                    if (!local.ok())
                        return local;
                    slot.fingerprint = fingerprint;
                    slot.type_ref = output.value();
                    ++derived_count;
                    return {};
                }
                if (slot.fingerprint == fingerprint) {
                    const auto& record = prepared_graph.canonical_types[slot.type_ref];
                    if (record.kind == canonical_type_kind::derived &&
                        record.detail == static_cast<std::uint8_t>(kind) &&
                        record.child_or_handle == child.value() &&
                        record.payload == payload) {
                        output = TypeRef{slot.type_ref};
                        return {};
                    }
                }
                position = (position + 1) & derived_mask;
            }
            return {status_code::not_available};
        };

        const auto type_ref_begin = clock_type::now();
        for (std::size_t index = 0; index < storage.members.size(); ++index) {
            const auto& member = storage.members[index];
            TypeRef current;
            if (member.type.identity != nullptr)
                result = get_named(member_base_handles[index], current);
            else
                result = get_intrinsic(member.type.intrinsic, current);
            if (!result.ok()) {
                emit_failure(result, {}, operation, "member base TypeRef materialization failed", diagnostics);
                return result;
            }

            const auto modifier_begin = static_cast<std::size_t>(member.type.modifiers.begin);
            const auto modifier_count = static_cast<std::size_t>(member.type.modifiers.count);
            if (modifier_begin > storage.modifiers.size() ||
                modifier_count > storage.modifiers.size() - modifier_begin) {
                result = {status_code::invalid_argument};
                emit_failure(result, {}, operation, "member modifier range invalid", diagnostics);
                return result;
            }

            for (std::size_t modifier_index = 0; modifier_index < modifier_count; ++modifier_index) {
                const auto& modifier = storage.modifiers[modifier_begin + modifier_index];
                TypeRef wrapped;
                result = get_derived(derived_kind(modifier.kind), current, modifier.value, wrapped);
                if (!result.ok()) {
                    emit_failure(result, {}, operation, "derived TypeRef materialization failed", diagnostics);
                    return result;
                }
                current = wrapped;
            }
            member_type_refs[index] = current;
        }

        for (std::size_t index = 0; index < storage.objects.size(); ++index) {
            const auto& object = storage.objects[index];
            TypeRef current;
            if (object.type.identity != nullptr)
                result = get_named(object_base_handles[index], current);
            else
                result = get_intrinsic(object.type.intrinsic, current);
            if (!result.ok()) {
                emit_failure(result, {}, operation, "object base TypeRef materialization failed", diagnostics);
                return result;
            }

            const auto modifier_begin = static_cast<std::size_t>(object.type.modifiers.begin);
            const auto modifier_count = static_cast<std::size_t>(object.type.modifiers.count);
            if (modifier_begin > storage.modifiers.size() ||
                modifier_count > storage.modifiers.size() - modifier_begin) {
                result = {status_code::invalid_argument};
                emit_failure(result, {}, operation, "object modifier range invalid", diagnostics);
                return result;
            }
            for (std::size_t modifier_index = 0; modifier_index < modifier_count; ++modifier_index) {
                const auto& modifier = storage.modifiers[modifier_begin + modifier_index];
                TypeRef wrapped;
                result = get_derived(derived_kind(modifier.kind), current, modifier.value, wrapped);
                if (!result.ok()) {
                    emit_failure(result, {}, operation, "object derived TypeRef materialization failed", diagnostics);
                    return result;
                }
                current = wrapped;
            }
            object_type_refs[index] = current;
            prepared_graph.objects[index].type = current;
            prepared_graph.objects[index].flags = 0x80000000u | object.construction_flags;
        }
        const auto type_ref_end = clock_type::now();
        telemetry_value.type_ref_materialization_ns = elapsed_ns(type_ref_begin, type_ref_end);
        telemetry_value.derived_type_refs = derived_count;
        prepared_graph.intrinsic_refs = intrinsic_refs;
        prepared_graph.named_refs = std::move(named_refs);
        prepared_graph.derived_index = std::move(derived_slots);
        prepared_graph.derived_index_entries = static_cast<std::size_t>(derived_count);
        prepared_graph.dependency_versions.assign(prepared_graph.types.size(), 1);
        prepared_graph.reverse_dependency_heads.assign(prepared_graph.types.size(), 0);
        prepared_graph.dependency_edges.reserve(storage.members.size());

        const auto definition_begin = clock_type::now();
        for (std::size_t source_index = 1; source_index < storage.sources.size(); ++source_index) {
            const auto& source_state = storage.sources[source_index];
            if (!source_state.source)
                continue;

            const auto begin = static_cast<std::size_t>(source_state.types.begin);
            const auto count = static_cast<std::size_t>(source_state.types.count);
            for (std::size_t offset = 0; offset < count; ++offset) {
                const auto global_index = begin + offset;
                const auto& contribution = storage.types[global_index];
                if (!contribution.definition())
                    continue;

                const auto handle_value = declaration_handles[global_index];
                auto& seen = definition_seen[handle_value - 1];
                auto& entry = prepared_graph.types[handle_value - 1];
                if (seen != 0) {
                    result = {status_code::semantic_conflict};
                    emit_failure(result, source_state.source, operation, "multiple type definitions", diagnostics);
                    return result;
                }
                seen = 1;

                if (contribution.kind == source_contribution_type_kind::record) {
                    const auto item_begin = static_cast<std::size_t>(contribution.definition_items.begin);
                    const auto item_count = static_cast<std::size_t>(contribution.definition_items.count);
                    if (item_begin > storage.members.size() || item_count > storage.members.size() - item_begin ||
                        prepared_graph.members.size() >= (std::numeric_limits<std::uint32_t>::max)() ||
                        item_count > (std::numeric_limits<std::uint32_t>::max)()) {
                        result = {status_code::not_available};
                        emit_failure(result, source_state.source, operation, "record definition range too large", diagnostics);
                        return result;
                    }
                    entry.definition.begin = static_cast<std::uint32_t>(prepared_graph.members.size() + 1);
                    entry.definition.count = static_cast<std::uint32_t>(item_count);
                    for (std::size_t item = 0; item < item_count; ++item) {
                        const auto member_index = item_begin + item;
                        const auto& contribution_member = storage.members[member_index];
                        member_record materialized;
                        materialized.name = contribution_member.name;
                        materialized.type = member_type_refs[member_index];
                        materialized.access = contribution_member.access;
                        materialized.construction = contribution_member.construction;
                        prepared_graph.members.push_back(materialized);
                        if (contribution_member.type.identity != nullptr) {
                            const auto target_handle = member_base_handles[member_index];
                            graph_dependency_edge edge;
                            edge.owner_handle = handle_value;
                            edge.owner_version = 1;
                            edge.next_for_target =
                                prepared_graph.reverse_dependency_heads[target_handle - 1];
                            prepared_graph.dependency_edges.push_back(edge);
                            prepared_graph.reverse_dependency_heads[target_handle - 1] =
                                static_cast<std::uint32_t>(prepared_graph.dependency_edges.size());
                        }
                    }
                } else {
                    const auto item_begin = static_cast<std::size_t>(contribution.definition_items.begin);
                    const auto item_count = static_cast<std::size_t>(contribution.definition_items.count);
                    if (item_begin > storage.enum_values.size() || item_count > storage.enum_values.size() - item_begin ||
                        prepared_graph.enum_values.size() >= (std::numeric_limits<std::uint32_t>::max)() ||
                        item_count > (std::numeric_limits<std::uint32_t>::max)()) {
                        result = {status_code::not_available};
                        emit_failure(result, source_state.source, operation, "enum definition range too large", diagnostics);
                        return result;
                    }

                    if (entry.enum_fixed_underlying()) {
                        // A previous compatible redeclaration may have fixed the
                        // underlying type before this defining declaration.
                    } else if (contribution.explicit_underlying != intrinsic_type::none) {
                        entry.enum_underlying = contribution.explicit_underlying;
                        entry.flags = static_cast<std::uint8_t>(entry.flags | 0x02u);
                    } else if (contribution.enum_scoped()) {
                        entry.enum_underlying = intrinsic_type::signed_int;
                    } else {
                        const auto values = item_count == 0 ?
                            std::span<const source_contribution_enum_value>{} :
                            std::span<const source_contribution_enum_value>{
                                storage.enum_values.data() + item_begin, item_count};
                        result = select_enum_underlying(values, abi, entry.enum_underlying);
                        if (!result.ok()) {
                            emit_failure(result, source_state.source, operation, "enum underlying selection failed", diagnostics);
                            return result;
                        }
                    }

                    entry.definition.begin = static_cast<std::uint32_t>(prepared_graph.enum_values.size() + 1);
                    entry.definition.count = static_cast<std::uint32_t>(item_count);
                    for (std::size_t item = 0; item < item_count; ++item) {
                        const auto& value = storage.enum_values[item_begin + item];
                        enum_value_record materialized;
                        materialized.bits = value.value.bits;
                        materialized.name = value.name;
                        materialized.intrinsic = value.value.intrinsic;
                        prepared_graph.enum_values.push_back(materialized);
                    }
                }
            }
        }
        const auto definition_end = clock_type::now();
        telemetry_value.definition_materialization_ns = elapsed_ns(definition_begin, definition_end);
        prepared_graph.live_type_count = prepared_graph.types.size();

        {
            std::size_t object_index_capacity = 0;
            if (!checked_index_capacity(prepared_graph.objects.size(), object_index_capacity)) {
                result = {status_code::not_available};
                emit_failure(result, {}, operation, "Graph object identity index too large", diagnostics);
                return result;
            }
            if (object_index_capacity == 0)
                object_index_capacity = 8;
            prepared_graph.object_identity_index.assign(object_index_capacity, {});
            for (std::size_t index = 0; index < prepared_graph.object_identities.size(); ++index) {
                insert_object_identity_slot(
                    prepared_graph.object_identity_index, prepared_graph.object_identities,
                    prepared_graph.object_identities[index], static_cast<std::uint32_t>(index + 1));
            }
        }

        std::size_t link_index_capacity = 0;
        if (!checked_index_capacity(storage.links.size(), link_index_capacity)) {
            result = {status_code::not_available};
            emit_failure(result, {}, operation, "Graph link index too large", diagnostics);
            return result;
        }
        if (link_index_capacity == 0)
            link_index_capacity = 8;
        prepared_graph.link_index.assign(link_index_capacity, {});
        for (std::size_t source_index = 1; source_index < storage.sources.size(); ++source_index) {
            const auto& source_state = storage.sources[source_index];
            if (!source_state.source)
                continue;
            const auto links_begin = static_cast<std::size_t>(source_state.links.begin);
            const auto links_count = static_cast<std::size_t>(source_state.links.count);
            if (links_begin > storage.links.size() || links_count > storage.links.size() - links_begin) {
                result = {status_code::invalid_argument};
                emit_failure(result, source_state.source, operation, "SourceContribution link range invalid", diagnostics);
                return result;
            }
            for (std::size_t offset = 0; offset < links_count; ++offset) {
                const auto& link = storage.links[links_begin + offset];
                const auto source_object = object_identity_index.find(link.source.object);
                const auto target_object = object_identity_index.find(link.target.object);
                if (source_object == 0 || target_object == 0) {
                    result = {status_code::semantic_conflict};
                    emit_failure(result, source_state.source, operation, "link endpoint object absent from Graph", diagnostics);
                    return result;
                }

                const object_endpoint source_endpoint{
                    object_handle{source_object}, link.source.member};
                const object_endpoint target_endpoint{
                    object_handle{target_object}, link.target.member};

                const auto validate_endpoint = [&](object_endpoint endpoint) noexcept -> bool {
                    if (!endpoint.object || !endpoint.member || endpoint.object.value() > prepared_graph.objects.size())
                        return false;
                    const auto& object = prepared_graph.objects[endpoint.object.value() - 1];
                    if (!object.live() || !object.type || object.type.value() >= prepared_graph.canonical_types.size())
                        return false;
                    const auto& type_ref = prepared_graph.canonical_types[object.type.value()];
                    if (type_ref.kind != canonical_type_kind::named || type_ref.child_or_handle == 0 ||
                        type_ref.child_or_handle > prepared_graph.types.size())
                        return false;
                    const auto& type = prepared_graph.types[type_ref.child_or_handle - 1];
                    return type.live() && type.kind == graph_type_kind::record && type.defined() &&
                        endpoint.member.value() < type.definition.count;
                };
                if (!validate_endpoint(source_endpoint) || !validate_endpoint(target_endpoint)) {
                    result = {status_code::semantic_conflict};
                    emit_failure(result, source_state.source, operation, "link endpoint is not a live record member", diagnostics);
                    return result;
                }

                prepared_graph.links.push_back({source_endpoint, target_endpoint});
                const auto handle = static_cast<std::uint32_t>(prepared_graph.links.size());
                if (!insert_link_slot(
                        prepared_graph.link_index, prepared_graph.links, target_endpoint, handle)) {
                    result = {status_code::semantic_conflict};
                    emit_failure(result, source_state.source, operation, "multiple links target the same endpoint", diagnostics);
                    return result;
                }
            }
        }
        prepared_graph.live_object_count = prepared_graph.objects.size();
        prepared_graph.live_link_count = prepared_graph.links.size();

        result = reserve_headroom(prepared_graph.types);
        if (!result.ok()) return result;
        result = reserve_headroom(prepared_graph.identities);
        if (!result.ok()) return result;
        result = reserve_headroom(prepared_graph.members);
        if (!result.ok()) return result;
        result = reserve_headroom(prepared_graph.enum_values);
        if (!result.ok()) return result;
        result = reserve_headroom(prepared_graph.objects);
        if (!result.ok()) return result;
        result = reserve_headroom(prepared_graph.object_identities);
        if (!result.ok()) return result;
        result = reserve_headroom(prepared_graph.links);
        if (!result.ok()) return result;
        result = reserve_headroom(prepared_graph.canonical_types);
        if (!result.ok()) return result;
        result = reserve_headroom(prepared_graph.named_refs);
        if (!result.ok()) return result;
        result = reserve_headroom(prepared_graph.dependency_versions);
        if (!result.ok()) return result;
        result = reserve_headroom(prepared_graph.reverse_dependency_heads);
        if (!result.ok()) return result;
        result = reserve_headroom(prepared_graph.dependency_edges);
        if (!result.ok()) return result;

        const auto validation_begin = clock_type::now();
        if (prepared_graph.types.size() != prepared_graph.identities.size() ||
            prepared_graph.types.size() != definition_seen.size()) {
            result = {status_code::invalid_argument};
            emit_failure(result, {}, operation, "detached Graph type arrays diverged", diagnostics);
            return result;
        }
        for (std::size_t index = 0; index < prepared_graph.types.size(); ++index) {
            if (prepared_graph.identities[index] == nullptr) {
                result = {status_code::invalid_argument};
                emit_failure(result, {}, operation, "detached Graph contains null identity_ref", diagnostics);
                return result;
            }
            const auto& entry = prepared_graph.types[index];
            if (!entry.definition)
                continue;
            const auto range_begin = static_cast<std::size_t>(entry.definition.begin - 1);
            const auto range_count = static_cast<std::size_t>(entry.definition.count);
            const auto arena_size = entry.kind == graph_type_kind::record ?
                prepared_graph.members.size() : prepared_graph.enum_values.size();
            if (range_begin > arena_size || range_count > arena_size - range_begin) {
                result = {status_code::invalid_argument};
                emit_failure(result, {}, operation, "detached Graph definition range invalid", diagnostics);
                return result;
            }
        }
        if (prepared_graph.objects.size() != prepared_graph.object_identities.size()) {
            result = {status_code::invalid_argument};
            emit_failure(result, {}, operation, "detached Graph object arrays diverged", diagnostics);
            return result;
        }
        for (std::size_t index = 0; index < prepared_graph.objects.size(); ++index) {
            if (prepared_graph.object_identities[index] == nullptr || !prepared_graph.objects[index].live() ||
                !prepared_graph.objects[index].type ||
                prepared_graph.objects[index].type.value() >= prepared_graph.canonical_types.size()) {
                result = {status_code::invalid_argument};
                emit_failure(result, {}, operation, "detached Graph object invalid", diagnostics);
                return result;
            }
        }
        for (const auto& link : prepared_graph.links) {
            if (!link.live()) {
                result = {status_code::invalid_argument};
                emit_failure(result, {}, operation, "detached Graph link invalid", diagnostics);
                return result;
            }
        }

        for (std::size_t index = 1; index < prepared_graph.canonical_types.size(); ++index) {
            const auto& type = prepared_graph.canonical_types[index];
            if (type.kind == canonical_type_kind::named) {
                if (type.child_or_handle == 0 || type.child_or_handle > prepared_graph.types.size()) {
                    result = {status_code::invalid_argument};
                    emit_failure(result, {}, operation, "named TypeRef handle invalid", diagnostics);
                    return result;
                }
            } else if (type.kind == canonical_type_kind::derived) {
                if (type.child_or_handle == 0 || type.child_or_handle >= prepared_graph.canonical_types.size()) {
                    result = {status_code::invalid_argument};
                    emit_failure(result, {}, operation, "derived TypeRef child invalid", diagnostics);
                    return result;
                }
            }
        }
        const auto validation_end = clock_type::now();
        telemetry_value.validation_ns = elapsed_ns(validation_begin, validation_end);
        return {};
    } catch (const std::bad_alloc&) {
        result = {status_code::initialization_failed};
    } catch (const std::length_error&) {
        result = {status_code::not_available};
    } catch (...) {
        result = {status_code::initialization_failed};
    }

    emit_failure(result, {}, operation, "Full Graph preparation failed", diagnostics);
    return result;
}

void generation_builder::publish_prepared() noexcept {
    if (!prepared || published_value)
        return;

    const auto begin = clock_type::now();
    if (mode == build_mode::rebuild) {
        target.publish_prepared(prepared_graph);
        contributions.publish_prepared();
    } else if (mode == build_mode::incremental) {
        target.publish_prepared(prepared_update);
        sparse_contributions.publish_prepared();
    } else {
        return;
    }
    const auto end = clock_type::now();
    telemetry_value.publish_ns = elapsed_ns(begin, end);
    published_value = true;
}

} // namespace cw::server
