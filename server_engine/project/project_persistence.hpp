#pragma once

#include "persistence/baseline_store.hpp"
#include "persistence/compiled_image.hpp"
#include "persistence/source_manager_image.hpp"
#include "persistence/build_cache_image.hpp"
#include "persistence/change_state_image.hpp"
#include "project_configuration.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace cw::server {

class project_context;

struct project_baseline_images final {
    std::vector<std::byte> compiled;
    std::vector<std::byte> source_manager;
    std::vector<std::byte> change_state;
    std::vector<std::byte> build_cache;
};

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
