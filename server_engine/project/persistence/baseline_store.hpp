#pragma once

#include "../../status.hpp"
#include "../source/file_snapshot.hpp"
#include "../source/source_change_tracker.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cw::server {

inline constexpr std::uint32_t baseline_format_version = 1;
inline constexpr std::size_t baseline_fingerprint_size = 32;

struct baseline_fingerprint final {
    std::array<std::uint8_t, baseline_fingerprint_size> bytes{};

    friend constexpr bool operator==(
        const baseline_fingerprint&,
        const baseline_fingerprint&) noexcept = default;
};

enum class baseline_artifact_kind : std::uint8_t {
    compiled,
    source_manager,
    change_state,
    build_cache,
};

struct baseline_commit_result final {
    std::string transaction;
    std::uint64_t bytes_written = 0;
};

// Persisted project.json fast-path identity. Filesystem metadata is only a
// change token; semantic compatibility remains the baseline fingerprint.
struct baseline_configuration_state final {
    file_snapshot_observation observation{};
    source_content_hash content_hash{};
    file_change_token change_token{};
    std::uint32_t project_version = 0;
    std::uint32_t abi_target = 0;
    std::uint32_t abi_pack = 0;
    bool available = false;
    bool content_hash_available = false;
    bool change_token_available = false;
};

struct baseline_probe final {
    baseline_fingerprint fingerprint{};
    baseline_configuration_state configuration{};
    std::string transaction;
    std::uint64_t source_manager_size = 0;
    std::uint64_t build_cache_size = 0;
};

struct baseline_open_telemetry final {
    std::uint64_t current_read_ns = 0;
    std::uint64_t embedded_manifest_parse_ns = 0;
    std::uint64_t manifest_validation_ns = 0;
    std::uint64_t compiled_map_ns = 0;
    std::uint64_t source_manager_map_ns = 0;
    std::uint64_t change_state_map_ns = 0;
    std::uint64_t build_cache_map_ns = 0;
    std::uint64_t size_validation_ns = 0;
};

// Owns one read-only mapped file. Mapping lifetime pins the backing artifact even
// after a later SAVE commits a different baseline for the next LOAD/BUILD.
class read_only_file_mapping final {
public:
    read_only_file_mapping() noexcept;
    ~read_only_file_mapping() noexcept;

    read_only_file_mapping(const read_only_file_mapping&) = delete;
    read_only_file_mapping& operator=(const read_only_file_mapping&) = delete;
    read_only_file_mapping(read_only_file_mapping&&) noexcept;
    read_only_file_mapping& operator=(read_only_file_mapping&&) noexcept;

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
    [[nodiscard]] bool open() const noexcept;

private:
    [[nodiscard]] status map(const std::filesystem::path& path) noexcept;

    struct state;
    std::unique_ptr<state> value;

    friend class baseline_store;
};

// One committed persisted baseline. READY LOAD may intentionally own only the
// compiled/source-manager mappings; BUILD/SAVE may open all three artifacts.
class baseline_snapshot final {
public:
    baseline_snapshot() noexcept = default;

    baseline_snapshot(const baseline_snapshot&) = delete;
    baseline_snapshot& operator=(const baseline_snapshot&) = delete;
    baseline_snapshot(baseline_snapshot&&) noexcept = default;
    baseline_snapshot& operator=(baseline_snapshot&&) noexcept = default;

    [[nodiscard]] std::span<const std::byte> artifact(
        baseline_artifact_kind kind) const noexcept;

    [[nodiscard]] bool mapped(
        baseline_artifact_kind kind) const noexcept;

    [[nodiscard]] const baseline_fingerprint& fingerprint() const noexcept {
        return fingerprint_value;
    }

    [[nodiscard]] std::string_view transaction() const noexcept {
        return transaction_value;
    }

    [[nodiscard]] bool valid() const noexcept {
        return !transaction_value.empty();
    }

private:
    baseline_fingerprint fingerprint_value{};
    std::string transaction_value;
    read_only_file_mapping compiled;
    read_only_file_mapping source_manager;
    read_only_file_mapping change_state;

    // CURRENT v3 may own the compact BUILD decision gate directly. Canonical
    // change_state.bin remains available for transaction compatibility.
    std::vector<std::byte> embedded_change_state;

    read_only_file_mapping build_cache;

    friend class baseline_store;
};

// Transactional persistent-baseline storage. Artifact directories are immutable;
// CURRENT is the only selector changed during commit.
class baseline_store final {
public:
    explicit baseline_store(std::filesystem::path project_configuration_path)
        : configuration_path(std::move(project_configuration_path)) {}

    baseline_store(const baseline_store&) = delete;
    baseline_store& operator=(const baseline_store&) = delete;

    // Reads CURRENT + manifest only. No baseline artifact is mapped.
    [[nodiscard]] status probe(
        baseline_probe& output) const noexcept;

    // BUILD fast path: reads CURRENT + manifest once, returns the
    // persisted configuration identity, and maps compiled + Source Manager
    // from that same immutable transaction. build_cache stays deferred.
    [[nodiscard]] status open_current_decision(
        baseline_probe& probe,
        baseline_snapshot& output,
        baseline_open_telemetry* telemetry = nullptr) const noexcept;


    // BUILD/SAVE boundary: maps all three artifacts.
    [[nodiscard]] status open(
        const baseline_fingerprint& expected,
        baseline_snapshot& output) const noexcept;

    // Fast LOAD boundary: maps compiled.bin and source_manager.bin only.
    // build_cache.bin is not opened and therefore cannot fault on READY LOAD.
    [[nodiscard]] status open_ready(
        const baseline_fingerprint& expected,
        baseline_snapshot& output) const noexcept;

    // Opens one immutable transaction by name. SAVE-after-LOAD uses this to copy
    // the active baseline without rebinding the READY Project to CURRENT.
    [[nodiscard]] status open_transaction(
        const baseline_fingerprint& expected,
        std::string_view transaction,
        baseline_snapshot& output,
        baseline_open_telemetry* telemetry = nullptr) const noexcept;

    // Opens only compiled + Source Manager. build_cache stays unmapped until
    // dirty detection proves that a sparse construction overlay is required.
    [[nodiscard]] status open_transaction_ready(
        const baseline_fingerprint& expected,
        std::string_view transaction,
        baseline_snapshot& output,
        baseline_open_telemetry* telemetry = nullptr) const noexcept;

    [[nodiscard]] status open_transaction_decision(
        const baseline_fingerprint& expected,
        std::string_view transaction,
        baseline_snapshot& output,
        baseline_open_telemetry* telemetry = nullptr) const noexcept;

    [[nodiscard]] status map_source_manager(
        const baseline_fingerprint& expected,
        std::string_view transaction,
        baseline_snapshot& snapshot,
        baseline_open_telemetry* telemetry = nullptr) const noexcept;

    [[nodiscard]] status map_source_manager_cached(
        const baseline_fingerprint& expected,
        std::string_view transaction,
        std::uint64_t expected_source_manager_size,
        baseline_snapshot& snapshot,
        baseline_open_telemetry* telemetry = nullptr) const noexcept;


    [[nodiscard]] status map_build_cache(
        const baseline_fingerprint& expected,
        std::string_view transaction,
        baseline_snapshot& snapshot,
        baseline_open_telemetry* telemetry = nullptr) const noexcept;

    // Fused BUILD path: manifest identity and artifact size were already
    // validated while opening CURRENT. This maps only the immutable cache file.
    [[nodiscard]] status map_build_cache_cached(
        const baseline_fingerprint& expected,
        std::string_view transaction,
        std::uint64_t expected_build_cache_size,
        baseline_snapshot& snapshot,
        baseline_open_telemetry* telemetry = nullptr) const noexcept;

    [[nodiscard]] status commit(
        const baseline_fingerprint& fingerprint,
        const baseline_configuration_state& configuration,
        std::span<const std::byte> compiled,
        std::span<const std::byte> source_manager,
        std::span<const std::byte> change_state,
        std::span<const std::byte> build_cache,
        baseline_commit_result& output) const noexcept;

    [[nodiscard]] status commit(
        const baseline_fingerprint& fingerprint,
        const baseline_configuration_state& configuration,
        std::span<const std::byte> compiled,
        std::span<const std::byte> source_manager,
        std::span<const std::byte> build_cache,
        baseline_commit_result& output) const noexcept;

    // Compatibility boundary for low-level persistence callers. This overload
    // intentionally commits without a fast project.json identity.
    [[nodiscard]] status commit(
        const baseline_fingerprint& fingerprint,
        std::span<const std::byte> compiled,
        std::span<const std::byte> source_manager,
        std::span<const std::byte> build_cache,
        baseline_commit_result& output) const noexcept;

    // Cold maintenance boundary. The current transaction and the optional pinned
    // transaction are retained; all other tx-* directories are reclaimable.
    [[nodiscard]] status collect_garbage(
        std::string_view pinned_transaction = {}) const noexcept;

private:
    [[nodiscard]] status open_selected(
        const baseline_fingerprint& expected,
        std::string_view transaction,
        bool include_source_manager,
        bool include_change_state,
        bool include_build_cache,
        baseline_snapshot& output,
        baseline_open_telemetry* telemetry) const noexcept;

    [[nodiscard]] std::filesystem::path root_path() const;

    std::filesystem::path configuration_path;
};

} // namespace cw::server
