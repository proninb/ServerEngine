#include "build_cache_image.hpp"
#include "crc64_ecma.hpp"

#include "compiled_image.hpp"
#include "source_manager_image.hpp"
#include "../project_context.hpp"
#include "../source/source_hash.hpp"
#include "../source/source_manager.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <atomic>
#include <thread>
#include <type_traits>
#include <system_error>
#include <vector>

namespace cw::server {
namespace {

constexpr std::array<std::byte, 8> image_magic{
    std::byte{'S'}, std::byte{'E'}, std::byte{'B'}, std::byte{'C'},
    std::byte{'V'}, std::byte{'1'}, std::byte{0}, std::byte{0},
};

constexpr std::uint32_t endian_marker = 0x01020304u;
constexpr std::size_t directory_offset = build_cache_image_header_size;
constexpr std::size_t directory_bytes =
    build_cache_image_directory_count * build_cache_image_directory_entry_size;
constexpr std::size_t first_section_offset =
    build_cache_image_prefix_size;

constexpr std::size_t header_flags_offset = 28;
constexpr std::size_t header_source_count_offset = 48;
constexpr std::size_t header_frontend_count_offset = 56;
constexpr std::size_t header_source_bytes_offset = 64;
constexpr std::size_t header_derived_entries_offset = 72;
constexpr std::size_t header_statistics_offset = 80;
constexpr std::size_t header_change_backend_offset = 136;
constexpr std::size_t header_change_volume_offset = 144;
constexpr std::size_t header_change_journal_offset = 152;
constexpr std::size_t header_change_usn_offset = 160;
constexpr std::size_t header_reserved_begin = 168;
constexpr std::size_t header_directory_crc_offset = 240;
constexpr std::size_t header_crc_offset = 248;

constexpr std::uint32_t flag_frontend_complete = 0x01u;
constexpr std::uint32_t flag_contributions_complete = 0x02u;
constexpr std::uint32_t known_flags =
    flag_frontend_complete | flag_contributions_complete;

constexpr std::uint32_t source_flag_snapshot = 0x01u;
constexpr std::uint32_t source_flag_frontend = 0x02u;
constexpr std::uint32_t source_known_flags =
    source_flag_snapshot | source_flag_frontend;

constexpr std::uint32_t source_directory_record_size = 56;
static_assert(
    sizeof(project_generation_build_cache_source_directory_record) ==
        source_directory_record_size);
static_assert(
    offsetof(
        project_generation_build_cache_source_directory_record,
        source) == 0);
static_assert(
    offsetof(
        project_generation_build_cache_source_directory_record,
        flags) == 4);
static_assert(
    offsetof(
        project_generation_build_cache_source_directory_record,
        text_offset) == 8);
static_assert(
    offsetof(
        project_generation_build_cache_source_directory_record,
        text_length) == 16);
static_assert(
    offsetof(
        project_generation_build_cache_source_directory_record,
        reserved) == 20);
static_assert(
    offsetof(
        project_generation_build_cache_source_directory_record,
        local_types_begin) == 24);
static_assert(
    offsetof(
        project_generation_build_cache_source_directory_record,
        member_slots_count) == 52);
constexpr std::uint32_t frontend_local_type_record_size = 4;
constexpr std::uint32_t frontend_type_slot_record_size = 12;
constexpr std::uint32_t frontend_object_slot_record_size = 16;
constexpr std::uint32_t frontend_member_slot_record_size = 12;
constexpr std::uint32_t contribution_state_record_size = 56;
constexpr std::uint32_t contribution_type_record_size = 16;
constexpr std::uint32_t contribution_member_record_size = 24;
constexpr std::uint32_t contribution_modifier_record_size = 16;
constexpr std::uint32_t contribution_enum_value_record_size = 16;
constexpr std::uint32_t contribution_object_record_size = 20;
constexpr std::uint32_t contribution_link_record_size = 16;
constexpr std::uint32_t construction_state_record_size = 40;
constexpr std::uint32_t type_ref_record_size = 4;
constexpr std::uint32_t derived_index_record_size = 8;
constexpr std::uint32_t u32_record_size = 4;
constexpr std::uint32_t dependency_edge_record_size = 12;
constexpr std::uint32_t historical_index_record_size = 8;
constexpr std::uint32_t source_file_identity_index_record_size = 16;
constexpr std::uint32_t tracked_directory_identity_index_record_size = 16;

// GEN-02C18 native arena serialization is valid only when the in-memory
// compact value layout is byte-identical to Build Cache v4 on little-endian.
static_assert(sizeof(identity_ref) ==
    frontend_local_type_record_size);
static_assert(sizeof(source_interface_type_slot) ==
    frontend_type_slot_record_size);
static_assert(sizeof(source_interface_object_slot) ==
    frontend_object_slot_record_size);
static_assert(sizeof(source_interface_member_slot) ==
    frontend_member_slot_record_size);

static_assert(std::is_trivially_copyable_v<identity_ref>);
static_assert(std::is_trivially_copyable_v<
    source_interface_type_slot>);
static_assert(std::is_trivially_copyable_v<
    source_interface_object_slot>);
static_assert(std::is_trivially_copyable_v<
    source_interface_member_slot>);

static_assert(std::is_standard_layout_v<
    source_interface_type_slot>);
static_assert(std::is_standard_layout_v<
    source_interface_object_slot>);
static_assert(std::is_standard_layout_v<
    source_interface_member_slot>);

static_assert(offsetof(
    source_interface_type_slot, parent) == 0);
static_assert(offsetof(
    source_interface_type_slot, name) == 4);
static_assert(offsetof(
    source_interface_type_slot, identity) == 8);

static_assert(offsetof(
    source_interface_object_slot, parent) == 0);
static_assert(offsetof(
    source_interface_object_slot, name) == 4);
static_assert(offsetof(
    source_interface_object_slot, identity) == 8);
static_assert(offsetof(
    source_interface_object_slot, named_type) == 12);

static_assert(offsetof(
    source_interface_member_slot, type) == 0);
static_assert(offsetof(
    source_interface_member_slot, name) == 4);
static_assert(offsetof(
    source_interface_member_slot, index) == 8);

// R5E4-B1: these construction records are persistence-native on
// little-endian hosts. Explicit reserved bytes remove implicit-padding
// dependence from the direct Generation section contract.
static_assert(sizeof(source_contribution_state) ==
    contribution_state_record_size);
static_assert(offsetof(source_contribution_state, source) == 0);
static_assert(offsetof(source_contribution_state, reserved) == 4);
static_assert(offsetof(source_contribution_state, types) == 8);
static_assert(offsetof(source_contribution_state, links) == 48);

static_assert(sizeof(source_contribution_type_ref) == 16);
static_assert(offsetof(source_contribution_type_ref, identity) == 0);
static_assert(offsetof(source_contribution_type_ref, modifiers) == 4);
static_assert(offsetof(source_contribution_type_ref, intrinsic) == 12);
static_assert(offsetof(source_contribution_type_ref, reserved) == 13);

static_assert(sizeof(source_contribution_type) ==
    contribution_type_record_size);
static_assert(offsetof(source_contribution_type, identity) == 0);
static_assert(offsetof(source_contribution_type, definition_items) == 4);
static_assert(offsetof(source_contribution_type, explicit_underlying) == 12);
static_assert(offsetof(source_contribution_type, record_kind) == 13);
static_assert(offsetof(source_contribution_type, kind) == 14);
static_assert(offsetof(source_contribution_type, flags) == 15);

static_assert(sizeof(source_contribution_member) ==
    contribution_member_record_size);
static_assert(offsetof(source_contribution_member, type) == 0);
static_assert(offsetof(source_contribution_member, name) == 16);
static_assert(offsetof(source_contribution_member, access) == 20);
static_assert(offsetof(source_contribution_member, reserved) == 21);

static_assert(sizeof(source_type_modifier) ==
    contribution_modifier_record_size);
static_assert(offsetof(source_type_modifier, value) == 0);
static_assert(offsetof(source_type_modifier, kind) == 8);
static_assert(offsetof(source_type_modifier, reserved) == 9);

static_assert(sizeof(source_contribution_object) ==
    contribution_object_record_size);
static_assert(offsetof(source_contribution_object, identity) == 0);
static_assert(offsetof(source_contribution_object, type) == 4);

static_assert(sizeof(source_object_endpoint_fact) == 8);
static_assert(sizeof(source_contribution_link) ==
    contribution_link_record_size);
static_assert(offsetof(source_contribution_link, source) == 0);
static_assert(offsetof(source_contribution_link, target) == 8);

static_assert(sizeof(source_construction_state) ==
    construction_state_record_size);
static_assert(offsetof(source_construction_state, declarations) == 0);
static_assert(offsetof(source_construction_state, enum_fixed) == 32);
static_assert(offsetof(source_construction_state, fixed_underlying) == 36);
static_assert(offsetof(source_construction_state, kind) == 37);
static_assert(offsetof(source_construction_state, reserved) == 38);

// GEN-02C29 native Graph persistence layout. Fresh G0 may bulk-copy these
// arrays only when their in-memory representation is exactly Build Cache v4.
static_assert(sizeof(TypeRef) == type_ref_record_size);
static_assert(std::is_trivially_copyable_v<TypeRef>);
static_assert(std::is_standard_layout_v<TypeRef>);

static_assert(sizeof(graph_derived_index_slot) == derived_index_record_size);
static_assert(std::is_trivially_copyable_v<graph_derived_index_slot>);
static_assert(std::is_standard_layout_v<graph_derived_index_slot>);
static_assert(offsetof(graph_derived_index_slot, fingerprint) == 0);
static_assert(offsetof(graph_derived_index_slot, type_ref) == 4);

static_assert(sizeof(graph_dependency_edge) == dependency_edge_record_size);
static_assert(std::is_trivially_copyable_v<graph_dependency_edge>);
static_assert(std::is_standard_layout_v<graph_dependency_edge>);
static_assert(offsetof(graph_dependency_edge, owner_handle) == 0);
static_assert(offsetof(graph_dependency_edge, next_for_target) == 4);
static_assert(offsetof(graph_dependency_edge, owner_version) == 8);

static_assert(sizeof(graph_identity_index_slot) == historical_index_record_size);
static_assert(std::is_trivially_copyable_v<graph_identity_index_slot>);
static_assert(std::is_standard_layout_v<graph_identity_index_slot>);
static_assert(offsetof(graph_identity_index_slot, fingerprint) == 0);
static_assert(offsetof(graph_identity_index_slot, handle) == 4);

static_assert(sizeof(graph_object_identity_index_slot) ==
    historical_index_record_size);
static_assert(std::is_trivially_copyable_v<
    graph_object_identity_index_slot>);
static_assert(std::is_standard_layout_v<
    graph_object_identity_index_slot>);
static_assert(offsetof(
    graph_object_identity_index_slot, fingerprint) == 0);
static_assert(offsetof(
    graph_object_identity_index_slot, handle) == 4);

static_assert(sizeof(graph_link_index_slot) == historical_index_record_size);
static_assert(std::is_trivially_copyable_v<graph_link_index_slot>);
static_assert(std::is_standard_layout_v<graph_link_index_slot>);
static_assert(offsetof(graph_link_index_slot, fingerprint) == 0);
static_assert(offsetof(graph_link_index_slot, handle) == 4);

struct layout_section final {
    build_cache_image_section kind{};
    std::uint32_t record_size = 0;
    std::uint64_t count = 0;
    std::uint64_t offset = 0;
    std::uint64_t crc64 = 0;
};

[[nodiscard]] constexpr std::size_t section_index(
    build_cache_image_section kind) noexcept {

    const auto raw = static_cast<std::uint32_t>(kind);
    return raw >= 1 && raw <= build_cache_image_directory_count
        ? static_cast<std::size_t>(raw - 1)
        : build_cache_image_directory_count;
}

// Tracks one architectural fact only: whether each logical output section is
// still byte-identical to its whole pinned-baseline section. A baseline copy
// establishes exact identity; a geometry change invalidates it; an overwrite
// preserves it only when the serialized replacement bytes equal the bytes they
// replaced. This same mechanism is shared by every baseline-backed encoder path.
class build_cache_section_identity_tracker final {
public:
    explicit build_cache_section_identity_tracker(
        build_cache_encode_provenance* output_value) noexcept
        : output(output_value) {}

    void establish_exact(
        build_cache_image_section kind) noexcept {

        if (output == nullptr)
            return;

        const auto index = section_index(kind);
        if (index >= output->sections.size())
            return;

        output->sections[index] =
            build_cache_baseline_section_identity::exact;
    }

    void invalidate(
        build_cache_image_section kind) noexcept {

        if (output == nullptr)
            return;

        const auto index = section_index(kind);
        if (index >= output->sections.size())
            return;

        output->sections[index] =
            build_cache_baseline_section_identity::none;
    }

    void observe_overwrite(
        build_cache_image_section kind,
        std::span<const std::byte> encoded,
        std::span<const std::byte> baseline) noexcept {

        if (output == nullptr)
            return;

        const auto index = section_index(kind);
        if (index >= output->sections.size() ||
            !output->exact(index)) {
            return;
        }

        if (encoded.size() != baseline.size() ||
            (!encoded.empty() &&
             std::memcmp(
                 encoded.data(),
                 baseline.data(),
                 encoded.size()) != 0)) {

            output->sections[index] =
                build_cache_baseline_section_identity::none;
        }
    }

private:
    build_cache_encode_provenance* output = nullptr;
};

[[nodiscard]] constexpr std::uint64_t align64(std::uint64_t value) noexcept {
    return (value + 63u) & ~std::uint64_t{63u};
}

[[nodiscard]] bool add_u64(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& output) noexcept {

    if (left > (std::numeric_limits<std::uint64_t>::max)() - right)
        return false;
    output = left + right;
    return true;
}

[[nodiscard]] bool multiply_u64(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& output) noexcept {

    if (left != 0 &&
        right > (std::numeric_limits<std::uint64_t>::max)() / left) {
        return false;
    }
    output = left * right;
    return true;
}

template <typename T>
[[nodiscard]] status native_frontend_span(
    std::span<const std::byte> bytes,
    build_cache_range range,
    std::uint32_t record_size,
    std::span<const T>& output) noexcept {

    output = {};

    if constexpr (std::endian::native != std::endian::little)
        return {status_code::not_available};

    if (record_size != sizeof(T) ||
        !std::is_trivially_copyable_v<T> ||
        !std::is_standard_layout_v<T> ||
        !std::has_unique_object_representations_v<T>) {
        return {status_code::not_available};
    }

    const auto stride =
        static_cast<std::size_t>(record_size);
    const auto begin =
        static_cast<std::size_t>(range.begin);
    const auto count =
        static_cast<std::size_t>(range.count);
    const auto maximum =
        (std::numeric_limits<std::size_t>::max)();

    if (stride == 0 ||
        begin > maximum / stride ||
        count > maximum / stride) {
        return {status_code::artifact_corrupt};
    }

    const auto byte_begin = begin * stride;
    const auto byte_count = count * stride;

    // A valid sparse/sectioned image may not expose one contiguous byte span.
    // R4a is a native-view optimization only; keep the canonical accessor path
    // available by reporting not_available instead of treating that as damage.
    if (count != 0 && bytes.empty())
        return {status_code::not_available};

    if (byte_begin > bytes.size() ||
        byte_count > bytes.size() - byte_begin) {
        return {status_code::artifact_corrupt};
    }

    if (count == 0)
        return {};

    const auto* address =
        bytes.data() + byte_begin;

    if (reinterpret_cast<std::uintptr_t>(address) %
            alignof(T) != 0) {
        return {status_code::not_available};
    }

    output = std::span<const T>{
        reinterpret_cast<const T*>(address),
        count,
    };

    return {};
}

template <typename T>
[[nodiscard]] status native_frontend_span(
    const project_generation_segment& segment,
    build_cache_range range,
    std::uint32_t record_size,
    std::span<const T>& output) noexcept {

    output = {};

    if constexpr (std::endian::native != std::endian::little)
        return {status_code::not_available};

    if (record_size != sizeof(T) ||
        !std::is_trivially_copyable_v<T> ||
        !std::is_standard_layout_v<T> ||
        !std::has_unique_object_representations_v<T>) {
        return {status_code::not_available};
    }

    const auto stride =
        static_cast<std::size_t>(record_size);
    const auto begin =
        static_cast<std::size_t>(range.begin);
    const auto count =
        static_cast<std::size_t>(range.count);
    const auto maximum =
        (std::numeric_limits<std::size_t>::max)();

    if (stride == 0 ||
        begin > maximum / stride ||
        count > maximum / stride) {
        return {status_code::artifact_corrupt};
    }

    const auto byte_begin = begin * stride;
    const auto byte_count = count * stride;

    if (byte_begin > segment.size() ||
        byte_count > segment.size() - byte_begin) {
        return {status_code::artifact_corrupt};
    }

    if (count == 0)
        return {};

    std::size_t cursor = 0;

    for (std::size_t extent_index = 0;
         extent_index < segment.extent_count();
         ++extent_index) {

        const auto extent =
            segment.extent(extent_index);

        if (byte_begin >= cursor) {
            const auto local_offset =
                byte_begin - cursor;

            if (local_offset <= extent.size() &&
                byte_count <=
                    extent.size() - local_offset) {

                const auto* address =
                    extent.data() + local_offset;

                if (reinterpret_cast<std::uintptr_t>(
                        address) %
                        alignof(T) != 0) {
                    return {status_code::not_available};
                }

                output = std::span<const T>{
                    reinterpret_cast<const T*>(
                        address),
                    count,
                };
                return {};
            }
        }

        if (extent.size() > maximum - cursor)
            return {status_code::artifact_corrupt};

        cursor += extent.size();
    }

    // source_frontend_block_store keeps one Source block inside one page.
    // A cross-extent range cannot become one native std::span without copying.
    return {status_code::not_available};
}



// GEN-02C15: persisted integers are little-endian. Native little-endian hosts
// can load/store the complete scalar with memcpy; big/mixed-endian hosts retain
// the explicit byte-wise canonical representation. memcpy is alignment-safe.
void write_u32(std::byte* target, std::uint32_t value) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        std::memcpy(target, &value, sizeof(value));
        return;
    }

    target[0] = static_cast<std::byte>(value & 0xffu);
    target[1] = static_cast<std::byte>((value >> 8) & 0xffu);
    target[2] = static_cast<std::byte>((value >> 16) & 0xffu);
    target[3] = static_cast<std::byte>((value >> 24) & 0xffu);
}

void write_u64(std::byte* target, std::uint64_t value) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        std::memcpy(target, &value, sizeof(value));
        return;
    }

    for (std::size_t index = 0; index < 8; ++index) {
        target[index] =
            static_cast<std::byte>((value >> (index * 8)) & 0xffu);
    }
}

[[nodiscard]] std::uint32_t read_u32(const std::byte* source) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        std::uint32_t value = 0;
        std::memcpy(&value, source, sizeof(value));
        return value;
    }

    return
        static_cast<std::uint32_t>(source[0]) |
        (static_cast<std::uint32_t>(source[1]) << 8) |
        (static_cast<std::uint32_t>(source[2]) << 16) |
        (static_cast<std::uint32_t>(source[3]) << 24);
}

[[nodiscard]] std::uint64_t read_u64(const std::byte* source) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        std::uint64_t value = 0;
        std::memcpy(&value, source, sizeof(value));
        return value;
    }

    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index)
        value |= static_cast<std::uint64_t>(source[index]) << (index * 8);
    return value;
}

[[nodiscard]] bool zero_bytes(
    const std::byte* data,
    std::size_t count) noexcept {

    for (std::size_t index = 0; index < count; ++index) {
        if (data[index] != std::byte{0})
            return false;
    }
    return true;
}

[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

[[nodiscard]] constexpr std::uint32_t fold32(std::uint64_t value) noexcept {
    auto result = static_cast<std::uint32_t>(value ^ (value >> 32));
    return result == 0 ? 1u : result;
}

[[nodiscard]] constexpr std::uint64_t graph_identity_hash(identity_ref identity) noexcept {
    return mix64(static_cast<std::uint64_t>(identity.value()));
}

[[nodiscard]] constexpr std::uint64_t endpoint_hash(object_endpoint endpoint) noexcept {
    return mix64(
        (static_cast<std::uint64_t>(endpoint.object.value()) << 32) ^
        static_cast<std::uint64_t>(endpoint.member.value()));
}

[[nodiscard]] constexpr std::uint32_t expected_record_size(
    build_cache_image_section kind) noexcept {

    switch (kind) {
    case build_cache_image_section::source_directory:
        return source_directory_record_size;
    case build_cache_image_section::source_bytes:
        return 1;
    case build_cache_image_section::frontend_local_types:
        return frontend_local_type_record_size;
    case build_cache_image_section::frontend_type_slots:
        return frontend_type_slot_record_size;
    case build_cache_image_section::frontend_object_slots:
        return frontend_object_slot_record_size;
    case build_cache_image_section::frontend_member_slots:
        return frontend_member_slot_record_size;
    case build_cache_image_section::contribution_states:
        return contribution_state_record_size;
    case build_cache_image_section::contribution_types:
        return contribution_type_record_size;
    case build_cache_image_section::contribution_members:
        return contribution_member_record_size;
    case build_cache_image_section::contribution_modifiers:
        return contribution_modifier_record_size;
    case build_cache_image_section::contribution_enum_values:
        return contribution_enum_value_record_size;
    case build_cache_image_section::contribution_objects:
        return contribution_object_record_size;
    case build_cache_image_section::contribution_links:
        return contribution_link_record_size;
    case build_cache_image_section::construction_states:
        return construction_state_record_size;
    case build_cache_image_section::graph_intrinsic_refs:
    case build_cache_image_section::graph_named_refs:
    case build_cache_image_section::graph_dependency_versions:
    case build_cache_image_section::graph_reverse_dependency_heads:
        return u32_record_size;
    case build_cache_image_section::graph_derived_index:
        return derived_index_record_size;
    case build_cache_image_section::graph_dependency_edges:
        return dependency_edge_record_size;
    case build_cache_image_section::graph_type_identity_index:
    case build_cache_image_section::graph_object_identity_index:
    case build_cache_image_section::graph_link_target_index:
        return historical_index_record_size;
    case build_cache_image_section::source_file_identity_index:
        return source_file_identity_index_record_size;
    case build_cache_image_section::tracked_directory_identity_index:
        return tracked_directory_identity_index_record_size;
    }
    return 0;
}

[[nodiscard]] bool valid_intrinsic(std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(intrinsic_type::nullptr_type);
}

[[nodiscard]] bool valid_member_access(std::uint8_t value) noexcept {
    return value <=
        static_cast<std::uint8_t>(source_member_access::private_access);
}

[[nodiscard]] bool valid_record_kind(std::uint8_t value) noexcept {
    return value <=
        static_cast<std::uint8_t>(source_record_kind::union_type);
}

[[nodiscard]] bool valid_contribution_kind(std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(
        source_contribution_type_kind::enumeration);
}

[[nodiscard]] bool valid_modifier_kind(std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(
        source_type_modifier_kind::unbounded_array);
}

[[nodiscard]] bool valid_identity_kind(std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(identity_kind::object);
}

[[nodiscard]] bool valid_range(
    source_fact_range range,
    std::uint64_t count) noexcept {

    return range.begin <= count &&
        range.count <= count - range.begin;
}

[[nodiscard]] bool valid_cache_range(
    build_cache_range range,
    std::uint64_t count) noexcept {

    return range.begin <= count &&
        range.count <= count - range.begin;
}

void write_range(
    std::byte* target,
    source_fact_range range) noexcept {

    write_u32(target, range.begin);
    write_u32(target + 4, range.count);
}

[[nodiscard]] source_fact_range read_range(
    const std::byte* source) noexcept {

    return {read_u32(source), read_u32(source + 4)};
}

void write_cache_range(
    std::byte* target,
    build_cache_range range) noexcept {

    write_u32(target, range.begin);
    write_u32(target + 4, range.count);
}

[[nodiscard]] build_cache_range read_cache_range(
    const std::byte* source) noexcept {

    return {read_u32(source), read_u32(source + 4)};
}

[[nodiscard]] bool add_u32_count(
    std::size_t current,
    std::size_t additional,
    std::uint32_t& output) noexcept {

    if (additional >
        (std::numeric_limits<std::uint32_t>::max)() - current) {
        return false;
    }
    output = static_cast<std::uint32_t>(current + additional);
    return true;
}

} // namespace

const build_cache_image_view::section_view& build_cache_image_view::section(
    build_cache_image_section kind) const noexcept {

    static const section_view empty{};
    const auto index = section_index(kind);
    return index < build_cache_image_directory_count
        ? sections[index]
        : empty;
}

void build_cache_image_view::reset() noexcept {
    bytes = {};
    for (auto& value : sections)
        value = {};
    contribution_statistics_value = {};
    source_count_value = 0;
    frontend_count_value = 0;
    derived_index_entries_value = 0;
    change_checkpoint_value = {};
    frontend_complete_value = false;
    contributions_complete_value = false;
}

std::span<const std::byte> build_cache_image_view::section_bytes(
    build_cache_image_section kind) const noexcept {

    const auto& value = section(kind);

    std::uint64_t byte_count = 0;
    if (value.data == nullptr ||
        !multiply_u64(
            value.count,
            value.record_size,
            byte_count) ||
        byte_count >
            (std::numeric_limits<std::size_t>::max)()) {
        return {};
    }

    return {
        value.data,
        static_cast<std::size_t>(byte_count),
    };
}

void build_cache_generation_segments::reset() noexcept {
    prefix_value = {};
    for (auto& value : sections_value)
        value = {};
    section_offsets.fill(0);
    logical_size_value = 0;
    physical_size_value = 0;
    physical_extent_count_value = 0;
}

const project_generation_segment&
build_cache_generation_segments::section(
    build_cache_image_section kind) const noexcept {

    static const project_generation_segment empty{};

    const auto index = section_index(kind);
    return index < sections_value.size()
        ? sections_value[index]
        : empty;
}

status build_cache_generation_segments::bind(
    std::span<const std::byte> image) noexcept {

    reset();

    build_cache_image_view validated;
    const auto result = validated.bind(image);
    if (!result.ok())
        return result;

    return bind_validated_contiguous(
        image,
        validated);
}

status build_cache_generation_segments::
bind_validated_contiguous(
    std::span<const std::byte> image,
    const build_cache_image_view& validated) noexcept {

    reset();

    if (!validated.valid() ||
        image.size() < build_cache_image_prefix_size) {
        return {status_code::invalid_argument};
    }

    const auto image_begin =
        reinterpret_cast<std::uintptr_t>(
            image.data());
    const auto image_end =
        image_begin + image.size();

    if (image_end < image_begin)
        return {status_code::artifact_corrupt};

    std::array<
        project_generation_segment,
        build_cache_image_directory_count>
        candidate{};

    for (std::size_t index = 0;
         index < candidate.size();
         ++index) {

        const auto kind =
            static_cast<build_cache_image_section>(
                index + 1);
        const auto bytes =
            validated.section_bytes(kind);

        if (bytes.empty())
            continue;

        const auto begin =
            reinterpret_cast<std::uintptr_t>(
                bytes.data());
        const auto end =
            begin + bytes.size();

        if (end < begin ||
            begin < image_begin ||
            end > image_end) {
            return {status_code::invalid_argument};
        }

        candidate[index] =
            project_generation_segment{bytes};
    }

    return bind_sectioned(
        image.first(build_cache_image_prefix_size),
        candidate,
        image.size());
}

status build_cache_generation_segments::bind_sectioned(
    std::span<const std::byte> prefix,
    const std::array<
        project_generation_segment,
        build_cache_image_directory_count>& section_values,
    std::size_t logical_size) noexcept {

    reset();

    if (prefix.size() !=
            build_cache_image_prefix_size ||
        logical_size <
            build_cache_image_prefix_size) {
        return {status_code::artifact_corrupt};
    }

    if (!std::equal(
            image_magic.begin(),
            image_magic.end(),
            prefix.begin())) {
        return {status_code::artifact_corrupt};
    }

    if (read_u32(prefix.data() + 8) !=
        build_cache_image_format_version) {
        return {status_code::rebuild_required};
    }

    const auto flags =
        read_u32(
            prefix.data() +
            header_flags_offset);

    if (read_u32(prefix.data() + 12) !=
            endian_marker ||
        read_u32(prefix.data() + 16) !=
            build_cache_image_header_size ||
        read_u32(prefix.data() + 20) !=
            build_cache_image_directory_count ||
        read_u32(prefix.data() + 24) !=
            build_cache_image_directory_entry_size ||
        (flags & ~known_flags) != 0 ||
        (flags & known_flags) != known_flags ||
        read_u64(prefix.data() + 32) !=
            directory_offset ||
        read_u64(prefix.data() + 40) !=
            logical_size) {
        return {status_code::artifact_corrupt};
    }

    if (!zero_bytes(
            prefix.data() +
                header_reserved_begin,
            header_directory_crc_offset -
                header_reserved_begin)) {
        return {status_code::artifact_corrupt};
    }

    std::array<
        std::byte,
        build_cache_image_header_size>
        header{};

    std::memcpy(
        header.data(),
        prefix.data(),
        header.size());

    const auto stored_header_crc =
        read_u64(
            header.data() +
            header_crc_offset);

    write_u64(
        header.data() +
            header_crc_offset,
        0);

    if (persistence_crc64(header) !=
        stored_header_crc) {
        return {status_code::artifact_corrupt};
    }

    const auto directory_span =
        prefix.subspan(
            directory_offset,
            directory_bytes);

    if (persistence_crc64(
            directory_span) !=
        read_u64(
            prefix.data() +
            header_directory_crc_offset)) {
        return {status_code::artifact_corrupt};
    }

    const auto prefix_payload_end =
        directory_offset +
        directory_bytes;

    if (prefix_payload_end >
            prefix.size() ||
        !zero_bytes(
            prefix.data() +
                prefix_payload_end,
            prefix.size() -
                prefix_payload_end)) {
        return {status_code::artifact_corrupt};
    }

    std::uint64_t previous_end =
        first_section_offset;

    std::size_t physical_size =
        prefix.size();
    std::size_t physical_extents = 1;

    for (std::size_t index = 0;
         index <
            build_cache_image_directory_count;
         ++index) {

        const auto* entry =
            prefix.data() +
            directory_offset +
            index *
                build_cache_image_directory_entry_size;

        const auto raw_kind =
            read_u32(entry);
        const auto kind =
            static_cast<build_cache_image_section>(
                raw_kind);
        const auto record_size =
            read_u32(entry + 4);
        const auto offset =
            read_u64(entry + 8);
        const auto count =
            read_u64(entry + 16);

        const auto aligned_offset =
            align64(previous_end);

        if (raw_kind != index + 1 ||
            record_size !=
                expected_record_size(kind) ||
            offset != aligned_offset ||
            (offset & 63u) != 0) {
            return {status_code::artifact_corrupt};
        }

        std::uint64_t byte_count = 0;
        std::uint64_t end = 0;

        if (!multiply_u64(
                count,
                record_size,
                byte_count) ||
            !add_u64(
                offset,
                byte_count,
                end) ||
            end > logical_size ||
            byte_count !=
                section_values[index].size()) {
            return {status_code::artifact_corrupt};
        }

        if (section_values[index].size() >
            (std::numeric_limits<std::size_t>::max)() -
                physical_size) {
            return {status_code::not_available};
        }

        physical_size +=
            section_values[index].size();

        if (section_values[index].extent_count() >
            (std::numeric_limits<std::size_t>::max)() -
                physical_extents) {
            return {status_code::not_available};
        }

        physical_extents +=
            section_values[index].extent_count();

        section_offsets[index] = offset;
        previous_end = end;
    }

    if (previous_end != logical_size)
        return {status_code::artifact_corrupt};

    prefix_value = prefix;
    sections_value = section_values;
    logical_size_value = logical_size;
    physical_size_value = physical_size;
    physical_extent_count_value =
        physical_extents;

    return {};
}


status build_cache_generation_segments::bind_view(
    build_cache_image_view& output) const noexcept {

    output.reset();

    if (!valid())
        return {status_code::invalid_state};

    return output.bind_sectioned(
        prefix_value,
        sections_value);
}

status build_cache_generation_segments::bind_validated_sections(
    std::span<const std::byte> layout_backing,
    const build_cache_image_view& validated,
    const build_cache_encode_sparse_sections* sparse) noexcept {

    reset();

    if (!validated.valid() ||
        layout_backing.size() < build_cache_image_prefix_size) {
        return {status_code::invalid_argument};
    }

    std::array<
        project_generation_segment,
        build_cache_image_directory_count> sections{};

    for (std::size_t index = 0; index < sections.size(); ++index) {
        const auto kind =
            static_cast<build_cache_image_section>(index + 1);
        if (sparse != nullptr &&
            !sparse->sections[index].empty()) {
            sections[index] =
                sparse->sections[index];
        }
        else {
            sections[index] =
                project_generation_segment{
                    validated.section_bytes(kind)};
        }
    }

    return bind_sectioned(
        layout_backing.first(build_cache_image_prefix_size),
        sections,
        layout_backing.size());
}

project_generation_segment
build_cache_generation_segments::logical_segment(
    std::span<const std::byte> layout_backing) const noexcept {

    // R5E2D-A: logical bytes are reconstructed from the canonical prefix,
    // section carriers and deterministic zero alignment gaps. The large
    // monolithic encoder buffer is no longer a lifetime requirement.
    (void)layout_backing;

    project_generation_segment output;

    if (!valid() ||
        prefix_value.size() != build_cache_image_prefix_size ||
        logical_size_value < prefix_value.size()) {
        return output;
    }

    if (!output.append(prefix_value))
        return {};

    static constexpr std::array<std::byte, 64>
        zero_padding{};

    const auto append_padding =
        [&](std::size_t count) noexcept {

            while (count != 0) {
                const auto chunk =
                    (std::min)(
                        count,
                        zero_padding.size());

                if (!output.append(
                        std::span<const std::byte>{
                            zero_padding.data(),
                            chunk})) {
                    return false;
                }

                count -= chunk;
            }

            return true;
        };

    std::size_t previous_end =
        build_cache_image_prefix_size;

    for (std::size_t index = 0;
         index < sections_value.size();
         ++index) {

        const auto raw_offset =
            section_offsets[index];

        if (raw_offset >
            (std::numeric_limits<std::size_t>::max)()) {
            return {};
        }

        const auto offset =
            static_cast<std::size_t>(
                raw_offset);

        const auto& value =
            sections_value[index];

        if (offset < previous_end ||
            offset > logical_size_value ||
            value.size() >
                logical_size_value - offset) {
            return {};
        }

        if (!append_padding(
                offset - previous_end)) {
            return {};
        }

        for (std::size_t extent = 0;
             extent < value.extent_count();
             ++extent) {

            if (!output.append(
                    value.extent(extent))) {
                return {};
            }
        }

        previous_end =
            offset + value.size();
    }

    if (previous_end > logical_size_value ||
        !append_padding(
            logical_size_value - previous_end)) {
        return {};
    }

    return output.size() == logical_size_value
        ? output
        : project_generation_segment{};
}

status build_cache_image_view::bind_encoded_mixed(
    std::span<const std::byte> layout_backing,
    const build_cache_encode_borrowed_sections& borrowed) noexcept {

    if (layout_backing.size() < build_cache_image_prefix_size)
        return {status_code::artifact_corrupt};

    const auto prefix =
        layout_backing.first(build_cache_image_prefix_size);

    std::array<
        std::span<const std::byte>,
        build_cache_image_directory_count> sections{};

    std::uint64_t previous_end = first_section_offset;

    for (std::size_t index = 0; index < sections.size(); ++index) {
        const auto* entry =
            prefix.data() +
            directory_offset +
            index * build_cache_image_directory_entry_size;

        const auto record_size = read_u32(entry + 4);
        const auto offset = read_u64(entry + 8);
        const auto count = read_u64(entry + 16);

        std::uint64_t byte_count = 0;
        std::uint64_t end = 0;

        if (!multiply_u64(count, record_size, byte_count) ||
            !add_u64(offset, byte_count, end) ||
            offset != align64(previous_end) ||
            end > layout_backing.size()) {
            return {status_code::artifact_corrupt};
        }

        const auto exact = borrowed.sections[index];

        if (!exact.empty()) {
            if (exact.size() != byte_count)
                return {status_code::artifact_corrupt};
            sections[index] = exact;
        }
        else {
            sections[index] =
                layout_backing.subspan(
                    static_cast<std::size_t>(offset),
                    static_cast<std::size_t>(byte_count));
        }

        previous_end = end;
    }

    if (previous_end != layout_backing.size())
        return {status_code::artifact_corrupt};

    return bind_sectioned(prefix, sections);
}


status build_cache_image_view::bind_encoded_sparse(
    std::span<const std::byte> layout_backing,
    const build_cache_encode_borrowed_sections& borrowed,
    const build_cache_encode_sparse_sections& sparse) noexcept {

    auto result =
        bind_encoded_mixed(
            layout_backing,
            borrowed);
    if (!result.ok())
        return result;

    for (std::size_t index = 0;
         index < sparse.sections.size();
         ++index) {

        const auto& segment =
            sparse.sections[index];

        if (segment.empty())
            continue;

        auto& value = sections[index];

        std::uint64_t byte_count = 0;
        if (!multiply_u64(
                value.count,
                value.record_size,
                byte_count) ||
            byte_count != segment.size()) {
            reset();
            return {status_code::artifact_corrupt};
        }

        value.segment = segment;
        value.data =
            segment.is_contiguous()
            ? segment.contiguous().data()
            : nullptr;
    }

    return {};
}


status build_cache_image_view::bind(
    std::span<const std::byte> image) noexcept {

    reset();

    if (image.size() < first_section_offset)
        return {status_code::artifact_corrupt};

    if (!std::equal(image_magic.begin(), image_magic.end(), image.begin()))
        return {status_code::artifact_corrupt};

    if (read_u32(image.data() + 8) != build_cache_image_format_version)
        return {status_code::rebuild_required};

    const auto flags = read_u32(image.data() + header_flags_offset);
    if (read_u32(image.data() + 12) != endian_marker ||
        read_u32(image.data() + 16) != build_cache_image_header_size ||
        read_u32(image.data() + 20) != build_cache_image_directory_count ||
        read_u32(image.data() + 24) != build_cache_image_directory_entry_size ||
        (flags & ~known_flags) != 0 ||
        read_u64(image.data() + 32) != directory_offset ||
        read_u64(image.data() + 40) != image.size()) {
        return {status_code::artifact_corrupt};
    }

    if ((flags & known_flags) != known_flags)
        return {status_code::artifact_corrupt};

    const auto raw_change_backend =
        read_u32(image.data() + header_change_backend_offset);
    if (read_u32(image.data() + header_change_backend_offset + 4) != 0 ||
        raw_change_backend >
            static_cast<std::uint32_t>(
                source_change_backend::windows_usn)) {
        return {status_code::artifact_corrupt};
    }

    source_change_checkpoint change_checkpoint;
    change_checkpoint.backend =
        static_cast<source_change_backend>(raw_change_backend);
    change_checkpoint.volume_serial =
        read_u64(image.data() + header_change_volume_offset);
    change_checkpoint.journal_id =
        read_u64(image.data() + header_change_journal_offset);
    change_checkpoint.next_usn =
        static_cast<std::int64_t>(
            read_u64(image.data() + header_change_usn_offset));

    if (!change_checkpoint &&
        (change_checkpoint.volume_serial != 0 ||
         change_checkpoint.journal_id != 0 ||
         change_checkpoint.next_usn != 0)) {
        return {status_code::artifact_corrupt};
    }

    if (change_checkpoint &&
        (change_checkpoint.volume_serial == 0 ||
         change_checkpoint.journal_id == 0 ||
         change_checkpoint.next_usn < 0)) {
        return {status_code::artifact_corrupt};
    }

    if (!zero_bytes(
            image.data() + header_reserved_begin,
            header_directory_crc_offset - header_reserved_begin)) {
        return {status_code::artifact_corrupt};
    }

    std::array<std::byte, build_cache_image_header_size> header{};
    std::memcpy(header.data(), image.data(), header.size());
    const auto stored_header_crc =
        read_u64(header.data() + header_crc_offset);
    write_u64(header.data() + header_crc_offset, 0);
    if (persistence_crc64(header) != stored_header_crc)
        return {status_code::artifact_corrupt};

    const auto directory_span =
        image.subspan(directory_offset, directory_bytes);
    if (persistence_crc64(directory_span) !=
        read_u64(image.data() + header_directory_crc_offset)) {
        return {status_code::artifact_corrupt};
    }

    section_view candidate[build_cache_image_directory_count]{};
    std::uint64_t previous_end = first_section_offset;

    for (std::size_t index = 0;
         index < build_cache_image_directory_count;
         ++index) {

        const auto* entry =
            image.data() +
            directory_offset +
            index * build_cache_image_directory_entry_size;

        const auto raw_kind = read_u32(entry);
        const auto kind =
            static_cast<build_cache_image_section>(raw_kind);
        const auto record_size = read_u32(entry + 4);
        const auto offset = read_u64(entry + 8);
        const auto count = read_u64(entry + 16);
        const auto section_crc = read_u64(entry + 24);
        const auto aligned_offset = align64(previous_end);

        if (raw_kind != index + 1 ||
            record_size != expected_record_size(kind) ||
            offset != aligned_offset ||
            (offset & 63u) != 0 ||
            offset > image.size()) {
            return {status_code::artifact_corrupt};
        }

        if (offset > previous_end &&
            !zero_bytes(
                image.data() + static_cast<std::size_t>(previous_end),
                static_cast<std::size_t>(offset - previous_end))) {
            return {status_code::artifact_corrupt};
        }

        std::uint64_t byte_count = 0;
        std::uint64_t end = 0;
        if (!multiply_u64(count, record_size, byte_count) ||
            !add_u64(offset, byte_count, end) ||
            end > image.size()) {
            return {status_code::artifact_corrupt};
        }

        candidate[index] = {
            image.data() + static_cast<std::size_t>(offset),
            count,
            record_size,
            section_crc,
        };
        previous_end = end;
    }

    if (previous_end != image.size())
        return {status_code::artifact_corrupt};

    const auto source_count =
        read_u64(image.data() + header_source_count_offset);
    const auto frontend_count =
        read_u64(image.data() + header_frontend_count_offset);
    const auto source_bytes =
        read_u64(image.data() + header_source_bytes_offset);
    const auto derived_entries =
        read_u64(image.data() + header_derived_entries_offset);

    const auto& source_directory =
        candidate[section_index(build_cache_image_section::source_directory)];
    const auto& source_bytes_section =
        candidate[section_index(build_cache_image_section::source_bytes)];
    const auto& contribution_states =
        candidate[section_index(build_cache_image_section::contribution_states)];
    const auto& intrinsic_refs =
        candidate[section_index(build_cache_image_section::graph_intrinsic_refs)];
    const auto& named_refs =
        candidate[section_index(build_cache_image_section::graph_named_refs)];
    const auto& derived_index =
        candidate[section_index(build_cache_image_section::graph_derived_index)];
    const auto& dependency_versions =
        candidate[section_index(build_cache_image_section::graph_dependency_versions)];
    const auto& reverse_heads =
        candidate[section_index(build_cache_image_section::graph_reverse_dependency_heads)];
    const auto& type_identity_index =
        candidate[section_index(build_cache_image_section::graph_type_identity_index)];
    const auto& object_identity_index =
        candidate[section_index(build_cache_image_section::graph_object_identity_index)];
    const auto& link_target_index =
        candidate[section_index(build_cache_image_section::graph_link_target_index)];
    const auto& source_file_identity_index =
        candidate[section_index(build_cache_image_section::source_file_identity_index)];
    const auto& tracked_directory_identity_index =
        candidate[section_index(build_cache_image_section::tracked_directory_identity_index)];

    const auto valid_historical_index = [](std::uint64_t count) noexcept {
        return count != 0 && (count & (count - 1)) == 0;
    };
    const auto valid_optional_index = [](std::uint64_t count) noexcept {
        return count == 0 || (count & (count - 1)) == 0;
    };

    if (source_count > (std::numeric_limits<std::uint32_t>::max)() ||
        frontend_count > source_count ||
        source_directory.count != source_count ||
        source_bytes_section.count != source_bytes ||
        contribution_states.count != source_count + 1 ||
        intrinsic_refs.count != graph_intrinsic_type_count ||
        named_refs.count != dependency_versions.count + 1 ||
        reverse_heads.count != dependency_versions.count ||
        !valid_historical_index(type_identity_index.count) ||
        !valid_historical_index(object_identity_index.count) ||
        !valid_historical_index(link_target_index.count) ||
        !valid_optional_index(source_file_identity_index.count) ||
        !valid_optional_index(tracked_directory_identity_index.count) ||
        (!change_checkpoint &&
         (source_file_identity_index.count != 0 ||
          tracked_directory_identity_index.count != 0)) ||
        (change_checkpoint &&
         source_count != 0 &&
         source_file_identity_index.count == 0) ||
        derived_entries > derived_index.count ||
        source_count > (std::numeric_limits<std::size_t>::max)() ||
        frontend_count > (std::numeric_limits<std::size_t>::max)() ||
        derived_entries > (std::numeric_limits<std::size_t>::max)()) {
        return {status_code::artifact_corrupt};
    }

    const std::array<std::uint64_t, 7> raw_statistics{
        read_u64(image.data() + header_statistics_offset),
        read_u64(image.data() + header_statistics_offset + 8),
        read_u64(image.data() + header_statistics_offset + 16),
        read_u64(image.data() + header_statistics_offset + 24),
        read_u64(image.data() + header_statistics_offset + 32),
        read_u64(image.data() + header_statistics_offset + 40),
        read_u64(image.data() + header_statistics_offset + 48),
    };

    for (const auto value : raw_statistics) {
        if (value > (std::numeric_limits<std::size_t>::max)())
            return {status_code::artifact_corrupt};
    }

    source_contribution_statistics statistics;
    statistics.sources =
        static_cast<std::size_t>(raw_statistics[0]);
    statistics.type_declarations =
        static_cast<std::size_t>(raw_statistics[1]);
    statistics.members =
        static_cast<std::size_t>(raw_statistics[2]);
    statistics.modifiers =
        static_cast<std::size_t>(raw_statistics[3]);
    statistics.enum_values =
        static_cast<std::size_t>(raw_statistics[4]);
    statistics.objects =
        static_cast<std::size_t>(raw_statistics[5]);
    statistics.links =
        static_cast<std::size_t>(raw_statistics[6]);

    bytes = image;
    std::copy(
        std::begin(candidate),
        std::end(candidate),
        std::begin(sections));

    contribution_statistics_value = statistics;
    source_count_value = static_cast<std::size_t>(source_count);
    frontend_count_value = static_cast<std::size_t>(frontend_count);
    derived_index_entries_value = static_cast<std::size_t>(derived_entries);
    change_checkpoint_value = change_checkpoint;
    frontend_complete_value = true;
    contributions_complete_value = true;

    return {};
}

status build_cache_image_view::bind_sectioned(
    std::span<const std::byte> prefix,
    const std::array<
        std::span<const std::byte>,
        build_cache_image_directory_count>& section_images) noexcept {

    reset();

    if (prefix.size() != first_section_offset)
        return {status_code::artifact_corrupt};

    if (!std::equal(
            image_magic.begin(),
            image_magic.end(),
            prefix.begin())) {
        return {status_code::artifact_corrupt};
    }

    if (read_u32(prefix.data() + 8) !=
        build_cache_image_format_version) {
        return {status_code::rebuild_required};
    }

    const auto flags =
        read_u32(prefix.data() + header_flags_offset);

    const auto logical_size =
        read_u64(prefix.data() + 40);

    if (read_u32(prefix.data() + 12) != endian_marker ||
        read_u32(prefix.data() + 16) !=
            build_cache_image_header_size ||
        read_u32(prefix.data() + 20) !=
            build_cache_image_directory_count ||
        read_u32(prefix.data() + 24) !=
            build_cache_image_directory_entry_size ||
        (flags & ~known_flags) != 0 ||
        read_u64(prefix.data() + 32) != directory_offset ||
        logical_size < first_section_offset) {
        return {status_code::artifact_corrupt};
    }

    if ((flags & known_flags) != known_flags)
        return {status_code::artifact_corrupt};

    const auto raw_change_backend =
        read_u32(
            prefix.data() +
            header_change_backend_offset);

    if (read_u32(
            prefix.data() +
            header_change_backend_offset + 4) != 0 ||
        raw_change_backend >
            static_cast<std::uint32_t>(
                source_change_backend::windows_usn)) {
        return {status_code::artifact_corrupt};
    }

    source_change_checkpoint change_checkpoint;
    change_checkpoint.backend =
        static_cast<source_change_backend>(
            raw_change_backend);
    change_checkpoint.volume_serial =
        read_u64(
            prefix.data() +
            header_change_volume_offset);
    change_checkpoint.journal_id =
        read_u64(
            prefix.data() +
            header_change_journal_offset);
    change_checkpoint.next_usn =
        static_cast<std::int64_t>(
            read_u64(
                prefix.data() +
                header_change_usn_offset));

    if (!change_checkpoint &&
        (change_checkpoint.volume_serial != 0 ||
         change_checkpoint.journal_id != 0 ||
         change_checkpoint.next_usn != 0)) {
        return {status_code::artifact_corrupt};
    }

    if (change_checkpoint &&
        (change_checkpoint.volume_serial == 0 ||
         change_checkpoint.journal_id == 0 ||
         change_checkpoint.next_usn < 0)) {
        return {status_code::artifact_corrupt};
    }

    if (!zero_bytes(
            prefix.data() +
                header_reserved_begin,
            header_directory_crc_offset -
                header_reserved_begin)) {
        return {status_code::artifact_corrupt};
    }

    std::array<
        std::byte,
        build_cache_image_header_size>
        header{};

    std::memcpy(
        header.data(),
        prefix.data(),
        header.size());

    const auto stored_header_crc =
        read_u64(
            header.data() +
            header_crc_offset);

    write_u64(
        header.data() +
            header_crc_offset,
        0);

    if (persistence_crc64(header) !=
        stored_header_crc) {
        return {status_code::artifact_corrupt};
    }

    const auto directory_span =
        prefix.subspan(
            directory_offset,
            directory_bytes);

    if (persistence_crc64(directory_span) !=
        read_u64(
            prefix.data() +
            header_directory_crc_offset)) {
        return {status_code::artifact_corrupt};
    }

    if (directory_offset + directory_bytes <
            first_section_offset &&
        !zero_bytes(
            prefix.data() +
                directory_offset +
                directory_bytes,
            first_section_offset -
                directory_offset -
                directory_bytes)) {
        return {status_code::artifact_corrupt};
    }

    section_view
        candidate[
            build_cache_image_directory_count]{};

    std::uint64_t previous_end =
        first_section_offset;

    for (std::size_t index = 0;
         index <
            build_cache_image_directory_count;
         ++index) {

        const auto* entry =
            prefix.data() +
            directory_offset +
            index *
                build_cache_image_directory_entry_size;

        const auto raw_kind =
            read_u32(entry);

        const auto kind =
            static_cast<
                build_cache_image_section>(
                    raw_kind);

        const auto record_size =
            read_u32(entry + 4);
        const auto offset =
            read_u64(entry + 8);
        const auto count =
            read_u64(entry + 16);
        const auto section_crc =
            read_u64(entry + 24);

        const auto aligned_offset =
            align64(previous_end);

        if (raw_kind != index + 1 ||
            record_size !=
                expected_record_size(kind) ||
            offset != aligned_offset ||
            (offset & 63u) != 0 ||
            offset > logical_size) {
            return {
                status_code::artifact_corrupt};
        }

        std::uint64_t byte_count = 0;
        std::uint64_t end = 0;

        if (!multiply_u64(
                count,
                record_size,
                byte_count) ||
            !add_u64(
                offset,
                byte_count,
                end) ||
            end > logical_size ||
            byte_count >
                (std::numeric_limits<
                    std::size_t>::max)()) {
            return {
                status_code::artifact_corrupt};
        }

        if (section_images[index].size() !=
            static_cast<std::size_t>(
                byte_count)) {
            return {
                status_code::artifact_corrupt};
        }

        candidate[index] = {
            section_images[index].data(),
            count,
            record_size,
            section_crc,
        };

        previous_end = end;
    }

    if (previous_end != logical_size)
        return {status_code::artifact_corrupt};

    const auto source_count =
        read_u64(
            prefix.data() +
            header_source_count_offset);

    const auto frontend_count =
        read_u64(
            prefix.data() +
            header_frontend_count_offset);

    const auto source_bytes =
        read_u64(
            prefix.data() +
            header_source_bytes_offset);

    const auto derived_entries =
        read_u64(
            prefix.data() +
            header_derived_entries_offset);

    const auto& source_directory =
        candidate[
            section_index(
                build_cache_image_section::
                    source_directory)];

    const auto& source_bytes_section =
        candidate[
            section_index(
                build_cache_image_section::
                    source_bytes)];

    const auto& contribution_states =
        candidate[
            section_index(
                build_cache_image_section::
                    contribution_states)];

    const auto& intrinsic_refs =
        candidate[
            section_index(
                build_cache_image_section::
                    graph_intrinsic_refs)];

    const auto& named_refs =
        candidate[
            section_index(
                build_cache_image_section::
                    graph_named_refs)];

    const auto& derived_index =
        candidate[
            section_index(
                build_cache_image_section::
                    graph_derived_index)];

    const auto& dependency_versions =
        candidate[
            section_index(
                build_cache_image_section::
                    graph_dependency_versions)];

    const auto& reverse_heads =
        candidate[
            section_index(
                build_cache_image_section::
                    graph_reverse_dependency_heads)];

    const auto& type_identity_index =
        candidate[
            section_index(
                build_cache_image_section::
                    graph_type_identity_index)];

    const auto& object_identity_index =
        candidate[
            section_index(
                build_cache_image_section::
                    graph_object_identity_index)];

    const auto& link_target_index =
        candidate[
            section_index(
                build_cache_image_section::
                    graph_link_target_index)];

    const auto& source_file_identity_index =
        candidate[
            section_index(
                build_cache_image_section::
                    source_file_identity_index)];

    const auto&
        tracked_directory_identity_index =
        candidate[
            section_index(
                build_cache_image_section::
                    tracked_directory_identity_index)];

    const auto valid_historical_index =
        [](std::uint64_t count) noexcept {
            return count != 0 &&
                (count & (count - 1)) == 0;
        };

    const auto valid_optional_index =
        [](std::uint64_t count) noexcept {
            return count == 0 ||
                (count & (count - 1)) == 0;
        };

    if (source_count >
            (std::numeric_limits<
                std::uint32_t>::max)() ||
        frontend_count > source_count ||
        source_directory.count != source_count ||
        source_bytes_section.count !=
            source_bytes ||
        contribution_states.count !=
            source_count + 1 ||
        intrinsic_refs.count !=
            graph_intrinsic_type_count ||
        named_refs.count !=
            dependency_versions.count + 1 ||
        reverse_heads.count !=
            dependency_versions.count ||
        !valid_historical_index(
            type_identity_index.count) ||
        !valid_historical_index(
            object_identity_index.count) ||
        !valid_historical_index(
            link_target_index.count) ||
        !valid_optional_index(
            source_file_identity_index.count) ||
        !valid_optional_index(
            tracked_directory_identity_index.count) ||
        (!change_checkpoint &&
         (source_file_identity_index.count != 0 ||
          tracked_directory_identity_index.count != 0)) ||
        (change_checkpoint &&
         source_count != 0 &&
         source_file_identity_index.count == 0) ||
        derived_entries >
            derived_index.count ||
        source_count >
            (std::numeric_limits<
                std::size_t>::max)() ||
        frontend_count >
            (std::numeric_limits<
                std::size_t>::max)() ||
        derived_entries >
            (std::numeric_limits<
                std::size_t>::max)()) {
        return {status_code::artifact_corrupt};
    }

    const std::array<std::uint64_t, 7>
        raw_statistics{
            read_u64(
                prefix.data() +
                header_statistics_offset),
            read_u64(
                prefix.data() +
                header_statistics_offset + 8),
            read_u64(
                prefix.data() +
                header_statistics_offset + 16),
            read_u64(
                prefix.data() +
                header_statistics_offset + 24),
            read_u64(
                prefix.data() +
                header_statistics_offset + 32),
            read_u64(
                prefix.data() +
                header_statistics_offset + 40),
            read_u64(
                prefix.data() +
                header_statistics_offset + 48),
        };

    for (const auto value : raw_statistics) {
        if (value >
            (std::numeric_limits<
                std::size_t>::max)()) {
            return {
                status_code::artifact_corrupt};
        }
    }

    source_contribution_statistics statistics;
    statistics.sources =
        static_cast<std::size_t>(
            raw_statistics[0]);
    statistics.type_declarations =
        static_cast<std::size_t>(
            raw_statistics[1]);
    statistics.members =
        static_cast<std::size_t>(
            raw_statistics[2]);
    statistics.modifiers =
        static_cast<std::size_t>(
            raw_statistics[3]);
    statistics.enum_values =
        static_cast<std::size_t>(
            raw_statistics[4]);
    statistics.objects =
        static_cast<std::size_t>(
            raw_statistics[5]);
    statistics.links =
        static_cast<std::size_t>(
            raw_statistics[6]);

    bytes = prefix;

    std::copy(
        std::begin(candidate),
        std::end(candidate),
        std::begin(sections));

    contribution_statistics_value =
        statistics;
    source_count_value =
        static_cast<std::size_t>(
            source_count);
    frontend_count_value =
        static_cast<std::size_t>(
            frontend_count);
    derived_index_entries_value =
        static_cast<std::size_t>(
            derived_entries);
    change_checkpoint_value =
        change_checkpoint;
    frontend_complete_value = true;
    contributions_complete_value = true;

    return {};
}

status build_cache_image_view::bind_sectioned(
    std::span<const std::byte> prefix,
    const std::array<
        project_generation_segment,
        build_cache_image_directory_count>& section_images) noexcept {

    reset();

    if (prefix.size() != first_section_offset)
        return {status_code::artifact_corrupt};

    if (!std::equal(
            image_magic.begin(),
            image_magic.end(),
            prefix.begin())) {
        return {status_code::artifact_corrupt};
    }

    if (read_u32(prefix.data() + 8) !=
        build_cache_image_format_version) {
        return {status_code::rebuild_required};
    }

    const auto flags =
        read_u32(prefix.data() + header_flags_offset);

    const auto logical_size =
        read_u64(prefix.data() + 40);

    if (read_u32(prefix.data() + 12) != endian_marker ||
        read_u32(prefix.data() + 16) !=
            build_cache_image_header_size ||
        read_u32(prefix.data() + 20) !=
            build_cache_image_directory_count ||
        read_u32(prefix.data() + 24) !=
            build_cache_image_directory_entry_size ||
        (flags & ~known_flags) != 0 ||
        read_u64(prefix.data() + 32) != directory_offset ||
        logical_size < first_section_offset) {
        return {status_code::artifact_corrupt};
    }

    if ((flags & known_flags) != known_flags)
        return {status_code::artifact_corrupt};

    const auto raw_change_backend =
        read_u32(
            prefix.data() +
            header_change_backend_offset);

    if (read_u32(
            prefix.data() +
            header_change_backend_offset + 4) != 0 ||
        raw_change_backend >
            static_cast<std::uint32_t>(
                source_change_backend::windows_usn)) {
        return {status_code::artifact_corrupt};
    }

    source_change_checkpoint change_checkpoint;
    change_checkpoint.backend =
        static_cast<source_change_backend>(
            raw_change_backend);
    change_checkpoint.volume_serial =
        read_u64(
            prefix.data() +
            header_change_volume_offset);
    change_checkpoint.journal_id =
        read_u64(
            prefix.data() +
            header_change_journal_offset);
    change_checkpoint.next_usn =
        static_cast<std::int64_t>(
            read_u64(
                prefix.data() +
                header_change_usn_offset));

    if (!change_checkpoint &&
        (change_checkpoint.volume_serial != 0 ||
         change_checkpoint.journal_id != 0 ||
         change_checkpoint.next_usn != 0)) {
        return {status_code::artifact_corrupt};
    }

    if (change_checkpoint &&
        (change_checkpoint.volume_serial == 0 ||
         change_checkpoint.journal_id == 0 ||
         change_checkpoint.next_usn < 0)) {
        return {status_code::artifact_corrupt};
    }

    if (!zero_bytes(
            prefix.data() +
                header_reserved_begin,
            header_directory_crc_offset -
                header_reserved_begin)) {
        return {status_code::artifact_corrupt};
    }

    std::array<
        std::byte,
        build_cache_image_header_size>
        header{};

    std::memcpy(
        header.data(),
        prefix.data(),
        header.size());

    const auto stored_header_crc =
        read_u64(
            header.data() +
            header_crc_offset);

    write_u64(
        header.data() +
            header_crc_offset,
        0);

    if (persistence_crc64(header) !=
        stored_header_crc) {
        return {status_code::artifact_corrupt};
    }

    const auto directory_span =
        prefix.subspan(
            directory_offset,
            directory_bytes);

    if (persistence_crc64(directory_span) !=
        read_u64(
            prefix.data() +
            header_directory_crc_offset)) {
        return {status_code::artifact_corrupt};
    }

    if (directory_offset + directory_bytes <
            first_section_offset &&
        !zero_bytes(
            prefix.data() +
                directory_offset +
                directory_bytes,
            first_section_offset -
                directory_offset -
                directory_bytes)) {
        return {status_code::artifact_corrupt};
    }

    section_view
        candidate[
            build_cache_image_directory_count]{};

    std::uint64_t previous_end =
        first_section_offset;

    for (std::size_t index = 0;
         index <
            build_cache_image_directory_count;
         ++index) {

        const auto* entry =
            prefix.data() +
            directory_offset +
            index *
                build_cache_image_directory_entry_size;

        const auto raw_kind =
            read_u32(entry);

        const auto kind =
            static_cast<
                build_cache_image_section>(
                    raw_kind);

        const auto record_size =
            read_u32(entry + 4);
        const auto offset =
            read_u64(entry + 8);
        const auto count =
            read_u64(entry + 16);
        const auto section_crc =
            read_u64(entry + 24);

        const auto aligned_offset =
            align64(previous_end);

        if (raw_kind != index + 1 ||
            record_size !=
                expected_record_size(kind) ||
            offset != aligned_offset ||
            (offset & 63u) != 0 ||
            offset > logical_size) {
            return {
                status_code::artifact_corrupt};
        }

        std::uint64_t byte_count = 0;
        std::uint64_t end = 0;

        if (!multiply_u64(
                count,
                record_size,
                byte_count) ||
            !add_u64(
                offset,
                byte_count,
                end) ||
            end > logical_size ||
            byte_count >
                (std::numeric_limits<
                    std::size_t>::max)()) {
            return {
                status_code::artifact_corrupt};
        }

        if (section_images[index].size() !=
            static_cast<std::size_t>(
                byte_count)) {
            return {
                status_code::artifact_corrupt};
        }

        const auto contiguous =
            section_images[index].contiguous();

        candidate[index] = {
            contiguous.data(),
            count,
            record_size,
            section_crc,
            section_images[index],
        };

        previous_end = end;
    }

    if (previous_end != logical_size)
        return {status_code::artifact_corrupt};

    const auto source_count =
        read_u64(
            prefix.data() +
            header_source_count_offset);

    const auto frontend_count =
        read_u64(
            prefix.data() +
            header_frontend_count_offset);

    const auto source_bytes =
        read_u64(
            prefix.data() +
            header_source_bytes_offset);

    const auto derived_entries =
        read_u64(
            prefix.data() +
            header_derived_entries_offset);

    const auto& source_directory =
        candidate[
            section_index(
                build_cache_image_section::
                    source_directory)];

    const auto& source_bytes_section =
        candidate[
            section_index(
                build_cache_image_section::
                    source_bytes)];

    const auto& contribution_states =
        candidate[
            section_index(
                build_cache_image_section::
                    contribution_states)];

    const auto& intrinsic_refs =
        candidate[
            section_index(
                build_cache_image_section::
                    graph_intrinsic_refs)];

    const auto& named_refs =
        candidate[
            section_index(
                build_cache_image_section::
                    graph_named_refs)];

    const auto& derived_index =
        candidate[
            section_index(
                build_cache_image_section::
                    graph_derived_index)];

    const auto& dependency_versions =
        candidate[
            section_index(
                build_cache_image_section::
                    graph_dependency_versions)];

    const auto& reverse_heads =
        candidate[
            section_index(
                build_cache_image_section::
                    graph_reverse_dependency_heads)];

    const auto& type_identity_index =
        candidate[
            section_index(
                build_cache_image_section::
                    graph_type_identity_index)];

    const auto& object_identity_index =
        candidate[
            section_index(
                build_cache_image_section::
                    graph_object_identity_index)];

    const auto& link_target_index =
        candidate[
            section_index(
                build_cache_image_section::
                    graph_link_target_index)];

    const auto& source_file_identity_index =
        candidate[
            section_index(
                build_cache_image_section::
                    source_file_identity_index)];

    const auto&
        tracked_directory_identity_index =
        candidate[
            section_index(
                build_cache_image_section::
                    tracked_directory_identity_index)];

    const auto valid_historical_index =
        [](std::uint64_t count) noexcept {
            return count != 0 &&
                (count & (count - 1)) == 0;
        };

    const auto valid_optional_index =
        [](std::uint64_t count) noexcept {
            return count == 0 ||
                (count & (count - 1)) == 0;
        };

    if (source_count >
            (std::numeric_limits<
                std::uint32_t>::max)() ||
        frontend_count > source_count ||
        source_directory.count != source_count ||
        source_bytes_section.count !=
            source_bytes ||
        contribution_states.count !=
            source_count + 1 ||
        intrinsic_refs.count !=
            graph_intrinsic_type_count ||
        named_refs.count !=
            dependency_versions.count + 1 ||
        reverse_heads.count !=
            dependency_versions.count ||
        !valid_historical_index(
            type_identity_index.count) ||
        !valid_historical_index(
            object_identity_index.count) ||
        !valid_historical_index(
            link_target_index.count) ||
        !valid_optional_index(
            source_file_identity_index.count) ||
        !valid_optional_index(
            tracked_directory_identity_index.count) ||
        (!change_checkpoint &&
         (source_file_identity_index.count != 0 ||
          tracked_directory_identity_index.count != 0)) ||
        (change_checkpoint &&
         source_count != 0 &&
         source_file_identity_index.count == 0) ||
        derived_entries >
            derived_index.count ||
        source_count >
            (std::numeric_limits<
                std::size_t>::max)() ||
        frontend_count >
            (std::numeric_limits<
                std::size_t>::max)() ||
        derived_entries >
            (std::numeric_limits<
                std::size_t>::max)()) {
        return {status_code::artifact_corrupt};
    }

    const std::array<std::uint64_t, 7>
        raw_statistics{
            read_u64(
                prefix.data() +
                header_statistics_offset),
            read_u64(
                prefix.data() +
                header_statistics_offset + 8),
            read_u64(
                prefix.data() +
                header_statistics_offset + 16),
            read_u64(
                prefix.data() +
                header_statistics_offset + 24),
            read_u64(
                prefix.data() +
                header_statistics_offset + 32),
            read_u64(
                prefix.data() +
                header_statistics_offset + 40),
            read_u64(
                prefix.data() +
                header_statistics_offset + 48),
        };

    for (const auto value : raw_statistics) {
        if (value >
            (std::numeric_limits<
                std::size_t>::max)()) {
            return {
                status_code::artifact_corrupt};
        }
    }

    source_contribution_statistics statistics;
    statistics.sources =
        static_cast<std::size_t>(
            raw_statistics[0]);
    statistics.type_declarations =
        static_cast<std::size_t>(
            raw_statistics[1]);
    statistics.members =
        static_cast<std::size_t>(
            raw_statistics[2]);
    statistics.modifiers =
        static_cast<std::size_t>(
            raw_statistics[3]);
    statistics.enum_values =
        static_cast<std::size_t>(
            raw_statistics[4]);
    statistics.objects =
        static_cast<std::size_t>(
            raw_statistics[5]);
    statistics.links =
        static_cast<std::size_t>(
            raw_statistics[6]);

    bytes = prefix;

    std::copy(
        std::begin(candidate),
        std::end(candidate),
        std::begin(sections));

    contribution_statistics_value =
        statistics;
    source_count_value =
        static_cast<std::size_t>(
            source_count);
    frontend_count_value =
        static_cast<std::size_t>(
            frontend_count);
    derived_index_entries_value =
        static_cast<std::size_t>(
            derived_entries);
    change_checkpoint_value =
        change_checkpoint;
    frontend_complete_value = true;
    contributions_complete_value = true;

    return {};
}

string_id build_cache_image_view::string_from_raw(
    std::uint32_t value) const noexcept {

    return value == 0 ? string_id{} : string_id{value};
}

identity_ref build_cache_image_view::identity_from_raw(
    std::uint32_t value) const noexcept {

    if (value == 0)
        return {};

    const auto slot = value & identity_ref::slot_mask;
    const auto raw_kind = value >> identity_ref::kind_shift;
    if (slot == 0 ||
        !valid_identity_kind(static_cast<std::uint8_t>(raw_kind))) {
        return {};
    }

    return identity_ref::make(
        slot,
        static_cast<identity_kind>(raw_kind));
}

TypeRef build_cache_image_view::type_ref_from_raw(
    std::uint32_t value) const noexcept {

    return value == 0 ? TypeRef{} : TypeRef{value};
}

status build_cache_image_view::source(
    source_id id,
    build_cache_source_record& output) const noexcept {

    output = {};
    if (!valid_source(id))
        return {status_code::not_found};

    const auto& values =
        section(build_cache_image_section::source_directory);
    const auto record_offset =
        static_cast<std::size_t>(id.value() - 1) *
            source_directory_record_size;

    const std::byte* record = nullptr;

    if (values.segment.empty()) {
        record =
            values.data +
            record_offset;
    }
    else {
        std::size_t cursor = 0;

        for (std::size_t extent_index = 0;
             extent_index < values.segment.extent_count();
             ++extent_index) {

            const auto extent =
                values.segment.extent(
                    extent_index);

            if (record_offset >= cursor &&
                record_offset - cursor <=
                    extent.size() &&
                source_directory_record_size <=
                    extent.size() -
                        (record_offset - cursor)) {

                record =
                    extent.data() +
                    (record_offset - cursor);
                break;
            }

            cursor += extent.size();
        }

        if (record == nullptr)
            return {status_code::artifact_corrupt};
    }

    const auto raw_source = read_u32(record);
    const auto flags = read_u32(record + 4);
    if (raw_source != id.value() ||
        (flags & ~source_known_flags) != 0) {
        return {status_code::artifact_corrupt};
    }

    output.source = source_id{raw_source};
    output.snapshot_present = (flags & source_flag_snapshot) != 0;
    output.frontend_present = (flags & source_flag_frontend) != 0;
    output.text_offset = read_u64(record + 8);
    output.text_length = read_u32(record + 16);

    if (read_u32(record + 20) != 0)
        return {status_code::artifact_corrupt};

    output.local_types = read_cache_range(record + 24);
    output.type_slots = read_cache_range(record + 32);
    output.object_slots = read_cache_range(record + 40);
    output.member_slots = read_cache_range(record + 48);
    return {};
}

source_id build_cache_image_view::find_source_file(
    std::uint64_t file_reference) const noexcept {

    if (file_reference == 0)
        return {};

    const auto& values =
        section(build_cache_image_section::source_file_identity_index);
    if (values.count == 0 ||
        (values.count & (values.count - 1)) != 0) {
        return {};
    }

    const auto mask =
        static_cast<std::size_t>(values.count - 1);
    auto position =
        static_cast<std::size_t>(mix64(file_reference)) & mask;

    for (std::size_t probe = 0; probe < values.count; ++probe) {
        const auto* slot =
            values.data +
            position * source_file_identity_index_record_size;
        const auto candidate = read_u64(slot);

        if (candidate == 0)
            return {};

        if (candidate == file_reference) {
            const source_id source{read_u32(slot + 8)};
            if (source &&
                static_cast<std::size_t>(source.value()) <=
                    source_count_value &&
                read_u32(slot + 12) == 0) {
                return source;
            }
            return {};
        }

        position = (position + 1) & mask;
    }

    return {};
}

std::uint32_t build_cache_image_view::directory_watch_flags(
    std::uint64_t file_reference) const noexcept {

    if (file_reference == 0)
        return 0;

    const auto& values =
        section(build_cache_image_section::tracked_directory_identity_index);
    if (values.count == 0 ||
        (values.count & (values.count - 1)) != 0) {
        return 0;
    }

    const auto mask =
        static_cast<std::size_t>(values.count - 1);
    auto position =
        static_cast<std::size_t>(mix64(file_reference)) & mask;

    for (std::size_t probe = 0; probe < values.count; ++probe) {
        const auto* slot =
            values.data +
            position *
                tracked_directory_identity_index_record_size;

        const auto candidate =
            read_u64(slot);

        if (candidate == 0)
            return 0;

        if (candidate == file_reference) {
            const auto flags = read_u32(slot + 8);
            const auto reserved = read_u32(slot + 12);

            if (reserved != 0 ||
                flags == 0 ||
                (flags & ~source_change_directory_watch_known) != 0) {
                return 0;
            }

            return flags;
        }

        position = (position + 1) & mask;
    }

    return 0;
}

std::string_view build_cache_image_view::source_text(
    source_id id) const noexcept {

    build_cache_source_record record;
    if (!source(id, record).ok() || !record.snapshot_present)
        return {};

    const auto& values = section(build_cache_image_section::source_bytes);
    if (record.text_offset > values.count ||
        record.text_length > values.count - record.text_offset) {
        return {};
    }

    const auto logical_offset =
        static_cast<std::size_t>(
            record.text_offset);
    const auto logical_size =
        static_cast<std::size_t>(
            record.text_length);

    if (values.segment.empty()) {
        return {
            reinterpret_cast<const char*>(
                values.data + logical_offset),
            logical_size,
        };
    }

    std::size_t cursor = 0;

    for (std::size_t extent_index = 0;
         extent_index < values.segment.extent_count();
         ++extent_index) {

        const auto extent =
            values.segment.extent(
                extent_index);

        if (logical_offset >= cursor &&
            logical_offset - cursor <=
                extent.size() &&
            logical_size <=
                extent.size() -
                    (logical_offset - cursor)) {

            return {
                reinterpret_cast<const char*>(
                    extent.data() +
                    (logical_offset - cursor)),
                logical_size,
            };
        }

        cursor += extent.size();
    }

    return {};
}

status build_cache_image_view::frontend_block_view(
    source_id source_value,
    source_interface_data_view& output) const noexcept {

    output = {};

    build_cache_source_record record;
    auto result =
        source(
            source_value,
            record);

    if (!result.ok())
        return result;

    if (!record.frontend_present)
        return {status_code::not_found};

    const auto native_section_span =
        [&](build_cache_image_section kind,
            build_cache_range range,
            std::uint32_t record_size,
            auto& target) noexcept {

            const auto& values =
                section(kind);

            if (!values.segment.empty()) {
                return native_frontend_span(
                    values.segment,
                    range,
                    record_size,
                    target);
            }

            return native_frontend_span(
                section_bytes(kind),
                range,
                record_size,
                target);
        };

    std::span<const identity_ref> local_types;
    std::span<const source_interface_type_slot> type_slots;
    std::span<const source_interface_object_slot> object_slots;
    std::span<const source_interface_member_slot> member_slots;

    result =
        native_section_span(
            build_cache_image_section::frontend_local_types,
            record.local_types,
            frontend_local_type_record_size,
            local_types);
    if (!result.ok())
        return result;

    result =
        native_section_span(
            build_cache_image_section::frontend_type_slots,
            record.type_slots,
            frontend_type_slot_record_size,
            type_slots);
    if (!result.ok())
        return result;

    result =
        native_section_span(
            build_cache_image_section::frontend_object_slots,
            record.object_slots,
            frontend_object_slot_record_size,
            object_slots);
    if (!result.ok())
        return result;

    result =
        native_section_span(
            build_cache_image_section::frontend_member_slots,
            record.member_slots,
            frontend_member_slot_record_size,
            member_slots);
    if (!result.ok())
        return result;

    output = {
        local_types,
        type_slots,
        object_slots,
        member_slots,
    };

    return {};
}


const std::byte* build_cache_image_view::section_record_data(
    build_cache_image_section kind,
    std::uint64_t index) const noexcept {

    const auto& values = section(kind);

    if (values.record_size == 0 ||
        index >= values.count) {
        return nullptr;
    }

    std::uint64_t raw_offset = 0;
    if (!multiply_u64(
            index,
            values.record_size,
            raw_offset) ||
        raw_offset >
            (std::numeric_limits<std::size_t>::max)()) {
        return nullptr;
    }

    const auto offset =
        static_cast<std::size_t>(raw_offset);
    const auto record_size =
        static_cast<std::size_t>(
            values.record_size);

    if (values.segment.empty()) {
        if (values.data == nullptr)
            return nullptr;

        return values.data + offset;
    }

    std::size_t cursor = 0;

    for (std::size_t extent_index = 0;
         extent_index < values.segment.extent_count();
         ++extent_index) {

        const auto extent =
            values.segment.extent(extent_index);

        if (offset >= cursor) {
            const auto local_offset =
                offset - cursor;

            if (local_offset <= extent.size() &&
                record_size <=
                    extent.size() - local_offset) {
                return extent.data() +
                    local_offset;
            }
        }

        if (extent.size() >
            (std::numeric_limits<std::size_t>::max)() -
                cursor) {
            return nullptr;
        }

        cursor += extent.size();
    }

    return nullptr;
}


status build_cache_image_view::frontend_local_type(
    source_id source_value,
    std::size_t index,
    identity_ref& output) const noexcept {

    output = {};
    build_cache_source_record record;
    if (!source(source_value, record).ok() ||
        !record.frontend_present ||
        index >= record.local_types.count) {
        return {status_code::not_found};
    }

    const auto& values =
        section(build_cache_image_section::frontend_local_types);
    const auto absolute =
        static_cast<std::uint64_t>(record.local_types.begin) + index;
    if (absolute >= values.count)
        return {status_code::artifact_corrupt};

    const auto* value =
        section_record_data(
            build_cache_image_section::frontend_local_types,
            absolute);
    if (value == nullptr)
        return {status_code::artifact_corrupt};

    output = identity_from_raw(read_u32(value));
    return output ? status{} : status{status_code::artifact_corrupt};
}

status build_cache_image_view::frontend_type_slot(
    source_id source_value,
    std::size_t index,
    source_interface_type_slot& output) const noexcept {

    output = {};
    build_cache_source_record record;
    if (!source(source_value, record).ok() ||
        !record.frontend_present ||
        index >= record.type_slots.count) {
        return {status_code::not_found};
    }

    const auto& values =
        section(build_cache_image_section::frontend_type_slots);
    const auto absolute =
        static_cast<std::uint64_t>(record.type_slots.begin) + index;
    if (absolute >= values.count)
        return {status_code::artifact_corrupt};

    const auto* value =
        section_record_data(
            build_cache_image_section::frontend_type_slots,
            absolute);
    if (value == nullptr)
        return {status_code::artifact_corrupt};

    const auto raw_parent = read_u32(value);
    const auto raw_name = read_u32(value + 4);
    const auto raw_identity = read_u32(value + 8);

    if (raw_identity == 0) {
        if (raw_parent != 0 || raw_name != 0)
            return {status_code::artifact_corrupt};
        return {};
    }

    output.parent = identity_from_raw(raw_parent);
    output.name = string_from_raw(raw_name);
    output.identity = identity_from_raw(raw_identity);

    return output.parent && output.name && output.identity
        ? status{}
        : status{status_code::artifact_corrupt};
}

status build_cache_image_view::frontend_object_slot(
    source_id source_value,
    std::size_t index,
    source_interface_object_slot& output) const noexcept {

    output = {};
    build_cache_source_record record;
    if (!source(source_value, record).ok() ||
        !record.frontend_present ||
        index >= record.object_slots.count) {
        return {status_code::not_found};
    }

    const auto& values =
        section(build_cache_image_section::frontend_object_slots);
    const auto absolute =
        static_cast<std::uint64_t>(record.object_slots.begin) + index;
    if (absolute >= values.count)
        return {status_code::artifact_corrupt};

    const auto* value =
        section_record_data(
            build_cache_image_section::frontend_object_slots,
            absolute);
    if (value == nullptr)
        return {status_code::artifact_corrupt};

    const auto raw_parent = read_u32(value);
    const auto raw_name = read_u32(value + 4);
    const auto raw_identity = read_u32(value + 8);
    const auto raw_named_type = read_u32(value + 12);

    if (raw_identity == 0) {
        if (raw_parent != 0 || raw_name != 0 || raw_named_type != 0)
            return {status_code::artifact_corrupt};
        return {};
    }

    output.parent = identity_from_raw(raw_parent);
    output.name = string_from_raw(raw_name);
    output.identity = identity_from_raw(raw_identity);
    output.named_type = identity_from_raw(raw_named_type);

    return output.parent && output.name && output.identity
        ? status{}
        : status{status_code::artifact_corrupt};
}

status build_cache_image_view::frontend_member_slot(
    source_id source_value,
    std::size_t index,
    source_interface_member_slot& output) const noexcept {

    output = {};
    build_cache_source_record record;
    if (!source(source_value, record).ok() ||
        !record.frontend_present ||
        index >= record.member_slots.count) {
        return {status_code::not_found};
    }

    const auto& values =
        section(build_cache_image_section::frontend_member_slots);
    const auto absolute =
        static_cast<std::uint64_t>(record.member_slots.begin) + index;
    if (absolute >= values.count)
        return {status_code::artifact_corrupt};

    const auto* value =
        section_record_data(
            build_cache_image_section::frontend_member_slots,
            absolute);
    if (value == nullptr)
        return {status_code::artifact_corrupt};

    const auto raw_type = read_u32(value);
    const auto raw_name = read_u32(value + 4);
    const auto raw_member = read_u32(value + 8);

    if (raw_type == 0) {
        if (raw_name != 0 || raw_member !=
            (std::numeric_limits<std::uint32_t>::max)()) {
            return {status_code::artifact_corrupt};
        }
        return {};
    }

    output.type = identity_from_raw(raw_type);
    output.name = string_from_raw(raw_name);
    output.index = member_index::from_zero_based(raw_member);

    return output.type && output.name && output.index
        ? status{}
        : status{status_code::artifact_corrupt};
}

status build_cache_image_view::contribution_state(
    source_id source_value,
    source_contribution_state& output) const noexcept {

    output = {};
    if (!valid_source(source_value))
        return {status_code::not_found};

    const auto& values =
        section(build_cache_image_section::contribution_states);

    const auto* record =
        values.data +
        static_cast<std::size_t>(source_value.value()) *
            contribution_state_record_size;

    const auto raw_source = read_u32(record);
    if (read_u32(record + 4) != 0)
        return {status_code::artifact_corrupt};
    if (raw_source == 0)
        return {status_code::not_found};
    if (raw_source != source_value.value())
        return {status_code::artifact_corrupt};

    output.source = source_id{raw_source};
    output.types = read_range(record + 8);
    output.members = read_range(record + 16);
    output.modifiers = read_range(record + 24);
    output.enum_values = read_range(record + 32);
    output.objects = read_range(record + 40);
    output.links = read_range(record + 48);
    return {};
}

std::size_t build_cache_image_view::contribution_type_count() const noexcept {
    return static_cast<std::size_t>(
        section(build_cache_image_section::contribution_types).count);
}

std::size_t build_cache_image_view::contribution_member_count() const noexcept {
    return static_cast<std::size_t>(
        section(build_cache_image_section::contribution_members).count);
}

std::size_t build_cache_image_view::contribution_modifier_count() const noexcept {
    return static_cast<std::size_t>(
        section(build_cache_image_section::contribution_modifiers).count);
}

std::size_t build_cache_image_view::contribution_enum_value_count() const noexcept {
    return static_cast<std::size_t>(
        section(build_cache_image_section::contribution_enum_values).count);
}

std::size_t build_cache_image_view::contribution_object_count() const noexcept {
    return static_cast<std::size_t>(
        section(build_cache_image_section::contribution_objects).count);
}

std::size_t build_cache_image_view::contribution_link_count() const noexcept {
    return static_cast<std::size_t>(
        section(build_cache_image_section::contribution_links).count);
}

std::size_t build_cache_image_view::construction_slot_count() const noexcept {
    const auto count =
        section(build_cache_image_section::construction_states).count;
    return count == 0 ? 0 : static_cast<std::size_t>(count - 1);
}

status build_cache_image_view::contribution_type(
    std::size_t index,
    source_contribution_type& output) const noexcept {

    output = {};
    const auto& values =
        section(build_cache_image_section::contribution_types);
    if (index >= values.count)
        return {status_code::not_found};

    const auto* record =
        values.data + index * contribution_type_record_size;

    const auto raw_underlying = static_cast<std::uint8_t>(record[12]);
    const auto raw_record_kind = static_cast<std::uint8_t>(record[13]);
    const auto raw_kind = static_cast<std::uint8_t>(record[14]);

    if (!valid_intrinsic(raw_underlying) ||
        !valid_record_kind(raw_record_kind) ||
        !valid_contribution_kind(raw_kind)) {
        return {status_code::artifact_corrupt};
    }

    output.identity = identity_from_raw(read_u32(record));
    output.definition_items = read_range(record + 4);
    output.explicit_underlying =
        static_cast<intrinsic_type>(raw_underlying);
    output.record_kind =
        static_cast<source_record_kind>(raw_record_kind);
    output.kind =
        static_cast<source_contribution_type_kind>(raw_kind);
    output.flags = static_cast<std::uint8_t>(record[15]);

    if ((output.flags & ~std::uint8_t{0x03u}) != 0)
        return {status_code::artifact_corrupt};

    return output.identity
        ? status{}
        : status{status_code::artifact_corrupt};
}

status build_cache_image_view::contribution_member(
    std::size_t index,
    source_contribution_member& output) const noexcept {

    output = {};
    const auto& values =
        section(build_cache_image_section::contribution_members);
    if (index >= values.count)
        return {status_code::not_found};

    const auto* record =
        values.data + index * contribution_member_record_size;

    const auto raw_intrinsic = static_cast<std::uint8_t>(record[12]);
    const auto raw_access = static_cast<std::uint8_t>(record[20]);

    if (!valid_intrinsic(raw_intrinsic) ||
        !valid_member_access(raw_access) ||
        !zero_bytes(record + 13, 3) ||
        !zero_bytes(record + 21, 3)) {
        return {status_code::artifact_corrupt};
    }

    output.type.identity = identity_from_raw(read_u32(record));
    output.type.modifiers = read_range(record + 4);
    output.type.intrinsic =
        static_cast<intrinsic_type>(raw_intrinsic);
    output.name = string_from_raw(read_u32(record + 16));
    output.access = static_cast<source_member_access>(raw_access);

    return output.name
        ? status{}
        : status{status_code::artifact_corrupt};
}

status build_cache_image_view::contribution_modifier(
    std::size_t index,
    source_type_modifier& output) const noexcept {

    output = {};
    const auto& values =
        section(build_cache_image_section::contribution_modifiers);
    if (index >= values.count)
        return {status_code::not_found};

    const auto* record =
        values.data + index * contribution_modifier_record_size;
    const auto raw_kind = static_cast<std::uint8_t>(record[8]);
    if (!valid_modifier_kind(raw_kind) ||
        !zero_bytes(record + 9, 7)) {
        return {status_code::artifact_corrupt};
    }

    output.value = read_u64(record);
    output.kind = static_cast<source_type_modifier_kind>(raw_kind);
    return {};
}

status build_cache_image_view::contribution_enum_value(
    std::size_t index,
    source_contribution_enum_value& output) const noexcept {

    output = {};
    const auto& values =
        section(build_cache_image_section::contribution_enum_values);
    if (index >= values.count)
        return {status_code::not_found};

    const auto* record =
        values.data + index * contribution_enum_value_record_size;
    const auto raw_intrinsic = static_cast<std::uint8_t>(record[4]);

    if (!valid_intrinsic(raw_intrinsic) ||
        !zero_bytes(record + 5, 3)) {
        return {status_code::artifact_corrupt};
    }

    output.name = string_from_raw(read_u32(record));
    output.value.intrinsic =
        static_cast<intrinsic_type>(raw_intrinsic);
    output.value.bits = read_u64(record + 8);

    return output.name
        ? status{}
        : status{status_code::artifact_corrupt};
}

status build_cache_image_view::contribution_object(
    std::size_t index,
    source_contribution_object& output) const noexcept {

    output = {};
    const auto& values =
        section(build_cache_image_section::contribution_objects);
    if (index >= values.count)
        return {status_code::not_found};

    const auto* record =
        values.data + index * contribution_object_record_size;
    const auto raw_intrinsic = static_cast<std::uint8_t>(record[16]);

    if (!valid_intrinsic(raw_intrinsic) ||
        !zero_bytes(record + 17, 3)) {
        return {status_code::artifact_corrupt};
    }

    output.identity = identity_from_raw(read_u32(record));
    output.type.identity = identity_from_raw(read_u32(record + 4));
    output.type.modifiers = read_range(record + 8);
    output.type.intrinsic =
        static_cast<intrinsic_type>(raw_intrinsic);

    return output.identity
        ? status{}
        : status{status_code::artifact_corrupt};
}

status build_cache_image_view::contribution_link(
    std::size_t index,
    source_contribution_link& output) const noexcept {

    output = {};
    const auto& values =
        section(build_cache_image_section::contribution_links);
    if (index >= values.count)
        return {status_code::not_found};

    const auto* record =
        values.data + index * contribution_link_record_size;

    output.source.object = identity_from_raw(read_u32(record));
    output.source.member =
        member_index::from_zero_based(read_u32(record + 4));
    output.target.object = identity_from_raw(read_u32(record + 8));
    output.target.member =
        member_index::from_zero_based(read_u32(record + 12));

    return output.source.object &&
        output.source.member &&
        output.target.object &&
        output.target.member
        ? status{}
        : status{status_code::artifact_corrupt};
}

status build_cache_image_view::construction(
    type_handle handle,
    source_construction_state& output) const noexcept {

    output = {};
    if (!handle)
        return {status_code::not_found};

    const auto& values =
        section(build_cache_image_section::construction_states);
    if (handle.value() >= values.count)
        return {status_code::not_found};

    const auto* record =
        values.data +
        static_cast<std::size_t>(handle.value()) *
            construction_state_record_size;

    output.declarations = read_u32(record);
    output.definitions = read_u32(record + 4);
    output.definition_type = read_u32(record + 8);
    output.record_struct = read_u32(record + 12);
    output.record_class = read_u32(record + 16);
    output.record_union = read_u32(record + 20);
    output.enum_scoped = read_u32(record + 24);
    output.enum_unscoped = read_u32(record + 28);
    output.enum_fixed = read_u32(record + 32);

    const auto raw_underlying = static_cast<std::uint8_t>(record[36]);
    const auto raw_kind = static_cast<std::uint8_t>(record[37]);

    if (!valid_intrinsic(raw_underlying) ||
        !valid_contribution_kind(raw_kind) ||
        !zero_bytes(record + 38, 2)) {
        output = {};
        return {status_code::artifact_corrupt};
    }

    output.fixed_underlying =
        static_cast<intrinsic_type>(raw_underlying);
    output.kind =
        static_cast<source_contribution_type_kind>(raw_kind);
    return {};
}

status build_cache_image_view::construction_at_slot(
    std::size_t index,
    source_construction_state& output) const noexcept {

    output = {};

    const auto& values =
        section(build_cache_image_section::construction_states);
    if (index >= values.count)
        return {status_code::not_found};
    if (index == 0)
        return {};

    if (index > (std::numeric_limits<std::uint32_t>::max)())
        return {status_code::artifact_corrupt};

    return construction(
        type_handle{static_cast<std::uint32_t>(index)},
        output);
}

TypeRef build_cache_image_view::intrinsic_ref(
    intrinsic_type type) const noexcept {

    const auto index = static_cast<std::size_t>(type);
    const auto& values =
        section(build_cache_image_section::graph_intrinsic_refs);
    if (index >= values.count)
        return {};

    return type_ref_from_raw(read_u32(values.data + index * 4));
}

TypeRef build_cache_image_view::named_ref(
    type_handle handle) const noexcept {

    if (!handle)
        return {};

    const auto& values =
        section(build_cache_image_section::graph_named_refs);
    if (handle.value() >= values.count)
        return {};

    return type_ref_from_raw(read_u32(
        values.data +
        static_cast<std::size_t>(handle.value()) * 4));
}

std::size_t build_cache_image_view::derived_index_slot_count() const noexcept {
    return static_cast<std::size_t>(
        section(build_cache_image_section::graph_derived_index).count);
}

status build_cache_image_view::derived_index_slot(
    std::size_t index,
    build_cache_derived_index_slot& output) const noexcept {

    output = {};
    const auto& values =
        section(build_cache_image_section::graph_derived_index);
    if (index >= values.count)
        return {status_code::not_found};

    const auto* record =
        values.data + index * derived_index_record_size;

    output.fingerprint = read_u32(record);
    output.type = type_ref_from_raw(read_u32(record + 4));

    if (!output.type && output.fingerprint != 0)
        return {status_code::artifact_corrupt};
    if (output.type && output.fingerprint == 0)
        return {status_code::artifact_corrupt};

    return {};
}

std::size_t build_cache_image_view::dependency_version_count() const noexcept {
    return static_cast<std::size_t>(
        section(build_cache_image_section::graph_dependency_versions).count);
}

std::uint32_t build_cache_image_view::dependency_version(
    type_handle handle) const noexcept {

    if (!handle)
        return 0;

    const auto& values =
        section(build_cache_image_section::graph_dependency_versions);
    if (handle.value() > values.count)
        return 0;

    return read_u32(
        values.data +
        static_cast<std::size_t>(handle.value() - 1) * 4);
}

std::uint32_t build_cache_image_view::reverse_dependency_head(
    type_handle handle) const noexcept {

    if (!handle)
        return 0;

    const auto& values =
        section(build_cache_image_section::graph_reverse_dependency_heads);
    if (handle.value() > values.count)
        return 0;

    return read_u32(
        values.data +
        static_cast<std::size_t>(handle.value() - 1) * 4);
}

std::size_t build_cache_image_view::dependency_edge_count() const noexcept {
    return static_cast<std::size_t>(
        section(build_cache_image_section::graph_dependency_edges).count);
}

status build_cache_image_view::dependency_edge(
    std::size_t index,
    graph_dependency_edge& output) const noexcept {

    output = {};
    const auto& values =
        section(build_cache_image_section::graph_dependency_edges);
    if (index >= values.count)
        return {status_code::not_found};

    const auto* record =
        values.data + index * dependency_edge_record_size;
    output.owner_handle = read_u32(record);
    output.next_for_target = read_u32(record + 4);
    output.owner_version = read_u32(record + 8);

    return output.owner_handle != 0 && output.owner_version != 0
        ? status{}
        : status{status_code::artifact_corrupt};
}

type_handle build_cache_image_view::find_type_identity(
    identity_ref identity,
    const compiled_image_view& compiled) const noexcept {

    if (!identity || identity.kind() != identity_kind::type)
        return {};
    const auto& index = section(build_cache_image_section::graph_type_identity_index);
    if (index.count == 0 || (index.count & (index.count - 1)) != 0)
        return {};
    const auto hash = graph_identity_hash(identity);
    const auto fingerprint = fold32(hash);
    const auto mask = static_cast<std::size_t>(index.count - 1);
    auto position = static_cast<std::size_t>(hash) & mask;
    for (std::size_t probe = 0; probe < index.count; ++probe) {
        const auto* slot = index.data + position * historical_index_record_size;
        const auto handle = read_u32(slot + 4);
        if (handle == 0)
            return {};
        if (read_u32(slot) == fingerprint &&
            compiled.type_identity_at_slot(static_cast<std::size_t>(handle - 1)) == identity) {
            return type_handle{handle};
        }
        position = (position + 1) & mask;
    }
    return {};
}

object_handle build_cache_image_view::find_object_identity(
    identity_ref identity,
    const compiled_image_view& compiled) const noexcept {

    if (!identity || identity.kind() != identity_kind::object)
        return {};
    const auto& index = section(build_cache_image_section::graph_object_identity_index);
    if (index.count == 0 || (index.count & (index.count - 1)) != 0)
        return {};
    const auto hash = graph_identity_hash(identity);
    const auto fingerprint = fold32(hash);
    const auto mask = static_cast<std::size_t>(index.count - 1);
    auto position = static_cast<std::size_t>(hash) & mask;
    for (std::size_t probe = 0; probe < index.count; ++probe) {
        const auto* slot = index.data + position * historical_index_record_size;
        const auto handle = read_u32(slot + 4);
        if (handle == 0)
            return {};
        if (read_u32(slot) == fingerprint &&
            compiled.object_identity_at_slot(static_cast<std::size_t>(handle - 1)) == identity) {
            return object_handle{handle};
        }
        position = (position + 1) & mask;
    }
    return {};
}

link_handle build_cache_image_view::find_link_target(
    object_endpoint target,
    const compiled_image_view& compiled) const noexcept {

    if (!target.object || !target.member)
        return {};
    const auto& index = section(build_cache_image_section::graph_link_target_index);
    if (index.count == 0 || (index.count & (index.count - 1)) != 0)
        return {};
    const auto hash = endpoint_hash(target);
    const auto fingerprint = fold32(hash);
    const auto mask = static_cast<std::size_t>(index.count - 1);
    auto position = static_cast<std::size_t>(hash) & mask;
    for (std::size_t probe = 0; probe < index.count; ++probe) {
        const auto* slot = index.data + position * historical_index_record_size;
        const auto handle = read_u32(slot + 4);
        if (handle == 0)
            return {};
        if (read_u32(slot) == fingerprint) {
            compiled_image_link_record record;
            if (compiled.link_raw(link_handle{handle}, record).ok() && record.target == target)
                return link_handle{handle};
        }
        position = (position + 1) & mask;
    }
    return {};
}

std::size_t build_cache_image_view::type_identity_index_slot_count() const noexcept {
    return static_cast<std::size_t>(
        section(build_cache_image_section::graph_type_identity_index).count);
}

status build_cache_image_view::type_identity_index_slot(
    std::size_t index,
    graph_identity_index_slot& output) const noexcept {

    output = {};
    const auto& values =
        section(build_cache_image_section::graph_type_identity_index);
    if (index >= values.count)
        return {status_code::not_found};
    const auto* slot = values.data + index * historical_index_record_size;
    output.fingerprint = read_u32(slot);
    output.handle = read_u32(slot + 4);
    return {};
}

std::size_t build_cache_image_view::object_identity_index_slot_count() const noexcept {
    return static_cast<std::size_t>(
        section(build_cache_image_section::graph_object_identity_index).count);
}

status build_cache_image_view::object_identity_index_slot(
    std::size_t index,
    graph_object_identity_index_slot& output) const noexcept {

    output = {};
    const auto& values =
        section(build_cache_image_section::graph_object_identity_index);
    if (index >= values.count)
        return {status_code::not_found};
    const auto* slot = values.data + index * historical_index_record_size;
    output.fingerprint = read_u32(slot);
    output.handle = read_u32(slot + 4);
    return {};
}

std::size_t build_cache_image_view::link_target_index_slot_count() const noexcept {
    return static_cast<std::size_t>(
        section(build_cache_image_section::graph_link_target_index).count);
}

status build_cache_image_view::link_target_index_slot(
    std::size_t index,
    graph_link_index_slot& output) const noexcept {

    output = {};
    const auto& values =
        section(build_cache_image_section::graph_link_target_index);
    if (index >= values.count)
        return {status_code::not_found};
    const auto* slot = values.data + index * historical_index_record_size;
    output.fingerprint = read_u32(slot);
    output.handle = read_u32(slot + 4);
    return {};
}

std::size_t build_cache_image_view::named_ref_count() const noexcept {
    return static_cast<std::size_t>(
        section(build_cache_image_section::graph_named_refs).count);
}

status build_cache_image_view::verify_contents(
    bool verify_section_crc) const noexcept {
    if (!valid())
        return {status_code::invalid_state};

    for (const auto& value : sections) {
        std::uint64_t byte_count = 0;
        if (!multiply_u64(value.count, value.record_size, byte_count) ||
            byte_count > (std::numeric_limits<std::size_t>::max)()) {
            return {status_code::artifact_corrupt};
        }

        if (verify_section_crc) {
            std::uint64_t crc = 0;

            if (!value.segment.empty()) {
                for (std::size_t extent_index = 0;
                     extent_index <
                        value.segment.extent_count();
                     ++extent_index) {

                    crc = persistence_crc64_update(
                        crc,
                        value.segment.extent(
                            extent_index));
                }
            }
            else {
                crc = persistence_crc64(
                    std::span<const std::byte>{
                        value.data,
                        static_cast<std::size_t>(
                            byte_count)});
            }

            if (crc != value.crc64)
                return {status_code::artifact_corrupt};
        }
    }

    const auto& source_bytes =
        section(build_cache_image_section::source_bytes);
    const auto& local_types =
        section(build_cache_image_section::frontend_local_types);
    const auto& type_slots =
        section(build_cache_image_section::frontend_type_slots);
    const auto& object_slots =
        section(build_cache_image_section::frontend_object_slots);
    const auto& member_slots =
        section(build_cache_image_section::frontend_member_slots);

    std::uint64_t text_cursor = 0;
    std::uint64_t local_type_cursor = 0;
    std::uint64_t type_slot_cursor = 0;
    std::uint64_t object_slot_cursor = 0;
    std::uint64_t member_slot_cursor = 0;
    std::size_t observed_frontends = 0;

    for (std::size_t index = 0; index < source_count_value; ++index) {
        const source_id id{static_cast<std::uint32_t>(index + 1)};
        build_cache_source_record record;
        if (!source(id, record).ok())
            return {status_code::artifact_corrupt};

        if (record.snapshot_present) {
            if (record.text_offset != text_cursor ||
                record.text_length > source_bytes.count - text_cursor) {
                return {status_code::artifact_corrupt};
            }
            text_cursor += record.text_length;
        } else if (record.text_offset != 0 || record.text_length != 0) {
            return {status_code::artifact_corrupt};
        }

        if (record.frontend_present && !record.snapshot_present)
            return {status_code::artifact_corrupt};

        if (record.frontend_present) {
            ++observed_frontends;

            if (record.local_types.begin != local_type_cursor ||
                record.type_slots.begin != type_slot_cursor ||
                record.object_slots.begin != object_slot_cursor ||
                record.member_slots.begin != member_slot_cursor ||
                !valid_cache_range(record.local_types, local_types.count) ||
                !valid_cache_range(record.type_slots, type_slots.count) ||
                !valid_cache_range(record.object_slots, object_slots.count) ||
                !valid_cache_range(record.member_slots, member_slots.count)) {
                return {status_code::artifact_corrupt};
            }

            local_type_cursor += record.local_types.count;
            type_slot_cursor += record.type_slots.count;
            object_slot_cursor += record.object_slots.count;
            member_slot_cursor += record.member_slots.count;

            for (std::size_t local = 0;
                 local < record.local_types.count;
                 ++local) {
                identity_ref identity;
                if (!frontend_local_type(id, local, identity).ok())
                    return {status_code::artifact_corrupt};
            }

            for (std::size_t local = 0;
                 local < record.type_slots.count;
                 ++local) {
                source_interface_type_slot slot;
                if (!frontend_type_slot(id, local, slot).ok())
                    return {status_code::artifact_corrupt};
            }

            for (std::size_t local = 0;
                 local < record.object_slots.count;
                 ++local) {
                source_interface_object_slot slot;
                if (!frontend_object_slot(id, local, slot).ok())
                    return {status_code::artifact_corrupt};
            }

            for (std::size_t local = 0;
                 local < record.member_slots.count;
                 ++local) {
                source_interface_member_slot slot;
                if (!frontend_member_slot(id, local, slot).ok())
                    return {status_code::artifact_corrupt};
            }
        } else if (record.local_types.count != 0 ||
                   record.type_slots.count != 0 ||
                   record.object_slots.count != 0 ||
                   record.member_slots.count != 0 ||
                   record.local_types.begin != 0 ||
                   record.type_slots.begin != 0 ||
                   record.object_slots.begin != 0 ||
                   record.member_slots.begin != 0) {
            return {status_code::artifact_corrupt};
        }
    }

    if (text_cursor != source_bytes.count ||
        local_type_cursor != local_types.count ||
        type_slot_cursor != type_slots.count ||
        object_slot_cursor != object_slots.count ||
        member_slot_cursor != member_slots.count ||
        observed_frontends != frontend_count_value) {
        return {status_code::artifact_corrupt};
    }

    const auto& source_file_index =
        section(build_cache_image_section::source_file_identity_index);
    for (std::size_t index = 0;
         index < source_file_index.count;
         ++index) {

        const auto* slot =
            source_file_index.data +
            index * source_file_identity_index_record_size;
        const auto file_reference = read_u64(slot);
        const auto raw_source = read_u32(slot + 8);
        const auto reserved = read_u32(slot + 12);

        if (reserved != 0)
            return {status_code::artifact_corrupt};

        if (file_reference == 0) {
            if (raw_source != 0)
                return {status_code::artifact_corrupt};
            continue;
        }

        const auto indexed_source =
            find_source_file(file_reference);
        if (raw_source == 0 ||
            raw_source > source_count_value ||
            !indexed_source ||
            indexed_source.value() != raw_source) {
            return {status_code::artifact_corrupt};
        }
    }

    const auto& directory_index =
        section(build_cache_image_section::tracked_directory_identity_index);
    for (std::size_t index = 0;
         index < directory_index.count;
         ++index) {

        const auto* slot =
            directory_index.data +
            index *
                tracked_directory_identity_index_record_size;

        const auto file_reference = read_u64(slot);
        const auto flags = read_u32(slot + 8);
        const auto reserved = read_u32(slot + 12);

        if (file_reference == 0) {
            if (flags != 0 || reserved != 0)
                return {status_code::artifact_corrupt};
            continue;
        }

        if (reserved != 0 ||
            flags == 0 ||
            (flags & ~source_change_directory_watch_known) != 0 ||
            directory_watch_flags(file_reference) != flags) {
            return {status_code::artifact_corrupt};
        }
    }

    const auto& states =
        section(build_cache_image_section::contribution_states);
    const auto& types =
        section(build_cache_image_section::contribution_types);
    const auto& members =
        section(build_cache_image_section::contribution_members);
    const auto& modifiers =
        section(build_cache_image_section::contribution_modifiers);
    const auto& enum_values =
        section(build_cache_image_section::contribution_enum_values);
    const auto& objects =
        section(build_cache_image_section::contribution_objects);
    const auto& links =
        section(build_cache_image_section::contribution_links);

    if (states.count == 0 ||
        !zero_bytes(states.data, contribution_state_record_size)) {
        return {status_code::artifact_corrupt};
    }

    source_contribution_statistics observed_statistics;

    for (std::size_t index = 1; index < states.count; ++index) {
        const auto* raw =
            states.data + index * contribution_state_record_size;
        const auto raw_source = read_u32(raw);

        if (raw_source == 0) {
            if (!zero_bytes(raw, contribution_state_record_size))
                return {status_code::artifact_corrupt};
            continue;
        }

        if (raw_source != index)
            return {status_code::artifact_corrupt};

        source_contribution_state state;
        const auto result =
            contribution_state(
                source_id{static_cast<std::uint32_t>(index)},
                state);
        if (!result.ok() ||
            !valid_range(state.types, types.count) ||
            !valid_range(state.members, members.count) ||
            !valid_range(state.modifiers, modifiers.count) ||
            !valid_range(state.enum_values, enum_values.count) ||
            !valid_range(state.objects, objects.count) ||
            !valid_range(state.links, links.count)) {
            return {status_code::artifact_corrupt};
        }

        ++observed_statistics.sources;
        observed_statistics.type_declarations += state.types.count;
        observed_statistics.members += state.members.count;
        observed_statistics.modifiers += state.modifiers.count;
        observed_statistics.enum_values += state.enum_values.count;
        observed_statistics.objects += state.objects.count;
        observed_statistics.links += state.links.count;
    }

    if (observed_statistics.sources != contribution_statistics_value.sources ||
        observed_statistics.type_declarations != contribution_statistics_value.type_declarations ||
        observed_statistics.members != contribution_statistics_value.members ||
        observed_statistics.modifiers != contribution_statistics_value.modifiers ||
        observed_statistics.enum_values != contribution_statistics_value.enum_values ||
        observed_statistics.objects != contribution_statistics_value.objects ||
        observed_statistics.links != contribution_statistics_value.links) {
        return {status_code::artifact_corrupt};
    }

    for (std::size_t index = 0; index < types.count; ++index) {
        source_contribution_type value;
        if (!contribution_type(index, value).ok())
            return {status_code::artifact_corrupt};

        const auto limit =
            value.kind == source_contribution_type_kind::record
                ? members.count
                : enum_values.count;
        if (!valid_range(value.definition_items, limit))
            return {status_code::artifact_corrupt};
    }

    for (std::size_t index = 0; index < members.count; ++index) {
        source_contribution_member value;
        if (!contribution_member(index, value).ok() ||
            !valid_range(value.type.modifiers, modifiers.count)) {
            return {status_code::artifact_corrupt};
        }
    }

    for (std::size_t index = 0; index < modifiers.count; ++index) {
        source_type_modifier value;
        if (!contribution_modifier(index, value).ok())
            return {status_code::artifact_corrupt};
    }

    for (std::size_t index = 0; index < enum_values.count; ++index) {
        source_contribution_enum_value value;
        if (!contribution_enum_value(index, value).ok())
            return {status_code::artifact_corrupt};
    }

    for (std::size_t index = 0; index < objects.count; ++index) {
        source_contribution_object value;
        if (!contribution_object(index, value).ok() ||
            !valid_range(value.type.modifiers, modifiers.count)) {
            return {status_code::artifact_corrupt};
        }
    }

    for (std::size_t index = 0; index < links.count; ++index) {
        source_contribution_link value;
        if (!contribution_link(index, value).ok())
            return {status_code::artifact_corrupt};
    }

    const auto& construction_states =
        section(build_cache_image_section::construction_states);
    if (construction_states.count == 0 ||
        !zero_bytes(
            construction_states.data,
            construction_state_record_size)) {
        return {status_code::artifact_corrupt};
    }

    for (std::size_t index = 1;
         index < construction_states.count;
         ++index) {

        source_construction_state value;
        if (!construction(
                type_handle{static_cast<std::uint32_t>(index)},
                value).ok()) {
            return {status_code::artifact_corrupt};
        }

        if (value.definition_type > types.count ||
            value.definitions > 1 ||
            (value.definitions == 0 && value.definition_type != 0) ||
            (value.definitions != 0 && value.definition_type == 0)) {
            return {status_code::artifact_corrupt};
        }
    }

    const auto& intrinsic_refs =
        section(build_cache_image_section::graph_intrinsic_refs);
    if (intrinsic_refs.count != graph_intrinsic_type_count ||
        read_u32(intrinsic_refs.data) != 0) {
        return {status_code::artifact_corrupt};
    }

    const auto& named_refs =
        section(build_cache_image_section::graph_named_refs);
    if (named_refs.count == 0 || read_u32(named_refs.data) != 0)
        return {status_code::artifact_corrupt};

    const auto& derived_index =
        section(build_cache_image_section::graph_derived_index);
    if (derived_index.count != 0 &&
        (derived_index.count & (derived_index.count - 1)) != 0) {
        return {status_code::artifact_corrupt};
    }

    std::size_t observed_derived = 0;

    for (std::size_t index = 0; index < derived_index.count; ++index) {
        build_cache_derived_index_slot slot;
        if (!derived_index_slot(index, slot).ok())
            return {status_code::artifact_corrupt};
        if (slot.type)
            ++observed_derived;
    }

    if (observed_derived != derived_index_entries_value)
        return {status_code::artifact_corrupt};

    const auto& dependency_versions =
        section(build_cache_image_section::graph_dependency_versions);
    const auto& reverse_heads =
        section(build_cache_image_section::graph_reverse_dependency_heads);
    const auto& dependency_edges =
        section(build_cache_image_section::graph_dependency_edges);

    if (named_refs.count != dependency_versions.count + 1 ||
        reverse_heads.count != dependency_versions.count) {
        return {status_code::artifact_corrupt};
    }

    for (std::size_t index = 0; index < reverse_heads.count; ++index) {
        const auto head = read_u32(reverse_heads.data + index * 4);
        if (head > dependency_edges.count)
            return {status_code::artifact_corrupt};
    }

    for (std::size_t index = 0; index < dependency_edges.count; ++index) {
        graph_dependency_edge edge;
        const auto one_based_index =
            static_cast<std::uint64_t>(index) + 1;
        if (!dependency_edge(index, edge).ok() ||
            edge.owner_handle > dependency_versions.count ||
            edge.next_for_target >= one_based_index) {
            return {status_code::artifact_corrupt};
        }
    }

    const auto verify_historical_index = [&](build_cache_image_section kind) noexcept {
        const auto& values = section(kind);
        for (std::size_t index = 0; index < values.count; ++index) {
            const auto* slot = values.data + index * historical_index_record_size;
            const auto fingerprint = read_u32(slot);
            const auto handle = read_u32(slot + 4);
            if (handle == 0) {
                if (fingerprint != 0)
                    return false;
            } else if (fingerprint == 0) {
                return false;
            }
        }
        return true;
    };

    if (!verify_historical_index(build_cache_image_section::graph_type_identity_index) ||
        !verify_historical_index(build_cache_image_section::graph_object_identity_index) ||
        !verify_historical_index(build_cache_image_section::graph_link_target_index)) {
        return {status_code::artifact_corrupt};
    }

    return {};
}

struct build_cache_image_view::source_validation_access final {
    const source_manager_image_view* image = nullptr;
    const source_manager* manager = nullptr;
    std::span<const source_generation_physical_record> native_physical;
    std::size_t native_source_count = 0;
    bool content_hash_proven = false;
    bool historical_index_proven = false;

    [[nodiscard]] bool valid() const noexcept {
        if (image != nullptr)
            return image->valid();
        if (manager != nullptr)
            return manager->source_count() == native_source_count;
        return native_physical.size() == native_source_count;
    }

    [[nodiscard]] std::size_t source_count() const noexcept {
        if (image != nullptr)
            return image->source_count();
        if (manager != nullptr)
            return manager->source_count();
        return native_source_count;
    }

    [[nodiscard]] status physical(
        source_id source,
        source_manager_image_physical_state& output) const noexcept {

        output = {};

        if (image != nullptr)
            return image->physical(source, output);

        if (manager != nullptr) {
            if (!source ||
                static_cast<std::size_t>(source.value()) >
                    manager->source_count()) {
                return {status_code::invalid_argument};
            }

            const auto snapshot =
                manager->current(source);

            if (!snapshot)
                return {};

            const auto observation =
                snapshot.observation();

            output.present = true;
            output.write_time_ticks =
                observation.write_time_ticks;
            output.size =
                static_cast<std::uint64_t>(
                    observation.size);
            output.hash = snapshot.hash();
            return {};
        }

        if (!source ||
            static_cast<std::size_t>(source.value()) >
                native_physical.size()) {
            return {status_code::invalid_argument};
        }

        const auto& value =
            native_physical[
                static_cast<std::size_t>(
                    source.value() - 1)];

        output.present = value.present();
        output.write_time_ticks =
            value.write_time_ticks;
        output.size = value.size;
        output.hash = value.hash;
        return {};
    }
};

status build_cache_image_view::verify_against(
    const compiled_image_view& compiled,
    const source_manager_image_view& sources) const noexcept {

    source_validation_access access;
    access.image = &sources;

    return verify_against_impl(
        compiled,
        access);
}

status build_cache_image_view::verify_against(
    const compiled_image_view& compiled,
    const source_manager& sources) const noexcept {

    const auto native =
        sources.native_generation();

    if (!native.complete ||
        native.physical.size() !=
            sources.source_count()) {
        return {status_code::invalid_state};
    }

    source_validation_access access;
    access.native_physical =
        native.physical;
    access.native_source_count =
        sources.source_count();

    return verify_against_impl(
        compiled,
        access);
}

status build_cache_image_view::verify_against_sparse_generation(
    const compiled_image_view& compiled,
    const source_manager& sources) const noexcept {

    if (!sources.baseline_backed())
        return {status_code::invalid_state};

    source_validation_access access;
    access.manager = &sources;
    access.native_source_count =
        sources.source_count();

    // Build Cache source bytes were encoded from this same committed
    // Generation. The physical snapshot hash is therefore an encoder proof,
    // exactly as in the fresh encoded-generation verifier. Historical indexes
    // are deliberately NOT marked proven: verify_against_impl() audits them
    // against compiled.bin without requiring contiguous Graph storage.
    access.content_hash_proven = true;
    access.historical_index_proven = false;

    return verify_against_impl(
        compiled,
        access);
}

status build_cache_image_view::verify_against_encoded_generation(
    const compiled_image_view& compiled,
    const source_manager& sources,
    const graph& committed_graph) const noexcept {

    const auto native =
        sources.native_generation();

    if (!native.complete ||
        native.physical.size() !=
            sources.source_count() ||
        committed_graph.baseline_backed()) {
        return {status_code::invalid_state};
    }

    const auto semantic =
        committed_graph.data_view();
    const auto build =
        committed_graph.build_data_view();

    const auto graph_type_identities =
        static_cast<std::span<const identity_ref>>(
            semantic.type_identities);
    const auto graph_object_identities =
        static_cast<std::span<const identity_ref>>(
            semantic.object_identities);
    const auto graph_links =
        static_cast<std::span<const link_record>>(
            semantic.links);

    const auto graph_type_index =
        static_cast<std::span<
            const graph_identity_index_slot>>(
                build.type_identity_index);
    const auto graph_object_index =
        static_cast<std::span<
            const graph_object_identity_index_slot>>(
                build.object_identity_index);
    const auto graph_link_index =
        static_cast<std::span<
            const graph_link_index_slot>>(
                build.link_target_index);

    // GEN-02C28: encoder-proven historical indexes.
    // Fresh G0 owns contiguous Graph arrays. First triangulate compiled.bin
    // semantic slots against the committed Graph. Then prove the persisted
    // Build Cache historical tables are the exact canonical serialization of
    // that same validated Graph. The cold/mapped verifier keeps the original
    // lookup-by-lookup audit.
    if (graph_type_identities.size() !=
            committed_graph.type_slot_count() ||
        graph_object_identities.size() !=
            committed_graph.object_slot_count() ||
        graph_links.size() !=
            committed_graph.link_slot_count() ||
        graph_type_index.size() !=
            build.type_identity_index.size() ||
        graph_object_index.size() !=
            build.object_identity_index.size() ||
        graph_link_index.size() !=
            build.link_target_index.size() ||
        compiled.type_slot_count() !=
            graph_type_identities.size() ||
        compiled.object_slot_count() !=
            graph_object_identities.size() ||
        compiled.link_slot_count() !=
            graph_links.size()) {
        return {status_code::artifact_corrupt};
    }

    for (std::size_t index = 0;
         index < graph_type_identities.size();
         ++index) {

        const auto expected =
            graph_type_identities[index];
        if (!expected ||
            compiled.type_identity_at_slot(index) !=
                expected) {
            return {status_code::artifact_corrupt};
        }
    }

    for (std::size_t index = 0;
         index < graph_object_identities.size();
         ++index) {

        const auto expected =
            graph_object_identities[index];
        if (!expected ||
            compiled.object_identity_at_slot(index) !=
                expected) {
            return {status_code::artifact_corrupt};
        }
    }

    for (std::size_t index = 0;
         index < graph_links.size();
         ++index) {

        compiled_image_link_record encoded;
        if (!compiled.link_raw(
                link_handle{
                    static_cast<std::uint32_t>(
                        index + 1)},
                encoded).ok()) {
            return {status_code::artifact_corrupt};
        }

        const auto& expected =
            graph_links[index];

        if (!encoded.target.object ||
            !encoded.target.member ||
            encoded.source != expected.source ||
            encoded.target != expected.target) {
            return {status_code::artifact_corrupt};
        }
    }

    const auto verify_index =
        [&](build_cache_image_section kind,
            const auto& expected) noexcept {

            const auto& persisted =
                section(kind);

            if (persisted.record_size !=
                    historical_index_record_size ||
                persisted.count !=
                    expected.size()) {
                return false;
            }

            if (expected.empty())
                return true;

            if constexpr (
                std::endian::native ==
                    std::endian::little) {

                static_assert(
                    sizeof(
                        typename std::remove_reference_t<
                            decltype(expected)>::value_type) ==
                    historical_index_record_size);

                return std::memcmp(
                    persisted.data,
                    expected.data(),
                    expected.size_bytes()) == 0;
            }
            else {
                for (std::size_t index = 0;
                     index < expected.size();
                     ++index) {

                    const auto* slot =
                        persisted.data +
                        index *
                            historical_index_record_size;

                    if (read_u32(slot) !=
                            expected[index].fingerprint ||
                        read_u32(slot + 4) !=
                            expected[index].handle) {
                        return false;
                    }
                }

                return true;
            }
        };

    if (!verify_index(
            build_cache_image_section::
                graph_type_identity_index,
            graph_type_index) ||
        !verify_index(
            build_cache_image_section::
                graph_object_identity_index,
            graph_object_index) ||
        !verify_index(
            build_cache_image_section::
                graph_link_target_index,
            graph_link_index)) {
        return {status_code::artifact_corrupt};
    }

    source_validation_access access;
    access.native_physical =
        native.physical;
    access.native_source_count =
        sources.source_count();
    access.content_hash_proven = true;
    access.historical_index_proven = true;

    return verify_against_impl(
        compiled,
        access);
}

status build_cache_image_view::verify_against_impl(
    const compiled_image_view& compiled,
    const source_validation_access& sources) const noexcept {

    if (!valid() || !compiled.valid() || !sources.valid())
        return {status_code::invalid_state};

    if (source_count_value != sources.source_count() ||
        construction_slot_count() != compiled.type_slot_count() ||
        dependency_version_count() != compiled.type_slot_count()) {
        return {status_code::artifact_corrupt};
    }

    const auto verify_source =
        [&](std::size_t index) noexcept {
            const source_id source_value{
                static_cast<std::uint32_t>(index + 1)};

            build_cache_source_record record;
            source_manager_image_physical_state physical;
            if (!source(source_value, record).ok() ||
                !sources.physical(source_value, physical).ok()) {
                return false;
            }

            if (record.snapshot_present != physical.present)
                return false;

            if (record.snapshot_present) {
                const auto text = source_text(source_value);
                if (text.size() != record.text_length ||
                    physical.size != text.size() ||
                    (!sources.content_hash_proven &&
                     hash_source_content(text) != physical.hash)) {
                    return false;
                }
            }

            if (!record.frontend_present)
                return true;

            for (std::size_t local = 0;
                 local < record.local_types.count;
                 ++local) {

                identity_ref identity;
                if (!frontend_local_type(
                        source_value,
                        local,
                        identity).ok() ||
                    !compiled.identity_valid(identity) ||
                    identity.kind() != identity_kind::type) {
                    return false;
                }
            }

            for (std::size_t local = 0;
                 local < record.type_slots.count;
                 ++local) {

                source_interface_type_slot slot;
                if (!frontend_type_slot(
                        source_value,
                        local,
                        slot).ok()) {
                    return false;
                }

                if (!slot.identity)
                    continue;

                if (!compiled.identity_valid(slot.parent) ||
                    !compiled.identity_valid(slot.identity) ||
                    slot.identity.kind() != identity_kind::type ||
                    compiled.string(slot.name).empty()) {
                    return false;
                }
            }

            for (std::size_t local = 0;
                 local < record.object_slots.count;
                 ++local) {

                source_interface_object_slot slot;
                if (!frontend_object_slot(
                        source_value,
                        local,
                        slot).ok()) {
                    return false;
                }

                if (!slot.identity)
                    continue;

                if (!compiled.identity_valid(slot.parent) ||
                    !compiled.identity_valid(slot.identity) ||
                    slot.identity.kind() != identity_kind::object ||
                    compiled.string(slot.name).empty() ||
                    (slot.named_type &&
                     (!compiled.identity_valid(slot.named_type) ||
                      slot.named_type.kind() != identity_kind::type))) {
                    return false;
                }
            }

            for (std::size_t local = 0;
                 local < record.member_slots.count;
                 ++local) {

                source_interface_member_slot slot;
                if (!frontend_member_slot(
                        source_value,
                        local,
                        slot).ok()) {
                    return false;
                }

                if (!slot.type)
                    continue;

                if (!compiled.identity_valid(slot.type) ||
                    slot.type.kind() != identity_kind::type ||
                    compiled.string(slot.name).empty()) {
                    return false;
                }
            }

            return true;
        };

    // GEN-02C25: coarse-grained Source verification scheduler.
    // Mapped/cold audits still rehash every Source. GEN-02C27 freshly encoded
    // native Generations consume the encoder's immutable snapshot-hash proof and
    // avoid only that redundant content pass. All Source shape and Frontend
    // cross-artifact checks remain unchanged.
    constexpr std::size_t max_source_verify_workers = 8;
    constexpr std::size_t minimum_sources_per_verify_worker = 8192;
    constexpr std::size_t source_verify_batch_size = 256;

    const auto parallel_verify_source_count =
        source_count_value;

    auto verify_sources_serial = [&]() noexcept {
        for (std::size_t index = 0;
             index < parallel_verify_source_count;
             ++index) {

            if (!verify_source(index))
                return false;
        }

        return true;
    };

    if (parallel_verify_source_count != 0) {
        const auto hardware_workers =
            (std::max)(
                std::size_t{1},
                static_cast<std::size_t>(
                    std::thread::hardware_concurrency()));

        const auto desired_workers =
            std::size_t{1} +
            (parallel_verify_source_count - 1) /
                minimum_sources_per_verify_worker;

        const auto worker_count =
            (std::min)(
                max_source_verify_workers,
                (std::min)(
                    hardware_workers,
                    desired_workers));

        if (worker_count <= 1) {
            if (!verify_sources_serial())
                return {status_code::artifact_corrupt};
        }
        else {
            std::atomic<std::size_t> next_source{0};
            std::atomic<bool> source_failed{false};

            const auto verify_worker = [&]() noexcept {
                for (;;) {
                    if (source_failed.load(
                            std::memory_order_relaxed)) {
                        return;
                    }

                    const auto begin =
                        next_source.fetch_add(
                            source_verify_batch_size,
                            std::memory_order_relaxed);

                    if (begin >=
                        parallel_verify_source_count) {
                        return;
                    }

                    const auto end =
                        (std::min)(
                            begin +
                                source_verify_batch_size,
                            parallel_verify_source_count);

                    for (std::size_t index = begin;
                         index < end;
                         ++index) {

                        if (!verify_source(index)) {
                            source_failed.store(
                                true,
                                std::memory_order_relaxed);
                            return;
                        }
                    }
                }
            };

            std::array<std::jthread,
                max_source_verify_workers - 1>
                workers{};

            std::size_t launched_workers = 0;
            bool parallel_started = true;

            try {
                for (std::size_t worker = 1;
                     worker < worker_count;
                     ++worker) {

                    workers[launched_workers] =
                        std::jthread(
                            [&]() noexcept {
                                verify_worker();
                            });

                    ++launched_workers;
                }
            }
            catch (const std::bad_alloc&) {
                parallel_started = false;
            }
            catch (const std::system_error&) {
                parallel_started = false;
            }

            if (parallel_started)
                verify_worker();

            for (std::size_t worker = 0;
                 worker < launched_workers;
                 ++worker) {

                if (workers[worker].joinable())
                    workers[worker].join();
            }

            if (!parallel_started) {
                // Partially launched workers may already have completed some
                // checks. Restart the complete deterministic validation
                // serially rather than accepting a partial proof.
                if (!verify_sources_serial())
                    return {status_code::artifact_corrupt};
            }
            else if (source_failed.load(
                         std::memory_order_relaxed)) {
                return {status_code::artifact_corrupt};
            }
        }
    }

    for (std::size_t index = 0;
         index < contribution_type_count();
         ++index) {

        source_contribution_type value;
        if (!contribution_type(index, value).ok() ||
            !compiled.identity_valid(value.identity) ||
            value.identity.kind() != identity_kind::type) {
            return {status_code::artifact_corrupt};
        }
    }

    for (std::size_t index = 0;
         index < contribution_member_count();
         ++index) {

        source_contribution_member value;
        if (!contribution_member(index, value).ok() ||
            compiled.string(value.name).empty()) {
            return {status_code::artifact_corrupt};
        }

        if (value.type.identity) {
            if (!compiled.identity_valid(value.type.identity) ||
                value.type.identity.kind() != identity_kind::type ||
                value.type.intrinsic != intrinsic_type::none) {
                return {status_code::artifact_corrupt};
            }
        } else if (value.type.intrinsic == intrinsic_type::none) {
            return {status_code::artifact_corrupt};
        }
    }

    for (std::size_t index = 0;
         index < contribution_enum_value_count();
         ++index) {

        source_contribution_enum_value value;
        if (!contribution_enum_value(index, value).ok() ||
            compiled.string(value.name).empty()) {
            return {status_code::artifact_corrupt};
        }
    }

    for (std::size_t index = 0;
         index < contribution_object_count();
         ++index) {

        source_contribution_object value;
        if (!contribution_object(index, value).ok() ||
            !compiled.identity_valid(value.identity) ||
            value.identity.kind() != identity_kind::object) {
            return {status_code::artifact_corrupt};
        }

        if (value.type.identity) {
            if (!compiled.identity_valid(value.type.identity) ||
                value.type.identity.kind() != identity_kind::type ||
                value.type.intrinsic != intrinsic_type::none) {
                return {status_code::artifact_corrupt};
            }
        } else if (value.type.intrinsic == intrinsic_type::none) {
            return {status_code::artifact_corrupt};
        }
    }

    for (std::size_t index = 0;
         index < contribution_link_count();
         ++index) {

        source_contribution_link value;
        if (!contribution_link(index, value).ok() ||
            !compiled.identity_valid(value.source.object) ||
            !compiled.identity_valid(value.target.object) ||
            value.source.object.kind() != identity_kind::object ||
            value.target.object.kind() != identity_kind::object) {
            return {status_code::artifact_corrupt};
        }
    }

    for (std::size_t index = 1;
         index < graph_intrinsic_type_count;
         ++index) {

        const auto intrinsic =
            static_cast<intrinsic_type>(index);
        const auto ref = intrinsic_ref(intrinsic);
        if (!ref)
            continue;

        intrinsic_type decoded;
        if (!compiled.intrinsic(ref, decoded) ||
            decoded != intrinsic) {
            return {status_code::artifact_corrupt};
        }
    }

    const auto type_slots = compiled.type_slot_count();
    const auto& named_refs =
        section(build_cache_image_section::graph_named_refs);
    if (named_refs.count != type_slots + 1)
        return {status_code::artifact_corrupt};

    for (std::size_t index = 1; index < named_refs.count; ++index) {
        const auto ref = type_ref_from_raw(
            read_u32(named_refs.data + index * 4));
        if (!ref)
            continue;

        type_handle decoded;
        if (!compiled.named(ref, decoded) ||
            decoded.value() != index) {
            return {status_code::artifact_corrupt};
        }
    }

    for (std::size_t index = 0;
         index < derived_index_slot_count();
         ++index) {

        build_cache_derived_index_slot slot;
        if (!derived_index_slot(index, slot).ok())
            return {status_code::artifact_corrupt};
        if (!slot.type)
            continue;

        compiled_image_canonical_type_record decoded;
        if (!compiled.canonical_type(slot.type, decoded).ok() ||
            decoded.kind != canonical_type_kind::derived) {
            return {status_code::artifact_corrupt};
        }
    }

    if (!sources.historical_index_proven) {
        for (std::size_t index = 0;
             index < compiled.type_slot_count();
             ++index) {

            const auto identity =
                compiled.type_identity_at_slot(index);
            if (!identity ||
                find_type_identity(
                    identity,
                    compiled).value() !=
                        index + 1) {
                return {status_code::artifact_corrupt};
            }
        }

        for (std::size_t index = 0;
             index < compiled.object_slot_count();
             ++index) {

            const auto identity =
                compiled.object_identity_at_slot(index);
            if (!identity ||
                find_object_identity(
                    identity,
                    compiled).value() !=
                        index + 1) {
                return {status_code::artifact_corrupt};
            }
        }

        for (std::size_t index = 0;
             index < compiled.link_slot_count();
             ++index) {

            compiled_image_link_record link;
            if (!compiled.link_raw(
                    link_handle{
                        static_cast<std::uint32_t>(
                            index + 1)},
                    link).ok() ||
                !link.target.object ||
                !link.target.member ||
                find_link_target(
                    link.target,
                    compiled).value() !=
                        index + 1) {
                return {status_code::artifact_corrupt};
            }
        }
    }

    return {};
}

status encode_build_cache_image(
    const project_context& project,
    std::vector<std::byte>& output) noexcept {

    source_change_capture empty_capture;
    return encode_build_cache_image(
        project,
        empty_capture,
        output);
}

status encode_build_cache_image(
    const project_context& project,
    const source_change_capture& change_capture,
    std::vector<std::byte>& output) noexcept {

    return encode_build_cache_image(
        project,
        change_capture,
        output,
        nullptr);
}

status encode_build_cache_image(
    const project_context& project,
    const source_change_capture& change_capture,
    std::vector<std::byte>& output,
    build_cache_encode_telemetry* telemetry,
    build_cache_encode_provenance* provenance,
    build_cache_encode_borrowed_sections* borrowed,
    build_cache_encode_sparse_sections* sparse,
    build_cache_encode_owned_sections* owned) noexcept {

    output.clear();
    if (telemetry != nullptr)
        *telemetry = {};
    if (provenance != nullptr)
        *provenance = {};
    if (borrowed != nullptr)
        *borrowed = {};
    if (sparse != nullptr)
        *sparse = {};
    if (owned != nullptr)
        owned->reset();

    build_cache_section_identity_tracker
        section_identity{provenance};

    const auto encode_begin =
        std::chrono::steady_clock::now();
    const auto layout_begin = encode_begin;

    const auto elapsed = [](
        std::chrono::steady_clock::time_point begin) noexcept {

        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin).count());
    };

    const auto& frontend = project.frontend_cache();
    const auto& contributions = project.contributions();
    if (!frontend.complete() || !contributions.complete())
        return {status_code::invalid_state};

    const auto source_count = project.sources().source_count();
    if (source_count > (std::numeric_limits<std::uint32_t>::max)() ||
        frontend.source_slots() != source_count) {
        return {status_code::initialization_failed};
    }

    const auto contribution = contributions.data_view();
    const auto graph = project.compiled_graph().build_data_view();

    if (!contribution.complete ||
        contribution.sources.size() != source_count + 1 ||
        contribution.construction.empty() ||
        graph.intrinsic_refs.size() != graph_intrinsic_type_count ||
        graph.named_refs.size() != graph.dependency_versions.size() + 1 ||
        graph.reverse_dependency_heads.size() != graph.dependency_versions.size() ||
        contribution.construction.size() !=
            graph.dependency_versions.size() + 1) {
        return {status_code::initialization_failed};
    }

    const auto source_bytes_count =
        project.sources().persistence_text_bytes();

    const auto native_sources =
        project.sources().native_generation();

    const bool native_source_proof_available =
        native_sources.complete &&
        native_sources.physical.size() == source_count;

    const auto& frontend_summary =
        frontend.persistence_summary();

    const auto frontend_count =
        frontend_summary.frontend_count;
    const auto local_type_count =
        frontend_summary.local_types;
    const auto type_slot_count =
        frontend_summary.type_slots;
    const auto object_slot_count =
        frontend_summary.object_slots;
    const auto member_slot_count =
        frontend_summary.member_slots;

    if (frontend_count > source_count)
        return {status_code::initialization_failed};

    const auto fits_u32 = [](std::size_t value) noexcept {
        return value <= (std::numeric_limits<std::uint32_t>::max)();
    };

    if (!fits_u32(local_type_count) ||
        !fits_u32(type_slot_count) ||
        !fits_u32(object_slot_count) ||
        !fits_u32(member_slot_count) ||
        !fits_u32(contribution.types.size()) ||
        !fits_u32(contribution.members.size()) ||
        !fits_u32(contribution.modifiers.size()) ||
        !fits_u32(contribution.enum_values.size()) ||
        !fits_u32(contribution.objects.size()) ||
        !fits_u32(contribution.links.size()) ||
        !fits_u32(contribution.construction.size()) ||
        !fits_u32(graph.named_refs.size()) ||
        !fits_u32(graph.derived_index.size()) ||
        !fits_u32(graph.dependency_versions.size()) ||
        !fits_u32(graph.reverse_dependency_heads.size()) ||
        !fits_u32(graph.dependency_edges.size()) ||
        !fits_u32(graph.type_identity_index.size()) ||
        !fits_u32(graph.object_identity_index.size()) ||
        !fits_u32(graph.link_target_index.size())) {
        return {status_code::not_available};
    }

    const auto* native_block_storage =
        frontend.native_frontend_block_bulk_storage();

    const bool use_native_block_storage =
        std::endian::native == std::endian::little &&
        native_block_storage != nullptr &&
        native_block_storage->local_type_count() == local_type_count &&
        native_block_storage->type_slot_count() == type_slot_count &&
        native_block_storage->object_slot_count() == object_slot_count &&
        native_block_storage->member_slot_count() == member_slot_count;

    const bool defer_native_frontend_staging =
        owned != nullptr &&
        sparse != nullptr &&
        use_native_block_storage;

    const auto native_contribution =
        contributions.native_generation();

    const bool use_native_contribution_storage =
        std::endian::native == std::endian::little &&
        native_contribution.complete &&
        native_contribution.sources.size() ==
            contribution.sources.size() &&
        native_contribution.types.size() ==
            contribution.types.size() &&
        native_contribution.members.size() ==
            contribution.members.size() &&
        native_contribution.modifiers.size() ==
            contribution.modifiers.size() &&
        native_contribution.objects.size() ==
            contribution.objects.size() &&
        native_contribution.links.size() ==
            contribution.links.size() &&
        native_contribution.construction.size() ==
            contribution.construction.size();

    const bool defer_native_contribution_staging =
        owned != nullptr &&
        sparse != nullptr &&
        use_native_contribution_storage;

    const auto native_source_directory =
        project.generation_native_segments().
            build_cache_source_directory();

    bool direct_native_source_directory = false;

    if (owned != nullptr &&
        sparse != nullptr &&
        std::endian::native == std::endian::little &&
        native_source_directory.size() == source_count) {

        const auto bytes =
            std::as_bytes(
                native_source_directory);

        project_generation_segment segment;
        if (!bytes.empty() &&
            !segment.append(bytes)) {
            return {status_code::not_available};
        }

        if (segment.size() !=
            source_count *
                source_directory_record_size) {
            return {status_code::initialization_failed};
        }

        sparse->sections[
            section_index(
                build_cache_image_section::
                    source_directory)] =
                        segment;

        direct_native_source_directory = true;

        if (telemetry != nullptr) {
            telemetry->
                native_source_directory_direct_bytes =
                    segment.size();
            telemetry->
                native_source_directory_direct_extents =
                    static_cast<std::uint32_t>(
                        segment.extent_count());
            telemetry->
                native_source_directory_direct_sections =
                    segment.empty() ? 0u : 1u;
        }
    }
    else if (telemetry != nullptr &&
             owned != nullptr &&
             sparse != nullptr) {
        telemetry->
            native_source_directory_direct_fallback = 1;
    }

    project_generation_segment
        direct_source_bytes_segment;
    bool direct_native_source_bytes = false;

    const bool direct_source_candidate =
        owned != nullptr &&
        sparse != nullptr &&
        native_source_proof_available &&
        project.sources().
            native_snapshot_text_complete();

    if (direct_source_candidate) {
        bool pages_valid = true;

        for (std::size_t page_index = 0;
             page_index <
                project.sources().
                    native_snapshot_text_page_count();
             ++page_index) {

            const auto page =
                project.sources().
                    native_snapshot_text_page(
                        page_index);

            if (!page.empty() &&
                !direct_source_bytes_segment.append(
                    page)) {
                pages_valid = false;
                break;
            }
        }

        direct_native_source_bytes =
            pages_valid &&
            direct_source_bytes_segment.size() ==
                source_bytes_count;

        if (direct_native_source_bytes) {
            sparse->sections[
                section_index(
                    build_cache_image_section::
                        source_bytes)] =
                            direct_source_bytes_segment;

            if (telemetry != nullptr) {
                telemetry->native_source_direct_bytes =
                    direct_source_bytes_segment.size();
                telemetry->native_source_direct_extents =
                    static_cast<std::uint32_t>(
                        direct_source_bytes_segment.
                            extent_count());
                telemetry->native_source_direct_sections =
                    source_bytes_count != 0 ? 1u : 0u;
            }
        }
        else if (telemetry != nullptr) {
            telemetry->native_source_direct_fallback = 1;
        }
    }
    else if (telemetry != nullptr &&
             owned != nullptr &&
             sparse != nullptr) {
        telemetry->native_source_direct_fallback = 1;
    }

    std::array<layout_section, build_cache_image_directory_count> layout{{
        {build_cache_image_section::source_directory,
            source_directory_record_size, source_count},
        {build_cache_image_section::source_bytes,
            1, source_bytes_count},
        {build_cache_image_section::frontend_local_types,
            frontend_local_type_record_size, local_type_count},
        {build_cache_image_section::frontend_type_slots,
            frontend_type_slot_record_size, type_slot_count},
        {build_cache_image_section::frontend_object_slots,
            frontend_object_slot_record_size, object_slot_count},
        {build_cache_image_section::frontend_member_slots,
            frontend_member_slot_record_size, member_slot_count},
        {build_cache_image_section::contribution_states,
            contribution_state_record_size, contribution.sources.size()},
        {build_cache_image_section::contribution_types,
            contribution_type_record_size, contribution.types.size()},
        {build_cache_image_section::contribution_members,
            contribution_member_record_size, contribution.members.size()},
        {build_cache_image_section::contribution_modifiers,
            contribution_modifier_record_size, contribution.modifiers.size()},
        {build_cache_image_section::contribution_enum_values,
            contribution_enum_value_record_size, contribution.enum_values.size()},
        {build_cache_image_section::contribution_objects,
            contribution_object_record_size, contribution.objects.size()},
        {build_cache_image_section::contribution_links,
            contribution_link_record_size, contribution.links.size()},
        {build_cache_image_section::construction_states,
            construction_state_record_size, contribution.construction.size()},
        {build_cache_image_section::graph_intrinsic_refs,
            type_ref_record_size, graph.intrinsic_refs.size()},
        {build_cache_image_section::graph_named_refs,
            type_ref_record_size, graph.named_refs.size()},
        {build_cache_image_section::graph_derived_index,
            derived_index_record_size, graph.derived_index.size()},
        {build_cache_image_section::graph_dependency_versions,
            u32_record_size, graph.dependency_versions.size()},
        {build_cache_image_section::graph_reverse_dependency_heads,
            u32_record_size, graph.reverse_dependency_heads.size()},
        {build_cache_image_section::graph_dependency_edges,
            dependency_edge_record_size, graph.dependency_edges.size()},
        {build_cache_image_section::graph_type_identity_index,
            historical_index_record_size, graph.type_identity_index.size()},
        {build_cache_image_section::graph_object_identity_index,
            historical_index_record_size, graph.object_identity_index.size()},
        {build_cache_image_section::graph_link_target_index,
            historical_index_record_size, graph.link_target_index.size()},
        {build_cache_image_section::source_file_identity_index,
            source_file_identity_index_record_size,
            change_capture.file_index.size()},
        {build_cache_image_section::tracked_directory_identity_index,
            tracked_directory_identity_index_record_size,
            change_capture.directory_index.size()},
    }};

    std::uint64_t cursor = first_section_offset;
    for (auto& value : layout) {
        cursor = align64(cursor);
        value.offset = cursor;

        std::uint64_t byte_count = 0;
        if (!multiply_u64(
                value.count,
                value.record_size,
                byte_count) ||
            !add_u64(cursor, byte_count, cursor)) {
            return {status_code::not_available};
        }
    }

    if (cursor > (std::numeric_limits<std::size_t>::max)())
        return {status_code::not_available};

    const auto logical_size =
        static_cast<std::size_t>(cursor);

    try {
        if (owned != nullptr) {
            // R5E2D-B: section-owned Build Cache staging. The legacy vector
            // owns only the immutable prefix; logical sections own exact bytes.
            output.assign(
                build_cache_image_prefix_size,
                std::byte{0});

            owned->logical_size = logical_size;

            for (std::size_t index = 0;
                 index < layout.size();
                 ++index) {

                const auto& value = layout[index];
                std::uint64_t byte_count = 0;
                if (!multiply_u64(
                        value.count,
                        value.record_size,
                        byte_count) ||
                    byte_count >
                        (std::numeric_limits<std::size_t>::max)()) {
                    return {status_code::not_available};
                }

                const bool native_source_directory_section =
                    index == section_index(
                        build_cache_image_section::source_directory);

                const bool native_source_section =
                    index == section_index(
                        build_cache_image_section::source_bytes);

                const bool frontend_section =
                    index >= section_index(
                        build_cache_image_section::frontend_local_types) &&
                    index <= section_index(
                        build_cache_image_section::frontend_member_slots);

                const bool native_contribution_section =
                    index == section_index(
                        build_cache_image_section::contribution_states) ||
                    index == section_index(
                        build_cache_image_section::contribution_types) ||
                    index == section_index(
                        build_cache_image_section::contribution_members) ||
                    index == section_index(
                        build_cache_image_section::contribution_modifiers) ||
                    index == section_index(
                        build_cache_image_section::contribution_objects) ||
                    index == section_index(
                        build_cache_image_section::contribution_links) ||
                    index == section_index(
                        build_cache_image_section::construction_states);

                auto& section = owned->sections[index];

                if ((direct_native_source_directory &&
                     native_source_directory_section) ||
                    (direct_native_source_bytes &&
                     native_source_section) ||
                    (defer_native_frontend_staging &&
                     frontend_section) ||
                    (defer_native_contribution_staging &&
                     native_contribution_section)) {
                    section.clear();
                    continue;
                }

                section.assign(
                    static_cast<std::size_t>(byte_count),
                    std::byte{0});
            }
        }
        else {
            output.assign(
                logical_size,
                std::byte{0});
        }
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }

    auto* base = output.data();
    const auto section_data =
        [&](build_cache_image_section kind) noexcept {
            const auto index = section_index(kind);

            if (owned != nullptr) {
                auto& section = owned->sections[index];
                return section.empty()
                    ? base
                    : section.data();
            }

            return base + static_cast<std::size_t>(
                layout[index].offset);
        };

    const auto* baseline_build_cache =
        frontend.baseline_persistence_image();

    const bool use_mapped_baseline_sections =
        std::endian::native == std::endian::little &&
        baseline_build_cache != nullptr &&
        baseline_build_cache->valid();

    const auto encode_mapped_baseline_section =
        [&](build_cache_image_section kind,
            const auto& values,
            std::size_t record_size,
            const auto& write_record) noexcept {

            const auto direct_index = section_index(kind);
            if (sparse != nullptr &&
                direct_index < sparse->sections.size() &&
                !sparse->sections[direct_index].empty()) {
                return true;
            }

            if (!use_mapped_baseline_sections)
                return false;

            const auto baseline_count =
                values.baseline_size();
            const auto local_values =
                values.local_values();

            if (values.size() !=
                baseline_count + local_values.size()) {
                return false;
            }

            if (record_size != 0 &&
                baseline_count >
                    (std::numeric_limits<std::size_t>::max)() /
                        record_size) {
                return false;
            }

            const auto baseline_bytes =
                baseline_build_cache->section_bytes(kind);
            const auto expected_baseline_bytes =
                baseline_count * record_size;

            if (baseline_bytes.size() !=
                expected_baseline_bytes) {
                return false;
            }

            bool patches_valid = true;
            bool patches_exact = true;
            std::size_t patch_records = 0;
            std::array<std::byte, 64> encoded_patch{};

            if (record_size > encoded_patch.size())
                return false;

            values.for_each_materialized(
                [&](std::size_t index,
                    const auto& value) noexcept {

                    if (index >= baseline_count) {
                        patches_valid = false;
                        return;
                    }

                    ++patch_records;
                    encoded_patch.fill(std::byte{0});
                    write_record(encoded_patch.data(), value);

                    const auto old =
                        baseline_bytes.subspan(
                            index * record_size,
                            record_size);

                    if (std::memcmp(
                            encoded_patch.data(),
                            old.data(),
                            record_size) != 0) {
                        patches_exact = false;
                    }
                });

            if (!patches_valid)
                return false;

            const bool whole_section_exact =
                local_values.empty() &&
                patches_exact;

            if (whole_section_exact &&
                borrowed != nullptr &&
                !baseline_bytes.empty()) {

                const auto borrow_index =
                    section_index(kind);

                if (borrow_index >= borrowed->sections.size())
                    return false;

                borrowed->sections[borrow_index] =
                    baseline_bytes;

                section_identity.establish_exact(kind);

                if (telemetry != nullptr) {
                    telemetry->mapped_baseline_bulk_bytes +=
                        baseline_bytes.size();
                    telemetry->mapped_baseline_borrowed_bytes +=
                        baseline_bytes.size();
                    telemetry->mapped_baseline_patch_records +=
                        patch_records;
                    ++telemetry->mapped_baseline_bulk_sections;
                    ++telemetry->mapped_baseline_borrowed_sections;
                }

                return true;
            }

            auto* target = section_data(kind);

            if (!baseline_bytes.empty()) {
                std::memcpy(
                    target,
                    baseline_bytes.data(),
                    baseline_bytes.size());
            }

            section_identity.establish_exact(kind);

            if (!local_values.empty())
                section_identity.invalidate(kind);

            values.for_each_materialized(
                [&](std::size_t index,
                    const auto& value) noexcept {

                    auto* record =
                        target + index * record_size;

                    write_record(
                        record,
                        value);

                    section_identity.observe_overwrite(
                        kind,
                        std::span<const std::byte>{
                            record,
                            record_size},
                        baseline_bytes.subspan(
                            index * record_size,
                            record_size));
                });

            for (std::size_t index = 0;
                 index < local_values.size();
                 ++index) {

                write_record(
                    target +
                        (baseline_count + index) *
                            record_size,
                    local_values[index]);
            }

            if (telemetry != nullptr) {
                telemetry->mapped_baseline_bulk_bytes +=
                    baseline_bytes.size();
                telemetry->mapped_baseline_patch_records +=
                    patch_records;
                telemetry->mapped_baseline_append_records +=
                    local_values.size();
                ++telemetry->mapped_baseline_bulk_sections;
            }

            return true;
        };

    if (telemetry != nullptr)
        telemetry->layout_allocate_ns =
            elapsed(layout_begin);

    const auto source_frontend_begin =
        std::chrono::steady_clock::now();

    auto* source_directory =
        section_data(build_cache_image_section::source_directory);
    auto* source_bytes =
        section_data(build_cache_image_section::source_bytes);
    auto* local_types =
        section_data(build_cache_image_section::frontend_local_types);
    auto* type_slots =
        section_data(build_cache_image_section::frontend_type_slots);
    auto* object_slots =
        section_data(build_cache_image_section::frontend_object_slots);
    auto* member_slots =
        section_data(build_cache_image_section::frontend_member_slots);

    std::uint64_t text_cursor = 0;
    std::uint32_t local_type_cursor = 0;
    std::uint32_t type_slot_cursor = 0;
    std::uint32_t object_slot_cursor = 0;
    std::uint32_t member_slot_cursor = 0;
    std::size_t observed_frontends = 0;

    if (telemetry != nullptr)
        telemetry->source_frontend_total_sources = source_count;

    const auto native_frontend =
        frontend.native_persistence_view();
    const bool use_native_frontend =
        static_cast<bool>(native_frontend) &&
        native_frontend.size() == source_count;

    // R5E2B_DIRECT_FRONTEND_SECTIONS
    // Fresh G0 Frontend payload is already persistence-native inside immutable
    // Generation chunks. Prefer those chunks as the exact Build Cache section
    // carriers. The staging buffer remains only a layout/prefix + fallback
    // owner for sections that have not migrated yet.
    const auto build_native_frontend_segment =
        [&](std::size_t page_count,
            const auto& page_at,
            std::size_t expected_count,
            std::size_t record_size,
            project_generation_segment& output_segment) noexcept {

            output_segment = {};

            if (record_size == 0 ||
                expected_count >
                    (std::numeric_limits<std::size_t>::max)() /
                        record_size) {
                return false;
            }

            const auto expected_bytes =
                expected_count * record_size;
            std::size_t observed_count = 0;

            for (std::size_t page_index = 0;
                 page_index < page_count;
                 ++page_index) {

                const auto page = page_at(page_index);

                if (observed_count > expected_count ||
                    page.size() >
                        expected_count - observed_count) {
                    return false;
                }

                if (!page.empty() &&
                    !output_segment.append(
                        std::as_bytes(page))) {
                    return false;
                }

                observed_count += page.size();
            }

            return observed_count == expected_count &&
                output_segment.size() == expected_bytes;
        };

    const auto copy_native_pages =
        [&](std::size_t page_count,
            const auto& page_at,
            std::byte* target,
            std::size_t expected_count,
            std::size_t record_size) noexcept {

            std::size_t cursor = 0;

            for (std::size_t page_index = 0;
                 page_index < page_count;
                 ++page_index) {

                const auto page = page_at(page_index);

                if (cursor > expected_count ||
                    page.size() > expected_count - cursor) {
                    return false;
                }

                if (!page.empty()) {
                    std::memcpy(
                        target + cursor * record_size,
                        page.data(),
                        page.size_bytes());
                }

                cursor += page.size();
            }

            return cursor == expected_count;
        };

    bool direct_native_contribution = false;

    if (defer_native_contribution_staging) {
        const auto bind_direct_contribution =
            [&](build_cache_image_section kind,
                const auto& values) noexcept {

                const auto index = section_index(kind);
                if (index >= sparse->sections.size())
                    return false;

                const auto bytes = std::as_bytes(values);

                if (bytes.empty()) {
                    sparse->sections[index] = {};
                    return true;
                }

                project_generation_segment segment;
                if (!segment.append(bytes))
                    return false;

                sparse->sections[index] = segment;
                return true;
            };

        direct_native_contribution =
            bind_direct_contribution(
                build_cache_image_section::contribution_states,
                native_contribution.sources) &&
            bind_direct_contribution(
                build_cache_image_section::contribution_types,
                native_contribution.types) &&
            bind_direct_contribution(
                build_cache_image_section::contribution_members,
                native_contribution.members) &&
            bind_direct_contribution(
                build_cache_image_section::contribution_modifiers,
                native_contribution.modifiers) &&
            bind_direct_contribution(
                build_cache_image_section::contribution_objects,
                native_contribution.objects) &&
            bind_direct_contribution(
                build_cache_image_section::contribution_links,
                native_contribution.links) &&
            bind_direct_contribution(
                build_cache_image_section::construction_states,
                native_contribution.construction);

        if (!direct_native_contribution)
            return {status_code::initialization_failed};

        if (telemetry != nullptr) {
            telemetry->native_contribution_direct_bytes =
                native_contribution.sources.size_bytes() +
                native_contribution.types.size_bytes() +
                native_contribution.members.size_bytes() +
                native_contribution.modifiers.size_bytes() +
                native_contribution.objects.size_bytes() +
                native_contribution.links.size_bytes() +
                native_contribution.construction.size_bytes();

            telemetry->native_contribution_direct_sections =
                static_cast<std::uint32_t>(
                    (!native_contribution.sources.empty() ? 1 : 0) +
                    (!native_contribution.types.empty() ? 1 : 0) +
                    (!native_contribution.members.empty() ? 1 : 0) +
                    (!native_contribution.modifiers.empty() ? 1 : 0) +
                    (!native_contribution.objects.empty() ? 1 : 0) +
                    (!native_contribution.links.empty() ? 1 : 0) +
                    (!native_contribution.construction.empty() ? 1 : 0));

            telemetry->native_contribution_direct_extents =
                telemetry->native_contribution_direct_sections;
        }
    }
    else if (telemetry != nullptr &&
             owned != nullptr &&
             sparse != nullptr) {
        telemetry->native_contribution_direct_fallback = 1;
    }

    bool direct_native_frontend = false;

    if (use_native_block_storage &&
        sparse != nullptr) {

        project_generation_segment direct_local_types;
        project_generation_segment direct_type_slots;
        project_generation_segment direct_object_slots;
        project_generation_segment direct_member_slots;

        const bool direct_geometry_valid =
            build_native_frontend_segment(
                native_block_storage->local_type_page_count(),
                [&](std::size_t index) noexcept {
                    return native_block_storage->local_type_page(index);
                },
                local_type_count,
                frontend_local_type_record_size,
                direct_local_types) &&
            build_native_frontend_segment(
                native_block_storage->type_slot_page_count(),
                [&](std::size_t index) noexcept {
                    return native_block_storage->type_slot_page(index);
                },
                type_slot_count,
                frontend_type_slot_record_size,
                direct_type_slots) &&
            build_native_frontend_segment(
                native_block_storage->object_slot_page_count(),
                [&](std::size_t index) noexcept {
                    return native_block_storage->object_slot_page(index);
                },
                object_slot_count,
                frontend_object_slot_record_size,
                direct_object_slots) &&
            build_native_frontend_segment(
                native_block_storage->member_slot_page_count(),
                [&](std::size_t index) noexcept {
                    return native_block_storage->member_slot_page(index);
                },
                member_slot_count,
                frontend_member_slot_record_size,
                direct_member_slots);

        std::size_t direct_extent_count = 0;
        const auto add_extent_count =
            [&](const project_generation_segment& segment) noexcept {

                if (segment.extent_count() >
                    (std::numeric_limits<std::size_t>::max)() -
                        direct_extent_count) {
                    return false;
                }

                direct_extent_count +=
                    segment.extent_count();
                return true;
            };

        const bool extent_count_valid =
            direct_geometry_valid &&
            add_extent_count(direct_local_types) &&
            add_extent_count(direct_type_slots) &&
            add_extent_count(direct_object_slots) &&
            add_extent_count(direct_member_slots);

        // R5E2D-B/R5E2D-A: reserve one extent for every non-direct
        // section plus prefix and every possible canonical alignment gap.
        constexpr std::size_t direct_carrier_reserve =
            1 +
            (build_cache_image_directory_count - 4) +
            build_cache_image_directory_count;

        static_assert(
            project_generation_segment_max_extents >
                direct_carrier_reserve);

        if (extent_count_valid &&
            direct_extent_count <=
                project_generation_segment_max_extents -
                    direct_carrier_reserve) {

            const auto bind_direct =
                [&](build_cache_image_section kind,
                    const project_generation_segment& segment) noexcept {

                    const auto index = section_index(kind);
                    if (index >= sparse->sections.size())
                        return false;

                    const auto& value = layout[index];

                    std::uint64_t expected_bytes = 0;
                    if (!multiply_u64(
                            value.count,
                            value.record_size,
                            expected_bytes) ||
                        expected_bytes !=
                            segment.size()) {
                        return false;
                    }

                    // Empty logical sections need no physical carrier.
                    if (!segment.empty())
                        sparse->sections[index] = segment;

                    return true;
                };

            direct_native_frontend =
                bind_direct(
                    build_cache_image_section::
                        frontend_local_types,
                    direct_local_types) &&
                bind_direct(
                    build_cache_image_section::
                        frontend_type_slots,
                    direct_type_slots) &&
                bind_direct(
                    build_cache_image_section::
                        frontend_object_slots,
                    direct_object_slots) &&
                bind_direct(
                    build_cache_image_section::
                        frontend_member_slots,
                    direct_member_slots);

            if (!direct_native_frontend) {
                sparse->sections[
                    section_index(
                        build_cache_image_section::
                            frontend_local_types)] = {};
                sparse->sections[
                    section_index(
                        build_cache_image_section::
                            frontend_type_slots)] = {};
                sparse->sections[
                    section_index(
                        build_cache_image_section::
                            frontend_object_slots)] = {};
                sparse->sections[
                    section_index(
                        build_cache_image_section::
                            frontend_member_slots)] = {};
            }
            else if (telemetry != nullptr) {
                telemetry->native_frontend_direct_bytes =
                    direct_local_types.size() +
                    direct_type_slots.size() +
                    direct_object_slots.size() +
                    direct_member_slots.size();
                telemetry->native_frontend_direct_extents =
                    static_cast<std::uint32_t>(
                        direct_extent_count);
                telemetry->native_frontend_direct_sections =
                    static_cast<std::uint32_t>(
                        (!direct_local_types.empty() ? 1 : 0) +
                        (!direct_type_slots.empty() ? 1 : 0) +
                        (!direct_object_slots.empty() ? 1 : 0) +
                        (!direct_member_slots.empty() ? 1 : 0));
            }
        }
        else if (telemetry != nullptr) {
            telemetry->native_frontend_direct_fallback = 1;
        }
    }

    if (use_native_block_storage &&
        !direct_native_frontend) {

        if (telemetry != nullptr &&
            sparse != nullptr) {
            telemetry->native_frontend_direct_fallback = 1;
        }

        // R5E2D-B_FRONTEND_FALLBACK_STORAGE
        if (defer_native_frontend_staging) {
            try {
                constexpr std::array<
                    build_cache_image_section,
                    4> frontend_sections{{
                    build_cache_image_section::frontend_local_types,
                    build_cache_image_section::frontend_type_slots,
                    build_cache_image_section::frontend_object_slots,
                    build_cache_image_section::frontend_member_slots,
                }};

                for (const auto kind : frontend_sections) {
                    const auto index = section_index(kind);
                    const auto& value = layout[index];

                    std::uint64_t byte_count = 0;
                    if (!multiply_u64(
                            value.count,
                            value.record_size,
                            byte_count) ||
                        byte_count >
                            (std::numeric_limits<std::size_t>::max)()) {
                        return {status_code::not_available};
                    }

                    owned->sections[index].assign(
                        static_cast<std::size_t>(byte_count),
                        std::byte{0});
                }
            }
            catch (const std::bad_alloc&) {
                return {status_code::not_available};
            }
            catch (const std::length_error&) {
                return {status_code::not_available};
            }

            local_types =
                section_data(
                    build_cache_image_section::frontend_local_types);
            type_slots =
                section_data(
                    build_cache_image_section::frontend_type_slots);
            object_slots =
                section_data(
                    build_cache_image_section::frontend_object_slots);
            member_slots =
                section_data(
                    build_cache_image_section::frontend_member_slots);
        }

        if (!copy_native_pages(
                native_block_storage->local_type_page_count(),
                [&](std::size_t index) noexcept {
                    return native_block_storage->local_type_page(index);
                },
                local_types,
                local_type_count,
                frontend_local_type_record_size) ||
            !copy_native_pages(
                native_block_storage->type_slot_page_count(),
                [&](std::size_t index) noexcept {
                    return native_block_storage->type_slot_page(index);
                },
                type_slots,
                type_slot_count,
                frontend_type_slot_record_size) ||
            !copy_native_pages(
                native_block_storage->object_slot_page_count(),
                [&](std::size_t index) noexcept {
                    return native_block_storage->object_slot_page(index);
                },
                object_slots,
                object_slot_count,
                frontend_object_slot_record_size) ||
            !copy_native_pages(
                native_block_storage->member_slot_page_count(),
                [&](std::size_t index) noexcept {
                    return native_block_storage->member_slot_page(index);
                },
                member_slots,
                member_slot_count,
                frontend_member_slot_record_size)) {

            return {status_code::initialization_failed};
        }
    }

    const auto read_native_block_layout =
        [&](source_id source_value,
            source_frontend_block_layout& layout_value,
            bool& present) noexcept {

            layout_value = {};
            present = false;

            if (!use_native_block_storage)
                return status{status_code::not_available};

            source_frontend_block_ref block;
            auto result =
                frontend.native_frontend_block(
                    source_value,
                    block);

            if (!result.ok()) {
                if (result.code == status_code::not_found)
                    return status{};
                return result;
            }

            result =
                native_block_storage->layout(
                    block,
                    layout_value);

            if (!result.ok())
                return result;

            if (layout_value.source != source_value)
                return status{status_code::initialization_failed};

            present = true;
            return status{};
        };

    // GEN-02C24.1: production cleanup keeps the C24 fast path while removing
    // temporary C23 phase timing from the SAVE hot path.
    // GEN-02C24: parallel native Source persistence is enabled only for a
    // complete fresh G0. Sparse/baseline-backed Generations remain on the
    // canonical sequential path below.
    constexpr std::size_t max_native_source_workers = 8;
    constexpr std::size_t minimum_sources_per_worker = 8192;

    bool native_source_encoded = false;

    // D4G2: baseline-backed Source/Frontend reconstruction.
    //
    // Build Cache v4 stores Source text and Frontend arenas compacted in
    // source_id order. Sparse BUILD therefore reconstructs those sections from
    // unchanged baseline runs plus sparse replacements instead of decoding all
    // untouched Sources one by one.
    const auto* generation_change_capture =
        project.generation_provenance().source_change();

    const bool sparse_source_frontend_candidate =
        !native_source_encoded &&
        use_mapped_baseline_sections &&
        frontend.baseline_backed() &&
        generation_change_capture != nullptr &&
        generation_change_capture->baseline_overlay();

    if (sparse_source_frontend_candidate) {
        const auto clear_source_frontend_sections =
            [&]() noexcept {

                const auto clear_section =
                    [&](build_cache_image_section kind) noexcept {

                        // D4O2C: any abandoned sparse reconstruction proof is
                        // invalid before the canonical fallback rewrites bytes.
                        section_identity.invalidate(kind);

                        const auto& value =
                            layout[section_index(kind)];

                        std::uint64_t byte_count = 0;
                        if (!multiply_u64(
                                value.count,
                                value.record_size,
                                byte_count) ||
                            byte_count >
                                (std::numeric_limits<
                                    std::size_t>::max)()) {
                            return;
                        }

                        if (byte_count != 0) {
                            std::memset(
                                section_data(kind),
                                0,
                                static_cast<std::size_t>(
                                    byte_count));
                        }
                    };

                clear_section(
                    build_cache_image_section::source_directory);
                clear_section(
                    build_cache_image_section::source_bytes);
                clear_section(
                    build_cache_image_section::frontend_local_types);
                clear_section(
                    build_cache_image_section::frontend_type_slots);
                clear_section(
                    build_cache_image_section::frontend_object_slots);
                clear_section(
                    build_cache_image_section::frontend_member_slots);
            };

        const auto try_sparse_source_frontend =
            [&]() -> bool {

                try {
                    const auto baseline_source_count =
                        baseline_build_cache->source_count();

                    if (baseline_source_count > source_count ||
                        source_count >
                            (std::numeric_limits<
                                std::uint32_t>::max)()) {
                        return false;
                    }

                    const auto baseline_directory =
                        baseline_build_cache->section_bytes(
                            build_cache_image_section::
                                source_directory);
                    const auto baseline_source_bytes =
                        baseline_build_cache->section_bytes(
                            build_cache_image_section::
                                source_bytes);

                    std::uint64_t expected_directory_bytes = 0;
                    if (!multiply_u64(
                            baseline_source_count,
                            source_directory_record_size,
                            expected_directory_bytes) ||
                        expected_directory_bytes !=
                            baseline_directory.size() ||
                        baseline_source_bytes.size() !=
                            baseline_build_cache->
                                source_bytes_count()) {
                        return false;
                    }

                    struct sparse_source_index final {
                        std::uint32_t frontend_overlay = 0;
                        bool text_candidate = false;
                    };

                    std::vector<sparse_source_index>
                        sparse_index(source_count + 1);

                    for (const auto& update :
                         generation_change_capture->
                            file_updates) {

                        if (!update.source ||
                            static_cast<std::size_t>(
                                update.source.value()) >
                                source_count ||
                            update.reserved != 0) {
                            return false;
                        }

                        sparse_index[
                            update.source.value()].
                                text_candidate = true;
                    }

                    const auto overlay_count =
                        frontend.persistence_overlay_count();

                    if (overlay_count >
                        (std::numeric_limits<
                            std::uint32_t>::max)()) {
                        return false;
                    }

                    std::vector<
                        source_frontend_persistence_view>
                            overlay_views(overlay_count);

                    for (std::size_t index = 0;
                         index < overlay_count;
                         ++index) {

                        source_id source_value;
                        const auto result =
                            frontend.persistence_overlay(
                                index,
                                source_value,
                                overlay_views[index]);

                        if (!result.ok() ||
                            !source_value ||
                            static_cast<std::size_t>(
                                source_value.value()) >
                                source_count) {
                            return false;
                        }

                        auto& slot =
                            sparse_index[
                                source_value.value()];

                        if (slot.frontend_overlay != 0) {
                            return false;
                        }

                        slot.frontend_overlay =
                            static_cast<std::uint32_t>(
                                index + 1);

                        // Overlay entries accumulate across sparse BUILDs. A
                        // retained overlay therefore also proves this Source is
                        // safe to resolve directly without touching unrelated
                        // baseline Sources.
                        slot.text_candidate = true;
                    }

                    // D4Q2C2B_SOURCE_DIRECTORY_PARTIAL_ZERO_COPY
                    project_generation_segment
                        sparse_source_directory;
                    std::uint64_t
                        sparse_source_directory_borrowed = 0;
                    std::uint32_t
                        sparse_source_directory_borrowed_extents = 0;
                    std::size_t
                        directory_borrow_begin = 0;
                    std::size_t
                        directory_borrow_count = 0;
                    std::size_t
                        directory_owned_begin = 0;
                    std::size_t
                        directory_owned_count = 0;
                    bool directory_force_owned_tail = false;

                    // D4Q2C2B_DIRECTORY_RUN_COALESCE_AND_BOUND
                    const bool use_sparse_source_directory =
                        sparse != nullptr;

                    if (!use_sparse_source_directory &&
                        !baseline_directory.empty()) {
                        std::memcpy(
                            source_directory,
                            baseline_directory.data(),
                            baseline_directory.size());
                    }

                    std::uint64_t baseline_bytes_copied =
                        use_sparse_source_directory
                        ? 0
                        : baseline_directory.size();

                    const auto flush_directory_borrow =
                        [&]() noexcept {

                            if (directory_borrow_count == 0)
                                return true;

                            if (directory_borrow_begin >
                                    (std::numeric_limits<
                                        std::size_t>::max)() /
                                        source_directory_record_size ||
                                directory_borrow_count >
                                    (std::numeric_limits<
                                        std::size_t>::max)() /
                                        source_directory_record_size) {
                                return false;
                            }

                            const auto offset =
                                directory_borrow_begin *
                                source_directory_record_size;
                            const auto bytes =
                                directory_borrow_count *
                                source_directory_record_size;

                            if (offset >
                                    baseline_directory.size() ||
                                bytes >
                                    baseline_directory.size() -
                                        offset) {
                                return false;
                            }

                            const auto run =
                                baseline_directory.subspan(
                                    offset,
                                    bytes);

                            if (!sparse_source_directory.append(
                                    run)) {
                                return false;
                            }

                            sparse_source_directory_borrowed +=
                                run.size();
                            ++sparse_source_directory_borrowed_extents;
                            directory_borrow_count = 0;
                            return true;
                        };

                    const auto flush_directory_owned =
                        [&]() noexcept {

                            if (directory_owned_count == 0)
                                return true;

                            if (directory_owned_begin >
                                    (std::numeric_limits<
                                        std::size_t>::max)() /
                                        source_directory_record_size ||
                                directory_owned_count >
                                    (std::numeric_limits<
                                        std::size_t>::max)() /
                                        source_directory_record_size) {
                                return false;
                            }

                            const auto offset =
                                directory_owned_begin *
                                source_directory_record_size;
                            const auto bytes =
                                directory_owned_count *
                                source_directory_record_size;
                            const auto directory_bytes =
                                source_count *
                                source_directory_record_size;

                            if (offset > directory_bytes ||
                                bytes > directory_bytes - offset) {
                                return false;
                            }

                            if (!sparse_source_directory.append(
                                    std::span<const std::byte>{
                                        source_directory + offset,
                                        bytes})) {
                                return false;
                            }

                            directory_owned_count = 0;
                            return true;
                        };

                    // D4Q2C1_SOURCE_FRONTEND_WHOLE_SECTION_BORROW
                    struct sparse_arena_state final {
                        build_cache_image_section kind{};
                        std::span<const std::byte> baseline;
                        std::byte* output = nullptr;
                        std::size_t record_size = 0;
                        std::size_t baseline_count = 0;
                        std::size_t current_count = 0;
                        std::size_t logical_cursor = 0;
                        std::size_t copy_cursor = 0;
                        std::size_t output_cursor = 0;
                        project_generation_segment sparse_segment;
                        std::size_t owned_run_begin = 0;
                        std::size_t owned_run_bytes = 0;
                        std::uint64_t borrowed_bytes = 0;
                        std::uint64_t owned_bytes = 0;
                        std::uint32_t borrowed_extents = 0;
                        std::uint32_t owned_extents = 0;
                        bool borrow_candidate = false;
                        bool whole_materialized = false;
                    };

                    std::array<sparse_arena_state, 4>
                        arenas{{
                            {
                                build_cache_image_section::
                                    frontend_local_types,
                                baseline_build_cache->
                                    section_bytes(
                                        build_cache_image_section::
                                            frontend_local_types),
                                local_types,
                                frontend_local_type_record_size,
                                baseline_build_cache->
                                    frontend_local_type_count(),
                                local_type_count,
                            },
                            {
                                build_cache_image_section::
                                    frontend_type_slots,
                                baseline_build_cache->
                                    section_bytes(
                                        build_cache_image_section::
                                            frontend_type_slots),
                                type_slots,
                                frontend_type_slot_record_size,
                                baseline_build_cache->
                                    frontend_type_slot_count(),
                                type_slot_count,
                            },
                            {
                                build_cache_image_section::
                                    frontend_object_slots,
                                baseline_build_cache->
                                    section_bytes(
                                        build_cache_image_section::
                                            frontend_object_slots),
                                object_slots,
                                frontend_object_slot_record_size,
                                baseline_build_cache->
                                    frontend_object_slot_count(),
                                object_slot_count,
                            },
                            {
                                build_cache_image_section::
                                    frontend_member_slots,
                                baseline_build_cache->
                                    section_bytes(
                                        build_cache_image_section::
                                            frontend_member_slots),
                                member_slots,
                                frontend_member_slot_record_size,
                                baseline_build_cache->
                                    frontend_member_slot_count(),
                                member_slot_count,
                            },
                        }};

                    const bool use_sparse_frontend_arenas =
                        sparse != nullptr;

                    for (auto& arena : arenas) {
                        if (arena.record_size == 0 ||
                            arena.baseline_count >
                                (std::numeric_limits<
                                    std::size_t>::max)() /
                                    arena.record_size ||
                            arena.baseline.size() !=
                                arena.baseline_count *
                                    arena.record_size) {
                            return false;
                        }

                        // Equal geometry is logically identical before
                        // sparse replacements are applied. Delay the baseline
                        // memcpy while a pinned whole-section borrow remains
                        // possible.
                        if (arena.baseline_count ==
                            arena.current_count) {

                            section_identity.establish_exact(
                                arena.kind);

                            arena.borrow_candidate =
                                borrowed != nullptr &&
                                !arena.baseline.empty();

                            if (!arena.borrow_candidate &&
                                !use_sparse_frontend_arenas) {

                                if (!arena.baseline.empty()) {
                                    std::memcpy(
                                        arena.output,
                                        arena.baseline.data(),
                                        arena.baseline.size());

                                    baseline_bytes_copied +=
                                        arena.baseline.size();
                                }

                                arena.whole_materialized = true;
                            }
                        }
                    }

                    const auto materialize_equal_arena =
                        [&](sparse_arena_state& arena) noexcept {

                            if (arena.baseline_count !=
                                    arena.current_count ||
                                arena.whole_materialized) {
                                return;
                            }

                            arena.borrow_candidate = false;

                            if (use_sparse_frontend_arenas)
                                return;

                            if (!arena.baseline.empty()) {
                                std::memcpy(
                                    arena.output,
                                    arena.baseline.data(),
                                    arena.baseline.size());

                                baseline_bytes_copied +=
                                    arena.baseline.size();
                            }

                            arena.whole_materialized = true;
                        };

                    const auto flush_arena_owned =
                        [&](sparse_arena_state& arena) noexcept {

                            if (arena.owned_run_bytes == 0)
                                return true;

                            if (arena.record_size == 0 ||
                                arena.current_count >
                                    (std::numeric_limits<
                                        std::size_t>::max)() /
                                        arena.record_size) {
                                return false;
                            }

                            const auto section_bytes =
                                arena.current_count *
                                arena.record_size;

                            if (arena.owned_run_begin >
                                    section_bytes ||
                                arena.owned_run_bytes >
                                    section_bytes -
                                        arena.owned_run_begin) {
                                return false;
                            }

                            const auto run =
                                std::span<const std::byte>{
                                    arena.output +
                                        arena.owned_run_begin,
                                    arena.owned_run_bytes};

                            if (!arena.sparse_segment.append(run))
                                return false;

                            arena.owned_bytes += run.size();
                            ++arena.owned_extents;
                            arena.owned_run_begin = 0;
                            arena.owned_run_bytes = 0;
                            return true;
                        };

                    const auto copy_arena_until =
                        [&](sparse_arena_state& arena,
                            std::size_t target) noexcept {

                            if (target <
                                    arena.copy_cursor ||
                                target >
                                    arena.baseline_count ||
                                arena.output_cursor >
                                    arena.current_count) {
                                return false;
                            }

                            const auto records =
                                target -
                                arena.copy_cursor;

                            if (records >
                                arena.current_count -
                                    arena.output_cursor) {
                                return false;
                            }

                            const auto bytes =
                                records *
                                arena.record_size;

                            if (bytes != 0) {
                                const auto output_offset =
                                    arena.output_cursor *
                                    arena.record_size;
                                const auto baseline_offset =
                                    arena.copy_cursor *
                                    arena.record_size;

                                if (use_sparse_frontend_arenas) {
                                    if (!flush_arena_owned(arena))
                                        return false;

                                    const auto baseline_run =
                                        arena.baseline.subspan(
                                            baseline_offset,
                                            bytes);

                                    if (!arena.sparse_segment.append(
                                            baseline_run)) {
                                        return false;
                                    }

                                    arena.borrowed_bytes += bytes;
                                    ++arena.borrowed_extents;
                                }
                                else {
                                    // Equal-geometry arenas were copied
                                    // wholesale once above. A baseline run that
                                    // remains at the same offset is already
                                    // present; shifted runs are copied only over
                                    // the bytes actually moved.
                                    const bool already_exact_at_target =
                                        arena.baseline_count ==
                                            arena.current_count &&
                                        output_offset ==
                                            baseline_offset;

                                    if (!already_exact_at_target) {
                                        materialize_equal_arena(
                                            arena);

                                        auto* target =
                                            arena.output +
                                            output_offset;

                                        std::memcpy(
                                            target,
                                            arena.baseline.data() +
                                                baseline_offset,
                                            bytes);

                                        baseline_bytes_copied +=
                                            bytes;

                                        if (arena.baseline_count ==
                                            arena.current_count) {

                                            section_identity.observe_overwrite(
                                                arena.kind,
                                                std::span<const std::byte>{
                                                    target,
                                                    bytes},
                                                arena.baseline.subspan(
                                                    output_offset,
                                                    bytes));
                                        }
                                    }
                                }
                            }

                            arena.output_cursor +=
                                records;
                            arena.copy_cursor =
                                target;
                            return true;
                        };

                    const auto unchanged_arena_range =
                        [&](sparse_arena_state& arena,
                            build_cache_range old_range,
                            build_cache_range& current_range)
                            noexcept {

                            if (old_range.begin !=
                                    arena.logical_cursor ||
                                old_range.count >
                                    arena.baseline_count -
                                        arena.logical_cursor ||
                                arena.logical_cursor <
                                    arena.copy_cursor) {
                                return false;
                            }

                            const auto begin =
                                arena.output_cursor +
                                (arena.logical_cursor -
                                    arena.copy_cursor);

                            if (begin >
                                    (std::numeric_limits<
                                        std::uint32_t>::max)()) {
                                return false;
                            }

                            current_range = {
                                static_cast<std::uint32_t>(
                                    begin),
                                old_range.count,
                            };

                            arena.logical_cursor +=
                                old_range.count;
                            return true;
                        };

                    const auto replace_arena_range =
                        [&](sparse_arena_state& arena,
                            build_cache_range old_range,
                            bool old_present,
                            std::span<const std::byte>
                                replacement,
                            std::size_t replacement_count,
                            bool baseline_source,
                            build_cache_range& current_range)
                            noexcept {

                            if (replacement_count != 0 &&
                                replacement_count >
                                    (std::numeric_limits<
                                        std::size_t>::max)() /
                                        arena.record_size) {
                                return false;
                            }

                            const auto replacement_bytes =
                                replacement_count *
                                arena.record_size;

                            if (replacement.size() !=
                                replacement_bytes) {
                                return false;
                            }

                            const auto replaced_baseline_count =
                                baseline_source && old_present
                                ? static_cast<std::size_t>(
                                    old_range.count)
                                : std::size_t{0};

                            if (baseline_source) {
                                if (old_present) {
                                    if (old_range.begin !=
                                            arena.logical_cursor ||
                                        old_range.count >
                                            arena.baseline_count -
                                                arena.logical_cursor) {
                                        return false;
                                    }
                                }
                                else if (old_range.begin != 0 ||
                                         old_range.count != 0) {
                                    return false;
                                }

                                if (!copy_arena_until(
                                        arena,
                                        arena.logical_cursor)) {
                                    return false;
                                }

                                const auto old_count =
                                    old_present
                                    ? static_cast<std::size_t>(
                                        old_range.count)
                                    : std::size_t{0};

                                if (old_count >
                                    arena.baseline_count -
                                        arena.copy_cursor) {
                                    return false;
                                }

                                arena.copy_cursor +=
                                    old_count;
                                arena.logical_cursor +=
                                    old_count;
                            }
                            else {
                                if (arena.logical_cursor !=
                                        arena.baseline_count ||
                                    !copy_arena_until(
                                        arena,
                                        arena.baseline_count)) {
                                    return false;
                                }
                            }

                            if (replacement_count >
                                    (std::numeric_limits<
                                        std::uint32_t>::max)() ||
                                arena.output_cursor >
                                    (std::numeric_limits<
                                        std::uint32_t>::max)() ||
                                replacement_count >
                                    arena.current_count -
                                        arena.output_cursor) {
                                return false;
                            }

                            current_range = {
                                static_cast<std::uint32_t>(
                                    arena.output_cursor),
                                static_cast<std::uint32_t>(
                                    replacement_count),
                            };

                            if (!replacement.empty()) {
                                const auto output_offset =
                                    arena.output_cursor *
                                    arena.record_size;

                                const bool equal_geometry =
                                    arena.baseline_count ==
                                        arena.current_count;

                                const auto baseline_replacement =
                                    equal_geometry &&
                                    output_offset <=
                                        arena.baseline.size() &&
                                    replacement.size() <=
                                        arena.baseline.size() -
                                            output_offset
                                    ? arena.baseline.subspan(
                                        output_offset,
                                        replacement.size())
                                    : std::span<const std::byte>{};

                                const bool replacement_exact =
                                    arena.borrow_candidate &&
                                    replaced_baseline_count ==
                                        replacement_count &&
                                    baseline_replacement.size() ==
                                        replacement.size() &&
                                    std::memcmp(
                                        replacement.data(),
                                        baseline_replacement.data(),
                                        replacement.size()) == 0;

                                if (replacement_exact) {
                                    if (use_sparse_frontend_arenas) {
                                        if (!flush_arena_owned(arena) ||
                                            !arena.sparse_segment.append(
                                                baseline_replacement)) {
                                            return false;
                                        }

                                        arena.borrowed_bytes +=
                                            baseline_replacement.size();
                                        ++arena.borrowed_extents;
                                    }
                                }
                                else {
                                    materialize_equal_arena(
                                        arena);

                                    auto* target =
                                        arena.output +
                                        output_offset;

                                    std::memcpy(
                                        target,
                                        replacement.data(),
                                        replacement.size());

                                    if (use_sparse_frontend_arenas) {
                                        if (arena.owned_run_bytes == 0) {
                                            arena.owned_run_begin =
                                                output_offset;
                                        }
                                        else {
                                            if (arena.owned_run_begin >
                                                    (std::numeric_limits<
                                                        std::size_t>::max)() -
                                                        arena.owned_run_bytes ||
                                                arena.owned_run_begin +
                                                    arena.owned_run_bytes !=
                                                    output_offset) {
                                                return false;
                                            }
                                        }

                                        if (replacement.size() >
                                                (std::numeric_limits<
                                                    std::size_t>::max)() -
                                                    arena.owned_run_bytes) {
                                            return false;
                                        }

                                        arena.owned_run_bytes +=
                                            replacement.size();
                                    }
                                }

                                if (equal_geometry) {
                                    if (!replacement_exact)
                                        section_identity.invalidate(
                                            arena.kind);
                                    else
                                        section_identity.observe_overwrite(
                                            arena.kind,
                                            replacement,
                                            baseline_replacement);
                                }
                            }
                            else if (arena.borrow_candidate &&
                                     replaced_baseline_count != 0) {
                                // Equal total section geometry does not imply
                                // byte identity when one Source removes a
                                // non-empty range and another Source adds it
                                // elsewhere.
                                materialize_equal_arena(arena);
                                section_identity.invalidate(arena.kind);
                            }

                            arena.output_cursor +=
                                replacement_count;
                            return true;
                        };

                    // D4Q2C2A_SOURCE_BYTES_PARTIAL_ZERO_COPY
                    std::size_t baseline_text_logical = 0;
                    std::size_t baseline_text_copy = 0;
                    std::size_t current_text_cursor = 0;

                    project_generation_segment
                        sparse_source_bytes;
                    std::uint64_t
                        sparse_source_bytes_borrowed = 0;
                    std::uint32_t
                        sparse_source_bytes_borrowed_extents = 0;

                    const bool use_sparse_source_bytes =
                        sparse != nullptr;

                    const auto copy_text_until =
                        [&](std::size_t target) noexcept {

                            if (target <
                                    baseline_text_copy ||
                                target >
                                    baseline_source_bytes.size() ||
                                current_text_cursor >
                                    source_bytes_count) {
                                return false;
                            }

                            const auto bytes =
                                target -
                                baseline_text_copy;

                            if (static_cast<std::uint64_t>(
                                    bytes) >
                                source_bytes_count -
                                    current_text_cursor) {
                                return false;
                            }

                            if (bytes != 0) {
                                const auto baseline_run =
                                    baseline_source_bytes.subspan(
                                        baseline_text_copy,
                                        bytes);

                                if (use_sparse_source_bytes) {
                                    if (!sparse_source_bytes.append(
                                            baseline_run)) {
                                        return false;
                                    }

                                    sparse_source_bytes_borrowed +=
                                        bytes;
                                    ++sparse_source_bytes_borrowed_extents;
                                }
                                else {
                                    std::memcpy(
                                        source_bytes +
                                            current_text_cursor,
                                        baseline_run.data(),
                                        bytes);

                                    baseline_bytes_copied +=
                                        bytes;
                                }
                            }

                            current_text_cursor +=
                                bytes;
                            baseline_text_copy =
                                target;
                            return true;
                        };

                    std::size_t current_frontends = 0;

                    for (std::size_t index = 0;
                         index < source_count;
                         ++index) {

                        const source_id source_value{
                            static_cast<std::uint32_t>(
                                index + 1)};

                        const bool baseline_source =
                            index <
                            baseline_source_count;

                        const std::byte*
                            baseline_record =
                                baseline_source
                                ? baseline_directory.data() +
                                    index *
                                        source_directory_record_size
                                : nullptr;

                        std::array<
                            std::byte,
                            source_directory_record_size>
                            current_record_storage;

                        auto* current_record =
                            source_directory +
                            index *
                                source_directory_record_size;

                        if (use_sparse_source_directory) {
                            current_record =
                                current_record_storage.data();

                            if (baseline_record != nullptr) {
                                std::memcpy(
                                    current_record,
                                    baseline_record,
                                    source_directory_record_size);
                            }
                            else {
                                std::memset(
                                    current_record,
                                    0,
                                    source_directory_record_size);
                            }
                        }

                        write_u32(
                            current_record,
                            source_value.value());

                        std::uint32_t old_flags = 0;
                        std::uint64_t old_text_offset = 0;
                        std::uint32_t old_text_length = 0;
                        build_cache_range old_ranges[4]{};

                        if (baseline_source) {
                            if (read_u32(
                                    baseline_record) !=
                                    source_value.value()) {
                                return false;
                            }

                            old_flags =
                                read_u32(
                                    baseline_record + 4);

                            if ((old_flags &
                                    ~source_known_flags) != 0 ||
                                read_u32(
                                    baseline_record + 20) != 0) {
                                return false;
                            }

                            old_text_offset =
                                read_u64(
                                    baseline_record + 8);
                            old_text_length =
                                read_u32(
                                    baseline_record + 16);

                            old_ranges[0] = {
                                read_u32(
                                    baseline_record + 24),
                                read_u32(
                                    baseline_record + 28),
                            };
                            old_ranges[1] = {
                                read_u32(
                                    baseline_record + 32),
                                read_u32(
                                    baseline_record + 36),
                            };
                            old_ranges[2] = {
                                read_u32(
                                    baseline_record + 40),
                                read_u32(
                                    baseline_record + 44),
                            };
                            old_ranges[3] = {
                                read_u32(
                                    baseline_record + 48),
                                read_u32(
                                    baseline_record + 52),
                            };
                        }

                        auto flags = old_flags;

                        const bool old_snapshot =
                            (old_flags &
                             source_flag_snapshot) != 0;

                        if (baseline_source &&
                            old_snapshot) {

                            if (old_text_offset !=
                                    baseline_text_logical ||
                                old_text_length >
                                    baseline_source_bytes.size() -
                                        baseline_text_logical) {
                                return false;
                            }
                        }

                        const auto& sparse =
                            sparse_index[
                                source_value.value()];

                        if (sparse.text_candidate) {
                            if (baseline_source) {
                                if (!copy_text_until(
                                        baseline_text_logical)) {
                                    return false;
                                }

                                if (old_snapshot) {
                                    if (old_text_length >
                                        baseline_source_bytes.size() -
                                            baseline_text_copy) {
                                        return false;
                                    }

                                    baseline_text_copy +=
                                        old_text_length;
                                    baseline_text_logical +=
                                        old_text_length;
                                }
                            }
                            else {
                                if (baseline_text_logical !=
                                        baseline_source_bytes.size() ||
                                    !copy_text_until(
                                        baseline_source_bytes.size())) {
                                    return false;
                                }
                            }

                            const auto snapshot =
                                project.sources().current(
                                    source_value);

                            flags &=
                                ~source_flag_snapshot;

                            if (snapshot) {
                                const auto text =
                                    snapshot.text();

                                if (text.size() >
                                        (std::numeric_limits<
                                            std::uint32_t>::max)() ||
                                    current_text_cursor >
                                        source_bytes_count ||
                                    static_cast<std::uint64_t>(
                                        text.size()) >
                                        source_bytes_count -
                                            current_text_cursor) {
                                    return false;
                                }

                                write_u64(
                                    current_record + 8,
                                    current_text_cursor);
                                write_u32(
                                    current_record + 16,
                                    static_cast<std::uint32_t>(
                                        text.size()));

                                if (!text.empty()) {
                                    auto* current_text =
                                        source_bytes +
                                        current_text_cursor;

                                    std::memcpy(
                                        current_text,
                                        text.data(),
                                        text.size());

                                    if (use_sparse_source_bytes &&
                                        !sparse_source_bytes.append(
                                            std::span<const std::byte>{
                                                current_text,
                                                text.size()})) {
                                        return false;
                                    }
                                }

                                current_text_cursor +=
                                    text.size();
                                flags |=
                                    source_flag_snapshot;
                            }
                            else {
                                write_u64(
                                    current_record + 8,
                                    0);
                                write_u32(
                                    current_record + 16,
                                    0);
                            }
                        }
                        else if (baseline_source) {
                            if (old_snapshot) {
                                if (baseline_text_logical <
                                        baseline_text_copy) {
                                    return false;
                                }

                                const auto adjusted =
                                    current_text_cursor +
                                    (baseline_text_logical -
                                        baseline_text_copy);

                                write_u64(
                                    current_record + 8,
                                    adjusted);

                                baseline_text_logical +=
                                    old_text_length;
                            }
                        }
                        else {
                            return false;
                        }

                        const bool old_frontend =
                            (old_flags &
                             source_flag_frontend) != 0;

                        if (old_frontend &&
                            !old_snapshot) {
                            return false;
                        }

                        const auto overlay_slot =
                            sparse.frontend_overlay;

                        if (overlay_slot != 0) {
                            const auto overlay_index =
                                static_cast<std::size_t>(
                                    overlay_slot - 1);

                            if (overlay_index >=
                                overlay_views.size()) {
                                return false;
                            }

                            const auto& view =
                                overlay_views[
                                    overlay_index];

                            flags &=
                                ~source_flag_frontend;

                            for (std::size_t range = 0;
                                 range < 4;
                                 ++range) {
                                write_cache_range(
                                    current_record +
                                        24 +
                                        range * 8,
                                    {});
                            }

                            if (view.record.present) {
                                if ((flags &
                                     source_flag_snapshot) == 0 ||
                                    view.storage !=
                                        source_frontend_persistence_storage::
                                            native_interface) {
                                    return false;
                                }

                                const auto data =
                                    view.data;

                                if (data.local_types.size() !=
                                        view.record.local_types ||
                                    data.type_slots.size() !=
                                        view.record.type_slots ||
                                    data.object_slots.size() !=
                                        view.record.object_slots ||
                                    data.member_slots.size() !=
                                        view.record.member_slots) {
                                    return false;
                                }

                                build_cache_range
                                    current_ranges[4]{};

                                if (!replace_arena_range(
                                        arenas[0],
                                        old_ranges[0],
                                        old_frontend,
                                        std::as_bytes(
                                            data.local_types),
                                        data.local_types.size(),
                                        baseline_source,
                                        current_ranges[0]) ||
                                    !replace_arena_range(
                                        arenas[1],
                                        old_ranges[1],
                                        old_frontend,
                                        std::as_bytes(
                                            data.type_slots),
                                        data.type_slots.size(),
                                        baseline_source,
                                        current_ranges[1]) ||
                                    !replace_arena_range(
                                        arenas[2],
                                        old_ranges[2],
                                        old_frontend,
                                        std::as_bytes(
                                            data.object_slots),
                                        data.object_slots.size(),
                                        baseline_source,
                                        current_ranges[2]) ||
                                    !replace_arena_range(
                                        arenas[3],
                                        old_ranges[3],
                                        old_frontend,
                                        std::as_bytes(
                                            data.member_slots),
                                        data.member_slots.size(),
                                        baseline_source,
                                        current_ranges[3])) {
                                    return false;
                                }

                                for (std::size_t range = 0;
                                     range < 4;
                                     ++range) {
                                    write_cache_range(
                                        current_record +
                                            24 +
                                            range * 8,
                                        current_ranges[
                                            range]);
                                }

                                flags |=
                                    source_flag_frontend;
                                ++current_frontends;
                            }
                            else {
                                for (std::size_t range = 0;
                                     range < 4;
                                     ++range) {

                                    build_cache_range
                                        ignored{};

                                    if (!replace_arena_range(
                                            arenas[range],
                                            old_ranges[range],
                                            old_frontend,
                                            {},
                                            0,
                                            baseline_source,
                                            ignored)) {
                                        return false;
                                    }
                                }
                            }
                        }
                        else if (baseline_source) {
                            if (old_frontend) {
                                build_cache_range
                                    current_ranges[4]{};

                                for (std::size_t range = 0;
                                     range < 4;
                                     ++range) {

                                    if (!unchanged_arena_range(
                                            arenas[range],
                                            old_ranges[range],
                                            current_ranges[
                                                range])) {
                                        return false;
                                    }

                                    write_cache_range(
                                        current_record +
                                            24 +
                                            range * 8,
                                        current_ranges[
                                            range]);
                                }

                                ++current_frontends;
                            }
                        }
                        else if ((flags &
                                  source_flag_snapshot) != 0) {
                            return false;
                        }

                        write_u32(
                            current_record + 4,
                            flags);

                        if (use_sparse_source_directory) {
                            const bool exact_record =
                                baseline_record != nullptr &&
                                std::memcmp(
                                    current_record,
                                    baseline_record,
                                    source_directory_record_size) == 0;

                            bool borrow_record =
                                exact_record &&
                                !directory_force_owned_tail;

                            // Preserve already borrowed runs, but do not let
                            // alternating exact/adjusted records exhaust the
                            // fixed Generation carrier. Near the limit the
                            // remaining directory becomes one owned tail.
                            if (borrow_record &&
                                directory_owned_count != 0 &&
                                sparse_source_directory.extent_count() >=
                                    project_generation_segment_max_extents -
                                        6) {

                                directory_force_owned_tail = true;
                                borrow_record = false;
                            }

                            if (borrow_record) {
                                if (!flush_directory_owned())
                                    return false;

                                if (directory_borrow_count == 0)
                                    directory_borrow_begin = index;

                                ++directory_borrow_count;
                            }
                            else {
                                if (!flush_directory_borrow())
                                    return false;

                                auto* owned_record =
                                    source_directory +
                                    index *
                                        source_directory_record_size;

                                std::memcpy(
                                    owned_record,
                                    current_record,
                                    source_directory_record_size);

                                // D4Q2C2B_DIRECTORY_BASELINE_ACCOUNTING
                                // Count baseline payload materialized into
                                // current Generation storage. The temporary
                                // stack comparison is not counted twice.
                                if (baseline_record != nullptr) {
                                    baseline_bytes_copied +=
                                        source_directory_record_size;
                                }

                                if (directory_owned_count == 0)
                                    directory_owned_begin = index;

                                ++directory_owned_count;
                            }
                        }
                    }

                    if (use_sparse_source_directory) {
                        if (!flush_directory_borrow() ||
                            !flush_directory_owned()) {
                            return false;
                        }

                        const auto current_directory_bytes =
                            source_count *
                            source_directory_record_size;

                        if (sparse_source_directory.size() !=
                                current_directory_bytes) {
                            return false;
                        }

                        const auto sparse_index =
                            section_index(
                                build_cache_image_section::
                                    source_directory);

                        sparse->sections[sparse_index] =
                            sparse_source_directory;
                    }

                    if (baseline_text_logical !=
                            baseline_source_bytes.size() ||
                        !copy_text_until(
                            baseline_source_bytes.size()) ||
                        current_text_cursor !=
                            source_bytes_count) {
                        return false;
                    }

                    if (use_sparse_source_bytes) {
                        if (sparse_source_bytes.size() !=
                                source_bytes_count) {
                            return false;
                        }

                        const auto sparse_index =
                            section_index(
                                build_cache_image_section::
                                    source_bytes);

                        sparse->sections[sparse_index] =
                            sparse_source_bytes;
                    }

                    std::uint64_t
                        sparse_frontend_borrowed_bytes = 0;
                    std::uint64_t
                        sparse_frontend_owned_bytes = 0;
                    std::uint32_t
                        sparse_frontend_borrowed_extents = 0;
                    std::uint32_t
                        sparse_frontend_owned_extents = 0;

                    for (auto& arena : arenas) {
                        if (arena.logical_cursor !=
                                arena.baseline_count ||
                            !copy_arena_until(
                                arena,
                                arena.baseline_count) ||
                            !flush_arena_owned(arena) ||
                            arena.output_cursor !=
                                arena.current_count) {
                            return false;
                        }

                        if (use_sparse_frontend_arenas &&
                            !arena.borrow_candidate) {

                            if (arena.current_count >
                                (std::numeric_limits<
                                    std::size_t>::max)() /
                                    arena.record_size) {
                                return false;
                            }

                            const auto expected_bytes =
                                arena.current_count *
                                arena.record_size;

                            if (arena.sparse_segment.size() !=
                                    expected_bytes) {
                                return false;
                            }

                            if (expected_bytes != 0) {
                                const auto sparse_index =
                                    section_index(arena.kind);

                                if (sparse_index >=
                                        sparse->sections.size()) {
                                    return false;
                                }

                                sparse->sections[sparse_index] =
                                    arena.sparse_segment;
                            }

                            sparse_frontend_borrowed_bytes +=
                                arena.borrowed_bytes;
                            sparse_frontend_owned_bytes +=
                                arena.owned_bytes;
                            sparse_frontend_borrowed_extents +=
                                arena.borrowed_extents;
                            sparse_frontend_owned_extents +=
                                arena.owned_extents;
                        }
                    }

                    if (sparse != nullptr) {
                        std::size_t sparse_extent_count = 0;

                        for (const auto& section :
                             sparse->sections) {

                            if (section.extent_count() >
                                    (std::numeric_limits<
                                        std::size_t>::max)() -
                                        sparse_extent_count) {
                                return false;
                            }

                            sparse_extent_count +=
                                section.extent_count();
                        }

                        // Reserve one extent for the prefix and one for every
                        // non-sparse logical section. This is conservative but
                        // guarantees the final flattened Build Cache carrier
                        // cannot exceed its fixed extent capacity.
                        constexpr auto carrier_reserve =
                            build_cache_image_directory_count + 1;

                        static_assert(
                            project_generation_segment_max_extents >
                            carrier_reserve);

                        if (sparse_extent_count >
                            project_generation_segment_max_extents -
                                carrier_reserve) {
                            return false;
                        }
                    }

                    if (current_frontends !=
                        frontend_count) {
                        return false;
                    }

                    std::uint64_t baseline_bytes_borrowed = 0;
                    std::uint32_t baseline_sections_borrowed = 0;

                    if (borrowed != nullptr) {
                        for (auto& arena : arenas) {
                            if (!arena.borrow_candidate)
                                continue;

                            const auto borrow_index =
                                section_index(arena.kind);

                            if (borrow_index >=
                                borrowed->sections.size()) {
                                return false;
                            }

                            borrowed->sections[borrow_index] =
                                arena.baseline;
                            baseline_bytes_borrowed +=
                                arena.baseline.size();
                            ++baseline_sections_borrowed;
                        }
                    }

                    text_cursor =
                        source_bytes_count;
                    local_type_cursor =
                        static_cast<std::uint32_t>(
                            local_type_count);
                    type_slot_cursor =
                        static_cast<std::uint32_t>(
                            type_slot_count);
                    object_slot_cursor =
                        static_cast<std::uint32_t>(
                            object_slot_count);
                    member_slot_cursor =
                        static_cast<std::uint32_t>(
                            member_slot_count);
                    observed_frontends =
                        current_frontends;

                    if (telemetry != nullptr) {
                        telemetry->
                            mapped_baseline_bulk_bytes +=
                                baseline_bytes_copied +
                                baseline_bytes_borrowed +
                                sparse_source_bytes_borrowed +
                                sparse_source_directory_borrowed +
                                sparse_frontend_borrowed_bytes;
                        telemetry->
                            mapped_baseline_borrowed_bytes +=
                                baseline_bytes_borrowed;
                        telemetry->
                            mapped_baseline_sparse_borrowed_bytes +=
                                sparse_source_bytes_borrowed +
                                sparse_source_directory_borrowed +
                                sparse_frontend_borrowed_bytes;
                        telemetry->
                            mapped_baseline_sparse_directory_borrowed_bytes +=
                                sparse_source_directory_borrowed;
                        telemetry->
                            mapped_baseline_sparse_frontend_borrowed_bytes +=
                                sparse_frontend_borrowed_bytes;
                        telemetry->
                            mapped_baseline_sparse_frontend_owned_bytes +=
                                sparse_frontend_owned_bytes;
                        telemetry->
                            mapped_baseline_borrowed_sections +=
                                baseline_sections_borrowed;
                        if (use_sparse_source_bytes) {
                            telemetry->
                                mapped_baseline_sparse_borrowed_extents +=
                                    sparse_source_bytes_borrowed_extents;
                        }
                        if (use_sparse_source_directory) {
                            telemetry->
                                mapped_baseline_sparse_borrowed_extents +=
                                    sparse_source_directory_borrowed_extents;
                            telemetry->
                                mapped_baseline_sparse_directory_borrowed_extents +=
                                    sparse_source_directory_borrowed_extents;
                        }
                        if (use_sparse_frontend_arenas) {
                            telemetry->
                                mapped_baseline_sparse_borrowed_extents +=
                                    sparse_frontend_borrowed_extents;
                            telemetry->
                                mapped_baseline_sparse_frontend_borrowed_extents +=
                                    sparse_frontend_borrowed_extents;
                            telemetry->
                                mapped_baseline_sparse_frontend_owned_extents +=
                                    sparse_frontend_owned_extents;
                        }
                        telemetry->
                            mapped_baseline_bulk_sections +=
                                6;
                    }

                    return true;
                }
                catch (const std::bad_alloc&) {
                    return false;
                }
                catch (const std::length_error&) {
                    return false;
                }
            };

        if (try_sparse_source_frontend()) {
            native_source_encoded = true;
        }
        else {
            // Fast path is optional. Partial writes and partial-section
            // carriers must never leak into the canonical fallback encoder.
            if (sparse != nullptr)
                *sparse = {};

            clear_source_frontend_sections();
        }
    }

    if (!native_source_encoded &&
        direct_native_source_directory) {

        text_cursor = source_bytes_count;
        local_type_cursor =
            static_cast<std::uint32_t>(
                local_type_count);
        type_slot_cursor =
            static_cast<std::uint32_t>(
                type_slot_count);
        object_slot_cursor =
            static_cast<std::uint32_t>(
                object_slot_count);
        member_slot_cursor =
            static_cast<std::uint32_t>(
                member_slot_count);
        observed_frontends = frontend_count;
        native_source_encoded = true;
    }

    const auto hardware_workers =
        (std::max)(
            std::size_t{1},
            static_cast<std::size_t>(
                std::thread::hardware_concurrency()));

    const auto desired_native_workers =
        source_count == 0
        ? std::size_t{1}
        : (std::min)(
            max_native_source_workers,
            std::size_t{1} +
                (source_count - 1) /
                    minimum_sources_per_worker);

    const auto native_worker_count =
        (std::min)(
            hardware_workers,
            desired_native_workers);

    const bool use_parallel_native_sources =
        !native_source_encoded &&
        use_native_block_storage &&
        native_source_proof_available &&
        native_worker_count > 1;

    if (use_parallel_native_sources) {
        std::array<std::size_t, max_native_source_workers>
            chunk_begin{};
        std::array<std::size_t, max_native_source_workers>
            chunk_end{};
        std::array<std::uint64_t, max_native_source_workers>
            chunk_text_begin{};
        std::array<std::uint64_t, max_native_source_workers>
            chunk_text_end{};

        const auto base_chunk =
            source_count / native_worker_count;
        const auto extra_sources =
            source_count % native_worker_count;

        std::size_t source_cursor = 0;
        for (std::size_t worker = 0;
             worker < native_worker_count;
             ++worker) {

            const auto chunk_size =
                base_chunk +
                (worker < extra_sources ? 1u : 0u);

            chunk_begin[worker] = source_cursor;
            source_cursor += chunk_size;
            chunk_end[worker] = source_cursor;
        }

        if (source_cursor != source_count)
            return {status_code::initialization_failed};

        // One cheap metadata pass proves the same global contiguity invariants
        // as the old sequential encoder and computes one text prefix per chunk.
        std::uint64_t native_text_cursor = 0;
        std::uint32_t native_local_type_cursor = 0;
        std::uint32_t native_type_slot_cursor = 0;
        std::uint32_t native_object_slot_cursor = 0;
        std::uint32_t native_member_slot_cursor = 0;
        std::size_t native_frontend_count = 0;

        for (std::size_t worker = 0;
             worker < native_worker_count;
             ++worker) {

            chunk_text_begin[worker] =
                native_text_cursor;

            for (std::size_t index = chunk_begin[worker];
                 index < chunk_end[worker];
                 ++index) {

                const auto& physical =
                    native_sources.physical[index];

                if (physical.reserved != 0 ||
                    (physical.flags &
                        ~source_generation_physical_present) != 0) {
                    return {status_code::initialization_failed};
                }

                if (physical.present()) {
                    if (physical.size >
                        (std::numeric_limits<std::uint32_t>::max)()) {
                        return {status_code::initialization_failed};
                    }

                    if (!add_u64(
                            native_text_cursor,
                            physical.size,
                            native_text_cursor) ||
                        native_text_cursor >
                            source_bytes_count) {
                        return {status_code::initialization_failed};
                    }
                }

                source_frontend_block_layout record;
                bool record_present = false;
                const auto block_result =
                    read_native_block_layout(
                        source_id{
                            static_cast<std::uint32_t>(
                                index + 1)},
                        record,
                        record_present);

                if (!block_result.ok())
                    return block_result;

                if (!record_present)
                    continue;

                ++native_frontend_count;

                if (record.local_types.begin !=
                        native_local_type_cursor ||
                    record.local_types.count >
                        local_type_count -
                            static_cast<std::size_t>(
                                native_local_type_cursor) ||
                    record.type_slots.begin !=
                        native_type_slot_cursor ||
                    record.type_slots.count >
                        type_slot_count -
                            static_cast<std::size_t>(
                                native_type_slot_cursor) ||
                    record.object_slots.begin !=
                        native_object_slot_cursor ||
                    record.object_slots.count >
                        object_slot_count -
                            static_cast<std::size_t>(
                                native_object_slot_cursor) ||
                    record.member_slots.begin !=
                        native_member_slot_cursor ||
                    record.member_slots.count >
                        member_slot_count -
                            static_cast<std::size_t>(
                                native_member_slot_cursor)) {
                    return {status_code::initialization_failed};
                }

                native_local_type_cursor += record.local_types.count;
                native_type_slot_cursor += record.type_slots.count;
                native_object_slot_cursor += record.object_slots.count;
                native_member_slot_cursor += record.member_slots.count;
            }

            chunk_text_end[worker] =
                native_text_cursor;
        }

        if (native_text_cursor != source_bytes_count ||
            static_cast<std::size_t>(
                native_local_type_cursor) != local_type_count ||
            static_cast<std::size_t>(
                native_type_slot_cursor) != type_slot_count ||
            static_cast<std::size_t>(
                native_object_slot_cursor) != object_slot_count ||
            static_cast<std::size_t>(
                native_member_slot_cursor) != member_slot_count ||
            native_frontend_count != frontend_count) {
            return {status_code::initialization_failed};
        }

        const auto encode_native_chunk =
            [&](std::size_t worker) noexcept -> status {

                auto chunk_text_cursor =
                    chunk_text_begin[worker];
                const auto chunk_text_limit =
                    chunk_text_end[worker];

                for (std::size_t index = chunk_begin[worker];
                     index < chunk_end[worker];
                     ++index) {

                    const source_id source_value{
                        static_cast<std::uint32_t>(
                            index + 1)};

                    auto* directory_record =
                        source_directory +
                        index *
                            source_directory_record_size;

                    write_u32(
                        directory_record,
                        source_value.value());

                    std::uint32_t flags = 0;

                    const auto& physical =
                        native_sources.physical[index];

                    const auto snapshot =
                        project.sources().current(
                            source_value);

                    if (physical.present()) {
                        if (!snapshot)
                            return {
                                status_code::
                                    initialization_failed};

                        const auto text =
                            snapshot.text();

                        // GEN-02C27: encoder-proven native Source content.
                        // The immutable snapshot owns the exact digest produced
                        // during acquisition. Prove it still matches the dense
                        // Generation physical record before copying bytes.
                        if (snapshot.hash() != physical.hash ||
                            text.size() != physical.size ||
                            text.size() >
                                (std::numeric_limits<
                                    std::uint32_t>::max)() ||
                            chunk_text_cursor >
                                chunk_text_limit ||
                            static_cast<std::uint64_t>(
                                text.size()) >
                                chunk_text_limit -
                                    chunk_text_cursor) {
                            return {
                                status_code::
                                    initialization_failed};
                        }

                        flags |= source_flag_snapshot;

                        write_u64(
                            directory_record + 8,
                            chunk_text_cursor);
                        write_u32(
                            directory_record + 16,
                            static_cast<std::uint32_t>(
                                text.size()));

                        if (!text.empty() &&
                            !direct_native_source_bytes) {
                            std::memcpy(
                                source_bytes +
                                    static_cast<std::size_t>(
                                        chunk_text_cursor),
                                text.data(),
                                text.size());
                        }

                        chunk_text_cursor +=
                            text.size();
                    }
                    else if (snapshot) {
                        return {
                            status_code::
                                initialization_failed};
                    }

                    source_frontend_block_layout record;
                    bool record_present = false;
                    const auto block_result =
                        read_native_block_layout(
                            source_value,
                            record,
                            record_present);

                    if (!block_result.ok())
                        return block_result;

                    if (record_present) {
                        if ((flags &
                                source_flag_snapshot) == 0) {
                            return {
                                status_code::
                                    initialization_failed};
                        }

                        flags |= source_flag_frontend;

                        write_u32(directory_record + 24, record.local_types.begin);
                        write_u32(directory_record + 28, record.local_types.count);
                        write_u32(directory_record + 32, record.type_slots.begin);
                        write_u32(directory_record + 36, record.type_slots.count);
                        write_u32(directory_record + 40, record.object_slots.begin);
                        write_u32(directory_record + 44, record.object_slots.count);
                        write_u32(directory_record + 48, record.member_slots.begin);
                        write_u32(directory_record + 52, record.member_slots.count);
                    }

                    write_u32(
                        directory_record + 4,
                        flags);
                }

                return chunk_text_cursor ==
                        chunk_text_limit
                    ? status{}
                    : status{
                        status_code::
                            initialization_failed};
            };

        std::array<status, max_native_source_workers>
            native_results{};
        std::array<std::jthread,
            max_native_source_workers - 1>
            native_workers{};

        std::size_t launched_workers = 0;
        bool thread_launch_failed = false;

        try {
            for (std::size_t worker = 1;
                 worker < native_worker_count;
                 ++worker) {

                native_workers[launched_workers] =
                    std::jthread(
                        [&, worker]() noexcept {
                            native_results[worker] =
                                encode_native_chunk(
                                    worker);
                        });

                ++launched_workers;
            }

            native_results[0] =
                encode_native_chunk(0);
        }
        catch (const std::bad_alloc&) {
            thread_launch_failed = true;
        }
        catch (const std::system_error&) {
            thread_launch_failed = true;
        }

        for (std::size_t worker = 0;
             worker < launched_workers;
             ++worker) {

            if (native_workers[worker].joinable())
                native_workers[worker].join();
        }

        if (thread_launch_failed) {
            // A partially launched batch may already have written complete
            // chunks. Re-encode every chunk sequentially; writes are
            // deterministic and target disjoint canonical ranges.
            for (std::size_t worker = 0;
                 worker < native_worker_count;
                 ++worker) {

                native_results[worker] =
                    encode_native_chunk(worker);

                if (!native_results[worker].ok())
                    return native_results[worker];
            }
        }
        else {
            for (std::size_t worker = 0;
                 worker < native_worker_count;
                 ++worker) {

                if (!native_results[worker].ok())
                    return native_results[worker];
            }
        }

        text_cursor = source_bytes_count;
        local_type_cursor =
            static_cast<std::uint32_t>(
                local_type_count);
        type_slot_cursor =
            static_cast<std::uint32_t>(
                type_slot_count);
        object_slot_cursor =
            static_cast<std::uint32_t>(
                object_slot_count);
        member_slot_cursor =
            static_cast<std::uint32_t>(
                member_slot_count);
        observed_frontends =
            native_frontend_count;

        native_source_encoded = true;
    }

    if (!native_source_encoded) {
        for (std::size_t index = 0;
             index < source_count;
             ++index) {
        const source_id source_value{
            static_cast<std::uint32_t>(index + 1)};
        auto* directory_record =
            source_directory + index * source_directory_record_size;

        write_u32(directory_record, source_value.value());

        std::uint32_t flags = 0;
        const auto snapshot = project.sources().current(source_value);

        if (native_source_proof_available) {
            const auto& physical =
                native_sources.physical[index];

            if (physical.present() !=
                static_cast<bool>(snapshot)) {
                return {status_code::initialization_failed};
            }

            if (snapshot &&
                (snapshot.hash() != physical.hash ||
                 snapshot.text().size() != physical.size)) {
                return {status_code::initialization_failed};
            }
        }

        if (snapshot) {
            flags |= source_flag_snapshot;
            const auto text = snapshot.text();

            if (text_cursor > source_bytes_count ||
                static_cast<std::uint64_t>(text.size()) >
                    source_bytes_count - text_cursor ||
                text.size() >
                    (std::numeric_limits<std::uint32_t>::max)()) {
                return {status_code::initialization_failed};
            }

            write_u64(directory_record + 8, text_cursor);
            write_u32(
                directory_record + 16,
                static_cast<std::uint32_t>(text.size()));

            if (!text.empty() &&
                !direct_native_source_bytes) {
                std::memcpy(
                    source_bytes + static_cast<std::size_t>(text_cursor),
                    text.data(),
                    text.size());
            }

            text_cursor += text.size();
        }

        source_frontend_persistence_record interface_record;
        source_frontend_persistence_storage interface_storage =
            source_frontend_persistence_storage::none;
        source_interface_data_view interface_data{};
        source_frontend_block_layout native_record{};
        bool native_record_present = false;
        bool interface_preencoded = false;

        status result;
        if (use_native_block_storage) {
            result =
                read_native_block_layout(
                    source_value,
                    native_record,
                    native_record_present);

            if (!result.ok())
                return result;

            if (native_record_present) {
                interface_record.present = true;
                interface_record.local_types =
                    native_record.local_types.count;
                interface_record.type_slots =
                    native_record.type_slots.count;
                interface_record.object_slots =
                    native_record.object_slots.count;
                interface_record.member_slots =
                    native_record.member_slots.count;
                interface_preencoded = true;
            }
        }
        else if (use_native_frontend) {
            if (const auto* interface_value =
                    native_frontend[index];
                interface_value != nullptr) {

                interface_data = interface_value->persistence_data_view();
                interface_storage =
                    source_frontend_persistence_storage::
                        native_interface;
                interface_record.present = true;
                interface_record.local_types =
                    interface_data.local_types.size();
                interface_record.type_slots =
                    interface_data.type_slots.size();
                interface_record.object_slots =
                    interface_data.object_slots.size();
                interface_record.member_slots =
                    interface_data.member_slots.size();
            }
        }
        else {
            source_frontend_persistence_view interface_view;
            result = frontend.persistence_view(
                source_value,
                interface_view);
            if (!result.ok())
                return result;

            interface_record = interface_view.record;
            interface_storage = interface_view.storage;
            interface_data = interface_view.data;
        }

        if (interface_record.present) {
            if ((flags & source_flag_snapshot) == 0)
                return {status_code::initialization_failed};

            flags |= source_flag_frontend;
            ++observed_frontends;

            if (static_cast<std::size_t>(local_type_cursor) >
                    local_type_count ||
                interface_record.local_types >
                    local_type_count -
                        static_cast<std::size_t>(local_type_cursor) ||
                static_cast<std::size_t>(type_slot_cursor) >
                    type_slot_count ||
                interface_record.type_slots >
                    type_slot_count -
                        static_cast<std::size_t>(type_slot_cursor) ||
                static_cast<std::size_t>(object_slot_cursor) >
                    object_slot_count ||
                interface_record.object_slots >
                    object_slot_count -
                        static_cast<std::size_t>(object_slot_cursor) ||
                static_cast<std::size_t>(member_slot_cursor) >
                    member_slot_count ||
                interface_record.member_slots >
                    member_slot_count -
                        static_cast<std::size_t>(member_slot_cursor)) {
                return {status_code::initialization_failed};
            }

            const build_cache_range local_type_range{
                local_type_cursor,
                static_cast<std::uint32_t>(interface_record.local_types)};
            const build_cache_range type_slot_range{
                type_slot_cursor,
                static_cast<std::uint32_t>(interface_record.type_slots)};
            const build_cache_range object_slot_range{
                object_slot_cursor,
                static_cast<std::uint32_t>(interface_record.object_slots)};
            const build_cache_range member_slot_range{
                member_slot_cursor,
                static_cast<std::uint32_t>(interface_record.member_slots)};

            write_cache_range(directory_record + 24, local_type_range);
            write_cache_range(directory_record + 32, type_slot_range);
            write_cache_range(directory_record + 40, object_slot_range);
            write_cache_range(directory_record + 48, member_slot_range);

            if (interface_preencoded) {
                if (!native_record_present ||
                    native_record.local_types.begin !=
                        local_type_range.begin ||
                    native_record.local_types.count !=
                        local_type_range.count ||
                    native_record.type_slots.begin !=
                        type_slot_range.begin ||
                    native_record.type_slots.count !=
                        type_slot_range.count ||
                    native_record.object_slots.begin !=
                        object_slot_range.begin ||
                    native_record.object_slots.count !=
                        object_slot_range.count ||
                    native_record.member_slots.begin !=
                        member_slot_range.begin ||
                    native_record.member_slots.count !=
                        member_slot_range.count) {
                    return {status_code::initialization_failed};
                }

                local_type_cursor +=
                    local_type_range.count;
                type_slot_cursor +=
                    type_slot_range.count;
                object_slot_cursor +=
                    object_slot_range.count;
                member_slot_cursor +=
                    member_slot_range.count;
            }
            else if (interface_storage ==
                source_frontend_persistence_storage::native_interface) {

                const auto data = interface_data;

                if (data.local_types.size() != interface_record.local_types ||
                    data.type_slots.size() != interface_record.type_slots ||
                    data.object_slots.size() != interface_record.object_slots ||
                    data.member_slots.size() != interface_record.member_slots) {
                    return {status_code::initialization_failed};
                }

                for (const auto identity : data.local_types) {
                    if (!identity)
                        return {status_code::initialization_failed};

                    write_u32(
                        local_types +
                            static_cast<std::size_t>(
                                local_type_cursor++) * 4,
                        identity.value());
                }

                for (const auto& slot : data.type_slots) {
                    auto* target =
                        type_slots +
                        static_cast<std::size_t>(
                            type_slot_cursor++) *
                            frontend_type_slot_record_size;
                    write_u32(target, slot.parent.value());
                    write_u32(target + 4, slot.name.value());
                    write_u32(target + 8, slot.identity.value());
                }

                for (const auto& slot : data.object_slots) {
                    auto* target =
                        object_slots +
                        static_cast<std::size_t>(
                            object_slot_cursor++) *
                            frontend_object_slot_record_size;
                    write_u32(target, slot.parent.value());
                    write_u32(target + 4, slot.name.value());
                    write_u32(target + 8, slot.identity.value());
                    write_u32(target + 12, slot.named_type.value());
                }

                for (const auto& slot : data.member_slots) {
                    auto* target =
                        member_slots +
                        static_cast<std::size_t>(
                            member_slot_cursor++) *
                            frontend_member_slot_record_size;
                    write_u32(target, slot.type.value());
                    write_u32(target + 4, slot.name.value());
                    write_u32(target + 8, slot.index.value());
                }
            }
            else if (interface_storage ==
                     source_frontend_persistence_storage::persisted_baseline) {

                if (telemetry != nullptr) {
                    const auto elements =
                        static_cast<std::uint64_t>(
                            interface_record.local_types) +
                        static_cast<std::uint64_t>(
                            interface_record.type_slots) +
                        static_cast<std::uint64_t>(
                            interface_record.object_slots) +
                        static_cast<std::uint64_t>(
                            interface_record.member_slots);

                    telemetry->
                        mapped_baseline_frontend_element_reads +=
                            elements;
                    telemetry->
                        mapped_baseline_frontend_elements_encoded +=
                            elements;
                }

                for (std::size_t item = 0;
                     item < interface_record.local_types;
                     ++item) {
                    identity_ref identity;
                    result = frontend.persistence_local_type(
                        source_value, item, identity);
                    if (!result.ok() || !identity)
                        return {status_code::initialization_failed};

                    write_u32(
                        local_types +
                            static_cast<std::size_t>(
                                local_type_cursor++) * 4,
                        identity.value());
                }

                for (std::size_t item = 0;
                     item < interface_record.type_slots;
                     ++item) {
                    source_interface_type_slot slot;
                    result = frontend.persistence_type_slot(
                        source_value, item, slot);
                    if (!result.ok())
                        return result;

                    auto* target =
                        type_slots +
                        static_cast<std::size_t>(
                            type_slot_cursor++) *
                            frontend_type_slot_record_size;
                    write_u32(target, slot.parent.value());
                    write_u32(target + 4, slot.name.value());
                    write_u32(target + 8, slot.identity.value());
                }

                for (std::size_t item = 0;
                     item < interface_record.object_slots;
                     ++item) {
                    source_interface_object_slot slot;
                    result = frontend.persistence_object_slot(
                        source_value, item, slot);
                    if (!result.ok())
                        return result;

                    auto* target =
                        object_slots +
                        static_cast<std::size_t>(
                            object_slot_cursor++) *
                            frontend_object_slot_record_size;
                    write_u32(target, slot.parent.value());
                    write_u32(target + 4, slot.name.value());
                    write_u32(target + 8, slot.identity.value());
                    write_u32(target + 12, slot.named_type.value());
                }

                for (std::size_t item = 0;
                     item < interface_record.member_slots;
                     ++item) {
                    source_interface_member_slot slot;
                    result = frontend.persistence_member_slot(
                        source_value, item, slot);
                    if (!result.ok())
                        return result;

                    auto* target =
                        member_slots +
                        static_cast<std::size_t>(
                            member_slot_cursor++) *
                            frontend_member_slot_record_size;
                    write_u32(target, slot.type.value());
                    write_u32(target + 4, slot.name.value());
                    write_u32(target + 8, slot.index.value());
                }
            }
            else {
                return {status_code::initialization_failed};
            }

        }
        else if (interface_storage !=
                 source_frontend_persistence_storage::none) {
            return {status_code::initialization_failed};
        }

            write_u32(directory_record + 4, flags);
        }
    }

    if (text_cursor != source_bytes_count ||
        static_cast<std::size_t>(local_type_cursor) != local_type_count ||
        static_cast<std::size_t>(type_slot_cursor) != type_slot_count ||
        static_cast<std::size_t>(object_slot_cursor) != object_slot_count ||
        static_cast<std::size_t>(member_slot_cursor) != member_slot_count ||
        observed_frontends != frontend_count) {
        return {status_code::initialization_failed};
    }

    if (telemetry != nullptr)
        telemetry->source_frontend_ns =
            elapsed(source_frontend_begin);

    const auto contribution_begin =
        std::chrono::steady_clock::now();

    const auto write_contribution_state =
        [&](std::byte* target,
            const source_contribution_state& value) noexcept {

            write_u32(target, value.source.value());
            write_u32(target + 4, 0);
            write_range(target + 8, value.types);
            write_range(target + 16, value.members);
            write_range(target + 24, value.modifiers);
            write_range(target + 32, value.enum_values);
            write_range(target + 40, value.objects);
            write_range(target + 48, value.links);
        };

    if (!encode_mapped_baseline_section(
            build_cache_image_section::contribution_states,
            contribution.sources,
            contribution_state_record_size,
            write_contribution_state)) {

        auto* target =
            section_data(
                build_cache_image_section::contribution_states);

        for (std::size_t index = 0;
             index < contribution.sources.size();
             ++index) {

            write_contribution_state(
                target +
                    index * contribution_state_record_size,
                contribution.sources[index]);
        }
    }

    const auto write_contribution_type =
        [&](std::byte* target,
            const source_contribution_type& value) noexcept {

            write_u32(target, value.identity.value());
            write_range(target + 4, value.definition_items);
            target[12] = static_cast<std::byte>(
                static_cast<std::uint8_t>(
                    value.explicit_underlying));
            target[13] = static_cast<std::byte>(
                static_cast<std::uint8_t>(
                    value.record_kind));
            target[14] = static_cast<std::byte>(
                static_cast<std::uint8_t>(
                    value.kind));
            target[15] =
                static_cast<std::byte>(value.flags);
        };

    if (!encode_mapped_baseline_section(
            build_cache_image_section::contribution_types,
            contribution.types,
            contribution_type_record_size,
            write_contribution_type)) {

        auto* target =
            section_data(
                build_cache_image_section::contribution_types);

        for (std::size_t index = 0;
             index < contribution.types.size();
             ++index) {

            write_contribution_type(
                target +
                    index * contribution_type_record_size,
                contribution.types[index]);
        }
    }

    const auto write_contribution_member =
        [&](std::byte* target,
            const source_contribution_member& value) noexcept {

            write_u32(
                target,
                value.type.identity.value());
            write_range(
                target + 4,
                value.type.modifiers);
            target[12] = static_cast<std::byte>(
                static_cast<std::uint8_t>(
                    value.type.intrinsic));
            write_u32(
                target + 16,
                value.name.value());
            target[20] = static_cast<std::byte>(
                static_cast<std::uint8_t>(
                    value.access));
        };

    if (!encode_mapped_baseline_section(
            build_cache_image_section::contribution_members,
            contribution.members,
            contribution_member_record_size,
            write_contribution_member)) {

        auto* target =
            section_data(
                build_cache_image_section::contribution_members);

        for (std::size_t index = 0;
             index < contribution.members.size();
             ++index) {

            write_contribution_member(
                target +
                    index * contribution_member_record_size,
                contribution.members[index]);
        }
    }

    const auto write_contribution_modifier =
        [&](std::byte* target,
            const source_type_modifier& value) noexcept {

            write_u64(target, value.value);
            target[8] = static_cast<std::byte>(
                static_cast<std::uint8_t>(
                    value.kind));
        };

    if (!encode_mapped_baseline_section(
            build_cache_image_section::contribution_modifiers,
            contribution.modifiers,
            contribution_modifier_record_size,
            write_contribution_modifier)) {

        auto* target =
            section_data(
                build_cache_image_section::contribution_modifiers);

        for (std::size_t index = 0;
             index < contribution.modifiers.size();
             ++index) {

            write_contribution_modifier(
                target +
                    index * contribution_modifier_record_size,
                contribution.modifiers[index]);
        }
    }

    const auto write_contribution_enum_value =
        [&](std::byte* target,
            const source_contribution_enum_value& value) noexcept {

            write_u32(target, value.name.value());
            target[4] = static_cast<std::byte>(
                static_cast<std::uint8_t>(
                    value.value.intrinsic));
            write_u64(
                target + 8,
                value.value.bits);
        };

    if (!encode_mapped_baseline_section(
            build_cache_image_section::contribution_enum_values,
            contribution.enum_values,
            contribution_enum_value_record_size,
            write_contribution_enum_value)) {

        auto* target =
            section_data(
                build_cache_image_section::contribution_enum_values);

        for (std::size_t index = 0;
             index < contribution.enum_values.size();
             ++index) {

            write_contribution_enum_value(
                target +
                    index *
                        contribution_enum_value_record_size,
                contribution.enum_values[index]);
        }
    }

    const auto write_contribution_object =
        [&](std::byte* target,
            const source_contribution_object& value) noexcept {

            write_u32(
                target,
                value.identity.value());
            write_u32(
                target + 4,
                value.type.identity.value());
            write_range(
                target + 8,
                value.type.modifiers);
            target[16] = static_cast<std::byte>(
                static_cast<std::uint8_t>(
                    value.type.intrinsic));
        };

    if (!encode_mapped_baseline_section(
            build_cache_image_section::contribution_objects,
            contribution.objects,
            contribution_object_record_size,
            write_contribution_object)) {

        auto* target =
            section_data(
                build_cache_image_section::contribution_objects);

        for (std::size_t index = 0;
             index < contribution.objects.size();
             ++index) {

            write_contribution_object(
                target +
                    index * contribution_object_record_size,
                contribution.objects[index]);
        }
    }

    const auto write_contribution_link =
        [&](std::byte* target,
            const source_contribution_link& value) noexcept {

            write_u32(
                target,
                value.source.object.value());
            write_u32(
                target + 4,
                value.source.member.value());
            write_u32(
                target + 8,
                value.target.object.value());
            write_u32(
                target + 12,
                value.target.member.value());
        };

    if (!encode_mapped_baseline_section(
            build_cache_image_section::contribution_links,
            contribution.links,
            contribution_link_record_size,
            write_contribution_link)) {

        auto* target =
            section_data(
                build_cache_image_section::contribution_links);

        for (std::size_t index = 0;
             index < contribution.links.size();
             ++index) {

            write_contribution_link(
                target +
                    index * contribution_link_record_size,
                contribution.links[index]);
        }
    }

    const auto write_construction_state =
        [&](std::byte* target,
            const source_construction_state& value) noexcept {

            write_u32(target, value.declarations);
            write_u32(target + 4, value.definitions);
            write_u32(target + 8, value.definition_type);
            write_u32(target + 12, value.record_struct);
            write_u32(target + 16, value.record_class);
            write_u32(target + 20, value.record_union);
            write_u32(target + 24, value.enum_scoped);
            write_u32(target + 28, value.enum_unscoped);
            write_u32(target + 32, value.enum_fixed);
            target[36] = static_cast<std::byte>(
                static_cast<std::uint8_t>(
                    value.fixed_underlying));
            target[37] = static_cast<std::byte>(
                static_cast<std::uint8_t>(
                    value.kind));
        };

    if (!encode_mapped_baseline_section(
            build_cache_image_section::construction_states,
            contribution.construction,
            construction_state_record_size,
            write_construction_state)) {

        auto* target =
            section_data(
                build_cache_image_section::construction_states);

        for (std::size_t index = 0;
             index < contribution.construction.size();
             ++index) {

            write_construction_state(
                target +
                    index * construction_state_record_size,
                contribution.construction[index]);
        }
    }

    if (telemetry != nullptr)
        telemetry->contribution_ns =
            elapsed(contribution_begin);

    const auto graph_begin =
        std::chrono::steady_clock::now();

    auto* intrinsic_refs =
        section_data(build_cache_image_section::graph_intrinsic_refs);
    auto* named_refs =
        section_data(build_cache_image_section::graph_named_refs);
    auto* derived_index =
        section_data(build_cache_image_section::graph_derived_index);
    auto* dependency_versions =
        section_data(build_cache_image_section::graph_dependency_versions);
    auto* reverse_heads =
        section_data(build_cache_image_section::graph_reverse_dependency_heads);
    auto* dependency_edges =
        section_data(build_cache_image_section::graph_dependency_edges);

    const auto native_named_refs =
        static_cast<std::span<const TypeRef>>(graph.named_refs);
    const auto native_derived_index =
        static_cast<std::span<const graph_derived_index_slot>>(
            graph.derived_index);
    const auto native_dependency_versions =
        static_cast<std::span<const std::uint32_t>>(
            graph.dependency_versions);
    const auto native_reverse_heads =
        static_cast<std::span<const std::uint32_t>>(
            graph.reverse_dependency_heads);
    const auto native_dependency_edges =
        static_cast<std::span<const graph_dependency_edge>>(
            graph.dependency_edges);
    const auto native_type_identity_index =
        static_cast<std::span<const graph_identity_index_slot>>(
            graph.type_identity_index);
    const auto native_object_identity_index =
        static_cast<std::span<const graph_object_identity_index_slot>>(
            graph.object_identity_index);
    const auto native_link_target_index =
        static_cast<std::span<const graph_link_index_slot>>(
            graph.link_target_index);

    // GEN-02C29: fresh-G0 native Graph bulk serialization.
    // Fresh Graph owns contiguous canonical arrays. On little-endian their
    // proven layouts are byte-identical to Build Cache v4. Baseline/sparse and
    // non-little-endian paths retain the field-wise encoder below.
    const bool use_native_graph_storage =
        std::endian::native == std::endian::little &&
        !project.compiled_graph().baseline_backed() &&
        native_named_refs.size() == graph.named_refs.size() &&
        native_derived_index.size() == graph.derived_index.size() &&
        native_dependency_versions.size() ==
            graph.dependency_versions.size() &&
        native_reverse_heads.size() ==
            graph.reverse_dependency_heads.size() &&
        native_dependency_edges.size() ==
            graph.dependency_edges.size() &&
        native_type_identity_index.size() ==
            graph.type_identity_index.size() &&
        native_object_identity_index.size() ==
            graph.object_identity_index.size() &&
        native_link_target_index.size() ==
            graph.link_target_index.size();

    const auto copy_native_graph_section =
        [&](build_cache_image_section kind, const auto& values) noexcept {
            if (!values.empty()) {
                std::memcpy(
                    section_data(kind),
                    values.data(),
                    values.size_bytes());
            }
        };

    if (use_native_graph_storage) {
        copy_native_graph_section(
            build_cache_image_section::graph_intrinsic_refs,
            graph.intrinsic_refs);
        copy_native_graph_section(
            build_cache_image_section::graph_named_refs,
            native_named_refs);
        copy_native_graph_section(
            build_cache_image_section::graph_derived_index,
            native_derived_index);
        copy_native_graph_section(
            build_cache_image_section::graph_dependency_versions,
            native_dependency_versions);
        copy_native_graph_section(
            build_cache_image_section::graph_reverse_dependency_heads,
            native_reverse_heads);
        copy_native_graph_section(
            build_cache_image_section::graph_dependency_edges,
            native_dependency_edges);
        copy_native_graph_section(
            build_cache_image_section::graph_type_identity_index,
            native_type_identity_index);
        copy_native_graph_section(
            build_cache_image_section::graph_object_identity_index,
            native_object_identity_index);
        copy_native_graph_section(
            build_cache_image_section::graph_link_target_index,
            native_link_target_index);
    } else {
        for (std::size_t index = 0;
             index < graph.intrinsic_refs.size();
             ++index) {

            write_u32(
                intrinsic_refs + index * 4,
                graph.intrinsic_refs[index].value());
        }

        const auto write_type_ref =
            [&](std::byte* target,
                const TypeRef& value) noexcept {

                write_u32(target, value.value());
            };

        if (!encode_mapped_baseline_section(
                build_cache_image_section::graph_named_refs,
                graph.named_refs,
                type_ref_record_size,
                write_type_ref)) {

            for (std::size_t index = 0;
                 index < graph.named_refs.size();
                 ++index) {

                write_type_ref(
                    named_refs + index * type_ref_record_size,
                    graph.named_refs[index]);
            }
        }

        const auto write_derived_index =
            [&](std::byte* target,
                const graph_derived_index_slot& value) noexcept {

                write_u32(
                    target,
                    value.fingerprint);
                write_u32(
                    target + 4,
                    value.type_ref);
            };

        if (!encode_mapped_baseline_section(
                build_cache_image_section::graph_derived_index,
                graph.derived_index,
                derived_index_record_size,
                write_derived_index)) {

            for (std::size_t index = 0;
                 index < graph.derived_index.size();
                 ++index) {

                write_derived_index(
                    derived_index +
                        index * derived_index_record_size,
                    graph.derived_index[index]);
            }
        }

        const auto write_u32_record =
            [&](std::byte* target,
                const std::uint32_t& value) noexcept {

                write_u32(target, value);
            };

        if (!encode_mapped_baseline_section(
                build_cache_image_section::graph_dependency_versions,
                graph.dependency_versions,
                u32_record_size,
                write_u32_record)) {

            for (std::size_t index = 0;
                 index < graph.dependency_versions.size();
                 ++index) {

                write_u32_record(
                    dependency_versions +
                        index * u32_record_size,
                    graph.dependency_versions[index]);
            }
        }

        if (!encode_mapped_baseline_section(
                build_cache_image_section::graph_reverse_dependency_heads,
                graph.reverse_dependency_heads,
                u32_record_size,
                write_u32_record)) {

            for (std::size_t index = 0;
                 index < graph.reverse_dependency_heads.size();
                 ++index) {

                write_u32_record(
                    reverse_heads +
                        index * u32_record_size,
                    graph.reverse_dependency_heads[index]);
            }
        }

        const auto write_dependency_edge =
            [&](std::byte* target,
                const graph_dependency_edge& value) noexcept {

                write_u32(
                    target,
                    value.owner_handle);
                write_u32(
                    target + 4,
                    value.next_for_target);
                write_u32(
                    target + 8,
                    value.owner_version);
            };

        if (!encode_mapped_baseline_section(
                build_cache_image_section::graph_dependency_edges,
                graph.dependency_edges,
                dependency_edge_record_size,
                write_dependency_edge)) {

            for (std::size_t index = 0;
                 index < graph.dependency_edges.size();
                 ++index) {

                write_dependency_edge(
                    dependency_edges +
                        index * dependency_edge_record_size,
                    graph.dependency_edges[index]);
            }
        }

        const auto encode_historical_index =
            [&](build_cache_image_section kind,
                const auto& values) noexcept {

                const auto write_index =
                    [&](std::byte* target,
                        const auto& value) noexcept {

                        write_u32(
                            target,
                            value.fingerprint);
                        write_u32(
                            target + 4,
                            value.handle);
                    };

                if (encode_mapped_baseline_section(
                        kind,
                        values,
                        historical_index_record_size,
                        write_index)) {
                    return;
                }

                auto* target = section_data(kind);

                for (std::size_t index = 0;
                     index < values.size();
                     ++index) {

                    write_index(
                        target +
                            index *
                                historical_index_record_size,
                        values[index]);
                }
            };

        encode_historical_index(
            build_cache_image_section::graph_type_identity_index,
            graph.type_identity_index);
        encode_historical_index(
            build_cache_image_section::graph_object_identity_index,
            graph.object_identity_index);
        encode_historical_index(
            build_cache_image_section::graph_link_target_index,
            graph.link_target_index);
    }

    if (telemetry != nullptr)
        telemetry->graph_ns =
            elapsed(graph_begin);
    const auto change_identity_begin =
        std::chrono::steady_clock::now();

    auto* source_file_index =
        section_data(
            build_cache_image_section::source_file_identity_index);
    for (std::size_t index = 0;
         index < change_capture.file_index.size();
         ++index) {
        const auto& value =
            change_capture.file_index[index];
        auto* target =
            source_file_index +
            index *
                source_file_identity_index_record_size;
        write_u64(target, value.file_reference);
        write_u32(target + 8, value.source.value());
        write_u32(target + 12, 0);
    }

    auto* tracked_directory_index =
        section_data(
            build_cache_image_section::tracked_directory_identity_index);
    for (std::size_t index = 0;
         index < change_capture.directory_index.size();
         ++index) {

        const auto& value =
            change_capture.directory_index[index];

        auto* target =
            tracked_directory_index +
            index *
                tracked_directory_identity_index_record_size;

        write_u64(target, value.file_reference);
        write_u32(target + 8, value.flags);
        write_u32(target + 12, 0);
    }

    if (telemetry != nullptr)
        telemetry->change_identity_ns =
            elapsed(change_identity_begin);

    const auto section_crc_begin =
        std::chrono::steady_clock::now();

    const auto parallel_crc_worker_count =
        (std::min)(
            layout.size(),
            (std::max)(
                std::size_t{1},
                static_cast<std::size_t>(
                    std::thread::hardware_concurrency())));

    std::atomic<std::size_t> next_crc_section{0};
    std::atomic<bool> crc_failed{false};

    const auto crc_worker = [&]() noexcept {
        for (;;) {
            if (crc_failed.load(
                    std::memory_order_relaxed)) {
                return;
            }

            const auto index =
                next_crc_section.fetch_add(
                    1,
                    std::memory_order_relaxed);

            if (index >= layout.size())
                return;

            auto& value = layout[index];
            std::uint64_t byte_count = 0;
            if (!multiply_u64(
                    value.count,
                    value.record_size,
                    byte_count) ||
                byte_count >
                    (std::numeric_limits<std::size_t>::max)()) {
                crc_failed.store(
                    true,
                    std::memory_order_relaxed);
                return;
            }

            const auto expected_crc_bytes =
                static_cast<std::size_t>(byte_count);

            std::span<const std::byte> crc_bytes;

            if (owned != nullptr) {
                const auto& section =
                    owned->sections[index];

                crc_bytes = {
                    section.empty()
                        ? base
                        : section.data(),
                    section.size(),
                };
            }
            else {
                crc_bytes = {
                    base + static_cast<std::size_t>(value.offset),
                    expected_crc_bytes,
                };
            }

            if (borrowed != nullptr) {
                const auto exact =
                    borrowed->sections[index];

                if (!exact.empty()) {
                    if (exact.size() != expected_crc_bytes) {
                        crc_failed.store(
                            true,
                            std::memory_order_relaxed);
                        return;
                    }

                    crc_bytes = exact;
                }
            }

            if (sparse != nullptr &&
                !sparse->sections[index].empty()) {

                const auto& segment =
                    sparse->sections[index];

                if (segment.size() !=
                    expected_crc_bytes) {
                    crc_failed.store(
                        true,
                        std::memory_order_relaxed);
                    return;
                }

                std::uint64_t crc = 0;
                for (std::size_t extent_index = 0;
                     extent_index <
                        segment.extent_count();
                     ++extent_index) {

                    crc = persistence_crc64_update(
                        crc,
                        segment.extent(
                            extent_index));
                }

                value.crc64 = crc;
            }
            else {
                if (crc_bytes.size() != expected_crc_bytes) {
                    crc_failed.store(
                        true,
                        std::memory_order_relaxed);
                    return;
                }

                value.crc64 =
                    persistence_crc64(
                        crc_bytes);
            }

        }
    };

    if (parallel_crc_worker_count == 1) {
        crc_worker();
    }
    else {
        try {
            std::vector<std::jthread> crc_workers;
            crc_workers.reserve(
                parallel_crc_worker_count);

            for (std::size_t worker = 0;
                 worker < parallel_crc_worker_count;
                 ++worker) {

                crc_workers.emplace_back(
                    [&]() noexcept {
                        crc_worker();
                    });
            }
        }
        catch (const std::bad_alloc&) {
            output.clear();
            return {status_code::not_available};
        }
        catch (const std::length_error&) {
            output.clear();
            return {status_code::not_available};
        }
        catch (const std::system_error&) {
            output.clear();
            return {status_code::not_available};
        }
    }

    if (crc_failed.load(
            std::memory_order_relaxed)) {
        output.clear();
        return {status_code::not_available};
    }

    if (telemetry != nullptr)
        telemetry->section_crc_ns =
            elapsed(section_crc_begin);

    const auto header_directory_begin =
        std::chrono::steady_clock::now();

    std::copy(image_magic.begin(), image_magic.end(), base);
    write_u32(base + 8, build_cache_image_format_version);
    write_u32(base + 12, endian_marker);
    write_u32(base + 16, build_cache_image_header_size);
    write_u32(base + 20, build_cache_image_directory_count);
    write_u32(base + 24, build_cache_image_directory_entry_size);
    write_u32(
        base + header_flags_offset,
        flag_frontend_complete | flag_contributions_complete);
    write_u64(base + 32, directory_offset);
    write_u64(base + 40, logical_size);
    write_u64(base + header_source_count_offset, source_count);
    write_u64(base + header_frontend_count_offset, frontend_count);
    write_u64(base + header_source_bytes_offset, source_bytes_count);
    write_u64(
        base + header_derived_entries_offset,
        graph.derived_index_entries);

    write_u64(
        base + header_statistics_offset,
        contribution.statistics.sources);
    write_u64(
        base + header_statistics_offset + 8,
        contribution.statistics.type_declarations);
    write_u64(
        base + header_statistics_offset + 16,
        contribution.statistics.members);
    write_u64(
        base + header_statistics_offset + 24,
        contribution.statistics.modifiers);
    write_u64(
        base + header_statistics_offset + 32,
        contribution.statistics.enum_values);
    write_u64(
        base + header_statistics_offset + 40,
        contribution.statistics.objects);
    write_u64(
        base + header_statistics_offset + 48,
        contribution.statistics.links);

    write_u32(
        base + header_change_backend_offset,
        static_cast<std::uint32_t>(
            change_capture.checkpoint.backend));
    write_u32(
        base + header_change_backend_offset + 4,
        0);
    write_u64(
        base + header_change_volume_offset,
        change_capture.checkpoint.volume_serial);
    write_u64(
        base + header_change_journal_offset,
        change_capture.checkpoint.journal_id);
    write_u64(
        base + header_change_usn_offset,
        static_cast<std::uint64_t>(
            change_capture.checkpoint.next_usn));

    for (std::size_t index = 0; index < layout.size(); ++index) {
        const auto& value = layout[index];
        auto* entry =
            base +
            directory_offset +
            index * build_cache_image_directory_entry_size;

        write_u32(
            entry,
            static_cast<std::uint32_t>(value.kind));
        write_u32(entry + 4, value.record_size);
        write_u64(entry + 8, value.offset);
        write_u64(entry + 16, value.count);
        write_u64(entry + 24, value.crc64);
    }

    write_u64(
        base + header_directory_crc_offset,
        persistence_crc64(std::span<const std::byte>{
            base + directory_offset,
            directory_bytes}));

    std::array<std::byte, build_cache_image_header_size> header{};
    std::memcpy(header.data(), base, header.size());
    write_u64(header.data() + header_crc_offset, 0);
    write_u64(
        base + header_crc_offset,
        persistence_crc64(header));

    if (telemetry != nullptr)
        telemetry->header_directory_ns =
            elapsed(header_directory_begin);

    build_cache_image_view validation;

    const auto bind_begin =
        std::chrono::steady_clock::now();

    status result;

    if (owned != nullptr) {
        // R5E2D-B_SECTIONED_VALIDATION
        std::array<
            project_generation_segment,
            build_cache_image_directory_count>
            section_values{};

        for (std::size_t index = 0;
             index < section_values.size();
             ++index) {

            if (sparse != nullptr &&
                !sparse->sections[index].empty()) {

                section_values[index] =
                    sparse->sections[index];
                continue;
            }

            if (borrowed != nullptr &&
                !borrowed->sections[index].empty()) {

                section_values[index] =
                    project_generation_segment{
                        borrowed->sections[index]};
                continue;
            }

            const auto& section =
                owned->sections[index];

            section_values[index] =
                project_generation_segment{
                    std::span<const std::byte>{
                        section.data(),
                        section.size()}};
        }

        result =
            validation.bind_sectioned(
                std::span<const std::byte>{
                    output.data(),
                    output.size()},
                section_values);
    }
    else {
        result =
            sparse != nullptr &&
                sparse->any()
            ? validation.bind_encoded_sparse(
                output,
                borrowed != nullptr
                    ? *borrowed
                    : build_cache_encode_borrowed_sections{},
                *sparse)
            : borrowed != nullptr &&
                borrowed->any()
                ? validation.bind_encoded_mixed(
                    output,
                    *borrowed)
                : validation.bind(output);
    }

    if (telemetry != nullptr)
        telemetry->bind_ns =
            elapsed(bind_begin);

    if (!result.ok()) {
        output.clear();
        return result;
    }

    // Fresh construction is guarded by writer invariants, section CRCs,
    // structural bind(), and the freeze-level verify_against() gate.
    if (telemetry != nullptr)
        telemetry->total_ns =
            elapsed(encode_begin);

    return {};
}

} // namespace cw::server
