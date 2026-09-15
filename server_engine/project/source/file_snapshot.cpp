#include "file_snapshot.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace cw::server {
namespace {


[[nodiscard]] bool observe(
    const std::filesystem::path& path,
    file_snapshot_observation& output,
    bool& missing) noexcept {

    missing = false;
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error) {
        if (error == std::errc::no_such_file_or_directory) {
            missing = true;
            return true;
        }
        return false;
    }
    if (!std::filesystem::exists(status)) {
        missing = true;
        return true;
    }
    if (!std::filesystem::is_regular_file(status))
        return false;

    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return false;
    const auto write_time = std::filesystem::last_write_time(path, error);
    if (error)
        return false;

    output.size = size;
    output.write_time_ticks = static_cast<std::int64_t>(write_time.time_since_epoch().count());
    return true;
}

#if defined(_WIN32)

[[nodiscard]] std::uint64_t windows_file_time_ticks(
    const FILETIME& value) noexcept {

    return
        (static_cast<std::uint64_t>(
            value.dwHighDateTime) << 32) |
        static_cast<std::uint64_t>(
            value.dwLowDateTime);
}

[[nodiscard]] bool compatible_windows_write_time_ticks(
    const std::filesystem::path& path,
    const FILETIME& native_time,
    std::int64_t& output) noexcept {

    using file_duration =
        std::filesystem::file_time_type::duration;

    // MSVC's filesystem clock uses native 100 ns FILETIME resolution.
    // Calibrate the epoch once instead of reopening every Source pathname.
    if constexpr (
        file_duration::period::num == 1 &&
        file_duration::period::den == 10'000'000) {

        constexpr auto unresolved =
            (std::numeric_limits<std::int64_t>::min)();

        static std::atomic<std::int64_t>
            epoch_offset{unresolved};

        const auto native_unsigned =
            windows_file_time_ticks(native_time);

        if (native_unsigned >
            static_cast<std::uint64_t>(
                (std::numeric_limits<std::int64_t>::max)())) {
            return false;
        }

        const auto native =
            static_cast<std::int64_t>(
                native_unsigned);

        auto offset =
            epoch_offset.load(
                std::memory_order_acquire);

        if (offset == unresolved) {
            std::error_code error;
            const auto observed =
                std::filesystem::last_write_time(
                    path,
                    error);

            if (error)
                return false;

            const auto observed_count =
                observed.time_since_epoch().count();

            const auto compatible =
                static_cast<std::int64_t>(
                    observed_count);

            const auto candidate =
                compatible - native;

            auto expected = unresolved;
            if (epoch_offset.compare_exchange_strong(
                    expected,
                    candidate,
                    std::memory_order_release,
                    std::memory_order_acquire)) {
                offset = candidate;
            }
            else {
                offset = expected;
            }
        }

        if ((offset > 0 &&
             native >
                (std::numeric_limits<std::int64_t>::max)() -
                    offset) ||
            (offset < 0 &&
             native <
                (std::numeric_limits<std::int64_t>::min)() -
                    offset)) {
            return false;
        }

        output = native + offset;
        return true;
    }
    else {
        // Non-MSVC Windows implementations may use another file_clock
        // period. Preserve their existing representation.
        std::error_code error;
        const auto observed =
            std::filesystem::last_write_time(
                path,
                error);

        if (error)
            return false;

        output =
            static_cast<std::int64_t>(
                observed.time_since_epoch().count());
        return true;
    }
}

[[nodiscard]] bool same_file_information(
    const BY_HANDLE_FILE_INFORMATION& left,
    const BY_HANDLE_FILE_INFORMATION& right) noexcept {

    return
        left.dwVolumeSerialNumber ==
            right.dwVolumeSerialNumber &&
        left.nFileIndexHigh ==
            right.nFileIndexHigh &&
        left.nFileIndexLow ==
            right.nFileIndexLow &&
        left.nFileSizeHigh ==
            right.nFileSizeHigh &&
        left.nFileSizeLow ==
            right.nFileSizeLow &&
        left.ftLastWriteTime.dwHighDateTime ==
            right.ftLastWriteTime.dwHighDateTime &&
        left.ftLastWriteTime.dwLowDateTime ==
            right.ftLastWriteTime.dwLowDateTime;
}

[[nodiscard]] file_snapshot_result
acquire_fresh_windows_snapshot(
    const std::filesystem::path& path,
    file_snapshot& output) noexcept {

    output = {};

    const auto handle = ::CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL |
            FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);

    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = ::GetLastError();

        if (error == ERROR_FILE_NOT_FOUND ||
            error == ERROR_PATH_NOT_FOUND) {
            return file_snapshot_result::missing;
        }

        if (error == ERROR_SHARING_VIOLATION ||
            error == ERROR_LOCK_VIOLATION) {
            return file_snapshot_result::changed_during_read;
        }

        return file_snapshot_result::failed;
    }

    const auto close_handle =
        [&]() noexcept {
            ::CloseHandle(handle);
        };

    BY_HANDLE_FILE_INFORMATION before{};
    if (::GetFileInformationByHandle(
            handle,
            &before) == 0) {
        close_handle();
        return file_snapshot_result::failed;
    }

    const auto native_size =
        (static_cast<std::uint64_t>(
            before.nFileSizeHigh) << 32) |
        static_cast<std::uint64_t>(
            before.nFileSizeLow);

    if (native_size >
        (std::numeric_limits<std::uint32_t>::max)()) {
        close_handle();
        return file_snapshot_result::failed;
    }

    try {
        std::string bytes(
            static_cast<std::size_t>(native_size),
            '\0');

        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto remaining =
                bytes.size() - offset;

            constexpr std::size_t maximum_chunk =
                1024u * 1024u * 1024u;

            const auto chunk =
                static_cast<DWORD>(
                    (std::min<std::size_t>)(
                        remaining,
                        maximum_chunk));

            DWORD read = 0;
            if (::ReadFile(
                    handle,
                    bytes.data() + offset,
                    chunk,
                    &read,
                    nullptr) == 0 ||
                read != chunk) {
                close_handle();
                return
                    file_snapshot_result::
                        changed_during_read;
            }

            offset +=
                static_cast<std::size_t>(read);
        }

        BY_HANDLE_FILE_INFORMATION after{};
        if (::GetFileInformationByHandle(
                handle,
                &after) == 0) {
            close_handle();
            return file_snapshot_result::failed;
        }

        if (!same_file_information(
                before,
                after)) {
            close_handle();
            return
                file_snapshot_result::
                    changed_during_read;
        }

        std::int64_t write_time_ticks = 0;
        if (!compatible_windows_write_time_ticks(
                path,
                after.ftLastWriteTime,
                write_time_ticks)) {
            close_handle();
            return file_snapshot_result::failed;
        }

        close_handle();

        output.identity.volume_serial =
            static_cast<std::uint64_t>(
                after.dwVolumeSerialNumber);
        output.identity.file_reference =
            (static_cast<std::uint64_t>(
                after.nFileIndexHigh) << 32) |
            static_cast<std::uint64_t>(
                after.nFileIndexLow);

        output.observation.size =
            static_cast<std::uintmax_t>(
                native_size);
        output.observation.write_time_ticks =
            write_time_ticks;
        output.hash =
            hash_source_content(bytes);
        output.bytes =
            std::move(bytes);

        return file_snapshot_result::acquired;
    }
    catch (const std::bad_alloc&) {
        close_handle();
        return
            file_snapshot_result::
                allocation_failed;
    }
    catch (const std::length_error&) {
        close_handle();
        return
            file_snapshot_result::
                allocation_failed;
    }
}

#endif


} // namespace

file_snapshot_result acquire_file_snapshot(
    const std::filesystem::path& path,
    const std::optional<file_snapshot_observation>& baseline,
    file_snapshot& output) noexcept {

#if defined(_WIN32)
    if (baseline.has_value()) {
        file_snapshot_observation observed{};
        bool missing = false;

        if (!observe(path, observed, missing))
            return file_snapshot_result::failed;
        if (missing)
            return file_snapshot_result::missing;
        if (*baseline == observed)
            return file_snapshot_result::unchanged;
    }

    // D4B: changed Windows Sources must preserve native identity from the
    // same stable handle that supplies the bytes published into the Generation.
    return acquire_fresh_windows_snapshot(
        path,
        output);
#else
    file_snapshot_observation before{};
    bool missing = false;
    if (!observe(path, before, missing))
        return file_snapshot_result::failed;
    if (missing)
        return file_snapshot_result::missing;
    if (baseline.has_value() && *baseline == before)
        return file_snapshot_result::unchanged;
    if (before.size > (std::numeric_limits<std::uint32_t>::max)())
        return file_snapshot_result::failed;

    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            return file_snapshot_result::failed;

        std::string bytes(static_cast<std::size_t>(before.size), '\0');
        if (!bytes.empty()) {
            stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (stream.gcount() != static_cast<std::streamsize>(bytes.size()))
                return file_snapshot_result::changed_during_read;
        }

        file_snapshot_observation after{};
        if (!observe(path, after, missing))
            return file_snapshot_result::failed;
        if (missing || after != before)
            return file_snapshot_result::changed_during_read;

        output.observation = after;
        output.hash = hash_source_content(bytes);
        output.bytes = std::move(bytes);
        return file_snapshot_result::acquired;
    }
    catch (const std::bad_alloc&) {
        return file_snapshot_result::allocation_failed;
    }
    catch (const std::length_error&) {
        return file_snapshot_result::allocation_failed;
    }
#endif
}

} // namespace cw::server
