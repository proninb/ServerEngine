#include "project_context.hpp"

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
        result = source_view.bind(
            snapshot.artifact(
                baseline_artifact_kind::source_manager));
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
        snapshot.artifact(baseline_artifact_kind::source_manager).empty() ||
        snapshot.artifact(baseline_artifact_kind::build_cache).empty()) {
        return {status_code::artifact_corrupt};
    }

    compiled_image_view compiled_view;
    auto result = compiled_view.bind(
        snapshot.artifact(baseline_artifact_kind::compiled));
    if (!result.ok())
        return result;

    source_manager_image_view source_view;
    result = source_view.bind(
        snapshot.artifact(baseline_artifact_kind::source_manager));
    if (!result.ok())
        return result;

    build_cache_image_view build_view;
    result = build_view.bind(
        snapshot.artifact(baseline_artifact_kind::build_cache));
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
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
}



status project_context::ensure_sources_mapped() const noexcept {
    std::lock_guard<std::mutex> lock{source_mapping_mutex};

    if (mapped_sources.valid())
        return {};

    if (source_mapping_attempted)
        return source_mapping_status;

    source_mapping_attempted = true;

    if (baseline == nullptr ||
        !baseline->valid() ||
        !build_fingerprint_available) {
        source_mapping_status = {status_code::invalid_state};
        return source_mapping_status;
    }

    if (!baseline->mapped(baseline_artifact_kind::source_manager)) {
        baseline_store store{project_configuration_path};
        auto result = store.map_source_manager(
            build_fingerprint_value,
            baseline->transaction(),
            *baseline);
        if (!result.ok()) {
            source_mapping_status = result;
            return result;
        }
    }

    auto result = mapped_sources.bind(
        baseline->artifact(baseline_artifact_kind::source_manager));
    source_mapping_status = result;
    return result;
}

project_storage_pressure project_context::storage_pressure() const noexcept {
    if (compiled == nullptr)
        return {};

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
