#include "source_change_tracker.hpp"

#include "../persistence/build_cache_image.hpp"
#include "../persistence/source_manager_image.hpp"
#include "../persistence/change_state_image.hpp"
#include "source_manager.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <filesystem>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <winioctl.h>
#endif

namespace cw::server {
namespace {

[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

[[nodiscard]] std::size_t next_capacity(std::size_t count) noexcept {
    const auto quarter =
        count / 4 + (count % 4 != 0 ? 1u : 0u);

    if (count >
        (std::numeric_limits<std::size_t>::max)() -
            quarter - 1) {
        return 0;
    }

    // Keep identity tables below 80% load. For 100k Sources this selects
    // 131072 slots instead of 262144 without introducing sorting.
    const auto required =
        count + quarter + 1;

    std::size_t capacity = 16;
    while (capacity < required) {
        if (capacity >
            (std::numeric_limits<std::size_t>::max)() / 2) {
            return 0;
        }
        capacity *= 2;
    }

    return capacity;
}

class sparse_source_set final {
public:
    [[nodiscard]] status insert(source_id source, bool& inserted) noexcept {
        inserted = false;
        if (!source)
            return {status_code::invalid_argument};

        if (slots.empty() || (count + 1) * 2 >= slots.size()) {
            const auto requested =
                slots.empty() ? std::size_t{16} : slots.size() * 2;
            const auto result = grow(requested);
            if (!result.ok())
                return result;
        }

        const auto mask = slots.size() - 1;
        auto position =
            static_cast<std::size_t>(mix64(source.value())) & mask;

        for (;;) {
            auto& value = slots[position];
            if (value == 0) {
                value = source.value();
                ++count;
                inserted = true;
                return {};
            }
            if (value == source.value())
                return {};
            position = (position + 1) & mask;
        }
    }

private:
    [[nodiscard]] status grow(std::size_t capacity) noexcept {
        try {
            std::vector<std::uint32_t> replacement(capacity, 0);
            const auto mask = replacement.size() - 1;

            for (const auto value : slots) {
                if (value == 0)
                    continue;
                auto position =
                    static_cast<std::size_t>(mix64(value)) & mask;
                while (replacement[position] != 0)
                    position = (position + 1) & mask;
                replacement[position] = value;
            }

            slots.swap(replacement);
            return {};
        }
        catch (const std::bad_alloc&) {
            return {status_code::not_available};
        }
        catch (const std::length_error&) {
            return {status_code::not_available};
        }
    }

    std::vector<std::uint32_t> slots;
    std::size_t count = 0;
};

[[nodiscard]] std::uint64_t hash_text(std::string_view value) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto byte : value) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ULL;
    }
    return mix64(hash ^ static_cast<std::uint64_t>(value.size()));
}

class unique_path_set final {
public:
    struct entry final {
        std::string path;
        std::uint32_t flags = 0;
    };

    [[nodiscard]] status insert(
        std::filesystem::path value,
        std::uint32_t flags) noexcept {

        if ((flags & ~source_change_directory_watch_known) != 0 ||
            flags == 0) {
            return {status_code::invalid_argument};
        }

        try {
            value = value.lexically_normal();
            auto text = value.generic_string();
            if (text.empty())
                return {};

#ifdef _WIN32
            if (text.size() >= 2 &&
                text[1] == ':' &&
                text[0] >= 'A' &&
                text[0] <= 'Z') {
                text[0] =
                    static_cast<char>(
                        text[0] - 'A' + 'a');
            }
#endif

            if (slots.empty() ||
                (paths.size() + 1) * 2 >=
                    slots.size()) {

                const auto requested =
                    slots.empty()
                    ? std::size_t{16}
                    : slots.size() * 2;

                const auto result = grow(requested);
                if (!result.ok())
                    return result;
            }

            const auto hash = hash_text(text);
            const auto mask = slots.size() - 1;
            auto position =
                static_cast<std::size_t>(hash) & mask;

            for (;;) {
                const auto stored = slots[position];

                if (stored == 0) {
                    if (paths.size() >=
                        (std::numeric_limits<std::uint32_t>::max)()) {
                        return {status_code::not_available};
                    }

                    entry item;
                    item.path = std::move(text);
                    item.flags = flags;
                    paths.push_back(std::move(item));

                    slots[position] =
                        static_cast<std::uint32_t>(
                            paths.size());
                    return {};
                }

                const auto index =
                    static_cast<std::size_t>(
                        stored - 1);

                if (index < paths.size() &&
                    paths[index].path == text) {
                    paths[index].flags |= flags;
                    return {};
                }

                position =
                    (position + 1) & mask;
            }
        }
        catch (const std::bad_alloc&) {
            return {status_code::not_available};
        }
        catch (const std::length_error&) {
            return {status_code::not_available};
        }
        catch (const std::system_error&) {
            return {status_code::not_available};
        }
    }

    [[nodiscard]] const std::vector<entry>&
    values() const noexcept {
        return paths;
    }

private:
    [[nodiscard]] status grow(
        std::size_t capacity) noexcept {

        try {
            std::vector<std::uint32_t>
                replacement(capacity, 0);

            const auto mask =
                replacement.size() - 1;

            for (std::size_t index = 0;
                 index < paths.size();
                 ++index) {

                const auto hash =
                    hash_text(paths[index].path);

                auto position =
                    static_cast<std::size_t>(
                        hash) & mask;

                while (replacement[position] != 0) {
                    position =
                        (position + 1) & mask;
                }

                replacement[position] =
                    static_cast<std::uint32_t>(
                        index + 1);
            }

            slots.swap(replacement);
            return {};
        }
        catch (const std::bad_alloc&) {
            return {status_code::not_available};
        }
        catch (const std::length_error&) {
            return {status_code::not_available};
        }
    }

    std::vector<entry> paths;
    std::vector<std::uint32_t> slots;
};

[[nodiscard]] bool insert_file_identity(
    std::vector<source_change_file_index_slot>& slots,
    std::uint64_t file_reference,
    source_id source) noexcept {

    if (file_reference == 0 || slots.empty() || !source)
        return false;

    const auto mask = slots.size() - 1;
    auto position =
        static_cast<std::size_t>(
            mix64(file_reference)) & mask;

    for (std::size_t probe = 0;
         probe < slots.size();
         ++probe) {

        auto& slot = slots[position];
        if (slot.file_reference == 0) {
            slot.file_reference = file_reference;
            slot.source = source;
            return true;
        }

        if (slot.file_reference == file_reference) {
            return slot.source == source;
        }

        position = (position + 1) & mask;
    }

    return false;
}

[[nodiscard]] bool insert_directory_identity(
    std::vector<source_change_directory_index_slot>& slots,
    std::uint64_t file_reference,
    std::uint32_t flags) noexcept {

    if (file_reference == 0 ||
        slots.empty() ||
        flags == 0 ||
        (flags & ~source_change_directory_watch_known) != 0) {
        return false;
    }

    const auto mask = slots.size() - 1;
    auto position =
        static_cast<std::size_t>(
            mix64(file_reference)) & mask;

    for (std::size_t probe = 0;
         probe < slots.size();
         ++probe) {

        auto& slot = slots[position];

        if (slot.file_reference == 0) {
            slot.file_reference = file_reference;
            slot.flags = flags;
            slot.reserved = 0;
            return true;
        }

        if (slot.file_reference ==
            file_reference) {
            slot.flags |= flags;
            return true;
        }

        position = (position + 1) & mask;
    }

    return false;
}

struct observed_file_identity final {
    std::uint64_t volume_serial = 0;
    std::uint64_t file_reference = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return volume_serial != 0 &&
            file_reference != 0;
    }
};

[[nodiscard]] bool observe_regular_file(
    const std::filesystem::path& path,
    file_snapshot_observation& observation,
    bool& missing) noexcept {

    observation = {};
    missing = false;

    std::error_code error;
    const auto file_status =
        std::filesystem::status(path, error);

    if (error) {
        if (error ==
            std::errc::no_such_file_or_directory) {
            missing = true;
            return true;
        }
        return false;
    }

    if (!std::filesystem::exists(file_status)) {
        missing = true;
        return true;
    }

    if (!std::filesystem::is_regular_file(
            file_status)) {
        return false;
    }

    const auto size =
        std::filesystem::file_size(path, error);
    if (error)
        return false;

    const auto write_time =
        std::filesystem::last_write_time(
            path,
            error);
    if (error)
        return false;

    observation.size = size;
    observation.write_time_ticks =
        static_cast<std::int64_t>(
            write_time.time_since_epoch().count());
    return true;
}

#ifdef _WIN32

class windows_handle final {
public:
    windows_handle() noexcept = default;

    explicit windows_handle(HANDLE value) noexcept
        : handle(value) {}

    windows_handle(const windows_handle&) = delete;
    windows_handle& operator=(const windows_handle&) = delete;

    windows_handle(windows_handle&& other) noexcept
        : handle(std::exchange(other.handle, INVALID_HANDLE_VALUE)) {}

    windows_handle& operator=(windows_handle&& other) noexcept {
        if (this != &other) {
            reset();
            handle =
                std::exchange(
                    other.handle,
                    INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    ~windows_handle() {
        reset();
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle != INVALID_HANDLE_VALUE &&
            handle != nullptr;
    }

private:
    void reset() noexcept {
        if (*this)
            CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }

    HANDLE handle = INVALID_HANDLE_VALUE;
};

[[nodiscard]] bool query_file_identity(
    const std::filesystem::path& path,
    observed_file_identity& output) noexcept {

    output = {};

    try {
        const auto native = path.wstring();
        windows_handle file{
            CreateFileW(
                native.c_str(),
                FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ |
                    FILE_SHARE_WRITE |
                    FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS,
                nullptr)};

        if (!file)
            return false;

        BY_HANDLE_FILE_INFORMATION information{};
        if (GetFileInformationByHandle(
                file.get(),
                &information) == 0) {
            return false;
        }

        output.volume_serial =
            information.dwVolumeSerialNumber;
        output.file_reference =
            (static_cast<std::uint64_t>(
                information.nFileIndexHigh) << 32) |
            information.nFileIndexLow;

        return static_cast<bool>(output);
    }
    catch (...) {
        return false;
    }
}


struct observed_file_change_state final {
    std::uint64_t volume_serial = 0;
    std::uint64_t file_reference = 0;
    std::int64_t file_usn = -1;

    [[nodiscard]] explicit operator bool() const noexcept {
        return volume_serial != 0 &&
            file_reference != 0 &&
            file_usn >= 0;
    }
};

[[nodiscard]] status query_file_change_state(
    const std::filesystem::path& path,
    observed_file_change_state& output) noexcept {

    output = {};

    try {
        const auto native = path.wstring();
        windows_handle file{
            CreateFileW(
                native.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ |
                    FILE_SHARE_WRITE |
                    FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr)};

        if (!file) {
            const auto error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND ||
                error == ERROR_PATH_NOT_FOUND
                ? status{status_code::not_found}
                : status{status_code::io_failed};
        }

        BY_HANDLE_FILE_INFORMATION information{};
        if (GetFileInformationByHandle(
                file.get(),
                &information) == 0) {
            return {status_code::io_failed};
        }

        READ_FILE_USN_DATA request{};
        request.MinMajorVersion = 2;
        request.MaxMajorVersion = 2;

        std::array<std::byte, 4096> buffer{};
        DWORD returned = 0;

        if (DeviceIoControl(
                file.get(),
                FSCTL_READ_FILE_USN_DATA,
                &request,
                sizeof(request),
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &returned,
                nullptr) == 0) {
            const auto error = GetLastError();
            if (error == ERROR_INVALID_FUNCTION ||
                error == ERROR_NOT_SUPPORTED ||
                error == ERROR_INVALID_PARAMETER) {
                return {status_code::not_found};
            }
            return {status_code::io_failed};
        }

        if (returned < sizeof(USN_RECORD_V2))
            return {status_code::artifact_corrupt};

        USN_RECORD_V2 record{};
        std::memcpy(
            &record,
            buffer.data(),
            sizeof(record));

        if (record.MajorVersion != 2 ||
            record.RecordLength < sizeof(USN_RECORD_V2) ||
            record.RecordLength > returned ||
            record.Usn < 0) {
            return {status_code::artifact_corrupt};
        }

        const auto file_reference =
            (static_cast<std::uint64_t>(
                information.nFileIndexHigh) << 32) |
            information.nFileIndexLow;

        if (file_reference == 0 ||
            record.FileReferenceNumber != file_reference) {
            return {status_code::artifact_corrupt};
        }

        output.volume_serial =
            information.dwVolumeSerialNumber;
        output.file_reference =
            file_reference;
        output.file_usn =
            static_cast<std::int64_t>(record.Usn);

        return output
            ? status{}
            : status{status_code::not_found};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
    catch (const std::system_error&) {
        return {status_code::io_failed};
    }
}

[[nodiscard]] bool volume_paths(
    const std::filesystem::path& source,
    std::wstring& root,
    std::wstring& device) noexcept {

    try {
        const auto native = source.wstring();

        wchar_t root_buffer[MAX_PATH]{};
        if (GetVolumePathNameW(
                native.c_str(),
                root_buffer,
                static_cast<DWORD>(
                    std::size(root_buffer))) == 0) {
            return false;
        }

        root.assign(root_buffer);

        if (root.size() >= 2 &&
            root[1] == L':') {
            device = L"\\\\.\\";
            device.push_back(root[0]);
            device.push_back(L':');
            return true;
        }

        wchar_t volume_buffer[MAX_PATH]{};
        if (GetVolumeNameForVolumeMountPointW(
                root.c_str(),
                volume_buffer,
                static_cast<DWORD>(
                    std::size(volume_buffer))) == 0) {
            return false;
        }

        device.assign(volume_buffer);
        while (!device.empty() &&
               (device.back() == L'\\' ||
                device.back() == L'/')) {
            device.pop_back();
        }

        return !device.empty();
    }
    catch (...) {
        return false;
    }
}

[[nodiscard]] bool query_volume_serial(
    const std::wstring& root,
    std::uint64_t& output) noexcept {

    output = 0;
    DWORD serial = 0;
    wchar_t filesystem[16]{};
    if (GetVolumeInformationW(
            root.c_str(),
            nullptr,
            0,
            &serial,
            nullptr,
            nullptr,
            filesystem,
            static_cast<DWORD>(
                std::size(filesystem))) == 0) {
        return false;
    }

    const auto ntfs =
        (filesystem[0] == L'N' || filesystem[0] == L'n') &&
        (filesystem[1] == L'T' || filesystem[1] == L't') &&
        (filesystem[2] == L'F' || filesystem[2] == L'f') &&
        (filesystem[3] == L'S' || filesystem[3] == L's') &&
        filesystem[4] == L'\0';

    if (!ntfs)
        return false;

    output = serial;
    return output != 0;
}

[[nodiscard]] bool query_usn_checkpoint(
    const std::filesystem::path& path,
    std::uint64_t expected_volume,
    source_change_checkpoint& output) noexcept {

    output = {};

    std::wstring root;
    std::wstring device;
    if (!volume_paths(path, root, device))
        return false;

    std::uint64_t serial = 0;
    if (!query_volume_serial(root, serial) ||
        serial != expected_volume) {
        return false;
    }

    windows_handle volume{
        CreateFileW(
            device.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr)};

    if (!volume)
        return false;

    USN_JOURNAL_DATA_V0 journal{};
    DWORD returned = 0;

    if (DeviceIoControl(
            volume.get(),
            FSCTL_QUERY_USN_JOURNAL,
            nullptr,
            0,
            &journal,
            sizeof(journal),
            &returned,
            nullptr) == 0 ||
        returned < sizeof(journal)) {
        return false;
    }

    output.backend =
        source_change_backend::windows_usn;
    output.volume_serial = serial;
    output.journal_id = journal.UsnJournalID;
    output.next_usn =
        static_cast<std::int64_t>(journal.NextUsn);
    return true;
}

template <typename ChangeView>
[[nodiscard]] status read_usn_changes(
    const ChangeView& sources,
    const source_change_checkpoint& checkpoint,
    const file_change_token* configuration,
    std::vector<source_id>& dirty_sources,
    bool* configuration_proven,
    bool* configuration_changed,
    source_change_detection_telemetry& telemetry) noexcept {

    dirty_sources.clear();

    if (configuration_proven != nullptr)
        *configuration_proven = false;
    if (configuration_changed != nullptr)
        *configuration_changed = false;

    if (sources.source_count() == 0)
        return {};

    const auto first_path =
        sources.path(source_id{1});
    if (first_path.empty())
        return {status_code::not_found};

    std::filesystem::path first_path_value;
    try {
        first_path_value =
            std::filesystem::path{first_path};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
    catch (const std::system_error&) {
        return {status_code::not_available};
    }

    std::wstring root;
    std::wstring device;
    if (!volume_paths(
            first_path_value,
            root,
            device)) {
        return {status_code::not_found};
    }

    std::uint64_t serial = 0;
    if (!query_volume_serial(root, serial) ||
        serial != checkpoint.volume_serial) {
        return {status_code::not_found};
    }

    const bool configuration_candidate =
        configuration != nullptr &&
        static_cast<bool>(*configuration) &&
        configuration->volume_serial ==
            checkpoint.volume_serial;

    windows_handle volume{
        CreateFileW(
            device.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr)};

    if (!volume)
        return {status_code::not_found};

    USN_JOURNAL_DATA_V0 journal{};
    DWORD returned = 0;

    if (DeviceIoControl(
            volume.get(),
            FSCTL_QUERY_USN_JOURNAL,
            nullptr,
            0,
            &journal,
            sizeof(journal),
            &returned,
            nullptr) == 0 ||
        returned < sizeof(journal)) {
        return {status_code::not_found};
    }

    if (journal.UsnJournalID != checkpoint.journal_id ||
        checkpoint.next_usn < journal.FirstUsn ||
        checkpoint.next_usn > journal.NextUsn) {
        return {status_code::not_found};
    }

    telemetry.backend =
        source_change_backend::windows_usn;

    try {
        std::vector<std::byte> buffer(1024u * 1024u);
        sparse_source_set seen;

        auto start =
            static_cast<USN>(checkpoint.next_usn);
        const auto target = journal.NextUsn;

        while (start < target) {
            READ_USN_JOURNAL_DATA_V1 request{};
            request.StartUsn = start;
            request.ReasonMask =
                USN_REASON_DATA_OVERWRITE |
                USN_REASON_DATA_EXTEND |
                USN_REASON_DATA_TRUNCATION |
                USN_REASON_FILE_CREATE |
                USN_REASON_FILE_DELETE |
                USN_REASON_RENAME_OLD_NAME |
                USN_REASON_RENAME_NEW_NAME |
                USN_REASON_HARD_LINK_CHANGE |
                USN_REASON_REPARSE_POINT_CHANGE;
            request.ReturnOnlyOnClose = 0;
            request.Timeout = 0;
            request.BytesToWaitFor = 0;
            request.UsnJournalID = checkpoint.journal_id;
            request.MinMajorVersion = 2;
            request.MaxMajorVersion = 2;

            returned = 0;
            if (DeviceIoControl(
                    volume.get(),
                    FSCTL_READ_USN_JOURNAL,
                    &request,
                    sizeof(request),
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    &returned,
                    nullptr) == 0) {
                return {status_code::not_found};
            }

            if (returned < sizeof(USN))
                return {status_code::artifact_corrupt};

            USN next = 0;
            std::memcpy(
                &next,
                buffer.data(),
                sizeof(next));

            std::size_t offset = sizeof(USN);
            while (offset < returned) {
                if (returned - offset <
                    sizeof(USN_RECORD_V2)) {
                    return {status_code::artifact_corrupt};
                }

                USN_RECORD_V2 record{};
                std::memcpy(
                    &record,
                    buffer.data() + offset,
                    sizeof(record));

                if (record.RecordLength <
                        sizeof(USN_RECORD_V2) ||
                    record.RecordLength >
                        returned - offset ||
                    record.MajorVersion != 2) {
                    return {status_code::artifact_corrupt};
                }

                ++telemetry.journal_records;

                if ((record.FileAttributes &
                     FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    constexpr DWORD directory_topology_reasons =
                        USN_REASON_FILE_CREATE |
                        USN_REASON_FILE_DELETE |
                        USN_REASON_RENAME_OLD_NAME |
                        USN_REASON_RENAME_NEW_NAME |
                        USN_REASON_REPARSE_POINT_CHANGE;

                    const auto watch_flags =
                        sources.directory_watch_flags(
                            record.FileReferenceNumber);

                    if ((record.Reason &
                         directory_topology_reasons) != 0 &&
                        (watch_flags &
                         source_change_directory_watch_topology) != 0) {
                        dirty_sources.clear();
                        return {status_code::not_found};
                    }
                }
                else {
                    if (configuration_candidate &&
                        record.FileReferenceNumber ==
                            configuration->file_reference &&
                        configuration_changed != nullptr) {
                        *configuration_changed = true;
                    }

                    constexpr DWORD arrival_reasons =
                        USN_REASON_FILE_CREATE |
                        USN_REASON_RENAME_NEW_NAME;

                    const auto parent_watch_flags =
                        sources.directory_watch_flags(
                            record.ParentFileReferenceNumber);

                    if ((record.Reason & arrival_reasons) != 0 &&
                        (parent_watch_flags &
                         source_change_directory_watch_arrival) != 0) {
                        dirty_sources.clear();
                        return {status_code::not_found};
                    }

                    const auto source =
                        sources.find_source_file(
                            record.FileReferenceNumber);

                    if (source) {
                        constexpr DWORD topology_reasons =
                            USN_REASON_FILE_CREATE |
                            USN_REASON_FILE_DELETE |
                            USN_REASON_RENAME_OLD_NAME |
                            USN_REASON_RENAME_NEW_NAME |
                            USN_REASON_HARD_LINK_CHANGE |
                            USN_REASON_REPARSE_POINT_CHANGE;

                        if ((record.Reason &
                             topology_reasons) != 0) {
                            dirty_sources.clear();
                            return {status_code::not_found};
                        }

                        bool inserted = false;
                        const auto result =
                            seen.insert(
                                source,
                                inserted);
                        if (!result.ok()) {
                            dirty_sources.clear();
                            return result;
                        }

                        if (inserted) {
                            dirty_sources.push_back(source);
                            ++telemetry.matched_sources;
                        }
                    }
                }

                offset += record.RecordLength;
            }

            if (next <= start) {
                dirty_sources.clear();
                return {status_code::not_found};
            }

            start = next;
        }

        if (configuration_proven != nullptr)
            *configuration_proven = configuration_candidate;

        telemetry.fast_path = true;
        return {};
    }
    catch (const std::bad_alloc&) {
        dirty_sources.clear();
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        dirty_sources.clear();
        return {status_code::not_available};
    }
}


[[nodiscard]] status read_usn_gate(
    const change_state_image_view& sources,
    const source_change_checkpoint& checkpoint,
    const file_change_token* configuration,
    std::vector<source_change_journal_candidate>& candidates,
    bool* configuration_proven,
    bool* configuration_changed,
    source_change_detection_telemetry& telemetry) noexcept {

    candidates.clear();
    if (configuration_proven != nullptr) *configuration_proven = false;
    if (configuration_changed != nullptr) *configuration_changed = false;
    if (sources.source_count() == 0) return {};

    const auto first_path = sources.path(source_id{1});
    if (first_path.empty()) return {status_code::not_found};

    std::filesystem::path first_path_value;
    try { first_path_value = std::filesystem::path{first_path}; }
    catch (const std::bad_alloc&) { return {status_code::not_available}; }
    catch (const std::length_error&) { return {status_code::not_available}; }
    catch (const std::system_error&) { return {status_code::not_available}; }

    std::wstring root;
    std::wstring device;
    if (!volume_paths(first_path_value, root, device)) return {status_code::not_found};

    std::uint64_t serial = 0;
    if (!query_volume_serial(root, serial) || serial != checkpoint.volume_serial)
        return {status_code::not_found};

    const bool configuration_candidate =
        configuration != nullptr && static_cast<bool>(*configuration) &&
        configuration->volume_serial == checkpoint.volume_serial;

    windows_handle volume{CreateFileW(device.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)};
    if (!volume) return {status_code::not_found};

    USN_JOURNAL_DATA_V0 journal{};
    DWORD returned = 0;
    if (DeviceIoControl(volume.get(), FSCTL_QUERY_USN_JOURNAL, nullptr, 0,
            &journal, sizeof(journal), &returned, nullptr) == 0 ||
        returned < sizeof(journal)) return {status_code::not_found};

    if (journal.UsnJournalID != checkpoint.journal_id ||
        checkpoint.next_usn < journal.FirstUsn || checkpoint.next_usn > journal.NextUsn)
        return {status_code::not_found};

    telemetry.backend = source_change_backend::windows_usn;

    try {
        std::vector<std::byte> buffer(1024u * 1024u);
        auto start = static_cast<USN>(checkpoint.next_usn);
        const auto target = journal.NextUsn;

        while (start < target) {
            READ_USN_JOURNAL_DATA_V1 request{};
            request.StartUsn = start;
            request.ReasonMask = USN_REASON_DATA_OVERWRITE | USN_REASON_DATA_EXTEND |
                USN_REASON_DATA_TRUNCATION | USN_REASON_FILE_CREATE | USN_REASON_FILE_DELETE |
                USN_REASON_RENAME_OLD_NAME | USN_REASON_RENAME_NEW_NAME |
                USN_REASON_HARD_LINK_CHANGE | USN_REASON_REPARSE_POINT_CHANGE;
            request.UsnJournalID = checkpoint.journal_id;
            request.MinMajorVersion = 2;
            request.MaxMajorVersion = 2;

            returned = 0;
            if (DeviceIoControl(volume.get(), FSCTL_READ_USN_JOURNAL,
                    &request, sizeof(request), buffer.data(),
                    static_cast<DWORD>(buffer.size()), &returned, nullptr) == 0)
                return {status_code::not_found};
            if (returned < sizeof(USN)) return {status_code::artifact_corrupt};

            USN next = 0;
            std::memcpy(&next, buffer.data(), sizeof(next));
            std::size_t offset = sizeof(USN);

            while (offset < returned) {
                if (returned - offset < sizeof(USN_RECORD_V2))
                    return {status_code::artifact_corrupt};
                USN_RECORD_V2 record{};
                std::memcpy(&record, buffer.data() + offset, sizeof(record));
                if (record.RecordLength < sizeof(USN_RECORD_V2) ||
                    record.RecordLength > returned - offset || record.MajorVersion != 2)
                    return {status_code::artifact_corrupt};

                ++telemetry.journal_records;
                bool possible = false;

                if ((record.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    constexpr DWORD topology = USN_REASON_FILE_CREATE | USN_REASON_FILE_DELETE |
                        USN_REASON_RENAME_OLD_NAME | USN_REASON_RENAME_NEW_NAME |
                        USN_REASON_REPARSE_POINT_CHANGE;
                    possible = (record.Reason & topology) != 0 &&
                        sources.may_watch_directory_topology(record.FileReferenceNumber);
                } else {
                    if (configuration_candidate &&
                        record.FileReferenceNumber == configuration->file_reference &&
                        configuration_changed != nullptr) *configuration_changed = true;
                    constexpr DWORD arrival = USN_REASON_FILE_CREATE | USN_REASON_RENAME_NEW_NAME;
                    possible = ((record.Reason & arrival) != 0 &&
                        sources.may_watch_directory_arrival(record.ParentFileReferenceNumber)) ||
                        sources.may_contain_source_file(record.FileReferenceNumber);
                }

                if (possible) {
                    candidates.push_back({record.FileReferenceNumber,
                        record.ParentFileReferenceNumber, record.Reason, record.FileAttributes});
                }
                offset += record.RecordLength;
            }

            if (next <= start) { candidates.clear(); return {status_code::not_found}; }
            start = next;
        }

        if (configuration_proven != nullptr) *configuration_proven = configuration_candidate;
        telemetry.fast_path = true;
        return {};
    }
    catch (const std::bad_alloc&) { candidates.clear(); return {status_code::not_available}; }
    catch (const std::length_error&) { candidates.clear(); return {status_code::not_available}; }
}

[[nodiscard]] status resolve_candidates_exact(
    const source_manager_image_view& sources,
    std::span<const source_change_journal_candidate> candidates,
    std::vector<source_id>& dirty_sources,
    source_change_detection_telemetry& telemetry) noexcept {

    dirty_sources.clear();
    try {
        sparse_source_set seen;
        for (const auto& candidate : candidates) {
            if ((candidate.file_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                constexpr DWORD topology = USN_REASON_FILE_CREATE | USN_REASON_FILE_DELETE |
                    USN_REASON_RENAME_OLD_NAME | USN_REASON_RENAME_NEW_NAME |
                    USN_REASON_REPARSE_POINT_CHANGE;
                const auto flags = sources.directory_watch_flags(candidate.file_reference);
                if ((candidate.reason & topology) != 0 &&
                    (flags & source_change_directory_watch_topology) != 0) {
                    dirty_sources.clear();
                    return {status_code::not_found};
                }
                continue;
            }

            constexpr DWORD arrival = USN_REASON_FILE_CREATE | USN_REASON_RENAME_NEW_NAME;
            const auto parent_flags = sources.directory_watch_flags(candidate.parent_file_reference);
            if ((candidate.reason & arrival) != 0 &&
                (parent_flags & source_change_directory_watch_arrival) != 0) {
                dirty_sources.clear();
                return {status_code::not_found};
            }

            const auto source = sources.find_source_file(candidate.file_reference);
            if (!source) continue;

            constexpr DWORD topology = USN_REASON_FILE_CREATE | USN_REASON_FILE_DELETE |
                USN_REASON_RENAME_OLD_NAME | USN_REASON_RENAME_NEW_NAME |
                USN_REASON_HARD_LINK_CHANGE | USN_REASON_REPARSE_POINT_CHANGE;
            if ((candidate.reason & topology) != 0) {
                dirty_sources.clear();
                return {status_code::not_found};
            }

            bool inserted = false;
            const auto result = seen.insert(source, inserted);
            if (!result.ok()) { dirty_sources.clear(); return result; }
            if (inserted) { dirty_sources.push_back(source); ++telemetry.matched_sources; }
        }
        telemetry.fast_path = true;
        return {};
    }
    catch (const std::bad_alloc&) { dirty_sources.clear(); return {status_code::not_available}; }
    catch (const std::length_error&) { dirty_sources.clear(); return {status_code::not_available}; }
}

#endif

} // namespace

status capture_file_change_token(
    const std::filesystem::path& path,
    file_change_token& output) noexcept {

    output = {};

#ifndef _WIN32
    (void)path;
    return {status_code::not_found};
#else
    observed_file_change_state state;
    const auto result =
        query_file_change_state(path, state);
    if (!result.ok())
        return result;

    output.volume_serial = state.volume_serial;
    output.file_reference = state.file_reference;
    output.file_usn = state.file_usn;
    return {};
#endif
}

status prove_file_unchanged(
    const std::filesystem::path& path,
    const file_change_token& token,
    bool& unchanged,
    std::uint64_t* journal_records) noexcept {

    unchanged = false;
    if (journal_records != nullptr)
        *journal_records = 0;

    if (!token)
        return {status_code::not_found};

#ifndef _WIN32
    (void)path;
    return {status_code::not_found};
#else
    observed_file_change_state current;
    const auto result =
        query_file_change_state(path, current);

    if (!result.ok())
        return result;

    unchanged =
        current.volume_serial == token.volume_serial &&
        current.file_reference == token.file_reference &&
        current.file_usn == token.file_usn;

    return {};
#endif
}


status same_file_identity(
    const std::filesystem::path& path,
    const file_change_token& expected,
    bool& same) noexcept {

    same = false;
    if (!expected)
        return {status_code::invalid_argument};

#ifndef _WIN32
    (void)path;
    return {status_code::not_found};
#else
    observed_file_identity current;
    if (!query_file_identity(path, current))
        return {status_code::not_found};

    same =
        current.volume_serial == expected.volume_serial &&
        current.file_reference == expected.file_reference;
    return {};
#endif
}


status prepare_source_change_capture(
    const source_manager& sources,
    source_change_capture& output) noexcept {

    output.reset();

#ifndef _WIN32
    (void)sources;
    return {};
#else
    if (sources.source_count() == 0)
        return {};

    if (sources.source_count() >
        (std::numeric_limits<std::uint32_t>::max)()) {
        return {status_code::not_available};
    }

    try {
        source_snapshot first;
        source_id first_id;
        observed_file_identity first_identity;

        for (std::size_t index = 0;
             index < sources.source_count();
             ++index) {

            if (index >=
                (std::numeric_limits<std::uint32_t>::max)()) {
                return {status_code::not_available};
            }

            const source_id source{
                static_cast<std::uint32_t>(index + 1)};
            first = sources.current(source);
            if (!first)
                continue;

            if (!query_file_identity(
                    std::filesystem::path{
                        first.normalized_path()},
                    first_identity)) {
                output.reset();
                return {};
            }

            first_id = source;
            break;
        }

        if (!first_id || !first_identity) {
            output.reset();
            return {};
        }

        source_change_checkpoint checkpoint;
        if (!query_usn_checkpoint(
                std::filesystem::path{
                    first.normalized_path()},
                first_identity.volume_serial,
                checkpoint)) {
            output.reset();
            return {};
        }

        output.journal_anchor_path.assign(
            first.normalized_path());

        const auto file_capacity =
            next_capacity(sources.source_count());
        if (file_capacity == 0)
            return {status_code::not_available};

        output.file_index.assign(
            file_capacity,
            source_change_file_index_slot{});

        unique_path_set directories;

        for (std::size_t index = 0;
             index < sources.source_count();
             ++index) {

            const source_id source{
                static_cast<std::uint32_t>(index + 1)};
            const auto path_text =
                sources.path(source);
            if (path_text.empty()) {
                output.reset();
                return {};
            }

            const auto path =
                std::filesystem::path{path_text};
            const auto snapshot =
                sources.current(source);

            file_snapshot_observation observation;
            bool missing = false;
            if (!observe_regular_file(
                    path,
                    observation,
                    missing)) {
                output.reset();
                return {};
            }

            if (snapshot) {
                if (missing) {
                    output.reset();
                    return {status_code::rebuild_required};
                }

                file_snapshot verified;
                const auto acquisition =
                    acquire_file_snapshot(
                        path,
                        std::nullopt,
                        verified);

                if (acquisition ==
                        file_snapshot_result::allocation_failed) {
                    output.reset();
                    return {status_code::not_available};
                }

                if (acquisition !=
                        file_snapshot_result::acquired) {
                    output.reset();
                    return acquisition ==
                        file_snapshot_result::missing
                        ? status{status_code::rebuild_required}
                        : status{status_code::io_failed};
                }

                if (verified.hash != snapshot.hash()) {
                    output.reset();
                    return {status_code::rebuild_required};
                }

                observed_file_identity identity;
                if (!query_file_identity(
                        path,
                        identity) ||
                    identity.volume_serial !=
                        checkpoint.volume_serial) {
                    output.reset();
                    return {};
                }

                if (!insert_file_identity(
                        output.file_index,
                        identity.file_reference,
                        source)) {
                    output.reset();
                    return {};
                }
            }
            else if (!missing) {
                output.reset();
                return {status_code::rebuild_required};
            }

            const bool watch_arrival =
                !snapshot && missing;

            auto parent = path.parent_path();
            const auto root = parent.root_path();
            bool first_parent = true;

            while (!parent.empty()) {
                auto flags =
                    source_change_directory_watch_topology;

                if (first_parent && watch_arrival) {
                    flags |=
                        source_change_directory_watch_arrival;
                }

                const auto directory_result =
                    directories.insert(
                        parent,
                        flags);

                if (!directory_result.ok()) {
                    output.reset();
                    return directory_result;
                }

                first_parent = false;

                if (parent == root)
                    break;

                const auto next =
                    parent.parent_path();
                if (next == parent)
                    break;
                parent = next;
            }
        }

        const auto directory_capacity =
            next_capacity(
                directories.values().size());

        if (!directories.values().empty() &&
            directory_capacity == 0) {
            output.reset();
            return {status_code::not_available};
        }

        if (!directories.values().empty()) {
            output.directory_index.assign(
                directory_capacity,
                source_change_directory_index_slot{});
        }

        for (const auto& directory :
             directories.values()) {

            observed_file_identity identity;
            if (!query_file_identity(
                    std::filesystem::path{
                        directory.path},
                    identity) ||
                identity.volume_serial !=
                    checkpoint.volume_serial ||
                !insert_directory_identity(
                    output.directory_index,
                    identity.file_reference,
                    directory.flags)) {
                output.reset();
                return {};
            }
        }

        output.checkpoint = checkpoint;
        return {};
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
#endif
}

status resolve_source_change_candidates(
    const source_manager_image_view& sources,
    std::span<const source_change_journal_candidate> candidates,
    std::vector<source_id>& dirty_sources,
    source_change_detection_telemetry& telemetry) noexcept {
    dirty_sources.clear(); telemetry = {};
    if (!sources.valid()) return {status_code::invalid_state};
#ifndef _WIN32
    (void)candidates;
    return {status_code::not_found};
#else
    telemetry.backend = source_change_backend::windows_usn;
    return resolve_candidates_exact(sources, candidates, dirty_sources, telemetry);
#endif
}


status detect_source_changes(
    const source_manager_image_view& sources,
    std::vector<source_id>& dirty_sources,
    source_change_detection_telemetry& telemetry) noexcept {

    dirty_sources.clear();
    telemetry = {};

    const auto checkpoint =
        sources.change_checkpoint();

    if (!checkpoint) {
        telemetry.fallback = true;
        return {status_code::not_found};
    }

#ifdef _WIN32
    const auto result =
        read_usn_changes(
            sources,
            checkpoint,
            nullptr,
            dirty_sources,
            nullptr,
            nullptr,
            telemetry);

    if (!result.ok())
        telemetry.fallback = true;

    return result;
#else
    (void)sources;
    telemetry.fallback = true;
    return {status_code::not_found};
#endif
}


status detect_source_changes(
    const source_manager_image_view& sources,
    const file_change_token& configuration,
    std::vector<source_id>& dirty_sources,
    bool& configuration_proven,
    bool& configuration_changed,
    source_change_detection_telemetry& telemetry) noexcept {

    dirty_sources.clear();
    telemetry = {};
    configuration_proven = false;
    configuration_changed = false;

    const auto checkpoint =
        sources.change_checkpoint();

    if (!checkpoint) {
        telemetry.fallback = true;
        return {status_code::not_found};
    }

#ifdef _WIN32
    const auto result =
        read_usn_changes(
            sources,
            checkpoint,
            &configuration,
            dirty_sources,
            &configuration_proven,
            &configuration_changed,
            telemetry);

    if (!result.ok()) {
        configuration_proven = false;
        telemetry.fallback = true;
    }

    return result;
#else
    (void)sources;
    (void)configuration;
    telemetry.fallback = true;
    return {status_code::not_found};
#endif
}


status detect_source_changes(
    const change_state_image_view& sources,
    std::vector<source_change_journal_candidate>& candidates,
    source_change_detection_telemetry& telemetry) noexcept {
    candidates.clear(); telemetry = {};
    if (!sources.valid()) return {status_code::invalid_state};
    const auto checkpoint = sources.change_checkpoint();
    if (!checkpoint) return {status_code::not_found};
#ifndef _WIN32
    return {status_code::not_found};
#else
    if (!sources.gate_only()) return {status_code::not_found};
    const auto result = read_usn_gate(sources, checkpoint, nullptr, candidates,
        nullptr, nullptr, telemetry);
    if (!result.ok()) telemetry.fallback = true;
    return result;
#endif
}

status detect_source_changes(
    const change_state_image_view& sources,
    std::vector<source_id>& dirty_sources,
    source_change_detection_telemetry& telemetry) noexcept {
    dirty_sources.clear();
    if (!sources.gate_only()) {
        const auto checkpoint = sources.change_checkpoint();
        if (!checkpoint) return {status_code::not_found};
#ifndef _WIN32
        return {status_code::not_found};
#else
        const auto result = read_usn_changes(sources, checkpoint, nullptr, dirty_sources,
            nullptr, nullptr, telemetry);
        if (!result.ok()) telemetry.fallback = true;
        return result;
#endif
    }
    std::vector<source_change_journal_candidate> candidates;
    const auto result = detect_source_changes(sources, candidates, telemetry);
    if (!result.ok()) return result;
    return candidates.empty() ? status{} : status{status_code::not_found};
}



status detect_source_changes(
    const change_state_image_view& sources,
    const file_change_token& configuration,
    std::vector<source_change_journal_candidate>& candidates,
    bool& configuration_proven,
    bool& configuration_changed,
    source_change_detection_telemetry& telemetry) noexcept {
    candidates.clear(); telemetry = {}; configuration_proven = false; configuration_changed = false;
    if (!sources.valid()) return {status_code::invalid_state};
    const auto checkpoint = sources.change_checkpoint();
    if (!checkpoint) return {status_code::not_found};
#ifndef _WIN32
    (void)configuration;
    return {status_code::not_found};
#else
    if (!sources.gate_only()) return {status_code::not_found};
    const auto result = read_usn_gate(sources, checkpoint, &configuration, candidates,
        &configuration_proven, &configuration_changed, telemetry);
    if (!result.ok()) { configuration_proven = false; telemetry.fallback = true; }
    return result;
#endif
}

status detect_source_changes(
    const change_state_image_view& sources,
    const file_change_token& configuration,
    std::vector<source_id>& dirty_sources,
    bool& configuration_proven,
    bool& configuration_changed,
    source_change_detection_telemetry& telemetry) noexcept {
    dirty_sources.clear();
    if (!sources.gate_only()) {
        const auto checkpoint = sources.change_checkpoint();
        if (!checkpoint) return {status_code::not_found};
#ifndef _WIN32
        (void)configuration;
        return {status_code::not_found};
#else
        const auto result = read_usn_changes(sources, checkpoint, &configuration, dirty_sources,
            &configuration_proven, &configuration_changed, telemetry);
        if (!result.ok()) { configuration_proven = false; telemetry.fallback = true; }
        return result;
#endif
    }
    std::vector<source_change_journal_candidate> candidates;
    const auto result = detect_source_changes(sources, configuration, candidates,
        configuration_proven, configuration_changed, telemetry);
    if (!result.ok()) return result;
    return candidates.empty() ? status{} : status{status_code::not_found};
}



} // namespace cw::server
