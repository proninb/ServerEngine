#include "source_manager_image.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace cw::server {
namespace {

constexpr std::array<std::byte, 8> image_magic{
    std::byte{'C'}, std::byte{'W'}, std::byte{'S'}, std::byte{'M'},
    std::byte{'V'}, std::byte{'1'}, std::byte{0}, std::byte{0},
};

constexpr std::uint32_t endian_marker = 0x01020304u;
constexpr std::size_t directory_offset = source_manager_image_header_size;
constexpr std::size_t directory_bytes =
    source_manager_image_directory_count * source_manager_image_directory_entry_size;
constexpr std::size_t first_section_offset =
    (directory_offset + directory_bytes + 63u) & ~std::size_t{63u};

constexpr std::uint32_t physical_present = 0x00000001u;

constexpr std::size_t header_change_backend_offset = 96;
constexpr std::size_t header_change_volume_offset = 104;
constexpr std::size_t header_change_journal_offset = 112;
constexpr std::size_t header_change_usn_offset = 120;
constexpr std::size_t header_crc_offset = 128;
constexpr std::size_t header_directory_crc_offset = 136;
constexpr std::size_t header_reserved_begin = 144;

constexpr std::size_t source_core_size = 16;
constexpr std::size_t physical_state_size = 56;
constexpr std::size_t root_record_size = 8;
constexpr std::size_t path_index_record_size = 8;
constexpr std::size_t source_file_identity_index_record_size = 16;
constexpr std::size_t tracked_directory_identity_index_record_size = 16;

struct layout_section final {
    source_manager_image_section kind{};
    std::uint32_t record_size = 0;
    std::uint64_t count = 0;
    std::uint64_t offset = 0;
    std::uint64_t crc64 = 0;
};

[[nodiscard]] constexpr std::size_t section_index(source_manager_image_section kind) noexcept {
    const auto raw = static_cast<std::uint32_t>(kind);
    return raw >= 1 && raw <= source_manager_image_directory_count
        ? static_cast<std::size_t>(raw - 1)
        : source_manager_image_directory_count;
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

    if (left != 0 && right > (std::numeric_limits<std::uint64_t>::max)() / left)
        return false;
    output = left * right;
    return true;
}

void write_u32(std::byte* target, std::uint32_t value) noexcept {
    target[0] = static_cast<std::byte>(value & 0xffu);
    target[1] = static_cast<std::byte>((value >> 8) & 0xffu);
    target[2] = static_cast<std::byte>((value >> 16) & 0xffu);
    target[3] = static_cast<std::byte>((value >> 24) & 0xffu);
}

void write_u64(std::byte* target, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8; ++index)
        target[index] = static_cast<std::byte>((value >> (index * 8)) & 0xffu);
}

void write_i64(std::byte* target, std::int64_t value) noexcept {
    write_u64(target, static_cast<std::uint64_t>(value));
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

[[nodiscard]] std::int64_t read_i64(const std::byte* source) noexcept {
    return static_cast<std::int64_t>(read_u64(source));
}

constexpr std::uint64_t crc64_polynomial = 0x42f0e1eba9ea3693ULL;

[[nodiscard]] std::uint64_t crc64(std::span<const std::byte> bytes) noexcept {
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

[[nodiscard]] std::uint64_t load_u64_unaligned(const char* data) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index)
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
    return value;
}

[[nodiscard]] std::uint32_t load_u32_unaligned(const char* data) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    return
        static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) |
        (static_cast<std::uint32_t>(bytes[3]) << 24);
}

[[nodiscard]] constexpr std::uint64_t xxh64_round(
    std::uint64_t accumulator,
    std::uint64_t input) noexcept {

    accumulator += input * 14029467366897019727ULL;
    accumulator = std::rotl(accumulator, 31);
    return accumulator * 11400714785074694791ULL;
}

[[nodiscard]] std::uint64_t hash_path(std::string_view value) noexcept {
    constexpr std::uint64_t prime1 = 11400714785074694791ULL;
    constexpr std::uint64_t prime2 = 14029467366897019727ULL;
    constexpr std::uint64_t prime3 = 1609587929392839161ULL;
    constexpr std::uint64_t prime4 = 9650029242287828579ULL;
    constexpr std::uint64_t prime5 = 2870177450012600261ULL;

    const char* position = value.data();
    const char* const end = position + value.size();
    std::uint64_t hash = 0;

    if (value.size() >= 32) {
        std::uint64_t lane1 = prime1 + prime2;
        std::uint64_t lane2 = prime2;
        std::uint64_t lane3 = 0;
        std::uint64_t lane4 = 0 - prime1;
        const char* const limit = end - 32;

        do {
            lane1 = xxh64_round(lane1, load_u64_unaligned(position));
            position += 8;
            lane2 = xxh64_round(lane2, load_u64_unaligned(position));
            position += 8;
            lane3 = xxh64_round(lane3, load_u64_unaligned(position));
            position += 8;
            lane4 = xxh64_round(lane4, load_u64_unaligned(position));
            position += 8;
        } while (position <= limit);

        hash =
            std::rotl(lane1, 1) +
            std::rotl(lane2, 7) +
            std::rotl(lane3, 12) +
            std::rotl(lane4, 18);

        const std::uint64_t lanes[] = {lane1, lane2, lane3, lane4};
        for (const auto lane : lanes) {
            hash ^= xxh64_round(0, lane);
            hash = hash * prime1 + prime4;
        }
    }
    else {
        hash = prime5;
    }

    hash += value.size();

    while (position + 8 <= end) {
        const auto lane = xxh64_round(0, load_u64_unaligned(position));
        hash ^= lane;
        hash = std::rotl(hash, 27) * prime1 + prime4;
        position += 8;
    }

    if (position + 4 <= end) {
        hash ^= static_cast<std::uint64_t>(load_u32_unaligned(position)) * prime1;
        hash = std::rotl(hash, 23) * prime2 + prime3;
        position += 4;
    }

    while (position < end) {
        hash ^= static_cast<unsigned char>(*position++) * prime5;
        hash = std::rotl(hash, 11) * prime1;
    }

    hash ^= hash >> 33;
    hash *= prime2;
    hash ^= hash >> 29;
    hash *= prime3;
    hash ^= hash >> 32;
    return hash;
}

[[nodiscard]] constexpr std::uint32_t path_fingerprint(std::uint64_t hash) noexcept {
    const auto folded = static_cast<std::uint32_t>(hash ^ (hash >> 32));
    return folded == 0 ? 1U : folded;
}

[[nodiscard]] std::size_t path_index_capacity(std::size_t source_count) noexcept {
    if (source_count > ((std::numeric_limits<std::size_t>::max)() - 1) / 2)
        return 0;

    const auto required = source_count * 2 + 1;
    std::size_t capacity = 16;
    while (capacity < required) {
        if (capacity > (std::numeric_limits<std::size_t>::max)() / 2)
            return 0;
        capacity *= 2;
    }
    return capacity;
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

[[nodiscard]] bool section_bounds(
    std::size_t image_size,
    std::uint64_t offset,
    std::uint64_t count,
    std::uint32_t record_size) noexcept {

    std::uint64_t byte_size = 0;
    if (!multiply_u64(count, record_size, byte_size))
        return false;

    std::uint64_t end = 0;
    if (!add_u64(offset, byte_size, end))
        return false;

    return offset <= image_size && end <= image_size;
}

[[nodiscard]] bool valid_role(std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(project_item_role::project);
}

} // namespace

source_id source_id_image_range::operator[](std::size_t index) const noexcept {
    if (bytes == nullptr || index >= count)
        return {};
    return source_id{read_u32(bytes + index * sizeof(std::uint32_t))};
}

const source_manager_image_view::section_view& source_manager_image_view::section(
    source_manager_image_section kind) const noexcept {

    static const section_view empty{};
    const auto index = section_index(kind);
    return index < source_manager_image_directory_count ? sections[index] : empty;
}

void source_manager_image_view::reset() noexcept {
    bytes = {};
    for (auto& item : sections)
        item = {};
    generation_value = 0;
    source_count_value = 0;
    root_count_value = 0;
    change_checkpoint_value = {};
}

status source_manager_image_view::bind(std::span<const std::byte> image) noexcept {
    reset();

    if (image.size() < first_section_offset)
        return {status_code::artifact_corrupt};

    if (!std::equal(image_magic.begin(), image_magic.end(), image.begin()))
        return {status_code::artifact_corrupt};

    if (read_u32(image.data() + 8) != source_manager_image_format_version)
        return {status_code::rebuild_required};
    if (read_u32(image.data() + 12) != endian_marker)
        return {status_code::artifact_corrupt};
    if (read_u32(image.data() + 16) != source_manager_image_header_size ||
        read_u32(image.data() + 20) != source_manager_image_directory_count ||
        read_u32(image.data() + 24) != source_manager_image_directory_entry_size) {
        return {status_code::artifact_corrupt};
    }

    const auto stored_directory_offset = read_u64(image.data() + 32);
    const auto stored_file_size = read_u64(image.data() + 40);
    if (stored_directory_offset != directory_offset || stored_file_size != image.size())
        return {status_code::artifact_corrupt};

    std::array<std::byte, source_manager_image_header_size> header{};
    std::memcpy(header.data(), image.data(), header.size());
    const auto stored_header_crc =
        read_u64(header.data() + header_crc_offset);
    write_u64(header.data() + header_crc_offset, 0);
    if (crc64(header) != stored_header_crc)
        return {status_code::artifact_corrupt};

    const auto directory_crc =
        read_u64(image.data() + header_directory_crc_offset);
    const auto directory_span = image.subspan(directory_offset, directory_bytes);
    if (crc64(directory_span) != directory_crc)
        return {status_code::artifact_corrupt};

    section_view candidate[source_manager_image_directory_count]{};
    std::uint64_t previous_end = first_section_offset;

    for (std::size_t index = 0;
         index < source_manager_image_directory_count;
         ++index) {

        const auto* entry =
            image.data() +
            directory_offset +
            index * source_manager_image_directory_entry_size;

        const auto raw_kind = read_u32(entry);
        const auto record_size = read_u32(entry + 4);
        const auto offset = read_u64(entry + 8);
        const auto count = read_u64(entry + 16);
        const auto section_crc = read_u64(entry + 24);
        const auto expected_offset = align64(previous_end);

        if (raw_kind != index + 1 ||
            record_size == 0 ||
            offset != expected_offset ||
            offset > image.size() ||
            !section_bounds(
                image.size(),
                offset,
                count,
                record_size)) {
            return {status_code::artifact_corrupt};
        }

        if (offset > previous_end &&
            !zero_bytes(
                image.data() +
                    static_cast<std::size_t>(previous_end),
                static_cast<std::size_t>(
                    offset - previous_end))) {
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

    if (candidate[section_index(source_manager_image_section::source_core)].record_size !=
            source_core_size ||
        candidate[section_index(source_manager_image_section::physical_state)].record_size !=
            physical_state_size ||
        candidate[section_index(source_manager_image_section::forward_offsets)].record_size != 8 ||
        candidate[section_index(source_manager_image_section::forward_edges)].record_size != 4 ||
        candidate[section_index(source_manager_image_section::reverse_offsets)].record_size != 8 ||
        candidate[section_index(source_manager_image_section::reverse_edges)].record_size != 4 ||
        candidate[section_index(source_manager_image_section::roots)].record_size !=
            root_record_size ||
        candidate[section_index(source_manager_image_section::path_index)].record_size !=
            path_index_record_size ||
        candidate[section_index(source_manager_image_section::path_bytes)].record_size != 1 ||
        candidate[section_index(source_manager_image_section::source_file_identity_index)].record_size !=
            source_file_identity_index_record_size ||
        candidate[section_index(source_manager_image_section::tracked_directory_identity_index)].record_size !=
            tracked_directory_identity_index_record_size) {
        return {status_code::artifact_corrupt};
    }

    const auto source_count = read_u64(image.data() + 56);
    const auto root_count = read_u64(image.data() + 64);
    const auto path_index_count = read_u64(image.data() + 72);
    const auto forward_edge_count = read_u64(image.data() + 80);
    const auto reverse_edge_count = read_u64(image.data() + 88);

    if (source_count > (std::numeric_limits<std::uint32_t>::max)() ||
        source_count > (std::numeric_limits<std::size_t>::max)() ||
        root_count > (std::numeric_limits<std::size_t>::max)()) {
        return {status_code::artifact_corrupt};
    }

    const auto& source_core = candidate[section_index(source_manager_image_section::source_core)];
    const auto& physical = candidate[section_index(source_manager_image_section::physical_state)];
    const auto& forward_offsets =
        candidate[section_index(source_manager_image_section::forward_offsets)];
    const auto& forward_edges =
        candidate[section_index(source_manager_image_section::forward_edges)];
    const auto& reverse_offsets =
        candidate[section_index(source_manager_image_section::reverse_offsets)];
    const auto& reverse_edges =
        candidate[section_index(source_manager_image_section::reverse_edges)];
    const auto& roots = candidate[section_index(source_manager_image_section::roots)];
    const auto& path_index = candidate[section_index(source_manager_image_section::path_index)];
    const auto& source_file_identity_index =
        candidate[section_index(source_manager_image_section::source_file_identity_index)];
    const auto& tracked_directory_identity_index =
        candidate[section_index(source_manager_image_section::tracked_directory_identity_index)];

    const auto raw_change_backend =
        read_u32(image.data() + header_change_backend_offset);
    if (read_u32(image.data() + header_change_backend_offset + 4) != 0 ||
        raw_change_backend >
            static_cast<std::uint32_t>(source_change_backend::windows_usn)) {
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

    const auto valid_optional_index = [](std::uint64_t count) noexcept {
        return count == 0 || (count & (count - 1)) == 0;
    };

    if ((!change_checkpoint &&
         (change_checkpoint.volume_serial != 0 ||
          change_checkpoint.journal_id != 0 ||
          change_checkpoint.next_usn != 0 ||
          source_file_identity_index.count != 0 ||
          tracked_directory_identity_index.count != 0)) ||
        (change_checkpoint &&
         (change_checkpoint.volume_serial == 0 ||
          change_checkpoint.journal_id == 0 ||
          change_checkpoint.next_usn < 0 ||
          (source_count != 0 && source_file_identity_index.count == 0))) ||
        !valid_optional_index(source_file_identity_index.count) ||
        !valid_optional_index(tracked_directory_identity_index.count)) {
        return {status_code::artifact_corrupt};
    }

    for (std::size_t index = header_reserved_begin;
         index < source_manager_image_header_size;
         ++index) {
        if (image[index] != std::byte{0})
            return {status_code::artifact_corrupt};
    }

    if (source_core.count != source_count ||
        physical.count != source_count ||
        forward_offsets.count != source_count + 1 ||
        reverse_offsets.count != source_count + 1 ||
        forward_edges.count != forward_edge_count ||
        reverse_edges.count != reverse_edge_count ||
        roots.count != root_count ||
        path_index.count != path_index_count ||
        path_index_count == 0 ||
        (path_index_count & (path_index_count - 1)) != 0) {
        return {status_code::artifact_corrupt};
    }

    const auto read_last_offset = [](const section_view& offsets, std::uint64_t index) noexcept {
        return read_u64(offsets.data + static_cast<std::size_t>(index) * 8);
    };

    if (read_last_offset(forward_offsets, source_count) != forward_edge_count ||
        read_last_offset(reverse_offsets, source_count) != reverse_edge_count) {
        return {status_code::artifact_corrupt};
    }

    bytes = image;
    std::copy(std::begin(candidate), std::end(candidate), std::begin(sections));
    generation_value = read_u64(image.data() + 48);
    source_count_value = static_cast<std::size_t>(source_count);
    root_count_value = static_cast<std::size_t>(root_count);
    change_checkpoint_value = change_checkpoint;
    return {};
}

std::string_view source_manager_image_view::path(source_id source) const noexcept {
    if (!valid_source(source))
        return {};

    const auto index = static_cast<std::size_t>(source.value() - 1);
    const auto& core = section(source_manager_image_section::source_core);
    const auto& paths = section(source_manager_image_section::path_bytes);
    const auto* record = core.data + index * source_core_size;
    const auto offset = read_u64(record);
    const auto length = read_u32(record + 8);

    if (offset > paths.count || length > paths.count - offset)
        return {};

    return {
        reinterpret_cast<const char*>(paths.data + static_cast<std::size_t>(offset)),
        static_cast<std::size_t>(length),
    };
}

status source_manager_image_view::physical(
    source_id source,
    source_manager_image_physical_state& output) const noexcept {

    output = {};
    if (!valid_source(source))
        return {status_code::invalid_argument};

    const auto index = static_cast<std::size_t>(source.value() - 1);
    const auto& physical_section = section(source_manager_image_section::physical_state);
    const auto* record = physical_section.data + index * physical_state_size;

    output.present = (read_u32(record) & physical_present) != 0;
    output.write_time_ticks = read_i64(record + 8);
    output.size = read_u64(record + 16);
    std::memcpy(output.hash.bytes.data(), record + 24, output.hash.bytes.size());
    return {};
}

source_id_image_range source_manager_image_view::includes(source_id source) const noexcept {
    if (!valid_source(source))
        return {};

    const auto index = static_cast<std::size_t>(source.value() - 1);
    const auto& offsets = section(source_manager_image_section::forward_offsets);
    const auto& edges = section(source_manager_image_section::forward_edges);
    const auto begin = read_u64(offsets.data + index * 8);
    const auto end = read_u64(offsets.data + (index + 1) * 8);

    if (begin > end || end > edges.count)
        return {};

    return source_id_image_range{
        edges.data + static_cast<std::size_t>(begin) * 4,
        static_cast<std::size_t>(end - begin),
    };
}

source_id_image_range source_manager_image_view::dependents(source_id source) const noexcept {
    if (!valid_source(source))
        return {};

    const auto index = static_cast<std::size_t>(source.value() - 1);
    const auto& offsets = section(source_manager_image_section::reverse_offsets);
    const auto& edges = section(source_manager_image_section::reverse_edges);
    const auto begin = read_u64(offsets.data + index * 8);
    const auto end = read_u64(offsets.data + (index + 1) * 8);

    if (begin > end || end > edges.count)
        return {};

    return source_id_image_range{
        edges.data + static_cast<std::size_t>(begin) * 4,
        static_cast<std::size_t>(end - begin),
    };
}

status source_manager_image_view::root(
    std::size_t index,
    source_manager_image_root& output) const noexcept {

    output = {};
    const auto& roots = section(source_manager_image_section::roots);
    if (index >= roots.count)
        return {status_code::invalid_argument};

    const auto* record = roots.data + index * root_record_size;
    const auto source = read_u32(record);
    const auto role = static_cast<std::uint8_t>(record[4]);
    if (source == 0 || source > source_count_value || !valid_role(role))
        return {status_code::artifact_corrupt};

    output.source = source_id{source};
    output.role = static_cast<project_item_role>(role);
    return {};
}

status source_manager_image_view::find(
    std::string_view normalized_path,
    source_id& output) const noexcept {

    output = {};
    if (!valid() || normalized_path.empty())
        return {status_code::not_found};

    const auto& index = section(source_manager_image_section::path_index);
    if (index.count == 0)
        return {status_code::not_found};

    const auto hash = hash_path(normalized_path);
    const auto fingerprint = path_fingerprint(hash);
    const auto mask = static_cast<std::size_t>(index.count - 1);
    auto position = static_cast<std::size_t>(hash) & mask;

    for (std::size_t probe = 0; probe < index.count; ++probe) {
        const auto* slot = index.data + position * path_index_record_size;
        const auto slot_source = read_u32(slot + 4);
        if (slot_source == 0)
            return {status_code::not_found};

        if (read_u32(slot) == fingerprint) {
            const auto candidate = source_id{slot_source};
            if (path(candidate) == normalized_path) {
                output = candidate;
                return {};
            }
        }

        position = (position + 1) & mask;
    }

    return {status_code::not_found};
}

source_id source_manager_image_view::find_source_file(
    std::uint64_t file_reference) const noexcept {

    if (file_reference == 0)
        return {};

    const auto& values =
        section(source_manager_image_section::source_file_identity_index);
    if (values.count == 0 ||
        (values.count & (values.count - 1)) != 0) {
        return {};
    }

    const auto mask = static_cast<std::size_t>(values.count - 1);
    auto position = static_cast<std::size_t>(mix64(file_reference)) & mask;

    for (std::size_t probe = 0; probe < values.count; ++probe) {
        const auto* slot =
            values.data + position * source_file_identity_index_record_size;
        const auto candidate = read_u64(slot);
        if (candidate == 0)
            return {};
        if (candidate == file_reference) {
            const source_id source{read_u32(slot + 8)};
            return source &&
                static_cast<std::size_t>(source.value()) <= source_count_value &&
                read_u32(slot + 12) == 0
                ? source
                : source_id{};
        }
        position = (position + 1) & mask;
    }

    return {};
}

std::uint32_t source_manager_image_view::directory_watch_flags(
    std::uint64_t file_reference) const noexcept {

    if (file_reference == 0)
        return 0;

    const auto& values =
        section(source_manager_image_section::tracked_directory_identity_index);
    if (values.count == 0 ||
        (values.count & (values.count - 1)) != 0) {
        return 0;
    }

    const auto mask = static_cast<std::size_t>(values.count - 1);
    auto position = static_cast<std::size_t>(mix64(file_reference)) & mask;

    for (std::size_t probe = 0; probe < values.count; ++probe) {
        const auto* slot =
            values.data + position * tracked_directory_identity_index_record_size;
        const auto candidate = read_u64(slot);
        if (candidate == 0)
            return 0;
        if (candidate == file_reference) {
            const auto flags = read_u32(slot + 8);
            return read_u32(slot + 12) == 0 &&
                (flags & ~source_change_directory_watch_known) == 0
                ? flags
                : 0;
        }
        position = (position + 1) & mask;
    }

    return 0;
}

status source_manager_image_view::verify_contents() const noexcept {
    if (!valid())
        return {status_code::invalid_state};

    for (std::size_t index = 0; index < source_manager_image_directory_count; ++index) {
        const auto& item = sections[index];
        std::uint64_t byte_count = 0;
        if (!multiply_u64(item.count, item.record_size, byte_count) ||
            byte_count > (std::numeric_limits<std::size_t>::max)()) {
            return {status_code::artifact_corrupt};
        }

        if (crc64(std::span<const std::byte>{
                item.data, static_cast<std::size_t>(byte_count)}) != item.crc64) {
            return {status_code::artifact_corrupt};
        }
    }

    for (std::size_t index = 0; index < source_count_value; ++index) {
        const auto source = source_id{static_cast<std::uint32_t>(index + 1)};
        const auto source_path = path(source);
        if (source_path.empty())
            return {status_code::artifact_corrupt};

        for (std::size_t edge = 0; edge < includes(source).size(); ++edge) {
            if (!valid_source(includes(source)[edge]))
                return {status_code::artifact_corrupt};
        }
        for (std::size_t edge = 0; edge < dependents(source).size(); ++edge) {
            if (!valid_source(dependents(source)[edge]))
                return {status_code::artifact_corrupt};
        }

        source_id found;
        if (!find(source_path, found).ok() || found != source)
            return {status_code::artifact_corrupt};
    }

    for (std::size_t index = 0; index < root_count_value; ++index) {
        source_manager_image_root root_value;
        if (!root(index, root_value).ok())
            return {status_code::artifact_corrupt};
    }

    return {};
}

status encode_source_manager_image(
    const source_manager& manager,
    const source_manager_image_options& options,
    std::vector<std::byte>& output) noexcept {

    output.clear();

    const auto source_count = manager.source_count();
    if (source_count > (std::numeric_limits<std::uint32_t>::max)())
        return {status_code::not_available};

    std::uint64_t path_bytes = 0;
    std::uint64_t forward_edges = 0;
    std::uint64_t reverse_edges = 0;

    for (std::size_t index = 0; index < source_count; ++index) {
        const auto source = source_id{static_cast<std::uint32_t>(index + 1)};
        const auto source_path = manager.path(source);
        if (source_path.empty())
            return {status_code::artifact_corrupt};
        if (source_path.size() > (std::numeric_limits<std::uint32_t>::max)())
            return {status_code::not_available};

        if (!add_u64(path_bytes, source_path.size(), path_bytes) ||
            !add_u64(forward_edges, manager.include_count(source), forward_edges) ||
            !add_u64(reverse_edges, manager.dependent_count(source), reverse_edges)) {
            return {status_code::not_available};
        }
    }

    for (const auto& root : options.roots) {
        if (!root.source ||
            static_cast<std::size_t>(root.source.value()) > source_count ||
            !valid_role(static_cast<std::uint8_t>(root.role))) {
            return {status_code::invalid_argument};
        }
    }

    if ((!options.change_checkpoint &&
         (!options.file_identity_index.empty() ||
          !options.directory_identity_index.empty())) ||
        (options.change_checkpoint &&
         (options.change_checkpoint.volume_serial == 0 ||
          options.change_checkpoint.journal_id == 0 ||
          options.change_checkpoint.next_usn < 0 ||
          (source_count != 0 && options.file_identity_index.empty()))) ||
        (!options.file_identity_index.empty() &&
         (options.file_identity_index.size() &
          (options.file_identity_index.size() - 1)) != 0) ||
        (!options.directory_identity_index.empty() &&
         (options.directory_identity_index.size() &
          (options.directory_identity_index.size() - 1)) != 0)) {
        return {status_code::invalid_argument};
    }

    const auto index_capacity = path_index_capacity(source_count);
    if (index_capacity == 0)
        return {status_code::not_available};

    std::array<layout_section, source_manager_image_directory_count> layout{{
        {source_manager_image_section::source_core, source_core_size, source_count},
        {source_manager_image_section::physical_state, physical_state_size, source_count},
        {source_manager_image_section::forward_offsets, 8, source_count + 1},
        {source_manager_image_section::forward_edges, 4, forward_edges},
        {source_manager_image_section::reverse_offsets, 8, source_count + 1},
        {source_manager_image_section::reverse_edges, 4, reverse_edges},
        {source_manager_image_section::roots, root_record_size, options.roots.size()},
        {source_manager_image_section::path_index, path_index_record_size, index_capacity},
        {source_manager_image_section::path_bytes, 1, path_bytes},
        {source_manager_image_section::source_file_identity_index,
            source_file_identity_index_record_size, options.file_identity_index.size()},
        {source_manager_image_section::tracked_directory_identity_index,
            tracked_directory_identity_index_record_size, options.directory_identity_index.size()},
    }};

    std::uint64_t cursor = first_section_offset;
    for (auto& item : layout) {
        cursor = align64(cursor);
        item.offset = cursor;

        std::uint64_t bytes = 0;
        if (!multiply_u64(item.count, item.record_size, bytes) ||
            !add_u64(cursor, bytes, cursor)) {
            return {status_code::not_available};
        }
    }

    if (cursor > (std::numeric_limits<std::size_t>::max)())
        return {status_code::not_available};

    try {
        output.assign(static_cast<std::size_t>(cursor), std::byte{0});
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }

    auto* base = output.data();

    std::uint64_t path_cursor = 0;
    std::uint64_t forward_cursor = 0;
    std::uint64_t reverse_cursor = 0;

    auto* source_core = base + static_cast<std::size_t>(
        layout[section_index(source_manager_image_section::source_core)].offset);
    auto* physical = base + static_cast<std::size_t>(
        layout[section_index(source_manager_image_section::physical_state)].offset);
    auto* forward_offsets = base + static_cast<std::size_t>(
        layout[section_index(source_manager_image_section::forward_offsets)].offset);
    auto* forward_edge_data = base + static_cast<std::size_t>(
        layout[section_index(source_manager_image_section::forward_edges)].offset);
    auto* reverse_offsets = base + static_cast<std::size_t>(
        layout[section_index(source_manager_image_section::reverse_offsets)].offset);
    auto* reverse_edge_data = base + static_cast<std::size_t>(
        layout[section_index(source_manager_image_section::reverse_edges)].offset);
    auto* root_data = base + static_cast<std::size_t>(
        layout[section_index(source_manager_image_section::roots)].offset);
    auto* path_index = base + static_cast<std::size_t>(
        layout[section_index(source_manager_image_section::path_index)].offset);
    auto* path_data = base + static_cast<std::size_t>(
        layout[section_index(source_manager_image_section::path_bytes)].offset);
    auto* file_identity_data = base + static_cast<std::size_t>(
        layout[section_index(source_manager_image_section::source_file_identity_index)].offset);
    auto* directory_identity_data = base + static_cast<std::size_t>(
        layout[section_index(source_manager_image_section::tracked_directory_identity_index)].offset);

    write_u64(forward_offsets, 0);
    write_u64(reverse_offsets, 0);

    for (std::size_t index = 0; index < source_count; ++index) {
        const auto source = source_id{static_cast<std::uint32_t>(index + 1)};
        const auto source_path = manager.path(source);
        const auto snapshot = manager.current(source);

        auto* core = source_core + index * source_core_size;
        write_u64(core, path_cursor);
        write_u32(core + 8, static_cast<std::uint32_t>(source_path.size()));
        write_u32(core + 12, 0);
        std::memcpy(
            path_data + static_cast<std::size_t>(path_cursor),
            source_path.data(),
            source_path.size());
        path_cursor += source_path.size();

        auto* physical_record = physical + index * physical_state_size;
        write_u32(physical_record, snapshot ? physical_present : 0);
        write_u32(physical_record + 4, 0);
        if (snapshot) {
            const auto observation = snapshot.observation();
            write_i64(physical_record + 8, observation.write_time_ticks);
            write_u64(physical_record + 16, observation.size);
            std::memcpy(
                physical_record + 24,
                snapshot.hash().bytes.data(),
                snapshot.hash().bytes.size());
        }

        const auto include_count = manager.include_count(source);
        for (std::size_t edge = 0; edge < include_count; ++edge) {
            const auto dependency = manager.include_at(source, edge);
            if (!dependency ||
                static_cast<std::size_t>(dependency.value()) > source_count) {
                output.clear();
                return {status_code::artifact_corrupt};
            }
            write_u32(
                forward_edge_data + static_cast<std::size_t>(forward_cursor) * 4,
                dependency.value());
            ++forward_cursor;
        }
        write_u64(forward_offsets + (index + 1) * 8, forward_cursor);

        const auto dependent_count = manager.dependent_count(source);
        for (std::size_t edge = 0; edge < dependent_count; ++edge) {
            const auto dependent = manager.dependent_at(source, edge);
            if (!dependent ||
                static_cast<std::size_t>(dependent.value()) > source_count) {
                output.clear();
                return {status_code::artifact_corrupt};
            }
            write_u32(
                reverse_edge_data + static_cast<std::size_t>(reverse_cursor) * 4,
                dependent.value());
            ++reverse_cursor;
        }
        write_u64(reverse_offsets + (index + 1) * 8, reverse_cursor);
    }

    for (std::size_t index = 0; index < options.roots.size(); ++index) {
        const auto& root = options.roots[index];
        auto* record = root_data + index * root_record_size;
        write_u32(record, root.source.value());
        record[4] = static_cast<std::byte>(static_cast<std::uint8_t>(root.role));
    }

    const auto index_mask = index_capacity - 1;
    for (std::size_t index = 0; index < source_count; ++index) {
        const auto source = source_id{static_cast<std::uint32_t>(index + 1)};
        const auto normalized = manager.path(source);
        const auto hash = hash_path(normalized);
        auto position = static_cast<std::size_t>(hash) & index_mask;

        while (read_u32(path_index + position * path_index_record_size + 4) != 0)
            position = (position + 1) & index_mask;

        auto* slot = path_index + position * path_index_record_size;
        write_u32(slot, path_fingerprint(hash));
        write_u32(slot + 4, source.value());
    }

    for (std::size_t index = 0;
         index < options.file_identity_index.size();
         ++index) {
        const auto& item = options.file_identity_index[index];
        if ((item.file_reference == 0 &&
             (item.source || item.reserved != 0)) ||
            (item.file_reference != 0 &&
             (!item.source ||
              static_cast<std::size_t>(item.source.value()) > source_count ||
              item.reserved != 0))) {
            output.clear();
            return {status_code::invalid_argument};
        }

        auto* record =
            file_identity_data +
            index * source_file_identity_index_record_size;
        write_u64(record, item.file_reference);
        write_u32(record + 8, item.source.value());
        write_u32(record + 12, item.reserved);
    }

    for (std::size_t index = 0;
         index < options.directory_identity_index.size();
         ++index) {
        const auto& item = options.directory_identity_index[index];
        if ((item.file_reference == 0 &&
             (item.flags != 0 || item.reserved != 0)) ||
            (item.file_reference != 0 &&
             (item.flags == 0 ||
              (item.flags & ~source_change_directory_watch_known) != 0 ||
              item.reserved != 0))) {
            output.clear();
            return {status_code::invalid_argument};
        }

        auto* record =
            directory_identity_data +
            index * tracked_directory_identity_index_record_size;
        write_u64(record, item.file_reference);
        write_u32(record + 8, item.flags);
        write_u32(record + 12, item.reserved);
    }

    for (auto& item : layout) {
        std::uint64_t byte_count = 0;
        if (!multiply_u64(item.count, item.record_size, byte_count) ||
            byte_count > (std::numeric_limits<std::size_t>::max)()) {
            output.clear();
            return {status_code::not_available};
        }

        item.crc64 = crc64(std::span<const std::byte>{
            base + static_cast<std::size_t>(item.offset),
            static_cast<std::size_t>(byte_count)});
    }

    std::copy(image_magic.begin(), image_magic.end(), base);
    write_u32(base + 8, source_manager_image_format_version);
    write_u32(base + 12, endian_marker);
    write_u32(base + 16, source_manager_image_header_size);
    write_u32(base + 20, source_manager_image_directory_count);
    write_u32(base + 24, source_manager_image_directory_entry_size);
    write_u32(base + 28, 0);
    write_u64(base + 32, directory_offset);
    write_u64(base + 40, output.size());
    write_u64(base + 48, options.generation);
    write_u64(base + 56, source_count);
    write_u64(base + 64, options.roots.size());
    write_u64(base + 72, index_capacity);
    write_u64(base + 80, forward_edges);
    write_u64(base + 88, reverse_edges);
    write_u32(
        base + header_change_backend_offset,
        static_cast<std::uint32_t>(options.change_checkpoint.backend));
    write_u32(base + header_change_backend_offset + 4, 0);
    write_u64(base + header_change_volume_offset, options.change_checkpoint.volume_serial);
    write_u64(base + header_change_journal_offset, options.change_checkpoint.journal_id);
    write_u64(
        base + header_change_usn_offset,
        static_cast<std::uint64_t>(options.change_checkpoint.next_usn));
    write_u64(base + header_crc_offset, 0);
    write_u64(base + header_directory_crc_offset, 0);
    write_u64(base + header_reserved_begin, 0);
    write_u64(base + header_reserved_begin + 8, 0);

    for (std::size_t index = 0; index < layout.size(); ++index) {
        const auto& item = layout[index];
        auto* entry = base + directory_offset +
            index * source_manager_image_directory_entry_size;
        write_u32(entry, static_cast<std::uint32_t>(item.kind));
        write_u32(entry + 4, item.record_size);
        write_u64(entry + 8, item.offset);
        write_u64(entry + 16, item.count);
        write_u64(entry + 24, item.crc64);
    }

    const auto directory_crc = crc64(std::span<const std::byte>{
        base + directory_offset, directory_bytes});
    write_u64(base + header_directory_crc_offset, directory_crc);

    std::array<std::byte, source_manager_image_header_size> header{};
    std::memcpy(header.data(), base, header.size());
    write_u64(header.data() + header_crc_offset, 0);
    const auto header_crc = crc64(header);
    write_u64(base + header_crc_offset, header_crc);

    source_manager_image_view validation;
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
