#pragma once

#include "../../source_id.hpp"
#include "../../status.hpp"
#include "../builder/source_contribution.hpp"
#include "../graph/graph.hpp"
#include "../project_generation_segments.hpp"
#include "../parser/source_environment.hpp"
#include "../source/source_change_tracker.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace cw::server {

class compiled_image_view;
class project_context;
class source_manager;
class source_manager_image_view;

inline constexpr std::uint32_t build_cache_image_format_version = 4;
inline constexpr std::size_t build_cache_image_header_size = 256;
inline constexpr std::size_t build_cache_image_directory_count = 25;
inline constexpr std::size_t build_cache_image_directory_entry_size = 32;
inline constexpr std::size_t build_cache_image_prefix_size =
    (build_cache_image_header_size +
     build_cache_image_directory_count *
         build_cache_image_directory_entry_size +
     63u) &
    ~std::size_t{63u};

enum class build_cache_image_section : std::uint32_t {
    source_directory = 1,
    source_bytes = 2,
    frontend_local_types = 3,
    frontend_type_slots = 4,
    frontend_object_slots = 5,
    frontend_member_slots = 6,
    contribution_states = 7,
    contribution_types = 8,
    contribution_members = 9,
    contribution_modifiers = 10,
    contribution_enum_values = 11,
    contribution_objects = 12,
    contribution_links = 13,
    construction_states = 14,
    graph_intrinsic_refs = 15,
    graph_named_refs = 16,
    graph_derived_index = 17,
    graph_dependency_versions = 18,
    graph_reverse_dependency_heads = 19,
    graph_dependency_edges = 20,
    graph_type_identity_index = 21,
    graph_object_identity_index = 22,
    graph_link_target_index = 23,
    source_file_identity_index = 24,
    tracked_directory_identity_index = 25,
};

struct build_cache_range final {
    std::uint32_t begin = 0;
    std::uint32_t count = 0;
};

struct build_cache_source_record final {
    source_id source{};
    bool snapshot_present = false;
    bool frontend_present = false;
    std::uint64_t text_offset = 0;
    std::uint32_t text_length = 0;
    build_cache_range local_types{};
    build_cache_range type_slots{};
    build_cache_range object_slots{};
    build_cache_range member_slots{};
};

struct build_cache_derived_index_slot final {
    std::uint32_t fingerprint = 0;
    TypeRef type{};
};

enum class build_cache_baseline_section_identity : std::uint8_t {
    none = 0,
    exact = 1,
};

// Encoder-owned proof state for logical Build Cache sections. exact means the
// complete encoded section is byte-identical to the corresponding whole
// section in the pinned baseline. Persistence may translate only exact state
// into a baseline-owned section provenance capability.
struct build_cache_encode_provenance final {
    std::array<
        build_cache_baseline_section_identity,
        build_cache_image_directory_count>
        sections{};

    [[nodiscard]] bool exact(std::size_t index) const noexcept {
        return index < sections.size() &&
            sections[index] ==
                build_cache_baseline_section_identity::exact;
    }
};

struct build_cache_encode_telemetry final {
    std::uint64_t total_ns = 0;
    std::uint64_t layout_allocate_ns = 0;
    std::uint64_t source_frontend_ns = 0;
    std::uint64_t source_directory_text_ns = 0;
    std::uint64_t frontend_record_ranges_ns = 0;
    std::uint64_t frontend_local_types_ns = 0;
    std::uint64_t frontend_type_slots_ns = 0;
    std::uint64_t frontend_object_slots_ns = 0;
    std::uint64_t frontend_member_slots_ns = 0;

    // Stratified SAVE profiling. Only every 64th Source is timed so telemetry
    // does not materially perturb the hot 100k+ Source serialization loop.
    std::uint64_t source_frontend_total_sources = 0;
    std::uint64_t source_frontend_sampled_sources = 0;
    std::uint64_t source_lookup_sample_ns = 0;
    std::uint64_t source_text_copy_sample_ns = 0;
    std::uint64_t frontend_record_sample_ns = 0;
    std::uint64_t frontend_local_types_sample_ns = 0;
    std::uint64_t frontend_type_slots_sample_ns = 0;
    std::uint64_t frontend_object_slots_sample_ns = 0;
    std::uint64_t frontend_member_slots_sample_ns = 0;

    std::uint64_t contribution_ns = 0;
    std::uint64_t graph_ns = 0;
    std::uint64_t change_identity_ns = 0;
    std::uint64_t section_crc_ns = 0;
    std::uint64_t header_directory_ns = 0;
    std::uint64_t bind_ns = 0;
    std::uint64_t verify_ns = 0;

    std::uint64_t mapped_baseline_bulk_bytes = 0;
    std::uint64_t mapped_baseline_borrowed_bytes = 0;
    std::uint64_t mapped_baseline_sparse_borrowed_bytes = 0;
    std::uint64_t mapped_baseline_sparse_directory_borrowed_bytes = 0;
    std::uint64_t mapped_baseline_patch_records = 0;
    std::uint64_t mapped_baseline_append_records = 0;
    std::uint32_t mapped_baseline_bulk_sections = 0;
    std::uint32_t mapped_baseline_borrowed_sections = 0;
    std::uint32_t mapped_baseline_sparse_borrowed_extents = 0;
    std::uint32_t mapped_baseline_sparse_directory_borrowed_extents = 0;
};

struct build_cache_encode_borrowed_sections final {
    std::array<
        std::span<const std::byte>,
        build_cache_image_directory_count> sections{};

    [[nodiscard]] bool any() const noexcept {
        for (const auto value : sections) {
            if (!value.empty())
                return true;
        }
        return false;
    }
};

// D4Q2C2A: partial logical sections may borrow immutable baseline runs while
// dirty ranges remain owned by the frozen Generation. Extent count is bounded
// by project_generation_segment_max_extents and failure falls back to the
// canonical contiguous encoder.
struct build_cache_encode_sparse_sections final {
    std::array<
        project_generation_segment,
        build_cache_image_directory_count> sections{};

    [[nodiscard]] bool any() const noexcept {
        for (const auto& value : sections) {
            if (!value.empty())
                return true;
        }
        return false;
    }
};


// Mmap-native BUILD-only baseline. The view contains Source bytes, Parser-local
// interface tables, SourceContribution append arenas, and Builder lineage caches.
// It contains no Runtime/READY ownership and no process pointers.
class build_cache_image_view final {
public:
    build_cache_image_view() noexcept = default;

    [[nodiscard]] status bind(std::span<const std::byte> image) noexcept;

    // Binds the same logical Build Cache v4 image from an immutable sectioned
    // physical store. The prefix contains the canonical header + directory;
    // each section span contains only that section's logical bytes.
    [[nodiscard]] status bind_sectioned(
        std::span<const std::byte> prefix,
        const std::array<
            std::span<const std::byte>,
            build_cache_image_directory_count>& section_images) noexcept;

    [[nodiscard]] status bind_encoded_mixed(
        std::span<const std::byte> layout_backing,
        const build_cache_encode_borrowed_sections& borrowed) noexcept;

    [[nodiscard]] status bind_encoded_sparse(
        std::span<const std::byte> layout_backing,
        const build_cache_encode_borrowed_sections& borrowed,
        const build_cache_encode_sparse_sections& sparse) noexcept;

    void reset() noexcept;

    [[nodiscard]] bool valid() const noexcept {
        return bytes.data() != nullptr;
    }

    [[nodiscard]] bool frontend_complete() const noexcept {
        return frontend_complete_value;
    }

    [[nodiscard]] bool contributions_complete() const noexcept {
        return contributions_complete_value;
    }

    [[nodiscard]] std::size_t source_count() const noexcept {
        return source_count_value;
    }

    [[nodiscard]] std::size_t frontend_count() const noexcept {
        return frontend_count_value;
    }

    [[nodiscard]] std::span<const std::byte> section_bytes(
        build_cache_image_section kind) const noexcept;

    // O(1) persistence sizing metadata. These counts are validated while the
    // mmap image is bound and seed sparse Generation summaries without scans.
    [[nodiscard]] std::uint64_t source_bytes_count() const noexcept {
        return section(
            build_cache_image_section::source_bytes).count;
    }

    [[nodiscard]] std::size_t frontend_local_type_count() const noexcept {
        return static_cast<std::size_t>(
            section(
                build_cache_image_section::frontend_local_types).count);
    }

    [[nodiscard]] std::size_t frontend_type_slot_count() const noexcept {
        return static_cast<std::size_t>(
            section(
                build_cache_image_section::frontend_type_slots).count);
    }

    [[nodiscard]] std::size_t frontend_object_slot_count() const noexcept {
        return static_cast<std::size_t>(
            section(
                build_cache_image_section::frontend_object_slots).count);
    }

    [[nodiscard]] std::size_t frontend_member_slot_count() const noexcept {
        return static_cast<std::size_t>(
            section(
                build_cache_image_section::frontend_member_slots).count);
    }

    [[nodiscard]] std::size_t derived_index_entries() const noexcept {
        return derived_index_entries_value;
    }

    [[nodiscard]] const source_contribution_statistics&
    contribution_statistics() const noexcept {
        return contribution_statistics_value;
    }

    [[nodiscard]] status source(
        source_id id,
        build_cache_source_record& output) const noexcept;

    [[nodiscard]] std::string_view source_text(source_id id) const noexcept;

    [[nodiscard]] source_change_checkpoint change_checkpoint() const noexcept {
        return change_checkpoint_value;
    }

    [[nodiscard]] source_id find_source_file(
        std::uint64_t file_reference) const noexcept;

    [[nodiscard]] std::uint32_t directory_watch_flags(
        std::uint64_t file_reference) const noexcept;

    [[nodiscard]] status frontend_local_type(
        source_id source,
        std::size_t index,
        identity_ref& output) const noexcept;

    [[nodiscard]] status frontend_type_slot(
        source_id source,
        std::size_t index,
        source_interface_type_slot& output) const noexcept;

    [[nodiscard]] status frontend_object_slot(
        source_id source,
        std::size_t index,
        source_interface_object_slot& output) const noexcept;

    [[nodiscard]] status frontend_member_slot(
        source_id source,
        std::size_t index,
        source_interface_member_slot& output) const noexcept;

    [[nodiscard]] status contribution_state(
        source_id source,
        source_contribution_state& output) const noexcept;

    [[nodiscard]] std::size_t contribution_type_count() const noexcept;
    [[nodiscard]] std::size_t contribution_member_count() const noexcept;
    [[nodiscard]] std::size_t contribution_modifier_count() const noexcept;
    [[nodiscard]] std::size_t contribution_enum_value_count() const noexcept;
    [[nodiscard]] std::size_t contribution_object_count() const noexcept;
    [[nodiscard]] std::size_t contribution_link_count() const noexcept;
    [[nodiscard]] std::size_t construction_slot_count() const noexcept;

    [[nodiscard]] status contribution_type(
        std::size_t index,
        source_contribution_type& output) const noexcept;

    [[nodiscard]] status contribution_member(
        std::size_t index,
        source_contribution_member& output) const noexcept;

    [[nodiscard]] status contribution_modifier(
        std::size_t index,
        source_type_modifier& output) const noexcept;

    [[nodiscard]] status contribution_enum_value(
        std::size_t index,
        source_contribution_enum_value& output) const noexcept;

    [[nodiscard]] status contribution_object(
        std::size_t index,
        source_contribution_object& output) const noexcept;

    [[nodiscard]] status contribution_link(
        std::size_t index,
        source_contribution_link& output) const noexcept;

    [[nodiscard]] status construction(
        type_handle handle,
        source_construction_state& output) const noexcept;

    [[nodiscard]] status construction_at_slot(
        std::size_t index,
        source_construction_state& output) const noexcept;

    [[nodiscard]] TypeRef intrinsic_ref(intrinsic_type type) const noexcept;
    [[nodiscard]] TypeRef named_ref(type_handle handle) const noexcept;

    [[nodiscard]] std::size_t derived_index_slot_count() const noexcept;
    [[nodiscard]] status derived_index_slot(
        std::size_t index,
        build_cache_derived_index_slot& output) const noexcept;

    [[nodiscard]] std::size_t dependency_version_count() const noexcept;
    [[nodiscard]] std::uint32_t dependency_version(
        type_handle handle) const noexcept;

    [[nodiscard]] std::uint32_t reverse_dependency_head(
        type_handle handle) const noexcept;

    [[nodiscard]] std::size_t dependency_edge_count() const noexcept;
    [[nodiscard]] status dependency_edge(
        std::size_t index,
        graph_dependency_edge& output) const noexcept;

    // BUILD-only historical indexes include tombstones; collision candidates
    // are verified against compiled.bin before a handle is accepted.
    [[nodiscard]] type_handle find_type_identity(
        identity_ref identity,
        const compiled_image_view& compiled) const noexcept;

    [[nodiscard]] object_handle find_object_identity(
        identity_ref identity,
        const compiled_image_view& compiled) const noexcept;

    [[nodiscard]] link_handle find_link_target(
        object_endpoint target,
        const compiled_image_view& compiled) const noexcept;

    [[nodiscard]] std::size_t type_identity_index_slot_count() const noexcept;
    [[nodiscard]] status type_identity_index_slot(
        std::size_t index,
        graph_identity_index_slot& output) const noexcept;

    [[nodiscard]] std::size_t object_identity_index_slot_count() const noexcept;
    [[nodiscard]] status object_identity_index_slot(
        std::size_t index,
        graph_object_identity_index_slot& output) const noexcept;

    [[nodiscard]] std::size_t link_target_index_slot_count() const noexcept;
    [[nodiscard]] status link_target_index_slot(
        std::size_t index,
        graph_link_index_slot& output) const noexcept;

    [[nodiscard]] std::size_t named_ref_count() const noexcept;

    // Cold full-image audit. This validates section CRCs and all internal ranges,
    // sentinels, hash-table occupancy, dependency chains, and live statistics.
    [[nodiscard]] status verify_contents(
        bool verify_section_crc = true) const noexcept;

    // Cross-artifact audit used by SAVE/maintenance tests. Fast LOAD does not
    // require this pass.
    [[nodiscard]] status verify_against(
        const compiled_image_view& compiled,
        const source_manager_image_view& sources) const noexcept;

    // Fresh dense Generation audit. This preserves the SAVE cross-artifact gate
    // when source_manager.bin is exposed as native scatter/gather extents.
    [[nodiscard]] status verify_against(
        const compiled_image_view& compiled,
        const source_manager& sources) const noexcept;

    // Fresh SAVE-only audit. encode_build_cache_image() has already proven that
    // every immutable Source snapshot hash matches the native Generation
    // physical hash before copying Source bytes into this image. This audit
    // therefore validates the copied ranges and all cross-artifact references
    // without recomputing Source content hashes.
    [[nodiscard]] status verify_against_encoded_generation(
        const compiled_image_view& compiled,
        const source_manager& sources,
        const graph& committed_graph) const noexcept;

    // D4D4_SPARSE_GENERATION_VERIFY
    // Baseline-backed sparse Generation audit. Source physical state is read
    // through the logical Source Manager, while historical Build Cache indexes
    // retain the cold lookup-by-lookup verification against compiled.bin.
    [[nodiscard]] status verify_against_sparse_generation(
        const compiled_image_view& compiled,
        const source_manager& sources) const noexcept;

private:
    struct source_validation_access;

    [[nodiscard]] status verify_against_impl(
        const compiled_image_view& compiled,
        const source_validation_access& sources) const noexcept;

    struct section_view final {
        const std::byte* data = nullptr;
        std::uint64_t count = 0;
        std::uint32_t record_size = 0;
        std::uint64_t crc64 = 0;
        project_generation_segment segment{};
    };

    [[nodiscard]] const section_view& section(
        build_cache_image_section kind) const noexcept;

    [[nodiscard]] bool valid_source(source_id source) const noexcept {
        return source &&
            static_cast<std::size_t>(source.value()) <= source_count_value;
    }

    [[nodiscard]] string_id string_from_raw(std::uint32_t value) const noexcept;
    [[nodiscard]] identity_ref identity_from_raw(std::uint32_t value) const noexcept;
    [[nodiscard]] TypeRef type_ref_from_raw(std::uint32_t value) const noexcept;

    std::span<const std::byte> bytes;
    section_view sections[build_cache_image_directory_count]{};
    source_contribution_statistics contribution_statistics_value{};
    std::size_t source_count_value = 0;
    std::size_t frontend_count_value = 0;
    std::size_t derived_index_entries_value = 0;
    source_change_checkpoint change_checkpoint_value{};
    bool frontend_complete_value = false;
    bool contributions_complete_value = false;
};

// Storage-neutral physical-section view of one immutable Build Cache
// Generation. The prefix and each logical section are independent carriers;
// one section may itself contain multiple scatter/gather extents. This matches
// the durable sectioned artifact and removes the architectural requirement for
// one monolithic Build Cache buffer.
//
// The class owns no bytes. Every extent must remain immutable and alive for the
// lifetime of this view.
class build_cache_generation_segments final {
public:
    build_cache_generation_segments() noexcept = default;

    void reset() noexcept;

    // Convenience conversion for the current contiguous encoder. No bytes are
    // copied: every section extent points directly into image.
    [[nodiscard]] status bind(
        std::span<const std::byte> image) noexcept;

    // O(section-count) conversion after build_cache_image_view::bind(image)
    // already established the contiguous image contract. This avoids a second
    // complete bind in the SAVE path while still proves that every exposed
    // section span belongs to the exact supplied image.
    [[nodiscard]] status bind_validated_contiguous(
        std::span<const std::byte> image,
        const build_cache_image_view& validated) noexcept;

    // Native sparse entry point. Prefix geometry is validated against the
    // logical size and each section carrier, but section bytes are not
    // materialized or concatenated.
    [[nodiscard]] status bind_sectioned(
        std::span<const std::byte> prefix,
        const std::array<
            project_generation_segment,
            build_cache_image_directory_count>& sections,
        std::size_t logical_size) noexcept;

    [[nodiscard]] status bind_view(
        build_cache_image_view& output) const noexcept;

    [[nodiscard]] status bind_validated_sections(
        std::span<const std::byte> layout_backing,
        const build_cache_image_view& validated,
        const build_cache_encode_sparse_sections* sparse = nullptr) noexcept;

    [[nodiscard]] project_generation_segment logical_segment(
        std::span<const std::byte> layout_backing) const noexcept;

    [[nodiscard]] bool valid() const noexcept {
        return !prefix_value.empty();
    }

    [[nodiscard]] std::span<const std::byte>
    prefix() const noexcept {
        return prefix_value;
    }

    [[nodiscard]] const project_generation_segment&
    section(build_cache_image_section kind) const noexcept;

    [[nodiscard]] std::size_t logical_size() const noexcept {
        return logical_size_value;
    }

    // Physical bytes exclude canonical zero alignment gaps between section
    // files. They are prefix + exact section payload bytes.
    [[nodiscard]] std::size_t physical_size() const noexcept {
        return physical_size_value;
    }

    [[nodiscard]] std::size_t physical_extent_count() const noexcept {
        return physical_extent_count_value;
    }

private:
    std::span<const std::byte> prefix_value;
    std::array<
        project_generation_segment,
        build_cache_image_directory_count> sections_value{};
    std::array<
        std::uint64_t,
        build_cache_image_directory_count> section_offsets{};
    std::size_t logical_size_value = 0;
    std::size_t physical_size_value = 0;
    std::size_t physical_extent_count_value = 0;
};

// Deterministic field-wise little-endian staging encoder for the persisted
// Build Cache v4 image.
// It walks dense source_id and existing append-arena order only; no sort or
// runtime hash-table traversal is used.
[[nodiscard]] status encode_build_cache_image(
    const project_context& project,
    std::vector<std::byte>& output) noexcept;

[[nodiscard]] status encode_build_cache_image(
    const project_context& project,
    const source_change_capture& change_capture,
    std::vector<std::byte>& output) noexcept;

[[nodiscard]] status encode_build_cache_image(
    const project_context& project,
    const source_change_capture& change_capture,
    std::vector<std::byte>& output,
    build_cache_encode_telemetry* telemetry,
    build_cache_encode_provenance* provenance = nullptr,
    build_cache_encode_borrowed_sections* borrowed = nullptr,
    build_cache_encode_sparse_sections* sparse = nullptr) noexcept;

} // namespace cw::server
