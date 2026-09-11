#include "project_persistence.hpp"

#include "project_context.hpp"
#include "source/file_snapshot.hpp"
#include "source/source_hash.hpp"
#include "source/source_manager.hpp"

#include <array>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>

namespace cw::server {
namespace {

constexpr std::string_view fingerprint_tag =
    "SE-V3-PROJECT-BASELINE-COMPATIBILITY-V1";
constexpr std::uint32_t build_contract_version = 2;

void append_u32(std::string& output, std::uint32_t value) {
    output.push_back(static_cast<char>(value & 0xffu));
    output.push_back(static_cast<char>((value >> 8) & 0xffu));
    output.push_back(static_cast<char>((value >> 16) & 0xffu));
    output.push_back(static_cast<char>((value >> 24) & 0xffu));
}

void append_bytes(std::string& output, std::string_view value) {
    append_u32(output, static_cast<std::uint32_t>(value.size()));
    output.append(value);
}

[[nodiscard]] bool append_checked(
    std::string& output,
    std::string_view value) {

    if (value.size() > (std::numeric_limits<std::uint32_t>::max)())
        return false;
    append_bytes(output, value);
    return true;
}

} // namespace

status make_project_baseline_fingerprint(
    const project_configuration& configuration,
    baseline_fingerprint& output) noexcept {

    output = {};

    try {
        std::string canonical;
        canonical.reserve(
            fingerprint_tag.size() +
            configuration.name.size() +
            configuration.project.size() * 64);

        canonical.append(fingerprint_tag);

        append_u32(canonical, baseline_format_version);
        append_u32(canonical, source_manager_image_format_version);
        append_u32(canonical, compiled_image_format_version);
        append_u32(canonical, build_cache_image_format_version);
        append_u32(canonical, build_contract_version);
        append_u32(canonical, configuration.version);
        append_u32(
            canonical,
            static_cast<std::uint32_t>(configuration.abi.target));
        append_u32(canonical, configuration.abi.pack);

        if (!append_checked(canonical, configuration.name) ||
            configuration.project.size() >
                (std::numeric_limits<std::uint32_t>::max)()) {
            return {status_code::not_available};
        }

        append_u32(
            canonical,
            static_cast<std::uint32_t>(configuration.project.size()));

        for (const auto& item : configuration.project) {
            std::string normalized;
            const auto normalize_result =
                normalize_source_path(item.path, normalized);
            if (!normalize_result.ok())
                return normalize_result;

            append_u32(
                canonical,
                static_cast<std::uint32_t>(item.role));

            if (!append_checked(canonical, normalized))
                return {status_code::not_available};
        }

        const auto digest = hash_source_content(canonical);
        for (std::size_t index = 0; index < output.bytes.size(); ++index) {
            output.bytes[index] =
                std::to_integer<std::uint8_t>(digest.bytes[index]);
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

status encode_project_baseline(
    const project_context& project,
    project_baseline_images& output) noexcept {

    output = {};

    if (!project.construction_backed())
        return {status_code::invalid_state};

    auto result =
        encode_compiled_image(project, output.compiled);
    if (!result.ok())
        return result;

    try {
        std::vector<source_manager_image_root> roots;
        roots.reserve(project.configuration().project.size());

        for (const auto& item : project.configuration().project) {
            std::string normalized;
            result = normalize_source_path(item.path, normalized);
            if (!result.ok())
                return result;

            source_id source;
            result = project.sources().find(normalized, source);
            if (!result.ok())
                return status{status_code::initialization_failed};

            roots.push_back({source, item.role});
        }

        source_manager_image_options options;
        options.generation = 1;
        options.roots = std::span<const source_manager_image_root>{roots};

        result = encode_source_manager_image(
            project.sources(),
            options,
            output.source_manager);
        if (!result.ok())
            return result;
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }

    result = encode_build_cache_image(
        project,
        output.build_cache);
    if (!result.ok())
        return result;

    compiled_image_view compiled;
    source_manager_image_view sources;
    build_cache_image_view build_cache;

    result = compiled.bind(output.compiled);
    if (!result.ok())
        return result;
    result = sources.bind(output.source_manager);
    if (!result.ok())
        return result;
    result = build_cache.bind(output.build_cache);
    if (!result.ok())
        return result;

    result = compiled.verify_contents();
    if (!result.ok())
        return result;
    result = sources.verify_contents();
    if (!result.ok())
        return result;
    result = build_cache.verify_contents();
    if (!result.ok())
        return result;

    return build_cache.verify_against(compiled, sources);
}

status project_baseline_dirty_sources(
    const source_manager_image_view& sources,
    std::vector<source_id>& dirty_sources) noexcept {

    dirty_sources.clear();
    if (!sources.valid())
        return {status_code::invalid_state};

    try {
        for (std::size_t index = 0; index < sources.source_count(); ++index) {
            if (index >= (std::numeric_limits<std::uint32_t>::max)())
                return {status_code::artifact_corrupt};

            const source_id source{
                static_cast<std::uint32_t>(index + 1)};

            source_manager_image_physical_state physical;
            auto result = sources.physical(source, physical);
            if (!result.ok())
                return result;

            const auto path_text = sources.path(source);
            if (path_text.empty())
                return {status_code::artifact_corrupt};

            std::optional<file_snapshot_observation> baseline;
            if (physical.present) {
                if (physical.size >
                    (std::numeric_limits<std::uintmax_t>::max)()) {
                    return {status_code::artifact_corrupt};
                }

                baseline = file_snapshot_observation{
                    physical.write_time_ticks,
                    static_cast<std::uintmax_t>(physical.size)};
            }

            file_snapshot acquired;
            const auto acquisition = acquire_file_snapshot(
                std::filesystem::path{path_text},
                baseline,
                acquired);

            bool dirty = false;
            switch (acquisition) {
            case file_snapshot_result::unchanged:
                if (!physical.present)
                    return {status_code::artifact_corrupt};
                break;

            case file_snapshot_result::missing:
                dirty = physical.present;
                break;

            case file_snapshot_result::acquired:
                dirty = !physical.present ||
                    acquired.hash != physical.hash;
                break;

            case file_snapshot_result::changed_during_read:
            case file_snapshot_result::failed:
                return {status_code::io_failed};

            case file_snapshot_result::allocation_failed:
                return {status_code::not_available};
            }

            if (dirty)
                dirty_sources.push_back(source);
        }
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

status project_baseline_sources_changed(
    const source_manager_image_view& sources,
    bool& changed) noexcept {

    std::vector<source_id> dirty_sources;
    const auto result = project_baseline_dirty_sources(
        sources,
        dirty_sources);
    changed = result.ok() && !dirty_sources.empty();
    return result;
}

} // namespace cw::server
