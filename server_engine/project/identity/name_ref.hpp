#pragma once

#include <cstdint>
#include <string_view>

namespace cw::server {

// Non-owning view over immutable Project-lifetime identifier bytes.
class name_ref final {
public:
    constexpr name_ref() noexcept = default;

    constexpr name_ref(const char* data, std::uint32_t size) noexcept
        : data_value(data), size_value(size) {}

    [[nodiscard]] constexpr const char* data() const noexcept { return data_value; }
    [[nodiscard]] constexpr std::uint32_t size() const noexcept { return size_value; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_value == 0; }

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return data_value == nullptr ? std::string_view{} : std::string_view{data_value, size_value};
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return data_value != nullptr && size_value != 0;
    }

private:
    const char* data_value = nullptr;
    std::uint32_t size_value = 0;
};

} // namespace cw::server
