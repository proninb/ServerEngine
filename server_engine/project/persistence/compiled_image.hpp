#pragma once

#include "../../member_index.hpp"
#include "../../status.hpp"
#include "../../string_id.hpp"
#include "../graph/graph.hpp"
#include "../project_generation_segments.hpp"
#include "../identity/identity_node.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace cw::server {

class project_context;

inline constexpr std::uint32_t compiled_image_format_version = 3;
inline constexpr std::size_t compiled_image_header_size = 256;
inline constexpr std::size_t compiled_image_directory_count = 16;
inline constexpr std::size_t compiled_image_directory_entry_size = 32;
inline constexpr std::size_t compiled_image_prefix_size =
    (compiled_image_header_size +
     compiled_image_directory_count *
         compiled_image_directory_entry_size +
     63u) &
    ~std::size_t{63u};

enum class compiled_image_section : std::uint32_t {
    string_core = 1,
    string_index = 2,
    string_bytes = 3,
    identity_core = 4,
    identity_index = 5,
    types = 6,
    type_identities = 7,
    members = 8,
    enum_values = 9,
    objects = 10,
    object_identities = 11,
    links = 12,
    canonical_types = 13,
    graph_type_index = 14,
    graph_object_index = 15,
    graph_link_index = 16,
};

struct compiled_image_type_record final {
    definition_range definition{};
    graph_type_kind kind = graph_type_kind::record;
    source_record_kind record_kind = source_record_kind::struct_type;
    intrinsic_type enum_underlying = intrinsic_type::none;
    std::uint8_t flags = 0;

    [[nodiscard]] constexpr bool defined() const noexcept {
        return definition.valid();
    }

    [[nodiscard]] constexpr bool enum_scoped() const noexcept {
        return (flags & 0x01u) != 0;
    }

    [[nodiscard]] constexpr bool enum_fixed_underlying() const noexcept {
        return (flags & 0x02u) != 0;
    }

    [[nodiscard]] constexpr bool live() const noexcept {
        return (flags & 0x80u) != 0;
    }
};

struct compiled_image_member_record final {
    string_id name{};
    TypeRef type{};
    source_member_access access = source_member_access::public_access;
    construction_value construction{};
};

struct compiled_image_enum_value_record final {
    std::uint64_t bits = 0;
    string_id name{};
    intrinsic_type intrinsic = intrinsic_type::signed_int;
};

struct compiled_image_object_record final {
    TypeRef type{};
    std::uint32_t flags = 0;

    [[nodiscard]] constexpr bool live() const noexcept {
        return (flags & 0x80000000u) != 0;
    }
};

struct compiled_image_link_record final {
    object_endpoint source{};
    object_endpoint target{};

    [[nodiscard]] constexpr bool live() const noexcept {
        return static_cast<bool>(source.object) &&
            static_cast<bool>(target.object);
    }
};

struct compiled_image_canonical_type_record final {
    std::uint64_t payload = 0;
    std::uint32_t child_or_handle = 0;
    canonical_type_kind kind = canonical_type_kind::intrinsic;
    std::uint8_t detail = 0;
};

static_assert(sizeof(compiled_image_type_record) == 12);
static_assert(sizeof(compiled_image_member_record) == 28);
static_assert(sizeof(compiled_image_enum_value_record) == 16);
static_assert(sizeof(compiled_image_object_record) == 8);
static_assert(sizeof(compiled_image_link_record) == 16);
static_assert(sizeof(compiled_image_canonical_type_record) == 16);

struct compiled_image_encode_telemetry final {
    std::uint64_t total_ns = 0;
    std::uint64_t sizing_layout_ns = 0;
    std::uint64_t allocate_zero_ns = 0;
    std::uint64_t strings_ns = 0;
    std::uint64_t identities_ns = 0;
    std::uint64_t graph_arrays_ns = 0;
    std::uint64_t graph_indexes_ns = 0;
    std::uint64_t section_crc_ns = 0;
    std::uint64_t header_bind_ns = 0;
    std::uint64_t baseline_bulk_bytes = 0;
    std::uint32_t baseline_bulk_sections = 0;
    std::uint64_t output_bytes = 0;
};

// Read-only mmap-native semantic/query view over compiled.bin v1. It owns no
// Project storage and performs no reconstruction, allocation, or semantic
// canonicalization after bind().
class compiled_image_view final {
public:
    compiled_image_view() noexcept = default;

    [[nodiscard]] status bind(std::span<const std::byte> image) noexcept;

    // Binds the same compiled.bin v1 logical image from independent immutable
    // sections. Directory offsets remain file offsets; no contiguous in-memory
    // copy is required.
    [[nodiscard]] status bind_sectioned(
        std::span<const std::byte> prefix,
        const std::array<
            std::span<const std::byte>,
            compiled_image_directory_count>& section_images) noexcept;

    void reset() noexcept;

    [[nodiscard]] bool valid() const noexcept {
        return prefix_bytes.data() != nullptr;
    }

    [[nodiscard]] std::size_t string_count() const noexcept {
        return string_live_count;
    }

    [[nodiscard]] std::size_t string_slot_count() const noexcept;
    [[nodiscard]] string_id string_at_slot(std::size_t index) const noexcept;
    [[nodiscard]] std::string_view string(string_id id) const noexcept;
    [[nodiscard]] status find_string(
        std::string_view value,
        string_id& output) const noexcept;

    [[nodiscard]] std::size_t identity_count() const noexcept {
        return identity_live_count;
    }

    [[nodiscard]] std::size_t identity_slot_count() const noexcept;
    [[nodiscard]] identity_ref identity_at_slot(std::size_t index) const noexcept;
    [[nodiscard]] identity_ref identity_root() const noexcept;
    [[nodiscard]] bool identity_valid(identity_ref identity) const noexcept;
    [[nodiscard]] identity_ref identity_parent(identity_ref identity) const noexcept;
    [[nodiscard]] string_id identity_name(identity_ref identity) const noexcept;
    [[nodiscard]] status find_identity(
        identity_ref parent,
        string_id name,
        identity_kind kind,
        identity_ref& output) const noexcept;

    [[nodiscard]] std::size_t type_count() const noexcept {
        return live_type_count;
    }

    [[nodiscard]] std::size_t type_slot_count() const noexcept;
    [[nodiscard]] type_handle type_at(std::size_t index) const noexcept;
    [[nodiscard]] status type(
        type_handle handle,
        compiled_image_type_record& output) const noexcept;
    [[nodiscard]] status type_raw(
        type_handle handle,
        compiled_image_type_record& output) const noexcept;
    [[nodiscard]] identity_ref identity(type_handle handle) const noexcept;
    [[nodiscard]] identity_ref type_identity_at_slot(std::size_t index) const noexcept;
    [[nodiscard]] type_handle find_type(identity_ref identity) const noexcept;

    [[nodiscard]] std::size_t member_count(type_handle handle) const noexcept;
    [[nodiscard]] status member(
        type_handle handle,
        member_index index,
        compiled_image_member_record& output) const noexcept;
    [[nodiscard]] member_index find_member(
        type_handle handle,
        string_id name) const noexcept;
    [[nodiscard]] std::size_t member_slot_count() const noexcept;
    [[nodiscard]] status member_at_slot(
        std::size_t index,
        compiled_image_member_record& output) const noexcept;

    [[nodiscard]] std::size_t enum_value_count(type_handle handle) const noexcept;
    [[nodiscard]] status enum_value(
        type_handle handle,
        std::size_t index,
        compiled_image_enum_value_record& output) const noexcept;
    [[nodiscard]] std::size_t enum_value_slot_count() const noexcept;
    [[nodiscard]] status enum_value_at_slot(
        std::size_t index,
        compiled_image_enum_value_record& output) const noexcept;

    [[nodiscard]] std::size_t object_count() const noexcept {
        return live_object_count;
    }

    [[nodiscard]] std::size_t object_slot_count() const noexcept;
    [[nodiscard]] object_handle object_at(std::size_t index) const noexcept;
    [[nodiscard]] status object(
        object_handle handle,
        compiled_image_object_record& output) const noexcept;
    [[nodiscard]] status object_raw(
        object_handle handle,
        compiled_image_object_record& output) const noexcept;
    [[nodiscard]] identity_ref identity(object_handle handle) const noexcept;
    [[nodiscard]] identity_ref object_identity_at_slot(std::size_t index) const noexcept;
    [[nodiscard]] object_handle find_object(identity_ref identity) const noexcept;

    [[nodiscard]] std::size_t link_count() const noexcept {
        return live_link_count;
    }

    [[nodiscard]] std::size_t link_slot_count() const noexcept;
    [[nodiscard]] status link(
        link_handle handle,
        compiled_image_link_record& output) const noexcept;
    [[nodiscard]] status link_raw(
        link_handle handle,
        compiled_image_link_record& output) const noexcept;
    [[nodiscard]] link_handle find_link(object_endpoint target) const noexcept;

    [[nodiscard]] std::size_t canonical_type_slot_count() const noexcept;
    [[nodiscard]] status canonical_type(
        TypeRef type,
        compiled_image_canonical_type_record& output) const noexcept;
    [[nodiscard]] status canonical_type_at_slot(
        std::size_t index,
        compiled_image_canonical_type_record& output) const noexcept;
    [[nodiscard]] bool intrinsic(
        TypeRef type,
        intrinsic_type& output) const noexcept;
    [[nodiscard]] bool named(
        TypeRef type,
        type_handle& output) const noexcept;
    [[nodiscard]] bool derived(
        TypeRef type,
        derived_type_record& output) const noexcept;

    [[nodiscard]] std::span<const std::byte> section_bytes(
        compiled_image_section kind) const noexcept;

    // Cold integrity/semantic audit. Fast LOAD needs bind() only.
    [[nodiscard]] status verify_contents() const noexcept;

private:
    struct section_view final {
        const std::byte* data = nullptr;
        std::uint64_t count = 0;
        std::uint32_t record_size = 0;
        std::uint64_t crc64 = 0;
    };

    [[nodiscard]] const section_view& section(
        compiled_image_section kind) const noexcept;

    [[nodiscard]] identity_ref identity_from_raw(
        std::uint32_t value) const noexcept;

    [[nodiscard]] type_handle type_from_raw(
        std::uint32_t value) const noexcept;

    [[nodiscard]] object_handle object_from_raw(
        std::uint32_t value) const noexcept;

    [[nodiscard]] link_handle link_from_raw(
        std::uint32_t value) const noexcept;

    [[nodiscard]] TypeRef type_ref_from_raw(
        std::uint32_t value) const noexcept;

    [[nodiscard]] bool read_type_raw(
        std::uint32_t handle,
        compiled_image_type_record& output) const noexcept;

    [[nodiscard]] bool read_object_raw(
        std::uint32_t handle,
        compiled_image_object_record& output) const noexcept;

    [[nodiscard]] bool read_link_raw(
        std::uint32_t handle,
        compiled_image_link_record& output) const noexcept;

    std::span<const std::byte> bytes;
    std::span<const std::byte> prefix_bytes;
    section_view sections[compiled_image_directory_count]{};
    std::size_t string_live_count = 0;
    std::size_t identity_live_count = 0;
    std::size_t live_type_count = 0;
    std::size_t live_object_count = 0;
    std::size_t live_link_count = 0;
};

// Immutable compiled.bin v1 owner for a finalized Generation. The five
// string/identity sections remain byte-owned for now; eleven Graph/query
// sections may directly own vectors transferred from Graph.
class compiled_generation_storage final {
public:
    compiled_generation_storage() noexcept = default;

    compiled_generation_storage(
        const compiled_generation_storage&) = delete;
    compiled_generation_storage& operator=(
        const compiled_generation_storage&) = delete;
    compiled_generation_storage(
        compiled_generation_storage&&) noexcept = default;
    compiled_generation_storage& operator=(
        compiled_generation_storage&&) noexcept = default;

    void reset() noexcept;

    [[nodiscard]] bool valid() const noexcept {
        return valid_value;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return logical_size;
    }

    [[nodiscard]] std::size_t native_graph_bytes() const noexcept {
        return native_graph_bytes_value;
    }

    [[nodiscard]] std::size_t fallback_graph_bytes() const noexcept {
        return fallback_graph_bytes_value;
    }

    [[nodiscard]] std::uint32_t native_graph_sections() const noexcept {
        return native_graph_sections_value;
    }

    [[nodiscard]] std::uint32_t derived_graph_sections() const noexcept {
        return derived_graph_sections_value;
    }

    [[nodiscard]] std::size_t derived_graph_bytes() const noexcept {
        return derived_graph_bytes_value;
    }

    [[nodiscard]] std::uint32_t fallback_graph_mask() const noexcept {
        return fallback_graph_mask_value;
    }

    [[nodiscard]] std::uint32_t native_nonempty_graph_mask() const noexcept {
        return native_nonempty_graph_mask_value;
    }

    [[nodiscard]] std::uint32_t expected_nonzero_graph_mask() const noexcept {
        return expected_nonzero_graph_mask_value;
    }

    [[nodiscard]] status adopt_full_g0(
        std::vector<std::byte>& encoded,
        compiled_graph_generation_storage&& graph) noexcept;

    // Builds the finalized compiled.bin logical image directly from Project
    // string/identity state plus moved native Graph semantic arrays. Graph query
    // indexes are persistence-derived accelerators, never canonical Graph data.
    [[nodiscard]] status build_full_g0(
        const project_context& project,
        compiled_graph_generation_storage&& graph,
        compiled_image_encode_telemetry* telemetry = nullptr) noexcept;

    [[nodiscard]] status bind(
        compiled_image_view& output) const noexcept;

    [[nodiscard]] project_generation_segment
    segment() const noexcept;

private:
    [[nodiscard]] std::span<const std::byte>
    section_bytes(std::size_t index) const noexcept;

    std::array<std::byte, compiled_image_prefix_size> prefix{};
    std::array<
        std::vector<std::byte>,
        5> semantic_sections;
    std::array<
        std::vector<std::byte>,
        11> fallback_graph_sections;

    // Persisted Graph query indexes are derived from immutable semantic arrays.
    std::array<std::vector<std::byte>, 3> derived_query_indexes;

    // Empty minimum-capacity query indexes belong to compiled format,
    // not to semantic Graph ownership.
    std::array<bool, 11> derived_graph_section_flags{};
    std::array<std::byte, 16u * 8u> empty_query_index{};

    compiled_graph_generation_storage native_graph;

    std::array<std::uint64_t, compiled_image_directory_count>
        section_offsets{};
    std::array<std::uint64_t, compiled_image_directory_count>
        section_sizes{};

    std::size_t logical_size = 0;
    std::size_t native_graph_bytes_value = 0;
    std::size_t derived_graph_bytes_value = 0;
    std::size_t fallback_graph_bytes_value = 0;
    std::uint32_t native_graph_sections_value = 0;
    std::uint32_t derived_graph_sections_value = 0;
    std::uint32_t fallback_graph_mask_value = 0;
    std::uint32_t native_nonempty_graph_mask_value = 0;
    std::uint32_t expected_nonzero_graph_mask_value = 0;
    bool valid_value = false;
};

// Deterministic field-wise encoder used by the current in-memory SAVE staging
// path and tests. It preserves every numeric Project/Graph ID and slot. A later
// direct-file writer may share this schema without changing compiled.bin v1.
[[nodiscard]] status encode_compiled_image(
    const project_context& project,
    std::vector<std::byte>& output) noexcept;

[[nodiscard]] status encode_compiled_image(
    const project_context& project,
    std::vector<std::byte>& output,
    compiled_image_encode_telemetry* telemetry) noexcept;

} // namespace cw::server
