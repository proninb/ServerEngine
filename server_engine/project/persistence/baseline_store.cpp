#include "baseline_store.hpp"

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
constexpr std::string_view build_cache_name = "build_cache.bin";
constexpr std::string_view manifest_name = "manifest.bin";
constexpr std::string_view current_name = "CURRENT";

std::atomic<std::uint64_t> transaction_counter{0};

[[nodiscard]] constexpr std::uint64_t crc64_update(
    std::uint64_t crc,
    std::byte input) noexcept {

    constexpr std::uint64_t polynomial = 0x42F0E1EBA9EA3693ULL;
    crc ^= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(input)) << 56;
    for (int bit = 0; bit < 8; ++bit)
        crc = (crc & 0x8000000000000000ULL) != 0 ? (crc << 1) ^ polynomial : crc << 1;
    return crc;
}

[[nodiscard]] std::uint64_t crc64(std::span<const std::byte> bytes) noexcept {
    std::uint64_t crc = 0;
    for (const auto value : bytes)
        crc = crc64_update(crc, value);
    return crc;
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
    try {
        std::error_code existence_error;
        if (!std::filesystem::exists(path, existence_error))
            return existence_error ? status{status_code::io_failed} : status{status_code::not_found};

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            return {status_code::io_failed};
        const auto end = file.tellg();
        if (end < 0)
            return {status_code::io_failed};
        const auto size = static_cast<std::uint64_t>(end);
        if (size > maximum)
            return {status_code::artifact_corrupt};
        output.resize(static_cast<std::size_t>(size));
        file.seekg(0, std::ios::beg);
        if (!output.empty()) {
            file.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
            if (!file)
                return {status_code::io_failed};
        }
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
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
    write_u64(output, manifest_crc_offset, crc64(std::span<const std::byte>{output}.first(manifest_crc_offset)));
    return {};
}

struct parsed_manifest final {
    baseline_fingerprint fingerprint{};
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
    if (read_u64(input, manifest_crc_offset) != crc64(input.first(manifest_crc_offset)))
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
    return {};
}

[[nodiscard]] status read_current_transaction(
    const std::filesystem::path& root,
    std::string& output) noexcept {

    std::vector<std::byte> bytes;
    const auto result = read_small_file(root / current_name, 128, bytes);
    if (!result.ok())
        return result;

    while (!bytes.empty()) {
        const auto value = static_cast<char>(std::to_integer<unsigned char>(bytes.back()));
        if (value != '\n' && value != '\r')
            break;
        bytes.pop_back();
    }
    if (bytes.empty() || bytes.size() > 63)
        return {status_code::artifact_corrupt};

    try {
        output.resize(bytes.size());
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }

    for (std::size_t index = 0; index < bytes.size(); ++index)
        output[index] = static_cast<char>(std::to_integer<unsigned char>(bytes[index]));
    return valid_transaction_name(output) ? status{} : status{status_code::artifact_corrupt};
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
};

read_only_file_mapping::read_only_file_mapping() noexcept = default;

read_only_file_mapping::~read_only_file_mapping() noexcept {
    if (value == nullptr)
        return;
#if defined(_WIN32)
    if (value->address != nullptr)
        ::UnmapViewOfFile(value->address);
    if (value->mapping != nullptr)
        ::CloseHandle(value->mapping);
    if (value->file != INVALID_HANDLE_VALUE)
        ::CloseHandle(value->file);
#else
    if (value->address != nullptr && value->size != 0)
        ::munmap(value->address, value->size);
    if (value->file >= 0)
        ::close(value->file);
#endif
}

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

std::span<const std::byte> baseline_snapshot::artifact(baseline_artifact_kind kind) const noexcept {
    switch (kind) {
        case baseline_artifact_kind::compiled: return compiled.bytes();
        case baseline_artifact_kind::source_manager: return source_manager.bytes();
        case baseline_artifact_kind::build_cache: return build_cache.bytes();
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
    case baseline_artifact_kind::build_cache:
        return build_cache.open();
    }
    return false;
}

std::filesystem::path baseline_store::root_path() const {
    return configuration_path.parent_path() /
        ".serverengine" /
        configuration_path.filename();
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
            output);
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
            false,
            output);
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
    baseline_snapshot& output) const noexcept {

    output = {};

    if (!valid_transaction_name(transaction))
        return {status_code::invalid_argument};

    return open_selected(
        expected,
        transaction,
        true,
        output);
}

status baseline_store::open_selected(
    const baseline_fingerprint& expected,
    std::string_view transaction,
    bool include_build_cache,
    baseline_snapshot& output) const noexcept {

    output = {};

    if (!valid_transaction_name(transaction))
        return {status_code::invalid_argument};

    try {
        const auto root = root_path();
        const auto directory =
            root / std::string{transaction};

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

        baseline_snapshot candidate;
        candidate.fingerprint_value =
            manifest.fingerprint;
        candidate.transaction_value =
            manifest.transaction;

        result = candidate.compiled.map(
            directory / compiled_name);
        if (!result.ok()) {
            return result.code == status_code::not_found
                ? status{status_code::artifact_corrupt}
                : result;
        }

        result = candidate.source_manager.map(
            directory / source_manager_name);
        if (!result.ok()) {
            return result.code == status_code::not_found
                ? status{status_code::artifact_corrupt}
                : result;
        }

        if (include_build_cache) {
            result = candidate.build_cache.map(
                directory / build_cache_name);
            if (!result.ok()) {
                return result.code == status_code::not_found
                    ? status{status_code::artifact_corrupt}
                    : result;
            }
        }

        if (candidate.compiled.bytes().size() !=
                manifest.compiled_size ||
            candidate.source_manager.bytes().size() !=
                manifest.source_manager_size ||
            (include_build_cache &&
             candidate.build_cache.bytes().size() !=
                manifest.build_cache_size)) {
            return {status_code::artifact_corrupt};
        }

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
    std::span<const std::byte> compiled,
    std::span<const std::byte> source_manager,
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

        auto result = durable_write_file(directory / compiled_name, compiled);
        if (!result.ok()) {
            cleanup_failed_transaction();
            return {status_code::persistence_failed};
        }
        result = durable_write_file(directory / source_manager_name, source_manager);
        if (!result.ok()) {
            cleanup_failed_transaction();
            return {status_code::persistence_failed};
        }
        result = durable_write_file(directory / build_cache_name, build_cache);
        if (!result.ok()) {
            cleanup_failed_transaction();
            return {status_code::persistence_failed};
        }

        std::array<std::byte, manifest_size> manifest{};
        result = create_manifest(
            fingerprint,
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

        const auto selector_text = transaction + "\n";
        const auto selector = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(selector_text.data()), selector_text.size()};
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
            static_cast<std::uint64_t>(build_cache.size()) + manifest_size + selector_text.size();
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
