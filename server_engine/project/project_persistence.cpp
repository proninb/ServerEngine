#include "project_persistence.hpp"

#include "project_context.hpp"
#include "source/file_snapshot.hpp"
#include "source/source_hash.hpp"
#include "source/source_manager.hpp"
#include "source/source_change_tracker.hpp"

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
constexpr std::uint32_t build_contract_version = 3;

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

status observe_project_configuration(
    const std::filesystem::path& configuration_path,
    file_snapshot_observation& output) noexcept {

    output = {};

    std::error_code error;
    const auto file_status =
        std::filesystem::status(
            configuration_path,
            error);
    if (error) {
        return error ==
            std::errc::no_such_file_or_directory
            ? status{status_code::not_found}
            : status{status_code::io_failed};
    }

    if (!std::filesystem::exists(file_status))
        return {status_code::not_found};
    if (!std::filesystem::is_regular_file(file_status))
        return {status_code::io_failed};

    const auto size =
        std::filesystem::file_size(
            configuration_path,
            error);
    if (error)
        return {status_code::io_failed};

    const auto write_time =
        std::filesystem::last_write_time(
            configuration_path,
            error);
    if (error)
        return {status_code::io_failed};

    output.size = size;
    output.write_time_ticks =
        static_cast<std::int64_t>(
            write_time.time_since_epoch().count());
    return {};
}

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

            if (item.canonical_path) {
                normalized =
                    item.path.generic_string();
#ifdef _WIN32
                if (normalized.size() >= 2 &&
                    normalized[1] == ':' &&
                    normalized[0] >= 'A' &&
                    normalized[0] <= 'Z') {
                    normalized[0] =
                        static_cast<char>(
                            normalized[0] - 'A' + 'a');
                }
#endif
            }
            else {
                const auto normalize_result =
                    normalize_source_path(
                        item.path,
                        normalized);
                if (!normalize_result.ok())
                    return normalize_result;
            }

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

status freeze_project_generation(
    const project_context& project,
    const project_configuration& configuration,
    project_generation_storage& output) noexcept {

    output = {};

    if (!project.construction_backed())
        return {status_code::invalid_state};

    source_change_capture fallback_change_capture;
    const source_change_capture* change_capture =
        project.generation_provenance().source_change();

    auto result = status{};
    if (change_capture == nullptr) {
        result =
            prepare_source_change_capture(
                project.sources(),
                fallback_change_capture);
        if (!result.ok())
            return result;

        change_capture =
            &fallback_change_capture;
    }

    result =
        encode_compiled_image(
            project,
            output.compiled);
    if (!result.ok())
        return result;

    try {
        std::vector<source_manager_image_root> roots;
        roots.reserve(configuration.project.size());

        for (const auto& item : configuration.project) {
            std::string normalized;
            result = normalize_source_path(
                item.path,
                normalized);
            if (!result.ok())
                return result;

            source_id source;
            result = project.sources().find(
                normalized,
                source);
            if (!result.ok())
                return {status_code::initialization_failed};

            roots.push_back(
                {source, item.role});
        }

        source_manager_image_options options;
        options.generation = 1;
        options.roots =
            std::span<const source_manager_image_root>{roots};
        options.change_checkpoint =
            change_capture->checkpoint;
        options.file_identity_index =
            change_capture->file_index;
        options.directory_identity_index =
            change_capture->directory_index;

        result = encode_source_manager_image(
            project.sources(),
            options,
            output.sources);
        if (!result.ok())
            return result;

        const auto native_change =
            project.generation_native_segments().change();

        if (!native_change.empty()) {
            output.native_change = native_change;
        }
        else {
            result = encode_change_state_image(
                project.sources().source_count(),
                change_capture->journal_anchor_path,
                *change_capture,
                output.change_fallback);
            if (!result.ok())
                return result;
        }
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }

    result = encode_build_cache_image(
        project,
        *change_capture,
        output.build);
    if (!result.ok())
        return result;

    compiled_image_view compiled;
    source_manager_image_view sources;
    change_state_image_view change_state;
    build_cache_image_view build_cache;

    result = compiled.bind(output.compiled);
    if (!result.ok())
        return result;

    result = sources.bind(output.sources);
    if (!result.ok())
        return result;

    result = change_state.bind(output.segments().change());
    if (!result.ok())
        return result;

    result = build_cache.bind(output.build);
    if (!result.ok())
        return result;

    // Encoder-local validation is the integrity boundary for compiled,
    // source-manager, and build-cache images. Change-state has no equivalent
    // encoder-local full audit, so keep its validation here.
    result = change_state.verify_contents();
    if (!result.ok())
        return result;

    return build_cache.verify_against(
        compiled,
        sources);
}

status freeze_project_generation(
    const project_context& project,
    project_generation_storage& output) noexcept {

    return freeze_project_generation(
        project,
        project.configuration(),
        output);
}


status encode_project_baseline(
    const project_context& project,
    const project_configuration& configuration,
    project_baseline_images& output) noexcept {

    return freeze_project_generation(
        project,
        configuration,
        output);
}

status encode_project_baseline(
    const project_context& project,
    project_baseline_images& output) noexcept {

    return freeze_project_generation(
        project,
        output);
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
            // Fallback is a correctness boundary, not a stat-cache hint.
            // Equal size+mtime must never suppress content verification.
            const auto acquisition = acquire_file_snapshot(
                std::filesystem::path{path_text},
                std::nullopt,
                acquired);

            bool dirty = false;
            switch (acquisition) {
            case file_snapshot_result::unchanged:
                return {status_code::artifact_corrupt};

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

status project_baseline_dirty_sources(
    const source_manager_image_view& sources,
    std::vector<source_id>& dirty_sources,
    project_dirty_source_telemetry& telemetry) noexcept {

    dirty_sources.clear();
    telemetry = {};

    source_change_detection_telemetry detector;
    const auto fast_result =
        detect_source_changes(
            sources,
            dirty_sources,
            detector);

    telemetry.journal_records =
        detector.journal_records;
    telemetry.journal_matched_sources =
        detector.matched_sources;
    telemetry.backend =
        static_cast<std::uint32_t>(
            detector.backend);
    telemetry.fast_path =
        detector.fast_path;
    telemetry.fallback =
        detector.fallback;

    if (fast_result.ok())
        return {};

    if (fast_result.code != status_code::not_found) {
        dirty_sources.clear();
        return fast_result;
    }

    telemetry.fallback = true;
    return project_baseline_dirty_sources(
        sources,
        dirty_sources);
}


status project_baseline_dirty_sources(
    const source_manager_image_view& sources,
    const file_change_token& configuration,
    std::vector<source_id>& dirty_sources,
    bool& configuration_proven,
    bool& configuration_changed,
    project_dirty_source_telemetry& telemetry) noexcept {

    dirty_sources.clear();
    telemetry = {};
    configuration_proven = false;
    configuration_changed = false;

    source_change_detection_telemetry detector;
    const auto fast_result =
        detect_source_changes(
            sources,
            configuration,
            dirty_sources,
            configuration_proven,
            configuration_changed,
            detector);

    telemetry.journal_records =
        detector.journal_records;
    telemetry.journal_matched_sources =
        detector.matched_sources;
    telemetry.backend =
        static_cast<std::uint32_t>(
            detector.backend);
    telemetry.fast_path =
        detector.fast_path;
    telemetry.fallback =
        detector.fallback;

    if (fast_result.ok())
        return {};

    configuration_proven = false;

    if (fast_result.code != status_code::not_found) {
        dirty_sources.clear();
        return fast_result;
    }

    telemetry.fallback = true;
    return project_baseline_dirty_sources(
        sources,
        dirty_sources);
}


status project_baseline_dirty_sources(
    const change_state_image_view& sources,
    std::vector<source_change_journal_candidate>& candidates,
    project_dirty_source_telemetry& telemetry) noexcept {
    candidates.clear(); telemetry = {};
    source_change_detection_telemetry detector;
    const auto result = detect_source_changes(sources, candidates, detector);
    telemetry.journal_records = detector.journal_records;
    telemetry.journal_matched_sources = detector.matched_sources;
    telemetry.backend = static_cast<std::uint32_t>(detector.backend);
    telemetry.fast_path = detector.fast_path;
    telemetry.fallback = detector.fallback;
    return result;
}

status project_baseline_dirty_sources(
    const change_state_image_view& sources,
    const file_change_token& configuration,
    std::vector<source_change_journal_candidate>& candidates,
    bool& configuration_proven,
    bool& configuration_changed,
    project_dirty_source_telemetry& telemetry) noexcept {
    candidates.clear(); telemetry = {}; configuration_proven = false; configuration_changed = false;
    source_change_detection_telemetry detector;
    const auto result = detect_source_changes(sources, configuration, candidates,
        configuration_proven, configuration_changed, detector);
    telemetry.journal_records = detector.journal_records;
    telemetry.journal_matched_sources = detector.matched_sources;
    telemetry.backend = static_cast<std::uint32_t>(detector.backend);
    telemetry.fast_path = detector.fast_path;
    telemetry.fallback = detector.fallback;
    return result;
}

status project_baseline_resolve_candidates(
    const source_manager_image_view& sources,
    std::span<const source_change_journal_candidate> candidates,
    std::vector<source_id>& dirty_sources,
    project_dirty_source_telemetry& telemetry) noexcept {
    source_change_detection_telemetry detector;
    const auto result = resolve_source_change_candidates(sources, candidates, dirty_sources, detector);
    telemetry.journal_matched_sources = detector.matched_sources;
    if (detector.backend != source_change_backend::none)
        telemetry.backend = static_cast<std::uint32_t>(detector.backend);
    telemetry.fast_path = telemetry.fast_path || detector.fast_path;
    return result;
}


status project_baseline_dirty_sources(
    const change_state_image_view& sources,
    std::vector<source_id>& dirty_sources,
    project_dirty_source_telemetry& telemetry) noexcept {

    dirty_sources.clear();
    telemetry = {};

    source_change_detection_telemetry detector;
    const auto result =
        detect_source_changes(
            sources,
            dirty_sources,
            detector);

    telemetry.journal_records = detector.journal_records;
    telemetry.journal_matched_sources = detector.matched_sources;
    telemetry.backend =
        static_cast<std::uint32_t>(detector.backend);
    telemetry.fast_path = detector.fast_path;
    telemetry.fallback = detector.fallback;
    return result;
}


status project_baseline_dirty_sources(
    const change_state_image_view& sources,
    const file_change_token& configuration,
    std::vector<source_id>& dirty_sources,
    bool& configuration_proven,
    bool& configuration_changed,
    project_dirty_source_telemetry& telemetry) noexcept {

    dirty_sources.clear();
    telemetry = {};
    configuration_proven = false;
    configuration_changed = false;

    source_change_detection_telemetry detector;
    const auto result =
        detect_source_changes(
            sources,
            configuration,
            dirty_sources,
            configuration_proven,
            configuration_changed,
            detector);

    telemetry.journal_records = detector.journal_records;
    telemetry.journal_matched_sources = detector.matched_sources;
    telemetry.backend =
        static_cast<std::uint32_t>(detector.backend);
    telemetry.fast_path = detector.fast_path;
    telemetry.fallback = detector.fallback;
    return result;
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
