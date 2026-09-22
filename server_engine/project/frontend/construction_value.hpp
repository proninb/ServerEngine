#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include "../../string_id.hpp"

namespace cw::server {

enum class construction_kind : std::uint32_t {
    zero, signed_integer, unsigned_integer, real, member_binding, unsupported, aggregate,
};

// Pointer-free normalized construction semantics. operand is a one-based
// member index in the owning record; integer/real bits have explicit LE encoding.
struct construction_value final {
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    std::uint32_t operand = 0;
    construction_kind kind = construction_kind::zero;
    // Aggregate operand is an interned canonical initializer list, never a source span.
    [[nodiscard]] string_id expression() const noexcept { return string_id{operand}; }

    [[nodiscard]] std::uint64_t bits() const noexcept {
        return low | (std::uint64_t{high} << 32);
    }
    static construction_value constant(construction_kind kind, std::uint64_t bits) noexcept {
        return {static_cast<std::uint32_t>(bits), static_cast<std::uint32_t>(bits >> 32), 0, kind};
    }
    friend bool operator==(const construction_value&, const construction_value&) = default;
};
static_assert(sizeof(construction_value) == 16);

inline bool valid_construction(construction_value value) noexcept {
    switch (value.kind) {
    case construction_kind::zero:
    case construction_kind::unsupported: return value.bits() == 0 && value.operand == 0;
    case construction_kind::member_binding: return value.bits() == 0 && value.operand != 0;
    case construction_kind::aggregate: return value.bits() == 0 && value.operand != 0;
    case construction_kind::signed_integer:
    case construction_kind::unsigned_integer:
    case construction_kind::real: return value.operand == 0;
    }
    return false;
}
inline void write_construction(std::byte* out, construction_value value) noexcept {
    const std::uint32_t words[]{value.low, value.high, value.operand, static_cast<std::uint32_t>(value.kind)};
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < 4; ++j) out[i * 4 + j] = std::byte(words[i] >> (j * 8));
}
inline construction_value read_construction(const std::byte* in) noexcept {
    std::uint32_t words[4]{};
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < 4; ++j)
            words[i] |= std::uint32_t(std::to_integer<unsigned char>(in[i * 4 + j])) << (j * 8);
    return {words[0], words[1], words[2], static_cast<construction_kind>(words[3])};
}
} // namespace cw::server
