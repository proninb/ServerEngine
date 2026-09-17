#include "project_persistence.hpp"

#include "project_context.hpp"
#include "source/file_snapshot.hpp"
#include "source/source_hash.hpp"
#include "source/source_manager.hpp"
#include "source/source_change_tracker.hpp"

#include <array>
#include <chrono>
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

status project_generation_storage::materialize_owned_contiguous() noexcept {
    try {
        const auto flatten =
            [](const project_generation_segment& segment,
               std::vector<std::byte>& output) -> status {

            if (segment.empty()) {
                output.clear();
                return {};
            }

            std::vector<std::byte> replacement;
            replacement.reserve(segment.size());

            for (std::size_t index = 0;
                 index < segment.extent_count();
                 ++index) {

                const auto extent = segment.extent(index);
                replacement.insert(
                    replacement.end(),
                    extent.begin(),
                    extent.end());
            }

            if (replacement.size() != segment.size())
                return {status_code::initialization_failed};

            output.swap(replacement);
            return {};
        };

        auto result = status{};

        if (native_sources.valid() ||
            sparse_sources.valid()) {

            const auto segment =
                native_sources.valid()
                    ? native_sources.segment()
                    : sparse_sources.segment();

            result = flatten(segment, sources);
            if (!result.ok())
                return result;

            native_sources.reset();
            sparse_sources.reset();
        }

        if (!native_change.empty()) {
            const project_generation_segment segment{
                native_change};

            result = flatten(
                segment,
                change_fallback);
            if (!result.ok())
                return result;

            native_change = {};
        }

        // R5E2C-B: preserve the sectioned Build Cache when this
        // committed Generation pins the immutable native Frontend pages.
        if (build_sections.valid() &&
            !frontend_generation_owned()) {

            const auto segment =
                build_sections.logical_segment(
                    std::span<const std::byte>{
                        build.data(),
                        build.size()});

            result = flatten(segment, build);
            if (!result.ok())
                return result;

            build_sections.reset();
            build_owned_sections.reset();
        }

        baseline_reuse_provenance =
            baseline_commit_provenance{};

        const auto owned = segments();
        const bool sectioned_build_owned =
            build_sections.valid() &&
            frontend_generation_owned();

        if (!owned.persistable() ||
            (!compiled_sections.valid() &&
             !owned.compiled_segment().is_contiguous()) ||
            !owned.sources_segment().is_contiguous() ||
            !owned.change_segment().is_contiguous() ||
            (!sectioned_build_owned &&
             !owned.build_segment().is_contiguous())) {
            return {status_code::initialization_failed};
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
    project_generation_storage& output,
    project_generation_freeze_telemetry* telemetry,
    project_generation_freeze_mode mode) noexcept {

    output = {};
    if (telemetry != nullptr)
        *telemetry = {};

    const auto freeze_begin =
        std::chrono::steady_clock::now();

    const auto elapsed = [](
        std::chrono::steady_clock::time_point begin) noexcept {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin).count());
    };

    if (!project.construction_backed())
        return {status_code::invalid_state};

    output.frontend_generation_lifetime =
        project.frontend_cache().
            native_frontend_block_lifetime();

    if (project.frontend_cache().
            native_frontend_block_bulk_storage() != nullptr &&
        !output.frontend_generation_lifetime) {
        return {status_code::initialization_failed};
    }

    const source_change_capture* change_capture =
        project.generation_provenance().source_change();

    if (change_capture == nullptr)
        return {status_code::invalid_state};

    auto result = status{};
    source_change_capture materialized_change_capture;
    source_change_materialization_telemetry
        materialize_change_detail;

    if (change_capture->baseline_overlay()) {
        const auto phase_begin =
            std::chrono::steady_clock::now();

        result =
            materialize_generation_source_change_capture(
                project.sources(),
                *change_capture,
                materialized_change_capture,
                telemetry != nullptr
                    ? &materialize_change_detail
                    : nullptr);

        if (telemetry != nullptr) {
            telemetry->materialize_change_ns =
                elapsed(phase_begin);
            telemetry->materialize_change_update_index_allocate_zero_ns =
                materialize_change_detail.update_index_allocate_zero_ns;
            telemetry->materialize_change_baseline_file_count_ns =
                materialize_change_detail.baseline_file_count_ns;
            telemetry->materialize_change_file_index_allocate_zero_ns =
                materialize_change_detail.file_index_allocate_zero_ns;
            telemetry->materialize_change_baseline_file_merge_ns =
                materialize_change_detail.baseline_file_merge_ns;
            telemetry->materialize_change_sparse_file_updates_ns =
                materialize_change_detail.sparse_file_updates_ns;
            telemetry->materialize_change_baseline_directory_count_ns =
                materialize_change_detail.baseline_directory_count_ns;
            telemetry->materialize_change_directory_index_allocate_zero_ns =
                materialize_change_detail.directory_index_allocate_zero_ns;
            telemetry->materialize_change_baseline_directory_merge_ns =
                materialize_change_detail.baseline_directory_merge_ns;
            telemetry->materialize_change_sparse_directory_updates_ns =
                materialize_change_detail.sparse_directory_updates_ns;
            telemetry->materialize_change_source_count =
                materialize_change_detail.source_count;
            telemetry->materialize_change_baseline_file_capacity =
                materialize_change_detail.baseline_file_capacity;
            telemetry->materialize_change_baseline_file_occupied =
                materialize_change_detail.baseline_file_occupied;
            telemetry->materialize_change_baseline_directory_capacity =
                materialize_change_detail.baseline_directory_capacity;
            telemetry->materialize_change_baseline_directory_occupied =
                materialize_change_detail.baseline_directory_occupied;
            telemetry->materialize_change_file_updates =
                materialize_change_detail.file_updates;
            telemetry->materialize_change_directory_updates =
                materialize_change_detail.directory_updates;
            telemetry->materialize_change_update_index_bytes =
                materialize_change_detail.update_index_bytes;
            telemetry->materialize_change_file_index_bytes =
                materialize_change_detail.file_index_bytes;
            telemetry->materialize_change_directory_index_bytes =
                materialize_change_detail.directory_index_bytes;
            telemetry->materialize_change_peak_temporary_bytes =
                materialize_change_detail.peak_temporary_bytes;
            telemetry->materialize_change_peak_owned_bytes =
                materialize_change_detail.peak_materialization_owned_bytes;
        }

        if (!result.ok())
            return result;

        change_capture =
            &materialized_change_capture;
    }

    if (mode == project_generation_freeze_mode::complete) {
        const auto compiled_begin =
            std::chrono::steady_clock::now();

        compiled_image_encode_telemetry
            compiled_detail;

        result =
            encode_compiled_image(
                project,
                output.compiled,
                telemetry != nullptr
                    ? &compiled_detail
                    : nullptr);

        if (telemetry != nullptr) {
            telemetry->compiled_ns =
                elapsed(compiled_begin);
            telemetry->compiled_total_ns =
                compiled_detail.total_ns;
            telemetry->compiled_sizing_layout_ns =
                compiled_detail.sizing_layout_ns;
            telemetry->compiled_allocate_zero_ns =
                compiled_detail.allocate_zero_ns;
            telemetry->compiled_strings_ns =
                compiled_detail.strings_ns;
            telemetry->compiled_identities_ns =
                compiled_detail.identities_ns;
            telemetry->compiled_graph_arrays_ns =
                compiled_detail.graph_arrays_ns;
            telemetry->compiled_graph_indexes_ns =
                compiled_detail.graph_indexes_ns;
            telemetry->compiled_section_crc_ns =
                compiled_detail.section_crc_ns;
            telemetry->compiled_header_bind_ns =
                compiled_detail.header_bind_ns;
            telemetry->compiled_baseline_bulk_bytes =
                compiled_detail.baseline_bulk_bytes;
            telemetry->compiled_baseline_bulk_sections =
                compiled_detail.baseline_bulk_sections;
            telemetry->compiled_output_bytes =
                compiled_detail.output_bytes;
        }

        if (!result.ok())
            return result;

    }

    try {
        const auto roots_begin =
            std::chrono::steady_clock::now();

        std::vector<source_manager_image_root> roots;

        const auto generation_root_count =
            project.generation_root_count();

        if (generation_root_count != 0) {
            roots.reserve(generation_root_count);

            for (std::size_t index = 0;
                 index < generation_root_count;
                 ++index) {

                source_manager_image_root root;
                result = project.generation_root(
                    index,
                    root);
                if (!result.ok())
                    return result;

                if (!root.source ||
                    static_cast<std::size_t>(
                        root.source.value()) >
                        project.sources().source_count()) {
                    return {
                        status_code::initialization_failed};
                }

                roots.push_back(root);
            }
        }
        else {
            // Portable compatibility fallback for legacy construction states
            // that own neither Generation root records nor a pinned baseline.
            roots.reserve(configuration.project.size());

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
                    if (normalized.empty())
                        return {status_code::invalid_argument};
                }
                else {
                    result = normalize_source_path(
                        item.path,
                        normalized);
                    if (!result.ok())
                        return result;
                }

                source_id source;
                result = project.sources().find(
                    normalized,
                    source);
                if (!result.ok()) {
                    return {
                        status_code::initialization_failed};
                }

                roots.push_back({
                    source,
                    item.role,
                });
            }
        }

        if (telemetry != nullptr)
            telemetry->roots_ns = elapsed(roots_begin);

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

        const auto source_manager_begin =
            std::chrono::steady_clock::now();

        const auto native_sources =
            project.sources().native_generation();

        source_manager_freeze_telemetry
            source_manager_detail;

        const auto* generation_change_capture =
            project.generation_provenance().
                source_change();

        const bool native_borrow_lifetime_safe =
            native_sources.complete &&
            change_capture ==
                generation_change_capture;

        const auto* baseline_source_image =
            project.sources().baseline_source_image();

        const bool sparse_baseline_candidate =
            baseline_source_image != nullptr &&
            generation_change_capture != nullptr &&
            generation_change_capture->baseline_overlay() &&
            !change_capture->baseline_overlay();

        if (native_borrow_lifetime_safe) {
            // GEN-02C12: use the native Source Manager image only when every
            // borrowed extent is Generation-owned and therefore remains alive
            // through baseline_store::commit().
            output.sources.clear();
            output.sparse_sources.reset();

            result =
                freeze_source_manager_native_image(
                    project.sources(),
                    options,
                    output.native_sources,
                    telemetry != nullptr
                        ? &source_manager_detail
                        : nullptr);
        }
        else if (sparse_baseline_candidate) {
            output.sources.clear();
            output.native_sources.reset();

            result =
                freeze_source_manager_sparse_baseline_image(
                    project.sources(),
                    *baseline_source_image,
                    options,
                    generation_change_capture->file_updates,
                    true,
                    output.sparse_sources,
                    telemetry != nullptr
                        ? &source_manager_detail
                        : nullptr);

            if (result.ok()) {
                // D4L1 direct-borrow contract for sparse Source Manager.
                // physical_state and both identity tables are reconstructed or
                // materialized, so they deliberately receive no provenance.
                constexpr std::array<
                    source_manager_image_section,
                    7> direct_sections{
                    source_manager_image_section::source_core,
                    source_manager_image_section::graph_records,
                    source_manager_image_section::forward_edges,
                    source_manager_image_section::reverse_edges,
                    source_manager_image_section::roots,
                    source_manager_image_section::path_index,
                    source_manager_image_section::path_bytes,
                };

                for (const auto section :
                     direct_sections) {

                    const auto raw =
                        static_cast<std::uint32_t>(
                            section);

                    if (raw == 0 ||
                        raw >
                            output.baseline_reuse_provenance.
                                source_manager.size()) {
                        return {
                            status_code::
                                initialization_failed};
                    }

                    const auto index =
                        static_cast<std::size_t>(
                            raw - 1);

                    const auto bytes =
                        baseline_source_image->
                            section_bytes(section);

                    if (bytes.empty())
                        continue;

                    output.baseline_reuse_provenance.
                        source_manager[index] =
                            project.
                                prove_baseline_section_borrow(
                                    baseline_artifact_kind::
                                        source_manager,
                                    index,
                                    bytes);
                }
            }

            // not_found is an explicit structural ineligibility signal:
            // topology/source-count/extent constraints fall back to the
            // established full encoder without weakening correctness.
            if (!result.ok() &&
                result.code == status_code::not_found) {

                const auto sparse_fallback_reason =
                    source_manager_detail.sparse_fallback_reason;
                const auto sparse_required_extent_count =
                    source_manager_detail.
                        sparse_required_extent_count;

                output.sparse_sources.reset();

                result =
                    encode_source_manager_image(
                        project.sources(),
                        options,
                        output.sources,
                        telemetry != nullptr
                            ? &source_manager_detail
                            : nullptr);

                source_manager_detail.sparse_fallback_reason =
                    sparse_fallback_reason;
                source_manager_detail.sparse_required_extent_count =
                    sparse_required_extent_count;
            }
        }
        else {
            output.native_sources.reset();
            output.sparse_sources.reset();

            result = encode_source_manager_image(
                project.sources(),
                options,
                output.sources,
                telemetry != nullptr
                    ? &source_manager_detail
                    : nullptr);
        }

        if (telemetry != nullptr) {
            telemetry->source_manager_ns =
                elapsed(source_manager_begin);
            telemetry->source_manager_internal_ns =
                source_manager_detail.internal_ns;
            telemetry->source_manager_preflight_ns =
                source_manager_detail.preflight_ns;
            telemetry->source_manager_layout_ns =
                source_manager_detail.layout_ns;
            telemetry->source_manager_allocate_zero_ns =
                source_manager_detail.allocate_zero_ns;
            telemetry->source_manager_source_records_ns =
                source_manager_detail.source_records_ns;
            telemetry->source_manager_roots_ns =
                source_manager_detail.roots_ns;
            telemetry->source_manager_path_index_ns =
                source_manager_detail.path_index_ns;
            telemetry->source_manager_file_identity_ns =
                source_manager_detail.file_identity_ns;
            telemetry->source_manager_directory_identity_ns =
                source_manager_detail.directory_identity_ns;
            telemetry->source_manager_crc_wall_ns =
                source_manager_detail.crc_wall_ns;
            telemetry->source_manager_crc_total_bytes =
                source_manager_detail.crc_total_bytes;
            telemetry->source_manager_crc_source_core_ns =
                source_manager_detail.crc_source_core_ns;
            telemetry->source_manager_crc_source_core_bytes =
                source_manager_detail.crc_source_core_bytes;
            telemetry->source_manager_crc_physical_state_ns =
                source_manager_detail.crc_physical_state_ns;
            telemetry->source_manager_crc_physical_state_bytes =
                source_manager_detail.crc_physical_state_bytes;
            telemetry->source_manager_crc_graph_records_ns =
                source_manager_detail.crc_graph_records_ns;
            telemetry->source_manager_crc_graph_records_bytes =
                source_manager_detail.crc_graph_records_bytes;
            telemetry->source_manager_crc_forward_edges_ns =
                source_manager_detail.crc_forward_edges_ns;
            telemetry->source_manager_crc_forward_edges_bytes =
                source_manager_detail.crc_forward_edges_bytes;
            telemetry->source_manager_crc_reverse_edges_ns =
                source_manager_detail.crc_reverse_edges_ns;
            telemetry->source_manager_crc_reverse_edges_bytes =
                source_manager_detail.crc_reverse_edges_bytes;
            telemetry->source_manager_crc_roots_ns =
                source_manager_detail.crc_roots_ns;
            telemetry->source_manager_crc_roots_bytes =
                source_manager_detail.crc_roots_bytes;
            telemetry->source_manager_crc_path_index_ns =
                source_manager_detail.crc_path_index_ns;
            telemetry->source_manager_crc_path_index_bytes =
                source_manager_detail.crc_path_index_bytes;
            telemetry->source_manager_crc_path_bytes_ns =
                source_manager_detail.crc_path_bytes_ns;
            telemetry->source_manager_crc_path_bytes_bytes =
                source_manager_detail.crc_path_bytes_bytes;
            telemetry->source_manager_crc_file_identity_ns =
                source_manager_detail.crc_file_identity_ns;
            telemetry->source_manager_crc_file_identity_bytes =
                source_manager_detail.crc_file_identity_bytes;
            telemetry->source_manager_crc_directory_identity_ns =
                source_manager_detail.crc_directory_identity_ns;
            telemetry->source_manager_crc_directory_identity_bytes =
                source_manager_detail.crc_directory_identity_bytes;
            telemetry->source_manager_prefix_directory_encode_ns =
                source_manager_detail.prefix_directory_encode_ns;
            telemetry->source_manager_directory_crc_ns =
                source_manager_detail.directory_crc_ns;
            telemetry->source_manager_header_crc_ns =
                source_manager_detail.header_crc_ns;
            telemetry->source_manager_bind_ns =
                source_manager_detail.bind_ns;
            telemetry->source_manager_verify_ns =
                source_manager_detail.verify_ns;
            telemetry->source_manager_segment_validate_ns =
                source_manager_detail.segment_validate_ns;
            telemetry->source_manager_identity_copy_ns =
                source_manager_detail.identity_copy_ns;
            telemetry->source_manager_mode =
                source_manager_detail.mode;
            telemetry->source_manager_crc_worker_count =
                source_manager_detail.crc_worker_count;
            telemetry->source_manager_extent_count =
                source_manager_detail.extent_count;
            telemetry->source_manager_sparse_required_extent_count =
                source_manager_detail.
                    sparse_required_extent_count;
            telemetry->source_manager_sparse_fallback_reason =
                source_manager_detail.sparse_fallback_reason;
        }

        if (!result.ok())
            return result;

        const auto change_state_begin =
            std::chrono::steady_clock::now();

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

        if (telemetry != nullptr) {
            telemetry->change_state_ns =
                elapsed(change_state_begin);
        }
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }

    const auto build_cache_begin =
        std::chrono::steady_clock::now();

    build_cache_encode_telemetry build_cache_detail;
    build_cache_encode_provenance
        build_cache_provenance;
    build_cache_encode_borrowed_sections
        build_cache_borrowed;
    build_cache_encode_sparse_sections
        build_cache_sparse;

    result = encode_build_cache_image(
        project,
        *change_capture,
        output.build,
        telemetry != nullptr
            ? &build_cache_detail
            : nullptr,
        &build_cache_provenance,
        &build_cache_borrowed,
        &build_cache_sparse,
        &output.build_owned_sections);

    if (telemetry != nullptr) {
        telemetry->build_cache_ns =
            elapsed(build_cache_begin);
        telemetry->build_cache_layout_allocate_ns =
            build_cache_detail.layout_allocate_ns;
        telemetry->build_cache_source_frontend_ns =
            build_cache_detail.source_frontend_ns;
        telemetry->build_cache_source_directory_text_ns =
            build_cache_detail.source_directory_text_ns;
        telemetry->build_cache_frontend_record_ranges_ns =
            build_cache_detail.frontend_record_ranges_ns;
        telemetry->build_cache_frontend_local_types_ns =
            build_cache_detail.frontend_local_types_ns;
        telemetry->build_cache_frontend_type_slots_ns =
            build_cache_detail.frontend_type_slots_ns;
        telemetry->build_cache_frontend_object_slots_ns =
            build_cache_detail.frontend_object_slots_ns;
        telemetry->build_cache_frontend_member_slots_ns =
            build_cache_detail.frontend_member_slots_ns;
        telemetry->build_cache_source_frontend_total_sources =
            build_cache_detail.source_frontend_total_sources;
        telemetry->build_cache_source_frontend_sampled_sources =
            build_cache_detail.source_frontend_sampled_sources;
        telemetry->build_cache_source_lookup_sample_ns =
            build_cache_detail.source_lookup_sample_ns;
        telemetry->build_cache_source_text_copy_sample_ns =
            build_cache_detail.source_text_copy_sample_ns;
        telemetry->build_cache_frontend_record_sample_ns =
            build_cache_detail.frontend_record_sample_ns;
        telemetry->build_cache_frontend_local_types_sample_ns =
            build_cache_detail.frontend_local_types_sample_ns;
        telemetry->build_cache_frontend_type_slots_sample_ns =
            build_cache_detail.frontend_type_slots_sample_ns;
        telemetry->build_cache_frontend_object_slots_sample_ns =
            build_cache_detail.frontend_object_slots_sample_ns;
        telemetry->build_cache_frontend_member_slots_sample_ns =
            build_cache_detail.frontend_member_slots_sample_ns;
        telemetry->build_cache_contribution_ns =
            build_cache_detail.contribution_ns;
        telemetry->build_cache_graph_ns =
            build_cache_detail.graph_ns;
        telemetry->build_cache_change_identity_ns =
            build_cache_detail.change_identity_ns;
        telemetry->build_cache_section_crc_ns =
            build_cache_detail.section_crc_ns;
        telemetry->build_cache_header_directory_ns =
            build_cache_detail.header_directory_ns;
        telemetry->build_cache_bind_ns =
            build_cache_detail.bind_ns;
        telemetry->build_cache_verify_ns =
            build_cache_detail.verify_ns;
        telemetry->build_cache_mapped_baseline_bulk_bytes =
            build_cache_detail.mapped_baseline_bulk_bytes;
        telemetry->build_cache_mapped_baseline_borrowed_bytes =
            build_cache_detail.mapped_baseline_borrowed_bytes;
        telemetry->build_cache_mapped_baseline_sparse_borrowed_bytes =
            build_cache_detail.mapped_baseline_sparse_borrowed_bytes;
        telemetry->build_cache_mapped_baseline_sparse_directory_borrowed_bytes =
            build_cache_detail.mapped_baseline_sparse_directory_borrowed_bytes;
        telemetry->build_cache_mapped_baseline_sparse_frontend_borrowed_bytes =
            build_cache_detail.mapped_baseline_sparse_frontend_borrowed_bytes;
        telemetry->build_cache_mapped_baseline_sparse_frontend_owned_bytes =
            build_cache_detail.mapped_baseline_sparse_frontend_owned_bytes;
        telemetry->build_cache_mapped_baseline_frontend_element_reads =
            build_cache_detail.mapped_baseline_frontend_element_reads;
        telemetry->build_cache_mapped_baseline_frontend_elements_encoded =
            build_cache_detail.mapped_baseline_frontend_elements_encoded;
        telemetry->build_cache_mapped_baseline_patch_records =
            build_cache_detail.mapped_baseline_patch_records;
        telemetry->build_cache_mapped_baseline_append_records =
            build_cache_detail.mapped_baseline_append_records;
        telemetry->build_cache_mapped_baseline_bulk_sections =
            build_cache_detail.mapped_baseline_bulk_sections;
        telemetry->build_cache_mapped_baseline_borrowed_sections =
            build_cache_detail.mapped_baseline_borrowed_sections;
        telemetry->build_cache_mapped_baseline_sparse_borrowed_extents =
            build_cache_detail.mapped_baseline_sparse_borrowed_extents;
        telemetry->build_cache_mapped_baseline_sparse_directory_borrowed_extents =
            build_cache_detail.mapped_baseline_sparse_directory_borrowed_extents;
        telemetry->build_cache_mapped_baseline_sparse_frontend_borrowed_extents =
            build_cache_detail.mapped_baseline_sparse_frontend_borrowed_extents;
        telemetry->build_cache_mapped_baseline_sparse_frontend_owned_extents =
            build_cache_detail.mapped_baseline_sparse_frontend_owned_extents;
        telemetry->build_cache_native_source_direct_bytes =
            build_cache_detail.native_source_direct_bytes;
        telemetry->build_cache_native_source_direct_extents =
            build_cache_detail.native_source_direct_extents;
        telemetry->build_cache_native_source_direct_sections =
            build_cache_detail.native_source_direct_sections;
        telemetry->build_cache_native_source_direct_fallback =
            build_cache_detail.native_source_direct_fallback;
        telemetry->build_cache_native_frontend_direct_bytes =
            build_cache_detail.native_frontend_direct_bytes;
        telemetry->build_cache_native_frontend_direct_extents =
            build_cache_detail.native_frontend_direct_extents;
        telemetry->build_cache_native_frontend_direct_sections =
            build_cache_detail.native_frontend_direct_sections;
        telemetry->build_cache_native_frontend_direct_fallback =
            build_cache_detail.native_frontend_direct_fallback;
        telemetry->build_cache_native_contribution_direct_bytes =
            build_cache_detail.native_contribution_direct_bytes;
        telemetry->build_cache_native_contribution_direct_extents =
            build_cache_detail.native_contribution_direct_extents;
        telemetry->build_cache_native_contribution_direct_sections =
            build_cache_detail.native_contribution_direct_sections;
        telemetry->build_cache_native_contribution_direct_fallback =
            build_cache_detail.native_contribution_direct_fallback;
    }

    if (!result.ok())
        return result;


    compiled_image_view compiled;
    source_manager_image_view sources;
    change_state_image_view change_state;
    build_cache_image_view build_cache;

    const auto frozen_segments =
        output.segments();

    const auto bind_begin =
        std::chrono::steady_clock::now();

    if (mode ==
        project_generation_freeze_mode::complete) {

        result = compiled.bind(output.compiled);
        if (!result.ok())
            return result;
    }

    if (!output.native_sources.valid() &&
        !output.sparse_sources.valid()) {
        const auto source_bytes =
            frozen_segments.sources_segment().
                contiguous();

        if (source_bytes.empty())
            return {status_code::invalid_state};

        result = sources.bind(source_bytes);
        if (!result.ok())
            return result;
    }

    result = change_state.bind(
        frozen_segments.change_segment().
            contiguous());
    if (!result.ok())
        return result;

    // R5E2D-B_GENERATION_SECTION_BIND
    if (output.build_owned_sections.active()) {
        std::array<
            project_generation_segment,
            build_cache_image_directory_count>
            section_values{};

        for (std::size_t index = 0;
             index < section_values.size();
             ++index) {

            if (!build_cache_sparse.sections[index].empty()) {
                section_values[index] =
                    build_cache_sparse.sections[index];
                continue;
            }

            if (!build_cache_borrowed.sections[index].empty()) {
                section_values[index] =
                    project_generation_segment{
                        build_cache_borrowed.sections[index]};
                continue;
            }

            const auto& section =
                output.build_owned_sections.sections[index];

            section_values[index] =
                project_generation_segment{
                    std::span<const std::byte>{
                        section.data(),
                        section.size()}};
        }

        result =
            build_cache.bind_sectioned(
                std::span<const std::byte>{
                    output.build.data(),
                    output.build.size()},
                section_values);
        if (!result.ok())
            return result;

        result =
            output.build_sections.bind_sectioned(
                std::span<const std::byte>{
                    output.build.data(),
                    output.build.size()},
                section_values,
                output.build_owned_sections.logical_size);
        if (!result.ok())
            return result;
    }
    else {
        result =
            build_cache_sparse.any()
            ? build_cache.bind_encoded_sparse(
                output.build,
                build_cache_borrowed,
                build_cache_sparse)
            : build_cache_borrowed.any()
                ? build_cache.bind_encoded_mixed(
                    output.build,
                    build_cache_borrowed)
                : build_cache.bind(output.build);
        if (!result.ok())
            return result;

        result = output.build_sections.
            bind_validated_sections(
                output.build,
                build_cache,
                build_cache_sparse.any()
                    ? &build_cache_sparse
                    : nullptr);
        if (!result.ok())
            return result;
    }

    if (telemetry != nullptr)
        telemetry->bind_ns = elapsed(bind_begin);

    // Encoder-local validation is the integrity boundary for compiled,
    // source-manager, and build-cache images. Change-state has no equivalent
    // encoder-local full audit, so keep its validation here.
    const auto verify_change_begin =
        std::chrono::steady_clock::now();

    result = change_state.verify_contents();

    if (telemetry != nullptr) {
        telemetry->verify_change_state_ns =
            elapsed(verify_change_begin);
    }

    if (!result.ok())
        return result;

    const auto verify_build_begin =
        std::chrono::steady_clock::now();

    if (mode == project_generation_freeze_mode::complete) {
        if (output.native_sources.valid()) {
            result =
                build_cache.verify_against_encoded_generation(
                    compiled,
                    project.sources(),
                    project.compiled_graph());
        }
        else if (output.sparse_sources.valid()) {
            result =
                build_cache.verify_against_sparse_generation(
                    compiled,
                    project.sources());
        }
        else {
            result =
                build_cache.verify_against(
                    compiled,
                    sources);
        }

    }
    else {
        result = {};
    }

    if (telemetry != nullptr) {
        telemetry->verify_build_cache_ns =
            elapsed(verify_build_begin);
    }

    if (!result.ok())
        return result;

    // D4P1: mint durable capabilities only after the complete frozen
    // Generation has passed cross-artifact validation. From this boundary to
    // synchronous commit, project_generation_storage exposes these bytes only
    // through const spans.
    const auto* baseline_build_cache =
        project.frontend_cache().
            baseline_persistence_image();

    if (baseline_build_cache != nullptr &&
        baseline_build_cache->valid()) {

        for (std::size_t index = 0;
             index <
                build_cache_image_directory_count;
             ++index) {

            if (!build_cache_provenance.exact(index))
                continue;

            const auto section =
                static_cast<
                    build_cache_image_section>(
                        index + 1);

            const auto bytes =
                baseline_build_cache->
                    section_bytes(section);

            if (bytes.empty())
                continue;

            const auto proof =
                project.
                    prove_baseline_section_borrow(
                        baseline_artifact_kind::
                            build_cache,
                        index,
                        bytes);

            if (!proof.valid())
                continue;

            if (!output.bind_build_cache_provenance(
                    index,
                    proof)) {
                return {
                    status_code::initialization_failed};
            }

            if (telemetry != nullptr) {
                telemetry->
                    build_cache_provenance_bytes +=
                        bytes.size();
                ++telemetry->
                    build_cache_provenance_sections;
            }
        }
    }

    if (telemetry != nullptr) {
        const auto frozen = output.segments();

        telemetry->audit_compiled_bytes =
            frozen.compiled_segment().size();
        telemetry->audit_source_manager_bytes =
            frozen.sources_segment().size();
        telemetry->audit_change_state_bytes =
            frozen.change_segment().size();
        telemetry->audit_build_cache_bytes =
            frozen.build_segment().size();

        const auto reconstructed =
            static_cast<std::uint32_t>(
                project_generation_persistence_origin::
                    reconstructed);
        const auto generation_owned =
            static_cast<std::uint32_t>(
                project_generation_persistence_origin::
                    generation_owned);
        const auto mixed_generation =
            static_cast<std::uint32_t>(
                project_generation_persistence_origin::
                    mixed_generation);
        const auto mixed_baseline =
            static_cast<std::uint32_t>(
                project_generation_persistence_origin::
                    mixed_baseline);

        telemetry->audit_compiled_origin =
            output.compiled.empty()
                ? 0u
                : reconstructed;

        telemetry->audit_source_manager_origin =
            output.native_sources.valid()
                ? mixed_generation
                : output.sparse_sources.valid()
                    ? mixed_baseline
                    : output.sources.empty()
                        ? 0u
                        : reconstructed;

        telemetry->audit_change_state_origin =
            !output.native_change.empty()
                ? generation_owned
                : output.change_fallback.empty()
                    ? 0u
                    : reconstructed;

        telemetry->audit_build_cache_origin =
            output.build.empty()
                ? 0u
                : reconstructed;

        for (const auto& proof :
             output.baseline_reuse_provenance.
                 source_manager) {

            if (!proof.valid())
                continue;

            telemetry->
                audit_source_manager_baseline_direct_borrow_bytes +=
                    proof.bytes().size();
            ++telemetry->
                audit_source_manager_baseline_direct_borrow_sections;
        }

        for (const auto& proof :
             output.baseline_reuse_provenance.
                 build_cache) {

            if (!proof.valid())
                continue;

            telemetry->
                audit_build_cache_baseline_exact_bytes +=
                    proof.baseline().bytes().size();
            ++telemetry->
                audit_build_cache_baseline_exact_sections;
        }

        telemetry->audit_staging_ns =
            telemetry->materialize_change_ns +
            telemetry->compiled_ns +
            telemetry->roots_ns +
            telemetry->source_manager_ns +
            telemetry->change_state_ns +
            telemetry->build_cache_ns;

        telemetry->audit_validation_ns =
            telemetry->bind_ns +
            telemetry->verify_change_state_ns +
            telemetry->verify_build_cache_ns;

        telemetry->internal_ns =
            elapsed(freeze_begin);

        const auto accounted =
            telemetry->audit_staging_ns +
            telemetry->audit_validation_ns;

        telemetry->audit_unclassified_ns =
            telemetry->internal_ns > accounted
                ? telemetry->internal_ns - accounted
                : 0;
    }

    return {};
}

status freeze_project_generation(
    const project_context& project,
    const project_configuration& configuration,
    project_generation_storage& output,
    project_generation_freeze_telemetry* telemetry) noexcept {

    return freeze_project_generation(
        project,
        configuration,
        output,
        telemetry,
        project_generation_freeze_mode::complete);
}

status freeze_project_generation(
    const project_context& project,
    const project_configuration& configuration,
    project_generation_storage& output) noexcept {

    return freeze_project_generation(
        project,
        configuration,
        output,
        nullptr);
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
