#pragma once

#include "persistence/baseline_store.hpp"
#include "persistence/compiled_image.hpp"
#include "persistence/source_manager_image.hpp"
#include "project_generation_segments.hpp"
#include "persistence/build_cache_image.hpp"
#include "persistence/change_state_image.hpp"
#include "project_configuration.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace cw::server {

class project_context;

struct project_generation_freeze_telemetry final {
    std::uint64_t internal_ns = 0;
    std::uint64_t materialize_change_ns = 0;
    std::uint64_t compiled_ns = 0;
    std::uint64_t roots_ns = 0;
    std::uint64_t source_manager_ns = 0;
    std::uint64_t change_state_ns = 0;
    std::uint64_t build_cache_ns = 0;
    std::uint64_t build_cache_layout_allocate_ns = 0;
    std::uint64_t build_cache_source_frontend_ns = 0;
    std::uint64_t build_cache_source_directory_text_ns = 0;
    std::uint64_t build_cache_frontend_record_ranges_ns = 0;
    std::uint64_t build_cache_frontend_local_types_ns = 0;
    std::uint64_t build_cache_frontend_type_slots_ns = 0;
    std::uint64_t build_cache_frontend_object_slots_ns = 0;
    std::uint64_t build_cache_frontend_member_slots_ns = 0;
    std::uint64_t build_cache_source_frontend_total_sources = 0;
    std::uint64_t build_cache_source_frontend_sampled_sources = 0;
    std::uint64_t build_cache_source_lookup_sample_ns = 0;
    std::uint64_t build_cache_source_text_copy_sample_ns = 0;
    std::uint64_t build_cache_frontend_record_sample_ns = 0;
    std::uint64_t build_cache_frontend_local_types_sample_ns = 0;
    std::uint64_t build_cache_frontend_type_slots_sample_ns = 0;
    std::uint64_t build_cache_frontend_object_slots_sample_ns = 0;
    std::uint64_t build_cache_frontend_member_slots_sample_ns = 0;
    std::uint64_t build_cache_contribution_ns = 0;
    std::uint64_t build_cache_graph_ns = 0;
    std::uint64_t build_cache_change_identity_ns = 0;
    std::uint64_t build_cache_section_crc_ns = 0;
    std::uint64_t build_cache_header_directory_ns = 0;
    std::uint64_t build_cache_bind_ns = 0;
    std::uint64_t build_cache_verify_ns = 0;
    std::uint64_t bind_ns = 0;
    std::uint64_t verify_change_state_ns = 0;
    std::uint64_t verify_build_cache_ns = 0;
};

// Temporary owner used while existing encoders are migrated to native
// Generation segments. Consumers see only project_generation_segments.
class project_generation_storage final {
public:
    project_generation_storage() = default;

    project_generation_storage(
        const project_generation_storage&) = delete;
    project_generation_storage& operator=(
        const project_generation_storage&) = delete;
    project_generation_storage(
        project_generation_storage&&) noexcept = default;
    project_generation_storage& operator=(
        project_generation_storage&&) noexcept = default;

        [[nodiscard]] project_generation_segments
    segments() const noexcept {
        const auto change_segment =
            !native_change.empty()
            ? native_change
            : std::span<const std::byte>{
                change_fallback.data(),
                change_fallback.size()};

        return {
            compiled,
            sources,
            change_segment,
            build,
        };
    }

        void reset() noexcept {
        compiled.clear();
        sources.clear();
        change_fallback.clear();
        native_change = {};
        build.clear();
    }

private:
    std::vector<std::byte> compiled;
    std::vector<std::byte> sources;
    std::vector<std::byte> change_fallback;
    std::span<const std::byte> native_change;
    std::vector<std::byte> build;

    friend status freeze_project_generation(
        const project_context&,
        const project_configuration&,
        project_generation_storage&) noexcept;

    friend status freeze_project_generation(
        const project_context&,
        const project_configuration&,
        project_generation_storage&,
        project_generation_freeze_telemetry*) noexcept;

    friend status freeze_project_generation(
        const project_context&,
        project_generation_storage&) noexcept;
};

[[nodiscard]] status freeze_project_generation(
    const project_context& project,
    const project_configuration& configuration,
    project_generation_storage& output) noexcept;

[[nodiscard]] status freeze_project_generation(
    const project_context& project,
    const project_configuration& configuration,
    project_generation_storage& output,
    project_generation_freeze_telemetry* telemetry) noexcept;

[[nodiscard]] status freeze_project_generation(
    const project_context& project,
    project_generation_storage& output) noexcept;

using project_baseline_images = project_generation_storage;

// Reads only project.json filesystem metadata. This is the hot BUILD identity
// check; semantic compatibility is still decided by the baseline fingerprint
// after any metadata miss.
[[nodiscard]] status observe_project_configuration(
    const std::filesystem::path& configuration_path,
    file_snapshot_observation& output) noexcept;

// Produces the compatibility fingerprint used by manifest.bin. Source contents
// are intentionally excluded; they are validated by source_manager.bin.
[[nodiscard]] status make_project_baseline_fingerprint(
    const project_configuration& configuration,
    baseline_fingerprint& output) noexcept;

// Encodes one coherent READY construction state using an explicitly validated
// full configuration. Sparse baseline-backed BUILD may retain only execution
// metadata until this cold SAVE boundary.
[[nodiscard]] status encode_project_baseline(
    const project_context& project,
    const project_configuration& configuration,
    project_baseline_images& output) noexcept;

[[nodiscard]] status encode_project_baseline(
    const project_context& project,
    project_baseline_images& output) noexcept;

// Compares filesystem Sources with one persisted Source Manager image. Metadata is
// only a fast path; content hash decides semantic equality after a metadata miss.
struct project_dirty_source_telemetry final {
    std::uint64_t journal_records = 0;
    std::uint64_t journal_matched_sources = 0;
    std::uint32_t backend = 0;
    bool fast_path = false;
    bool fallback = false;
};

[[nodiscard]] status project_baseline_dirty_sources(
    const source_manager_image_view& sources,
    std::vector<source_id>& dirty_sources) noexcept;

[[nodiscard]] status project_baseline_dirty_sources(
    const source_manager_image_view& sources,
    std::vector<source_id>& dirty_sources,
    project_dirty_source_telemetry& telemetry) noexcept;

[[nodiscard]] status project_baseline_dirty_sources(
    const change_state_image_view& sources,
    std::vector<source_id>& dirty_sources,
    project_dirty_source_telemetry& telemetry) noexcept;

[[nodiscard]] status project_baseline_dirty_sources(
    const change_state_image_view& sources,
    std::vector<source_change_journal_candidate>& candidates,
    project_dirty_source_telemetry& telemetry) noexcept;


[[nodiscard]] status project_baseline_dirty_sources(
    const source_manager_image_view& sources,
    const file_change_token& configuration,
    std::vector<source_id>& dirty_sources,
    bool& configuration_proven,
    bool& configuration_changed,
    project_dirty_source_telemetry& telemetry) noexcept;

[[nodiscard]] status project_baseline_dirty_sources(
    const change_state_image_view& sources,
    const file_change_token& configuration,
    std::vector<source_id>& dirty_sources,
    bool& configuration_proven,
    bool& configuration_changed,
    project_dirty_source_telemetry& telemetry) noexcept;

[[nodiscard]] status project_baseline_dirty_sources(
    const change_state_image_view& sources,
    const file_change_token& configuration,
    std::vector<source_change_journal_candidate>& candidates,
    bool& configuration_proven,
    bool& configuration_changed,
    project_dirty_source_telemetry& telemetry) noexcept;

[[nodiscard]] status project_baseline_resolve_candidates(
    const source_manager_image_view& sources,
    std::span<const source_change_journal_candidate> candidates,
    std::vector<source_id>& dirty_sources,
    project_dirty_source_telemetry& telemetry) noexcept;


[[nodiscard]] status project_baseline_sources_changed(
    const source_manager_image_view& sources,
    bool& changed) noexcept;

} // namespace cw::server
