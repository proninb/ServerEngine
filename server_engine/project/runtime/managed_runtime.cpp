#include "managed_runtime.hpp"
#include "../frontend/aggregate_initializer.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace cw::server {
// Internal storage-neutral adapter; no borrowed pointer escapes construct().
class managed_graph_reader {
public:
    explicit managed_graph_reader(const project_context& value) : project(value) {}
    std::string_view string(string_id id) const { return project.string(id); }
    std::size_t object_slot_count() const { return project.construction_backed() ? project.compiled_graph().object_slot_count() : project.mapped_compiled.object_slot_count(); }
    object_handle object_at(std::size_t i) const { return project.construction_backed() ? project.compiled_graph().object_at(i) : project.mapped_compiled.object_at(i); }
    const object_entry* find(object_handle h) const {
        if (project.construction_backed()) return project.compiled_graph().find(h);
        compiled_image_object_record value;
        if (!project.mapped_compiled.object_raw(h, value).ok() || !value.live()) return nullptr;
        object_cache = {value.type, value.flags}; return &object_cache;
    }
    const type_entry* find(type_handle h) const {
        if (project.construction_backed()) return project.compiled_graph().find(h);
        compiled_image_type_record value;
        if (!project.mapped_compiled.type_raw(h, value).ok() || !value.live()) return nullptr;
        type_cache = {value.definition, value.kind, value.record_kind, value.enum_underlying, value.flags}; return &type_cache;
    }
    bool intrinsic(TypeRef t, intrinsic_type& out) const { return project.construction_backed() ? project.compiled_graph().intrinsic(t, out) : project.mapped_compiled.intrinsic(t, out); }
    bool named(TypeRef t, type_handle& out) const { return project.construction_backed() ? project.compiled_graph().named(t, out) : project.mapped_compiled.named(t, out); }
    bool derived(TypeRef t, derived_type_record& out) const { return project.construction_backed() ? project.compiled_graph().derived(t, out) : project.mapped_compiled.derived(t, out); }
    std::vector<member_record> members(type_handle type) const {
        if (project.construction_backed()) {
            const auto values = project.compiled_graph().members(type);
            return {values.begin(), values.end()};
        }
        std::vector<member_record> out;
        for (std::size_t i = 0; i < project.mapped_compiled.member_count(type); ++i) {
            compiled_image_member_record value;
            if (!project.mapped_compiled.member(type, member_index::from_zero_based(static_cast<std::uint32_t>(i)), value).ok()) throw std::runtime_error("Corrupt member");
            out.push_back({value.name, value.type, value.access, {}, value.construction});
        }
        return out;
    }
    std::vector<link_record> links() const {
        std::vector<link_record> out;
        if (project.construction_backed()) {
            const auto values = project.compiled_graph().data_view().links;
            for (std::size_t i = 0; i < values.size(); ++i) out.push_back(values[i]);
        } else {
            for (std::size_t i = 0; i < project.mapped_compiled.link_slot_count(); ++i) {
                compiled_image_link_record value;
                if (!project.mapped_compiled.link_raw(link_handle{static_cast<std::uint32_t>(i + 1)}, value).ok()) throw std::runtime_error("Corrupt link");
                out.push_back({value.source, value.target});
            }
        }
        return out;
    }
private:
    const project_context& project;
    mutable object_entry object_cache;
    mutable type_entry type_cache;
};
namespace {
constexpr auto absent = static_cast<std::size_t>(-1);
bool align_up(std::size_t& offset, std::size_t alignment) {
    if (offset > (std::numeric_limits<std::size_t>::max)() - (alignment - 1)) return false;
    offset = (offset + alignment - 1) / alignment * alignment;
    return true;
}

bool scalar_layout(const managed_graph_reader& graph, TypeRef type, abi_target abi, managed_field_layout& field) {
    derived_type_record derived;
    bool indirect = false;
    while (graph.derived(type, derived)) {
        switch (derived.kind) {
        case derived_type_kind::const_qualified: field.read_only = true; break;
        case derived_type_kind::volatile_qualified: return false;
        case derived_type_kind::lvalue_reference:
            if (indirect) return false;
            field.reference = true; indirect = true; break;
        case derived_type_kind::pointer:
            if (indirect) return false;
            field.pointer = true; indirect = true; break;
        default: return false; // Arrays and rvalue references are separate capabilities.
        }
        type = derived.child;
    }
    field.value_type = type;
    intrinsic_type intrinsic;
    if (!graph.intrinsic(type, intrinsic)) return false;
    if (field.pointer) { field.size = 8; field.alignment = 8; return true; }
    switch (intrinsic) {
    case intrinsic_type::bool_type: field.size = 1; field.boolean = true; break;
    case intrinsic_type::unsigned_char:
    case intrinsic_type::char8_type: field.size = 1; break;
    case intrinsic_type::char_type:
    case intrinsic_type::signed_char: field.size = 1; field.is_signed = true; break;
    case intrinsic_type::signed_short: field.size = 2; field.is_signed = true; break;
    case intrinsic_type::unsigned_short:
    case intrinsic_type::char16_type: field.size = 2; break;
    case intrinsic_type::signed_int: field.size = 4; field.is_signed = true; break;
    case intrinsic_type::unsigned_int:
    case intrinsic_type::char32_type: field.size = 4; break;
    case intrinsic_type::wchar_type: field.size = abi == abi_target::windows_x64 ? 2 : 4; break;
    case intrinsic_type::signed_long: field.size = abi == abi_target::windows_x64 ? 4 : 8; field.is_signed = true; break;
    case intrinsic_type::unsigned_long: field.size = abi == abi_target::windows_x64 ? 4 : 8; break;
    case intrinsic_type::signed_long_long: field.size = 8; field.is_signed = true; break;
    case intrinsic_type::unsigned_long_long: field.size = 8; break;
    case intrinsic_type::float_type: field.size = 4; field.floating = true; break;
    case intrinsic_type::double_type: field.size = 8; field.floating = true; break;
    default: return false;
    }
    field.alignment = field.size;
    return true;
}
bool integer_fits(const managed_field_layout& field, std::uint64_t bits, bool negative) {
    if (field.pointer) return bits == 0;
    if (field.boolean) return !negative && bits <= 1;
    if (!field.is_signed && negative) return false;
    if (field.size == 8) return !field.is_signed || negative || bits <= std::uint64_t(INT64_MAX);
    const unsigned width = field.size * 8;
    if (negative) return std::bit_cast<std::int64_t>(bits) >= -(std::int64_t{1} << (width - 1));
    return bits < (std::uint64_t{1} << (width - (field.is_signed ? 1 : 0)));
}
bool initialize(const managed_field_layout& field, std::byte* out) {
    const auto value = field.initial;
    if (!valid_construction(value) || value.kind == construction_kind::unsupported || value.kind == construction_kind::aggregate) return false;
    if (field.aggregate) return value.kind == construction_kind::zero;
    if (field.reference) return value.kind == construction_kind::zero || value.kind == construction_kind::member_binding;
    if (value.kind == construction_kind::member_binding) return false;
    if (field.floating) {
        const double number = value.kind == construction_kind::real ? std::bit_cast<double>(value.bits()) :
            value.kind == construction_kind::signed_integer ? static_cast<double>(std::bit_cast<std::int64_t>(value.bits())) : static_cast<double>(value.bits());
        if (!std::isfinite(number)) return false;
        if (field.size == 4) {
            const float narrowed = static_cast<float>(number);
            if (!std::isfinite(narrowed)) return false;
            std::memcpy(out, &narrowed, 4);
        } else std::memcpy(out, &number, 8);
        return true;
    }
    if (value.kind == construction_kind::real || !integer_fits(field, value.bits(), value.kind == construction_kind::signed_integer && std::bit_cast<std::int64_t>(value.bits()) < 0)) return false;
    const auto bits = value.bits();
    std::memcpy(out, &bits, field.size);
    return true;
}
}

std::size_t managed_runtime::field(object_endpoint endpoint) const noexcept {
    if (!endpoint.object || !endpoint.member || endpoint.object.value() >= object_slots.size()) return missing;
    const auto slot = object_slots[endpoint.object.value()];
    if (slot == missing) return missing;
    const auto& object = object_layouts[slot];
    return endpoint.member.value() < object.field_count ? object.first_field + endpoint.member.value() : missing;
}

std::size_t managed_runtime::field(object_endpoint endpoint, std::span<const member_index> path) const noexcept {
    auto index = field(endpoint);
    for (const auto member : path) {
        if (index == missing || !member) return missing;
        const auto& parent = field_layouts[index];
        if (!parent.aggregate || member.value() >= parent.child_count) return missing;
        index = parent.first_child + member.value();
    }
    return index;
}

status managed_runtime::construct(const project_read_view& project, std::string& error) noexcept {
    try {
        error.clear();
        const auto fail = [&](const char* detail) { error = detail; return status{status_code::not_available}; };
        if (!project.valid()) return fail("Project is not available");
        if (std::endian::native != std::endian::little) return fail("Managed Runtime requires a little-endian host");
        const managed_graph_reader graph{*project.project};
        const auto abi = project.configuration().abi;
        if (!is_supported_abi_configuration(abi)) return fail("Unsupported ABI configuration");
        managed_runtime candidate;
        candidate.object_slots.assign(graph.object_slot_count() + 1, missing);
        std::vector<type_handle> active_types;
        // Reserve siblings before descending so both Graph endpoints and local
        // reference operands retain their record-local member ordering.
        const auto layout_record = [&](auto&& self, type_handle type, object_endpoint root,
            bool read_only, std::size_t& size, std::size_t& alignment,
            std::size_t& first, std::size_t& members_count) -> bool {
            if (active_types.size() >= 64 || std::find(active_types.begin(), active_types.end(), type) != active_types.end()) {
                error = "Recursive value containment or managed nesting depth exceeds 64"; return false;
            }
            const auto* record = graph.find(type);
            if (!record || !record->defined() || record->kind != graph_type_kind::record || record->record_kind == source_record_kind::union_type) {
                error = "Managed values require defined struct/class records; unions are unsupported"; return false;
            }
            active_types.push_back(type);
            const auto members = graph.members(type);
            first = candidate.field_layouts.size(); members_count = members.size();
            candidate.field_layouts.resize(first + members_count);
            size = 0; alignment = 1;
            for (std::size_t local = 0; local < members_count; ++local) {
                const auto& member = members[local];
                managed_field_layout f;
                f.endpoint = root.member ? root : object_endpoint{root.object, member_index::from_zero_based(static_cast<std::uint32_t>(local))};
                f.initial = member.construction;
                f.sibling_first = first; f.sibling_count = members_count;
                const auto descendants_begin = candidate.field_layouts.size();
                if (!scalar_layout(graph, member.type, abi.target, f)) {
                    // Only const-qualified by-value records are expanded. Record
                    // references/pointers need a separate aggregate-link contract.
                    auto base = member.type;
                    bool qualified_const = false;
                    derived_type_record derived;
                    while (graph.derived(base, derived) && derived.kind == derived_type_kind::const_qualified) {
                        qualified_const = true; base = derived.child;
                    }
                    type_handle nested;
                    if (!graph.named(base, nested)) { error = "Unsupported managed field type"; return false; }
                    f = {};
                    f.endpoint = root.member ? root : object_endpoint{root.object, member_index::from_zero_based(static_cast<std::uint32_t>(local))};
                    f.initial = member.construction; f.value_type = base; f.aggregate = true;
                    f.read_only = read_only || qualified_const;
                    f.sibling_first = first; f.sibling_count = members_count;
                    std::size_t nested_size, nested_alignment;
                    if (!self(self, nested, f.endpoint, f.read_only, nested_size, nested_alignment, f.first_child, f.child_count)) return false;
                    if (nested_size > UINT32_MAX) { error = "Managed record layout exceeds 4 GiB"; return false; }
                    f.size = static_cast<std::uint32_t>(nested_size); f.alignment = static_cast<std::uint32_t>(nested_alignment);
                } else if (!f.reference) {
                    // Const containment does not make a reference's referent const.
                    f.read_only = f.read_only || read_only;
                }
                const auto field_alignment = (std::min)(std::size_t(f.reference ? 8 : f.alignment), std::size_t(abi.pack));
                if (!align_up(size, field_alignment)) { error = "Layout overflow"; return false; }
                f.offset = size;
                const auto bytes = f.reference ? 8 : f.size;
                if (size > SIZE_MAX - bytes) { error = "Layout overflow"; return false; }
                size += bytes; alignment = (std::max)(alignment, field_alignment);
                for (auto child = descendants_begin; child < candidate.field_layouts.size(); ++child)
                    candidate.field_layouts[child].offset += f.offset;
                candidate.field_layouts[first + local] = f;
            }
            if (!size) size = 1;
            if (!align_up(size, alignment)) { error = "Layout overflow"; return false; }
            active_types.pop_back(); return true;
        };
        std::size_t total = 0;
        for (std::size_t i = 0; i < graph.object_slot_count(); ++i) {
            const auto object = graph.object_at(i);
            const auto* entry = graph.find(object);
            if (!entry) continue;
            if (entry->flags & 1u) return fail("Non-default object initializers are unsupported by managed construction");
            type_handle type;
            if (!graph.named(entry->type, type)) return fail("Managed objects require a direct record type");
            managed_object_layout layout{object, 0, 0, 1, candidate.field_layouts.size(), 0};
            if (!layout_record(layout_record, type, {object, {}}, false, layout.size, layout.alignment, layout.first_field, layout.field_count))
                return {status_code::not_available};
            if (!align_up(layout.size, layout.alignment) || !align_up(total, layout.alignment) || total > SIZE_MAX - layout.size) return fail("Layout overflow");
            layout.offset = total;
            total += layout.size;
            for (std::size_t j = layout.first_field; j < candidate.field_layouts.size(); ++j) candidate.field_layouts[j].offset += layout.offset;
            candidate.object_slots[object.value()] = candidate.object_layouts.size();
            candidate.object_layouts.push_back(layout);
        }
        const auto count = candidate.field_layouts.size();
        const auto apply_initializer = [&](auto&& self, std::size_t index, const aggregate_initializer& value) -> bool {
            auto& f = candidate.field_layouts[index];
            if (f.aggregate) {
                if (!value.list || value.children.size() > f.child_count) return false;
                for (std::size_t local = 0; local < value.children.size(); ++local)
                    if (!self(self, f.first_child + local, value.children[local])) return false;
                f.initial = {}; return true;
            }
            if (value.list) {
                if (value.children.size() > 1) return false;
                if (value.children.empty()) { f.initial = {}; return true; }
                return self(self, index, value.children.front());
            }
            if (f.reference) return false; // Explicit numeric reference initializers are not bindings.
            f.initial = value.value; return true;
        };
        // Descendant defaults first, then enclosing explicit lists override only
        // supplied members. Omitted members keep their own managed defaults.
        for (std::size_t i = count; i != 0; --i) {
            auto& f = candidate.field_layouts[i - 1];
            if (f.initial.kind == construction_kind::aggregate) {
                aggregate_initializer value;
                if (!parse_aggregate_initializer(graph.string(f.initial.expression()), value) ||
                    !apply_initializer(apply_initializer, i - 1, value))
                    return fail("Invalid aggregate initializer shape, too many elements or unsupported reference initializer");
            } else if (f.aggregate && f.initial.kind != construction_kind::zero) {
                return fail("Nested record initialization requires braces");
            }
        }
        std::vector<std::size_t> references(count, missing), incoming(count, missing);
        const auto compatible = [&](std::size_t target, std::size_t source) {
            const auto& t = candidate.field_layouts[target]; const auto& s = candidate.field_layouts[source];
            return !t.aggregate && !s.aggregate && t.value_type == s.value_type && t.pointer == s.pointer && t.size == s.size &&
                (!t.reference || t.read_only || !s.read_only);
        };
        for (std::size_t i = 0; i < count; ++i) {
            const auto& f = candidate.field_layouts[i];
            if (f.initial.kind == construction_kind::member_binding) {
                const auto source = f.initial.operand && f.initial.operand <= f.sibling_count ? f.sibling_first + f.initial.operand - 1 : missing;
                if (!f.reference || source == missing || !compatible(i, source)) return fail("Invalid local reference binding");
                references[i] = source;
            }
        }
        for (const auto& link : graph.links()) {
            if (!link.live()) continue;
            const auto target = candidate.field(link.target), source = candidate.field(link.source);
            if (target == missing || source == missing || !compatible(target, source)) return fail("Link endpoint type mismatch");
            if (incoming[target] != missing) return fail("Multiple links target one field");
            incoming[target] = source;
            if (candidate.field_layouts[target].reference) references[target] = source;
            else if (candidate.field_layouts[target].read_only) return fail("A value link cannot target a const field");
        }
        candidate.resolved.assign(count, missing);
        // Resolve aliases iteratively, including forward references and chains.
        std::vector<unsigned char> state(count);
        for (std::size_t i = 0; i < count; ++i) {
            std::vector<std::size_t> path;
            auto current = i;
            while (candidate.resolved[current] == missing) {
                if (state[current] == 1) return fail("Reference binding cycle");
                state[current] = 1; path.push_back(current);
                if (!candidate.field_layouts[current].reference) { candidate.resolved[current] = current; break; }
                if (references[current] == missing) return fail("Reference field has no binding");
                current = references[current];
            }
            for (auto item : path) { candidate.resolved[item] = candidate.resolved[current]; state[item] = 2; }
        }
        // Value links are topologically ordered after reference aliases resolve.
        std::vector<std::vector<std::size_t>> outgoing(count);
        std::vector<std::size_t> indegree(count), source_for(count, missing), queue;
        for (std::size_t i = 0; i < count; ++i) {
            if (incoming[i] == missing || candidate.field_layouts[i].reference) continue;
            const auto source = candidate.resolved[incoming[i]];
            if (source == i) continue;
            source_for[i] = source; outgoing[source].push_back(i); ++indegree[i];
        }
        for (std::size_t i = 0; i < count; ++i) if (indegree[i] == 0) queue.push_back(i);
        for (std::size_t i = 0; i < queue.size(); ++i) {
            const auto node = queue[i];
            if (source_for[node] != missing) candidate.copies.push_back({node, source_for[node]});
            for (auto target : outgoing[node]) if (--indegree[target] == 0) queue.push_back(target);
        }
        if (queue.size() != count) return fail("Value link cycle");
        candidate.storage.resize(total); // Managed default initialization is deterministic zero.
        for (const auto& f : candidate.field_layouts)
            if (!initialize(f, candidate.storage.data() + f.offset)) return fail("Unsupported initializer or constant out of range");
        candidate.propagate();
        *this = std::move(candidate);
        return {};
    } catch (const std::bad_alloc&) { return {status_code::not_available}; }
      catch (const std::length_error&) { return {status_code::not_available}; }
      catch (const std::runtime_error&) { return {status_code::artifact_corrupt}; }
}

void managed_runtime::propagate() noexcept {
    for (const auto& link : copies) {
        const auto& source = field_layouts[link.source]; const auto& target = field_layouts[link.target];
        std::memcpy(storage.data() + target.offset, storage.data() + source.offset, target.size);
    }
}
bool managed_runtime::aliases(object_endpoint a, object_endpoint b) const noexcept {
    const auto first = field(a), second = field(b);
    return first != missing && second != missing && resolved[first] == resolved[second];
}
status managed_runtime::read_integer(object_endpoint endpoint, std::int64_t& value) const noexcept {
    return read_integer(endpoint, {}, value);
}
status managed_runtime::read_integer(object_endpoint endpoint, std::span<const member_index> path, std::int64_t& value) const noexcept {
    const auto index = field(endpoint, path);
    if (index == missing) return {status_code::not_found};
    const auto& f = field_layouts[resolved[index]];
    if (f.aggregate || f.floating || f.pointer) return {status_code::invalid_argument};
    std::uint64_t bits = 0; std::memcpy(&bits, storage.data() + f.offset, f.size);
    if (f.is_signed && f.size < 8 && (bits & (std::uint64_t{1} << (f.size * 8 - 1)))) bits |= ~((std::uint64_t{1} << (f.size * 8)) - 1);
    if (!f.is_signed && bits > std::uint64_t(INT64_MAX)) return {status_code::invalid_argument};
    value = std::bit_cast<std::int64_t>(bits); return {};
}
status managed_runtime::write_integer(object_endpoint endpoint, std::int64_t value) noexcept {
    return write_integer(endpoint, {}, value);
}
status managed_runtime::write_integer(object_endpoint endpoint, std::span<const member_index> path, std::int64_t value) noexcept {
    const auto index = field(endpoint, path);
    if (index == missing) return {status_code::not_found};
    const auto& f = field_layouts[resolved[index]];
    const auto bits = std::bit_cast<std::uint64_t>(value);
    if (f.aggregate || f.floating || f.pointer || field_layouts[index].read_only || f.read_only || !integer_fits(f, bits, value < 0)) return {status_code::invalid_argument};
    std::memcpy(storage.data() + f.offset, &bits, f.size); propagate(); return {};
}
status managed_runtime::read_real(object_endpoint endpoint, double& value) const noexcept {
    return read_real(endpoint, {}, value);
}
status managed_runtime::read_real(object_endpoint endpoint, std::span<const member_index> path, double& value) const noexcept {
    const auto index = field(endpoint, path);
    if (index == missing) return {status_code::not_found};
    const auto& f = field_layouts[resolved[index]];
    if (!f.floating) return {status_code::invalid_argument};
    if (f.size == 4) { float number; std::memcpy(&number, storage.data() + f.offset, 4); value = number; }
    else std::memcpy(&value, storage.data() + f.offset, 8);
    return {};
}
status managed_runtime::write_real(object_endpoint endpoint, double value) noexcept {
    return write_real(endpoint, {}, value);
}
status managed_runtime::write_real(object_endpoint endpoint, std::span<const member_index> path, double value) noexcept {
    const auto index = field(endpoint, path);
    if (index == missing) return {status_code::not_found};
    auto f = field_layouts[resolved[index]];
    if (!f.floating || f.read_only || field_layouts[index].read_only) return {status_code::invalid_argument};
    f.initial = construction_value::constant(construction_kind::real, std::bit_cast<std::uint64_t>(value));
    if (!initialize(f, storage.data() + f.offset)) return {status_code::invalid_argument};
    propagate(); return {};
}
} // namespace cw::server
