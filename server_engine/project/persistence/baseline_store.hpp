#pragma once

#include "../../status.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace cw::server {

inline constexpr std::uint32_t baseline_format_version = 1;
inline constexpr std::size_t baseline_fingerprint_size = 32;

struct baseline_fingerprint final {
    std::array<std::uint8_t, baseline_fingerprint_size> bytes{};

    friend constexpr bool operator==(
        const baseline_fingerprint&, const baseline_fingerprint&) noexcept = default;
};

enum class baseline_artifact_kind : std::uint8_t {
    compiled,
    source_manager,
    build_cache,
};

struct baseline_commit_result final {
    std::string transaction;
    std::uint64_t bytes_written = 0;
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

// One committed persisted baseline. It owns the three mappings as one lifetime
// unit so compiled/source/build-cache artifacts cannot be mixed across commits.
class baseline_snapshot final {
public:
    baseline_snapshot() noexcept = default;

    baseline_snapshot(const baseline_snapshot&) = delete;
    baseline_snapshot& operator=(const baseline_snapshot&) = delete;
    baseline_snapshot(baseline_snapshot&&) noexcept = default;
    baseline_snapshot& operator=(baseline_snapshot&&) noexcept = default;

    [[nodiscard]] std::span<const std::byte> artifact(
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
    read_only_file_mapping build_cache;

    friend class baseline_store;
};

// Transactional persistent-baseline storage. Artifact directories are immutable;
// CURRENT is the only selector changed during commit. LOAD maps artifacts read-only.
class baseline_store final {
public:
    explicit baseline_store(std::filesystem::path project_configuration_path)
        : configuration_path(std::move(project_configuration_path)) {}

    baseline_store(const baseline_store&) = delete;
    baseline_store& operator=(const baseline_store&) = delete;

    [[nodiscard]] status open(
        const baseline_fingerprint& expected,
        baseline_snapshot& output) const noexcept;

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
    [[nodiscard]] std::filesystem::path root_path() const;

    std::filesystem::path configuration_path;
};

} // namespace cw::server
