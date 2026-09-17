#include "project_context.hpp"
#include "project_persistence.hpp"

#include <chrono>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace cw::server {

project_context::project_context(
    project_configuration configuration,
    std::filesystem::path configuration_path)
    : project_configuration_value(std::move(configuration)),
      project_configuration_path(std::move(configuration_path)),
      compiled(std::make_unique<compiled_project_state>()) {}

project_context::project_context(
    project_configuration configuration,
    std::filesystem::path configuration_path,
    baseline_storage_tag) noexcept
    : project_configuration_value(std::move(configuration)),
      project_configuration_path(std::move(configuration_path)) {}

project_context::~project_context() noexcept = default;

status project_context::
release_frontend_generation_storage(
    project_generation_storage& output) noexcept {

    if (compiled == nullptr)
        return {status_code::invalid_state};

    source_frontend_block_store storage;
    auto result =
        compiled->frontend_cache.
            release_native_frontend_block_storage(
                storage);

    if (!result.ok())
        return result;

    return output.adopt_frontend_generation_storage(
        std::move(storage));
}


status project_context::
release_contribution_generation_storage(
    project_generation_storage& output) noexcept {

    if (compiled == nullptr)
        return {status_code::invalid_state};

    source_contribution_generation_storage storage;
    auto result =
        compiled->contributions.
            release_native_generation_storage(
                storage);

    if (!result.ok())
        return result;

    return output.adopt_contribution_generation_storage(
        std::move(storage));
}


status project_context::
release_snapshot_generation_storage(
    project_generation_storage& output) noexcept {

    if (compiled == nullptr)
        return {status_code::invalid_state};

    source_snapshot_generation_storage storage;
    auto result =
        compiled->sources.
            release_snapshot_generation_storage(
                storage);

    if (!result.ok())
        return result;

    return output.adopt_snapshot_generation_storage(
        std::move(storage));
}


status project_context::
prepare_generation_build_cache_source_directory() noexcept {

    if (compiled == nullptr)
        return {status_code::invalid_state};

    const auto source_count =
        compiled->sources.source_count();

    const auto native_sources =
        compiled->sources.native_generation();

    const auto* frontend_storage =
        compiled->frontend_cache.
            native_frontend_block_bulk_storage();

    if (source_count == 0 ||
        !native_sources.complete ||
        native_sources.physical.size() != source_count ||
        frontend_storage == nullptr ||
        !compiled->sources.native_snapshot_text_complete()) {
        generation_native_segments_value.
            clear_build_cache_source_directory();
        return {status_code::invalid_state};
    }

    if (source_count >
        static_cast<std::size_t>(
            (std::numeric_limits<std::uint32_t>::max)())) {
        return {status_code::not_available};
    }

    try {
        std::vector<
            project_generation_build_cache_source_directory_record>
                directory(source_count);

        std::uint64_t text_cursor = 0;
        std::uint32_t local_type_cursor = 0;
        std::uint32_t type_slot_cursor = 0;
        std::uint32_t object_slot_cursor = 0;
        std::uint32_t member_slot_cursor = 0;
        std::size_t frontend_count = 0;

        for (std::size_t index = 0;
             index < source_count;
             ++index) {

            const source_id source{
                static_cast<std::uint32_t>(
                    index + 1)};

            auto& output = directory[index];
            output.source = source;

            const auto& physical =
                native_sources.physical[index];

            if (physical.reserved != 0 ||
                (physical.flags &
                    ~source_generation_physical_present) != 0) {
                return {status_code::initialization_failed};
            }

            if (physical.present()) {
                if (physical.size >
                        (std::numeric_limits<std::uint32_t>::max)() ||
                    text_cursor >
                        (std::numeric_limits<std::uint64_t>::max)() -
                            physical.size) {
                    return {status_code::not_available};
                }

                output.flags |=
                    project_generation_build_cache_source_directory_record::
                        snapshot_present;

                output.text_offset = text_cursor;
                output.text_length =
                    static_cast<std::uint32_t>(
                        physical.size);

                text_cursor += physical.size;
            }

            source_frontend_block_ref block;
            auto result =
                compiled->frontend_cache.
                    native_frontend_block(
                        source,
                        block);

            if (!result.ok()) {
                if (result.code == status_code::not_found)
                    continue;
                return result;
            }

            source_frontend_block_layout layout;
            result = frontend_storage->layout(
                block,
                layout);

            if (!result.ok() ||
                layout.source != source ||
                layout.local_types.begin !=
                    local_type_cursor ||
                layout.type_slots.begin !=
                    type_slot_cursor ||
                layout.object_slots.begin !=
                    object_slot_cursor ||
                layout.member_slots.begin !=
                    member_slot_cursor) {
                return {status_code::initialization_failed};
            }

            output.flags |=
                project_generation_build_cache_source_directory_record::
                    frontend_present;

            output.local_types_begin =
                layout.local_types.begin;
            output.local_types_count =
                layout.local_types.count;
            output.type_slots_begin =
                layout.type_slots.begin;
            output.type_slots_count =
                layout.type_slots.count;
            output.object_slots_begin =
                layout.object_slots.begin;
            output.object_slots_count =
                layout.object_slots.count;
            output.member_slots_begin =
                layout.member_slots.begin;
            output.member_slots_count =
                layout.member_slots.count;

            if (layout.local_types.count >
                    (std::numeric_limits<std::uint32_t>::max)() -
                        local_type_cursor ||
                layout.type_slots.count >
                    (std::numeric_limits<std::uint32_t>::max)() -
                        type_slot_cursor ||
                layout.object_slots.count >
                    (std::numeric_limits<std::uint32_t>::max)() -
                        object_slot_cursor ||
                layout.member_slots.count >
                    (std::numeric_limits<std::uint32_t>::max)() -
                        member_slot_cursor) {
                return {status_code::not_available};
            }

            local_type_cursor +=
                layout.local_types.count;
            type_slot_cursor +=
                layout.type_slots.count;
            object_slot_cursor +=
                layout.object_slots.count;
            member_slot_cursor +=
                layout.member_slots.count;
            ++frontend_count;
        }

        const auto& summary =
            compiled->frontend_cache.persistence_summary();

        if (text_cursor !=
                compiled->sources.persistence_text_bytes() ||
            frontend_count != summary.frontend_count ||
            static_cast<std::size_t>(
                local_type_cursor) != summary.local_types ||
            static_cast<std::size_t>(
                type_slot_cursor) != summary.type_slots ||
            static_cast<std::size_t>(
                object_slot_cursor) != summary.object_slots ||
            static_cast<std::size_t>(
                member_slot_cursor) != summary.member_slots) {
            return {status_code::initialization_failed};
        }

        generation_native_segments_value.
            publish_build_cache_source_directory(
                std::move(directory));

        return {};
    }
    catch (const std::bad_alloc&) {
        generation_native_segments_value.
            clear_build_cache_source_directory();
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        generation_native_segments_value.
            clear_build_cache_source_directory();
        return {status_code::not_available};
    }
}

status project_context::
release_build_cache_source_directory_generation_storage(
    project_generation_storage& output) noexcept {

    auto directory =
        generation_native_segments_value.
            release_build_cache_source_directory();

    if (directory.empty())
        return {status_code::invalid_state};

    return output.adopt_build_cache_source_directory_generation_storage(
        std::move(directory));
}


status project_context::activate_ready_generation(
    project_generation_storage&& storage,
    project_ready_generation_activation_telemetry* telemetry) noexcept {

    if (telemetry != nullptr)
        *telemetry = {};

    const auto elapsed_ns =
        [](std::chrono::steady_clock::time_point begin) noexcept {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<
                    std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() -
                        begin).count());
        };

    try {
        const auto owner_begin =
            std::chrono::steady_clock::now();

        auto owner =
            std::make_unique<project_generation_storage>(
                std::move(storage));

        if (telemetry != nullptr)
            telemetry->owner_ns = elapsed_ns(owner_begin);

        const auto segments_begin =
            std::chrono::steady_clock::now();

        const auto segments = owner->segments();
        const auto source_bytes =
            segments.sources();
        const auto build_bytes =
            segments.build();

        if (telemetry != nullptr)
            telemetry->segments_ns = elapsed_ns(segments_begin);

        if (segments.compiled_segment().empty() ||
            source_bytes.empty() ||
            segments.build_segment().empty()) {
            return {status_code::initialization_failed};
        }

        compiled_image_view compiled_view;

        const auto bind_compiled_begin =
            std::chrono::steady_clock::now();

        auto result =
            owner->bind_compiled(compiled_view);

        if (telemetry != nullptr) {
            telemetry->bind_compiled_ns =
                elapsed_ns(bind_compiled_begin);
        }

        if (!result.ok())
            return result;

        source_manager_image_view source_view;

        const auto bind_sources_begin =
            std::chrono::steady_clock::now();

        result = source_view.bind(source_bytes);

        if (telemetry != nullptr) {
            telemetry->bind_sources_ns =
                elapsed_ns(bind_sources_begin);
        }

        if (!result.ok())
            return result;

        build_cache_image_view build_view;

        const auto bind_build_begin =
            std::chrono::steady_clock::now();

        if (owner->build_cache_sections().valid()) {
            result =
                owner->build_cache_sections().
                    bind_view(build_view);
        }
        else {
            result = build_view.bind(build_bytes);
        }

        if (telemetry != nullptr) {
            telemetry->bind_build_ns =
                elapsed_ns(bind_build_begin);
        }

        if (!result.ok())
            return result;

        const auto verify_begin =
            std::chrono::steady_clock::now();

        result =
            build_view.verify_against(
                compiled_view,
                source_view);

        if (telemetry != nullptr)
            telemetry->verify_ns = elapsed_ns(verify_begin);

        if (!result.ok())
            return result;

        const auto publish_begin =
            std::chrono::steady_clock::now();

        mapped_compiled = compiled_view;
        mapped_sources = source_view;
        mapped_build_cache = build_view;
        finalized_generation = std::move(owner);

        if (telemetry != nullptr)
            telemetry->publish_ns = elapsed_ns(publish_begin);

        compiled_project_state_teardown_telemetry
            compiled_teardown_detail;

        if (compiled != nullptr) {
            compiled->begin_teardown_audit(
                compiled_teardown_detail);
        }

        const auto compiled_destroy_begin =
            std::chrono::steady_clock::now();

        compiled.reset();

        if (telemetry != nullptr) {
            telemetry->compiled_destroy_ns =
                elapsed_ns(compiled_destroy_begin);
            telemetry->compiled_teardown_graph_ns =
                compiled_teardown_detail.graph_ns;
            telemetry->compiled_teardown_contributions_ns =
                compiled_teardown_detail.contributions_ns;
            telemetry->compiled_teardown_frontend_cache_ns =
                compiled_teardown_detail.frontend_cache_ns;
            telemetry->compiled_teardown_source_manager_ns =
                compiled_teardown_detail.source_manager_ns;
            telemetry->compiled_teardown_identities_ns =
                compiled_teardown_detail.identities_ns;
        }

        const auto baseline_destroy_begin =
            std::chrono::steady_clock::now();

        baseline.reset();

        if (telemetry != nullptr) {
            telemetry->baseline_destroy_ns =
                elapsed_ns(baseline_destroy_begin);
        }

        const auto cleanup_begin =
            std::chrono::steady_clock::now();

        generation_provenance_value.clear_source_change();
        generation_provenance_value.clear_roots();
        clear_generation_change_segment();

        source_mapping_status = {};
        source_mapping_attempted = true;
        source_mapping_ready.store(
            true,
            std::memory_order_release);

        if (telemetry != nullptr)
            telemetry->cleanup_ns = elapsed_ns(cleanup_begin);

        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}


status project_context::activate_ready_baseline(
    baseline_snapshot&& snapshot) noexcept {

    if (!snapshot.valid() ||
        snapshot.artifact(
            baseline_artifact_kind::compiled).empty()) {
        return {status_code::artifact_corrupt};
    }

    compiled_image_view compiled_view;
    auto result = compiled_view.bind(
        snapshot.artifact(
            baseline_artifact_kind::compiled));
    if (!result.ok())
        return result;

    source_manager_image_view source_view;
    if (snapshot.mapped(
            baseline_artifact_kind::source_manager)) {
        result =
            snapshot.bind_source_manager(
                source_view);
        if (!result.ok())
            return result;
    }

    try {
        auto owner =
            std::make_unique<baseline_snapshot>(std::move(snapshot));

        mapped_compiled = compiled_view;
        mapped_sources = source_view;
        set_build_fingerprint(owner->fingerprint());
        baseline = std::move(owner);
        compiled.reset();

        if (mapped_sources.valid()) {
            source_mapping_status = {};
            source_mapping_attempted = true;
            source_mapping_ready.store(
                true,
                std::memory_order_release);
        }

        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
}

status project_context::activate_build_baseline(
    baseline_snapshot&& snapshot) noexcept {

    if (!snapshot.valid() ||
        snapshot.artifact(baseline_artifact_kind::compiled).empty() ||
        !snapshot.mapped(baseline_artifact_kind::source_manager) ||
        !snapshot.mapped(baseline_artifact_kind::build_cache)) {
        return {status_code::artifact_corrupt};
    }

    compiled_image_view compiled_view;
    auto result = compiled_view.bind(
        snapshot.artifact(baseline_artifact_kind::compiled));
    if (!result.ok())
        return result;

    source_manager_image_view source_view;
    result =
        snapshot.bind_source_manager(
            source_view);
    if (!result.ok())
        return result;

    build_cache_image_view build_view;
    result = snapshot.bind_build_cache(build_view);
    if (!result.ok())
        return result;

    try {
        auto owner =
            std::make_unique<baseline_snapshot>(std::move(snapshot));

        mapped_compiled = compiled_view;
        mapped_sources = source_view;
        mapped_build_cache = build_view;
        set_build_fingerprint(owner->fingerprint());
        baseline = std::move(owner);
        compiled = std::make_unique<compiled_project_state>(
            mapped_compiled,
            mapped_sources,
            mapped_build_cache);

        source_mapping_status = {};
        source_mapping_attempted = true;
        source_mapping_ready.store(
            true,
            std::memory_order_release);

        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
}



status project_context::ensure_sources_mapped() const noexcept {
    if (source_mapping_ready.load(
            std::memory_order_acquire)) {
        return {};
    }

    std::lock_guard<std::mutex> lock{
        source_mapping_mutex};

    if (source_mapping_ready.load(
            std::memory_order_relaxed)) {
        return {};
    }

    if (source_mapping_attempted)
        return source_mapping_status;

    source_mapping_attempted = true;

    if (baseline == nullptr ||
        !baseline->valid() ||
        !build_fingerprint_available) {
        source_mapping_status = {
            status_code::invalid_state};
        return source_mapping_status;
    }

    if (!baseline->mapped(
            baseline_artifact_kind::source_manager)) {
        baseline_store store{
            project_configuration_path};

        auto result = store.map_source_manager(
            build_fingerprint_value,
            baseline->transaction(),
            *baseline);

        if (!result.ok()) {
            source_mapping_status = result;
            return result;
        }
    }

    auto result =
        baseline->bind_source_manager(
            mapped_sources);

    source_mapping_status = result;

    if (result.ok()) {
        // Release publishes mapped_sources and the mapped baseline ownership.
        // Warm readers need only the acquire load above and never enter mutex.
        source_mapping_ready.store(
            true,
            std::memory_order_release);
    }

    return result;
}

project_storage_pressure project_context::storage_pressure() const noexcept {
    if (compiled == nullptr) {
        project_storage_pressure output;

        if (finalized_generation != nullptr) {
            const auto segments =
                finalized_generation->segments();

            output.retained_bytes =
                segments.compiled_segment().size() +
                segments.sources_segment().size() +
                segments.change_segment().size() +
                segments.build_segment().size();
        }

        return output;
    }

    project_storage_pressure output;
    const auto& statistics = compiled->contributions.statistics();
    const auto contribution_usage = compiled->contributions.storage_usage();
    const auto graph_usage = compiled->graph_value.storage_usage(
        statistics.members, statistics.enum_values);

    output.retained_bytes =
        contribution_usage.retained_bytes + graph_usage.retained_bytes;
    output.reserve_bytes =
        contribution_usage.reserve_bytes + graph_usage.reserve_bytes;
    output.stale_bytes =
        contribution_usage.stale_bytes + graph_usage.stale_bytes;

    if (contribution_usage.construction_slots >
        compiled->graph_value.type_count()) {
        output.stale_bytes +=
            (contribution_usage.construction_slots -
             compiled->graph_value.type_count()) *
            sizeof(source_construction_state);
    }

    constexpr std::size_t minimum_policy_bytes = 1024u * 1024u;
    const auto used_bytes =
        output.retained_bytes > output.reserve_bytes
        ? output.retained_bytes - output.reserve_bytes
        : std::size_t{0};

    if (used_bytes >= minimum_policy_bytes) {
        const bool stale_limit =
            output.stale_bytes >= minimum_policy_bytes &&
            output.stale_bytes >= used_bytes / 4;
        const bool reserve_limit =
            output.reserve_bytes < used_bytes / 32;
        output.rebuild_recommended =
            stale_limit || reserve_limit;
    }

    return output;
}

identity_ref project_context::find_named_identity(
    std::string_view path,
    identity_kind final_kind) const noexcept {

    if (path.empty() || final_kind == identity_kind::root)
        return {};

    if (path.starts_with("::"))
        path.remove_prefix(2);
    if (path.empty())
        return {};

    auto parent = identity_root();
    std::size_t offset = 0;

    for (;;) {
        const auto separator = path.find("::", offset);
        const auto end =
            separator == std::string_view::npos
            ? path.size()
            : separator;

        if (end == offset)
            return {};

        const auto segment = path.substr(offset, end - offset);
        const auto name = find_string(segment);
        if (!name)
            return {};

        const auto kind =
            separator == std::string_view::npos
            ? final_kind
            : identity_kind::namespace_scope;

        parent = find_identity(parent, name, kind);
        if (!parent)
            return {};

        if (separator == std::string_view::npos)
            return parent;

        offset = separator + 2;
        if (offset >= path.size())
            return {};
    }
}

status project_context::find_type(
    std::string_view path,
    type_handle& output) const noexcept {

    output = {};
    if (path.empty())
        return {status_code::invalid_argument};

    const auto identity =
        find_named_identity(path, identity_kind::type);
    if (!identity)
        return {status_code::not_found};

    output = compiled != nullptr
        ? compiled->graph_value.find_type(identity)
        : mapped_compiled.find_type(identity);

    return output
        ? status{}
        : status{status_code::not_found};
}

status project_context::find_object(
    std::string_view path,
    object_handle& output) const noexcept {

    output = {};
    if (path.empty())
        return {status_code::invalid_argument};

    const auto identity =
        find_named_identity(path, identity_kind::object);
    if (!identity)
        return {status_code::not_found};

    output = compiled != nullptr
        ? compiled->graph_value.find_object(identity)
        : mapped_compiled.find_object(identity);

    return output
        ? status{}
        : status{status_code::not_found};
}

status project_context::find_endpoint(
    std::string_view path,
    object_endpoint& output) const noexcept {

    output = {};

    const auto separator = path.rfind('.');
    if (separator == std::string_view::npos ||
        separator == 0 ||
        separator + 1 >= path.size()) {
        return {status_code::invalid_argument};
    }

    if (path.find('.', separator + 1) != std::string_view::npos)
        return {status_code::invalid_argument};

    object_handle object;
    auto result =
        find_object(path.substr(0, separator), object);
    if (!result.ok())
        return result;

    const auto name = find_string(path.substr(separator + 1));
    if (!name)
        return {status_code::not_found};

    type_handle type;
    member_index member;

    if (compiled != nullptr) {
        TypeRef object_type;
        if (!compiled->graph_value.object_type(object, object_type))
            return {status_code::not_found};

        if (!compiled->graph_value.named(object_type, type))
            return {status_code::not_found};

        member = compiled->graph_value.find_member(type, name);
    } else {
        compiled_image_object_record entry;
        result = mapped_compiled.object(object, entry);
        if (!result.ok())
            return result;

        if (!mapped_compiled.named(entry.type, type))
            return {status_code::not_found};

        member = mapped_compiled.find_member(type, name);
    }

    if (!member)
        return {status_code::not_found};

    output = object_endpoint{object, member};
    return {};
}

status project_context::find_link(
    std::string_view target_path,
    link_handle& output) const noexcept {

    output = {};

    object_endpoint target_endpoint;
    const auto result =
        find_endpoint(target_path, target_endpoint);
    if (!result.ok())
        return result;

    output = compiled != nullptr
        ? compiled->graph_value.find_link(target_endpoint)
        : mapped_compiled.find_link(target_endpoint);

    return output
        ? status{}
        : status{status_code::not_found};
}

} // namespace cw::server
