#include "source_manager_image.hpp"
#include "crc64_ecma.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

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
    source_manager_image_prefix_size;

// Preserve the D4E sparse replacement budget even though the generic
// scatter/gather carrier is larger for sectioned physical storage.
constexpr std::size_t source_manager_sparse_extent_budget = 32;

constexpr std::uint32_t physical_present = 0x00000001u;

constexpr std::size_t header_change_backend_offset = 96;
constexpr std::size_t header_change_volume_offset = 104;
constexpr std::size_t header_change_journal_offset = 112;
constexpr std::size_t header_change_usn_offset = 120;
constexpr std::size_t header_crc_offset = 128;
constexpr std::size_t header_directory_crc_offset = 136;
constexpr std::size_t header_reserved_begin = 144;

constexpr std::size_t source_core_size = 8;
constexpr std::size_t physical_state_size = 56;
constexpr std::size_t graph_record_size = 40;
constexpr std::uint32_t graph_known_flags = 0x00000003u;
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

using source_manager_freeze_clock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t source_manager_elapsed_ns(
    source_manager_freeze_clock::time_point begin) noexcept {

    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            source_manager_freeze_clock::now() - begin).count());
}

void publish_source_manager_crc_sections(
    source_manager_freeze_telemetry* telemetry,
    const std::array<std::span<const std::byte>, 10>& sections,
    const std::array<std::uint64_t, 10>& elapsed) noexcept {

    if (telemetry == nullptr)
        return;

    std::uint64_t total_bytes = 0;
    for (const auto section : sections)
        total_bytes += static_cast<std::uint64_t>(section.size());

    telemetry->crc_total_bytes = total_bytes;

    telemetry->crc_source_core_ns = elapsed[0];
    telemetry->crc_source_core_bytes = sections[0].size();
    telemetry->crc_physical_state_ns = elapsed[1];
    telemetry->crc_physical_state_bytes = sections[1].size();
    telemetry->crc_graph_records_ns = elapsed[2];
    telemetry->crc_graph_records_bytes = sections[2].size();
    telemetry->crc_forward_edges_ns = elapsed[3];
    telemetry->crc_forward_edges_bytes = sections[3].size();
    telemetry->crc_reverse_edges_ns = elapsed[4];
    telemetry->crc_reverse_edges_bytes = sections[4].size();
    telemetry->crc_roots_ns = elapsed[5];
    telemetry->crc_roots_bytes = sections[5].size();
    telemetry->crc_path_index_ns = elapsed[6];
    telemetry->crc_path_index_bytes = sections[6].size();
    telemetry->crc_path_bytes_ns = elapsed[7];
    telemetry->crc_path_bytes_bytes = sections[7].size();
    telemetry->crc_file_identity_ns = elapsed[8];
    telemetry->crc_file_identity_bytes = sections[8].size();
    telemetry->crc_directory_identity_ns = elapsed[9];
    telemetry->crc_directory_identity_bytes = sections[9].size();
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
    prefix_bytes = {};
    logical_size_value = 0;
    for (auto& item : sections)
        item = {};
    generation_value = 0;
    source_count_value = 0;
    root_count_value = 0;
    change_checkpoint_value = {};
}

status source_manager_image_view::bind(
    std::span<const std::byte> image) noexcept {

    reset();

    if (image.size() < first_section_offset)
        return {status_code::artifact_corrupt};

    if (!std::equal(
            image_magic.begin(),
            image_magic.end(),
            image.begin())) {
        return {status_code::artifact_corrupt};
    }

    if (read_u32(image.data() + 8) !=
        source_manager_image_format_version) {
        return {status_code::rebuild_required};
    }

    if (read_u32(image.data() + 12) != endian_marker)
        return {status_code::artifact_corrupt};

    if (read_u32(image.data() + 16) !=
            source_manager_image_header_size ||
        read_u32(image.data() + 20) !=
            source_manager_image_directory_count ||
        read_u32(image.data() + 24) !=
            source_manager_image_directory_entry_size) {
        return {status_code::artifact_corrupt};
    }

    const auto stored_directory_offset =
        read_u64(image.data() + 32);
    const auto stored_file_size =
        read_u64(image.data() + 40);

    if (stored_directory_offset != directory_offset ||
        stored_file_size != image.size()) {
        return {status_code::artifact_corrupt};
    }

    std::array<std::byte, source_manager_image_header_size>
        header{};
    std::memcpy(
        header.data(),
        image.data(),
        header.size());

    const auto stored_header_crc =
        read_u64(header.data() + header_crc_offset);
    write_u64(
        header.data() + header_crc_offset,
        0);

    if (persistence_crc64(header) != stored_header_crc)
        return {status_code::artifact_corrupt};

    const auto directory_crc =
        read_u64(
            image.data() +
            header_directory_crc_offset);

    const auto directory_span =
        image.subspan(
            directory_offset,
            directory_bytes);

    if (persistence_crc64(directory_span) !=
        directory_crc) {
        return {status_code::artifact_corrupt};
    }

    section_view
        candidate[source_manager_image_directory_count]{};

    std::uint64_t previous_end =
        first_section_offset;

    for (std::size_t index = 0;
         index < source_manager_image_directory_count;
         ++index) {

        const auto* entry =
            image.data() +
            directory_offset +
            index *
                source_manager_image_directory_entry_size;

        const auto raw_kind =
            read_u32(entry);
        const auto record_size =
            read_u32(entry + 4);
        const auto offset =
            read_u64(entry + 8);
        const auto count =
            read_u64(entry + 16);
        const auto section_crc =
            read_u64(entry + 24);
        const auto expected_offset =
            align64(previous_end);

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
                    static_cast<std::size_t>(
                        previous_end),
                static_cast<std::size_t>(
                    offset - previous_end))) {
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
            end > image.size()) {
            return {status_code::artifact_corrupt};
        }

        candidate[index] = section_view{
            image.data() +
                static_cast<std::size_t>(offset),
            count,
            record_size,
            section_crc,
            offset,
        };

        previous_end = end;
    }

    if (previous_end != image.size())
        return {status_code::artifact_corrupt};

    if (candidate[
            section_index(
                source_manager_image_section::source_core)]
                .record_size != source_core_size ||
        candidate[
            section_index(
                source_manager_image_section::physical_state)]
                .record_size != physical_state_size ||
        candidate[
            section_index(
                source_manager_image_section::graph_records)]
                .record_size != graph_record_size ||
        candidate[
            section_index(
                source_manager_image_section::forward_edges)]
                .record_size != 4 ||
        candidate[
            section_index(
                source_manager_image_section::reverse_edges)]
                .record_size != 4 ||
        candidate[
            section_index(
                source_manager_image_section::roots)]
                .record_size != root_record_size ||
        candidate[
            section_index(
                source_manager_image_section::path_index)]
                .record_size != path_index_record_size ||
        candidate[
            section_index(
                source_manager_image_section::path_bytes)]
                .record_size != 1 ||
        candidate[
            section_index(
                source_manager_image_section::
                    source_file_identity_index)]
                .record_size !=
                    source_file_identity_index_record_size ||
        candidate[
            section_index(
                source_manager_image_section::
                    tracked_directory_identity_index)]
                .record_size !=
                    tracked_directory_identity_index_record_size) {
        return {status_code::artifact_corrupt};
    }

    const auto source_count =
        read_u64(image.data() + 56);
    const auto root_count =
        read_u64(image.data() + 64);
    const auto path_index_count =
        read_u64(image.data() + 72);
    const auto forward_edge_count =
        read_u64(image.data() + 80);
    const auto reverse_edge_count =
        read_u64(image.data() + 88);

    if (source_count >
            (std::numeric_limits<std::uint32_t>::max)() ||
        source_count >
            (std::numeric_limits<std::size_t>::max)() ||
        root_count >
            (std::numeric_limits<std::size_t>::max)()) {
        return {status_code::artifact_corrupt};
    }

    const auto& source_core =
        candidate[
            section_index(
                source_manager_image_section::source_core)];
    const auto& physical =
        candidate[
            section_index(
                source_manager_image_section::physical_state)];
    const auto& graph =
        candidate[
            section_index(
                source_manager_image_section::graph_records)];
    const auto& forward_edges =
        candidate[
            section_index(
                source_manager_image_section::forward_edges)];
    const auto& reverse_edges =
        candidate[
            section_index(
                source_manager_image_section::reverse_edges)];
    const auto& roots =
        candidate[
            section_index(
                source_manager_image_section::roots)];
    const auto& path_index =
        candidate[
            section_index(
                source_manager_image_section::path_index)];
    const auto& source_file_identity_index =
        candidate[
            section_index(
                source_manager_image_section::
                    source_file_identity_index)];
    const auto& tracked_directory_identity_index =
        candidate[
            section_index(
                source_manager_image_section::
                    tracked_directory_identity_index)];

    const auto raw_change_backend =
        read_u32(
            image.data() +
            header_change_backend_offset);

    if (read_u32(
            image.data() +
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
            image.data() +
            header_change_volume_offset);
    change_checkpoint.journal_id =
        read_u64(
            image.data() +
            header_change_journal_offset);
    change_checkpoint.next_usn =
        static_cast<std::int64_t>(
            read_u64(
                image.data() +
                header_change_usn_offset));

    const auto valid_optional_index =
        [](std::uint64_t count) noexcept {
            return count == 0 ||
                (count & (count - 1)) == 0;
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
          (source_count != 0 &&
           source_file_identity_index.count == 0))) ||
        !valid_optional_index(
            source_file_identity_index.count) ||
        !valid_optional_index(
            tracked_directory_identity_index.count)) {
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
        graph.count != source_count ||
        forward_edges.count != forward_edge_count ||
        reverse_edges.count != reverse_edge_count ||
        roots.count != root_count ||
        path_index.count != path_index_count ||
        path_index_count == 0 ||
        (path_index_count &
         (path_index_count - 1)) != 0) {
        return {status_code::artifact_corrupt};
    }

    bytes = image;
    prefix_bytes =
        image.first(first_section_offset);
    logical_size_value = image.size();

    std::copy(
        std::begin(candidate),
        std::end(candidate),
        std::begin(sections));

    generation_value =
        read_u64(image.data() + 48);
    source_count_value =
        static_cast<std::size_t>(
            source_count);
    root_count_value =
        static_cast<std::size_t>(
            root_count);
    change_checkpoint_value =
        change_checkpoint;

    return {};
}

status source_manager_image_view::bind_sectioned(
    std::span<const std::byte> prefix,
    const std::array<
        std::span<const std::byte>,
        source_manager_image_directory_count>& section_images) noexcept {

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
        source_manager_image_format_version) {
        return {status_code::rebuild_required};
    }

    if (read_u32(prefix.data() + 12) != endian_marker)
        return {status_code::artifact_corrupt};

    if (read_u32(prefix.data() + 16) !=
            source_manager_image_header_size ||
        read_u32(prefix.data() + 20) !=
            source_manager_image_directory_count ||
        read_u32(prefix.data() + 24) !=
            source_manager_image_directory_entry_size) {
        return {status_code::artifact_corrupt};
    }

    const auto stored_directory_offset =
        read_u64(prefix.data() + 32);
    const auto stored_file_size =
        read_u64(prefix.data() + 40);

    if (stored_directory_offset != directory_offset ||
        stored_file_size < first_section_offset) {
        return {status_code::artifact_corrupt};
    }

    std::array<
        std::byte,
        source_manager_image_header_size>
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

    const auto directory_crc =
        read_u64(
            prefix.data() +
            header_directory_crc_offset);

    const auto directory_span =
        prefix.subspan(
            directory_offset,
            directory_bytes);

    if (persistence_crc64(directory_span) !=
        directory_crc) {
        return {status_code::artifact_corrupt};
    }

    section_view
        candidate[source_manager_image_directory_count]{};

    std::uint64_t previous_end =
        first_section_offset;

    for (std::size_t index = 0;
         index <
            source_manager_image_directory_count;
         ++index) {

        const auto* entry =
            prefix.data() +
            directory_offset +
            index *
                source_manager_image_directory_entry_size;

        const auto raw_kind =
            read_u32(entry);
        const auto record_size =
            read_u32(entry + 4);
        const auto offset =
            read_u64(entry + 8);
        const auto count =
            read_u64(entry + 16);
        const auto section_crc =
            read_u64(entry + 24);
        const auto expected_offset =
            align64(previous_end);

        std::uint64_t byte_count = 0;
        std::uint64_t end = 0;

        if (raw_kind != index + 1 ||
            record_size == 0 ||
            offset != expected_offset ||
            !multiply_u64(
                count,
                record_size,
                byte_count) ||
            !add_u64(
                offset,
                byte_count,
                end) ||
            end > stored_file_size ||
            byte_count >
                (std::numeric_limits<
                    std::size_t>::max)() ||
            section_images[index].size() !=
                static_cast<std::size_t>(
                    byte_count)) {
            return {status_code::artifact_corrupt};
        }

        candidate[index] = section_view{
            section_images[index].data(),
            count,
            record_size,
            section_crc,
            offset,
        };

        previous_end = end;
    }

    if (previous_end != stored_file_size)
        return {status_code::artifact_corrupt};

    if (candidate[
            section_index(
                source_manager_image_section::source_core)]
                .record_size != source_core_size ||
        candidate[
            section_index(
                source_manager_image_section::physical_state)]
                .record_size != physical_state_size ||
        candidate[
            section_index(
                source_manager_image_section::graph_records)]
                .record_size != graph_record_size ||
        candidate[
            section_index(
                source_manager_image_section::forward_edges)]
                .record_size != 4 ||
        candidate[
            section_index(
                source_manager_image_section::reverse_edges)]
                .record_size != 4 ||
        candidate[
            section_index(
                source_manager_image_section::roots)]
                .record_size != root_record_size ||
        candidate[
            section_index(
                source_manager_image_section::path_index)]
                .record_size != path_index_record_size ||
        candidate[
            section_index(
                source_manager_image_section::path_bytes)]
                .record_size != 1 ||
        candidate[
            section_index(
                source_manager_image_section::
                    source_file_identity_index)]
                .record_size !=
                    source_file_identity_index_record_size ||
        candidate[
            section_index(
                source_manager_image_section::
                    tracked_directory_identity_index)]
                .record_size !=
                    tracked_directory_identity_index_record_size) {
        return {status_code::artifact_corrupt};
    }

    const auto source_count =
        read_u64(prefix.data() + 56);
    const auto root_count =
        read_u64(prefix.data() + 64);
    const auto path_index_count =
        read_u64(prefix.data() + 72);
    const auto forward_edge_count =
        read_u64(prefix.data() + 80);
    const auto reverse_edge_count =
        read_u64(prefix.data() + 88);

    if (source_count >
            (std::numeric_limits<
                std::uint32_t>::max)() ||
        source_count >
            (std::numeric_limits<
                std::size_t>::max)() ||
        root_count >
            (std::numeric_limits<
                std::size_t>::max)()) {
        return {status_code::artifact_corrupt};
    }

    const auto& source_core =
        candidate[
            section_index(
                source_manager_image_section::source_core)];
    const auto& physical =
        candidate[
            section_index(
                source_manager_image_section::physical_state)];
    const auto& graph =
        candidate[
            section_index(
                source_manager_image_section::graph_records)];
    const auto& forward_edges =
        candidate[
            section_index(
                source_manager_image_section::forward_edges)];
    const auto& reverse_edges =
        candidate[
            section_index(
                source_manager_image_section::reverse_edges)];
    const auto& roots =
        candidate[
            section_index(
                source_manager_image_section::roots)];
    const auto& path_index =
        candidate[
            section_index(
                source_manager_image_section::path_index)];
    const auto& source_file_identity_index =
        candidate[
            section_index(
                source_manager_image_section::
                    source_file_identity_index)];
    const auto&
        tracked_directory_identity_index =
        candidate[
            section_index(
                source_manager_image_section::
                    tracked_directory_identity_index)];

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

    const auto valid_optional_index =
        [](std::uint64_t count) noexcept {
            return count == 0 ||
                (count & (count - 1)) == 0;
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
          (source_count != 0 &&
           source_file_identity_index.count == 0))) ||
        !valid_optional_index(
            source_file_identity_index.count) ||
        !valid_optional_index(
            tracked_directory_identity_index.count)) {
        return {status_code::artifact_corrupt};
    }

    for (std::size_t index =
             header_reserved_begin;
         index <
             source_manager_image_header_size;
         ++index) {
        if (prefix[index] != std::byte{0})
            return {status_code::artifact_corrupt};
    }

    if (source_core.count != source_count ||
        physical.count != source_count ||
        graph.count != source_count ||
        forward_edges.count !=
            forward_edge_count ||
        reverse_edges.count !=
            reverse_edge_count ||
        roots.count != root_count ||
        path_index.count !=
            path_index_count ||
        path_index_count == 0 ||
        (path_index_count &
         (path_index_count - 1)) != 0) {
        return {status_code::artifact_corrupt};
    }

    bytes = {};
    prefix_bytes = prefix;
    logical_size_value =
        static_cast<std::size_t>(
            stored_file_size);

    std::copy(
        std::begin(candidate),
        std::end(candidate),
        std::begin(sections));

    generation_value =
        read_u64(prefix.data() + 48);
    source_count_value =
        static_cast<std::size_t>(
            source_count);
    root_count_value =
        static_cast<std::size_t>(
            root_count);
    change_checkpoint_value =
        change_checkpoint;

    return {};
}

std::span<const std::byte>
source_manager_image_view::section_bytes(
    source_manager_image_section kind) const noexcept {

    const auto& value =
        section(kind);

    if (value.data == nullptr ||
        value.record_size == 0 ||
        value.count >
            (std::numeric_limits<std::size_t>::max)() /
                value.record_size) {
        return {};
    }

    return {
        value.data,
        static_cast<std::size_t>(
            value.count) *
            value.record_size};
}

std::string_view source_manager_image_view::path(
    source_id source) const noexcept {

    if (!valid_source(source))
        return {};

    const auto index =
        static_cast<std::size_t>(
            source.value() - 1);

    const auto& core =
        section(
            source_manager_image_section::source_core);
    const auto& paths =
        section(
            source_manager_image_section::path_bytes);

    const auto* record =
        core.data +
        index * source_core_size;

    const auto offset =
        read_u32(record);
    const auto length =
        read_u32(record + 4);

    if (offset > paths.count ||
        length > paths.count - offset) {
        return {};
    }

    return {
        reinterpret_cast<const char*>(
            paths.data +
            static_cast<std::size_t>(offset)),
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

source_id_image_range source_manager_image_view::includes(
    source_id source) const noexcept {

    if (!valid_source(source))
        return {};

    const auto index =
        static_cast<std::size_t>(
            source.value() - 1);

    const auto& graph =
        section(
            source_manager_image_section::graph_records);
    const auto& edges =
        section(
            source_manager_image_section::forward_edges);

    const auto* record =
        graph.data +
        index * graph_record_size;

    const auto offset =
        read_u64(record);
    const auto count =
        read_u32(record + 8);

    if (read_u32(record + 12) != 0 ||
        offset > edges.count ||
        count > edges.count - offset) {
        return {};
    }

    return source_id_image_range{
        edges.data +
            static_cast<std::size_t>(offset) * 4,
        static_cast<std::size_t>(count),
    };
}

source_id_image_range source_manager_image_view::dependents(
    source_id source) const noexcept {

    if (!valid_source(source))
        return {};

    const auto index =
        static_cast<std::size_t>(
            source.value() - 1);

    const auto& graph =
        section(
            source_manager_image_section::graph_records);
    const auto& edges =
        section(
            source_manager_image_section::reverse_edges);

    const auto* record =
        graph.data +
        index * graph_record_size;

    const auto offset =
        read_u64(record + 16);
    const auto count =
        read_u32(record + 24);

    if (read_u32(record + 28) != 0 ||
        offset > edges.count ||
        count > edges.count - offset) {
        return {};
    }

    return source_id_image_range{
        edges.data +
            static_cast<std::size_t>(offset) * 4,
        static_cast<std::size_t>(count),
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

std::size_t
source_manager_image_view::source_file_identity_slot_count() const noexcept {
    return static_cast<std::size_t>(
        section(
            source_manager_image_section::
                source_file_identity_index).count);
}

status source_manager_image_view::source_file_identity_slot(
    std::size_t index,
    source_change_file_index_slot& output) const noexcept {

    output = {};

    const auto& values =
        section(
            source_manager_image_section::
                source_file_identity_index);

    if (index >= values.count)
        return {status_code::invalid_argument};

    const auto* slot =
        values.data +
        index * source_file_identity_index_record_size;

    output.file_reference = read_u64(slot);
    output.source = source_id{read_u32(slot + 8)};
    output.reserved = read_u32(slot + 12);

    if (output.file_reference == 0) {
        return !output.source && output.reserved == 0
            ? status{}
            : status{status_code::artifact_corrupt};
    }

    return valid_source(output.source) &&
        output.reserved == 0
        ? status{}
        : status{status_code::artifact_corrupt};
}

std::size_t
source_manager_image_view::tracked_directory_identity_slot_count()
    const noexcept {

    return static_cast<std::size_t>(
        section(
            source_manager_image_section::
                tracked_directory_identity_index).count);
}

status source_manager_image_view::tracked_directory_identity_slot(
    std::size_t index,
    source_change_directory_index_slot& output) const noexcept {

    output = {};

    const auto& values =
        section(
            source_manager_image_section::
                tracked_directory_identity_index);

    if (index >= values.count)
        return {status_code::invalid_argument};

    const auto* slot =
        values.data +
        index * tracked_directory_identity_index_record_size;

    output.file_reference = read_u64(slot);
    output.flags = read_u32(slot + 8);
    output.reserved = read_u32(slot + 12);

    if (output.file_reference == 0) {
        return output.flags == 0 &&
            output.reserved == 0
            ? status{}
            : status{status_code::artifact_corrupt};
    }

    return output.reserved == 0 &&
        output.flags != 0 &&
        (output.flags &
         ~source_change_directory_watch_known) == 0
        ? status{}
        : status{status_code::artifact_corrupt};
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

    for (std::size_t index = 0;
         index < source_manager_image_directory_count;
         ++index) {

        const auto& item = sections[index];
        std::uint64_t byte_count = 0;

        if (!multiply_u64(
                item.count,
                item.record_size,
                byte_count) ||
            byte_count >
                (std::numeric_limits<std::size_t>::max)()) {
            return {status_code::artifact_corrupt};
        }

        if (persistence_crc64(
                std::span<const std::byte>{
                    item.data,
                    static_cast<std::size_t>(
                        byte_count)}) != item.crc64) {
            return {status_code::artifact_corrupt};
        }
    }

    const auto& graph =
        section(
            source_manager_image_section::graph_records);
    const auto& forward_edges =
        section(
            source_manager_image_section::forward_edges);
    const auto& reverse_edges =
        section(
            source_manager_image_section::reverse_edges);

    for (std::size_t index = 0;
         index < source_count_value;
         ++index) {

        const auto source =
            source_id{
                static_cast<std::uint32_t>(
                    index + 1)};

        const auto* record =
            graph.data +
            index * graph_record_size;

        const auto forward_offset =
            read_u64(record);
        const auto forward_count =
            read_u32(record + 8);
        const auto reverse_offset =
            read_u64(record + 16);
        const auto reverse_count =
            read_u32(record + 24);
        const auto flags =
            read_u32(record + 32);

        if (read_u32(record + 12) != 0 ||
            read_u32(record + 28) != 0 ||
            (flags & ~graph_known_flags) != 0 ||
            read_u32(record + 36) != 0 ||
            forward_offset > forward_edges.count ||
            forward_count >
                forward_edges.count - forward_offset ||
            reverse_offset > reverse_edges.count ||
            reverse_count >
                reverse_edges.count - reverse_offset) {
            return {status_code::artifact_corrupt};
        }

        const auto source_path =
            path(source);

        if (source_path.empty())
            return {status_code::artifact_corrupt};

        const auto source_includes =
            includes(source);

        for (std::size_t edge = 0;
             edge < source_includes.size();
             ++edge) {
            if (!valid_source(
                    source_includes[edge])) {
                return {status_code::artifact_corrupt};
            }
        }

        const auto source_dependents =
            dependents(source);

        for (std::size_t edge = 0;
             edge < source_dependents.size();
             ++edge) {
            if (!valid_source(
                    source_dependents[edge])) {
                return {status_code::artifact_corrupt};
            }
        }

        source_id found;
        if (!find(source_path, found).ok() ||
            found != source) {
            return {status_code::artifact_corrupt};
        }
    }

    for (std::size_t index = 0;
         index < root_count_value;
         ++index) {

        source_manager_image_root root_value;
        if (!root(index, root_value).ok())
            return {status_code::artifact_corrupt};
    }

    return {};
}


namespace {

constexpr std::array<std::byte, 64> native_zero_padding{};

[[nodiscard]] std::span<const std::byte> byte_span(
    std::span<const source_record> values) noexcept {

    return std::as_bytes(values);
}

[[nodiscard]] std::span<const std::byte> byte_span(
    std::span<const source_generation_physical_record> values) noexcept {

    return std::as_bytes(values);
}

[[nodiscard]] std::span<const std::byte> byte_span(
    std::span<const source_generation_record> values) noexcept {

    return std::as_bytes(values);
}

[[nodiscard]] std::span<const std::byte> byte_span(
    std::span<const source_id> values) noexcept {

    return std::as_bytes(values);
}

[[nodiscard]] std::span<const std::byte> byte_span(
    std::span<const source_change_file_index_slot> values) noexcept {

    return std::as_bytes(values);
}

[[nodiscard]] std::span<const std::byte> byte_span(
    std::span<const source_change_directory_index_slot> values) noexcept {

    return std::as_bytes(values);
}

} // namespace

void source_manager_native_image_storage::reset() noexcept {
    prefix.fill(std::byte{0});
    roots.clear();
    native = {};
    file_identity_index = {};
    directory_identity_index = {};
    size_value = 0;
    valid_value = false;
}

project_generation_segment
source_manager_native_image_storage::segment() const noexcept {

    project_generation_segment output;

    if (!valid_value)
        return output;

    if (!output.append(prefix))
        return {};

    const std::array<std::span<const std::byte>, 10> sections{{
        byte_span(native.sources),
        byte_span(native.physical),
        byte_span(native.graph),
        byte_span(native.forward_edges),
        byte_span(native.reverse_edges),
        std::span<const std::byte>{roots.data(), roots.size()},
        native.path_index,
        native.path_bytes,
        byte_span(file_identity_index),
        byte_span(directory_identity_index),
    }};

    std::size_t cursor = prefix.size();

    for (std::size_t index = 0; index < sections.size(); ++index) {
        const auto aligned =
            (cursor + 63u) & ~std::size_t{63u};
        const auto padding = aligned - cursor;

        if (padding != 0 &&
            !output.append(
                std::span<const std::byte>{
                    native_zero_padding.data(),
                    padding})) {
            return {};
        }

        cursor = aligned;

        if (!output.append(sections[index]))
            return {};

        cursor += sections[index].size();
    }

    return output.size() == size_value
        ? output
        : project_generation_segment{};
}

void source_manager_sparse_image_storage::reset() noexcept {
    prefix = {};
    baseline_sections = {};
    baseline_offsets = {};
    patch_sources.clear();
    physical_patches.clear();
    file_identity_index.clear();
    directory_identity_index.clear();
    size_value = 0;
    valid_value = false;
}

project_generation_segment
source_manager_sparse_image_storage::segment() const noexcept {

    project_generation_segment output;

    if (!valid_value ||
        patch_sources.size() !=
            physical_patches.size()) {
        return output;
    }

    if (!output.append(prefix))
        return {};

    const auto physical_index =
        section_index(
            source_manager_image_section::
                physical_state);
    const auto file_identity_index_value =
        section_index(
            source_manager_image_section::
                source_file_identity_index);
    const auto directory_identity_index_value =
        section_index(
            source_manager_image_section::
                tracked_directory_identity_index);

    std::size_t logical_cursor =
        prefix.size();

    for (std::size_t section_number = 0;
         section_number <
            source_manager_image_directory_count;
         ++section_number) {

        const auto logical_offset =
            static_cast<std::size_t>(
                baseline_offsets[
                    section_number]);

        if (logical_offset <
            logical_cursor) {
            return {};
        }

        const auto padding =
            logical_offset -
            logical_cursor;

        if (padding != 0) {
            if (padding >
                    native_zero_padding.size() ||
                !output.append(
                    std::span<const std::byte>{
                        native_zero_padding.data(),
                        padding})) {
                return {};
            }
        }

        const auto baseline_section =
            baseline_sections[
                section_number];

        if (section_number ==
            physical_index) {

            std::size_t cursor = 0;

            for (std::size_t index = 0;
                 index <
                    patch_sources.size();
                 ++index) {

                const auto source =
                    patch_sources[index];

                if (!source)
                    return {};

                const auto patch_offset =
                    static_cast<std::size_t>(
                        source.value() - 1) *
                    sizeof(
                        source_generation_physical_record);

                if (patch_offset < cursor ||
                    patch_offset >
                        baseline_section.size() ||
                    sizeof(
                        source_generation_physical_record) >
                        baseline_section.size() -
                            patch_offset) {
                    return {};
                }

                if (!output.append(
                        baseline_section.subspan(
                            cursor,
                            patch_offset -
                                cursor))) {
                    return {};
                }

                const auto patch_bytes =
                    std::as_bytes(
                        std::span<
                            const source_generation_physical_record>{
                            &physical_patches[index],
                            1});

                if (!output.append(
                        patch_bytes)) {
                    return {};
                }

                cursor =
                    patch_offset +
                    patch_bytes.size();
            }

            if (cursor >
                    baseline_section.size() ||
                !output.append(
                    baseline_section.subspan(
                        cursor))) {
                return {};
            }
        }
        else if (section_number ==
                 file_identity_index_value) {

            const auto bytes_value =
                std::as_bytes(
                    std::span<
                        const source_change_file_index_slot>{
                        file_identity_index});

            if (bytes_value.size() !=
                    baseline_section.size() ||
                !output.append(
                    bytes_value)) {
                return {};
            }
        }
        else if (section_number ==
                 directory_identity_index_value) {

            const auto bytes_value =
                std::as_bytes(
                    std::span<
                        const source_change_directory_index_slot>{
                        directory_identity_index});

            if (bytes_value.size() !=
                    baseline_section.size() ||
                !output.append(
                    bytes_value)) {
                return {};
            }
        }
        else {
            if (!output.append(
                    baseline_section)) {
                return {};
            }
        }

        logical_cursor =
            logical_offset +
            baseline_section.size();
    }

    return output.size() == size_value
        ? output
        : project_generation_segment{};
}

status freeze_source_manager_native_image(
    const source_manager& manager,
    const source_manager_image_options& options,
    source_manager_native_image_storage& output,
    source_manager_freeze_telemetry* telemetry) noexcept {

    output.reset();
    if (telemetry != nullptr) {
        *telemetry = {};
        telemetry->mode = 1;
    }

    const auto internal_begin =
        source_manager_freeze_clock::now();
    const auto preflight_begin =
        source_manager_freeze_clock::now();

    if constexpr (std::endian::native != std::endian::little)
        return {status_code::not_available};

    const auto native =
        manager.native_generation();

    if (!native.complete)
        return {status_code::invalid_state};

    const auto source_count =
        manager.source_count();

    if (source_count >
            (std::numeric_limits<std::uint32_t>::max)() ||
        native.sources.size() != source_count ||
        native.physical.size() != source_count ||
        native.graph.size() != source_count ||
        native.path_index.empty() ||
        native.path_index.size() %
            path_index_record_size != 0 ||
        native.path_bytes.size() >
            (std::numeric_limits<std::uint32_t>::max)()) {
        return {status_code::artifact_corrupt};
    }

    for (const auto& root : options.roots) {
        if (!root.source ||
            static_cast<std::size_t>(
                root.source.value()) > source_count ||
            !valid_role(
                static_cast<std::uint8_t>(
                    root.role))) {
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
          (source_count != 0 &&
           options.file_identity_index.empty()))) ||
        (!options.file_identity_index.empty() &&
         (options.file_identity_index.size() &
          (options.file_identity_index.size() - 1)) != 0) ||
        (!options.directory_identity_index.empty() &&
         (options.directory_identity_index.size() &
          (options.directory_identity_index.size() - 1)) != 0)) {
        return {status_code::invalid_argument};
    }

    if (telemetry != nullptr) {
        telemetry->preflight_ns =
            source_manager_elapsed_ns(preflight_begin);
    }

    // GEN-02C9: native.graph and native.physical are fresh Generation-owned
    // publication records. source_generation_storage constructs their reserved
    // fields, flags, and arena ranges; rescanning all Sources here duplicates
    // that publication contract. Persisted/mapped input keeps its cold verifier.

    const auto roots_begin =
        source_manager_freeze_clock::now();

    try {
        output.roots.assign(
            options.roots.size() * root_record_size,
            std::byte{0});
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }

    for (std::size_t index = 0;
         index < options.roots.size();
         ++index) {

        const auto& root =
            options.roots[index];
        auto* record =
            output.roots.data() +
            index * root_record_size;

        write_u32(
            record,
            root.source.value());

        record[4] =
            static_cast<std::byte>(
                static_cast<std::uint8_t>(
                    root.role));
    }

    if (telemetry != nullptr) {
        telemetry->roots_ns =
            source_manager_elapsed_ns(roots_begin);
    }

    const auto layout_begin =
        source_manager_freeze_clock::now();

    output.native = native;
    output.file_identity_index =
        options.file_identity_index;
    output.directory_identity_index =
        options.directory_identity_index;

    const std::array<std::span<const std::byte>, 10> sections{{
        byte_span(native.sources),
        byte_span(native.physical),
        byte_span(native.graph),
        byte_span(native.forward_edges),
        byte_span(native.reverse_edges),
        std::span<const std::byte>{
            output.roots.data(),
            output.roots.size()},
        native.path_index,
        native.path_bytes,
        byte_span(options.file_identity_index),
        byte_span(options.directory_identity_index),
    }};

    const std::array<std::uint32_t, 10> record_sizes{{
        source_core_size,
        physical_state_size,
        graph_record_size,
        4,
        4,
        root_record_size,
        path_index_record_size,
        1,
        source_file_identity_index_record_size,
        tracked_directory_identity_index_record_size,
    }};

    const std::array<source_manager_image_section, 10> kinds{{
        source_manager_image_section::source_core,
        source_manager_image_section::physical_state,
        source_manager_image_section::graph_records,
        source_manager_image_section::forward_edges,
        source_manager_image_section::reverse_edges,
        source_manager_image_section::roots,
        source_manager_image_section::path_index,
        source_manager_image_section::path_bytes,
        source_manager_image_section::source_file_identity_index,
        source_manager_image_section::tracked_directory_identity_index,
    }};

    std::array<layout_section, 10> layout{};

    std::uint64_t cursor =
        source_manager_image_prefix_size;

    for (std::size_t index = 0;
         index < sections.size();
         ++index) {

        cursor = align64(cursor);

        const auto record_size =
            record_sizes[index];

        if (record_size == 0 ||
            sections[index].size() %
                record_size != 0) {
            output.reset();
            return {status_code::artifact_corrupt};
        }

        layout[index].kind =
            kinds[index];
        layout[index].record_size =
            record_size;
        layout[index].count =
            sections[index].size() /
            record_size;
        layout[index].offset =
            cursor;
        layout[index].crc64 = 0;

        if (!add_u64(
                cursor,
                sections[index].size(),
                cursor)) {
            output.reset();
            return {status_code::not_available};
        }
    }

    if (cursor >
        (std::numeric_limits<std::size_t>::max)()) {
        output.reset();
        return {status_code::not_available};
    }

    if (layout[
            section_index(
                source_manager_image_section::source_core)]
                .count != source_count ||
        layout[
            section_index(
                source_manager_image_section::physical_state)]
                .count != source_count ||
        layout[
            section_index(
                source_manager_image_section::graph_records)]
                .count != source_count ||
        layout[
            section_index(
                source_manager_image_section::path_index)]
                .count == 0 ||
        (layout[
            section_index(
                source_manager_image_section::path_index)]
                .count &
         (layout[
            section_index(
                source_manager_image_section::path_index)]
                .count - 1)) != 0) {
        output.reset();
        return {status_code::artifact_corrupt};
    }

    if (telemetry != nullptr) {
        telemetry->layout_ns =
            source_manager_elapsed_ns(layout_begin);
    }

    std::array<std::uint64_t, 10>
        crc_section_elapsed{};

    // GEN-02C9: Source Manager sections are immutable and independent.
    // Compute their CRCs concurrently; the persisted CRC contract is unchanged.
    const auto crc_wall_begin =
        source_manager_freeze_clock::now();

    const auto crc_worker_count =
        (std::min)(
            sections.size(),
            (std::max)(
                std::size_t{1},
                static_cast<std::size_t>(
                    std::thread::hardware_concurrency())));

    std::atomic<std::size_t> next_crc_section{0};

    const auto crc_worker = [&]() noexcept {
        for (;;) {
            const auto index =
                next_crc_section.fetch_add(
                    1,
                    std::memory_order_relaxed);

            if (index >= sections.size())
                return;

            const auto section_begin =
                source_manager_freeze_clock::now();

            layout[index].crc64 =
                persistence_crc64(
                    sections[index]);

            crc_section_elapsed[index] =
                source_manager_elapsed_ns(
                    section_begin);
        }
    };

    if (crc_worker_count == 1) {
        crc_worker();
    }
    else {
        try {
            std::vector<std::jthread> crc_workers;
            crc_workers.reserve(crc_worker_count);

            for (std::size_t worker = 0;
                 worker < crc_worker_count;
                 ++worker) {

                crc_workers.emplace_back(
                    [&]() noexcept {
                        crc_worker();
                    });
            }
        }
        catch (const std::bad_alloc&) {
            output.reset();
            return {status_code::not_available};
        }
        catch (const std::length_error&) {
            output.reset();
            return {status_code::not_available};
        }
        catch (const std::system_error&) {
            output.reset();
            return {status_code::not_available};
        }
    }

    if (telemetry != nullptr) {
        telemetry->crc_wall_ns =
            source_manager_elapsed_ns(crc_wall_begin);
        telemetry->crc_worker_count =
            static_cast<std::uint32_t>(
                crc_worker_count);
    }

    publish_source_manager_crc_sections(
        telemetry,
        sections,
        crc_section_elapsed);

    const auto prefix_begin =
        source_manager_freeze_clock::now();

    auto* base =
        output.prefix.data();

    std::copy(
        image_magic.begin(),
        image_magic.end(),
        base);

    write_u32(
        base + 8,
        source_manager_image_format_version);
    write_u32(
        base + 12,
        endian_marker);
    write_u32(
        base + 16,
        source_manager_image_header_size);
    write_u32(
        base + 20,
        source_manager_image_directory_count);
    write_u32(
        base + 24,
        source_manager_image_directory_entry_size);
    write_u32(
        base + 28,
        0);

    write_u64(
        base + 32,
        directory_offset);
    write_u64(
        base + 40,
        cursor);
    write_u64(
        base + 48,
        options.generation);
    write_u64(
        base + 56,
        source_count);
    write_u64(
        base + 64,
        options.roots.size());
    write_u64(
        base + 72,
        native.path_index.size() /
            path_index_record_size);
    write_u64(
        base + 80,
        native.forward_edges.size());
    write_u64(
        base + 88,
        native.reverse_edges.size());

    write_u32(
        base + header_change_backend_offset,
        static_cast<std::uint32_t>(
            options.change_checkpoint.backend));
    write_u32(
        base + header_change_backend_offset + 4,
        0);

    write_u64(
        base + header_change_volume_offset,
        options.change_checkpoint.volume_serial);
    write_u64(
        base + header_change_journal_offset,
        options.change_checkpoint.journal_id);
    write_u64(
        base + header_change_usn_offset,
        static_cast<std::uint64_t>(
            options.change_checkpoint.next_usn));

    for (std::size_t index = 0;
         index < layout.size();
         ++index) {

        const auto& item =
            layout[index];

        auto* entry =
            base +
            directory_offset +
            index *
                source_manager_image_directory_entry_size;

        write_u32(
            entry,
            static_cast<std::uint32_t>(
                item.kind));
        write_u32(
            entry + 4,
            item.record_size);
        write_u64(
            entry + 8,
            item.offset);
        write_u64(
            entry + 16,
            item.count);
        write_u64(
            entry + 24,
            item.crc64);
    }

    if (telemetry != nullptr) {
        telemetry->prefix_directory_encode_ns =
            source_manager_elapsed_ns(prefix_begin);
    }

    const auto directory_crc_begin =
        source_manager_freeze_clock::now();

    const auto directory_crc =
        persistence_crc64(
            std::span<const std::byte>{
                base + directory_offset,
                directory_bytes});

    write_u64(
        base + header_directory_crc_offset,
        directory_crc);

    if (telemetry != nullptr) {
        telemetry->directory_crc_ns =
            source_manager_elapsed_ns(
                directory_crc_begin);
    }

    const auto header_crc_begin =
        source_manager_freeze_clock::now();

    std::array<
        std::byte,
        source_manager_image_header_size>
        header{};

    std::memcpy(
        header.data(),
        base,
        header.size());

    write_u64(
        header.data() + header_crc_offset,
        0);

    write_u64(
        base + header_crc_offset,
        persistence_crc64(header));

    if (telemetry != nullptr) {
        telemetry->header_crc_ns =
            source_manager_elapsed_ns(
                header_crc_begin);
    }

    output.size_value =
        static_cast<std::size_t>(cursor);
    output.valid_value = true;

    const auto segment_validate_begin =
        source_manager_freeze_clock::now();

    const auto segment =
        output.segment();

    if (segment.empty() ||
        segment.size() != output.size_value) {
        output.reset();
        return {status_code::not_available};
    }

    if (telemetry != nullptr) {
        telemetry->segment_validate_ns =
            source_manager_elapsed_ns(
                segment_validate_begin);
        telemetry->extent_count =
            static_cast<std::uint32_t>(
                segment.extent_count());
        telemetry->internal_ns =
            source_manager_elapsed_ns(
                internal_begin);
    }

    return {};
}

status freeze_source_manager_sparse_baseline_image(
    const source_manager& manager,
    const source_manager_image_view& baseline,
    const source_manager_image_options& options,
    std::span<const source_change_file_identity_update> physical_updates,
    bool roots_baseline_proven,
    source_manager_sparse_image_storage& output,
    source_manager_freeze_telemetry* telemetry) noexcept {

    output.reset();

    if (telemetry != nullptr) {
        *telemetry = {};
        // D4D mode 3 = sparse baseline scatter/gather.
        telemetry->mode = 3;
        telemetry->crc_worker_count = 1;
    }

    const auto internal_begin =
        source_manager_freeze_clock::now();
    const auto preflight_begin =
        source_manager_freeze_clock::now();

    if constexpr (std::endian::native != std::endian::little) {
        if (telemetry != nullptr)
            telemetry->sparse_fallback_reason = 1;
        return {status_code::not_found};
    }

    if (!baseline.valid() ||
        manager.baseline_source_image() != &baseline) {
        if (telemetry != nullptr)
            telemetry->sparse_fallback_reason = 2;
        return {status_code::not_found};
    }

    if (!roots_baseline_proven) {
        if (telemetry != nullptr)
            telemetry->sparse_fallback_reason = 3;
        return {status_code::not_found};
    }

    if (manager.source_count() != baseline.source_count()) {
        if (telemetry != nullptr)
            telemetry->sparse_fallback_reason = 4;
        return {status_code::not_found};
    }

    if (options.roots.size() != baseline.root_count()) {
        if (telemetry != nullptr)
            telemetry->sparse_fallback_reason = 5;
        return {status_code::not_found};
    }

    // Incremental BUILD may own a freshly published root vector even when the
    // root set is byte-for-byte identical to the pinned baseline. Reuse the
    // baseline roots section only after proving every identity/role pair equal.
    for (std::size_t index = 0;
         index < options.roots.size();
         ++index) {

        source_manager_image_root baseline_root;
        const auto root_result =
            baseline.root(
                index,
                baseline_root);

        if (!root_result.ok()) {
            output.reset();
            return root_result;
        }

        const auto& current_root =
            options.roots[index];

        if (current_root.source !=
                baseline_root.source ||
            current_root.role !=
                baseline_root.role) {
            if (telemetry != nullptr)
                telemetry->sparse_fallback_reason = 6;
            output.reset();
            return {status_code::not_found};
        }
    }

    // A baseline-backed sparse Source Manager cannot reuse path/topology sections
    // if Source identity count changed.
    const auto source_count =
        manager.source_count();

    if (source_count == 0)
        return {status_code::not_found};

    const auto& physical =
        baseline.section(
            source_manager_image_section::physical_state);
    const auto& file_identity =
        baseline.section(
            source_manager_image_section::source_file_identity_index);
    const auto& directory_identity =
        baseline.section(
            source_manager_image_section::tracked_directory_identity_index);

    if (physical.data == nullptr ||
        physical.record_size !=
            sizeof(source_generation_physical_record) ||
        physical.count != source_count ||
        file_identity.data == nullptr ||
        file_identity.record_size !=
            sizeof(source_change_file_index_slot) ||
        directory_identity.data == nullptr ||
        directory_identity.record_size !=
            sizeof(source_change_directory_index_slot)) {
        if (telemetry != nullptr)
            telemetry->sparse_fallback_reason = 7;
        return {status_code::not_found};
    }

    const auto file_bytes =
        std::as_bytes(
            options.file_identity_index);
    const auto directory_bytes_value =
        std::as_bytes(
            options.directory_identity_index);

    const auto baseline_file_bytes =
        static_cast<std::size_t>(
            file_identity.count) *
            file_identity.record_size;
    const auto baseline_directory_bytes =
        static_cast<std::size_t>(
            directory_identity.count) *
            directory_identity.record_size;

    if (file_bytes.size() !=
            baseline_file_bytes ||
        directory_bytes_value.size() !=
            baseline_directory_bytes) {
        if (telemetry != nullptr)
            telemetry->sparse_fallback_reason = 8;
        return {status_code::not_found};
    }

    const auto physical_offset_value =
        static_cast<std::size_t>(
            physical.offset);
    const auto file_identity_offset_value =
        static_cast<std::size_t>(
            file_identity.offset);
    const auto directory_identity_offset_value =
        static_cast<std::size_t>(
            directory_identity.offset);

    if (baseline.prefix_bytes.size() !=
            source_manager_image_prefix_size ||
        baseline.logical_size_value <
            source_manager_image_prefix_size ||
        physical_offset_value <
            source_manager_image_prefix_size ||
        file_identity_offset_value <
            physical_offset_value +
                static_cast<std::size_t>(
                    physical.count) *
                    physical.record_size ||
        directory_identity_offset_value <
            file_identity_offset_value +
                baseline_file_bytes) {
        if (telemetry != nullptr)
            telemetry->sparse_fallback_reason = 9;
        return {status_code::not_found};
    }

    try {
        output.patch_sources.reserve(
            physical_updates.size());
        output.physical_patches.reserve(
            physical_updates.size());

        // Order by persisted record position. This does not assign or canonicalize
        // Source identity; it only makes scatter/gather extents monotonic.
        for (const auto& update : physical_updates) {
            if (!update.source ||
                static_cast<std::size_t>(
                    update.source.value()) >
                    source_count) {
                output.reset();
                return {status_code::invalid_argument};
            }

            const auto position =
                std::lower_bound(
                    output.patch_sources.begin(),
                    output.patch_sources.end(),
                    update.source,
                    [](source_id left, source_id right) noexcept {
                        return left.value() < right.value();
                    });

            if (position !=
                    output.patch_sources.end() &&
                position->value() ==
                    update.source.value()) {
                output.reset();
                return {status_code::invalid_argument};
            }

            output.patch_sources.insert(
                position,
                update.source);
        }

        // Count the exact extents segment() will emit. Empty baseline gaps are
        // not extents, so adjacent physical patches consume one extent each
        // instead of the conservative two-extents-per-patch bound.
        std::size_t required_extent_count = 1;
        std::size_t extent_cursor =
            source_manager_image_prefix_size;

        for (const auto source :
             output.patch_sources) {

            const auto patch_offset =
                physical_offset_value +
                static_cast<std::size_t>(
                    source.value() - 1) *
                    sizeof(
                        source_generation_physical_record);

            if (patch_offset > extent_cursor)
                ++required_extent_count;

            ++required_extent_count;
            extent_cursor =
                patch_offset +
                sizeof(
                    source_generation_physical_record);
        }

        if (file_identity_offset_value >
            extent_cursor) {
            ++required_extent_count;
        }

        if (!file_bytes.empty())
            ++required_extent_count;

        extent_cursor =
            file_identity_offset_value +
            file_bytes.size();

        if (directory_identity_offset_value >
            extent_cursor) {
            ++required_extent_count;
        }

        if (!directory_bytes_value.empty())
            ++required_extent_count;

        extent_cursor =
            directory_identity_offset_value +
            directory_bytes_value.size();

        if (baseline.logical_size_value >
            extent_cursor) {
            ++required_extent_count;
        }

        if (required_extent_count >
            source_manager_sparse_extent_budget) {
            if (telemetry != nullptr)
                telemetry->sparse_fallback_reason = 10;
            output.reset();
            return {status_code::not_found};
        }

        if (telemetry != nullptr) {
            telemetry->extent_count =
                static_cast<std::uint32_t>(
                    required_extent_count);
        }

        // Only include topology can change Source Manager graph topology.
        // Every changed physical Source is proven against the baseline before
        // byte-identical graph/edge sections are borrowed.
        for (const auto source :
             output.patch_sources) {

            const auto baseline_includes =
                baseline.includes(source);
            const auto current_count =
                manager.include_count(source);

            if (current_count !=
                baseline_includes.size()) {
                if (telemetry != nullptr)
                    telemetry->sparse_fallback_reason = 11;
                output.reset();
                return {status_code::not_found};
            }

            for (std::size_t index = 0;
                 index < current_count;
                 ++index) {
                if (manager.include_at(
                        source,
                        index) !=
                    baseline_includes[index]) {
                    if (telemetry != nullptr)
                        telemetry->sparse_fallback_reason = 12;
                    output.reset();
                    return {status_code::not_found};
                }
            }
        }

        output.physical_patches.resize(
            output.patch_sources.size());

        for (std::size_t index = 0;
             index < output.patch_sources.size();
             ++index) {

            auto& record =
                output.physical_patches[index];
            record = {};

            const auto snapshot =
                manager.current(
                    output.patch_sources[index]);

            if (!snapshot)
                continue;

            const auto observation =
                snapshot.observation();

            if (observation.size >
                (std::numeric_limits<std::uint64_t>::max)()) {
                output.reset();
                return {status_code::not_available};
            }

            record.flags =
                source_generation_physical_present;
            record.write_time_ticks =
                observation.write_time_ticks;
            record.size =
                static_cast<std::uint64_t>(
                    observation.size);
            record.hash = snapshot.hash();
        }
    }
    catch (const std::bad_alloc&) {
        output.reset();
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        output.reset();
        return {status_code::not_available};
    }

    if (telemetry != nullptr) {
        telemetry->preflight_ns =
            source_manager_elapsed_ns(
                preflight_begin);
    }

    const auto crc_wall_begin =
        source_manager_freeze_clock::now();

    std::uint64_t physical_crc =
        physical.crc64;

    if (!output.patch_sources.empty()) {
        const auto physical_bytes =
            std::span<const std::byte>{
                physical.data,
                static_cast<std::size_t>(
                    physical.count) *
                    physical.record_size};

        const auto physical_crc_begin =
            source_manager_freeze_clock::now();

        physical_crc = 0;
        std::size_t cursor = 0;

        for (std::size_t index = 0;
             index < output.patch_sources.size();
             ++index) {

            const auto patch_offset =
                static_cast<std::size_t>(
                    output.patch_sources[index].
                        value() - 1) *
                sizeof(
                    source_generation_physical_record);

            physical_crc =
                persistence_crc64_update(
                    physical_crc,
                    physical_bytes.subspan(
                        cursor,
                        patch_offset - cursor));

            physical_crc =
                persistence_crc64_update(
                    physical_crc,
                    std::as_bytes(
                        std::span<
                            const source_generation_physical_record>{
                            &output.physical_patches[index],
                            1}));

            cursor =
                patch_offset +
                sizeof(
                    source_generation_physical_record);
        }

        physical_crc =
            persistence_crc64_update(
                physical_crc,
                physical_bytes.subspan(cursor));

        if (telemetry != nullptr) {
            telemetry->crc_physical_state_ns =
                source_manager_elapsed_ns(
                    physical_crc_begin);
            telemetry->crc_physical_state_bytes =
                physical_bytes.size();
        }
    }

    const auto file_crc_begin =
        source_manager_freeze_clock::now();
    const auto file_crc =
        persistence_crc64(file_bytes);

    if (telemetry != nullptr) {
        telemetry->crc_file_identity_ns =
            source_manager_elapsed_ns(
                file_crc_begin);
        telemetry->crc_file_identity_bytes =
            file_bytes.size();
    }

    const auto directory_crc_section_begin =
        source_manager_freeze_clock::now();
    const auto directory_section_crc =
        persistence_crc64(
            directory_bytes_value);

    if (telemetry != nullptr) {
        telemetry->crc_directory_identity_ns =
            source_manager_elapsed_ns(
                directory_crc_section_begin);
        telemetry->crc_directory_identity_bytes =
            directory_bytes_value.size();
        telemetry->crc_total_bytes =
            telemetry->crc_physical_state_bytes +
            telemetry->crc_file_identity_bytes +
            telemetry->crc_directory_identity_bytes;
        telemetry->crc_wall_ns =
            source_manager_elapsed_ns(
                crc_wall_begin);
    }

    const auto prefix_begin =
        source_manager_freeze_clock::now();

    std::memcpy(
        output.prefix.data(),
        baseline.prefix_bytes.data(),
        output.prefix.size());

    auto* base =
        output.prefix.data();

    write_u64(
        base + 48,
        options.generation);
    write_u64(
        base + 56,
        source_count);
    write_u64(
        base + 64,
        options.roots.size());

    write_u32(
        base + header_change_backend_offset,
        static_cast<std::uint32_t>(
            options.change_checkpoint.backend));
    write_u32(
        base + header_change_backend_offset + 4,
        0);
    write_u64(
        base + header_change_volume_offset,
        options.change_checkpoint.volume_serial);
    write_u64(
        base + header_change_journal_offset,
        options.change_checkpoint.journal_id);
    write_u64(
        base + header_change_usn_offset,
        static_cast<std::uint64_t>(
            options.change_checkpoint.next_usn));

    const auto patch_section_crc =
        [&](source_manager_image_section kind,
            std::uint64_t crc) noexcept {

            const auto index =
                section_index(kind);
            auto* entry =
                base +
                directory_offset +
                index *
                    source_manager_image_directory_entry_size;
            write_u64(
                entry + 24,
                crc);
        };

    patch_section_crc(
        source_manager_image_section::physical_state,
        physical_crc);
    patch_section_crc(
        source_manager_image_section::source_file_identity_index,
        file_crc);
    patch_section_crc(
        source_manager_image_section::tracked_directory_identity_index,
        directory_section_crc);

    if (telemetry != nullptr) {
        telemetry->prefix_directory_encode_ns =
            source_manager_elapsed_ns(
                prefix_begin);
    }

    const auto directory_crc_begin =
        source_manager_freeze_clock::now();

    const auto directory_crc =
        persistence_crc64(
            std::span<const std::byte>{
                base + directory_offset,
                directory_bytes});

    write_u64(
        base + header_directory_crc_offset,
        directory_crc);

    if (telemetry != nullptr) {
        telemetry->directory_crc_ns =
            source_manager_elapsed_ns(
                directory_crc_begin);
    }

    const auto header_crc_begin =
        source_manager_freeze_clock::now();

    std::array<
        std::byte,
        source_manager_image_header_size>
        header{};

    std::memcpy(
        header.data(),
        base,
        header.size());

    write_u64(
        header.data() + header_crc_offset,
        0);

    write_u64(
        base + header_crc_offset,
        persistence_crc64(header));

    if (telemetry != nullptr) {
        telemetry->header_crc_ns =
            source_manager_elapsed_ns(
                header_crc_begin);
    }

    const auto identity_copy_begin =
        source_manager_freeze_clock::now();

    try {
        output.file_identity_index.assign(
            options.file_identity_index.begin(),
            options.file_identity_index.end());
        output.directory_identity_index.assign(
            options.directory_identity_index.begin(),
            options.directory_identity_index.end());
    }
    catch (const std::bad_alloc&) {
        output.reset();
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        output.reset();
        return {status_code::not_available};
    }

    if (telemetry != nullptr) {
        telemetry->identity_copy_ns =
            source_manager_elapsed_ns(
                identity_copy_begin);
    }

    for (std::size_t index = 0;
         index <
            source_manager_image_directory_count;
         ++index) {

        const auto& item =
            baseline.sections[index];

        std::uint64_t byte_count = 0;
        if (!multiply_u64(
                item.count,
                item.record_size,
                byte_count) ||
            byte_count >
                (std::numeric_limits<
                    std::size_t>::max)()) {
            output.reset();
            return {status_code::not_found};
        }

        output.baseline_sections[index] =
            std::span<const std::byte>{
                item.data,
                static_cast<std::size_t>(
                    byte_count)};
        output.baseline_offsets[index] =
            item.offset;
    }

    output.size_value =
        baseline.logical_size_value;
    output.valid_value = true;

    const auto segment_begin =
        source_manager_freeze_clock::now();

    const auto segment =
        output.segment();

    if (telemetry != nullptr) {
        telemetry->segment_validate_ns =
            source_manager_elapsed_ns(
                segment_begin);
    }

    if (segment.empty() ||
        segment.size() != output.size_value) {
        if (telemetry != nullptr)
            telemetry->sparse_fallback_reason = 13;
        output.reset();
        return {status_code::not_found};
    }

    if (telemetry != nullptr) {
        telemetry->internal_ns =
            source_manager_elapsed_ns(
                internal_begin);
    }

    return {};
}

status encode_source_manager_image(
    const source_manager& manager,
    const source_manager_image_options& options,
    std::vector<std::byte>& output,
    source_manager_freeze_telemetry* telemetry) noexcept {

    output.clear();
    if (telemetry != nullptr) {
        *telemetry = {};
        telemetry->mode = 2;
        telemetry->crc_worker_count = 1;
    }

    const auto internal_begin =
        source_manager_freeze_clock::now();
    const auto preflight_begin =
        source_manager_freeze_clock::now();

    const auto source_count =
        manager.source_count();

    if (source_count >
        (std::numeric_limits<std::uint32_t>::max)()) {
        return {status_code::not_available};
    }

    std::uint64_t path_bytes = 0;
    std::uint64_t forward_edges = 0;
    std::uint64_t reverse_edges = 0;

    for (std::size_t index = 0;
         index < source_count;
         ++index) {

        const auto source =
            source_id{
                static_cast<std::uint32_t>(
                    index + 1)};

        const auto source_path =
            manager.path(source);

        if (source_path.empty())
            return {status_code::artifact_corrupt};

        if (source_path.size() >
            (std::numeric_limits<std::uint32_t>::max)()) {
            return {status_code::not_available};
        }

        const auto include_count =
            manager.include_count(source);
        const auto dependent_count =
            manager.dependent_count(source);

        if (include_count >
                (std::numeric_limits<std::uint32_t>::max)() ||
            dependent_count >
                (std::numeric_limits<std::uint32_t>::max)() ||
            !add_u64(
                path_bytes,
                source_path.size(),
                path_bytes) ||
            !add_u64(
                forward_edges,
                include_count,
                forward_edges) ||
            !add_u64(
                reverse_edges,
                dependent_count,
                reverse_edges)) {
            return {status_code::not_available};
        }
    }

    if (path_bytes >
        (std::numeric_limits<std::uint32_t>::max)()) {
        return {status_code::not_available};
    }

    for (const auto& root : options.roots) {
        if (!root.source ||
            static_cast<std::size_t>(
                root.source.value()) > source_count ||
            !valid_role(
                static_cast<std::uint8_t>(
                    root.role))) {
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
          (source_count != 0 &&
           options.file_identity_index.empty()))) ||
        (!options.file_identity_index.empty() &&
         (options.file_identity_index.size() &
          (options.file_identity_index.size() - 1)) != 0) ||
        (!options.directory_identity_index.empty() &&
         (options.directory_identity_index.size() &
          (options.directory_identity_index.size() - 1)) != 0)) {
        return {status_code::invalid_argument};
    }

    const auto index_capacity =
        path_index_capacity(source_count);

    if (index_capacity == 0)
        return {status_code::not_available};

    if (telemetry != nullptr) {
        telemetry->preflight_ns =
            source_manager_elapsed_ns(preflight_begin);
    }

    const auto layout_begin =
        source_manager_freeze_clock::now();

    std::array<
        layout_section,
        source_manager_image_directory_count> layout{{
        {
            source_manager_image_section::source_core,
            source_core_size,
            source_count},
        {
            source_manager_image_section::physical_state,
            physical_state_size,
            source_count},
        {
            source_manager_image_section::graph_records,
            graph_record_size,
            source_count},
        {
            source_manager_image_section::forward_edges,
            4,
            forward_edges},
        {
            source_manager_image_section::reverse_edges,
            4,
            reverse_edges},
        {
            source_manager_image_section::roots,
            root_record_size,
            options.roots.size()},
        {
            source_manager_image_section::path_index,
            path_index_record_size,
            index_capacity},
        {
            source_manager_image_section::path_bytes,
            1,
            path_bytes},
        {
            source_manager_image_section::
                source_file_identity_index,
            source_file_identity_index_record_size,
            options.file_identity_index.size()},
        {
            source_manager_image_section::
                tracked_directory_identity_index,
            tracked_directory_identity_index_record_size,
            options.directory_identity_index.size()},
    }};

    std::uint64_t cursor =
        first_section_offset;

    for (auto& item : layout) {
        cursor = align64(cursor);
        item.offset = cursor;

        std::uint64_t bytes = 0;

        if (!multiply_u64(
                item.count,
                item.record_size,
                bytes) ||
            !add_u64(
                cursor,
                bytes,
                cursor)) {
            return {status_code::not_available};
        }
    }

    if (cursor >
        (std::numeric_limits<std::size_t>::max)()) {
        return {status_code::not_available};
    }

    if (telemetry != nullptr) {
        telemetry->layout_ns =
            source_manager_elapsed_ns(layout_begin);
    }

    const auto allocate_begin =
        source_manager_freeze_clock::now();

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

    if (telemetry != nullptr) {
        telemetry->allocate_zero_ns =
            source_manager_elapsed_ns(
                allocate_begin);
    }

    auto* base = output.data();

    std::uint32_t path_cursor = 0;
    std::uint64_t forward_cursor = 0;
    std::uint64_t reverse_cursor = 0;

    auto* source_core =
        base +
        static_cast<std::size_t>(
            layout[
                section_index(
                    source_manager_image_section::
                        source_core)]
                .offset);

    auto* physical =
        base +
        static_cast<std::size_t>(
            layout[
                section_index(
                    source_manager_image_section::
                        physical_state)]
                .offset);

    auto* graph =
        base +
        static_cast<std::size_t>(
            layout[
                section_index(
                    source_manager_image_section::
                        graph_records)]
                .offset);

    auto* forward_edge_data =
        base +
        static_cast<std::size_t>(
            layout[
                section_index(
                    source_manager_image_section::
                        forward_edges)]
                .offset);

    auto* reverse_edge_data =
        base +
        static_cast<std::size_t>(
            layout[
                section_index(
                    source_manager_image_section::
                        reverse_edges)]
                .offset);

    auto* root_data =
        base +
        static_cast<std::size_t>(
            layout[
                section_index(
                    source_manager_image_section::
                        roots)]
                .offset);

    auto* path_index =
        base +
        static_cast<std::size_t>(
            layout[
                section_index(
                    source_manager_image_section::
                        path_index)]
                .offset);

    auto* path_data =
        base +
        static_cast<std::size_t>(
            layout[
                section_index(
                    source_manager_image_section::
                        path_bytes)]
                .offset);

    auto* file_identity_data =
        base +
        static_cast<std::size_t>(
            layout[
                section_index(
                    source_manager_image_section::
                        source_file_identity_index)]
                .offset);

    auto* directory_identity_data =
        base +
        static_cast<std::size_t>(
            layout[
                section_index(
                    source_manager_image_section::
                        tracked_directory_identity_index)]
                .offset);

    const auto source_records_begin =
        source_manager_freeze_clock::now();

    for (std::size_t index = 0;
         index < source_count;
         ++index) {

        const auto source =
            source_id{
                static_cast<std::uint32_t>(
                    index + 1)};

        const auto source_path =
            manager.path(source);
        const auto snapshot =
            manager.current(source);

        auto* core =
            source_core +
            index * source_core_size;

        write_u32(
            core,
            path_cursor);
        write_u32(
            core + 4,
            static_cast<std::uint32_t>(
                source_path.size()));

        std::memcpy(
            path_data +
                static_cast<std::size_t>(
                    path_cursor),
            source_path.data(),
            source_path.size());

        path_cursor +=
            static_cast<std::uint32_t>(
                source_path.size());

        auto* physical_record =
            physical +
            index * physical_state_size;

        write_u32(
            physical_record,
            snapshot ? physical_present : 0);
        write_u32(
            physical_record + 4,
            0);

        if (snapshot) {
            const auto observation =
                snapshot.observation();

            if (observation.size >
                (std::numeric_limits<std::uint64_t>::max)()) {
                output.clear();
                return {status_code::not_available};
            }

            write_i64(
                physical_record + 8,
                observation.write_time_ticks);
            write_u64(
                physical_record + 16,
                static_cast<std::uint64_t>(
                    observation.size));

            std::memcpy(
                physical_record + 24,
                snapshot.hash().bytes.data(),
                snapshot.hash().bytes.size());
        }

        auto* graph_record =
            graph +
            index * graph_record_size;

        const auto include_count =
            manager.include_count(source);

        write_u64(
            graph_record,
            forward_cursor);
        write_u32(
            graph_record + 8,
            static_cast<std::uint32_t>(
                include_count));
        write_u32(
            graph_record + 12,
            0);

        for (std::size_t edge = 0;
             edge < include_count;
             ++edge) {

            const auto dependency =
                manager.include_at(
                    source,
                    edge);

            if (!dependency ||
                static_cast<std::size_t>(
                    dependency.value()) >
                    source_count) {
                output.clear();
                return {status_code::artifact_corrupt};
            }

            write_u32(
                forward_edge_data +
                    static_cast<std::size_t>(
                        forward_cursor) * 4,
                dependency.value());

            ++forward_cursor;
        }

        const auto dependent_count =
            manager.dependent_count(source);

        write_u64(
            graph_record + 16,
            reverse_cursor);
        write_u32(
            graph_record + 24,
            static_cast<std::uint32_t>(
                dependent_count));
        write_u32(
            graph_record + 28,
            0);

        for (std::size_t edge = 0;
             edge < dependent_count;
             ++edge) {

            const auto dependent =
                manager.dependent_at(
                    source,
                    edge);

            if (!dependent ||
                static_cast<std::size_t>(
                    dependent.value()) >
                    source_count) {
                output.clear();
                return {status_code::artifact_corrupt};
            }

            write_u32(
                reverse_edge_data +
                    static_cast<std::size_t>(
                        reverse_cursor) * 4,
                dependent.value());

            ++reverse_cursor;
        }

        write_u32(
            graph_record + 32,
            graph_known_flags);
        write_u32(
            graph_record + 36,
            0);
    }

    if (telemetry != nullptr) {
        telemetry->source_records_ns =
            source_manager_elapsed_ns(
                source_records_begin);
    }

    const auto roots_begin =
        source_manager_freeze_clock::now();

    for (std::size_t index = 0;
         index < options.roots.size();
         ++index) {

        const auto& root =
            options.roots[index];

        auto* record =
            root_data +
            index * root_record_size;

        write_u32(
            record,
            root.source.value());

        record[4] =
            static_cast<std::byte>(
                static_cast<std::uint8_t>(
                    root.role));
    }

    if (telemetry != nullptr) {
        telemetry->roots_ns =
            source_manager_elapsed_ns(
                roots_begin);
    }

    const auto path_index_begin =
        source_manager_freeze_clock::now();

    const auto index_mask =
        index_capacity - 1;

    for (std::size_t index = 0;
         index < source_count;
         ++index) {

        const auto source =
            source_id{
                static_cast<std::uint32_t>(
                    index + 1)};

        const auto normalized =
            manager.path(source);
        const auto hash =
            hash_path(normalized);

        auto position =
            static_cast<std::size_t>(
                hash) &
            index_mask;

        while (read_u32(
                   path_index +
                   position *
                       path_index_record_size +
                   4) != 0) {
            position =
                (position + 1) &
                index_mask;
        }

        auto* slot =
            path_index +
            position *
                path_index_record_size;

        write_u32(
            slot,
            path_fingerprint(hash));
        write_u32(
            slot + 4,
            source.value());
    }

    if (telemetry != nullptr) {
        telemetry->path_index_ns =
            source_manager_elapsed_ns(
                path_index_begin);
    }

    const auto file_identity_begin =
        source_manager_freeze_clock::now();

    for (std::size_t index = 0;
         index <
            options.file_identity_index.size();
         ++index) {

        const auto& item =
            options.file_identity_index[index];

        if ((item.file_reference == 0 &&
             (item.source ||
              item.reserved != 0)) ||
            (item.file_reference != 0 &&
             (!item.source ||
              static_cast<std::size_t>(
                  item.source.value()) >
                  source_count ||
              item.reserved != 0))) {
            output.clear();
            return {status_code::invalid_argument};
        }

        auto* record =
            file_identity_data +
            index *
                source_file_identity_index_record_size;

        write_u64(
            record,
            item.file_reference);
        write_u32(
            record + 8,
            item.source.value());
        write_u32(
            record + 12,
            item.reserved);
    }

    if (telemetry != nullptr) {
        telemetry->file_identity_ns =
            source_manager_elapsed_ns(
                file_identity_begin);
    }

    const auto directory_identity_begin =
        source_manager_freeze_clock::now();

    for (std::size_t index = 0;
         index <
            options.directory_identity_index.size();
         ++index) {

        const auto& item =
            options.directory_identity_index[index];

        if ((item.file_reference == 0 &&
             (item.flags != 0 ||
              item.reserved != 0)) ||
            (item.file_reference != 0 &&
             (item.flags == 0 ||
              (item.flags &
               ~source_change_directory_watch_known) != 0 ||
              item.reserved != 0))) {
            output.clear();
            return {status_code::invalid_argument};
        }

        auto* record =
            directory_identity_data +
            index *
                tracked_directory_identity_index_record_size;

        write_u64(
            record,
            item.file_reference);
        write_u32(
            record + 8,
            item.flags);
        write_u32(
            record + 12,
            item.reserved);
    }

    if (telemetry != nullptr) {
        telemetry->directory_identity_ns =
            source_manager_elapsed_ns(
                directory_identity_begin);
    }

    std::array<std::span<const std::byte>, 10>
        crc_sections{};
    std::array<std::uint64_t, 10>
        crc_section_elapsed{};

    for (std::size_t index = 0;
         index < layout.size();
         ++index) {

        const auto& item = layout[index];
        std::uint64_t byte_count = 0;

        if (!multiply_u64(
                item.count,
                item.record_size,
                byte_count) ||
            byte_count >
                (std::numeric_limits<std::size_t>::max)()) {
            output.clear();
            return {status_code::not_available};
        }

        crc_sections[index] =
            std::span<const std::byte>{
                base +
                    static_cast<std::size_t>(
                        item.offset),
                static_cast<std::size_t>(
                    byte_count)};
    }

    const auto crc_wall_begin =
        source_manager_freeze_clock::now();

    for (std::size_t index = 0;
         index < layout.size();
         ++index) {

        auto& item = layout[index];
        const auto section_begin =
            source_manager_freeze_clock::now();

        item.crc64 =
            persistence_crc64(
                crc_sections[index]);

        crc_section_elapsed[index] =
            source_manager_elapsed_ns(
                section_begin);
    }

    if (telemetry != nullptr) {
        telemetry->crc_wall_ns =
            source_manager_elapsed_ns(crc_wall_begin);
    }

    publish_source_manager_crc_sections(
        telemetry,
        crc_sections,
        crc_section_elapsed);

    const auto prefix_begin =
        source_manager_freeze_clock::now();

    std::copy(
        image_magic.begin(),
        image_magic.end(),
        base);

    write_u32(
        base + 8,
        source_manager_image_format_version);
    write_u32(
        base + 12,
        endian_marker);
    write_u32(
        base + 16,
        source_manager_image_header_size);
    write_u32(
        base + 20,
        source_manager_image_directory_count);
    write_u32(
        base + 24,
        source_manager_image_directory_entry_size);
    write_u32(
        base + 28,
        0);

    write_u64(
        base + 32,
        directory_offset);
    write_u64(
        base + 40,
        output.size());
    write_u64(
        base + 48,
        options.generation);
    write_u64(
        base + 56,
        source_count);
    write_u64(
        base + 64,
        options.roots.size());
    write_u64(
        base + 72,
        index_capacity);
    write_u64(
        base + 80,
        forward_edges);
    write_u64(
        base + 88,
        reverse_edges);

    write_u32(
        base + header_change_backend_offset,
        static_cast<std::uint32_t>(
            options.change_checkpoint.backend));
    write_u32(
        base + header_change_backend_offset + 4,
        0);

    write_u64(
        base + header_change_volume_offset,
        options.change_checkpoint.volume_serial);
    write_u64(
        base + header_change_journal_offset,
        options.change_checkpoint.journal_id);
    write_u64(
        base + header_change_usn_offset,
        static_cast<std::uint64_t>(
            options.change_checkpoint.next_usn));

    write_u64(
        base + header_crc_offset,
        0);
    write_u64(
        base + header_directory_crc_offset,
        0);
    write_u64(
        base + header_reserved_begin,
        0);
    write_u64(
        base + header_reserved_begin + 8,
        0);

    for (std::size_t index = 0;
         index < layout.size();
         ++index) {

        const auto& item =
            layout[index];

        auto* entry =
            base +
            directory_offset +
            index *
                source_manager_image_directory_entry_size;

        write_u32(
            entry,
            static_cast<std::uint32_t>(
                item.kind));
        write_u32(
            entry + 4,
            item.record_size);
        write_u64(
            entry + 8,
            item.offset);
        write_u64(
            entry + 16,
            item.count);
        write_u64(
            entry + 24,
            item.crc64);
    }

    if (telemetry != nullptr) {
        telemetry->prefix_directory_encode_ns =
            source_manager_elapsed_ns(prefix_begin);
    }

    const auto directory_crc_begin =
        source_manager_freeze_clock::now();

    const auto directory_crc =
        persistence_crc64(
            std::span<const std::byte>{
                base + directory_offset,
                directory_bytes});

    write_u64(
        base + header_directory_crc_offset,
        directory_crc);

    if (telemetry != nullptr) {
        telemetry->directory_crc_ns =
            source_manager_elapsed_ns(
                directory_crc_begin);
    }

    const auto header_crc_begin =
        source_manager_freeze_clock::now();

    std::array<
        std::byte,
        source_manager_image_header_size> header{};

    std::memcpy(
        header.data(),
        base,
        header.size());

    write_u64(
        header.data() + header_crc_offset,
        0);

    const auto header_crc =
        persistence_crc64(header);

    write_u64(
        base + header_crc_offset,
        header_crc);

    if (telemetry != nullptr) {
        telemetry->header_crc_ns =
            source_manager_elapsed_ns(
                header_crc_begin);
    }

    source_manager_image_view validation;

    const auto bind_begin =
        source_manager_freeze_clock::now();

    const auto bind_result =
        validation.bind(output);

    if (telemetry != nullptr) {
        telemetry->bind_ns =
            source_manager_elapsed_ns(bind_begin);
    }

    if (!bind_result.ok()) {
        output.clear();
        return bind_result;
    }

    const auto verify_begin =
        source_manager_freeze_clock::now();

    const auto verify_result =
        validation.verify_contents();

    if (telemetry != nullptr) {
        telemetry->verify_ns =
            source_manager_elapsed_ns(
                verify_begin);
    }

    if (!verify_result.ok()) {
        output.clear();
        return verify_result;
    }

    if (telemetry != nullptr) {
        telemetry->extent_count =
            output.empty() ? 0u : 1u;
        telemetry->internal_ns =
            source_manager_elapsed_ns(
                internal_begin);
    }

    return {};
}

} // namespace cw::server
