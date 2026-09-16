#include "baseline_store.hpp"
#include "hard_link_policy.hpp"
#include "build_cache_image.hpp"
#include "source_manager_image.hpp"
#include "crc64_ecma.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
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
constexpr std::string_view source_manager_directory_name = "source_manager";
constexpr std::string_view source_manager_prefix_name = "prefix.bin";
constexpr std::string_view change_state_name = "change_state.bin";
constexpr std::string_view build_cache_name = "build_cache.bin";
constexpr std::string_view build_cache_directory_name = "build_cache";
constexpr std::string_view build_cache_prefix_name = "prefix.bin";
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

struct durable_write_telemetry final {
    std::uint64_t write_ns = 0;
    std::uint64_t flush_ns = 0;
};

constexpr std::size_t default_transaction_io_workers = 10;

// One executor owns all transaction I/O scheduling. Artifact tasks and
// section tasks share this pool; nested persistence code never creates threads.
struct transaction_io_batch final {
    std::atomic<std::size_t> remaining{0};
};

struct transaction_io_job final {
    void (*invoke)(const void*) noexcept = nullptr;
    const void* context = nullptr;
    transaction_io_batch* batch = nullptr;
};

class transaction_io_executor final {
public:
    explicit transaction_io_executor(
        std::size_t requested) noexcept {

        const auto target =
            (std::min)(
                requested != 0
                    ? requested
                    : default_transaction_io_workers,
                maximum_workers);

        worker_count_value = 1;

        for (std::size_t index = 1;
             index < target;
             ++index) {

            try {
                workers[launched] =
                    std::jthread(
                        [this]() noexcept {
                            worker_loop();
                        });
                ++launched;
                ++worker_count_value;
            }
            catch (const std::bad_alloc&) {
                break;
            }
            catch (const std::system_error&) {
                break;
            }
        }
    }

    ~transaction_io_executor() noexcept {
        {
            std::lock_guard lock{queue_mutex};
            stopping = true;
        }

        queue_condition.notify_all();

        for (std::size_t index = 0;
             index < launched;
             ++index) {

            if (workers[index].joinable())
                workers[index].join();
        }
    }

    transaction_io_executor(
        const transaction_io_executor&) = delete;
    transaction_io_executor& operator=(
        const transaction_io_executor&) = delete;

    [[nodiscard]] std::size_t worker_count() const noexcept {
        return worker_count_value;
    }

    [[nodiscard]] std::size_t peak_active() const noexcept {
        return peak_active_workers.load(
            std::memory_order_relaxed);
    }

    template <typename... Functions>
    void run(Functions&... functions) noexcept {
        transaction_io_batch batch;
        (submit(batch, functions), ...);
        wait(batch);
    }

    template <typename Function>
    void run_workers(
        std::size_t count,
        Function& function) noexcept {

        transaction_io_batch batch;

        for (std::size_t index = 0;
             index < count;
             ++index) {
            submit(batch, function);
        }

        wait(batch);
    }

private:
    static constexpr std::size_t maximum_workers = 16;
    static constexpr std::size_t queue_capacity = 128;

    template <typename Function>
    static void invoke_function(const void* context) noexcept {
        (*static_cast<const Function*>(context))();
    }

    template <typename Function>
    void submit(
        transaction_io_batch& batch,
        Function& function) noexcept {

        batch.remaining.fetch_add(
            1,
            std::memory_order_relaxed);

        transaction_io_job job{
            &transaction_io_executor::
                invoke_function<Function>,
            &function,
            &batch,
        };

        if (!enqueue(job))
            execute(job);
    }

    [[nodiscard]] bool enqueue(
        transaction_io_job job) noexcept {

        {
            std::lock_guard lock{queue_mutex};

            if (queue_size == queue_capacity)
                return false;

            queue[queue_tail] = job;
            queue_tail =
                (queue_tail + 1) %
                queue_capacity;
            ++queue_size;
        }

        queue_condition.notify_one();
        return true;
    }

    [[nodiscard]] bool try_take(
        transaction_io_job& output) noexcept {

        std::lock_guard lock{queue_mutex};

        if (queue_size == 0)
            return false;

        output = queue[queue_head];
        queue_head =
            (queue_head + 1) %
            queue_capacity;
        --queue_size;
        return true;
    }

    void execute(
        const transaction_io_job& job) noexcept {

        thread_local const transaction_io_executor*
            active_executor = nullptr;

        const auto* previous_executor =
            active_executor;
        const bool outermost =
            previous_executor != this;

        if (outermost) {
            active_executor = this;
            const auto candidate =
                active_workers.fetch_add(
                    1,
                    std::memory_order_acq_rel) + 1;
            update_peak_active(candidate);
        }

        job.invoke(job.context);

        if (outermost) {
            active_workers.fetch_sub(
                1,
                std::memory_order_release);
            active_executor =
                previous_executor;
        }

        if (job.batch->remaining.fetch_sub(
                1,
                std::memory_order_acq_rel) == 1) {
            job.batch->remaining.notify_all();
        }
    }

    void wait(
        transaction_io_batch& batch) noexcept {

        for (;;) {
            auto remaining =
                batch.remaining.load(
                    std::memory_order_acquire);

            if (remaining == 0)
                return;

            transaction_io_job job;
            if (try_take(job)) {
                execute(job);
                continue;
            }

            remaining =
                batch.remaining.load(
                    std::memory_order_acquire);

            if (remaining != 0) {
                batch.remaining.wait(
                    remaining,
                    std::memory_order_acquire);
            }
        }
    }

    void update_peak_active(
        std::size_t candidate) noexcept {

        auto current =
            peak_active_workers.load(
                std::memory_order_relaxed);

        while (candidate > current &&
               !peak_active_workers.compare_exchange_weak(
                   current,
                   candidate,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
    }

    void worker_loop() noexcept {
        for (;;) {
            transaction_io_job job;

            {
                std::unique_lock lock{queue_mutex};

                queue_condition.wait(
                    lock,
                    [this]() noexcept {
                        return stopping ||
                            queue_size != 0;
                    });

                if (stopping &&
                    queue_size == 0) {
                    return;
                }

                job = queue[queue_head];
                queue_head =
                    (queue_head + 1) %
                    queue_capacity;
                --queue_size;
            }

            execute(job);
        }
    }

    std::array<std::jthread, maximum_workers - 1>
        workers{};
    std::array<transaction_io_job, queue_capacity>
        queue{};

    std::mutex queue_mutex;
    std::condition_variable queue_condition;

    std::size_t queue_head = 0;
    std::size_t queue_tail = 0;
    std::size_t queue_size = 0;
    std::size_t launched = 0;
    std::size_t worker_count_value = 1;
    std::atomic<std::size_t> active_workers{0};
    std::atomic<std::size_t> peak_active_workers{0};
    bool stopping = false;
};

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
    const project_generation_segment& first,
    const project_generation_segment& second =
        project_generation_segment{},
    durable_write_telemetry* telemetry = nullptr) noexcept {

    if (telemetry != nullptr)
        *telemetry = {};
#if defined(_WIN32)
    const auto handle = ::CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (handle == INVALID_HANDLE_VALUE)
        return {status_code::io_failed};

    const auto write_begin =
        std::chrono::steady_clock::now();

    const project_generation_segment* segments[]{
        &first,
        &second,
    };

    for (const auto* segment : segments) {
        for (std::size_t extent_index = 0;
             extent_index < segment->extent_count();
             ++extent_index) {

            const auto bytes =
                segment->extent(extent_index);

            std::size_t offset = 0;
            while (offset < bytes.size()) {
                const auto remaining =
                    bytes.size() - offset;

                const auto chunk =
                    static_cast<DWORD>(
                        (std::min)(
                            remaining,
                            static_cast<std::size_t>(
                                (std::numeric_limits<DWORD>::max)())));

                DWORD written = 0;
                if (::WriteFile(
                        handle,
                        bytes.data() + offset,
                        chunk,
                        &written,
                        nullptr) == 0 ||
                    written != chunk) {

                    ::CloseHandle(handle);
                    return {status_code::io_failed};
                }

                offset += static_cast<std::size_t>(written);
            }
        }
    }

    if (telemetry != nullptr) {
        telemetry->write_ns =
            elapsed_ns(
                write_begin,
                std::chrono::steady_clock::now());
    }

    const auto flush_begin =
        std::chrono::steady_clock::now();
    const auto flushed =
        ::FlushFileBuffers(handle) != 0;

    if (telemetry != nullptr) {
        telemetry->flush_ns =
            elapsed_ns(
                flush_begin,
                std::chrono::steady_clock::now());
    }

    const auto closed =
        ::CloseHandle(handle) != 0;

    return flushed && closed
        ? status{}
        : status{status_code::io_failed};
#else
    const auto handle =
        ::open(
            path.c_str(),
            O_CREAT | O_TRUNC | O_WRONLY,
            0666);

    if (handle < 0)
        return {status_code::io_failed};

    const auto write_begin =
        std::chrono::steady_clock::now();

    const project_generation_segment* segments[]{
        &first,
        &second,
    };

    for (const auto* segment : segments) {
        for (std::size_t extent_index = 0;
             extent_index < segment->extent_count();
             ++extent_index) {

            const auto bytes =
                segment->extent(extent_index);

            std::size_t offset = 0;
            while (offset < bytes.size()) {
                const auto result =
                    ::write(
                        handle,
                        bytes.data() + offset,
                        bytes.size() - offset);

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

                offset +=
                    static_cast<std::size_t>(result);
            }
        }
    }

    if (telemetry != nullptr) {
        telemetry->write_ns =
            elapsed_ns(
                write_begin,
                std::chrono::steady_clock::now());
    }

    const auto flush_begin =
        std::chrono::steady_clock::now();
    const auto synced =
        ::fsync(handle) == 0;

    if (telemetry != nullptr) {
        telemetry->flush_ns =
            elapsed_ns(
                flush_begin,
                std::chrono::steady_clock::now());
    }

    const auto closed =
        ::close(handle) == 0;

    return synced && closed
        ? status{}
        : status{status_code::io_failed};
#endif
}

[[nodiscard]] status durable_write_file(
    const std::filesystem::path& path,
    std::span<const std::byte> first,
    std::span<const std::byte> second = {},
    durable_write_telemetry* telemetry = nullptr) noexcept {

    return durable_write_file(
        path,
        project_generation_segment{first},
        project_generation_segment{second},
        telemetry);
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

[[nodiscard]] status exact_file_equals_segment(
    const std::filesystem::path& path,
    const project_generation_segment& expected,
    bool& equal) noexcept {

    equal = false;

#if defined(_WIN32)
    const auto handle = ::CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ |
            FILE_SHARE_WRITE |
            FILE_SHARE_DELETE,
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
    if (::GetFileSizeEx(
            handle,
            &native_size) == 0 ||
        native_size.QuadPart < 0) {

        ::CloseHandle(handle);
        return {status_code::io_failed};
    }

    if (static_cast<std::uint64_t>(
            native_size.QuadPart) !=
        static_cast<std::uint64_t>(
            expected.size())) {

        const auto closed =
            ::CloseHandle(handle) != 0;

        return closed
            ? status{}
            : status{status_code::io_failed};
    }

    if (expected.empty()) {
        const auto closed =
            ::CloseHandle(handle) != 0;

        if (!closed)
            return {status_code::io_failed};

        equal = true;
        return {};
    }

    const auto mapping =
        ::CreateFileMappingW(
            handle,
            nullptr,
            PAGE_READONLY,
            0,
            0,
            nullptr);

    if (mapping == nullptr) {
        ::CloseHandle(handle);
        return {status_code::io_failed};
    }

    const auto mapped =
        ::MapViewOfFile(
            mapping,
            FILE_MAP_READ,
            0,
            0,
            0);

    if (mapped == nullptr) {
        ::CloseHandle(mapping);
        ::CloseHandle(handle);
        return {status_code::io_failed};
    }

    const auto* bytes =
        static_cast<const std::byte*>(
            mapped);

    std::size_t offset = 0;
    bool same = true;

    for (std::size_t index = 0;
         index < expected.extent_count();
         ++index) {

        const auto extent =
            expected.extent(index);

        if (!extent.empty() &&
            std::memcmp(
                bytes + offset,
                extent.data(),
                extent.size()) != 0) {

            same = false;
            break;
        }

        offset += extent.size();
    }

    const auto unmapped =
        ::UnmapViewOfFile(mapped) != 0;
    const auto mapping_closed =
        ::CloseHandle(mapping) != 0;
    const auto file_closed =
        ::CloseHandle(handle) != 0;

    if (!unmapped ||
        !mapping_closed ||
        !file_closed) {
        return {status_code::io_failed};
    }

    equal =
        same &&
        offset == expected.size();

    return {};
#else
    const auto handle =
        ::open(
            path.c_str(),
            O_RDONLY);

    if (handle < 0) {
        return errno == ENOENT
            ? status{status_code::not_found}
            : status{status_code::io_failed};
    }

    struct stat information {};
    if (::fstat(
            handle,
            &information) != 0 ||
        information.st_size < 0) {

        ::close(handle);
        return {status_code::io_failed};
    }

    if (static_cast<std::uint64_t>(
            information.st_size) !=
        static_cast<std::uint64_t>(
            expected.size())) {

        return ::close(handle) == 0
            ? status{}
            : status{status_code::io_failed};
    }

    if (expected.empty()) {
        if (::close(handle) != 0)
            return {status_code::io_failed};

        equal = true;
        return {};
    }

    auto* mapped =
        ::mmap(
            nullptr,
            expected.size(),
            PROT_READ,
            MAP_PRIVATE,
            handle,
            0);

    if (mapped == MAP_FAILED) {
        ::close(handle);
        return {status_code::io_failed};
    }

    const auto* bytes =
        static_cast<const std::byte*>(
            mapped);

    std::size_t offset = 0;
    bool same = true;

    for (std::size_t index = 0;
         index < expected.extent_count();
         ++index) {

        const auto extent =
            expected.extent(index);

        if (!extent.empty() &&
            std::memcmp(
                bytes + offset,
                extent.data(),
                extent.size()) != 0) {

            same = false;
            break;
        }

        offset += extent.size();
    }

    const auto unmapped =
        ::munmap(
            mapped,
            expected.size()) == 0;
    const auto closed =
        ::close(handle) == 0;

    if (!unmapped || !closed)
        return {status_code::io_failed};

    equal =
        same &&
        offset == expected.size();

    return {};
#endif
}


struct source_manager_storage_section final {
    std::uint32_t record_size = 0;
    std::uint64_t count = 0;
    std::uint64_t offset = 0;
    std::uint64_t byte_count = 0;
    std::uint64_t crc64 = 0;
};

struct sectioned_source_manager_write_telemetry final {
    std::uint64_t link_ns = 0;
    std::uint64_t compare_ns = 0;
    std::uint64_t compare_bytes = 0;
    std::uint64_t provenance_reused_bytes = 0;
    std::uint64_t io_wall_ns = 0;
    std::uint64_t directory_flush_ns = 0;
    std::uint64_t written_bytes = 0;
    std::uint64_t reused_bytes = 0;
    std::uint32_t written_sections = 0;
    std::uint32_t reused_sections = 0;
    std::uint32_t compare_sections = 0;
    std::uint32_t provenance_reused_sections = 0;
    std::uint32_t hard_link_fallback_sections = 0;
    std::uint32_t io_worker_count = 0;
    bool sectioned = false;
};

[[nodiscard]] std::filesystem::path
source_manager_section_path(
    const std::filesystem::path& directory,
    std::size_t index) {

    return directory /
        ("section-" +
         std::to_string(index + 1) +
         ".bin");
}

[[nodiscard]] bool parse_source_manager_prefix(
    std::span<const std::byte> prefix,
    std::uint64_t expected_size,
    std::array<
        source_manager_storage_section,
        source_manager_image_directory_count>&
        output) noexcept {

    output = {};

    if (prefix.size() !=
        source_manager_image_prefix_size ||
        read_u32(prefix, 8) !=
            source_manager_image_format_version ||
        read_u32(prefix, 12) != endian_marker ||
        read_u32(prefix, 16) !=
            source_manager_image_header_size ||
        read_u32(prefix, 20) !=
            source_manager_image_directory_count ||
        read_u32(prefix, 24) !=
            source_manager_image_directory_entry_size ||
        read_u64(prefix, 32) !=
            source_manager_image_header_size ||
        read_u64(prefix, 40) !=
            expected_size) {
        return false;
    }

    std::uint64_t previous_end =
        source_manager_image_prefix_size;

    for (std::size_t index = 0;
         index <
            source_manager_image_directory_count;
         ++index) {

        const auto entry_offset =
            source_manager_image_header_size +
            index *
                source_manager_image_directory_entry_size;

        if (entry_offset +
                source_manager_image_directory_entry_size >
            prefix.size()) {
            return false;
        }

        const auto raw_kind =
            read_u32(prefix, entry_offset);
        const auto record_size =
            read_u32(prefix, entry_offset + 4);
        const auto offset =
            read_u64(prefix, entry_offset + 8);
        const auto count =
            read_u64(prefix, entry_offset + 16);
        const auto crc =
            read_u64(prefix, entry_offset + 24);

        if (raw_kind != index + 1 ||
            record_size == 0) {
            return false;
        }

        const auto aligned =
            (previous_end + 63u) &
            ~std::uint64_t{63u};

        if (offset != aligned)
            return false;

        if (count != 0 &&
            record_size >
                (std::numeric_limits<
                    std::uint64_t>::max)() /
                    count) {
            return false;
        }

        const auto byte_count =
            count * record_size;

        if (offset >
            (std::numeric_limits<
                std::uint64_t>::max)() -
                byte_count) {
            return false;
        }

        const auto end =
            offset + byte_count;

        if (end > expected_size)
            return false;

        output[index] = {
            record_size,
            count,
            offset,
            byte_count,
            crc,
        };

        previous_end = end;
    }

    return previous_end == expected_size;
}

[[nodiscard]] bool copy_segment_prefix(
    const project_generation_segment& input,
    std::span<std::byte> output) noexcept {

    std::size_t copied = 0;

    for (std::size_t extent_index = 0;
         extent_index < input.extent_count() &&
         copied < output.size();
         ++extent_index) {

        const auto extent =
            input.extent(extent_index);

        const auto count =
            (std::min)(
                output.size() - copied,
                extent.size());

        if (count != 0) {
            std::memcpy(
                output.data() + copied,
                extent.data(),
                count);
            copied += count;
        }
    }

    return copied == output.size();
}

[[nodiscard]] bool slice_generation_segment(
    const project_generation_segment& input,
    std::uint64_t offset,
    std::uint64_t size,
    project_generation_segment& output) noexcept {

    output = {};

    if (offset >
            input.size() ||
        size >
            input.size() - offset ||
        offset >
            (std::numeric_limits<
                std::size_t>::max)() ||
        size >
            (std::numeric_limits<
                std::size_t>::max)()) {
        return false;
    }

    const auto begin =
        static_cast<std::size_t>(
            offset);
    const auto end =
        begin +
        static_cast<std::size_t>(
            size);

    std::size_t logical = 0;

    for (std::size_t extent_index = 0;
         extent_index <
            input.extent_count();
         ++extent_index) {

        const auto extent =
            input.extent(extent_index);
        const auto extent_begin =
            logical;
        const auto extent_end =
            logical + extent.size();

        if (extent_end > begin &&
            extent_begin < end) {

            const auto local_begin =
                begin > extent_begin
                    ? begin - extent_begin
                    : std::size_t{0};

            const auto local_end =
                end < extent_end
                    ? end - extent_begin
                    : extent.size();

            if (local_end > local_begin &&
                !output.append(
                    extent.subspan(
                        local_begin,
                        local_end -
                            local_begin))) {
                return false;
            }
        }

        logical = extent_end;

        if (logical >= end)
            break;
    }

    return output.size() ==
        static_cast<std::size_t>(size);
}

[[nodiscard]] status durable_write_sectioned_source_manager(
    const std::filesystem::path& transaction_directory,
    const std::filesystem::path& previous_directory,
    const project_generation_segment& image,
    const std::array<
        baseline_section_provenance,
        10>& provenance,
    transaction_io_executor& io_executor,
    durable_write_telemetry& io,
    sectioned_source_manager_write_telemetry&
        detail) noexcept {

    io = {};
    detail = {};

    if (image.size() <
        source_manager_image_prefix_size) {
        return {status_code::not_available};
    }

    std::array<
        std::byte,
        source_manager_image_prefix_size>
        prefix_storage{};

    if (!copy_segment_prefix(
            image,
            prefix_storage)) {
        return {status_code::not_available};
    }

    const auto prefix =
        std::span<const std::byte>{
            prefix_storage};

    std::array<
        source_manager_storage_section,
        source_manager_image_directory_count>
        current{};

    const auto logical_size =
        static_cast<std::uint64_t>(
            image.size());

    if (!parse_source_manager_prefix(
            prefix,
            logical_size,
            current)) {
        return {status_code::not_available};
    }

    const auto section_directory =
        transaction_directory /
        source_manager_directory_name;

    std::error_code error;
    if (!std::filesystem::create_directory(
            section_directory,
            error) ||
        error) {
        return {status_code::persistence_failed};
    }

    const auto cleanup = [&]() noexcept {
        std::error_code cleanup_error;
        std::filesystem::remove_all(
            section_directory,
            cleanup_error);
    };

    std::array<
        source_manager_storage_section,
        source_manager_image_directory_count>
        previous{};

    bool previous_available = false;

    if (!previous_directory.empty()) {
        std::vector<std::byte> previous_prefix;

        const auto previous_result =
            read_small_file(
                previous_directory /
                    source_manager_prefix_name,
                source_manager_image_prefix_size,
                previous_prefix);

        if (!previous_result.ok()) {
            cleanup();
            return previous_result.code ==
                    status_code::not_found
                ? status{status_code::artifact_corrupt}
                : previous_result;
        }

        if (previous_prefix.size() !=
            source_manager_image_prefix_size) {
            cleanup();
            return {status_code::artifact_corrupt};
        }

        const auto previous_size =
            read_u64(
                std::span<const std::byte>{
                    previous_prefix},
                40);

        if (!parse_source_manager_prefix(
                previous_prefix,
                previous_size,
                previous)) {
            cleanup();
            return {status_code::artifact_corrupt};
        }

        previous_available = true;
    }

    std::array<
        project_generation_segment,
        source_manager_image_directory_count>
        pending_segments{};

    std::array<
        std::size_t,
        source_manager_image_directory_count>
        pending_sections{};

    std::size_t pending_count = 0;

    for (std::size_t index = 0;
         index <
            source_manager_image_directory_count;
         ++index) {

        const auto& value =
            current[index];

        project_generation_segment
            section_segment;

        if (!slice_generation_segment(
                image,
                value.offset,
                value.byte_count,
                section_segment)) {
            cleanup();
            return {status_code::not_available};
        }

        bool reused = false;

        // D4L1: a valid proof means this logical output section is literally
        // the same mapped whole section file owned by a pinned immutable
        // baseline. Pointer identity of the sliced Generation section is an
        // additional commit-side check; no CRC or memcmp proves equality again.
        if (index < provenance.size()) {
            const auto& proof =
                provenance[index];

            const auto proven_bytes =
                proof.bytes();

            const bool direct_borrow =
                proof.valid() &&
                proof.artifact() ==
                    baseline_artifact_kind::source_manager &&
                proof.section() == index &&
                proof.owner() != nullptr &&
                proof.owner()->
                    validate_section_borrow(proof) &&
                section_segment.extent_count() == 1 &&
                section_segment.extent(0).data() ==
                    proven_bytes.data() &&
                section_segment.extent(0).size() ==
                    proven_bytes.size() &&
                proven_bytes.size() ==
                    value.byte_count;

            if (direct_borrow) {
                const auto source =
                    transaction_directory.parent_path() /
                    std::string{
                        proof.owner()->transaction()} /
                    source_manager_directory_name /
                    ("section-" +
                     std::to_string(index + 1) +
                     ".bin");
                const auto target =
                    source_manager_section_path(
                        section_directory,
                        index);

                const auto link_begin =
                    std::chrono::
                        steady_clock::now();

                std::error_code link_error;
                std::filesystem::
                    create_hard_link(
                        source,
                        target,
                        link_error);

                detail.link_ns +=
                    elapsed_ns(
                        link_begin,
                        std::chrono::
                            steady_clock::now());

                if (!link_error) {
                    reused = true;
                    ++detail.reused_sections;
                    detail.reused_bytes +=
                        value.byte_count;
                    ++detail.provenance_reused_sections;
                    detail.provenance_reused_bytes +=
                        value.byte_count;
                }
                else if (classify_hard_link_failure(
                             link_error) ==
                         hard_link_failure_action::rewrite) {
                    ++detail.hard_link_fallback_sections;
                }
                else {
                    cleanup();
                    return {status_code::io_failed};
                }
            }
        }

        if (!reused && previous_available) {
            const auto& old =
                previous[index];

            const bool metadata_equal =
                old.record_size ==
                    value.record_size &&
                old.count ==
                    value.count &&
                old.byte_count ==
                    value.byte_count &&
                old.crc64 ==
                    value.crc64;

            if (metadata_equal) {
                const auto source =
                    source_manager_section_path(
                        previous_directory,
                        index);
                const auto target =
                    source_manager_section_path(
                        section_directory,
                        index);

                bool exact_equal = false;

                const auto compare_begin =
                    std::chrono::
                        steady_clock::now();

                const auto compare_result =
                    exact_file_equals_segment(
                        source,
                        section_segment,
                        exact_equal);

                detail.compare_ns +=
                    elapsed_ns(
                        compare_begin,
                        std::chrono::
                            steady_clock::now());
                detail.compare_bytes +=
                    value.byte_count;
                ++detail.compare_sections;

                if (!compare_result.ok()) {
                    cleanup();
                    return compare_result.code ==
                            status_code::not_found
                        ? status{status_code::artifact_corrupt}
                        : compare_result;
                }

                if (exact_equal) {

                    const auto link_begin =
                        std::chrono::
                            steady_clock::now();

                    std::error_code link_error;
                    std::filesystem::
                        create_hard_link(
                            source,
                            target,
                            link_error);

                    detail.link_ns +=
                        elapsed_ns(
                            link_begin,
                            std::chrono::
                                steady_clock::now());

                    if (!link_error) {
                        reused = true;
                        ++detail.reused_sections;
                        detail.reused_bytes +=
                            value.byte_count;
                    }
                    else if (classify_hard_link_failure(
                                 link_error) ==
                             hard_link_failure_action::rewrite) {
                        ++detail.hard_link_fallback_sections;
                    }
                    else {
                        cleanup();
                        return {status_code::io_failed};
                    }
                }
            }
        }

        if (reused)
            continue;

        pending_segments[
            pending_count] =
            section_segment;

        pending_sections[
            pending_count] = index;
        ++pending_count;

        ++detail.written_sections;
        detail.written_bytes +=
            value.byte_count;
    }

    constexpr std::size_t maximum_workers = 8;

    const auto task_count =
        pending_count + 1;

    std::array<
        durable_write_telemetry,
        source_manager_image_directory_count + 1>
        task_io{};

    std::array<
        status,
        source_manager_image_directory_count + 1>
        task_status{};

    for (std::size_t index = 0;
         index < task_count;
         ++index) {
        task_status[index] = {
            status_code::not_available};
    }

    const auto worker_count =
        (std::min)(
            task_count,
            (std::min)(
                maximum_workers,
                io_executor.worker_count()));

    detail.io_worker_count =
        static_cast<std::uint32_t>(
            worker_count);

    std::atomic<std::size_t>
        next_task{0};

    const auto io_begin =
        std::chrono::steady_clock::now();

    const auto worker = [&]() noexcept {
        for (;;) {
            const auto task =
                next_task.fetch_add(
                    1,
                    std::memory_order_relaxed);

            if (task >= task_count)
                return;

            if (task == 0) {
                task_status[task] =
                    durable_write_file(
                        section_directory /
                            source_manager_prefix_name,
                        prefix,
                        {},
                        &task_io[task]);
                continue;
            }

            const auto pending =
                task - 1;

            task_status[task] =
                durable_write_file(
                    source_manager_section_path(
                        section_directory,
                        pending_sections[
                            pending]),
                    pending_segments[
                        pending],
                    {},
                    &task_io[task]);
        }
    };

        io_executor.run_workers(
        worker_count,
        worker);

detail.io_wall_ns =
        elapsed_ns(
            io_begin,
            std::chrono::
                steady_clock::now());

    for (std::size_t index = 0;
         index < task_count;
         ++index) {

        io.write_ns +=
            task_io[index].write_ns;
        io.flush_ns +=
            task_io[index].flush_ns;

        if (!task_status[index].ok()) {
            cleanup();
            return task_status[index];
        }
    }

    detail.written_bytes +=
        prefix.size();

    const auto directory_flush_begin =
        std::chrono::steady_clock::now();

    const auto result =
        flush_directory(
            section_directory);

    detail.directory_flush_ns =
        elapsed_ns(
            directory_flush_begin,
            std::chrono::
                steady_clock::now());

    if (!result.ok()) {
        cleanup();
        return result;
    }

    detail.sectioned = true;
    return {};
}

struct build_cache_storage_section final {
    std::uint32_t record_size = 0;
    std::uint64_t count = 0;
    std::uint64_t offset = 0;
    std::uint64_t byte_count = 0;
    std::uint64_t crc64 = 0;
};

struct sectioned_build_cache_write_telemetry final {
    std::uint64_t link_ns = 0;
    std::uint64_t compare_ns = 0;
    std::uint64_t compare_bytes = 0;
    std::uint64_t io_wall_ns = 0;
    std::uint64_t directory_flush_ns = 0;
    std::uint64_t written_bytes = 0;
    std::uint64_t reused_bytes = 0;
    std::uint32_t written_sections = 0;
    std::uint32_t reused_sections = 0;
    std::uint32_t compare_sections = 0;
    std::uint32_t hard_link_fallback_sections = 0;
    std::uint32_t io_worker_count = 0;
    bool sectioned = false;
};

[[nodiscard]] std::filesystem::path
build_cache_section_path(
    const std::filesystem::path& directory,
    std::size_t index) {

    return directory /
        ("section-" +
         std::to_string(index + 1) +
         ".bin");
}

[[nodiscard]] bool parse_build_cache_prefix(
    std::span<const std::byte> prefix,
    std::uint64_t expected_size,
    std::array<
        build_cache_storage_section,
        build_cache_image_directory_count>&
        output) noexcept {

    output = {};

    if (prefix.size() !=
        build_cache_image_prefix_size ||
        read_u32(prefix, 8) !=
            build_cache_image_format_version ||
        read_u32(prefix, 12) != endian_marker ||
        read_u32(prefix, 16) !=
            build_cache_image_header_size ||
        read_u32(prefix, 20) !=
            build_cache_image_directory_count ||
        read_u32(prefix, 24) !=
            build_cache_image_directory_entry_size ||
        read_u64(prefix, 32) !=
            build_cache_image_header_size ||
        read_u64(prefix, 40) !=
            expected_size) {
        return false;
    }

    std::uint64_t previous_end =
        build_cache_image_prefix_size;

    for (std::size_t index = 0;
         index <
            build_cache_image_directory_count;
         ++index) {

        const auto entry_offset =
            build_cache_image_header_size +
            index *
                build_cache_image_directory_entry_size;

        if (entry_offset +
                build_cache_image_directory_entry_size >
            prefix.size()) {
            return false;
        }

        const auto raw_kind =
            read_u32(prefix, entry_offset);
        const auto record_size =
            read_u32(prefix, entry_offset + 4);
        const auto offset =
            read_u64(prefix, entry_offset + 8);
        const auto count =
            read_u64(prefix, entry_offset + 16);
        const auto crc =
            read_u64(prefix, entry_offset + 24);

        if (raw_kind != index + 1 ||
            record_size == 0) {
            return false;
        }

        const auto aligned =
            (previous_end + 63u) &
            ~std::uint64_t{63u};

        if (offset != aligned)
            return false;

        if (count != 0 &&
            record_size >
                (std::numeric_limits<
                    std::uint64_t>::max)() /
                    count) {
            return false;
        }

        const auto byte_count =
            count * record_size;

        if (offset >
            (std::numeric_limits<
                std::uint64_t>::max)() -
                byte_count) {
            return false;
        }

        const auto end =
            offset + byte_count;

        if (end > expected_size)
            return false;

        output[index] = {
            record_size,
            count,
            offset,
            byte_count,
            crc,
        };

        previous_end = end;
    }

    return previous_end == expected_size;
}

[[nodiscard]] status durable_write_sectioned_build_cache(
    const std::filesystem::path& transaction_directory,
    const std::filesystem::path& previous_directory,
    std::span<const std::byte> image,
    transaction_io_executor& io_executor,
    durable_write_telemetry& io,
    sectioned_build_cache_write_telemetry&
        detail) noexcept {

    io = {};
    detail = {};

    const auto total_begin =
        std::chrono::steady_clock::now();

    if (image.size() <
        build_cache_image_prefix_size) {
        return {status_code::not_available};
    }

    std::array<
        build_cache_storage_section,
        build_cache_image_directory_count>
        current{};

    const auto logical_size =
        static_cast<std::uint64_t>(
            image.size());

    const auto prefix =
        image.first(
            build_cache_image_prefix_size);

    if (!parse_build_cache_prefix(
            prefix,
            logical_size,
            current)) {
        return {status_code::not_available};
    }

    const auto section_directory =
        transaction_directory /
        build_cache_directory_name;

    std::error_code error;
    if (!std::filesystem::create_directory(
            section_directory,
            error) ||
        error) {
        return {status_code::persistence_failed};
    }

    const auto cleanup = [&]() noexcept {
        std::error_code cleanup_error;
        std::filesystem::remove_all(
            section_directory,
            cleanup_error);
    };

    std::array<
        build_cache_storage_section,
        build_cache_image_directory_count>
        previous{};

    bool previous_available = false;

    if (!previous_directory.empty()) {
        std::vector<std::byte> previous_prefix;

        const auto previous_result =
            read_small_file(
                previous_directory /
                    build_cache_prefix_name,
                build_cache_image_prefix_size,
                previous_prefix);

        if (!previous_result.ok()) {
            cleanup();
            return previous_result.code ==
                    status_code::not_found
                ? status{status_code::artifact_corrupt}
                : previous_result;
        }

        if (previous_prefix.size() !=
            build_cache_image_prefix_size) {
            cleanup();
            return {status_code::artifact_corrupt};
        }

        const auto previous_size =
            read_u64(
                std::span<const std::byte>{
                    previous_prefix},
                40);

        if (!parse_build_cache_prefix(
                previous_prefix,
                previous_size,
                previous)) {
            cleanup();
            return {status_code::artifact_corrupt};
        }

        previous_available = true;
    }

    std::array<
        std::size_t,
        build_cache_image_directory_count>
        pending_sections{};

    std::size_t pending_count = 0;

    for (std::size_t index = 0;
         index <
            build_cache_image_directory_count;
         ++index) {

        const auto& value =
            current[index];

        if (value.offset >
                logical_size ||
            value.byte_count >
                logical_size -
                    value.offset ||
            value.offset >
                (std::numeric_limits<
                    std::size_t>::max)() ||
            value.byte_count >
                (std::numeric_limits<
                    std::size_t>::max)()) {

            cleanup();
            return {status_code::not_available};
        }

        const auto bytes =
            image.subspan(
                static_cast<std::size_t>(
                    value.offset),
                static_cast<std::size_t>(
                    value.byte_count));

        const auto target =
            build_cache_section_path(
                section_directory,
                index);

        bool reused = false;

        if (previous_available) {
            const auto& old =
                previous[index];

            const bool metadata_equal =
                old.record_size ==
                    value.record_size &&
                old.count == value.count &&
                old.byte_count ==
                    value.byte_count &&
                old.crc64 == value.crc64;

            if (metadata_equal) {
                const auto source =
                    build_cache_section_path(
                        previous_directory,
                        index);

                bool exact_equal = false;

                const auto compare_begin =
                    std::chrono::
                        steady_clock::now();

                const auto compare_result =
                    exact_file_equals_segment(
                        source,
                        project_generation_segment{
                            bytes},
                        exact_equal);

                detail.compare_ns +=
                    elapsed_ns(
                        compare_begin,
                        std::chrono::
                            steady_clock::now());
                detail.compare_bytes +=
                    value.byte_count;
                ++detail.compare_sections;

                if (!compare_result.ok()) {
                    cleanup();
                    return compare_result.code ==
                            status_code::not_found
                        ? status{status_code::artifact_corrupt}
                        : compare_result;
                }

                if (exact_equal) {

                    const auto link_begin =
                        std::chrono::
                            steady_clock::now();

                    std::error_code link_error;
                    std::filesystem::
                        create_hard_link(
                            source,
                            target,
                            link_error);

                    detail.link_ns +=
                        elapsed_ns(
                            link_begin,
                            std::chrono::
                                steady_clock::now());

                    if (!link_error) {
                        reused = true;
                        ++detail.reused_sections;
                        detail.reused_bytes +=
                            value.byte_count;
                    }
                    else if (classify_hard_link_failure(
                                 link_error) ==
                             hard_link_failure_action::rewrite) {
                        ++detail.hard_link_fallback_sections;
                    }
                    else {
                        cleanup();
                        return {status_code::io_failed};
                    }
                }
            }
        }

        if (!reused) {
            pending_sections[
                pending_count++] = index;

            ++detail.written_sections;
            detail.written_bytes +=
                value.byte_count;
        }
    }

    // prefix.bin and changed section files are independent immutable
    // transaction artifacts. Their writes and durable flushes may overlap;
    // CURRENT remains unpublished until the complete group succeeds.
    constexpr std::size_t maximum_workers = 8;

    const auto task_count =
        pending_count + 1; // prefix.bin

    std::array<
        durable_write_telemetry,
        build_cache_image_directory_count + 1>
        task_io{};

    std::array<
        status,
        build_cache_image_directory_count + 1>
        task_status{};

    for (std::size_t index = 0;
         index < task_count;
         ++index) {
        task_status[index] = {
            status_code::not_available};
    }

    const auto worker_count =
        (std::min)(
            task_count,
            (std::min)(
                maximum_workers,
                io_executor.worker_count()));

    detail.io_worker_count =
        static_cast<std::uint32_t>(
            worker_count);

    std::atomic<std::size_t>
        next_task{0};

    const auto io_begin =
        std::chrono::steady_clock::now();

    const auto worker = [&]() noexcept {
        for (;;) {
            const auto task =
                next_task.fetch_add(
                    1,
                    std::memory_order_relaxed);

            if (task >= task_count)
                return;

            if (task == 0) {
                task_status[task] =
                    durable_write_file(
                        section_directory /
                            build_cache_prefix_name,
                        prefix,
                        {},
                        &task_io[task]);
                continue;
            }

            const auto section_index =
                pending_sections[
                    task - 1];

            const auto& value =
                current[section_index];

            const auto bytes =
                image.subspan(
                    static_cast<std::size_t>(
                        value.offset),
                    static_cast<std::size_t>(
                        value.byte_count));

            task_status[task] =
                durable_write_file(
                    build_cache_section_path(
                        section_directory,
                        section_index),
                    bytes,
                    {},
                    &task_io[task]);
        }
    };

        io_executor.run_workers(
        worker_count,
        worker);

for (std::size_t index = 0;
         index < task_count;
         ++index) {

        io.write_ns +=
            task_io[index].write_ns;
        io.flush_ns +=
            task_io[index].flush_ns;

        if (!task_status[index].ok()) {
            cleanup();
            return task_status[index];
        }
    }

    detail.written_bytes +=
        prefix.size();

    const auto directory_flush_begin =
        std::chrono::steady_clock::now();

    const auto result =
        flush_directory(
            section_directory);

    detail.directory_flush_ns =
        elapsed_ns(
            directory_flush_begin,
            std::chrono::steady_clock::now());

    if (!result.ok()) {
        cleanup();
        return result;
    }

    detail.sectioned = true;
    detail.io_wall_ns =
        elapsed_ns(
            total_begin,
            std::chrono::
                steady_clock::now());

    return {};
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

baseline_section_provenance
baseline_snapshot::prove_section_borrow(
    baseline_artifact_kind artifact,
    std::size_t section,
    std::span<const std::byte> bytes) const noexcept {

    std::span<const std::byte> mapped;

    switch (artifact) {
    case baseline_artifact_kind::source_manager:
        if (!sectioned_source_manager ||
            section >= source_manager_sections.size()) {
            return {};
        }

        mapped =
            source_manager_sections[section].bytes();
        break;

    case baseline_artifact_kind::build_cache:
        if (!sectioned_build_cache ||
            section >= build_cache_sections.size()) {
            return {};
        }

        mapped =
            build_cache_sections[section].bytes();
        break;

    case baseline_artifact_kind::compiled:
    case baseline_artifact_kind::change_state:
        return {};
    }

    if (mapped.data() != bytes.data() ||
        mapped.size() != bytes.size()) {
        return {};
    }

    return baseline_section_provenance{
        this,
        artifact,
        static_cast<std::uint32_t>(section),
        mapped.data(),
        static_cast<std::uint64_t>(mapped.size())};
}

bool baseline_snapshot::validate_section_borrow(
    const baseline_section_provenance& proof) const noexcept {

    if (!proof.valid() ||
        proof.owner() != this) {
        return false;
    }

    std::span<const std::byte> mapped;

    switch (proof.artifact()) {
    case baseline_artifact_kind::source_manager:
        if (!sectioned_source_manager ||
            proof.section() >=
                source_manager_sections.size()) {
            return false;
        }

        mapped =
            source_manager_sections[
                proof.section()].bytes();
        break;

    case baseline_artifact_kind::build_cache:
        if (!sectioned_build_cache ||
            proof.section() >=
                build_cache_sections.size()) {
            return false;
        }

        mapped =
            build_cache_sections[
                proof.section()].bytes();
        break;

    case baseline_artifact_kind::compiled:
    case baseline_artifact_kind::change_state:
        return false;
    }

    const auto proven =
        proof.bytes();

    return mapped.data() == proven.data() &&
        mapped.size() == proven.size();
}

status baseline_snapshot::bind_source_manager(
    source_manager_image_view& output) const noexcept {

    output.reset();

    if (sectioned_source_manager) {
        if (!source_manager_prefix.open())
            return {status_code::not_found};

        std::array<
            std::span<const std::byte>,
            source_manager_image_directory_count>
            section_images{};

        for (std::size_t index = 0;
             index <
                source_manager_image_directory_count;
             ++index) {

            section_images[index] =
                source_manager_sections[
                    index].bytes();
        }

        return output.bind_sectioned(
            source_manager_prefix.bytes(),
            section_images);
    }

    const auto image =
        artifact(
            baseline_artifact_kind::
                source_manager);

    return image.empty()
        ? status{status_code::not_found}
        : output.bind(image);
}


status baseline_snapshot::bind_build_cache(
    build_cache_image_view& output) const noexcept {

    output.reset();

    if (sectioned_build_cache) {
        if (!build_cache_prefix.open())
            return {status_code::not_found};

        std::array<
            std::span<const std::byte>,
            build_cache_image_directory_count>
            section_images{};

        for (std::size_t index = 0;
             index <
                build_cache_image_directory_count;
             ++index) {

            if (!build_cache_sections[index].open())
                return {status_code::not_found};

            section_images[index] =
                build_cache_sections[index].bytes();
        }

        return output.bind_sectioned(
            build_cache_prefix.bytes(),
            section_images);
    }

    const auto image =
        artifact(
            baseline_artifact_kind::
                build_cache);

    return image.empty()
        ? status{status_code::not_found}
        : output.bind(image);
}


std::span<const std::byte> baseline_snapshot::artifact(
    baseline_artifact_kind kind) const noexcept {

    switch (kind) {
    case baseline_artifact_kind::compiled:
        return compiled.bytes();

    case baseline_artifact_kind::source_manager: {
        if (sectioned_source_manager)
            return {};

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
        if (sectioned_build_cache)
            return {};

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


project_generation_segments
baseline_snapshot::segments() const noexcept {

    static constexpr std::array<std::byte, 64>
        zero_padding{};

    project_generation_segment source_segment{
        artifact(
            baseline_artifact_kind::
                source_manager)};

    if (sectioned_source_manager) {
        source_segment = {};

        const auto prefix =
            source_manager_prefix.bytes();

        std::array<
            source_manager_storage_section,
            source_manager_image_directory_count>
            metadata{};

        if (!parse_source_manager_prefix(
                prefix,
                source_manager_size_value,
                metadata) ||
            !source_segment.append(prefix)) {
            return {};
        }

        std::size_t cursor =
            prefix.size();

        for (std::size_t index = 0;
             index <
                source_manager_image_directory_count;
             ++index) {

            const auto offset =
                static_cast<std::size_t>(
                    metadata[index].offset);

            if (offset < cursor)
                return {};

            const auto padding =
                offset - cursor;

            if (padding != 0 &&
                (padding >
                    zero_padding.size() ||
                 !source_segment.append(
                    std::span<const std::byte>{
                        zero_padding.data(),
                        padding}))) {
                return {};
            }

            const auto section =
                source_manager_sections[
                    index].bytes();

            if (section.size() !=
                    metadata[index].byte_count ||
                !source_segment.append(
                    section)) {
                return {};
            }

            cursor =
                offset +
                section.size();
        }

        if (source_segment.size() !=
            source_manager_size_value) {
            return {};
        }
    }

    project_generation_segment build_segment{
        artifact(
            baseline_artifact_kind::
                build_cache)};

    if (sectioned_build_cache) {
        build_segment = {};

        const auto prefix =
            build_cache_prefix.bytes();

        std::array<
            build_cache_storage_section,
            build_cache_image_directory_count>
            metadata{};

        if (!parse_build_cache_prefix(
                prefix,
                build_cache_size_value,
                metadata) ||
            !build_segment.append(prefix)) {
            return {};
        }

        std::size_t cursor =
            prefix.size();

        for (std::size_t index = 0;
             index <
                build_cache_image_directory_count;
             ++index) {

            const auto offset =
                static_cast<std::size_t>(
                    metadata[index].offset);

            if (offset < cursor)
                return {};

            const auto padding =
                offset - cursor;

            if (padding != 0 &&
                (padding >
                    zero_padding.size() ||
                 !build_segment.append(
                    std::span<const std::byte>{
                        zero_padding.data(),
                        padding}))) {
                return {};
            }

            const auto section =
                build_cache_sections[
                    index].bytes();

            if (section.size() !=
                    metadata[index].byte_count ||
                !build_segment.append(
                    section)) {
                return {};
            }

            cursor =
                offset +
                section.size();
        }

        if (build_segment.size() !=
            build_cache_size_value) {
            return {};
        }
    }

    return {
        project_generation_segment{
            artifact(
                baseline_artifact_kind::
                    compiled)},
        source_segment,
        project_generation_segment{
            artifact(
                baseline_artifact_kind::
                    change_state)},
        build_segment,
    };
}

bool baseline_snapshot::mapped(
    baseline_artifact_kind kind) const noexcept {

    switch (kind) {
    case baseline_artifact_kind::compiled:
        return compiled.open();
    case baseline_artifact_kind::source_manager:
        return
            sectioned_source_manager ||
            source_manager.open();
    case baseline_artifact_kind::change_state:
        return !embedded_change_state.empty() ||
            change_state.open();
    case baseline_artifact_kind::build_cache:
        return
            sectioned_build_cache ||
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
        const auto root = root_path();

        std::string transaction;
        std::array<std::byte, manifest_size>
            embedded_manifest{};
        bool embedded_manifest_available = false;

        // Identity-only callers must not read the embedded BUILD decision gate.
        // CURRENT v3 is validated from its fixed prefix plus file size; the
        // change-state tail remains untouched.
        auto result = read_current_ready_selector(
            root,
            transaction,
            embedded_manifest,
            embedded_manifest_available);
        if (!result.ok())
            return result;

        parsed_manifest manifest;

        if (embedded_manifest_available) {
            result = parse_manifest(
                embedded_manifest,
                manifest);
        }
        else {
            const auto directory =
                root / transaction;

            std::vector<std::byte> manifest_bytes;
            result = read_small_file(
                directory / manifest_name,
                manifest_size,
                manifest_bytes);
            if (!result.ok()) {
                return result.code ==
                        status_code::not_found
                    ? status{
                        status_code::artifact_corrupt}
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

        output.fingerprint = manifest.fingerprint;
        output.configuration = manifest.configuration;
        output.transaction = std::move(transaction);
        output.source_manager_size =
            manifest.source_manager_size;
        output.build_cache_size =
            manifest.build_cache_size;
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

        snapshot.source_manager_size_value =
            manifest.source_manager_size;
        snapshot.build_cache_size_value =
            manifest.build_cache_size;

        return map_source_manager_artifact(
            directory,
            manifest.source_manager_size,
            snapshot,
            telemetry);
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
        const auto directory =
            root_path() /
            std::string{transaction};

        if (snapshot.source_manager_size_value == 0)
            snapshot.source_manager_size_value =
                expected_source_manager_size;

        if (snapshot.source_manager_size_value !=
            expected_source_manager_size) {
            return {status_code::artifact_corrupt};
        }

        return map_source_manager_artifact(
            directory,
            expected_source_manager_size,
            snapshot,
            telemetry);
    }
    catch (const std::bad_alloc&) { return {status_code::not_available}; }
    catch (const std::length_error&) { return {status_code::not_available}; }
    catch (const std::filesystem::filesystem_error&) { return {status_code::io_failed}; }
}


status baseline_store::map_source_manager_artifact(
    const std::filesystem::path& directory,
    std::uint64_t expected_size,
    baseline_snapshot& snapshot,
    baseline_open_telemetry* telemetry) const noexcept {

    const auto map_begin =
        std::chrono::steady_clock::now();

    const auto legacy_path =
        directory /
        source_manager_name;

    std::error_code exists_error;
    const bool legacy_exists =
        std::filesystem::exists(
            legacy_path,
            exists_error);

    if (exists_error)
        return {status_code::io_failed};

    if (legacy_exists) {
        auto result =
            snapshot.source_manager.map(
                legacy_path);

        if (telemetry != nullptr) {
            telemetry->source_manager_map_ns +=
                elapsed_ns(
                    map_begin,
                    std::chrono::
                        steady_clock::now());
        }

        if (!result.ok())
            return result;

        const auto physical_size =
            static_cast<std::uint64_t>(
                snapshot.source_manager.bytes().size());

        const bool separate =
            physical_size ==
                expected_size;

        const bool can_pack =
            expected_size <=
                (std::numeric_limits<
                    std::uint64_t>::max)() -
                    snapshot.build_cache_size_value;

        const bool packed =
            can_pack &&
            physical_size ==
                expected_size +
                    snapshot.build_cache_size_value;

        if (!separate && !packed) {
            snapshot.source_manager = {};
            return {
                status_code::artifact_corrupt};
        }

        snapshot.sectioned_source_manager =
            false;
        snapshot.packed_build_state =
            packed;
        snapshot.packed_build_cache_enabled =
            false;
        return {};
    }

    const auto section_directory =
        directory /
        source_manager_directory_name;

    auto result =
        snapshot.source_manager_prefix.map(
            section_directory /
                source_manager_prefix_name);

    if (!result.ok()) {
        if (telemetry != nullptr) {
            telemetry->source_manager_map_ns +=
                elapsed_ns(
                    map_begin,
                    std::chrono::
                        steady_clock::now());
        }

        return {
            status_code::artifact_corrupt};
    }

    const auto prefix =
        snapshot.source_manager_prefix.bytes();

    std::array<
        source_manager_storage_section,
        source_manager_image_directory_count>
        metadata{};

    if (!parse_source_manager_prefix(
            prefix,
            expected_size,
            metadata)) {

        snapshot.source_manager_prefix = {};

        if (telemetry != nullptr) {
            telemetry->source_manager_map_ns +=
                elapsed_ns(
                    map_begin,
                    std::chrono::
                        steady_clock::now());
        }

        return {
            status_code::artifact_corrupt};
    }

    for (std::size_t index = 0;
         index <
            source_manager_image_directory_count;
         ++index) {

        if (metadata[index].byte_count == 0)
            continue;

        result =
            snapshot.source_manager_sections[
                index].map(
                source_manager_section_path(
                    section_directory,
                    index));

        if (!result.ok() ||
            snapshot.source_manager_sections[
                index].bytes().size() !=
                metadata[index].byte_count) {

            snapshot.source_manager_prefix = {};
            for (auto& mapping :
                 snapshot.source_manager_sections) {
                mapping = {};
            }

            if (telemetry != nullptr) {
                telemetry->source_manager_map_ns +=
                    elapsed_ns(
                        map_begin,
                        std::chrono::
                            steady_clock::now());
            }

            return {
                status_code::artifact_corrupt};
        }
    }

    snapshot.sectioned_source_manager = true;
    snapshot.packed_build_state = false;
    snapshot.packed_build_cache_enabled = false;

    source_manager_image_view view;
    result =
        snapshot.bind_source_manager(view);

    if (telemetry != nullptr) {
        telemetry->source_manager_map_ns +=
            elapsed_ns(
                map_begin,
                std::chrono::
                    steady_clock::now());
    }

    if (!result.ok()) {
        snapshot.sectioned_source_manager =
            false;
        snapshot.source_manager_prefix = {};
        for (auto& mapping :
             snapshot.source_manager_sections) {
            mapping = {};
        }
        return result;
    }

    return {};
}


status baseline_store::map_build_cache_artifact(
    const std::filesystem::path& directory,
    std::uint64_t expected_size,
    baseline_snapshot& snapshot,
    baseline_open_telemetry* telemetry) const noexcept {

    const auto map_begin =
        std::chrono::steady_clock::now();

    const auto legacy_path =
        directory / build_cache_name;

    std::error_code exists_error;
    const bool legacy_exists =
        std::filesystem::exists(
            legacy_path,
            exists_error);

    if (exists_error)
        return {status_code::io_failed};

    if (legacy_exists) {
        auto result =
            snapshot.build_cache.map(
                legacy_path);

        if (telemetry != nullptr) {
            telemetry->build_cache_map_ns +=
                elapsed_ns(
                    map_begin,
                    std::chrono::
                        steady_clock::now());
        }

        if (!result.ok())
            return result;

        const auto validation_begin =
            std::chrono::steady_clock::now();

        const bool mismatch =
            snapshot.build_cache.bytes().size() !=
                expected_size;

        if (telemetry != nullptr) {
            telemetry->size_validation_ns +=
                elapsed_ns(
                    validation_begin,
                    std::chrono::
                        steady_clock::now());
        }

        if (mismatch) {
            snapshot.build_cache = {};
            return {
                status_code::artifact_corrupt};
        }

        snapshot.sectioned_build_cache =
            false;
        return {};
    }

    const auto section_directory =
        directory /
        build_cache_directory_name;

    auto result =
        snapshot.build_cache_prefix.map(
            section_directory /
                build_cache_prefix_name);

    if (!result.ok()) {
        if (telemetry != nullptr) {
            telemetry->build_cache_map_ns +=
                elapsed_ns(
                    map_begin,
                    std::chrono::
                        steady_clock::now());
        }

        return {
            status_code::artifact_corrupt};
    }

    for (std::size_t index = 0;
         index <
            build_cache_image_directory_count;
         ++index) {

        result =
            snapshot.build_cache_sections[index].
                map(
                    build_cache_section_path(
                        section_directory,
                        index));

        if (!result.ok()) {
            snapshot.build_cache_prefix = {};

            for (auto& mapping :
                 snapshot.build_cache_sections) {
                mapping = {};
            }

            if (telemetry != nullptr) {
                telemetry->build_cache_map_ns +=
                    elapsed_ns(
                        map_begin,
                        std::chrono::
                            steady_clock::now());
            }

            return {
                status_code::artifact_corrupt};
        }
    }

    snapshot.sectioned_build_cache =
        true;

    build_cache_image_view view;
    result =
        snapshot.bind_build_cache(view);

    const auto validation_begin =
        std::chrono::steady_clock::now();

    const auto prefix =
        snapshot.build_cache_prefix.bytes();

    const bool size_mismatch =
        prefix.size() <
            build_cache_image_header_size ||
        read_u64(prefix, 40) !=
            expected_size;

    if (telemetry != nullptr) {
        telemetry->build_cache_map_ns +=
            elapsed_ns(
                map_begin,
                validation_begin);

        telemetry->size_validation_ns +=
            elapsed_ns(
                validation_begin,
                std::chrono::
                    steady_clock::now());
    }

    if (!result.ok() || size_mismatch) {
        snapshot.sectioned_build_cache =
            false;
        snapshot.build_cache_prefix = {};

        for (auto& mapping :
             snapshot.build_cache_sections) {
            mapping = {};
        }

        return result.ok()
            ? status{
                status_code::artifact_corrupt}
            : result;
    }

    return {};
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

        return map_build_cache_artifact(
            directory,
            manifest.build_cache_size,
            snapshot,
            telemetry);
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

        auto result =
            map_build_cache_artifact(
                directory,
                expected_build_cache_size,
                snapshot,
                telemetry);

        if (!result.ok())
            return result;

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
            result =
                map_source_manager_artifact(
                    directory,
                    manifest.source_manager_size,
                    candidate,
                    telemetry);

            if (!result.ok())
                return result;
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
                result =
                    map_build_cache_artifact(
                        directory,
                        manifest.build_cache_size,
                        candidate,
                        telemetry);

                if (!result.ok())
                    return result;
            }
        }

        const auto size_validation_begin =
            std::chrono::steady_clock::now();

        const bool size_mismatch =
            candidate.compiled.bytes().size() !=
                manifest.compiled_size ||
            (include_source_manager &&
             !candidate.mapped(
                 baseline_artifact_kind::source_manager)) ||
            (include_build_cache &&
             !candidate.mapped(
                 baseline_artifact_kind::build_cache));

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
    project_generation_segments generation,
    baseline_commit_result& output) const noexcept {

    return commit(
        fingerprint,
        configuration,
        generation,
        baseline_commit_provenance{},
        output);
}

status baseline_store::commit(
    const baseline_fingerprint& fingerprint,
    const baseline_configuration_state& configuration,
    project_generation_segments generation,
    const baseline_commit_provenance& provenance,
    baseline_commit_result& output,
    std::size_t io_worker_budget) const noexcept {
    const auto& compiled =
        generation.compiled_segment();
    const auto& source_manager =
        generation.sources_segment();
    const auto& change_state =
        generation.change_segment();
    const auto& build_cache =
        generation.build_segment();

    if (!change_state.empty() &&
        !change_state.is_contiguous()) {
        return {status_code::invalid_argument};
    }


    output = {};
    const auto commit_begin =
        std::chrono::steady_clock::now();

    durable_write_telemetry io;
    transaction_io_executor transaction_executor{
        io_worker_budget};

    output.telemetry.transaction_io_budget =
        static_cast<std::uint32_t>(
            transaction_executor.worker_count());

    const auto transaction_write =
        [&](const auto& path,
            const auto& first,
            project_generation_segment,
            durable_write_telemetry* telemetry) noexcept {
            return durable_write_file(
                path,
                first,
                {},
                telemetry);
        };

    const auto record_transaction_io =
        [&](const durable_write_telemetry& value) noexcept {
            output.telemetry.transaction_write_ns +=
                value.write_ns;
            output.telemetry.transaction_flush_ns +=
                value.flush_ns;
        };

    const auto record_directory_flush =
        [&](std::chrono::steady_clock::time_point begin) noexcept {
            output.telemetry.directory_flush_ns +=
                elapsed_ns(
                    begin,
                    std::chrono::steady_clock::now());
        };

    try {
        const auto root = root_path();
        std::error_code error;
        std::filesystem::create_directories(root, error);
        if (error)
            return {status_code::persistence_failed};

        std::filesystem::path
            previous_source_manager_directory;
        std::filesystem::path
            previous_build_cache_directory;

        std::string previous_transaction;
        const auto previous_result =
            read_current_transaction(
                root,
                previous_transaction);

        if (previous_result.ok() &&
            valid_transaction_name(
                previous_transaction)) {

            const auto source_candidate =
                root /
                previous_transaction /
                source_manager_directory_name;
            const auto build_candidate =
                root /
                previous_transaction /
                build_cache_directory_name;

            std::error_code previous_error;
            const auto source_sectioned =
                std::filesystem::exists(
                    source_candidate /
                        source_manager_prefix_name,
                    previous_error);

            if (previous_error)
                return {status_code::io_failed};

            if (source_sectioned) {
                previous_source_manager_directory =
                    source_candidate;
            }

            previous_error.clear();
            const auto build_sectioned =
                std::filesystem::exists(
                    build_candidate /
                        build_cache_prefix_name,
                    previous_error);

            if (previous_error)
                return {status_code::io_failed};

            if (build_sectioned) {
                previous_build_cache_directory =
                    build_candidate;
            }
        }

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

        // Production sparse SAVE uses separate physical Source Manager and
        // Build Cache files. The logical artifact format is unchanged and the
        // loader remains backward-compatible with previously packed transactions.
        // Every artifact is durable before CURRENT publishes the transaction.
        const bool parallel_build_state =
            !change_state.empty() &&
            !source_manager.empty() &&
            !build_cache.empty();

        std::array<std::byte, manifest_size> manifest{};
        auto result = create_manifest(
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

        const auto transaction_io_begin =
            std::chrono::steady_clock::now();

        if (parallel_build_state) {
            status compiled_result;
            status source_manager_result;
            status build_cache_result;
            status change_state_result;
            status manifest_result;

            durable_write_telemetry compiled_io;
            durable_write_telemetry source_manager_io;
            sectioned_source_manager_write_telemetry
                source_manager_detail;
            baseline_sectioned_fallback_reason
                source_manager_fallback_reason =
                    baseline_sectioned_fallback_reason::none;
            std::uint64_t
                source_manager_failed_attempt_ns = 0;

            durable_write_telemetry build_cache_io;
            sectioned_build_cache_write_telemetry
                build_cache_detail;
            baseline_sectioned_fallback_reason
                build_cache_fallback_reason =
                    baseline_sectioned_fallback_reason::none;
            std::uint64_t
                build_cache_failed_attempt_ns = 0;
            durable_write_telemetry change_state_io;
            durable_write_telemetry manifest_io;

            const auto compiled_task =
                [&]() noexcept {
                    compiled_result =
                        transaction_write(
                            directory / compiled_name,
                            compiled,
                            {},
                            &compiled_io);
                };

            const auto source_manager_task =
                [&]() noexcept {
                    if (source_manager.is_contiguous()) {
                        source_manager_fallback_reason =
                            baseline_sectioned_fallback_reason::
                                structural_ineligible;

                        source_manager_result =
                            transaction_write(
                                directory /
                                    source_manager_name,
                                source_manager,
                                {},
                                &source_manager_io);
                        return;
                    }

                    const auto attempt_begin =
                        std::chrono::steady_clock::now();

                    source_manager_result =
                        durable_write_sectioned_source_manager(
                            directory,
                            previous_source_manager_directory,
                            source_manager,
                            provenance.source_manager,
                            transaction_executor,
                            source_manager_io,
                            source_manager_detail);

                    if (source_manager_result.ok())
                        return;

                    const auto attempt_ns =
                        elapsed_ns(
                            attempt_begin,
                            std::chrono::
                                steady_clock::now());

                    if (source_manager_result.code !=
                        status_code::not_available) {
                        source_manager_failed_attempt_ns =
                            attempt_ns;
                        return;
                    }

                    source_manager_fallback_reason =
                        baseline_sectioned_fallback_reason::
                            structural_ineligible;
                    source_manager_failed_attempt_ns =
                        attempt_ns;

                    std::error_code cleanup_error;
                    std::filesystem::remove_all(
                        directory /
                            source_manager_directory_name,
                        cleanup_error);

                    durable_write_telemetry fallback_io;
                    source_manager_result =
                        transaction_write(
                            directory /
                                source_manager_name,
                            source_manager,
                            {},
                            &fallback_io);

                    source_manager_io.write_ns +=
                        fallback_io.write_ns;
                    source_manager_io.flush_ns +=
                        fallback_io.flush_ns;
                };

            const auto build_cache_task =
                [&]() noexcept {
                    if (!build_cache.is_contiguous()) {
                        build_cache_fallback_reason =
                            baseline_sectioned_fallback_reason::
                                structural_ineligible;

                        build_cache_result =
                            transaction_write(
                                directory /
                                    build_cache_name,
                                build_cache,
                                {},
                                &build_cache_io);
                        return;
                    }

                    const auto attempt_begin =
                        std::chrono::steady_clock::now();

                    build_cache_result =
                        durable_write_sectioned_build_cache(
                            directory,
                            previous_build_cache_directory,
                            build_cache.contiguous(),
                            transaction_executor,
                            build_cache_io,
                            build_cache_detail);

                    if (build_cache_result.ok())
                        return;

                    const auto attempt_ns =
                        elapsed_ns(
                            attempt_begin,
                            std::chrono::
                                steady_clock::now());

                    if (build_cache_result.code !=
                        status_code::not_available) {
                        build_cache_failed_attempt_ns =
                            attempt_ns;
                        return;
                    }

                    build_cache_fallback_reason =
                        baseline_sectioned_fallback_reason::
                            structural_ineligible;
                    build_cache_failed_attempt_ns =
                        attempt_ns;

                    std::error_code cleanup_error;
                    std::filesystem::remove_all(
                        directory /
                            build_cache_directory_name,
                        cleanup_error);

                    durable_write_telemetry fallback_io;
                    build_cache_result =
                        transaction_write(
                            directory /
                                build_cache_name,
                            build_cache,
                            {},
                            &fallback_io);

                    build_cache_io.write_ns +=
                        fallback_io.write_ns;
                    build_cache_io.flush_ns +=
                        fallback_io.flush_ns;
                };

            const auto change_state_task =
                [&]() noexcept {
                    change_state_result =
                        transaction_write(
                            directory / change_state_name,
                            change_state,
                            {},
                            &change_state_io);
                };

            const auto manifest_task =
                [&]() noexcept {
                    manifest_result =
                        transaction_write(
                            directory / manifest_name,
                            manifest,
                            {},
                            &manifest_io);
                };

            transaction_executor.run(
                compiled_task,
                source_manager_task,
                build_cache_task,
                change_state_task,
                manifest_task);

            output.telemetry.transaction_io_worker_count =
                static_cast<std::uint32_t>(
                    transaction_executor.worker_count());

            output.telemetry.transaction_compiled_write_ns =
                compiled_io.write_ns;
            output.telemetry.transaction_compiled_flush_ns =
                compiled_io.flush_ns;

            output.telemetry.transaction_source_manager_write_ns =
                source_manager_io.write_ns;
            output.telemetry.transaction_source_manager_flush_ns =
                source_manager_io.flush_ns;
            output.telemetry.transaction_source_manager_link_ns =
                source_manager_detail.link_ns;
            output.telemetry.transaction_source_manager_compare_ns =
                source_manager_detail.compare_ns;
            output.telemetry.transaction_source_manager_compare_bytes =
                source_manager_detail.compare_bytes;
            output.telemetry.transaction_source_manager_compare_sections =
                source_manager_detail.compare_sections;
            output.telemetry.transaction_source_manager_provenance_reused_bytes =
                source_manager_detail.provenance_reused_bytes;
            output.telemetry.transaction_source_manager_provenance_reused_sections =
                source_manager_detail.provenance_reused_sections;
            output.telemetry.transaction_source_manager_failed_attempt_ns =
                source_manager_failed_attempt_ns;
            output.telemetry.transaction_source_manager_fallback_reason =
                static_cast<std::uint32_t>(
                    source_manager_fallback_reason);
            output.telemetry.transaction_source_manager_hard_link_fallback_sections =
                source_manager_detail.hard_link_fallback_sections;
            output.telemetry.transaction_source_manager_io_wall_ns =
                source_manager_detail.io_wall_ns;
            output.telemetry.transaction_source_manager_io_worker_count =
                source_manager_detail.io_worker_count;
            output.telemetry.transaction_source_manager_directory_flush_ns =
                source_manager_detail.directory_flush_ns;
            output.telemetry.transaction_source_manager_written_bytes =
                source_manager_detail.written_bytes;
            output.telemetry.transaction_source_manager_reused_bytes =
                source_manager_detail.reused_bytes;
            output.telemetry.transaction_source_manager_written_sections =
                source_manager_detail.written_sections;
            output.telemetry.transaction_source_manager_reused_sections =
                source_manager_detail.reused_sections;
            output.telemetry.transaction_source_manager_sectioned =
                source_manager_detail.sectioned ? 1u : 0u;

            output.telemetry.transaction_build_cache_write_ns =
                build_cache_io.write_ns;
            output.telemetry.transaction_build_cache_flush_ns =
                build_cache_io.flush_ns;
            output.telemetry.transaction_build_cache_link_ns =
                build_cache_detail.link_ns;
            output.telemetry.transaction_build_cache_compare_ns =
                build_cache_detail.compare_ns;
            output.telemetry.transaction_build_cache_compare_bytes =
                build_cache_detail.compare_bytes;
            output.telemetry.transaction_build_cache_compare_sections =
                build_cache_detail.compare_sections;
            output.telemetry.transaction_build_cache_failed_attempt_ns =
                build_cache_failed_attempt_ns;
            output.telemetry.transaction_build_cache_fallback_reason =
                static_cast<std::uint32_t>(
                    build_cache_fallback_reason);
            output.telemetry.transaction_build_cache_hard_link_fallback_sections =
                build_cache_detail.hard_link_fallback_sections;
            output.telemetry.transaction_build_cache_io_wall_ns =
                build_cache_detail.io_wall_ns;
            output.telemetry.transaction_build_cache_io_worker_count =
                build_cache_detail.io_worker_count;
            output.telemetry.transaction_build_cache_directory_flush_ns =
                build_cache_detail.directory_flush_ns;
            output.telemetry.transaction_build_cache_written_bytes =
                build_cache_detail.written_bytes;
            output.telemetry.transaction_build_cache_reused_bytes =
                build_cache_detail.reused_bytes;
            output.telemetry.transaction_build_cache_written_sections =
                build_cache_detail.written_sections;
            output.telemetry.transaction_build_cache_reused_sections =
                build_cache_detail.reused_sections;
            output.telemetry.transaction_build_cache_sectioned =
                build_cache_detail.sectioned ? 1u : 0u;

            output.telemetry.transaction_build_state_write_ns =
                source_manager_io.write_ns +
                build_cache_io.write_ns;
            output.telemetry.transaction_build_state_flush_ns =
                source_manager_io.flush_ns +
                build_cache_io.flush_ns;

            output.telemetry.transaction_change_state_write_ns =
                change_state_io.write_ns;
            output.telemetry.transaction_change_state_flush_ns =
                change_state_io.flush_ns;
            output.telemetry.transaction_manifest_write_ns =
                manifest_io.write_ns;
            output.telemetry.transaction_manifest_flush_ns =
                manifest_io.flush_ns;

            record_transaction_io(compiled_io);
            record_transaction_io(source_manager_io);
            record_transaction_io(build_cache_io);
            record_transaction_io(change_state_io);
            record_transaction_io(manifest_io);

            if (!compiled_result.ok() ||
                !source_manager_result.ok() ||
                !build_cache_result.ok() ||
                !change_state_result.ok() ||
                !manifest_result.ok()) {
                cleanup_failed_transaction();
                return {status_code::persistence_failed};
            }
        }
        else {
            output.telemetry.transaction_io_worker_count = 1;

            result = transaction_write(
                directory / compiled_name,
                compiled,
                {},
                &io);
            output.telemetry.transaction_compiled_write_ns =
                io.write_ns;
            output.telemetry.transaction_compiled_flush_ns =
                io.flush_ns;
            record_transaction_io(io);
            if (!result.ok()) {
                cleanup_failed_transaction();
                return {status_code::persistence_failed};
            }

            result = transaction_write(
                directory / source_manager_name,
                source_manager,
                {},
                &io);
            output.telemetry.transaction_source_manager_write_ns =
                io.write_ns;
            output.telemetry.transaction_source_manager_flush_ns =
                io.flush_ns;
            output.telemetry.transaction_build_state_write_ns +=
                io.write_ns;
            output.telemetry.transaction_build_state_flush_ns +=
                io.flush_ns;
            record_transaction_io(io);
            if (!result.ok()) {
                cleanup_failed_transaction();
                return {status_code::persistence_failed};
            }

            if (!change_state.empty()) {
                result = transaction_write(
                    directory / change_state_name,
                    change_state,
                    {},
                    &io);
                output.telemetry.transaction_change_state_write_ns =
                    io.write_ns;
                output.telemetry.transaction_change_state_flush_ns =
                    io.flush_ns;
                record_transaction_io(io);
                if (!result.ok()) {
                    cleanup_failed_transaction();
                    return {status_code::persistence_failed};
                }
            }

            result = transaction_write(
                directory / build_cache_name,
                build_cache,
                {},
                &io);
            output.telemetry.transaction_build_cache_write_ns =
                io.write_ns;
            output.telemetry.transaction_build_cache_flush_ns =
                io.flush_ns;
            output.telemetry.transaction_build_state_write_ns +=
                io.write_ns;
            output.telemetry.transaction_build_state_flush_ns +=
                io.flush_ns;
            record_transaction_io(io);
            if (!result.ok()) {
                cleanup_failed_transaction();
                return {status_code::persistence_failed};
            }

            result = transaction_write(
                directory / manifest_name,
                manifest,
                {},
                &io);
            output.telemetry.transaction_manifest_write_ns =
                io.write_ns;
            output.telemetry.transaction_manifest_flush_ns =
                io.flush_ns;
            record_transaction_io(io);
            if (!result.ok()) {
                cleanup_failed_transaction();
                return {status_code::persistence_failed};
            }
        }

        output.telemetry.transaction_io_wall_ns =
            elapsed_ns(
                transaction_io_begin,
                std::chrono::steady_clock::now());
        output.telemetry.transaction_io_budget_wait_ns = 0;
        output.telemetry.transaction_io_peak_active =
            static_cast<std::uint32_t>(
                parallel_build_state
                    ? transaction_executor.peak_active()
                    : std::size_t{1});

        auto directory_flush_begin =
            std::chrono::steady_clock::now();
        result = flush_directory(directory);
        record_directory_flush(directory_flush_begin);
        if (!result.ok()) {
            cleanup_failed_transaction();
            return {status_code::persistence_failed};
        }

        directory_flush_begin =
            std::chrono::steady_clock::now();
        result = flush_directory(root);
        record_directory_flush(directory_flush_begin);
        if (!result.ok()) {
            cleanup_failed_transaction();
            return {status_code::persistence_failed};
        }

        std::vector<std::byte> selector_bytes;
        result = create_current_selector(
            transaction,
            manifest,
            change_state.contiguous(),
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
        result = transaction_write(
            selector_temp,
            selector,
            {},
            &io);
        output.telemetry.current_write_ns +=
            io.write_ns;
        output.telemetry.current_flush_ns +=
            io.flush_ns;
        if (!result.ok()) {
            cleanup_failed_transaction();
            return {status_code::persistence_failed};
        }

        const auto replace_begin =
            std::chrono::steady_clock::now();
        result = atomic_replace(
            selector_temp,
            root / current_name);
        output.telemetry.current_replace_ns =
            elapsed_ns(
                replace_begin,
                std::chrono::steady_clock::now());
        if (!result.ok()) {
            std::filesystem::remove(selector_temp, error);
            cleanup_failed_transaction();
            return {status_code::persistence_failed};
        }
        // CURRENT replacement is the commit point. After it succeeds the new
        // transaction is authoritative; directory flush is a durability barrier,
        // not a reason to report the already-committed operation as failed.
        directory_flush_begin =
            std::chrono::steady_clock::now();
        (void)flush_directory(root);
        record_directory_flush(directory_flush_begin);

        output.transaction = transaction;
        output.bytes_written =
            static_cast<std::uint64_t>(compiled.size()) +
            static_cast<std::uint64_t>(source_manager.size()) +
            static_cast<std::uint64_t>(change_state.size()) +
            static_cast<std::uint64_t>(build_cache.size()) +
            manifest_size +
            static_cast<std::uint64_t>(selector_bytes.size());

        output.telemetry.store_commit_ns =
            elapsed_ns(
                commit_begin,
                std::chrono::steady_clock::now());
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
    std::span<const std::byte> change_state,
    std::span<const std::byte> build_cache,
    baseline_commit_result& output) const noexcept {

    return commit(
        fingerprint,
        configuration,
        project_generation_segments{
            compiled,
            source_manager,
            change_state,
            build_cache},
        output);
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
