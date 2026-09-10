#include "project_context.hpp"

#include <memory>
#include <utility>

namespace cw::server {

project_context::project_context(project_configuration configuration)
    : project_configuration_value(std::move(configuration)),
      compiled(std::make_unique<compiled_project_state>()) {}

project_storage_pressure project_context::storage_pressure() const noexcept {
    project_storage_pressure output;
    const auto& statistics = compiled->contributions.statistics();
    const auto contribution_usage = compiled->contributions.storage_usage();
    const auto graph_usage = compiled->graph_value.storage_usage(
        statistics.members, statistics.enum_values);

    output.retained_bytes = contribution_usage.retained_bytes + graph_usage.retained_bytes;
    output.reserve_bytes = contribution_usage.reserve_bytes + graph_usage.reserve_bytes;
    output.stale_bytes = contribution_usage.stale_bytes + graph_usage.stale_bytes;

    if (contribution_usage.construction_slots > compiled->graph_value.type_count()) {
        output.stale_bytes +=
            (contribution_usage.construction_slots - compiled->graph_value.type_count()) *
            sizeof(source_construction_state);
    }

    // G0 establishes approximately 6.25% incremental headroom. Rebuild before
    // half of that reserve remains, or when known stale storage reaches 25% of
    // used compiled storage. Small Projects are excluded from proactive churn.
    constexpr std::size_t minimum_policy_bytes = 1024u * 1024u;
    const auto used_bytes = output.retained_bytes > output.reserve_bytes
        ? output.retained_bytes - output.reserve_bytes
        : std::size_t{0};
    if (used_bytes >= minimum_policy_bytes) {
        const bool stale_limit = output.stale_bytes >= minimum_policy_bytes &&
            output.stale_bytes >= used_bytes / 4;
        const bool reserve_limit = output.reserve_bytes < used_bytes / 32;
        output.rebuild_recommended = stale_limit || reserve_limit;
    }
    return output;
}

identity_ref project_context::find_named_identity(
    std::string_view path,
    identity_kind final_kind) const noexcept {

    if (path.empty() || final_kind == identity_kind::root)
        return nullptr;

    if (path.starts_with("::"))
        path.remove_prefix(2);
    if (path.empty())
        return nullptr;

    auto parent = compiled->identities.root();
    std::size_t offset = 0;
    for (;;) {
        const auto separator = path.find("::", offset);
        const auto end = separator == std::string_view::npos ? path.size() : separator;
        if (end == offset)
            return nullptr;

        const auto segment = path.substr(offset, end - offset);
        const auto name = compiled->strings.find(segment);
        if (!name)
            return nullptr;

        const auto kind = separator == std::string_view::npos
            ? final_kind
            : identity_kind::namespace_scope;
        parent = compiled->identities.find(parent, name, kind);
        if (parent == nullptr)
            return nullptr;

        if (separator == std::string_view::npos)
            return parent;
        offset = separator + 2;
        if (offset >= path.size())
            return nullptr;
    }
}

status project_context::find_type(
    std::string_view path,
    type_handle& output) const noexcept {

    output = {};
    if (path.empty())
        return {status_code::invalid_argument};
    const auto identity = find_named_identity(path, identity_kind::type);
    if (identity == nullptr)
        return {status_code::not_found};
    output = compiled->graph_value.find_type(identity);
    return output ? status{} : status{status_code::not_found};
}

status project_context::find_object(
    std::string_view path,
    object_handle& output) const noexcept {

    output = {};
    if (path.empty())
        return {status_code::invalid_argument};
    const auto identity = find_named_identity(path, identity_kind::object);
    if (identity == nullptr)
        return {status_code::not_found};
    output = compiled->graph_value.find_object(identity);
    return output ? status{} : status{status_code::not_found};
}

status project_context::find_endpoint(
    std::string_view path,
    object_endpoint& output) const noexcept {

    output = {};
    const auto separator = path.rfind('.');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= path.size())
        return {status_code::invalid_argument};
    if (path.find('.', separator + 1) != std::string_view::npos)
        return {status_code::invalid_argument};

    object_handle object;
    auto result = find_object(path.substr(0, separator), object);
    if (!result.ok())
        return result;

    const auto name = compiled->strings.find(path.substr(separator + 1));
    if (!name)
        return {status_code::not_found};
    const auto* entry = compiled->graph_value.find(object);
    if (entry == nullptr)
        return {status_code::not_found};

    type_handle type;
    if (!compiled->graph_value.named(entry->type, type))
        return {status_code::not_found};
    const auto member = compiled->graph_value.find_member(type, name);
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
    const auto result = find_endpoint(target_path, target_endpoint);
    if (!result.ok())
        return result;
    output = compiled->graph_value.find_link(target_endpoint);
    return output ? status{} : status{status_code::not_found};
}

} // namespace cw::server
