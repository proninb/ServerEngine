#pragma once

#include "../../member_index.hpp"
#include "../../status.hpp"
#include "../../string_id.hpp"
#include "../graph/graph.hpp"
#include "../identity/identity_node.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace cw::server {

class project_context;

inline constexpr std::uint32_t compiled_image_format_version = 1;
inline constexpr std::size_t compiled_image_header_size = 256;
inline constexpr std::size_t compiled_image_directory_count = 16;
inline constexpr std::size_t compiled_image_directory_entry_size = 32;

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
static_assert(sizeof(compiled_image_member_record) == 12);
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
    void reset() noexcept;

    [[nodiscard]] bool valid() const noexcept {
        return bytes.data() != nullptr;
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
    section_view sections[compiled_image_directory_count]{};
    std::size_t string_live_count = 0;
    std::size_t identity_live_count = 0;
    std::size_t live_type_count = 0;
    std::size_t live_object_count = 0;
    std::size_t live_link_count = 0;
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
