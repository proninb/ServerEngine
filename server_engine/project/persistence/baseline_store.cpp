#include "baseline_store.hpp"
#include "crc64_ecma.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace cw::server {
namespace {

constexpr std::size_t manifest_size = 256;
constexpr std::size_t manifest_crc_offset = 248;
constexpr std::uint32_t endian_marker = 0x01020304u;
constexpr std::array<std::byte, 8> manifest_magic{
    std::byte{'S'}, std::byte{'E'}, std::byte{'B'}, std::byte{'A'},
    std::byte{'S'}, std::byte{'E'}, std::byte{'1'}, std::byte{0}};

constexpr std::string_view compiled_name = "compiled.bin";
constexpr std::string_view source_manager_name = "source_manager.bin";
constexpr std::string_view change_state_name = "change_state.bin";
constexpr std::string_view build_cache_name = "build_cache.bin";
constexpr std::string_view manifest_name = "manifest.bin";
constexpr std::string_view current_name = "CURRENT";
constexpr std::array<std::byte, 8> current_selector_magic_v2{
    std::byte{'S'}, std::byte{'E'}, std::byte{'C'}, std::byte{'U'},
    std::byte{'R'}, std::byte{'R'}, std::byte{'2'}, std::byte{0}};
constexpr std::uint32_t current_selector_version_v2 = 2;
constexpr std::size_t current_selector_transaction_capacity = 64;
constexpr std::size_t current_selector_header_size_v2 =
    8 + 4 + 4 + current_selector_transaction_capacity;
constexpr std::size_t current_selector_size_v2 =
    current_selector_header_size_v2 + manifest_size;

constexpr std::array<std::byte, 8> current_selector_magic_v3{
    std::byte{'S'}, std::byte{'E'}, std::byte{'C'}, std::byte{'U'},
    std::byte{'R'}, std::byte{'R'}, std::byte{'3'}, std::byte{0}};
constexpr std::uint32_t current_selector_version_v3 = 3;
constexpr std::size_t current_selector_header_size_v3 =
    8 + 4 + 4 + 8 + 8 + current_selector_transaction_capacity;
constexpr std::size_t current_selector_manifest_offset_v3 =
    current_selector_header_size_v3;
constexpr std::size_t current_selector_change_state_offset_v3 =
    current_selector_manifest_offset_v3 + manifest_size;
constexpr std::size_t current_selector_change_state_limit =
    64u * 1024u * 1024u;
constexpr std::size_t current_selector_maximum_size =
    current_selector_change_state_offset_v3 +
    current_selector_change_state_limit;

std::atomic<std::uint64_t> transaction_counter{0};

[[nodiscard]] std::uint64_t elapsed_ns(
    std::chrono::steady_clock::time_point begin,
    std::chrono::steady_clock::time_point end) noexcept {

    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            end - begin).count());
}

void write_u32(std::array<std::byte, manifest_size>& output, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t byte = 0; byte < 4; ++byte)
        output[offset + byte] = static_cast<std::byte>((value >> (byte * 8)) & 0xffu);
}

void write_u64(std::array<std::byte, manifest_size>& output, std::size_t offset, std::uint64_t value) noexcept {
    for (std::size_t byte = 0; byte < 8; ++byte)
        output[offset + byte] = static_cast<std::byte>((value >> (byte * 8)) & 0xffu);
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> input, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t byte = 0; byte < 4; ++byte)
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset + byte])) << (byte * 8);
    return value;
}

[[nodiscard]] std::uint64_t read_u64(std::span<const std::byte> input, std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t byte = 0; byte < 8; ++byte)
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(input[offset + byte])) << (byte * 8);
    return value;
}

[[nodiscard]] bool valid_transaction_name(std::string_view value) noexcept {
    if (value.size() < 4 || value.size() > 63 || !value.starts_with("tx-"))
        return false;
    for (const auto character : value) {
        const bool digit = character >= '0' && character <= '9';
        const bool lower = character >= 'a' && character <= 'z';
        const bool upper = character >= 'A' && character <= 'Z';
        if (!digit && !lower && !upper && character != '-')
            return false;
    }
    return true;
}

[[nodiscard]] std::uint64_t process_id() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

[[nodiscard]] std::string transaction_name_candidate() {
    const auto clock = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto counter = transaction_counter.fetch_add(1, std::memory_order_relaxed);
    return "tx-" + std::to_string(clock) + "-" + std::to_string(process_id()) + "-" +
        std::to_string(counter);
}

[[nodiscard]] status read_small_file(
    const std::filesystem::path& path,
    std::size_t maximum,
    std::vector<std::byte>& output) noexcept {

    output.clear();

#if defined(_WIN32)
    const auto handle = ::CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);

    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = ::GetLastError();
        return error == ERROR_FILE_NOT_FOUND ||
            error == ERROR_PATH_NOT_FOUND
            ? status{status_code::not_found}
            : status{status_code::io_failed};
    }

    LARGE_INTEGER native_size{};
    if (::GetFileSizeEx(handle, &native_size) == 0 ||
        native_size.QuadPart < 0) {
        ::CloseHandle(handle);
        return {status_code::io_failed};
    }

    const auto size =
        static_cast<std::uint64_t>(native_size.QuadPart);
    if (size > maximum ||
        size >
            static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
        ::CloseHandle(handle);
        return {status_code::artifact_corrupt};
    }

    try {
        output.resize(static_cast<std::size_t>(size));
    }
    catch (const std::bad_alloc&) {
        ::CloseHandle(handle);
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        ::CloseHandle(handle);
        return {status_code::not_available};
    }

    std::size_t offset = 0;
    while (offset < output.size()) {
        const auto remaining = output.size() - offset;
        const auto chunk = static_cast<DWORD>(
            (std::min<std::size_t>)(
                remaining,
                (std::numeric_limits<DWORD>::max)()));

        DWORD read = 0;
        if (::ReadFile(
                handle,
                output.data() + offset,
                chunk,
                &read,
                nullptr) == 0 ||
            read == 0) {
            ::CloseHandle(handle);
            output.clear();
            return {status_code::io_failed};
        }

        offset += static_cast<std::size_t>(read);
    }

    if (::CloseHandle(handle) == 0) {
        output.clear();
        return {status_code::io_failed};
    }

    return {};
#else
    const auto handle = ::open(path.c_str(), O_RDONLY);
    if (handle < 0) {
        return errno == ENOENT
            ? status{status_code::not_found}
            : status{status_code::io_failed};
    }

    struct stat information {};
    if (::fstat(handle, &information) != 0 ||
        information.st_size < 0) {
        ::close(handle);
        return {status_code::io_failed};
    }

    const auto size =
        static_cast<std::uint64_t>(information.st_size);
    if (size > maximum ||
        size >
            static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
        ::close(handle);
        return {status_code::artifact_corrupt};
    }

    try {
        output.resize(static_cast<std::size_t>(size));
    }
    catch (const std::bad_alloc&) {
        ::close(handle);
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        ::close(handle);
        return {status_code::not_available};
    }

    std::size_t offset = 0;
    while (offset < output.size()) {
        const auto result = ::read(
            handle,
            output.data() + offset,
            output.size() - offset);

        if (result < 0) {
            if (errno == EINTR)
                continue;

            ::close(handle);
            output.clear();
            return {status_code::io_failed};
        }

        if (result == 0) {
            ::close(handle);
            output.clear();
            return {status_code::io_failed};
        }

        offset += static_cast<std::size_t>(result);
    }

    if (::close(handle) != 0) {
        output.clear();
        return {status_code::io_failed};
    }

    return {};
#endif
}

[[nodiscard]] status durable_write_file(
    const std::filesystem::path& path,
    std::span<const std::byte> bytes) noexcept {
#if defined(_WIN32)
    const auto handle = ::CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return {status_code::io_failed};

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
            remaining, (std::numeric_limits<DWORD>::max)()));
        DWORD written = 0;
        if (!::WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) || written != chunk) {
            ::CloseHandle(handle);
            return {status_code::io_failed};
        }
        offset += written;
    }

    const auto flushed = ::FlushFileBuffers(handle) != 0;
    const auto closed = ::CloseHandle(handle) != 0;
    return flushed && closed ? status{} : status{status_code::io_failed};
#else
    const auto handle = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0666);
    if (handle < 0)
        return {status_code::io_failed};

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto result = ::write(handle, bytes.data() + offset, bytes.size() - offset);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            ::close(handle);
            return {status_code::io_failed};
        }
        if (result == 0) {
            ::close(handle);
            return {status_code::io_failed};
        }
        offset += static_cast<std::size_t>(result);
    }

    const auto synced = ::fsync(handle) == 0;
    const auto closed = ::close(handle) == 0;
    return synced && closed ? status{} : status{status_code::io_failed};
#endif
}

[[nodiscard]] status flush_directory(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
    (void)path;
    return {};
#else
    const auto handle = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (handle < 0)
        return {status_code::io_failed};
    const auto synced = ::fsync(handle) == 0;
    const auto closed = ::close(handle) == 0;
    return synced && closed ? status{} : status{status_code::io_failed};
#endif
}

[[nodiscard]] status atomic_replace(
    const std::filesystem::path& source,
    const std::filesystem::path& target) noexcept {
#if defined(_WIN32)
    return ::MoveFileExW(
        source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0
        ? status{}
        : status{status_code::io_failed};
#else
    return ::rename(source.c_str(), target.c_str()) == 0
        ? status{}
        : status{status_code::io_failed};
#endif
}

[[nodiscard]] status create_manifest(
    const baseline_fingerprint& fingerprint,
    const baseline_configuration_state& configuration,
    std::string_view transaction,
    std::uint64_t compiled_size,
    std::uint64_t source_manager_size,
    std::uint64_t build_cache_size,
    std::array<std::byte, manifest_size>& output) noexcept {

    if (!valid_transaction_name(transaction))
        return {status_code::invalid_argument};

    output.fill(std::byte{0});
    std::copy(manifest_magic.begin(), manifest_magic.end(), output.begin());
    write_u32(output, 8, baseline_format_version);
    write_u32(output, 12, endian_marker);
    write_u32(output, 16, static_cast<std::uint32_t>(manifest_size));
    write_u32(output, 20, 0);
    for (std::size_t index = 0; index < fingerprint.bytes.size(); ++index)
        output[24 + index] = static_cast<std::byte>(fingerprint.bytes[index]);
    write_u32(output, 56, static_cast<std::uint32_t>(transaction.size()));
    for (std::size_t index = 0; index < transaction.size(); ++index)
        output[64 + index] = static_cast<std::byte>(transaction[index]);
    write_u64(output, 128, compiled_size);
    write_u64(output, 136, source_manager_size);
    write_u64(output, 144, build_cache_size);

    if (configuration.available) {
        write_u64(
            output,
            152,
            static_cast<std::uint64_t>(
                configuration.observation.write_time_ticks));
        write_u64(
            output,
            160,
            static_cast<std::uint64_t>(
                configuration.observation.size));
        const auto identity_version =
            configuration.change_token_available
            ? 4u
            : (configuration.content_hash_available ? 2u : 1u);

        write_u32(output, 168, identity_version);
        write_u32(output, 172, configuration.project_version);
        write_u32(output, 176, configuration.abi_target);
        write_u32(output, 180, configuration.abi_pack);

        if (configuration.content_hash_available) {
            for (std::size_t index = 0;
                 index < configuration.content_hash.bytes.size();
                 ++index) {
                output[184 + index] =
                    configuration.content_hash.bytes[index];
            }
        }

        if (configuration.change_token_available) {
            const auto& token = configuration.change_token;
            if (!token)
                return {status_code::invalid_argument};

            write_u64(
                output,
                216,
                token.volume_serial);
            write_u64(
                output,
                224,
                token.file_reference);
            write_u64(
                output,
                232,
                static_cast<std::uint64_t>(
                    token.file_usn));
            write_u64(
                output,
                240,
                0);
        }
    }

    write_u64(output, manifest_crc_offset, persistence_crc64(std::span<const std::byte>{output}.first(manifest_crc_offset)));
    return {};
}

struct parsed_manifest final {
    baseline_fingerprint fingerprint{};
    baseline_configuration_state configuration{};
    std::string transaction;
    std::uint64_t compiled_size = 0;
    std::uint64_t source_manager_size = 0;
    std::uint64_t build_cache_size = 0;
};

[[nodiscard]] status parse_manifest(
    std::span<const std::byte> input,
    parsed_manifest& output) noexcept {

    output = {};
    if (input.size() != manifest_size)
        return {status_code::artifact_corrupt};
    if (!std::equal(manifest_magic.begin(), manifest_magic.end(), input.begin()))
        return {status_code::artifact_corrupt};
    if (read_u32(input, 8) != baseline_format_version ||
        read_u32(input, 12) != endian_marker ||
        read_u32(input, 16) != manifest_size ||
        read_u32(input, 20) != 0) {
        return {status_code::artifact_corrupt};
    }
    if (read_u64(input, manifest_crc_offset) != persistence_crc64(input.first(manifest_crc_offset)))
        return {status_code::artifact_corrupt};

    for (std::size_t index = 0; index < output.fingerprint.bytes.size(); ++index)
        output.fingerprint.bytes[index] = std::to_integer<std::uint8_t>(input[24 + index]);

    const auto transaction_size = read_u32(input, 56);
    if (transaction_size == 0 || transaction_size > 63)
        return {status_code::artifact_corrupt};

    try {
        output.transaction.resize(transaction_size);
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }

    for (std::size_t index = 0; index < transaction_size; ++index)
        output.transaction[index] = static_cast<char>(std::to_integer<unsigned char>(input[64 + index]));
    if (!valid_transaction_name(output.transaction))
        return {status_code::artifact_corrupt};

    output.compiled_size = read_u64(input, 128);
    output.source_manager_size = read_u64(input, 136);
    output.build_cache_size = read_u64(input, 144);

    const auto configuration_identity_version =
        read_u32(input, 168);
    if (configuration_identity_version > 4)
        return {status_code::artifact_corrupt};

    if (configuration_identity_version >= 1) {
        output.configuration.observation.write_time_ticks =
            static_cast<std::int64_t>(
                read_u64(input, 152));
        output.configuration.observation.size =
            static_cast<std::uintmax_t>(
                read_u64(input, 160));
        output.configuration.project_version =
            read_u32(input, 172);
        output.configuration.abi_target =
            read_u32(input, 176);
        output.configuration.abi_pack =
            read_u32(input, 180);
        output.configuration.available = true;
    }

    if (configuration_identity_version >= 2) {
        for (std::size_t index = 0;
             index < output.configuration.content_hash.bytes.size();
             ++index) {
            output.configuration.content_hash.bytes[index] =
                input[184 + index];
        }
        output.configuration.content_hash_available = true;
    }

    // v3 stored a volume-journal checkpoint. It remains readable but
    // intentionally uses the content-hash fallback until the next SAVE.
    if (configuration_identity_version >= 4) {
        file_change_token token;
        token.volume_serial =
            read_u64(input, 216);
        token.file_reference =
            read_u64(input, 224);
        token.file_usn =
            static_cast<std::int64_t>(
                read_u64(input, 232));

        if (!token || read_u64(input, 240) != 0)
            return {status_code::artifact_corrupt};

        output.configuration.change_token = token;
        output.configuration.change_token_available = true;
    }

    return {};
}

[[nodiscard]] status create_current_selector(
    std::string_view transaction,
    std::span<const std::byte, manifest_size> manifest,
    std::span<const std::byte> change_state,
    std::vector<std::byte>& output) noexcept {

    output.clear();

    if (!valid_transaction_name(transaction) ||
        transaction.size() > current_selector_transaction_capacity ||
        change_state.size() > current_selector_change_state_limit) {
        return {status_code::invalid_argument};
    }

    if (change_state.empty()) {
        try {
            output.assign(current_selector_size_v2, std::byte{0});
        }
        catch (const std::bad_alloc&) {
            return {status_code::not_available};
        }
        catch (const std::length_error&) {
            return {status_code::not_available};
        }

        std::copy(
            current_selector_magic_v2.begin(),
            current_selector_magic_v2.end(),
            output.begin());

        auto write_selector_u32 =
            [&output](std::size_t offset, std::uint32_t value) noexcept {
                output[offset + 0] = static_cast<std::byte>(value & 0xffu);
                output[offset + 1] = static_cast<std::byte>((value >> 8u) & 0xffu);
                output[offset + 2] = static_cast<std::byte>((value >> 16u) & 0xffu);
                output[offset + 3] = static_cast<std::byte>((value >> 24u) & 0xffu);
            };

        write_selector_u32(8, current_selector_version_v2);
        write_selector_u32(
            12,
            static_cast<std::uint32_t>(transaction.size()));

        for (std::size_t index = 0; index < transaction.size(); ++index) {
            output[16 + index] =
                static_cast<std::byte>(transaction[index]);
        }

        std::copy(
            manifest.begin(),
            manifest.end(),
            output.begin() + current_selector_header_size_v2);
        return {};
    }

    const auto total_size =
        current_selector_change_state_offset_v3 + change_state.size();

    try {
        output.assign(total_size, std::byte{0});
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }

    std::copy(
        current_selector_magic_v3.begin(),
        current_selector_magic_v3.end(),
        output.begin());

    auto write_selector_u32 =
        [&output](std::size_t offset, std::uint32_t value) noexcept {
            output[offset + 0] = static_cast<std::byte>(value & 0xffu);
            output[offset + 1] = static_cast<std::byte>((value >> 8u) & 0xffu);
            output[offset + 2] = static_cast<std::byte>((value >> 16u) & 0xffu);
            output[offset + 3] = static_cast<std::byte>((value >> 24u) & 0xffu);
        };

    auto write_selector_u64 =
        [&output](std::size_t offset, std::uint64_t value) noexcept {
            for (std::size_t byte = 0; byte < 8; ++byte) {
                output[offset + byte] =
                    static_cast<std::byte>(
                        (value >> (byte * 8)) & 0xffu);
            }
        };

    write_selector_u32(8, current_selector_version_v3);
    write_selector_u32(
        12,
        static_cast<std::uint32_t>(transaction.size()));
    write_selector_u64(
        16,
        static_cast<std::uint64_t>(change_state.size()));
    write_selector_u64(24, 0);

    for (std::size_t index = 0; index < transaction.size(); ++index) {
        output[32 + index] =
            static_cast<std::byte>(transaction[index]);
    }

    std::copy(
        manifest.begin(),
        manifest.end(),
        output.begin() + current_selector_manifest_offset_v3);

    std::copy(
        change_state.begin(),
        change_state.end(),
        output.begin() + current_selector_change_state_offset_v3);

    const auto embedded =
        std::span<const std::byte>{output}.subspan(
            current_selector_manifest_offset_v3,
            manifest_size);

    parsed_manifest validated_manifest;
    const auto validation =
        parse_manifest(embedded, validated_manifest);

    if (!validation.ok() ||
        validated_manifest.transaction != transaction) {
        output.clear();
        return {status_code::artifact_corrupt};
    }

    return {};
}


[[nodiscard]] status read_current_selector(
    const std::filesystem::path& root,
    std::string& transaction,
    std::array<std::byte, manifest_size>* manifest,
    bool* embedded_manifest_available = nullptr,
    std::vector<std::byte>* change_state = nullptr,
    bool* embedded_change_state_available = nullptr) noexcept {

    transaction.clear();

    if (manifest != nullptr)
        manifest->fill(std::byte{0});
    if (embedded_manifest_available != nullptr)
        *embedded_manifest_available = false;
    if (change_state != nullptr)
        change_state->clear();
    if (embedded_change_state_available != nullptr)
        *embedded_change_state_available = false;

    std::vector<std::byte> bytes;
    const auto result = read_small_file(
        root / current_name,
        current_selector_maximum_size,
        bytes);
    if (!result.ok())
        return result;

    const auto read_transaction =
        [&](std::size_t offset,
            std::size_t transaction_size) -> status {

            if (transaction_size == 0 ||
                transaction_size > current_selector_transaction_capacity ||
                offset + transaction_size > bytes.size()) {
                return {status_code::artifact_corrupt};
            }

            try {
                transaction.resize(transaction_size);
            }
            catch (const std::bad_alloc&) {
                return {status_code::not_available};
            }
            catch (const std::length_error&) {
                return {status_code::not_available};
            }

            for (std::size_t index = 0; index < transaction_size; ++index) {
                transaction[index] =
                    static_cast<char>(
                        std::to_integer<unsigned char>(
                            bytes[offset + index]));
            }

            return valid_transaction_name(transaction)
                ? status{}
                : status{status_code::artifact_corrupt};
        };

    if (bytes.size() >= current_selector_change_state_offset_v3 &&
        std::equal(
            current_selector_magic_v3.begin(),
            current_selector_magic_v3.end(),
            bytes.begin())) {

        if (read_u32(bytes, 8) != current_selector_version_v3 ||
            read_u64(bytes, 24) != 0) {
            return {status_code::artifact_corrupt};
        }

        const auto transaction_size = read_u32(bytes, 12);
        const auto change_state_size = read_u64(bytes, 16);

        if (change_state_size == 0 ||
            change_state_size > current_selector_change_state_limit ||
            change_state_size >
                (std::numeric_limits<std::size_t>::max)()) {
            return {status_code::artifact_corrupt};
        }

        const auto expected_size =
            current_selector_change_state_offset_v3 +
            static_cast<std::size_t>(change_state_size);

        if (bytes.size() != expected_size)
            return {status_code::artifact_corrupt};

        auto transaction_result =
            read_transaction(32, transaction_size);
        if (!transaction_result.ok())
            return transaction_result;

        const auto embedded =
            std::span<const std::byte>{bytes}.subspan(
                current_selector_manifest_offset_v3,
                manifest_size);

        parsed_manifest validated_manifest;
        const auto manifest_result =
            parse_manifest(embedded, validated_manifest);
        if (!manifest_result.ok() ||
            validated_manifest.transaction != transaction) {
            return {status_code::artifact_corrupt};
        }

        if (manifest != nullptr) {
            std::copy(
                embedded.begin(),
                embedded.end(),
                manifest->begin());
        }

        if (embedded_manifest_available != nullptr)
            *embedded_manifest_available = true;

        if (change_state != nullptr) {
            try {
                change_state->assign(
                    bytes.begin() + current_selector_change_state_offset_v3,
                    bytes.end());
            }
            catch (const std::bad_alloc&) {
                return {status_code::not_available};
            }
            catch (const std::length_error&) {
                return {status_code::not_available};
            }
        }

        if (embedded_change_state_available != nullptr)
            *embedded_change_state_available = true;

        return {};
    }

    if (bytes.size() == current_selector_size_v2 &&
        std::equal(
            current_selector_magic_v2.begin(),
            current_selector_magic_v2.end(),
            bytes.begin())) {

        if (read_u32(bytes, 8) != current_selector_version_v2)
            return {status_code::artifact_corrupt};

        const auto transaction_size = read_u32(bytes, 12);
        auto transaction_result =
            read_transaction(16, transaction_size);
        if (!transaction_result.ok())
            return transaction_result;

        const auto embedded =
            std::span<const std::byte>{bytes}.subspan(
                current_selector_header_size_v2,
                manifest_size);

        parsed_manifest validated_manifest;
        const auto manifest_result =
            parse_manifest(embedded, validated_manifest);
        if (!manifest_result.ok() ||
            validated_manifest.transaction != transaction) {
            return {status_code::artifact_corrupt};
        }

        if (manifest != nullptr) {
            std::copy(
                embedded.begin(),
                embedded.end(),
                manifest->begin());
        }

        if (embedded_manifest_available != nullptr)
            *embedded_manifest_available = true;

        return {};
    }

    while (!bytes.empty()) {
        const auto value =
            static_cast<char>(
                std::to_integer<unsigned char>(bytes.back()));
        if (value != '\n' && value != '\r')
            break;
        bytes.pop_back();
    }

    if (bytes.empty() || bytes.size() > 63)
        return {status_code::artifact_corrupt};

    try {
        transaction.resize(bytes.size());
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }

    for (std::size_t index = 0; index < bytes.size(); ++index) {
        transaction[index] =
            static_cast<char>(
                std::to_integer<unsigned char>(bytes[index]));
    }

    return valid_transaction_name(transaction)
        ? status{}
        : status{status_code::artifact_corrupt};
}


[[nodiscard]] status read_file_prefix(
    const std::filesystem::path& path,
    std::size_t requested,
    std::vector<std::byte>& output,
    std::uint64_t& file_size) noexcept {

    output.clear();
    file_size = 0;

#if defined(_WIN32)
    const auto handle = ::CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = ::GetLastError();
        return error == ERROR_FILE_NOT_FOUND ||
            error == ERROR_PATH_NOT_FOUND
            ? status{status_code::not_found}
            : status{status_code::io_failed};
    }

    LARGE_INTEGER native_size{};
    if (::GetFileSizeEx(handle, &native_size) == 0 ||
        native_size.QuadPart < 0) {
        ::CloseHandle(handle);
        return {status_code::io_failed};
    }

    file_size =
        static_cast<std::uint64_t>(
            native_size.QuadPart);

    const auto to_read =
        static_cast<std::size_t>(
            (std::min<std::uint64_t>)(
                file_size,
                static_cast<std::uint64_t>(requested)));

    try {
        output.resize(to_read);
    }
    catch (const std::bad_alloc&) {
        ::CloseHandle(handle);
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        ::CloseHandle(handle);
        return {status_code::not_available};
    }

    std::size_t offset = 0;
    while (offset < output.size()) {
        const auto remaining =
            output.size() - offset;
        const auto chunk =
            static_cast<DWORD>(
                (std::min<std::size_t>)(
                    remaining,
                    (std::numeric_limits<DWORD>::max)()));

        DWORD read = 0;
        if (::ReadFile(
                handle,
                output.data() + offset,
                chunk,
                &read,
                nullptr) == 0 ||
            read == 0) {
            ::CloseHandle(handle);
            output.clear();
            return {status_code::io_failed};
        }

        offset += static_cast<std::size_t>(read);
    }

    const auto closed = ::CloseHandle(handle) != 0;
    if (!closed) {
        output.clear();
        return {status_code::io_failed};
    }

    return {};
#else
    const auto handle =
        ::open(path.c_str(), O_RDONLY);
    if (handle < 0) {
        return errno == ENOENT
            ? status{status_code::not_found}
            : status{status_code::io_failed};
    }

    struct stat information {};
    if (::fstat(handle, &information) != 0 ||
        information.st_size < 0) {
        ::close(handle);
        return {status_code::io_failed};
    }

    file_size =
        static_cast<std::uint64_t>(
            information.st_size);

    const auto to_read =
        static_cast<std::size_t>(
            (std::min<std::uint64_t>)(
                file_size,
                static_cast<std::uint64_t>(requested)));

    try {
        output.resize(to_read);
    }
    catch (const std::bad_alloc&) {
        ::close(handle);
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        ::close(handle);
        return {status_code::not_available};
    }

    std::size_t offset = 0;
    while (offset < output.size()) {
        const auto result = ::read(
            handle,
            output.data() + offset,
            output.size() - offset);

        if (result < 0) {
            if (errno == EINTR)
                continue;

            ::close(handle);
            output.clear();
            return {status_code::io_failed};
        }

        if (result == 0) {
            ::close(handle);
            output.clear();
            return {status_code::io_failed};
        }

        offset += static_cast<std::size_t>(result);
    }

    if (::close(handle) != 0) {
        output.clear();
        return {status_code::io_failed};
    }

    return {};
#endif
}

[[nodiscard]] status read_current_ready_selector(
    const std::filesystem::path& root,
    std::string& transaction,
    std::array<std::byte, manifest_size>& manifest,
    bool& embedded_manifest_available) noexcept {

    transaction.clear();
    manifest.fill(std::byte{0});
    embedded_manifest_available = false;

    std::vector<std::byte> bytes;
    std::uint64_t file_size = 0;

    auto result = read_file_prefix(
        root / current_name,
        current_selector_change_state_offset_v3,
        bytes,
        file_size);
    if (!result.ok())
        return result;

    const auto read_transaction =
        [&](std::size_t offset,
            std::size_t transaction_size) -> status {

            if (transaction_size == 0 ||
                transaction_size >
                    current_selector_transaction_capacity ||
                offset + transaction_size >
                    bytes.size()) {
                return {status_code::artifact_corrupt};
            }

            try {
                transaction.resize(transaction_size);
            }
            catch (const std::bad_alloc&) {
                return {status_code::not_available};
            }
            catch (const std::length_error&) {
                return {status_code::not_available};
            }

            for (std::size_t index = 0;
                 index < transaction_size;
                 ++index) {
                transaction[index] =
                    static_cast<char>(
                        std::to_integer<unsigned char>(
                            bytes[offset + index]));
            }

            return valid_transaction_name(transaction)
                ? status{}
                : status{status_code::artifact_corrupt};
        };

    if (file_size >=
            current_selector_change_state_offset_v3 &&
        bytes.size() >=
            current_selector_change_state_offset_v3 &&
        std::equal(
            current_selector_magic_v3.begin(),
            current_selector_magic_v3.end(),
            bytes.begin())) {

        if (read_u32(bytes, 8) !=
                current_selector_version_v3 ||
            read_u64(bytes, 24) != 0) {
            return {status_code::artifact_corrupt};
        }

        const auto transaction_size =
            read_u32(bytes, 12);
        const auto change_state_size =
            read_u64(bytes, 16);

        if (change_state_size == 0 ||
            change_state_size >
                current_selector_change_state_limit ||
            change_state_size >
                (std::numeric_limits<std::uint64_t>::max)() -
                    current_selector_change_state_offset_v3 ||
            file_size !=
                current_selector_change_state_offset_v3 +
                    change_state_size) {
            return {status_code::artifact_corrupt};
        }

        result = read_transaction(
            32,
            transaction_size);
        if (!result.ok())
            return result;

        const auto embedded =
            std::span<const std::byte>{bytes}.subspan(
                current_selector_manifest_offset_v3,
                manifest_size);

        parsed_manifest validated_manifest;
        result = parse_manifest(
            embedded,
            validated_manifest);
        if (!result.ok() ||
            validated_manifest.transaction !=
                transaction) {
            return {status_code::artifact_corrupt};
        }

        std::copy(
            embedded.begin(),
            embedded.end(),
            manifest.begin());

        embedded_manifest_available = true;
        return {};
    }

    if (file_size == current_selector_size_v2 &&
        bytes.size() == current_selector_size_v2 &&
        std::equal(
            current_selector_magic_v2.begin(),
            current_selector_magic_v2.end(),
            bytes.begin())) {

        if (read_u32(bytes, 8) !=
            current_selector_version_v2) {
            return {status_code::artifact_corrupt};
        }

        result = read_transaction(
            16,
            read_u32(bytes, 12));
        if (!result.ok())
            return result;

        const auto embedded =
            std::span<const std::byte>{bytes}.subspan(
                current_selector_header_size_v2,
                manifest_size);

        parsed_manifest validated_manifest;
        result = parse_manifest(
            embedded,
            validated_manifest);
        if (!result.ok() ||
            validated_manifest.transaction !=
                transaction) {
            return {status_code::artifact_corrupt};
        }

        std::copy(
            embedded.begin(),
            embedded.end(),
            manifest.begin());

        embedded_manifest_available = true;
        return {};
    }

    if (file_size == 0 || file_size > 63 ||
        bytes.size() !=
            static_cast<std::size_t>(file_size)) {
        return {status_code::artifact_corrupt};
    }

    while (!bytes.empty()) {
        const auto value =
            static_cast<char>(
                std::to_integer<unsigned char>(
                    bytes.back()));

        if (value != '\n' && value != '\r')
            break;

        bytes.pop_back();
    }

    if (bytes.empty() || bytes.size() > 63)
        return {status_code::artifact_corrupt};

    try {
        transaction.resize(bytes.size());
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }

    for (std::size_t index = 0;
         index < bytes.size();
         ++index) {
        transaction[index] =
            static_cast<char>(
                std::to_integer<unsigned char>(
                    bytes[index]));
    }

    return valid_transaction_name(transaction)
        ? status{}
        : status{status_code::artifact_corrupt};
}

[[nodiscard]] status read_current_transaction(
    const std::filesystem::path& root,
    std::string& output) noexcept {

    return read_current_selector(
        root,
        output,
        nullptr,
        nullptr);
}

} // namespace

struct read_only_file_mapping::state final {
#if defined(_WIN32)
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
#else
    int file = -1;
#endif
    void* address = nullptr;
    std::size_t size = 0;

    ~state() noexcept {
#if defined(_WIN32)
        if (address != nullptr)
            ::UnmapViewOfFile(address);
        if (mapping != nullptr)
            ::CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE)
            ::CloseHandle(file);
#else
        if (address != nullptr && size != 0)
            ::munmap(address, size);
        if (file >= 0)
            ::close(file);
#endif
    }
};

read_only_file_mapping::read_only_file_mapping() noexcept = default;

read_only_file_mapping::~read_only_file_mapping() noexcept = default;

read_only_file_mapping::read_only_file_mapping(read_only_file_mapping&&) noexcept = default;
read_only_file_mapping& read_only_file_mapping::operator=(read_only_file_mapping&&) noexcept = default;

std::span<const std::byte> read_only_file_mapping::bytes() const noexcept {
    if (value == nullptr || value->size == 0)
        return {};
    return {static_cast<const std::byte*>(value->address), value->size};
}

bool read_only_file_mapping::open() const noexcept {
    return value != nullptr;
}

status read_only_file_mapping::map(const std::filesystem::path& path) noexcept {

    *this = {};
    try {
        auto state = std::make_unique<read_only_file_mapping::state>();
#if defined(_WIN32)
        state->file = ::CreateFileW(
            path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (state->file == INVALID_HANDLE_VALUE)
            return {status_code::io_failed};

        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(state->file, &size) || size.QuadPart < 0)
            return {status_code::io_failed};
        if (static_cast<std::uint64_t>(size.QuadPart) >
            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
            return {status_code::not_available};
        }
        state->size = static_cast<std::size_t>(size.QuadPart);
        if (state->size != 0) {
            state->mapping = ::CreateFileMappingW(
                state->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (state->mapping == nullptr)
                return {status_code::io_failed};
            state->address = ::MapViewOfFile(state->mapping, FILE_MAP_READ, 0, 0, 0);
            if (state->address == nullptr)
                return {status_code::io_failed};
        }
#else
        state->file = ::open(path.c_str(), O_RDONLY);
        if (state->file < 0)
            return errno == ENOENT ? status{status_code::not_found} : status{status_code::io_failed};
        struct stat information {};
        if (::fstat(state->file, &information) != 0 || information.st_size < 0)
            return {status_code::io_failed};
        if (static_cast<std::uint64_t>(information.st_size) >
            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
            return {status_code::not_available};
        }
        state->size = static_cast<std::size_t>(information.st_size);
        if (state->size != 0) {
            auto* address = ::mmap(nullptr, state->size, PROT_READ, MAP_PRIVATE, state->file, 0);
            if (address == MAP_FAILED)
                return {status_code::io_failed};
            state->address = address;
        }
#endif
        value = std::move(state);
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
}

std::span<const std::byte> baseline_snapshot::artifact(
    baseline_artifact_kind kind) const noexcept {

    switch (kind) {
    case baseline_artifact_kind::compiled:
        return compiled.bytes();

    case baseline_artifact_kind::source_manager: {
        const auto mapped_bytes = source_manager.bytes();
        if (source_manager_size_value == 0 ||
            source_manager_size_value > mapped_bytes.size()) {
            return mapped_bytes;
        }

        return mapped_bytes.first(
            static_cast<std::size_t>(
                source_manager_size_value));
    }

    case baseline_artifact_kind::change_state:
        return !embedded_change_state.empty()
            ? std::span<const std::byte>{
                embedded_change_state.data(),
                embedded_change_state.size()}
            : change_state.bytes();

    case baseline_artifact_kind::build_cache:
        if (packed_build_state &&
            packed_build_cache_enabled &&
            source_manager.open() &&
            source_manager_size_value <=
                source_manager.bytes().size() &&
            build_cache_size_value <=
                source_manager.bytes().size() -
                    source_manager_size_value) {

            return source_manager.bytes().subspan(
                static_cast<std::size_t>(
                    source_manager_size_value),
                static_cast<std::size_t>(
                    build_cache_size_value));
        }

        return build_cache.bytes();
    }

    return {};
}

bool baseline_snapshot::mapped(
    baseline_artifact_kind kind) const noexcept {

    switch (kind) {
    case baseline_artifact_kind::compiled:
        return compiled.open();
    case baseline_artifact_kind::source_manager:
        return source_manager.open();
    case baseline_artifact_kind::change_state:
        return !embedded_change_state.empty() ||
            change_state.open();
    case baseline_artifact_kind::build_cache:
        return
            (packed_build_state &&
             packed_build_cache_enabled &&
             source_manager.open()) ||
            build_cache.open();
    }
    return false;
}

std::filesystem::path baseline_store::root_path() const {
    return configuration_path.parent_path() /
        ".serverengine" /
        configuration_path.filename();
}

status baseline_store::open_current_decision(
    baseline_probe& probe,
    baseline_snapshot& output,
    baseline_open_telemetry* telemetry) const noexcept {

    probe = {};
    output = {};
    if (telemetry != nullptr)
        *telemetry = {};

    try {
        const auto root = root_path();

        std::string transaction;
        std::array<std::byte, manifest_size>
            embedded_manifest{};
        bool embedded_manifest_available = false;
        std::vector<std::byte> embedded_change_state;
        bool embedded_change_state_available = false;

        const auto current_read_begin =
            std::chrono::steady_clock::now();

        auto result = read_current_selector(
            root,
            transaction,
            &embedded_manifest,
            &embedded_manifest_available,
            &embedded_change_state,
            &embedded_change_state_available);

        if (telemetry != nullptr) {
            telemetry->current_read_ns =
                elapsed_ns(
                    current_read_begin,
                    std::chrono::steady_clock::now());
        }

        if (!result.ok())
            return result;

        const auto directory =
            root / transaction;

        const auto manifest_begin =
            std::chrono::steady_clock::now();

        parsed_manifest manifest;

        if (embedded_manifest_available) {
            const auto embedded_parse_begin =
                std::chrono::steady_clock::now();

            result = parse_manifest(
                embedded_manifest,
                manifest);

            if (telemetry != nullptr) {
                telemetry->embedded_manifest_parse_ns =
                    elapsed_ns(
                        embedded_parse_begin,
                        std::chrono::steady_clock::now());
            }
        }
        else {
            std::vector<std::byte> manifest_bytes;
            result = read_small_file(
                directory / manifest_name,
                manifest_size,
                manifest_bytes);
            if (!result.ok()) {
                return result.code == status_code::not_found
                    ? status{status_code::artifact_corrupt}
                    : result;
            }

            result = parse_manifest(
                manifest_bytes,
                manifest);
        }
        if (!result.ok())
            return result;

        if (manifest.transaction != transaction)
            return {status_code::artifact_corrupt};

        if (telemetry != nullptr) {
            telemetry->manifest_validation_ns =
                elapsed_ns(
                    manifest_begin,
                    std::chrono::steady_clock::now());
        }

        baseline_snapshot candidate;
        candidate.fingerprint_value =
            manifest.fingerprint;
        candidate.transaction_value =
            manifest.transaction;
        candidate.source_manager_size_value =
            manifest.source_manager_size;
        candidate.build_cache_size_value =
            manifest.build_cache_size;

        const auto compiled_map_begin =
            std::chrono::steady_clock::now();

        result = candidate.compiled.map(
            directory / compiled_name);

        if (telemetry != nullptr) {
            telemetry->compiled_map_ns =
                elapsed_ns(
                    compiled_map_begin,
                    std::chrono::steady_clock::now());
        }

        if (!result.ok()) {
            return result.code == status_code::not_found
                ? status{status_code::artifact_corrupt}
                : result;
        }

        if (embedded_change_state_available) {
            candidate.embedded_change_state =
                std::move(embedded_change_state);

            if (telemetry != nullptr)
                telemetry->change_state_map_ns = 0;
        }
        else {
            const auto change_state_map_begin =
                std::chrono::steady_clock::now();

            result = candidate.change_state.map(
                directory / change_state_name);

            if (telemetry != nullptr) {
                telemetry->change_state_map_ns =
                    elapsed_ns(
                        change_state_map_begin,
                        std::chrono::steady_clock::now());
            }

            if (!result.ok()) {
                return result.code == status_code::not_found
                    ? status{status_code::rebuild_required}
                    : result;
            }
        }

        const auto size_validation_begin =
            std::chrono::steady_clock::now();

        const bool size_mismatch =
            candidate.compiled.bytes().size() !=
                manifest.compiled_size;

        if (telemetry != nullptr) {
            telemetry->size_validation_ns =
                elapsed_ns(
                    size_validation_begin,
                    std::chrono::steady_clock::now());
        }

        if (size_mismatch)
            return {status_code::artifact_corrupt};

        probe.fingerprint = manifest.fingerprint;
        probe.configuration = manifest.configuration;
        probe.transaction = transaction;
        probe.source_manager_size = manifest.source_manager_size;
        probe.build_cache_size = manifest.build_cache_size;
        output = std::move(candidate);
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
    catch (const std::filesystem::filesystem_error&) {
        return {status_code::io_failed};
    }
}


status baseline_store::open_current_ready(
    baseline_probe& probe,
    baseline_snapshot& output,
    baseline_open_telemetry* telemetry) const noexcept {

    probe = {};
    output = {};

    if (telemetry != nullptr)
        *telemetry = {};

    try {
        const auto root = root_path();

        std::string transaction;
        std::array<std::byte, manifest_size>
            embedded_manifest{};
        bool embedded_manifest_available = false;

        const auto current_read_begin =
            std::chrono::steady_clock::now();

        auto result = read_current_ready_selector(
            root,
            transaction,
            embedded_manifest,
            embedded_manifest_available);

        if (telemetry != nullptr) {
            telemetry->current_read_ns =
                elapsed_ns(
                    current_read_begin,
                    std::chrono::steady_clock::now());
        }

        if (!result.ok())
            return result;

        const auto directory =
            root / transaction;

        const auto manifest_begin =
            std::chrono::steady_clock::now();

        parsed_manifest manifest;

        if (embedded_manifest_available) {
            const auto parse_begin =
                std::chrono::steady_clock::now();

            result = parse_manifest(
                embedded_manifest,
                manifest);

            if (telemetry != nullptr) {
                telemetry->embedded_manifest_parse_ns =
                    elapsed_ns(
                        parse_begin,
                        std::chrono::steady_clock::now());
            }
        }
        else {
            std::vector<std::byte> manifest_bytes;
            result = read_small_file(
                directory / manifest_name,
                manifest_size,
                manifest_bytes);

            if (!result.ok()) {
                return result.code ==
                    status_code::not_found
                    ? status{status_code::artifact_corrupt}
                    : result;
            }

            result = parse_manifest(
                manifest_bytes,
                manifest);
        }

        if (!result.ok())
            return result;

        if (manifest.transaction != transaction)
            return {status_code::artifact_corrupt};

        if (telemetry != nullptr) {
            telemetry->manifest_validation_ns =
                elapsed_ns(
                    manifest_begin,
                    std::chrono::steady_clock::now());
        }

        baseline_snapshot candidate;
        candidate.fingerprint_value =
            manifest.fingerprint;
        candidate.transaction_value =
            manifest.transaction;
        candidate.source_manager_size_value =
            manifest.source_manager_size;
        candidate.build_cache_size_value =
            manifest.build_cache_size;

        const auto map_begin =
            std::chrono::steady_clock::now();

        result = candidate.compiled.map(
            directory / compiled_name);

        if (telemetry != nullptr) {
            telemetry->compiled_map_ns =
                elapsed_ns(
                    map_begin,
                    std::chrono::steady_clock::now());
        }

        if (!result.ok()) {
            return result.code ==
                status_code::not_found
                ? status{status_code::artifact_corrupt}
                : result;
        }

        const auto validation_begin =
            std::chrono::steady_clock::now();

        const bool size_mismatch =
            candidate.compiled.bytes().size() !=
                manifest.compiled_size;

        if (telemetry != nullptr) {
            telemetry->size_validation_ns =
                elapsed_ns(
                    validation_begin,
                    std::chrono::steady_clock::now());
        }

        if (size_mismatch)
            return {status_code::artifact_corrupt};

        probe.fingerprint =
            manifest.fingerprint;
        probe.configuration =
            manifest.configuration;
        probe.transaction =
            transaction;
        probe.source_manager_size =
            manifest.source_manager_size;
        probe.build_cache_size =
            manifest.build_cache_size;

        output = std::move(candidate);
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
    catch (const std::filesystem::filesystem_error&) {
        return {status_code::io_failed};
    }
}

status baseline_store::probe(
    baseline_probe& output) const noexcept {

    output = {};

    try {
        std::string transaction;
        auto result =
            read_current_transaction(
                root_path(),
                transaction);
        if (!result.ok())
            return result;

        const auto directory =
            root_path() / transaction;

        std::vector<std::byte> manifest_bytes;
        result = read_small_file(
            directory / manifest_name,
            manifest_size,
            manifest_bytes);
        if (!result.ok()) {
            return result.code == status_code::not_found
                ? status{status_code::artifact_corrupt}
                : result;
        }

        parsed_manifest manifest;
        result = parse_manifest(
            manifest_bytes,
            manifest);
        if (!result.ok())
            return result;

        if (manifest.transaction != transaction)
            return {status_code::artifact_corrupt};

        output.fingerprint = manifest.fingerprint;
        output.configuration = manifest.configuration;
        output.transaction = std::move(transaction);
        output.source_manager_size = manifest.source_manager_size;
        output.build_cache_size = manifest.build_cache_size;
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
    catch (const std::filesystem::filesystem_error&) {
        return {status_code::io_failed};
    }
}

status baseline_store::open(
    const baseline_fingerprint& expected,
    baseline_snapshot& output) const noexcept {

    output = {};

    try {
        std::string transaction;
        const auto result =
            read_current_transaction(
                root_path(),
                transaction);
        if (!result.ok())
            return result;

        return open_selected(
            expected,
            transaction,
            true,
            false,
            true,
            output,
            nullptr);
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
    catch (const std::filesystem::filesystem_error&) {
        return {status_code::io_failed};
    }
}

status baseline_store::open_ready(
    const baseline_fingerprint& expected,
    baseline_snapshot& output) const noexcept {

    output = {};

    try {
        std::string transaction;
        const auto result =
            read_current_transaction(
                root_path(),
                transaction);
        if (!result.ok())
            return result;

        return open_selected(
            expected,
            transaction,
            true,
            false,
            false,
            output,
            nullptr);
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
    catch (const std::filesystem::filesystem_error&) {
        return {status_code::io_failed};
    }
}

status baseline_store::open_transaction(
    const baseline_fingerprint& expected,
    std::string_view transaction,
    baseline_snapshot& output,
    baseline_open_telemetry* telemetry) const noexcept {

    output = {};
    if (telemetry != nullptr)
        *telemetry = {};

    if (!valid_transaction_name(transaction))
        return {status_code::invalid_argument};

    return open_selected(
        expected,
        transaction,
        true,
        true,
        true,
        output,
        telemetry);
}

status baseline_store::open_transaction_ready(
    const baseline_fingerprint& expected,
    std::string_view transaction,
    baseline_snapshot& output,
    baseline_open_telemetry* telemetry) const noexcept {

    output = {};
    if (telemetry != nullptr)
        *telemetry = {};

    if (!valid_transaction_name(transaction))
        return {status_code::invalid_argument};

    return open_selected(
        expected,
        transaction,
        true,
        false,
        false,
        output,
        telemetry);
}


status baseline_store::open_transaction_decision(
    const baseline_fingerprint& expected,
    std::string_view transaction,
    baseline_snapshot& output,
    baseline_open_telemetry* telemetry) const noexcept {

    output = {};
    if (telemetry != nullptr)
        *telemetry = {};

    if (!valid_transaction_name(transaction))
        return {status_code::invalid_argument};

    // BUILD decision maps compiled.bin + source_manager.bin.
    // The logical Build Cache artifact remains deferred until dirty Sources
    // are confirmed.
    return open_selected(
        expected,
        transaction,
        true,
        false,
        false,
        output,
        telemetry);
}


status baseline_store::map_source_manager(
    const baseline_fingerprint& expected,
    std::string_view transaction,
    baseline_snapshot& snapshot,
    baseline_open_telemetry* telemetry) const noexcept {

    if (telemetry != nullptr)
        *telemetry = {};

    if (!valid_transaction_name(transaction) ||
        !snapshot.valid() ||
        snapshot.transaction() != transaction ||
        !(snapshot.fingerprint() == expected) ||
        snapshot.mapped(
            baseline_artifact_kind::source_manager)) {
        return {status_code::invalid_argument};
    }

    try {
        const auto directory =
            root_path() / std::string{transaction};

        std::vector<std::byte> manifest_bytes;
        auto result = read_small_file(
            directory / manifest_name,
            manifest_size,
            manifest_bytes);
        if (!result.ok())
            return result;

        parsed_manifest manifest;
        result = parse_manifest(
            manifest_bytes,
            manifest);
        if (!result.ok())
            return result;

        if (manifest.transaction != transaction ||
            !(manifest.fingerprint == expected)) {
            return {status_code::artifact_corrupt};
        }

        const auto begin =
            std::chrono::steady_clock::now();
        result = snapshot.source_manager.map(
            directory / source_manager_name);
        if (telemetry != nullptr) {
            telemetry->source_manager_map_ns =
                elapsed_ns(
                    begin,
                    std::chrono::steady_clock::now());
        }

        if (!result.ok())
            return result;

        snapshot.source_manager_size_value =
            manifest.source_manager_size;
        snapshot.build_cache_size_value =
            manifest.build_cache_size;

        const auto physical_size =
            static_cast<std::uint64_t>(
                snapshot.source_manager.bytes().size());

        const bool separate =
            physical_size ==
                manifest.source_manager_size;

        const bool can_pack =
            manifest.source_manager_size <=
                (std::numeric_limits<std::uint64_t>::max)() -
                    manifest.build_cache_size;

        const bool packed =
            can_pack &&
            physical_size ==
                manifest.source_manager_size +
                    manifest.build_cache_size;

        if (!separate && !packed) {
            snapshot.source_manager = {};
            snapshot.packed_build_state = false;
            snapshot.packed_build_cache_enabled = false;
            return {status_code::artifact_corrupt};
        }

        snapshot.packed_build_state = packed;
        snapshot.packed_build_cache_enabled = false;
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
    catch (const std::filesystem::filesystem_error&) {
        return {status_code::io_failed};
    }
}


status baseline_store::map_source_manager_cached(
    const baseline_fingerprint& expected,
    std::string_view transaction,
    std::uint64_t expected_source_manager_size,
    baseline_snapshot& snapshot,
    baseline_open_telemetry* telemetry) const noexcept {

    if (telemetry != nullptr)
        *telemetry = {};

    if (!valid_transaction_name(transaction) ||
        !snapshot.valid() ||
        snapshot.transaction() != transaction ||
        !(snapshot.fingerprint() == expected) ||
        !snapshot.mapped(baseline_artifact_kind::compiled) ||
        !snapshot.mapped(baseline_artifact_kind::change_state) ||
        snapshot.mapped(baseline_artifact_kind::source_manager)) {
        return {status_code::invalid_argument};
    }

    try {
        const auto directory = root_path() / std::string{transaction};
        const auto begin = std::chrono::steady_clock::now();
        auto result = snapshot.source_manager.map(directory / source_manager_name);
        if (telemetry != nullptr)
            telemetry->source_manager_map_ns = elapsed_ns(begin, std::chrono::steady_clock::now());
        if (!result.ok())
            return result.code == status_code::not_found ? status{status_code::artifact_corrupt} : result;

        const auto validation_begin =
            std::chrono::steady_clock::now();

        if (snapshot.source_manager_size_value == 0)
            snapshot.source_manager_size_value =
                expected_source_manager_size;

        const auto physical_size =
            static_cast<std::uint64_t>(
                snapshot.source_manager.bytes().size());

        const bool expected_matches =
            snapshot.source_manager_size_value ==
                expected_source_manager_size;

        const bool separate =
            physical_size ==
                expected_source_manager_size;

        const bool can_pack =
            expected_source_manager_size <=
                (std::numeric_limits<std::uint64_t>::max)() -
                    snapshot.build_cache_size_value;

        const bool packed =
            can_pack &&
            physical_size ==
                expected_source_manager_size +
                    snapshot.build_cache_size_value;

        const bool mismatch =
            !expected_matches ||
            (!separate && !packed);

        if (telemetry != nullptr) {
            telemetry->size_validation_ns =
                elapsed_ns(
                    validation_begin,
                    std::chrono::steady_clock::now());
        }

        if (mismatch) {
            snapshot.source_manager = {};
            snapshot.packed_build_state = false;
            snapshot.packed_build_cache_enabled = false;
            return {status_code::artifact_corrupt};
        }

        snapshot.packed_build_state = packed;
        snapshot.packed_build_cache_enabled = false;
        return {};
    }
    catch (const std::bad_alloc&) { return {status_code::not_available}; }
    catch (const std::length_error&) { return {status_code::not_available}; }
    catch (const std::filesystem::filesystem_error&) { return {status_code::io_failed}; }
}


status baseline_store::map_build_cache(
    const baseline_fingerprint& expected,
    std::string_view transaction,
    baseline_snapshot& snapshot,
    baseline_open_telemetry* telemetry) const noexcept {

    if (telemetry != nullptr)
        *telemetry = {};

    if (!valid_transaction_name(transaction) ||
        !snapshot.valid() ||
        snapshot.transaction() != transaction ||
        !(snapshot.fingerprint() == expected) ||
        snapshot.mapped(baseline_artifact_kind::build_cache)) {
        return {status_code::invalid_argument};
    }

    try {
        const auto directory =
            root_path() / std::string{transaction};

        const auto manifest_begin =
            std::chrono::steady_clock::now();

        std::vector<std::byte> manifest_bytes;
        auto result = read_small_file(
            directory / manifest_name,
            manifest_size,
            manifest_bytes);
        if (!result.ok()) {
            return result.code == status_code::not_found
                ? status{status_code::artifact_corrupt}
                : result;
        }

        parsed_manifest manifest;
        result = parse_manifest(manifest_bytes, manifest);
        if (!result.ok())
            return result;

        if (manifest.transaction != transaction ||
            !(manifest.fingerprint == expected)) {
            return {status_code::artifact_corrupt};
        }

        if (telemetry != nullptr) {
            telemetry->manifest_validation_ns =
                elapsed_ns(
                    manifest_begin,
                    std::chrono::steady_clock::now());
        }

        if (snapshot.packed_build_state &&
            snapshot.source_manager.open()) {

            const auto validation_begin =
                std::chrono::steady_clock::now();

            const auto mapped_size =
                static_cast<std::uint64_t>(
                    snapshot.source_manager.bytes().size());

            const bool size_ok =
                snapshot.source_manager_size_value ==
                    manifest.source_manager_size &&
                snapshot.build_cache_size_value ==
                    manifest.build_cache_size &&
                manifest.source_manager_size <=
                    (std::numeric_limits<std::uint64_t>::max)() -
                        manifest.build_cache_size &&
                mapped_size ==
                    manifest.source_manager_size +
                        manifest.build_cache_size;

            if (telemetry != nullptr) {
                telemetry->size_validation_ns =
                    elapsed_ns(
                        validation_begin,
                        std::chrono::steady_clock::now());
            }

            if (!size_ok)
                return {status_code::artifact_corrupt};

            snapshot.packed_build_cache_enabled = true;
            return {};
        }

        const auto map_begin =
            std::chrono::steady_clock::now();

        result = snapshot.build_cache.map(
            directory / build_cache_name);

        if (telemetry != nullptr) {
            telemetry->build_cache_map_ns =
                elapsed_ns(
                    map_begin,
                    std::chrono::steady_clock::now());
        }

        if (!result.ok()) {
            return result.code == status_code::not_found
                ? status{status_code::artifact_corrupt}
                : result;
        }

        const auto validation_begin =
            std::chrono::steady_clock::now();

        const bool mismatch =
            snapshot.build_cache.bytes().size() !=
            manifest.build_cache_size;

        if (telemetry != nullptr) {
            telemetry->size_validation_ns =
                elapsed_ns(
                    validation_begin,
                    std::chrono::steady_clock::now());
        }

        if (mismatch) {
            snapshot.build_cache = {};
            return {status_code::artifact_corrupt};
        }

        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
    catch (const std::filesystem::filesystem_error&) {
        return {status_code::io_failed};
    }
}

status baseline_store::map_build_cache_cached(
    const baseline_fingerprint& expected,
    std::string_view transaction,
    std::uint64_t expected_build_cache_size,
    baseline_snapshot& snapshot,
    baseline_open_telemetry* telemetry) const noexcept {

    if (telemetry != nullptr)
        *telemetry = {};

    if (!valid_transaction_name(transaction))
        return {status_code::invalid_argument};

    if (snapshot.transaction() != transaction ||
        !(snapshot.fingerprint() == expected) ||
        !snapshot.mapped(baseline_artifact_kind::compiled) ||
        !snapshot.mapped(baseline_artifact_kind::source_manager)) {
        return {status_code::invalid_argument};
    }

    try {
        if (snapshot.packed_build_state &&
            snapshot.source_manager.open()) {

            const auto validation_begin =
                std::chrono::steady_clock::now();

            const auto mapped_size =
                static_cast<std::uint64_t>(
                    snapshot.source_manager.bytes().size());

            const bool size_ok =
                expected_build_cache_size ==
                    snapshot.build_cache_size_value &&
                snapshot.source_manager_size_value <=
                    (std::numeric_limits<std::uint64_t>::max)() -
                        snapshot.build_cache_size_value &&
                mapped_size ==
                    snapshot.source_manager_size_value +
                        snapshot.build_cache_size_value;

            if (telemetry != nullptr) {
                telemetry->size_validation_ns =
                    elapsed_ns(
                        validation_begin,
                        std::chrono::steady_clock::now());
            }

            if (!size_ok)
                return {status_code::artifact_corrupt};

            snapshot.packed_build_cache_enabled = true;
            return {};
        }

        const auto directory =
            root_path() / std::string{transaction};

        const auto map_begin =
            std::chrono::steady_clock::now();

        auto result = snapshot.build_cache.map(
            directory / build_cache_name);

        if (telemetry != nullptr) {
            telemetry->build_cache_map_ns =
                elapsed_ns(
                    map_begin,
                    std::chrono::steady_clock::now());
        }

        if (!result.ok()) {
            return result.code == status_code::not_found
                ? status{status_code::artifact_corrupt}
                : result;
        }

        const auto validation_begin =
            std::chrono::steady_clock::now();

        const bool mismatch =
            snapshot.build_cache.bytes().size() !=
                expected_build_cache_size;

        if (telemetry != nullptr) {
            telemetry->size_validation_ns =
                elapsed_ns(
                    validation_begin,
                    std::chrono::steady_clock::now());
        }

        if (mismatch)
            return {status_code::artifact_corrupt};

        snapshot.packed_build_cache_enabled = false;
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
    catch (const std::filesystem::filesystem_error&) {
        return {status_code::io_failed};
    }
}


status baseline_store::open_selected(
    const baseline_fingerprint& expected,
    std::string_view transaction,
    bool include_source_manager,
    bool include_change_state,
    bool include_build_cache,
    baseline_snapshot& output,
    baseline_open_telemetry* telemetry) const noexcept {

    output = {};
    if (telemetry != nullptr)
        *telemetry = {};

    if (!valid_transaction_name(transaction))
        return {status_code::invalid_argument};

    try {
        const auto root = root_path();
        const auto directory =
            root / std::string{transaction};

        const auto manifest_begin =
            std::chrono::steady_clock::now();

        std::vector<std::byte> manifest_bytes;
        auto result = read_small_file(
            directory / manifest_name,
            manifest_size,
            manifest_bytes);

        if (!result.ok()) {
            return result.code == status_code::not_found
                ? status{status_code::artifact_corrupt}
                : result;
        }

        parsed_manifest manifest;
        result = parse_manifest(
            manifest_bytes,
            manifest);
        if (!result.ok())
            return result;

        if (manifest.transaction != transaction)
            return {status_code::artifact_corrupt};

        if (!(manifest.fingerprint == expected))
            return {status_code::rebuild_required};

        if (telemetry != nullptr) {
            telemetry->manifest_validation_ns =
                elapsed_ns(
                    manifest_begin,
                    std::chrono::steady_clock::now());
        }

        baseline_snapshot candidate;
        candidate.fingerprint_value =
            manifest.fingerprint;
        candidate.transaction_value =
            manifest.transaction;
        candidate.source_manager_size_value =
            manifest.source_manager_size;
        candidate.build_cache_size_value =
            manifest.build_cache_size;

        const auto compiled_map_begin =
            std::chrono::steady_clock::now();
        result = candidate.compiled.map(
            directory / compiled_name);
        if (telemetry != nullptr) {
            telemetry->compiled_map_ns =
                elapsed_ns(
                    compiled_map_begin,
                    std::chrono::steady_clock::now());
        }
        if (!result.ok()) {
            return result.code == status_code::not_found
                ? status{status_code::artifact_corrupt}
                : result;
        }

        if (include_source_manager) {
            const auto source_manager_map_begin =
                std::chrono::steady_clock::now();

            result = candidate.source_manager.map(
                directory / source_manager_name);

            if (telemetry != nullptr) {
                telemetry->source_manager_map_ns =
                    elapsed_ns(
                        source_manager_map_begin,
                        std::chrono::steady_clock::now());
            }

            if (!result.ok()) {
                return result.code == status_code::not_found
                    ? status{status_code::artifact_corrupt}
                    : result;
            }

            const auto physical_size =
                static_cast<std::uint64_t>(
                    candidate.source_manager.bytes().size());

            const bool separate =
                physical_size ==
                    manifest.source_manager_size;

            const bool can_pack =
                manifest.source_manager_size <=
                    (std::numeric_limits<std::uint64_t>::max)() -
                        manifest.build_cache_size;

            const bool packed =
                can_pack &&
                physical_size ==
                    manifest.source_manager_size +
                        manifest.build_cache_size;

            if (!separate && !packed)
                return {status_code::artifact_corrupt};

            candidate.packed_build_state = packed;
        }

        if (include_change_state) {
            const auto change_state_map_begin =
                std::chrono::steady_clock::now();
            result = candidate.change_state.map(
                directory / change_state_name);
            if (telemetry != nullptr) {
                telemetry->change_state_map_ns =
                    elapsed_ns(
                        change_state_map_begin,
                        std::chrono::steady_clock::now());
            }

            // change_state.bin was introduced after baseline format v1.
            // Generic LOAD/open remains backward-compatible with transactions
            // produced by the legacy three-artifact commit API. BUILD decision
            // validates presence explicitly in open_transaction_decision().
            if (!result.ok() &&
                result.code != status_code::not_found) {
                return result;
            }
        }

        if (include_build_cache) {
            if (candidate.packed_build_state &&
                candidate.source_manager.open()) {
                candidate.packed_build_cache_enabled = true;
            }
            else {
                const auto build_cache_map_begin =
                    std::chrono::steady_clock::now();

                result = candidate.build_cache.map(
                    directory / build_cache_name);

                if (telemetry != nullptr) {
                    telemetry->build_cache_map_ns =
                        elapsed_ns(
                            build_cache_map_begin,
                            std::chrono::steady_clock::now());
                }

                if (!result.ok()) {
                    return result.code == status_code::not_found
                        ? status{status_code::artifact_corrupt}
                        : result;
                }
            }
        }

        const auto size_validation_begin =
            std::chrono::steady_clock::now();

        const bool size_mismatch =
            candidate.compiled.bytes().size() !=
                manifest.compiled_size ||
            (include_source_manager &&
             candidate.artifact(
                 baseline_artifact_kind::source_manager).size() !=
                manifest.source_manager_size) ||
            (include_build_cache &&
             candidate.artifact(
                 baseline_artifact_kind::build_cache).size() !=
                manifest.build_cache_size);

        if (telemetry != nullptr) {
            telemetry->size_validation_ns =
                elapsed_ns(
                    size_validation_begin,
                    std::chrono::steady_clock::now());
        }

        if (size_mismatch)
            return {status_code::artifact_corrupt};

        output = std::move(candidate);
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
    catch (const std::filesystem::filesystem_error&) {
        return {status_code::io_failed};
    }
}

status baseline_store::commit(
    const baseline_fingerprint& fingerprint,
    const baseline_configuration_state& configuration,
    std::span<const std::byte> compiled,
    std::span<const std::byte> source_manager,
    std::span<const std::byte> change_state,
    std::span<const std::byte> build_cache,
    baseline_commit_result& output) const noexcept {

    output = {};
    try {
        const auto root = root_path();
        std::error_code error;
        std::filesystem::create_directories(root, error);
        if (error)
            return {status_code::persistence_failed};

        std::string transaction;
        std::filesystem::path directory;
        bool created = false;
        for (std::size_t attempt = 0; attempt < 1024; ++attempt) {
            transaction = transaction_name_candidate();
            directory = root / transaction;
            error.clear();
            if (std::filesystem::create_directory(directory, error)) {
                created = true;
                break;
            }
            if (error)
                return {status_code::persistence_failed};
        }
        if (!created)
            return {status_code::persistence_failed};

        const auto cleanup_failed_transaction = [&]() noexcept {
            std::error_code cleanup_error;
            std::filesystem::remove_all(directory, cleanup_error);
        };

        auto result =
            durable_write_file(
                directory / compiled_name,
                compiled);
        if (!result.ok()) {
            cleanup_failed_transaction();
            return {status_code::persistence_failed};
        }

        const bool pack_build_state =
            !change_state.empty() &&
            !source_manager.empty() &&
            !build_cache.empty();

        std::vector<std::byte> packed_source_manager;
        std::span<const std::byte> persisted_source_manager =
            source_manager;

        if (pack_build_state) {
            if (source_manager.size() >
                (std::numeric_limits<std::size_t>::max)() -
                    build_cache.size()) {
                cleanup_failed_transaction();
                return {status_code::not_available};
            }

            packed_source_manager.reserve(
                source_manager.size() +
                build_cache.size());

            packed_source_manager.insert(
                packed_source_manager.end(),
                source_manager.begin(),
                source_manager.end());

            packed_source_manager.insert(
                packed_source_manager.end(),
                build_cache.begin(),
                build_cache.end());

            persisted_source_manager = {
                packed_source_manager.data(),
                packed_source_manager.size()};
        }

        result = durable_write_file(
            directory / source_manager_name,
            persisted_source_manager);

        if (!result.ok()) {
            cleanup_failed_transaction();
            return {status_code::persistence_failed};
        }
        if (!change_state.empty()) {
            result = durable_write_file(
                directory / change_state_name,
                change_state);
            if (!result.ok()) {
                cleanup_failed_transaction();
                return {status_code::persistence_failed};
            }
        }

        // Packed production transactions already persist Build Cache as the
        // tail of source_manager.bin. Legacy three-artifact transactions keep
        // their standalone build_cache.bin for backward compatibility.
        if (!pack_build_state) {
            result = durable_write_file(
                directory / build_cache_name,
                build_cache);
            if (!result.ok()) {
                cleanup_failed_transaction();
                return {status_code::persistence_failed};
            }
        }

        std::array<std::byte, manifest_size> manifest{};
        result = create_manifest(
            fingerprint,
            configuration,
            transaction,
            static_cast<std::uint64_t>(compiled.size()),
            static_cast<std::uint64_t>(source_manager.size()),
            static_cast<std::uint64_t>(build_cache.size()),
            manifest);
        if (!result.ok()) {
            cleanup_failed_transaction();
            return result;
        }
        result = durable_write_file(directory / manifest_name, manifest);
        if (!result.ok()) {
            cleanup_failed_transaction();
            return {status_code::persistence_failed};
        }
        result = flush_directory(directory);
        if (!result.ok()) {
            cleanup_failed_transaction();
            return {status_code::persistence_failed};
        }
        result = flush_directory(root);
        if (!result.ok()) {
            cleanup_failed_transaction();
            return {status_code::persistence_failed};
        }

        std::vector<std::byte> selector_bytes;
        result = create_current_selector(
            transaction,
            manifest,
            change_state,
            selector_bytes);
        if (!result.ok()) {
            cleanup_failed_transaction();
            return result;
        }
        const auto selector =
            std::span<const std::byte>{
                selector_bytes.data(),
                selector_bytes.size()};
        const auto selector_temp = root / ("CURRENT.tmp-" + std::to_string(process_id()) + "-" +
            std::to_string(transaction_counter.fetch_add(1, std::memory_order_relaxed)));
        result = durable_write_file(selector_temp, selector);
        if (!result.ok()) {
            cleanup_failed_transaction();
            return {status_code::persistence_failed};
        }
        result = atomic_replace(selector_temp, root / current_name);
        if (!result.ok()) {
            std::filesystem::remove(selector_temp, error);
            cleanup_failed_transaction();
            return {status_code::persistence_failed};
        }
        // CURRENT replacement is the commit point. After it succeeds the new
        // transaction is authoritative; directory flush is a durability barrier,
        // not a reason to report the already-committed operation as failed.
        (void)flush_directory(root);

        output.transaction = transaction;
        output.bytes_written =
            static_cast<std::uint64_t>(compiled.size()) +
            static_cast<std::uint64_t>(source_manager.size()) +
            static_cast<std::uint64_t>(change_state.size()) +
            static_cast<std::uint64_t>(build_cache.size()) +
            manifest_size +
            static_cast<std::uint64_t>(selector_bytes.size());
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
    catch (const std::filesystem::filesystem_error&) {
        return {status_code::persistence_failed};
    }
}


status baseline_store::commit(
    const baseline_fingerprint& fingerprint,
    const baseline_configuration_state& configuration,
    std::span<const std::byte> compiled,
    std::span<const std::byte> source_manager,
    std::span<const std::byte> build_cache,
    baseline_commit_result& output) const noexcept {

    return commit(
        fingerprint,
        configuration,
        compiled,
        source_manager,
        {},
        build_cache,
        output);
}

status baseline_store::commit(
    const baseline_fingerprint& fingerprint,
    std::span<const std::byte> compiled,
    std::span<const std::byte> source_manager,
    std::span<const std::byte> build_cache,
    baseline_commit_result& output) const noexcept {

    return commit(
        fingerprint,
        baseline_configuration_state{},
        compiled,
        source_manager,
        build_cache,
        output);
}

status baseline_store::collect_garbage(std::string_view pinned_transaction) const noexcept {
    try {
        if (!pinned_transaction.empty() && !valid_transaction_name(pinned_transaction))
            return {status_code::invalid_argument};

        const auto root = root_path();
        std::error_code error;
        if (!std::filesystem::exists(root, error))
            return error ? status{status_code::io_failed} : status{};

        std::string current;
        const auto current_result = read_current_transaction(root, current);
        if (!current_result.ok() && current_result.code != status_code::not_found)
            return current_result;

        for (std::filesystem::directory_iterator iterator(root, error), end;
             !error && iterator != end;
             iterator.increment(error)) {
            const auto name = iterator->path().filename().string();
            if (!valid_transaction_name(name))
                continue;
            if (name == current || name == pinned_transaction)
                continue;
            if (!iterator->is_directory(error)) {
                if (error)
                    return {status_code::io_failed};
                continue;
            }
            std::filesystem::remove_all(iterator->path(), error);
            if (error)
                return {status_code::io_failed};
        }
        return error ? status{status_code::io_failed} : status{};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::filesystem::filesystem_error&) {
        return {status_code::io_failed};
    }
}

} // namespace cw::server
