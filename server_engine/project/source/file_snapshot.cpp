#include "file_snapshot.hpp"

#include <chrono>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>

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

} // namespace

file_snapshot_result acquire_file_snapshot(
    const std::filesystem::path& path,
    const std::optional<file_snapshot_observation>& baseline,
    file_snapshot& output) noexcept {

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
}

} // namespace cw::server
