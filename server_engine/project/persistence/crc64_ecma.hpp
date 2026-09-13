#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

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

[[nodiscard]] inline std::uint64_t persistence_crc64(
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

} // namespace cw::server
