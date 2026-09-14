#include "change_state_image.hpp"
#include "crc64_ecma.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>

namespace cw::server {
namespace {

constexpr std::array<std::byte, 8> image_magic_v1{
    std::byte{'C'}, std::byte{'W'}, std::byte{'C'}, std::byte{'S'},
    std::byte{'V'}, std::byte{'1'}, std::byte{0}, std::byte{0},
};

constexpr std::array<std::byte, 8> image_magic_v2{
    std::byte{'C'}, std::byte{'W'}, std::byte{'C'}, std::byte{'S'},
    std::byte{'V'}, std::byte{'2'}, std::byte{0}, std::byte{0},
};

constexpr std::array<std::byte, 8> image_magic_v3{
    std::byte{'C'}, std::byte{'W'}, std::byte{'C'}, std::byte{'S'},
    std::byte{'V'}, std::byte{'3'}, std::byte{0}, std::byte{0},
};

constexpr std::uint32_t endian_marker = 0x01020304u;
constexpr std::size_t header_size = 128;
constexpr std::size_t directory_offset = header_size;
constexpr std::size_t directory_entry_size = 32;

constexpr std::size_t v1_directory_count = 3;
constexpr std::size_t v1_directory_bytes =
    v1_directory_count * directory_entry_size;
constexpr std::size_t v1_first_section_offset = 256;
constexpr std::size_t v1_file_slot_size = 16;

constexpr std::size_t v2_directory_count = 5;
constexpr std::size_t v2_directory_bytes =
    v2_directory_count * directory_entry_size;
constexpr std::size_t v2_first_section_offset = 320;

constexpr std::size_t v3_directory_count = 4;
constexpr std::size_t v3_directory_bytes =
    v3_directory_count * directory_entry_size;
constexpr std::size_t v3_first_section_offset = 256;
constexpr std::size_t bloom_hash_count = 14;
constexpr std::size_t bloom_bits_per_entry = 20;
constexpr std::size_t minimum_bloom_bytes = 64;

constexpr std::size_t file_control_size = 1;
constexpr std::size_t file_reference_size = 8;
constexpr std::size_t file_source_size = 4;
constexpr std::size_t directory_slot_size = 16;

[[nodiscard]] constexpr std::uint64_t align64(
    std::uint64_t value) noexcept {
    return (value + 63u) & ~std::uint64_t{63u};
}

void write_u32(std::byte* target, std::uint32_t value) noexcept {
    target[0] = static_cast<std::byte>(value & 0xffu);
    target[1] = static_cast<std::byte>((value >> 8) & 0xffu);
    target[2] = static_cast<std::byte>((value >> 16) & 0xffu);
    target[3] = static_cast<std::byte>((value >> 24) & 0xffu);
}

void write_u64(std::byte* target, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8; ++index) {
        target[index] =
            static_cast<std::byte>((value >> (index * 8)) & 0xffu);
    }
}

[[nodiscard]] std::uint32_t read_u32(
    const std::byte* source) noexcept {
    return
        static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(source[0])) |
        (static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(source[1])) << 8) |
        (static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(source[2])) << 16) |
        (static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(source[3])) << 24);
}

[[nodiscard]] std::uint64_t read_u64(
    const std::byte* source) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |=
            static_cast<std::uint64_t>(
                std::to_integer<std::uint8_t>(source[index])) <<
            (index * 8);
    }
    return value;
}

[[nodiscard]] constexpr std::uint64_t mix64(
    std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}

[[nodiscard]] bool power_of_two_or_zero(
    std::size_t value) noexcept {
    return value == 0 || (value & (value - 1)) == 0;
}

[[nodiscard]] std::size_t bloom_bytes_for_count(
    std::size_t count) noexcept {

    if (count == 0)
        return 0;

    if (count >
        (std::numeric_limits<std::size_t>::max)() /
            bloom_bits_per_entry) {
        return 0;
    }

    const auto bits =
        count * bloom_bits_per_entry;
    const auto required_bytes =
        bits / 8 + (bits % 8 != 0 ? 1u : 0u);

    std::size_t bytes = minimum_bloom_bytes;
    while (bytes < required_bytes) {
        if (bytes >
            (std::numeric_limits<std::size_t>::max)() / 2) {
            return 0;
        }
        bytes *= 2;
    }

    return bytes;
}

void bloom_add(
    std::span<std::byte> bloom,
    std::uint64_t value) noexcept {

    if (bloom.empty() || value == 0)
        return;

    const auto bit_count =
        bloom.size() * 8;
    const auto mask =
        bit_count - 1;

    const auto h1 =
        mix64(value ^ 0x9e3779b97f4a7c15ULL);
    const auto h2 =
        mix64(value ^ 0xd1b54a32d192ed03ULL) | 1ULL;

    for (std::size_t index = 0;
         index < bloom_hash_count;
         ++index) {

        const auto bit =
            static_cast<std::size_t>(
                h1 + index * h2) & mask;

        auto& target =
            bloom[bit >> 3];

        target |=
            static_cast<std::byte>(
                std::uint8_t{1u} << (bit & 7u));
    }
}

[[nodiscard]] bool bloom_maybe(
    std::span<const std::byte> bloom,
    std::uint64_t value) noexcept {

    if (bloom.empty() || value == 0)
        return false;

    const auto bit_count =
        bloom.size() * 8;
    const auto mask =
        bit_count - 1;

    const auto h1 =
        mix64(value ^ 0x9e3779b97f4a7c15ULL);
    const auto h2 =
        mix64(value ^ 0xd1b54a32d192ed03ULL) | 1ULL;

    for (std::size_t index = 0;
         index < bloom_hash_count;
         ++index) {

        const auto bit =
            static_cast<std::size_t>(
                h1 + index * h2) & mask;

        const auto byte =
            std::to_integer<std::uint8_t>(
                bloom[bit >> 3]);

        if ((byte &
             (std::uint8_t{1u} << (bit & 7u))) == 0) {
            return false;
        }
    }

    return true;
}

[[nodiscard]] bool valid_checkpoint(
    std::size_t source_count,
    std::size_t file_count,
    std::size_t directory_count_value,
    const source_change_checkpoint& checkpoint) noexcept {

    return
        (!checkpoint &&
         checkpoint.volume_serial == 0 &&
         checkpoint.journal_id == 0 &&
         checkpoint.next_usn == 0 &&
         file_count == 0 &&
         directory_count_value == 0) ||
        (checkpoint &&
         checkpoint.volume_serial != 0 &&
         checkpoint.journal_id != 0 &&
         checkpoint.next_usn >= 0 &&
         (source_count == 0 || file_count != 0));
}

[[nodiscard]] status read_common_header(
    std::span<const std::byte> image,
    std::uint32_t expected_version,
    std::size_t expected_directory_count,
    std::size_t expected_directory_bytes,
    std::size_t expected_first_section_offset,
    std::size_t& source_count,
    source_change_checkpoint& checkpoint) noexcept {

    if (image.size() < expected_first_section_offset ||
        read_u32(image.data() + 8) != expected_version ||
        read_u32(image.data() + 12) != endian_marker ||
        read_u32(image.data() + 16) != header_size ||
        read_u32(image.data() + 20) != expected_directory_count ||
        read_u32(image.data() + 24) != directory_entry_size ||
        read_u64(image.data() + 32) != directory_offset ||
        read_u64(image.data() + 40) != image.size()) {
        return {status_code::artifact_corrupt};
    }

    const auto stored_header_crc =
        read_u64(image.data() + 96);

    std::array<std::byte, header_size> header{};
    std::memcpy(header.data(), image.data(), header.size());
    write_u64(header.data() + 96, 0);

    if (persistence_crc64(header) != stored_header_crc)
        return {status_code::artifact_corrupt};

    const auto directory_span =
        image.subspan(directory_offset, expected_directory_bytes);

    if (persistence_crc64(directory_span) != read_u64(image.data() + 104))
        return {status_code::artifact_corrupt};

    const auto stored_source_count =
        read_u64(image.data() + 48);

    if (stored_source_count >
            (std::numeric_limits<std::uint32_t>::max)() ||
        stored_source_count >
            (std::numeric_limits<std::size_t>::max)()) {
        return {status_code::artifact_corrupt};
    }

    source_count =
        static_cast<std::size_t>(stored_source_count);

    const auto backend =
        read_u32(image.data() + 56);

    if (backend >
            static_cast<std::uint32_t>(
                source_change_backend::windows_usn) ||
        read_u32(image.data() + 60) != 0) {
        return {status_code::artifact_corrupt};
    }

    checkpoint.backend =
        static_cast<source_change_backend>(backend);
    checkpoint.volume_serial =
        read_u64(image.data() + 64);
    checkpoint.journal_id =
        read_u64(image.data() + 72);
    checkpoint.next_usn =
        static_cast<std::int64_t>(
            read_u64(image.data() + 80));

    return {};
}

} // namespace

void change_state_image_view::reset() noexcept {
    bytes = {};
    format_version_value = 0;
    legacy_file_index = nullptr;
    file_control = nullptr;
    file_references = nullptr;
    file_sources = nullptr;
    source_bloom = nullptr;
    topology_bloom = nullptr;
    arrival_bloom = nullptr;
    source_bloom_bytes = 0;
    topology_bloom_bytes = 0;
    arrival_bloom_bytes = 0;
    directory_index = nullptr;
    journal_anchor_path = {};
    file_index_count = 0;
    directory_index_count = 0;
    source_count_value = 0;
    checkpoint_value = {};
    file_control_crc = 0;
    file_reference_crc = 0;
    file_source_crc = 0;
    directory_index_crc = 0;
    legacy_file_index_crc = 0;
    source_bloom_crc = 0;
    topology_bloom_crc = 0;
    arrival_bloom_crc = 0;
}

status change_state_image_view::bind(
    std::span<const std::byte> image) noexcept {

    reset();

    if (image.size() < header_size)
        return {status_code::artifact_corrupt};

    const bool version1 =
        std::equal(
            image_magic_v1.begin(),
            image_magic_v1.end(),
            image.begin());
    const bool version2 =
        std::equal(
            image_magic_v2.begin(),
            image_magic_v2.end(),
            image.begin());
    const bool version3 =
        std::equal(
            image_magic_v3.begin(),
            image_magic_v3.end(),
            image.begin());

    if (!version1 && !version2 && !version3)
        return {status_code::artifact_corrupt};

    std::size_t source_count = 0;
    source_change_checkpoint checkpoint;

    if (version3) {
        auto result = read_common_header(
            image,
            change_state_image_format_version,
            v3_directory_count,
            v3_directory_bytes,
            v3_first_section_offset,
            source_count,
            checkpoint);
        if (!result.ok())
            return result;

        const auto* source_entry =
            image.data() + directory_offset;
        const auto* topology_entry =
            source_entry + directory_entry_size;
        const auto* arrival_entry =
            topology_entry + directory_entry_size;
        const auto* anchor_entry =
            arrival_entry + directory_entry_size;

        if (read_u32(source_entry) != 1 ||
            read_u32(source_entry + 4) != 1 ||
            read_u32(topology_entry) != 2 ||
            read_u32(topology_entry + 4) != 1 ||
            read_u32(arrival_entry) != 3 ||
            read_u32(arrival_entry + 4) != 1 ||
            read_u32(anchor_entry) != 4 ||
            read_u32(anchor_entry + 4) != 1) {
            return {status_code::artifact_corrupt};
        }

        const auto source_offset =
            read_u64(source_entry + 8);
        const auto source_bytes =
            read_u64(source_entry + 16);
        const auto topology_offset =
            read_u64(topology_entry + 8);
        const auto topology_bytes =
            read_u64(topology_entry + 16);
        const auto arrival_offset =
            read_u64(arrival_entry + 8);
        const auto arrival_bytes =
            read_u64(arrival_entry + 16);
        const auto anchor_offset =
            read_u64(anchor_entry + 8);
        const auto anchor_size =
            read_u64(anchor_entry + 16);

        if (source_bytes >
                (std::numeric_limits<std::size_t>::max)() ||
            topology_bytes >
                (std::numeric_limits<std::size_t>::max)() ||
            arrival_bytes >
                (std::numeric_limits<std::size_t>::max)() ||
            !power_of_two_or_zero(
                static_cast<std::size_t>(source_bytes)) ||
            !power_of_two_or_zero(
                static_cast<std::size_t>(topology_bytes)) ||
            !power_of_two_or_zero(
                static_cast<std::size_t>(arrival_bytes))) {
            return {status_code::artifact_corrupt};
        }

        const auto expected_topology_offset =
            align64(source_offset + source_bytes);
        const auto expected_arrival_offset =
            align64(topology_offset + topology_bytes);
        const auto expected_anchor_offset =
            align64(arrival_offset + arrival_bytes);

        if (source_offset != v3_first_section_offset ||
            topology_offset != expected_topology_offset ||
            arrival_offset != expected_arrival_offset ||
            anchor_offset != expected_anchor_offset ||
            anchor_size == 0 ||
            anchor_offset + anchor_size != image.size()) {
            return {status_code::artifact_corrupt};
        }

        if (!valid_checkpoint(
                source_count,
                static_cast<std::size_t>(source_bytes),
                static_cast<std::size_t>(
                    topology_bytes + arrival_bytes),
                checkpoint)) {
            return {status_code::artifact_corrupt};
        }

        bytes = image;
        format_version_value =
            change_state_image_format_version;
        source_bloom =
            image.data() +
            static_cast<std::size_t>(source_offset);
        topology_bloom =
            image.data() +
            static_cast<std::size_t>(topology_offset);
        arrival_bloom =
            image.data() +
            static_cast<std::size_t>(arrival_offset);
        source_bloom_bytes =
            static_cast<std::size_t>(source_bytes);
        topology_bloom_bytes =
            static_cast<std::size_t>(topology_bytes);
        arrival_bloom_bytes =
            static_cast<std::size_t>(arrival_bytes);
        journal_anchor_path = {
            reinterpret_cast<const char*>(
                image.data() +
                static_cast<std::size_t>(anchor_offset)),
            static_cast<std::size_t>(anchor_size)};
        source_count_value = source_count;
        checkpoint_value = checkpoint;
        source_bloom_crc =
            read_u64(source_entry + 24);
        topology_bloom_crc =
            read_u64(topology_entry + 24);
        arrival_bloom_crc =
            read_u64(arrival_entry + 24);
        return {};
    }

    auto result = read_common_header(
        image,
        version1 ? 1u : 2u,
        version1 ? v1_directory_count : v2_directory_count,
        version1 ? v1_directory_bytes : v2_directory_bytes,
        version1 ? v1_first_section_offset : v2_first_section_offset,
        source_count,
        checkpoint);
    if (!result.ok())
        return result;

    if (version1) {
        const auto* file_entry =
            image.data() + directory_offset;
        const auto* directory_entry =
            file_entry + directory_entry_size;
        const auto* anchor_entry =
            directory_entry + directory_entry_size;

        if (read_u32(file_entry) != 1 ||
            read_u32(file_entry + 4) != v1_file_slot_size ||
            read_u32(directory_entry) != 2 ||
            read_u32(directory_entry + 4) != directory_slot_size ||
            read_u32(anchor_entry) != 3 ||
            read_u32(anchor_entry + 4) != 1) {
            return {status_code::artifact_corrupt};
        }

        const auto file_offset =
            read_u64(file_entry + 8);
        const auto file_count =
            read_u64(file_entry + 16);
        const auto directory_section_offset =
            read_u64(directory_entry + 8);
        const auto directory_count_value =
            read_u64(directory_entry + 16);
        const auto anchor_offset =
            read_u64(anchor_entry + 8);
        const auto anchor_size =
            read_u64(anchor_entry + 16);

        if (file_count >
                (std::numeric_limits<std::size_t>::max)() ||
            directory_count_value >
                (std::numeric_limits<std::size_t>::max)() ||
            !power_of_two_or_zero(
                static_cast<std::size_t>(file_count)) ||
            !power_of_two_or_zero(
                static_cast<std::size_t>(directory_count_value))) {
            return {status_code::artifact_corrupt};
        }

        const auto expected_directory_offset =
            align64(
                file_offset +
                file_count * v1_file_slot_size);
        const auto expected_anchor_offset =
            align64(
                directory_section_offset +
                directory_count_value * directory_slot_size);

        if (file_offset != v1_first_section_offset ||
            directory_section_offset != expected_directory_offset ||
            anchor_offset != expected_anchor_offset ||
            anchor_size == 0 ||
            anchor_offset + anchor_size != image.size()) {
            return {status_code::artifact_corrupt};
        }

        if (!valid_checkpoint(
                source_count,
                static_cast<std::size_t>(file_count),
                static_cast<std::size_t>(directory_count_value),
                checkpoint)) {
            return {status_code::artifact_corrupt};
        }

        bytes = image;
        format_version_value = 1;
        legacy_file_index =
            image.data() +
            static_cast<std::size_t>(file_offset);
        directory_index =
            image.data() +
            static_cast<std::size_t>(directory_section_offset);
        journal_anchor_path = {
            reinterpret_cast<const char*>(
                image.data() +
                static_cast<std::size_t>(anchor_offset)),
            static_cast<std::size_t>(anchor_size)};
        file_index_count =
            static_cast<std::size_t>(file_count);
        directory_index_count =
            static_cast<std::size_t>(directory_count_value);
        source_count_value = source_count;
        checkpoint_value = checkpoint;
        legacy_file_index_crc =
            read_u64(file_entry + 24);
        directory_index_crc =
            read_u64(directory_entry + 24);
        return {};
    }

    const auto* control_entry =
        image.data() + directory_offset;
    const auto* reference_entry =
        control_entry + directory_entry_size;
    const auto* source_entry =
        reference_entry + directory_entry_size;
    const auto* directory_entry =
        source_entry + directory_entry_size;
    const auto* anchor_entry =
        directory_entry + directory_entry_size;

    if (read_u32(control_entry) != 1 ||
        read_u32(control_entry + 4) != file_control_size ||
        read_u32(reference_entry) != 2 ||
        read_u32(reference_entry + 4) != file_reference_size ||
        read_u32(source_entry) != 3 ||
        read_u32(source_entry + 4) != file_source_size ||
        read_u32(directory_entry) != 4 ||
        read_u32(directory_entry + 4) != directory_slot_size ||
        read_u32(anchor_entry) != 5 ||
        read_u32(anchor_entry + 4) != 1) {
        return {status_code::artifact_corrupt};
    }

    const auto control_offset =
        read_u64(control_entry + 8);
    const auto control_count =
        read_u64(control_entry + 16);
    const auto reference_offset =
        read_u64(reference_entry + 8);
    const auto reference_count =
        read_u64(reference_entry + 16);
    const auto source_offset =
        read_u64(source_entry + 8);
    const auto source_slot_count =
        read_u64(source_entry + 16);
    const auto directory_section_offset =
        read_u64(directory_entry + 8);
    const auto directory_count_value =
        read_u64(directory_entry + 16);
    const auto anchor_offset =
        read_u64(anchor_entry + 8);
    const auto anchor_size =
        read_u64(anchor_entry + 16);

    if (control_count != reference_count ||
        control_count != source_slot_count ||
        control_count >
            (std::numeric_limits<std::size_t>::max)() ||
        directory_count_value >
            (std::numeric_limits<std::size_t>::max)() ||
        !power_of_two_or_zero(
            static_cast<std::size_t>(control_count)) ||
        !power_of_two_or_zero(
            static_cast<std::size_t>(directory_count_value))) {
        return {status_code::artifact_corrupt};
    }

    const auto expected_reference_offset =
        align64(
            control_offset +
            control_count * file_control_size);
    const auto expected_source_offset =
        align64(
            reference_offset +
            reference_count * file_reference_size);
    const auto expected_directory_offset =
        align64(
            source_offset +
            source_slot_count * file_source_size);
    const auto expected_anchor_offset =
        align64(
            directory_section_offset +
            directory_count_value * directory_slot_size);

    if (control_offset != v2_first_section_offset ||
        reference_offset != expected_reference_offset ||
        source_offset != expected_source_offset ||
        directory_section_offset != expected_directory_offset ||
        anchor_offset != expected_anchor_offset ||
        anchor_size == 0 ||
        anchor_offset + anchor_size != image.size()) {
        return {status_code::artifact_corrupt};
    }

    if (!valid_checkpoint(
            source_count,
            static_cast<std::size_t>(control_count),
            static_cast<std::size_t>(directory_count_value),
            checkpoint)) {
        return {status_code::artifact_corrupt};
    }

    bytes = image;
    format_version_value = 2;
    file_control =
        image.data() +
        static_cast<std::size_t>(control_offset);
    file_references =
        image.data() +
        static_cast<std::size_t>(reference_offset);
    file_sources =
        image.data() +
        static_cast<std::size_t>(source_offset);
    directory_index =
        image.data() +
        static_cast<std::size_t>(directory_section_offset);
    journal_anchor_path = {
        reinterpret_cast<const char*>(
            image.data() +
            static_cast<std::size_t>(anchor_offset)),
        static_cast<std::size_t>(anchor_size)};
    file_index_count =
        static_cast<std::size_t>(control_count);
    directory_index_count =
        static_cast<std::size_t>(directory_count_value);
    source_count_value = source_count;
    checkpoint_value = checkpoint;
    file_control_crc =
        read_u64(control_entry + 24);
    file_reference_crc =
        read_u64(reference_entry + 24);
    file_source_crc =
        read_u64(source_entry + 24);
    directory_index_crc =
        read_u64(directory_entry + 24);
    return {};
}

bool change_state_image_view::may_contain_source_file(
    std::uint64_t file_reference) const noexcept {

    if (gate_only()) {
        return bloom_maybe(
            std::span<const std::byte>{
                source_bloom,
                source_bloom_bytes},
            file_reference);
    }

    return static_cast<bool>(
        find_source_file(file_reference));
}

bool change_state_image_view::may_watch_directory_topology(
    std::uint64_t file_reference) const noexcept {

    if (gate_only()) {
        return bloom_maybe(
            std::span<const std::byte>{
                topology_bloom,
                topology_bloom_bytes},
            file_reference);
    }

    return
        (directory_watch_flags(file_reference) &
         source_change_directory_watch_topology) != 0;
}

bool change_state_image_view::may_watch_directory_arrival(
    std::uint64_t file_reference) const noexcept {

    if (gate_only()) {
        return bloom_maybe(
            std::span<const std::byte>{
                arrival_bloom,
                arrival_bloom_bytes},
            file_reference);
    }

    return
        (directory_watch_flags(file_reference) &
         source_change_directory_watch_arrival) != 0;
}

source_id change_state_image_view::find_source_file(
    std::uint64_t file_reference) const noexcept {

    if (gate_only())
        return {};

    if (file_reference == 0 || file_index_count == 0)
        return {};

    const auto mask = file_index_count - 1;
    auto position =
        static_cast<std::size_t>(mix64(file_reference)) & mask;

    for (std::size_t probe = 0;
         probe < file_index_count;
         ++probe) {

        if (format_version_value == 1) {
            const auto* slot =
                legacy_file_index +
                position * v1_file_slot_size;
            const auto candidate =
                read_u64(slot);

            if (candidate == 0)
                return {};

            if (candidate == file_reference) {
                const source_id source{
                    read_u32(slot + 8)};
                return source &&
                    static_cast<std::size_t>(
                        source.value()) <= source_count_value &&
                    read_u32(slot + 12) == 0
                    ? source
                    : source_id{};
            }
        }
        else {
            const auto control =
                std::to_integer<std::uint8_t>(
                    file_control[position]);

            if (control == 0)
                return {};
            if (control != 1)
                return {};

            const auto candidate =
                read_u64(
                    file_references +
                    position * file_reference_size);

            if (candidate == file_reference) {
                const source_id source{
                    read_u32(
                        file_sources +
                        position * file_source_size)};
                return source &&
                    static_cast<std::size_t>(
                        source.value()) <= source_count_value
                    ? source
                    : source_id{};
            }
        }

        position = (position + 1) & mask;
    }

    return {};
}

std::uint32_t change_state_image_view::directory_watch_flags(
    std::uint64_t file_reference) const noexcept {

    if (gate_only())
        return 0;

    if (file_reference == 0 ||
        directory_index_count == 0) {
        return 0;
    }

    const auto mask = directory_index_count - 1;
    auto position =
        static_cast<std::size_t>(mix64(file_reference)) & mask;

    for (std::size_t probe = 0;
         probe < directory_index_count;
         ++probe) {

        const auto* slot =
            directory_index +
            position * directory_slot_size;
        const auto candidate =
            read_u64(slot);

        if (candidate == 0)
            return 0;

        if (candidate == file_reference) {
            const auto flags =
                read_u32(slot + 8);
            return read_u32(slot + 12) == 0 &&
                (flags &
                 ~source_change_directory_watch_known) == 0
                ? flags
                : 0;
        }

        position = (position + 1) & mask;
    }

    return 0;
}

status change_state_image_view::verify_contents() const noexcept {
    if (!valid())
        return {status_code::invalid_state};

    if (gate_only()) {
        const auto* anchor_entry =
            bytes.data() +
            directory_offset +
            3 * directory_entry_size;

        const auto anchor_crc =
            read_u64(anchor_entry + 24);

        const auto source_bytes =
            std::span<const std::byte>{
                source_bloom,
                source_bloom_bytes};
        const auto topology_bytes =
            std::span<const std::byte>{
                topology_bloom,
                topology_bloom_bytes};
        const auto arrival_bytes =
            std::span<const std::byte>{
                arrival_bloom,
                arrival_bloom_bytes};
        const auto anchor_bytes =
            std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(
                    journal_anchor_path.data()),
                journal_anchor_path.size()};

        return
            persistence_crc64(source_bytes) == source_bloom_crc &&
            persistence_crc64(topology_bytes) == topology_bloom_crc &&
            persistence_crc64(arrival_bytes) == arrival_bloom_crc &&
            persistence_crc64(anchor_bytes) == anchor_crc
            ? status{}
            : status{status_code::artifact_corrupt};
    }

    const auto directory_bytes_view =
        std::span<const std::byte>{
            directory_index,
            directory_index_count * directory_slot_size};

    if (persistence_crc64(directory_bytes_view) != directory_index_crc)
        return {status_code::artifact_corrupt};

    const auto anchor_entry_index =
        format_version_value == 1 ? 2u : 4u;
    const auto* anchor_entry =
        bytes.data() +
        directory_offset +
        anchor_entry_index * directory_entry_size;

    const auto anchor_bytes =
        std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(
                journal_anchor_path.data()),
            journal_anchor_path.size()};

    if (persistence_crc64(anchor_bytes) != read_u64(anchor_entry + 24))
        return {status_code::artifact_corrupt};

    if (format_version_value == 1) {
        const auto file_bytes =
            std::span<const std::byte>{
                legacy_file_index,
                file_index_count * v1_file_slot_size};

        return persistence_crc64(file_bytes) == legacy_file_index_crc
            ? status{}
            : status{status_code::artifact_corrupt};
    }

    const auto control_bytes =
        std::span<const std::byte>{
            file_control,
            file_index_count * file_control_size};
    const auto reference_bytes =
        std::span<const std::byte>{
            file_references,
            file_index_count * file_reference_size};
    const auto source_bytes =
        std::span<const std::byte>{
            file_sources,
            file_index_count * file_source_size};

    return
        persistence_crc64(control_bytes) == file_control_crc &&
        persistence_crc64(reference_bytes) == file_reference_crc &&
        persistence_crc64(source_bytes) == file_source_crc
        ? status{}
        : status{status_code::artifact_corrupt};
}

status encode_change_state_image(
    std::size_t source_count,
    std::string_view journal_anchor_path,
    const source_change_capture& capture,
    std::vector<std::byte>& output) noexcept {

    output.clear();

    if (source_count >
            (std::numeric_limits<std::uint32_t>::max)() ||
        (capture.checkpoint &&
         journal_anchor_path.empty())) {
        return {status_code::invalid_argument};
    }

    if ((!capture.checkpoint &&
         (!capture.file_index.empty() ||
          !capture.directory_index.empty())) ||
        (capture.checkpoint &&
         (capture.checkpoint.volume_serial == 0 ||
          capture.checkpoint.journal_id == 0 ||
          capture.checkpoint.next_usn < 0 ||
          (source_count != 0 &&
           capture.file_index.empty()))) ||
        !power_of_two_or_zero(capture.file_index.size()) ||
        !power_of_two_or_zero(
            capture.directory_index.size())) {
        return {status_code::invalid_argument};
    }

    std::size_t source_identity_count = 0;
    std::size_t topology_count = 0;
    std::size_t arrival_count = 0;

    for (const auto& slot : capture.file_index) {
        if (slot.file_reference == 0) {
            if (slot.source || slot.reserved != 0)
                return {status_code::invalid_argument};
            continue;
        }

        if (!slot.source ||
            static_cast<std::size_t>(
                slot.source.value()) > source_count ||
            slot.reserved != 0) {
            return {status_code::invalid_argument};
        }

        ++source_identity_count;
    }

    for (const auto& slot : capture.directory_index) {
        if (slot.file_reference == 0) {
            if (slot.flags != 0 ||
                slot.reserved != 0) {
                return {status_code::invalid_argument};
            }
            continue;
        }

        if (slot.reserved != 0 ||
            slot.flags == 0 ||
            (slot.flags &
             ~source_change_directory_watch_known) != 0) {
            return {status_code::invalid_argument};
        }

        if ((slot.flags &
             source_change_directory_watch_topology) != 0) {
            ++topology_count;
        }

        if ((slot.flags &
             source_change_directory_watch_arrival) != 0) {
            ++arrival_count;
        }
    }

    const auto source_bytes =
        bloom_bytes_for_count(source_identity_count);
    const auto topology_bytes =
        bloom_bytes_for_count(topology_count);
    const auto arrival_bytes =
        bloom_bytes_for_count(arrival_count);

    if ((source_identity_count != 0 &&
         source_bytes == 0) ||
        (topology_count != 0 &&
         topology_bytes == 0) ||
        (arrival_count != 0 &&
         arrival_bytes == 0)) {
        return {status_code::not_available};
    }

    const auto source_offset =
        static_cast<std::uint64_t>(
            v3_first_section_offset);
    const auto topology_offset =
        align64(
            source_offset +
            source_bytes);
    const auto arrival_offset =
        align64(
            topology_offset +
            topology_bytes);
    const auto anchor_offset =
        align64(
            arrival_offset +
            arrival_bytes);
    const auto file_size =
        anchor_offset +
        journal_anchor_path.size();

    if (file_size >
        (std::numeric_limits<std::size_t>::max)()) {
        return {status_code::not_available};
    }

    try {
        output.assign(
            static_cast<std::size_t>(file_size),
            std::byte{0});

        std::copy(
            image_magic_v3.begin(),
            image_magic_v3.end(),
            output.begin());

        write_u32(
            output.data() + 8,
            change_state_image_format_version);
        write_u32(
            output.data() + 12,
            endian_marker);
        write_u32(
            output.data() + 16,
            static_cast<std::uint32_t>(
                header_size));
        write_u32(
            output.data() + 20,
            static_cast<std::uint32_t>(
                v3_directory_count));
        write_u32(
            output.data() + 24,
            static_cast<std::uint32_t>(
                directory_entry_size));
        write_u64(
            output.data() + 32,
            directory_offset);
        write_u64(
            output.data() + 40,
            file_size);
        write_u64(
            output.data() + 48,
            source_count);

        write_u32(
            output.data() + 56,
            static_cast<std::uint32_t>(
                capture.checkpoint.backend));
        write_u64(
            output.data() + 64,
            capture.checkpoint.volume_serial);
        write_u64(
            output.data() + 72,
            capture.checkpoint.journal_id);
        write_u64(
            output.data() + 80,
            static_cast<std::uint64_t>(
                capture.checkpoint.next_usn));

        auto* source_entry =
            output.data() + directory_offset;
        auto* topology_entry =
            source_entry + directory_entry_size;
        auto* arrival_entry =
            topology_entry + directory_entry_size;
        auto* anchor_entry =
            arrival_entry + directory_entry_size;

        write_u32(source_entry, 1);
        write_u32(source_entry + 4, 1);
        write_u64(source_entry + 8, source_offset);
        write_u64(source_entry + 16, source_bytes);

        write_u32(topology_entry, 2);
        write_u32(topology_entry + 4, 1);
        write_u64(topology_entry + 8, topology_offset);
        write_u64(topology_entry + 16, topology_bytes);

        write_u32(arrival_entry, 3);
        write_u32(arrival_entry + 4, 1);
        write_u64(arrival_entry + 8, arrival_offset);
        write_u64(arrival_entry + 16, arrival_bytes);

        write_u32(anchor_entry, 4);
        write_u32(anchor_entry + 4, 1);
        write_u64(anchor_entry + 8, anchor_offset);
        write_u64(
            anchor_entry + 16,
            journal_anchor_path.size());

        auto source_filter =
            std::span<std::byte>{
                output.data() +
                    static_cast<std::size_t>(
                        source_offset),
                source_bytes};
        auto topology_filter =
            std::span<std::byte>{
                output.data() +
                    static_cast<std::size_t>(
                        topology_offset),
                topology_bytes};
        auto arrival_filter =
            std::span<std::byte>{
                output.data() +
                    static_cast<std::size_t>(
                        arrival_offset),
                arrival_bytes};

        for (const auto& slot : capture.file_index) {
            if (slot.file_reference != 0) {
                bloom_add(
                    source_filter,
                    slot.file_reference);
            }
        }

        for (const auto& slot : capture.directory_index) {
            if (slot.file_reference == 0)
                continue;

            if ((slot.flags &
                 source_change_directory_watch_topology) != 0) {
                bloom_add(
                    topology_filter,
                    slot.file_reference);
            }

            if ((slot.flags &
                 source_change_directory_watch_arrival) != 0) {
                bloom_add(
                    arrival_filter,
                    slot.file_reference);
            }
        }

        auto* anchor_target =
            output.data() +
            static_cast<std::size_t>(
                anchor_offset);

        std::memcpy(
            anchor_target,
            journal_anchor_path.data(),
            journal_anchor_path.size());

        write_u64(
            source_entry + 24,
            persistence_crc64(source_filter));
        write_u64(
            topology_entry + 24,
            persistence_crc64(topology_filter));
        write_u64(
            arrival_entry + 24,
            persistence_crc64(arrival_filter));
        write_u64(
            anchor_entry + 24,
            persistence_crc64(
                std::span<const std::byte>{
                    anchor_target,
                    journal_anchor_path.size()}));

        write_u64(
            output.data() + 104,
            persistence_crc64(
                std::span<const std::byte>{
                    output}.subspan(
                        directory_offset,
                        v3_directory_bytes)));

        write_u64(
            output.data() + 96,
            0);
        write_u64(
            output.data() + 96,
            persistence_crc64(
                std::span<const std::byte>{
                    output}.first(
                        header_size)));

        return {};
    }
    catch (const std::bad_alloc&) {
        output.clear();
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        output.clear();
        return {status_code::not_available};
    }
}

} // namespace cw::server
