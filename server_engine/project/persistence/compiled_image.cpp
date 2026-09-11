#include "compiled_image.hpp"

#include "../project_context.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace cw::server {
namespace {

constexpr std::array<std::byte, 8> image_magic{
    std::byte{'S'}, std::byte{'E'}, std::byte{'C'}, std::byte{'M'},
    std::byte{'P'}, std::byte{'V'}, std::byte{'1'}, std::byte{0},
};

constexpr std::uint32_t endian_marker = 0x01020304u;
constexpr std::size_t directory_offset = compiled_image_header_size;
constexpr std::size_t directory_bytes =
    compiled_image_directory_count * compiled_image_directory_entry_size;
constexpr std::size_t first_section_offset =
    (directory_offset + directory_bytes + 63u) & ~std::size_t{63u};

constexpr std::size_t header_string_live_offset = 48;
constexpr std::size_t header_identity_live_offset = 56;
constexpr std::size_t header_type_live_offset = 64;
constexpr std::size_t header_object_live_offset = 72;
constexpr std::size_t header_link_live_offset = 80;
constexpr std::size_t header_reserved_begin = 88;
constexpr std::size_t header_directory_crc_offset = 240;
constexpr std::size_t header_crc_offset = 248;

constexpr std::uint32_t string_core_size = 16;
constexpr std::uint32_t index_record_size = 8;
constexpr std::uint32_t identity_core_size = 12;
constexpr std::uint32_t type_record_size = 12;
constexpr std::uint32_t member_record_size = 12;
constexpr std::uint32_t enum_value_record_size = 16;
constexpr std::uint32_t object_record_size = 8;
constexpr std::uint32_t link_record_size = 16;
constexpr std::uint32_t canonical_type_record_size = 16;

struct layout_section final {
    compiled_image_section kind{};
    std::uint32_t record_size = 0;
    std::uint64_t count = 0;
    std::uint64_t offset = 0;
    std::uint64_t crc64 = 0;
};

[[nodiscard]] constexpr std::size_t section_index(
    compiled_image_section kind) noexcept {

    const auto raw = static_cast<std::uint32_t>(kind);
    return raw >= 1 && raw <= compiled_image_directory_count
        ? static_cast<std::size_t>(raw - 1)
        : compiled_image_directory_count;
}

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

void write_u16(std::byte* target, std::uint16_t value) noexcept {
    target[0] = static_cast<std::byte>(value & 0xffu);
    target[1] = static_cast<std::byte>((value >> 8) & 0xffu);
}

void write_u32(std::byte* target, std::uint32_t value) noexcept {
    target[0] = static_cast<std::byte>(value & 0xffu);
    target[1] = static_cast<std::byte>((value >> 8) & 0xffu);
    target[2] = static_cast<std::byte>((value >> 16) & 0xffu);
    target[3] = static_cast<std::byte>((value >> 24) & 0xffu);
}

void write_u64(std::byte* target, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8; ++index)
        target[index] =
            static_cast<std::byte>((value >> (index * 8)) & 0xffu);
}

[[nodiscard]] std::uint16_t read_u16(const std::byte* source) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(source[0]) |
        (static_cast<std::uint16_t>(source[1]) << 8));
}

[[nodiscard]] std::uint32_t read_u32(const std::byte* source) noexcept {
    return
        static_cast<std::uint32_t>(source[0]) |
        (static_cast<std::uint32_t>(source[1]) << 8) |
        (static_cast<std::uint32_t>(source[2]) << 16) |
        (static_cast<std::uint32_t>(source[3]) << 24);
}

[[nodiscard]] std::uint64_t read_u64(const std::byte* source) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index)
        value |= static_cast<std::uint64_t>(source[index]) << (index * 8);
    return value;
}

constexpr std::uint64_t crc64_polynomial =
    0x42f0e1eba9ea3693ULL;

[[nodiscard]] std::uint64_t crc64(
    std::span<const std::byte> bytes) noexcept {

    std::uint64_t crc = 0;
    for (const auto byte : bytes) {
        crc ^= static_cast<std::uint64_t>(byte) << 56;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & (std::uint64_t{1} << 63)) != 0
                ? (crc << 1) ^ crc64_polynomial
                : crc << 1;
        }
    }
    return crc;
}

[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}

[[nodiscard]] std::uint32_t fold32(std::uint64_t value) noexcept {
    auto result = static_cast<std::uint32_t>(value ^ (value >> 32));
    return result == 0 ? 1u : result;
}

[[nodiscard]] std::uint64_t string_hash(std::string_view value) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char character : value) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
    return mix64(hash);
}

[[nodiscard]] std::uint64_t semantic_identity_hash(
    std::uint32_t parent,
    std::uint32_t name) noexcept {

    return mix64(
        static_cast<std::uint64_t>(parent) ^
        (static_cast<std::uint64_t>(name) << 32));
}

[[nodiscard]] std::uint64_t graph_identity_hash(
    std::uint32_t identity) noexcept {

    return mix64(static_cast<std::uint64_t>(identity));
}

[[nodiscard]] std::uint64_t endpoint_hash(
    std::uint32_t object,
    std::uint32_t member) noexcept {

    return mix64(
        (static_cast<std::uint64_t>(object) << 32) ^
        static_cast<std::uint64_t>(member));
}

[[nodiscard]] std::size_t index_capacity(std::size_t entries) noexcept {
    if (entries > ((std::numeric_limits<std::size_t>::max)() - 1) / 2)
        return 0;

    const auto target = entries * 2 + 1;
    std::size_t capacity = 16;

    while (capacity < target) {
        if (capacity > (std::numeric_limits<std::size_t>::max)() / 2)
            return 0;
        capacity *= 2;
    }

    return capacity;
}

[[nodiscard]] bool valid_index_capacity(std::uint64_t count) noexcept {
    return count >= 16 && (count & (count - 1)) == 0;
}

[[nodiscard]] bool valid_identity_kind(std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(identity_kind::object);
}

[[nodiscard]] bool valid_graph_type_kind(std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(graph_type_kind::enumeration);
}

[[nodiscard]] bool valid_record_kind(std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(source_record_kind::union_type);
}

[[nodiscard]] bool valid_intrinsic(std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(intrinsic_type::nullptr_type);
}

[[nodiscard]] bool valid_member_access(std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(
        source_member_access::private_access);
}

[[nodiscard]] bool valid_canonical_kind(std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(canonical_type_kind::derived);
}

[[nodiscard]] bool valid_derived_kind(std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(
        derived_type_kind::unbounded_array);
}

[[nodiscard]] constexpr std::uint32_t expected_record_size(
    compiled_image_section kind) noexcept {

    switch (kind) {
    case compiled_image_section::string_core:
        return string_core_size;
    case compiled_image_section::string_index:
    case compiled_image_section::identity_index:
    case compiled_image_section::graph_type_index:
    case compiled_image_section::graph_object_index:
    case compiled_image_section::graph_link_index:
        return index_record_size;
    case compiled_image_section::string_bytes:
        return 1;
    case compiled_image_section::identity_core:
        return identity_core_size;
    case compiled_image_section::types:
        return type_record_size;
    case compiled_image_section::type_identities:
    case compiled_image_section::object_identities:
        return 4;
    case compiled_image_section::members:
        return member_record_size;
    case compiled_image_section::enum_values:
        return enum_value_record_size;
    case compiled_image_section::objects:
        return object_record_size;
    case compiled_image_section::links:
        return link_record_size;
    case compiled_image_section::canonical_types:
        return canonical_type_record_size;
    }
    return 0;
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

} // namespace

const compiled_image_view::section_view& compiled_image_view::section(
    compiled_image_section kind) const noexcept {

    static const section_view empty{};
    const auto index = section_index(kind);
    return index < compiled_image_directory_count
        ? sections[index]
        : empty;
}

void compiled_image_view::reset() noexcept {
    bytes = {};
    for (auto& value : sections)
        value = {};
    string_live_count = 0;
    identity_live_count = 0;
    live_type_count = 0;
    live_object_count = 0;
    live_link_count = 0;
}

status compiled_image_view::bind(
    std::span<const std::byte> image) noexcept {

    reset();

    if (image.size() < first_section_offset)
        return {status_code::artifact_corrupt};

    if (!std::equal(image_magic.begin(), image_magic.end(), image.begin()))
        return {status_code::artifact_corrupt};

    if (read_u32(image.data() + 8) != compiled_image_format_version)
        return {status_code::rebuild_required};

    if (read_u32(image.data() + 12) != endian_marker ||
        read_u32(image.data() + 16) != compiled_image_header_size ||
        read_u32(image.data() + 20) != compiled_image_directory_count ||
        read_u32(image.data() + 24) != compiled_image_directory_entry_size ||
        read_u32(image.data() + 28) != 0 ||
        read_u64(image.data() + 32) != directory_offset ||
        read_u64(image.data() + 40) != image.size()) {
        return {status_code::artifact_corrupt};
    }

    if (!zero_bytes(
            image.data() + header_reserved_begin,
            header_directory_crc_offset - header_reserved_begin)) {
        return {status_code::artifact_corrupt};
    }

    std::array<std::byte, compiled_image_header_size> header{};
    std::memcpy(header.data(), image.data(), header.size());
    const auto stored_header_crc =
        read_u64(header.data() + header_crc_offset);
    write_u64(header.data() + header_crc_offset, 0);
    if (crc64(header) != stored_header_crc)
        return {status_code::artifact_corrupt};

    const auto directory_span =
        image.subspan(directory_offset, directory_bytes);
    if (crc64(directory_span) !=
        read_u64(image.data() + header_directory_crc_offset)) {
        return {status_code::artifact_corrupt};
    }

    section_view candidate[compiled_image_directory_count]{};
    std::uint64_t previous_end = first_section_offset;

    for (std::size_t index = 0;
         index < compiled_image_directory_count;
         ++index) {

        const auto* entry =
            image.data() +
            directory_offset +
            index * compiled_image_directory_entry_size;

        const auto raw_kind = read_u32(entry);
        const auto kind =
            static_cast<compiled_image_section>(raw_kind);
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

        candidate[index] = section_view{
            image.data() + static_cast<std::size_t>(offset),
            count,
            record_size,
            section_crc,
        };
        previous_end = end;
    }

    if (previous_end != image.size())
        return {status_code::artifact_corrupt};

    const auto& string_core =
        candidate[section_index(compiled_image_section::string_core)];
    const auto& string_index =
        candidate[section_index(compiled_image_section::string_index)];
    const auto& identity_core =
        candidate[section_index(compiled_image_section::identity_core)];
    const auto& identity_index =
        candidate[section_index(compiled_image_section::identity_index)];
    const auto& types =
        candidate[section_index(compiled_image_section::types)];
    const auto& type_identities =
        candidate[section_index(compiled_image_section::type_identities)];
    const auto& objects =
        candidate[section_index(compiled_image_section::objects)];
    const auto& object_identities =
        candidate[section_index(compiled_image_section::object_identities)];
    const auto& graph_type_index =
        candidate[section_index(compiled_image_section::graph_type_index)];
    const auto& graph_object_index =
        candidate[section_index(compiled_image_section::graph_object_index)];
    const auto& graph_link_index =
        candidate[section_index(compiled_image_section::graph_link_index)];

    const auto string_live = read_u64(image.data() + header_string_live_offset);
    const auto identity_live =
        read_u64(image.data() + header_identity_live_offset);
    const auto type_live = read_u64(image.data() + header_type_live_offset);
    const auto object_live = read_u64(image.data() + header_object_live_offset);
    const auto link_live = read_u64(image.data() + header_link_live_offset);

    if (string_live > string_core.count ||
        identity_core.count == 0 ||
        identity_live == 0 ||
        identity_live > identity_core.count ||
        type_live > types.count ||
        object_live > objects.count ||
        link_live >
            candidate[section_index(compiled_image_section::links)].count ||
        type_identities.count != types.count ||
        object_identities.count != objects.count ||
        string_core.count >
            (std::numeric_limits<std::uint32_t>::max)() ||
        identity_core.count > identity_ref::maximum_slot ||
        types.count >
            (std::numeric_limits<std::uint32_t>::max)() ||
        candidate[section_index(compiled_image_section::members)].count >
            (std::numeric_limits<std::uint32_t>::max)() ||
        candidate[section_index(compiled_image_section::enum_values)].count >
            (std::numeric_limits<std::uint32_t>::max)() ||
        objects.count >
            (std::numeric_limits<std::uint32_t>::max)() ||
        candidate[section_index(compiled_image_section::links)].count >
            (std::numeric_limits<std::uint32_t>::max)() ||
        candidate[section_index(compiled_image_section::canonical_types)].count >
            (std::numeric_limits<std::uint32_t>::max)() ||
        !valid_index_capacity(string_index.count) ||
        !valid_index_capacity(identity_index.count) ||
        !valid_index_capacity(graph_type_index.count) ||
        !valid_index_capacity(graph_object_index.count) ||
        !valid_index_capacity(graph_link_index.count) ||
        string_live > (std::numeric_limits<std::size_t>::max)() ||
        identity_live > (std::numeric_limits<std::size_t>::max)() ||
        type_live > (std::numeric_limits<std::size_t>::max)() ||
        object_live > (std::numeric_limits<std::size_t>::max)() ||
        link_live > (std::numeric_limits<std::size_t>::max)()) {
        return {status_code::artifact_corrupt};
    }

    bytes = image;
    std::copy(
        std::begin(candidate),
        std::end(candidate),
        std::begin(sections));

    string_live_count = static_cast<std::size_t>(string_live);
    identity_live_count = static_cast<std::size_t>(identity_live);
    live_type_count = static_cast<std::size_t>(type_live);
    live_object_count = static_cast<std::size_t>(object_live);
    live_link_count = static_cast<std::size_t>(link_live);

    return {};
}

std::size_t compiled_image_view::string_slot_count() const noexcept {
    return static_cast<std::size_t>(
        section(compiled_image_section::string_core).count);
}

std::string_view compiled_image_view::string(string_id id) const noexcept {
    if (!valid() || !id)
        return {};

    const auto& core = section(compiled_image_section::string_core);
    const auto& text = section(compiled_image_section::string_bytes);

    if (id.value() > core.count)
        return {};

    const auto* record =
        core.data +
        static_cast<std::size_t>(id.value() - 1) * string_core_size;

    const auto offset = read_u64(record);
    const auto length = read_u32(record + 8);
    if (length == 0 ||
        offset > text.count ||
        length > text.count - offset) {
        return {};
    }

    return {
        reinterpret_cast<const char*>(
            text.data + static_cast<std::size_t>(offset)),
        static_cast<std::size_t>(length),
    };
}

status compiled_image_view::find_string(
    std::string_view value,
    string_id& output) const noexcept {

    output = {};
    if (!valid() || value.empty())
        return {status_code::not_found};

    const auto& index = section(compiled_image_section::string_index);
    const auto hash = string_hash(value);
    const auto fingerprint = fold32(hash);
    const auto mask = static_cast<std::size_t>(index.count - 1);
    auto position = static_cast<std::size_t>(hash) & mask;

    for (std::size_t probe = 0; probe < index.count; ++probe) {
        const auto* slot = index.data + position * index_record_size;
        const auto raw_id = read_u32(slot + 4);
        if (raw_id == 0)
            return {status_code::not_found};

        if (read_u32(slot) == fingerprint) {
            const string_id candidate{raw_id};
            if (string(candidate) == value) {
                output = candidate;
                return {};
            }
        }

        position = (position + 1) & mask;
    }

    return {status_code::not_found};
}

std::size_t compiled_image_view::identity_slot_count() const noexcept {
    return static_cast<std::size_t>(
        section(compiled_image_section::identity_core).count);
}

identity_ref compiled_image_view::identity_from_raw(
    std::uint32_t value) const noexcept {

    if (value == 0)
        return {};

    const auto slot = value & identity_ref::slot_mask;
    const auto kind = static_cast<identity_kind>(
        value >> identity_ref::kind_shift);

    if (slot == 0 ||
        !valid_identity_kind(static_cast<std::uint8_t>(kind))) {
        return {};
    }

    return identity_ref::make(slot, kind);
}

type_handle compiled_image_view::type_from_raw(
    std::uint32_t value) const noexcept {

    return value == 0 ? type_handle{} : type_handle{value};
}

object_handle compiled_image_view::object_from_raw(
    std::uint32_t value) const noexcept {

    return value == 0 ? object_handle{} : object_handle{value};
}

link_handle compiled_image_view::link_from_raw(
    std::uint32_t value) const noexcept {

    return value == 0 ? link_handle{} : link_handle{value};
}

TypeRef compiled_image_view::type_ref_from_raw(
    std::uint32_t value) const noexcept {

    return value == 0 ? TypeRef{} : TypeRef{value};
}

identity_ref compiled_image_view::identity_root() const noexcept {
    if (!valid() || identity_slot_count() == 0)
        return {};

    const auto& core = section(compiled_image_section::identity_core);
    return identity_from_raw(read_u32(core.data));
}

bool compiled_image_view::identity_valid(
    identity_ref identity) const noexcept {

    if (!valid() || !identity ||
        identity.slot() == 0 ||
        identity.slot() > identity_slot_count()) {
        return false;
    }

    const auto& core = section(compiled_image_section::identity_core);
    const auto* record =
        core.data +
        static_cast<std::size_t>(identity.slot() - 1) *
            identity_core_size;

    return read_u32(record) == identity.value();
}

identity_ref compiled_image_view::identity_parent(
    identity_ref identity) const noexcept {

    if (!identity_valid(identity))
        return {};

    const auto& core = section(compiled_image_section::identity_core);
    const auto* record =
        core.data +
        static_cast<std::size_t>(identity.slot() - 1) *
            identity_core_size;

    return identity_from_raw(read_u32(record + 4));
}

string_id compiled_image_view::identity_name(
    identity_ref identity) const noexcept {

    if (!identity_valid(identity))
        return {};

    const auto& core = section(compiled_image_section::identity_core);
    const auto* record =
        core.data +
        static_cast<std::size_t>(identity.slot() - 1) *
            identity_core_size;

    const auto raw = read_u32(record + 8);
    return raw == 0 ? string_id{} : string_id{raw};
}

status compiled_image_view::find_identity(
    identity_ref parent,
    string_id name,
    identity_kind kind,
    identity_ref& output) const noexcept {

    output = {};

    if (!valid() ||
        !identity_valid(parent) ||
        !name ||
        kind == identity_kind::root) {
        return {status_code::not_found};
    }

    const auto& index = section(compiled_image_section::identity_index);
    const auto hash = semantic_identity_hash(parent.value(), name.value());
    const auto fingerprint = fold32(hash);
    const auto mask = static_cast<std::size_t>(index.count - 1);
    auto position = static_cast<std::size_t>(hash) & mask;

    for (std::size_t probe = 0; probe < index.count; ++probe) {
        const auto* slot = index.data + position * index_record_size;
        const auto raw_identity = read_u32(slot + 4);
        if (raw_identity == 0)
            return {status_code::not_found};

        if (read_u32(slot) == fingerprint) {
            const auto candidate = identity_from_raw(raw_identity);
            if (candidate &&
                candidate.kind() == kind &&
                identity_parent(candidate) == parent &&
                identity_name(candidate) == name) {
                output = candidate;
                return {};
            }
        }

        position = (position + 1) & mask;
    }

    return {status_code::not_found};
}

std::size_t compiled_image_view::type_slot_count() const noexcept {
    return static_cast<std::size_t>(
        section(compiled_image_section::types).count);
}

bool compiled_image_view::read_type_raw(
    std::uint32_t handle,
    compiled_image_type_record& output) const noexcept {

    output = {};
    const auto& values = section(compiled_image_section::types);
    if (!valid() || handle == 0 || handle > values.count)
        return false;

    const auto* record =
        values.data +
        static_cast<std::size_t>(handle - 1) * type_record_size;

    const auto raw_kind = static_cast<std::uint8_t>(record[8]);
    const auto raw_record_kind = static_cast<std::uint8_t>(record[9]);
    const auto raw_underlying = static_cast<std::uint8_t>(record[10]);

    if (!valid_graph_type_kind(raw_kind) ||
        !valid_record_kind(raw_record_kind) ||
        !valid_intrinsic(raw_underlying)) {
        return false;
    }

    output.definition.begin = read_u32(record);
    output.definition.count = read_u32(record + 4);
    output.kind = static_cast<graph_type_kind>(raw_kind);
    output.record_kind = static_cast<source_record_kind>(raw_record_kind);
    output.enum_underlying = static_cast<intrinsic_type>(raw_underlying);
    output.flags = static_cast<std::uint8_t>(record[11]);
    return true;
}

type_handle compiled_image_view::type_at(std::size_t index) const noexcept {
    if (index >= type_slot_count() ||
        index >= (std::numeric_limits<std::uint32_t>::max)()) {
        return {};
    }

    compiled_image_type_record value;
    if (!read_type_raw(static_cast<std::uint32_t>(index + 1), value) ||
        !value.live()) {
        return {};
    }

    return type_from_raw(static_cast<std::uint32_t>(index + 1));
}

status compiled_image_view::type(
    type_handle handle,
    compiled_image_type_record& output) const noexcept {

    if (!handle || !read_type_raw(handle.value(), output) || !output.live()) {
        output = {};
        return {status_code::not_found};
    }
    return {};
}

identity_ref compiled_image_view::identity(
    type_handle handle) const noexcept {

    compiled_image_type_record type_value;
    if (!handle ||
        !read_type_raw(handle.value(), type_value) ||
        !type_value.live()) {
        return {};
    }

    const auto& values =
        section(compiled_image_section::type_identities);
    if (handle.value() > values.count)
        return {};

    return identity_from_raw(read_u32(
        values.data +
        static_cast<std::size_t>(handle.value() - 1) * 4));
}

type_handle compiled_image_view::find_type(
    identity_ref identity_value) const noexcept {

    if (!identity_valid(identity_value))
        return {};

    const auto& index = section(compiled_image_section::graph_type_index);
    const auto hash = graph_identity_hash(identity_value.value());
    const auto fingerprint = fold32(hash);
    const auto mask = static_cast<std::size_t>(index.count - 1);
    auto position = static_cast<std::size_t>(hash) & mask;

    for (std::size_t probe = 0; probe < index.count; ++probe) {
        const auto* slot = index.data + position * index_record_size;
        const auto raw_handle = read_u32(slot + 4);
        if (raw_handle == 0)
            return {};

        if (read_u32(slot) == fingerprint) {
            const auto handle = type_from_raw(raw_handle);
            if (identity(handle) == identity_value)
                return handle;
        }

        position = (position + 1) & mask;
    }

    return {};
}

std::size_t compiled_image_view::member_count(
    type_handle handle) const noexcept {

    compiled_image_type_record value;
    if (!handle ||
        !read_type_raw(handle.value(), value) ||
        !value.live() ||
        value.kind != graph_type_kind::record ||
        !value.definition) {
        return 0;
    }

    return static_cast<std::size_t>(value.definition.count);
}

status compiled_image_view::member(
    type_handle handle,
    member_index index,
    compiled_image_member_record& output) const noexcept {

    output = {};
    if (!index)
        return {status_code::not_found};

    compiled_image_type_record type_value;
    if (!read_type_raw(handle.value(), type_value) ||
        !type_value.live() ||
        type_value.kind != graph_type_kind::record ||
        !type_value.definition ||
        index.value() >= type_value.definition.count) {
        return {status_code::not_found};
    }

    const auto& values = section(compiled_image_section::members);
    const auto begin =
        static_cast<std::uint64_t>(type_value.definition.begin - 1);
    const auto absolute = begin + index.value();
    if (absolute >= values.count)
        return {status_code::artifact_corrupt};

    const auto* record =
        values.data +
        static_cast<std::size_t>(absolute) * member_record_size;

    const auto access = static_cast<std::uint8_t>(record[8]);
    if (!valid_member_access(access))
        return {status_code::artifact_corrupt};

    const auto raw_name = read_u32(record);
    const auto raw_type = read_u32(record + 4);
    if (raw_name == 0 || raw_type == 0)
        return {status_code::artifact_corrupt};

    output.name = string_id{raw_name};
    output.type = type_ref_from_raw(raw_type);
    output.access = static_cast<source_member_access>(access);
    return {};
}

member_index compiled_image_view::find_member(
    type_handle handle,
    string_id name) const noexcept {

    if (!name)
        return {};

    const auto count = member_count(handle);
    for (std::size_t index = 0; index < count; ++index) {
        if (index > (std::numeric_limits<std::uint32_t>::max)())
            return {};

        compiled_image_member_record value;
        const auto local =
            member_index::from_zero_based(static_cast<std::uint32_t>(index));
        if (!member(handle, local, value).ok())
            return {};
        if (value.name == name)
            return local;
    }

    return {};
}

std::size_t compiled_image_view::enum_value_count(
    type_handle handle) const noexcept {

    compiled_image_type_record value;
    if (!handle ||
        !read_type_raw(handle.value(), value) ||
        !value.live() ||
        value.kind != graph_type_kind::enumeration ||
        !value.definition) {
        return 0;
    }

    return static_cast<std::size_t>(value.definition.count);
}

status compiled_image_view::enum_value(
    type_handle handle,
    std::size_t index,
    compiled_image_enum_value_record& output) const noexcept {

    output = {};

    compiled_image_type_record type_value;
    if (!read_type_raw(handle.value(), type_value) ||
        !type_value.live() ||
        type_value.kind != graph_type_kind::enumeration ||
        !type_value.definition ||
        index >= type_value.definition.count) {
        return {status_code::not_found};
    }

    const auto& values = section(compiled_image_section::enum_values);
    const auto begin =
        static_cast<std::uint64_t>(type_value.definition.begin - 1);
    const auto absolute = begin + index;
    if (absolute >= values.count)
        return {status_code::artifact_corrupt};

    const auto* record =
        values.data +
        static_cast<std::size_t>(absolute) * enum_value_record_size;

    const auto intrinsic = static_cast<std::uint8_t>(record[12]);
    if (!valid_intrinsic(intrinsic) ||
        !zero_bytes(record + 13, 3)) {
        return {status_code::artifact_corrupt};
    }

    const auto raw_name = read_u32(record + 8);
    if (raw_name == 0)
        return {status_code::artifact_corrupt};

    output.bits = read_u64(record);
    output.name = string_id{raw_name};
    output.intrinsic = static_cast<intrinsic_type>(intrinsic);
    return {};
}

std::size_t compiled_image_view::object_slot_count() const noexcept {
    return static_cast<std::size_t>(
        section(compiled_image_section::objects).count);
}

bool compiled_image_view::read_object_raw(
    std::uint32_t handle,
    compiled_image_object_record& output) const noexcept {

    output = {};
    const auto& values = section(compiled_image_section::objects);
    if (!valid() || handle == 0 || handle > values.count)
        return false;

    const auto* record =
        values.data +
        static_cast<std::size_t>(handle - 1) * object_record_size;

    output.type = type_ref_from_raw(read_u32(record));
    output.flags = read_u32(record + 4);
    return true;
}

object_handle compiled_image_view::object_at(
    std::size_t index) const noexcept {

    if (index >= object_slot_count() ||
        index >= (std::numeric_limits<std::uint32_t>::max)()) {
        return {};
    }

    compiled_image_object_record value;
    if (!read_object_raw(static_cast<std::uint32_t>(index + 1), value) ||
        !value.live()) {
        return {};
    }

    return object_from_raw(static_cast<std::uint32_t>(index + 1));
}

status compiled_image_view::object(
    object_handle handle,
    compiled_image_object_record& output) const noexcept {

    if (!handle ||
        !read_object_raw(handle.value(), output) ||
        !output.live()) {
        output = {};
        return {status_code::not_found};
    }

    return {};
}

identity_ref compiled_image_view::identity(
    object_handle handle) const noexcept {

    compiled_image_object_record object_value;
    if (!handle ||
        !read_object_raw(handle.value(), object_value) ||
        !object_value.live()) {
        return {};
    }

    const auto& values =
        section(compiled_image_section::object_identities);
    if (handle.value() > values.count)
        return {};

    return identity_from_raw(read_u32(
        values.data +
        static_cast<std::size_t>(handle.value() - 1) * 4));
}

object_handle compiled_image_view::find_object(
    identity_ref identity_value) const noexcept {

    if (!identity_valid(identity_value))
        return {};

    const auto& index =
        section(compiled_image_section::graph_object_index);
    const auto hash = graph_identity_hash(identity_value.value());
    const auto fingerprint = fold32(hash);
    const auto mask = static_cast<std::size_t>(index.count - 1);
    auto position = static_cast<std::size_t>(hash) & mask;

    for (std::size_t probe = 0; probe < index.count; ++probe) {
        const auto* slot = index.data + position * index_record_size;
        const auto raw_handle = read_u32(slot + 4);
        if (raw_handle == 0)
            return {};

        if (read_u32(slot) == fingerprint) {
            const auto handle = object_from_raw(raw_handle);
            if (identity(handle) == identity_value)
                return handle;
        }

        position = (position + 1) & mask;
    }

    return {};
}

std::size_t compiled_image_view::link_slot_count() const noexcept {
    return static_cast<std::size_t>(
        section(compiled_image_section::links).count);
}

bool compiled_image_view::read_link_raw(
    std::uint32_t handle,
    compiled_image_link_record& output) const noexcept {

    output = {};
    const auto& values = section(compiled_image_section::links);
    if (!valid() || handle == 0 || handle > values.count)
        return false;

    const auto* record =
        values.data +
        static_cast<std::size_t>(handle - 1) * link_record_size;

    output.source.object = object_from_raw(read_u32(record));
    output.source.member =
        member_index::from_zero_based(read_u32(record + 4));
    output.target.object = object_from_raw(read_u32(record + 8));
    output.target.member =
        member_index::from_zero_based(read_u32(record + 12));
    return true;
}

status compiled_image_view::link(
    link_handle handle,
    compiled_image_link_record& output) const noexcept {

    if (!handle ||
        !read_link_raw(handle.value(), output) ||
        !output.live()) {
        output = {};
        return {status_code::not_found};
    }

    return {};
}

link_handle compiled_image_view::find_link(
    object_endpoint target) const noexcept {

    if (!target.object || !target.member)
        return {};

    const auto& index =
        section(compiled_image_section::graph_link_index);
    const auto hash =
        endpoint_hash(target.object.value(), target.member.value());
    const auto fingerprint = fold32(hash);
    const auto mask = static_cast<std::size_t>(index.count - 1);
    auto position = static_cast<std::size_t>(hash) & mask;

    for (std::size_t probe = 0; probe < index.count; ++probe) {
        const auto* slot = index.data + position * index_record_size;
        const auto raw_handle = read_u32(slot + 4);
        if (raw_handle == 0)
            return {};

        if (read_u32(slot) == fingerprint) {
            const auto handle = link_from_raw(raw_handle);
            compiled_image_link_record value;
            if (link(handle, value).ok() && value.target == target)
                return handle;
        }

        position = (position + 1) & mask;
    }

    return {};
}

std::size_t compiled_image_view::canonical_type_slot_count() const noexcept {
    return static_cast<std::size_t>(
        section(compiled_image_section::canonical_types).count);
}

status compiled_image_view::canonical_type(
    TypeRef type,
    compiled_image_canonical_type_record& output) const noexcept {

    output = {};
    const auto& values =
        section(compiled_image_section::canonical_types);

    if (!type || type.value() >= values.count)
        return {status_code::not_found};

    const auto* record =
        values.data +
        static_cast<std::size_t>(type.value()) *
            canonical_type_record_size;

    const auto kind = static_cast<std::uint8_t>(record[12]);
    if (!valid_canonical_kind(kind) ||
        read_u16(record + 14) != 0) {
        return {status_code::artifact_corrupt};
    }

    output.payload = read_u64(record);
    output.child_or_handle = read_u32(record + 8);
    output.kind = static_cast<canonical_type_kind>(kind);
    output.detail = static_cast<std::uint8_t>(record[13]);
    return {};
}

bool compiled_image_view::intrinsic(
    TypeRef type,
    intrinsic_type& output) const noexcept {

    output = intrinsic_type::none;

    compiled_image_canonical_type_record record;
    if (!canonical_type(type, record).ok() ||
        record.kind != canonical_type_kind::intrinsic ||
        !valid_intrinsic(record.detail)) {
        return false;
    }

    output = static_cast<intrinsic_type>(record.detail);
    return true;
}

bool compiled_image_view::named(
    TypeRef type,
    type_handle& output) const noexcept {

    output = {};

    compiled_image_canonical_type_record record;
    if (!canonical_type(type, record).ok() ||
        record.kind != canonical_type_kind::named ||
        record.child_or_handle == 0 ||
        record.child_or_handle > type_slot_count()) {
        return false;
    }

    const auto handle = type_from_raw(record.child_or_handle);
    compiled_image_type_record type_value;
    if (!this->type(handle, type_value).ok())
        return false;

    output = handle;
    return true;
}

bool compiled_image_view::derived(
    TypeRef type,
    derived_type_record& output) const noexcept {

    output = {};

    compiled_image_canonical_type_record record;
    if (!canonical_type(type, record).ok() ||
        record.kind != canonical_type_kind::derived ||
        !valid_derived_kind(record.detail) ||
        record.child_or_handle == 0 ||
        record.child_or_handle >= canonical_type_slot_count()) {
        return false;
    }

    output.payload = record.payload;
    output.child = type_ref_from_raw(record.child_or_handle);
    output.kind = static_cast<derived_type_kind>(record.detail);
    return true;
}

status compiled_image_view::verify_contents() const noexcept {
    if (!valid())
        return {status_code::invalid_state};

    for (const auto& value : sections) {
        std::uint64_t byte_count = 0;
        if (!multiply_u64(value.count, value.record_size, byte_count) ||
            byte_count > (std::numeric_limits<std::size_t>::max)()) {
            return {status_code::artifact_corrupt};
        }

        if (crc64(std::span<const std::byte>{
                value.data,
                static_cast<std::size_t>(byte_count)}) != value.crc64) {
            return {status_code::artifact_corrupt};
        }
    }

    const auto& string_core =
        section(compiled_image_section::string_core);
    std::size_t observed_strings = 0;

    for (std::size_t index = 0; index < string_core.count; ++index) {
        const auto* record = string_core.data + index * string_core_size;
        const auto offset = read_u64(record);
        const auto length = read_u32(record + 8);
        const auto reserved = read_u32(record + 12);

        if (reserved != 0)
            return {status_code::artifact_corrupt};

        if (length == 0) {
            if (offset != 0)
                return {status_code::artifact_corrupt};
            continue;
        }

        ++observed_strings;
        if (index >= (std::numeric_limits<std::uint32_t>::max)())
            return {status_code::artifact_corrupt};

        const string_id id{
            static_cast<std::uint32_t>(index + 1)};
        const auto value = string(id);
        if (value.empty())
            return {status_code::artifact_corrupt};

        string_id found;
        if (!find_string(value, found).ok() || found != id)
            return {status_code::artifact_corrupt};
    }

    if (observed_strings != string_live_count)
        return {status_code::artifact_corrupt};

    const auto& string_index =
        section(compiled_image_section::string_index);
    std::size_t indexed_strings = 0;
    for (std::size_t index = 0; index < string_index.count; ++index) {
        const auto* slot = string_index.data + index * index_record_size;
        const auto fingerprint = read_u32(slot);
        const auto raw_id = read_u32(slot + 4);

        if (raw_id == 0) {
            if (fingerprint != 0)
                return {status_code::artifact_corrupt};
            continue;
        }

        ++indexed_strings;
        if (fingerprint == 0 || raw_id > string_core.count)
            return {status_code::artifact_corrupt};

        const string_id id{raw_id};
        const auto value = string(id);
        if (value.empty() ||
            fold32(string_hash(value)) != fingerprint) {
            return {status_code::artifact_corrupt};
        }

        string_id found;
        if (!find_string(value, found).ok() || found != id)
            return {status_code::artifact_corrupt};
    }

    if (indexed_strings != string_live_count)
        return {status_code::artifact_corrupt};

    const auto root = identity_root();
    if (!root ||
        root.slot() != 1 ||
        root.kind() != identity_kind::root ||
        identity_parent(root) ||
        identity_name(root)) {
        return {status_code::artifact_corrupt};
    }

    const auto& identity_core =
        section(compiled_image_section::identity_core);
    std::size_t observed_identities = 1;

    for (std::size_t index = 1; index < identity_core.count; ++index) {
        const auto* record =
            identity_core.data + index * identity_core_size;

        const auto raw = read_u32(record);
        const auto raw_parent = read_u32(record + 4);
        const auto raw_name = read_u32(record + 8);

        if (raw == 0) {
            if (raw_parent != 0 || raw_name != 0)
                return {status_code::artifact_corrupt};
            continue;
        }

        const auto identity = identity_from_raw(raw);
        if (!identity ||
            identity.slot() != index + 1 ||
            identity.kind() == identity_kind::root ||
            raw_parent == 0 ||
            raw_name == 0 ||
            !identity_valid(identity)) {
            return {status_code::artifact_corrupt};
        }

        const auto parent = identity_parent(identity);
        const auto name = identity_name(identity);
        if (!parent ||
            !identity_valid(parent) ||
            !name ||
            string(name).empty()) {
            return {status_code::artifact_corrupt};
        }

        identity_ref found;
        if (!find_identity(
                parent, name, identity.kind(), found).ok() ||
            found != identity) {
            return {status_code::artifact_corrupt};
        }

        ++observed_identities;
    }

    if (observed_identities != identity_live_count)
        return {status_code::artifact_corrupt};

    const auto& identity_index =
        section(compiled_image_section::identity_index);
    std::size_t indexed_identities = 0;
    for (std::size_t index = 0; index < identity_index.count; ++index) {
        const auto* slot = identity_index.data + index * index_record_size;
        const auto fingerprint = read_u32(slot);
        const auto raw_identity = read_u32(slot + 4);

        if (raw_identity == 0) {
            if (fingerprint != 0)
                return {status_code::artifact_corrupt};
            continue;
        }

        ++indexed_identities;
        const auto identity = identity_from_raw(raw_identity);
        if (fingerprint == 0 ||
            !identity ||
            identity.kind() == identity_kind::root ||
            !identity_valid(identity)) {
            return {status_code::artifact_corrupt};
        }

        const auto parent = identity_parent(identity);
        const auto name = identity_name(identity);
        const auto expected =
            fold32(semantic_identity_hash(parent.value(), name.value()));
        if (fingerprint != expected)
            return {status_code::artifact_corrupt};

        identity_ref found;
        if (!find_identity(
                parent,
                name,
                identity.kind(),
                found).ok() ||
            found != identity) {
            return {status_code::artifact_corrupt};
        }
    }

    if (indexed_identities + 1 != identity_live_count)
        return {status_code::artifact_corrupt};

    const auto& type_values = section(compiled_image_section::types);
    const auto& type_identities =
        section(compiled_image_section::type_identities);
    std::size_t observed_types = 0;

    for (std::size_t index = 0; index < type_values.count; ++index) {
        if (index >= (std::numeric_limits<std::uint32_t>::max)())
            return {status_code::artifact_corrupt};

        const auto handle =
            type_from_raw(static_cast<std::uint32_t>(index + 1));

        compiled_image_type_record value;
        if (!read_type_raw(handle.value(), value))
            return {status_code::artifact_corrupt};

        const auto identity_value = identity_from_raw(read_u32(
            type_identities.data + index * 4));
        if (!identity_value ||
            identity_value.kind() != identity_kind::type ||
            !identity_valid(identity_value)) {
            return {status_code::artifact_corrupt};
        }

        if (value.definition) {
            const auto begin =
                static_cast<std::uint64_t>(value.definition.begin - 1);
            const auto count =
                static_cast<std::uint64_t>(value.definition.count);
            const auto limit =
                value.kind == graph_type_kind::record
                    ? section(compiled_image_section::members).count
                    : section(compiled_image_section::enum_values).count;

            if (begin > limit || count > limit - begin)
                return {status_code::artifact_corrupt};
        }

        if (value.live()) {
            ++observed_types;
            if (find_type(identity_value) != handle)
                return {status_code::artifact_corrupt};
        }
    }

    if (observed_types != live_type_count)
        return {status_code::artifact_corrupt};

    const auto& graph_type_index =
        section(compiled_image_section::graph_type_index);
    std::size_t indexed_types = 0;
    for (std::size_t index = 0; index < graph_type_index.count; ++index) {
        const auto* slot = graph_type_index.data + index * index_record_size;
        const auto fingerprint = read_u32(slot);
        const auto raw_handle = read_u32(slot + 4);

        if (raw_handle == 0) {
            if (fingerprint != 0)
                return {status_code::artifact_corrupt};
            continue;
        }

        ++indexed_types;
        const auto handle = type_from_raw(raw_handle);
        const auto identity_value = identity(handle);
        if (!identity_value ||
            fingerprint != fold32(
                graph_identity_hash(identity_value.value())) ||
            find_type(identity_value) != handle) {
            return {status_code::artifact_corrupt};
        }
    }

    if (indexed_types != live_type_count)
        return {status_code::artifact_corrupt};

    const auto& member_values = section(compiled_image_section::members);
    for (std::size_t index = 0; index < member_values.count; ++index) {
        const auto* record =
            member_values.data + index * member_record_size;

        const auto name = read_u32(record);
        const auto type = read_u32(record + 4);
        const auto access = static_cast<std::uint8_t>(record[8]);

        if (name == 0 ||
            string(string_id{name}).empty() ||
            type == 0 ||
            type >= canonical_type_slot_count() ||
            !valid_member_access(access) ||
            !zero_bytes(record + 9, 3)) {
            return {status_code::artifact_corrupt};
        }
    }

    const auto& enum_values = section(compiled_image_section::enum_values);
    for (std::size_t index = 0; index < enum_values.count; ++index) {
        const auto* record =
            enum_values.data + index * enum_value_record_size;

        const auto name = read_u32(record + 8);
        const auto intrinsic = static_cast<std::uint8_t>(record[12]);

        if (name == 0 ||
            string(string_id{name}).empty() ||
            !valid_intrinsic(intrinsic) ||
            !zero_bytes(record + 13, 3)) {
            return {status_code::artifact_corrupt};
        }
    }

    const auto& object_values = section(compiled_image_section::objects);
    const auto& object_identities =
        section(compiled_image_section::object_identities);
    std::size_t observed_objects = 0;

    for (std::size_t index = 0; index < object_values.count; ++index) {
        if (index >= (std::numeric_limits<std::uint32_t>::max)())
            return {status_code::artifact_corrupt};

        const auto handle =
            object_from_raw(static_cast<std::uint32_t>(index + 1));

        compiled_image_object_record value;
        if (!read_object_raw(handle.value(), value))
            return {status_code::artifact_corrupt};

        const auto identity_value = identity_from_raw(read_u32(
            object_identities.data + index * 4));
        if (!identity_value ||
            identity_value.kind() != identity_kind::object ||
            !identity_valid(identity_value) ||
            (value.type &&
             value.type.value() >= canonical_type_slot_count())) {
            return {status_code::artifact_corrupt};
        }

        if (value.live()) {
            ++observed_objects;
            if (!value.type ||
                find_object(identity_value) != handle) {
                return {status_code::artifact_corrupt};
            }
        }
    }

    if (observed_objects != live_object_count)
        return {status_code::artifact_corrupt};

    const auto& graph_object_index =
        section(compiled_image_section::graph_object_index);
    std::size_t indexed_objects = 0;
    for (std::size_t index = 0;
         index < graph_object_index.count;
         ++index) {

        const auto* slot =
            graph_object_index.data + index * index_record_size;
        const auto fingerprint = read_u32(slot);
        const auto raw_handle = read_u32(slot + 4);

        if (raw_handle == 0) {
            if (fingerprint != 0)
                return {status_code::artifact_corrupt};
            continue;
        }

        ++indexed_objects;
        const auto handle = object_from_raw(raw_handle);
        const auto identity_value = identity(handle);
        if (!identity_value ||
            fingerprint != fold32(
                graph_identity_hash(identity_value.value())) ||
            find_object(identity_value) != handle) {
            return {status_code::artifact_corrupt};
        }
    }

    if (indexed_objects != live_object_count)
        return {status_code::artifact_corrupt};

    const auto& canonical_values =
        section(compiled_image_section::canonical_types);

    if (canonical_values.count != 0) {
        const auto* zero = canonical_values.data;
        if (read_u64(zero) != 0 ||
            read_u32(zero + 8) != 0 ||
            static_cast<std::uint8_t>(zero[12]) !=
                static_cast<std::uint8_t>(canonical_type_kind::intrinsic) ||
            static_cast<std::uint8_t>(zero[13]) != 0 ||
            read_u16(zero + 14) != 0) {
            return {status_code::artifact_corrupt};
        }
    }

    for (std::size_t index = 1;
         index < canonical_values.count;
         ++index) {

        if (index >= (std::numeric_limits<std::uint32_t>::max)())
            return {status_code::artifact_corrupt};

        const TypeRef ref{static_cast<std::uint32_t>(index)};
        compiled_image_canonical_type_record value;
        if (!canonical_type(ref, value).ok())
            return {status_code::artifact_corrupt};

        switch (value.kind) {
        case canonical_type_kind::intrinsic:
            if (!valid_intrinsic(value.detail))
                return {status_code::artifact_corrupt};
            break;
        case canonical_type_kind::named:
            if (value.child_or_handle == 0 ||
                value.child_or_handle > type_slot_count())
                return {status_code::artifact_corrupt};
            break;
        case canonical_type_kind::derived:
            if (!valid_derived_kind(value.detail) ||
                value.child_or_handle == 0 ||
                value.child_or_handle >= canonical_values.count) {
                return {status_code::artifact_corrupt};
            }
            break;
        }
    }

    const auto& links = section(compiled_image_section::links);
    std::size_t observed_links = 0;

    for (std::size_t index = 0; index < links.count; ++index) {
        if (index >= (std::numeric_limits<std::uint32_t>::max)())
            return {status_code::artifact_corrupt};

        const auto handle =
            link_from_raw(static_cast<std::uint32_t>(index + 1));

        compiled_image_link_record value;
        if (!read_link_raw(handle.value(), value))
            return {status_code::artifact_corrupt};

        if (!value.live())
            continue;

        ++observed_links;

        compiled_image_object_record source_object;
        compiled_image_object_record target_object;
        if (!object(value.source.object, source_object).ok() ||
            !object(value.target.object, target_object).ok()) {
            return {status_code::artifact_corrupt};
        }

        type_handle source_type;
        type_handle target_type;
        if (!named(source_object.type, source_type) ||
            !named(target_object.type, target_type) ||
            value.source.member.value() >= member_count(source_type) ||
            value.target.member.value() >= member_count(target_type) ||
            find_link(value.target) != handle) {
            return {status_code::artifact_corrupt};
        }
    }

    if (observed_links != live_link_count)
        return {status_code::artifact_corrupt};

    const auto& graph_link_index =
        section(compiled_image_section::graph_link_index);
    std::size_t indexed_links = 0;
    for (std::size_t index = 0;
         index < graph_link_index.count;
         ++index) {

        const auto* slot =
            graph_link_index.data + index * index_record_size;
        const auto fingerprint = read_u32(slot);
        const auto raw_handle = read_u32(slot + 4);

        if (raw_handle == 0) {
            if (fingerprint != 0)
                return {status_code::artifact_corrupt};
            continue;
        }

        ++indexed_links;
        const auto handle = link_from_raw(raw_handle);
        compiled_image_link_record value;
        if (!link(handle, value).ok() ||
            fingerprint != fold32(endpoint_hash(
                value.target.object.value(),
                value.target.member.value())) ||
            find_link(value.target) != handle) {
            return {status_code::artifact_corrupt};
        }
    }

    if (indexed_links != live_link_count)
        return {status_code::artifact_corrupt};

    return {};
}

status encode_compiled_image(
    const project_context& project,
    std::vector<std::byte>& output) noexcept {

    output.clear();

    const auto string_slots = project.string_slot_count();
    std::uint64_t string_bytes_count = 0;
    std::size_t observed_string_count = 0;

    for (std::size_t index = 0; index < string_slots; ++index) {
        const auto id = project.string_at_slot(index);
        if (!id)
            continue;

        const auto value = project.string(id);
        if (value.empty() ||
            value.size() >
                (std::numeric_limits<std::uint32_t>::max)() ||
            !add_u64(
                string_bytes_count,
                value.size(),
                string_bytes_count)) {
            return {status_code::initialization_failed};
        }

        ++observed_string_count;
    }

    if (observed_string_count != project.string_count())
        return {status_code::initialization_failed};

    const auto identity_slots = project.identity_slot_count();
    const auto identity_metadata = project.identity_metadata();
    std::size_t observed_identity_count = 0;

    for (std::size_t index = 0; index < identity_slots; ++index) {
        const auto identity = project.identity_at_slot(index);
        if (!identity)
            continue;

        if (index == 0) {
            if (identity.kind() != identity_kind::root ||
                identity.slot() != 1 ||
                identity_metadata.parent(identity) ||
                identity_metadata.name(identity)) {
                return {status_code::initialization_failed};
            }
        } else {
            const auto parent = identity_metadata.parent(identity);
            const auto name = identity_metadata.name(identity);
            if (identity.slot() != index + 1 ||
                identity.kind() == identity_kind::root ||
                !parent ||
                !identity_metadata.valid(parent) ||
                !name ||
                project.string(name).empty()) {
                return {status_code::initialization_failed};
            }
        }

        ++observed_identity_count;
    }

    if (observed_identity_count != project.identity_count())
        return {status_code::initialization_failed};

    const auto graph = project.compiled_graph().data_view();

    if (graph.type_identities.size() != graph.types.size() ||
        graph.object_identities.size() != graph.objects.size()) {
        return {status_code::initialization_failed};
    }

    const auto string_index_count =
        index_capacity(observed_string_count);
    const auto identity_index_count =
        index_capacity(
            observed_identity_count > 0
                ? observed_identity_count - 1
                : 0);
    const auto graph_type_index_count =
        index_capacity(graph.live_types);
    const auto graph_object_index_count =
        index_capacity(graph.live_objects);
    const auto graph_link_index_count =
        index_capacity(graph.live_links);

    if (string_index_count == 0 ||
        identity_index_count == 0 ||
        graph_type_index_count == 0 ||
        graph_object_index_count == 0 ||
        graph_link_index_count == 0) {
        return {status_code::not_available};
    }

    std::array<layout_section, compiled_image_directory_count> layout{{
        {compiled_image_section::string_core,
            string_core_size, string_slots},
        {compiled_image_section::string_index,
            index_record_size, string_index_count},
        {compiled_image_section::string_bytes,
            1, string_bytes_count},
        {compiled_image_section::identity_core,
            identity_core_size, identity_slots},
        {compiled_image_section::identity_index,
            index_record_size, identity_index_count},
        {compiled_image_section::types,
            type_record_size, graph.types.size()},
        {compiled_image_section::type_identities,
            4, graph.type_identities.size()},
        {compiled_image_section::members,
            member_record_size, graph.members.size()},
        {compiled_image_section::enum_values,
            enum_value_record_size, graph.enum_values.size()},
        {compiled_image_section::objects,
            object_record_size, graph.objects.size()},
        {compiled_image_section::object_identities,
            4, graph.object_identities.size()},
        {compiled_image_section::links,
            link_record_size, graph.links.size()},
        {compiled_image_section::canonical_types,
            canonical_type_record_size, graph.canonical_types.size()},
        {compiled_image_section::graph_type_index,
            index_record_size, graph_type_index_count},
        {compiled_image_section::graph_object_index,
            index_record_size, graph_object_index_count},
        {compiled_image_section::graph_link_index,
            index_record_size, graph_link_index_count},
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

    try {
        output.assign(
            static_cast<std::size_t>(cursor),
            std::byte{0});
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }

    auto* base = output.data();

    const auto section_data = [&](compiled_image_section kind) noexcept {
        return base + static_cast<std::size_t>(
            layout[section_index(kind)].offset);
    };

    auto* string_core =
        section_data(compiled_image_section::string_core);
    auto* string_index =
        section_data(compiled_image_section::string_index);
    auto* string_bytes =
        section_data(compiled_image_section::string_bytes);

    std::uint64_t string_offset = 0;
    const auto string_mask = string_index_count - 1;

    for (std::size_t index = 0; index < string_slots; ++index) {
        const auto id = project.string_at_slot(index);
        if (!id)
            continue;

        const auto value = project.string(id);
        const auto hash = string_hash(value);

        auto* core = string_core + index * string_core_size;
        write_u64(core, string_offset);
        write_u32(
            core + 8,
            static_cast<std::uint32_t>(value.size()));
        write_u32(core + 12, 0);

        std::memcpy(
            string_bytes + static_cast<std::size_t>(string_offset),
            value.data(),
            value.size());

        const auto fingerprint = fold32(hash);
        auto position =
            static_cast<std::size_t>(hash) & string_mask;

        for (;;) {
            auto* slot =
                string_index + position * index_record_size;
            if (read_u32(slot + 4) == 0) {
                write_u32(slot, fingerprint);
                write_u32(slot + 4, id.value());
                break;
            }
            position = (position + 1) & string_mask;
        }

        string_offset += value.size();
    }

    auto* identity_core =
        section_data(compiled_image_section::identity_core);
    auto* identity_index =
        section_data(compiled_image_section::identity_index);
    const auto identity_mask = identity_index_count - 1;

    for (std::size_t index = 0; index < identity_slots; ++index) {
        const auto identity = project.identity_at_slot(index);
        if (!identity)
            continue;

        auto* core = identity_core + index * identity_core_size;
        write_u32(core, identity.value());

        if (identity.kind() == identity_kind::root) {
            write_u32(core + 4, 0);
            write_u32(core + 8, 0);
            continue;
        }

        const auto parent = identity_metadata.parent(identity);
        const auto name = identity_metadata.name(identity);
        write_u32(core + 4, parent.value());
        write_u32(core + 8, name.value());

        const auto hash =
            semantic_identity_hash(parent.value(), name.value());
        const auto fingerprint = fold32(hash);
        auto position =
            static_cast<std::size_t>(hash) & identity_mask;

        for (;;) {
            auto* index_slot =
                identity_index + position * index_record_size;
            if (read_u32(index_slot + 4) == 0) {
                write_u32(index_slot, fingerprint);
                write_u32(index_slot + 4, identity.value());
                break;
            }
            position = (position + 1) & identity_mask;
        }
    }

    auto* type_data =
        section_data(compiled_image_section::types);
    auto* type_identity_data =
        section_data(compiled_image_section::type_identities);

    for (std::size_t index = 0; index < graph.types.size(); ++index) {
        const auto& value = graph.types[index];
        auto* record = type_data + index * type_record_size;

        write_u32(record, value.definition.begin);
        write_u32(record + 4, value.definition.count);
        record[8] = static_cast<std::byte>(
            static_cast<std::uint8_t>(value.kind));
        record[9] = static_cast<std::byte>(
            static_cast<std::uint8_t>(value.record_kind));
        record[10] = static_cast<std::byte>(
            static_cast<std::uint8_t>(value.enum_underlying));
        record[11] = static_cast<std::byte>(value.flags);

        write_u32(
            type_identity_data + index * 4,
            graph.type_identities[index].value());
    }

    auto* member_data =
        section_data(compiled_image_section::members);
    for (std::size_t index = 0;
         index < graph.members.size();
         ++index) {

        const auto& value = graph.members[index];
        auto* record = member_data + index * member_record_size;

        write_u32(record, value.name.value());
        write_u32(record + 4, value.type.value());
        record[8] = static_cast<std::byte>(
            static_cast<std::uint8_t>(value.access));
    }

    auto* enum_data =
        section_data(compiled_image_section::enum_values);
    for (std::size_t index = 0;
         index < graph.enum_values.size();
         ++index) {

        const auto& value = graph.enum_values[index];
        auto* record =
            enum_data + index * enum_value_record_size;

        write_u64(record, value.bits);
        write_u32(record + 8, value.name.value());
        record[12] = static_cast<std::byte>(
            static_cast<std::uint8_t>(value.intrinsic));
    }

    auto* object_data =
        section_data(compiled_image_section::objects);
    auto* object_identity_data =
        section_data(compiled_image_section::object_identities);

    for (std::size_t index = 0;
         index < graph.objects.size();
         ++index) {

        const auto& value = graph.objects[index];
        auto* record =
            object_data + index * object_record_size;

        write_u32(record, value.type.value());
        write_u32(record + 4, value.flags);

        write_u32(
            object_identity_data + index * 4,
            graph.object_identities[index].value());
    }

    auto* link_data =
        section_data(compiled_image_section::links);
    for (std::size_t index = 0;
         index < graph.links.size();
         ++index) {

        const auto& value = graph.links[index];
        auto* record = link_data + index * link_record_size;

        write_u32(record, value.source.object.value());
        write_u32(record + 4, value.source.member.value());
        write_u32(record + 8, value.target.object.value());
        write_u32(record + 12, value.target.member.value());
    }

    auto* canonical_data =
        section_data(compiled_image_section::canonical_types);
    for (std::size_t index = 0;
         index < graph.canonical_types.size();
         ++index) {

        const auto& value = graph.canonical_types[index];
        auto* record =
            canonical_data + index * canonical_type_record_size;

        write_u64(record, value.payload);
        write_u32(record + 8, value.child_or_handle);
        record[12] = static_cast<std::byte>(
            static_cast<std::uint8_t>(value.kind));
        record[13] = static_cast<std::byte>(value.detail);
        write_u16(record + 14, 0);
    }

    auto* graph_type_index =
        section_data(compiled_image_section::graph_type_index);
    const auto graph_type_mask = graph_type_index_count - 1;
    std::size_t inserted_types = 0;

    for (std::size_t index = 0;
         index < graph.types.size();
         ++index) {

        if (!graph.types[index].live())
            continue;

        const auto identity = graph.type_identities[index];
        if (!identity)
            return {status_code::initialization_failed};

        const auto hash = graph_identity_hash(identity.value());
        const auto fingerprint = fold32(hash);
        auto position =
            static_cast<std::size_t>(hash) & graph_type_mask;

        for (;;) {
            auto* slot =
                graph_type_index + position * index_record_size;
            if (read_u32(slot + 4) == 0) {
                write_u32(slot, fingerprint);
                write_u32(
                    slot + 4,
                    static_cast<std::uint32_t>(index + 1));
                ++inserted_types;
                break;
            }
            position = (position + 1) & graph_type_mask;
        }
    }

    auto* graph_object_index =
        section_data(compiled_image_section::graph_object_index);
    const auto graph_object_mask = graph_object_index_count - 1;
    std::size_t inserted_objects = 0;

    for (std::size_t index = 0;
         index < graph.objects.size();
         ++index) {

        if (!graph.objects[index].live())
            continue;

        const auto identity = graph.object_identities[index];
        if (!identity)
            return {status_code::initialization_failed};

        const auto hash = graph_identity_hash(identity.value());
        const auto fingerprint = fold32(hash);
        auto position =
            static_cast<std::size_t>(hash) & graph_object_mask;

        for (;;) {
            auto* slot =
                graph_object_index + position * index_record_size;
            if (read_u32(slot + 4) == 0) {
                write_u32(slot, fingerprint);
                write_u32(
                    slot + 4,
                    static_cast<std::uint32_t>(index + 1));
                ++inserted_objects;
                break;
            }
            position = (position + 1) & graph_object_mask;
        }
    }

    auto* graph_link_index =
        section_data(compiled_image_section::graph_link_index);
    const auto graph_link_mask = graph_link_index_count - 1;
    std::size_t inserted_links = 0;

    for (std::size_t index = 0;
         index < graph.links.size();
         ++index) {

        const auto& value = graph.links[index];
        if (!value.live())
            continue;

        const auto hash = endpoint_hash(
            value.target.object.value(),
            value.target.member.value());
        const auto fingerprint = fold32(hash);
        auto position =
            static_cast<std::size_t>(hash) & graph_link_mask;

        for (;;) {
            auto* slot =
                graph_link_index + position * index_record_size;
            if (read_u32(slot + 4) == 0) {
                write_u32(slot, fingerprint);
                write_u32(
                    slot + 4,
                    static_cast<std::uint32_t>(index + 1));
                ++inserted_links;
                break;
            }
            position = (position + 1) & graph_link_mask;
        }
    }

    if (inserted_types != graph.live_types ||
        inserted_objects != graph.live_objects ||
        inserted_links != graph.live_links) {
        output.clear();
        return {status_code::initialization_failed};
    }

    for (auto& value : layout) {
        std::uint64_t byte_count = 0;
        if (!multiply_u64(
                value.count,
                value.record_size,
                byte_count) ||
            byte_count >
                (std::numeric_limits<std::size_t>::max)()) {
            output.clear();
            return {status_code::not_available};
        }

        value.crc64 = crc64(std::span<const std::byte>{
            base + static_cast<std::size_t>(value.offset),
            static_cast<std::size_t>(byte_count)});
    }

    std::copy(image_magic.begin(), image_magic.end(), base);
    write_u32(base + 8, compiled_image_format_version);
    write_u32(base + 12, endian_marker);
    write_u32(base + 16, compiled_image_header_size);
    write_u32(base + 20, compiled_image_directory_count);
    write_u32(base + 24, compiled_image_directory_entry_size);
    write_u32(base + 28, 0);
    write_u64(base + 32, directory_offset);
    write_u64(base + 40, output.size());
    write_u64(
        base + header_string_live_offset,
        observed_string_count);
    write_u64(
        base + header_identity_live_offset,
        observed_identity_count);
    write_u64(
        base + header_type_live_offset,
        graph.live_types);
    write_u64(
        base + header_object_live_offset,
        graph.live_objects);
    write_u64(
        base + header_link_live_offset,
        graph.live_links);

    for (std::size_t index = 0; index < layout.size(); ++index) {
        const auto& value = layout[index];
        auto* entry =
            base +
            directory_offset +
            index * compiled_image_directory_entry_size;

        write_u32(
            entry,
            static_cast<std::uint32_t>(value.kind));
        write_u32(entry + 4, value.record_size);
        write_u64(entry + 8, value.offset);
        write_u64(entry + 16, value.count);
        write_u64(entry + 24, value.crc64);
    }

    const auto directory_crc = crc64(std::span<const std::byte>{
        base + directory_offset,
        directory_bytes});
    write_u64(
        base + header_directory_crc_offset,
        directory_crc);

    std::array<std::byte, compiled_image_header_size> header{};
    std::memcpy(header.data(), base, header.size());
    write_u64(header.data() + header_crc_offset, 0);
    write_u64(
        base + header_crc_offset,
        crc64(header));

    compiled_image_view validation;
    const auto bind_result = validation.bind(output);
    if (!bind_result.ok()) {
        output.clear();
        return bind_result;
    }

    const auto verify_result = validation.verify_contents();
    if (!verify_result.ok()) {
        output.clear();
        return verify_result;
    }

    return {};
}

} // namespace cw::server
