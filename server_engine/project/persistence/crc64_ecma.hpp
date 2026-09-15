#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

namespace cw::server {

// CRC-64/ECMA-182 used by persisted image formats. The table-driven update is
// bit-identical to the original non-reflected polynomial implementation.
inline constexpr std::uint64_t persistence_crc64_polynomial =
    0x42f0e1eba9ea3693ULL;

[[nodiscard]] constexpr std::array<std::uint64_t, 256>
make_persistence_crc64_table() noexcept {
    std::array<std::uint64_t, 256> output{};

    for (std::size_t index = 0; index < output.size(); ++index) {
        auto crc =
            static_cast<std::uint64_t>(index) << 56;

        for (unsigned bit = 0; bit < 8; ++bit) {
            crc =
                (crc & (std::uint64_t{1} << 63)) != 0
                    ? (crc << 1) ^
                        persistence_crc64_polynomial
                    : crc << 1;
        }

        output[index] = crc;
    }

    return output;
}

inline constexpr auto persistence_crc64_table =
    make_persistence_crc64_table();

[[nodiscard]] constexpr std::uint64_t persistence_crc64_advance_zero(
    std::uint64_t crc) noexcept {

    return
        (crc << 8) ^
        persistence_crc64_table[
            static_cast<std::uint8_t>(crc >> 56)];
}

[[nodiscard]] constexpr
std::array<std::array<std::uint64_t, 256>, 8>
make_persistence_crc64_slicing_table() noexcept {

    std::array<std::array<std::uint64_t, 256>, 8> output{};

    for (std::size_t index = 0; index < 256; ++index)
        output[7][index] = persistence_crc64_table[index];

    for (std::size_t slice = 7; slice > 0; --slice) {
        for (std::size_t index = 0; index < 256; ++index) {
            output[slice - 1][index] =
                persistence_crc64_advance_zero(
                    output[slice][index]);
        }
    }

    return output;
}

inline constexpr auto persistence_crc64_slicing_table =
    make_persistence_crc64_slicing_table();

[[nodiscard]] constexpr std::uint64_t persistence_crc64_byteswap(
    std::uint64_t value) noexcept {

    return
        ((value & 0x00000000000000ffULL) << 56) |
        ((value & 0x000000000000ff00ULL) << 40) |
        ((value & 0x0000000000ff0000ULL) << 24) |
        ((value & 0x00000000ff000000ULL) << 8) |
        ((value & 0x000000ff00000000ULL) >> 8) |
        ((value & 0x0000ff0000000000ULL) >> 24) |
        ((value & 0x00ff000000000000ULL) >> 40) |
        ((value & 0xff00000000000000ULL) >> 56);
}

[[nodiscard]] constexpr std::uint64_t persistence_crc64_load_be64(
    std::span<const std::byte> bytes,
    std::size_t offset) noexcept {

    if (std::is_constant_evaluated()) {
        return
            (static_cast<std::uint64_t>(
                std::to_integer<std::uint8_t>(
                    bytes[offset])) << 56) |
            (static_cast<std::uint64_t>(
                std::to_integer<std::uint8_t>(
                    bytes[offset + 1])) << 48) |
            (static_cast<std::uint64_t>(
                std::to_integer<std::uint8_t>(
                    bytes[offset + 2])) << 40) |
            (static_cast<std::uint64_t>(
                std::to_integer<std::uint8_t>(
                    bytes[offset + 3])) << 32) |
            (static_cast<std::uint64_t>(
                std::to_integer<std::uint8_t>(
                    bytes[offset + 4])) << 24) |
            (static_cast<std::uint64_t>(
                std::to_integer<std::uint8_t>(
                    bytes[offset + 5])) << 16) |
            (static_cast<std::uint64_t>(
                std::to_integer<std::uint8_t>(
                    bytes[offset + 6])) << 8) |
            static_cast<std::uint64_t>(
                std::to_integer<std::uint8_t>(
                    bytes[offset + 7]));
    }

    std::uint64_t value = 0;
    std::memcpy(
        &value,
        bytes.data() + offset,
        sizeof(value));

    if constexpr (std::endian::native == std::endian::little)
        return persistence_crc64_byteswap(value);

    static_assert(
        std::endian::native == std::endian::big ||
        std::endian::native == std::endian::little);

    return value;
}

// D4D_SPARSE_BASELINE_SOURCE_MANAGER
// Continues CRC-64/ECMA-182 over another immutable extent. This allows a
// scatter/gather persisted section to be checksummed without assembling a copy.
[[nodiscard]] constexpr std::uint64_t persistence_crc64_update(
    std::uint64_t crc,
    std::span<const std::byte> bytes) noexcept {

    std::size_t offset = 0;

    while (bytes.size() - offset >= 8) {
        const auto value =
            crc ^
            persistence_crc64_load_be64(
                bytes,
                offset);

        crc =
            persistence_crc64_slicing_table[0][
                static_cast<std::uint8_t>(value >> 56)] ^
            persistence_crc64_slicing_table[1][
                static_cast<std::uint8_t>(value >> 48)] ^
            persistence_crc64_slicing_table[2][
                static_cast<std::uint8_t>(value >> 40)] ^
            persistence_crc64_slicing_table[3][
                static_cast<std::uint8_t>(value >> 32)] ^
            persistence_crc64_slicing_table[4][
                static_cast<std::uint8_t>(value >> 24)] ^
            persistence_crc64_slicing_table[5][
                static_cast<std::uint8_t>(value >> 16)] ^
            persistence_crc64_slicing_table[6][
                static_cast<std::uint8_t>(value >> 8)] ^
            persistence_crc64_slicing_table[7][
                static_cast<std::uint8_t>(value)];

        offset += 8;
    }

    while (offset < bytes.size()) {
        const auto table_index =
            static_cast<std::uint8_t>(
                (crc >> 56) ^
                std::to_integer<std::uint8_t>(
                    bytes[offset]));

        crc =
            (crc << 8) ^
            persistence_crc64_table[table_index];

        ++offset;
    }

    return crc;
}

[[nodiscard]] constexpr std::uint64_t persistence_crc64(
    std::span<const std::byte> bytes) noexcept {

    std::uint64_t crc = 0;
    std::size_t offset = 0;

    while (bytes.size() - offset >= 8) {
        const auto value =
            crc ^
            persistence_crc64_load_be64(
                bytes,
                offset);

        crc =
            persistence_crc64_slicing_table[0][
                static_cast<std::uint8_t>(value >> 56)] ^
            persistence_crc64_slicing_table[1][
                static_cast<std::uint8_t>(value >> 48)] ^
            persistence_crc64_slicing_table[2][
                static_cast<std::uint8_t>(value >> 40)] ^
            persistence_crc64_slicing_table[3][
                static_cast<std::uint8_t>(value >> 32)] ^
            persistence_crc64_slicing_table[4][
                static_cast<std::uint8_t>(value >> 24)] ^
            persistence_crc64_slicing_table[5][
                static_cast<std::uint8_t>(value >> 16)] ^
            persistence_crc64_slicing_table[6][
                static_cast<std::uint8_t>(value >> 8)] ^
            persistence_crc64_slicing_table[7][
                static_cast<std::uint8_t>(value)];

        offset += 8;
    }

    while (offset < bytes.size()) {
        const auto table_index =
            static_cast<std::uint8_t>(
                (crc >> 56) ^
                std::to_integer<std::uint8_t>(
                    bytes[offset]));

        crc =
            (crc << 8) ^
            persistence_crc64_table[table_index];

        ++offset;
    }

    return crc;
}

[[nodiscard]] constexpr std::uint64_t persistence_crc64_reference(
    std::span<const std::byte> bytes) noexcept {

    std::uint64_t crc = 0;

    for (const auto byte : bytes) {
        const auto table_index =
            static_cast<std::uint8_t>(
                (crc >> 56) ^
                std::to_integer<std::uint8_t>(byte));

        crc =
            (crc << 8) ^
            persistence_crc64_table[table_index];
    }

    return crc;
}

inline constexpr std::array<std::byte, 17>
persistence_crc64_regression_bytes{
    std::byte{0x00},
    std::byte{0x01},
    std::byte{0x02},
    std::byte{0x03},
    std::byte{0x04},
    std::byte{0x05},
    std::byte{0x06},
    std::byte{0x07},
    std::byte{0x08},
    std::byte{0x09},
    std::byte{0xa5},
    std::byte{0x5a},
    std::byte{0xff},
    std::byte{0x80},
    std::byte{0x7f},
    std::byte{0x11},
    std::byte{0xee},
};

static_assert(
    persistence_crc64(
        persistence_crc64_regression_bytes) ==
    persistence_crc64_reference(
        persistence_crc64_regression_bytes));

inline constexpr std::array<std::byte, 9>
persistence_crc64_ecma_check{
    std::byte{0x31},
    std::byte{0x32},
    std::byte{0x33},
    std::byte{0x34},
    std::byte{0x35},
    std::byte{0x36},
    std::byte{0x37},
    std::byte{0x38},
    std::byte{0x39},
};

static_assert(
    persistence_crc64(
        persistence_crc64_ecma_check) ==
    0x6c40df5f0b497347ULL);

} // namespace cw::server
