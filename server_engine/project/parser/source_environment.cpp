#include "source_environment.hpp"

#include "../persistence/build_cache_image.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace cw::server {
namespace {

[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

[[nodiscard]] std::uint64_t binding_hash(identity_ref owner, string_id name) noexcept {
    return mix64(
        static_cast<std::uint64_t>(owner.value()) ^
        (static_cast<std::uint64_t>(name.value()) << 32));
}

[[nodiscard]] std::size_t capacity_for(std::size_t count) noexcept {
    std::size_t capacity = 16;
    const auto target = count > (std::numeric_limits<std::size_t>::max)() / 2
        ? (std::numeric_limits<std::size_t>::max)()
        : count * 2 + 1;

    while (capacity < target) {
        if (capacity > (std::numeric_limits<std::size_t>::max)() / 2)
            return 0;
        capacity *= 2;
    }
    return capacity;
}

template <typename T>
class runtime_page_store final {
public:
    [[nodiscard]] std::span<T> allocate(std::size_t count) {
        if (count == 0)
            return {};

        constexpr std::size_t page_bytes = 1024 * 1024;
        const auto default_capacity =
            (std::max)(
                std::size_t{1},
                page_bytes / sizeof(T));

        if (pages.empty() ||
            pages.back().capacity - pages.back().used < count) {

            const auto capacity =
                (std::max)(default_capacity, count);

            page next;
            next.values =
                std::make_unique<T[]>(capacity);
            next.capacity = capacity;
            pages.push_back(std::move(next));
        }

        auto& current = pages.back();
        auto* begin =
            current.values.get() + current.used;
        current.used += count;
        return {begin, count};
    }

    [[nodiscard]] std::size_t page_count() const noexcept {
        return pages.size();
    }

    [[nodiscard]] std::size_t reserved_bytes() const noexcept {
        std::size_t total = 0;
        for (const auto& item : pages) {
            const auto bytes =
                item.capacity * sizeof(T);

            if (total >
                (std::numeric_limits<std::size_t>::max)() -
                    bytes) {
                return (std::numeric_limits<std::size_t>::max)();
            }

            total += bytes;
        }
        return total;
    }

private:
    struct page final {
        std::unique_ptr<T[]> values;
        std::size_t capacity = 0;
        std::size_t used = 0;
    };

    std::vector<page> pages;
};

} // namespace

struct source_interface_runtime_store::implementation final {
    mutable std::mutex mutex;
    runtime_page_store<identity_ref> local_types;
    runtime_page_store<source_interface_type_slot> type_slots;
    runtime_page_store<source_interface_object_slot> object_slots;
    runtime_page_store<source_interface_member_slot> member_slots;
    runtime_page_store<const source_interface*> imports;
};

source_interface_runtime_store::source_interface_runtime_store() noexcept = default;

source_interface_runtime_store::~source_interface_runtime_store() = default;

source_interface_runtime_store::source_interface_runtime_store(
    source_interface_runtime_store&&) noexcept = default;

source_interface_runtime_store&
source_interface_runtime_store::operator=(
    source_interface_runtime_store&&) noexcept = default;

status source_interface_runtime_store::initialize() noexcept {
    if (value != nullptr)
        return {status_code::invalid_state};

    try {
        value = std::make_unique<implementation>();
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
}

status source_interface_runtime_store::allocate(
    std::size_t local_type_count,
    std::size_t type_slot_count,
    std::size_t object_slot_count,
    std::size_t member_slot_count,
    std::size_t import_count,
    source_interface_runtime_write_view& output) noexcept {

    output = {};
    if (value == nullptr)
        return {status_code::invalid_state};

    try {
        std::lock_guard<std::mutex> lock{
            value->mutex};

        output.local_types =
            value->local_types.allocate(local_type_count);
        output.type_slots =
            value->type_slots.allocate(type_slot_count);
        output.object_slots =
            value->object_slots.allocate(object_slot_count);
        output.member_slots =
            value->member_slots.allocate(member_slot_count);
        output.imports =
            value->imports.allocate(import_count);

        return {};
    }
    catch (const std::bad_alloc&) {
        output = {};
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        output = {};
        return {status_code::not_available};
    }
}

std::size_t
source_interface_runtime_store::page_count() const noexcept {
    if (value == nullptr)
        return 0;

    std::lock_guard<std::mutex> lock{
        value->mutex};

    return
        value->local_types.page_count() +
        value->type_slots.page_count() +
        value->object_slots.page_count() +
        value->member_slots.page_count() +
        value->imports.page_count();
}

std::size_t
source_interface_runtime_store::reserved_bytes() const noexcept {
    if (value == nullptr)
        return 0;

    std::lock_guard<std::mutex> lock{
        value->mutex};

    const std::array<std::size_t, 5> parts{
        value->local_types.reserved_bytes(),
        value->type_slots.reserved_bytes(),
        value->object_slots.reserved_bytes(),
        value->member_slots.reserved_bytes(),
        value->imports.reserved_bytes(),
    };

    std::size_t total = 0;
    for (const auto bytes : parts) {
        if (total >
            (std::numeric_limits<std::size_t>::max)() -
                bytes) {
            return (std::numeric_limits<std::size_t>::max)();
        }
        total += bytes;
    }

    return total;
}

status source_interface::initialize(
    const source_facts& facts,
    identity_view identities,
    std::span<const source_interface* const> imports,
    bool retain_compact_persistence,
    source_interface_runtime_store* runtime_store) noexcept {

    if (runtime_store != nullptr) {
        if (retain_compact_persistence)
            return {status_code::invalid_argument};

        return initialize_external(
            facts,
            identities,
            imports,
            *runtime_store);
    }

    try {
        const auto type_count = facts.records().size() + facts.enums().size();
        std::size_t member_count = 0;
        for (const auto& record : facts.records()) {
            if (record.declaration_kind == source_record_declaration_kind::definition)
                member_count += record.members.count;
        }

        const auto type_capacity = capacity_for(type_count);
        const auto object_capacity = capacity_for(facts.objects().size());
        const auto member_capacity = capacity_for(member_count);
        if (type_capacity == 0 || object_capacity == 0 || member_capacity == 0)
            return {status_code::not_available};

        std::vector<identity_ref> new_types;
        std::vector<type_slot> new_type_slots(type_capacity);
        std::vector<object_slot> new_object_slots(object_capacity);
        std::vector<member_slot> new_member_slots(member_capacity);

        std::vector<type_slot> new_persistence_type_slots;
        std::vector<object_slot> new_persistence_object_slots;
        std::vector<member_slot> new_persistence_member_slots;
        std::vector<const source_interface*> new_imports;

        std::size_t new_persistence_type_count = 0;
        std::size_t new_persistence_object_count = 0;
        std::size_t new_persistence_member_count = 0;

        new_types.reserve(type_count);

        if (retain_compact_persistence) {
            new_persistence_type_slots.reserve(type_count);
            new_persistence_object_slots.reserve(
                facts.objects().size());
            new_persistence_member_slots.reserve(member_count);
        }

        new_imports.reserve(imports.size());

        const auto insert_type = [&](identity_ref identity) -> status {
            if (!identity || identity.kind() != identity_kind::type)
                return {status_code::invalid_argument};

            const auto parent = identities.parent(identity);
            const auto name = identities.name(identity);
            if (!parent || !name)
                return {status_code::invalid_argument};

            const auto mask = new_type_slots.size() - 1;
            auto position =
                static_cast<std::size_t>(binding_hash(parent, name)) & mask;

            for (;;) {
                auto& slot = new_type_slots[position];
                if (!slot.identity) {
                    const type_slot value{
                        parent,
                        name,
                        identity,
                    };
                    slot = value;
                    new_types.push_back(identity);
                    ++new_persistence_type_count;

                    if (retain_compact_persistence)
                        new_persistence_type_slots.push_back(value);

                    return {};
                }

                if (slot.parent == parent && slot.name == name) {
                    return slot.identity == identity
                        ? status{}
                        : status{status_code::semantic_conflict};
                }

                position = (position + 1) & mask;
            }
        };

        for (const auto& record : facts.records()) {
            const auto result = insert_type(record.identity);
            if (!result.ok())
                return result;
        }

        for (const auto& enum_fact : facts.enums()) {
            const auto result = insert_type(enum_fact.identity);
            if (!result.ok())
                return result;
        }

        const auto object_mask = new_object_slots.size() - 1;
        for (const auto& object : facts.objects()) {
            if (!object.identity || object.identity.kind() != identity_kind::object)
                return {status_code::invalid_argument};

            const auto parent = identities.parent(object.identity);
            const auto name = identities.name(object.identity);
            if (!parent || !name)
                return {status_code::invalid_argument};

            auto position =
                static_cast<std::size_t>(binding_hash(parent, name)) & object_mask;

            const auto named_type = object.type.identity && object.type.modifiers.count == 0
                ? object.type.identity
                : identity_ref{};

            for (;;) {
                auto& slot = new_object_slots[position];
                if (!slot.identity) {
                    const object_slot value{
                        parent,
                        name,
                        object.identity,
                        named_type,
                    };
                    slot = value;
                    ++new_persistence_object_count;

                    if (retain_compact_persistence)
                        new_persistence_object_slots.push_back(value);

                    break;
                }

                if (slot.parent == parent && slot.name == name) {
                    if (slot.identity != object.identity ||
                        slot.named_type != named_type) {
                        return {status_code::semantic_conflict};
                    }
                    break;
                }

                position = (position + 1) & object_mask;
            }
        }

        const auto member_mask = new_member_slots.size() - 1;
        for (const auto& record : facts.records()) {
            if (record.declaration_kind != source_record_declaration_kind::definition)
                continue;

            if (!record.identity ||
                record.identity.kind() != identity_kind::type ||
                record.members.begin > facts.members().size() ||
                record.members.count > facts.members().size() - record.members.begin ||
                record.bases.begin > facts.bases().size() ||
                record.bases.count > facts.bases().size() - record.bases.begin ||
                record.methods.begin > facts.methods().size() ||
                record.methods.count > facts.methods().size() - record.methods.begin) {
                return {status_code::invalid_argument};
            }

            for (std::uint32_t index = 0; index < record.members.count; ++index) {
                const auto& member = facts.members()[record.members.begin + index];
                if (!member.name)
                    return {status_code::invalid_argument};

                auto position = static_cast<std::size_t>(
                    binding_hash(record.identity, member.name)) & member_mask;

                for (;;) {
                    auto& slot = new_member_slots[position];
                    if (!slot.type) {
                        const member_slot value{
                            record.identity,
                            member.name,
                            member_index::from_zero_based(index),
                        };
                        slot = value;
                        ++new_persistence_member_count;

                        if (retain_compact_persistence)
                            new_persistence_member_slots.push_back(value);

                        break;
                    }

                    if (slot.type == record.identity && slot.name == member.name)
                        return {status_code::semantic_conflict};

                    position = (position + 1) & member_mask;
                }
            }
        }

        for (const auto* imported : imports) {
            if (imported == nullptr)
                return {status_code::invalid_argument};
            new_imports.push_back(imported);
        }

        local_type_values.swap(new_types);
        type_slots.swap(new_type_slots);
        object_slots.swap(new_object_slots);
        member_slots.swap(new_member_slots);

        persistence_type_slots.swap(
            new_persistence_type_slots);
        persistence_object_slots.swap(
            new_persistence_object_slots);
        persistence_member_slots.swap(
            new_persistence_member_slots);

        persistence_local_type_count =
            local_type_values.size();
        persistence_type_slot_count =
            new_persistence_type_count;
        persistence_object_slot_count =
            new_persistence_object_count;
        persistence_member_slot_count =
            new_persistence_member_count;
        persistence_compact_state =
            retain_compact_persistence;

        external_persistence_data = {};
        persistence_externalized_state = false;
        imported_interfaces.swap(new_imports);
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

status source_interface::initialize_external(
    const source_facts& facts,
    identity_view identities,
    std::span<const source_interface* const> imports,
    source_interface_runtime_store& runtime_store) noexcept {

    if (runtime_externalized_state ||
        persistence_externalized_state) {
        return {status_code::invalid_state};
    }

    const auto type_count =
        facts.records().size() +
        facts.enums().size();

    std::size_t member_count = 0;
    for (const auto& record : facts.records()) {
        if (record.declaration_kind ==
            source_record_declaration_kind::definition) {
            member_count += record.members.count;
        }
    }

    const auto type_capacity =
        type_count == 0
        ? std::size_t{0}
        : capacity_for(type_count);

    const auto object_capacity =
        facts.objects().empty()
        ? std::size_t{0}
        : capacity_for(facts.objects().size());

    const auto member_capacity =
        member_count == 0
        ? std::size_t{0}
        : capacity_for(member_count);

    if ((type_count != 0 && type_capacity == 0) ||
        (!facts.objects().empty() &&
         object_capacity == 0) ||
        (member_count != 0 &&
         member_capacity == 0)) {
        return {status_code::not_available};
    }

    source_interface_runtime_write_view runtime;
    auto result = runtime_store.allocate(
        type_count,
        type_capacity,
        object_capacity,
        member_capacity,
        imports.size(),
        runtime);
    if (!result.ok())
        return result;

    std::size_t local_type_position = 0;
    std::size_t persistence_type_count = 0;
    std::size_t persistence_object_count = 0;
    std::size_t persistence_member_count = 0;

    const auto insert_type =
        [&](identity_ref identity) -> status {

        if (!identity ||
            identity.kind() != identity_kind::type ||
            runtime.type_slots.empty() ||
            local_type_position >= runtime.local_types.size()) {
            return {status_code::invalid_argument};
        }

        const auto parent = identities.parent(identity);
        const auto name = identities.name(identity);

        if (!parent || !name)
            return {status_code::invalid_argument};

        const auto mask =
            runtime.type_slots.size() - 1;

        auto position =
            static_cast<std::size_t>(
                binding_hash(parent, name)) &
            mask;

        for (;;) {
            auto& slot = runtime.type_slots[position];

            if (!slot.identity) {
                slot = {
                    parent,
                    name,
                    identity,
                };

                runtime.local_types[
                    local_type_position++] =
                        identity;

                ++persistence_type_count;
                return {};
            }

            if (slot.parent == parent &&
                slot.name == name) {
                return slot.identity == identity
                    ? status{}
                    : status{
                        status_code::semantic_conflict};
            }

            position = (position + 1) & mask;
        }
    };

    for (const auto& record : facts.records()) {
        result = insert_type(record.identity);
        if (!result.ok())
            return result;
    }

    for (const auto& enum_fact : facts.enums()) {
        result = insert_type(enum_fact.identity);
        if (!result.ok())
            return result;
    }

    if (local_type_position !=
        runtime.local_types.size()) {
        return {status_code::initialization_failed};
    }

    if (!runtime.object_slots.empty()) {
        const auto mask =
            runtime.object_slots.size() - 1;

        for (const auto& object : facts.objects()) {
            if (!object.identity ||
                object.identity.kind() !=
                    identity_kind::object) {
                return {status_code::invalid_argument};
            }

            const auto parent =
                identities.parent(object.identity);
            const auto name =
                identities.name(object.identity);

            if (!parent || !name)
                return {status_code::invalid_argument};

            auto position =
                static_cast<std::size_t>(
                    binding_hash(parent, name)) &
                mask;

            const auto named_type =
                object.type.identity &&
                object.type.modifiers.count == 0
                ? object.type.identity
                : identity_ref{};

            for (;;) {
                auto& slot =
                    runtime.object_slots[position];

                if (!slot.identity) {
                    slot = {
                        parent,
                        name,
                        object.identity,
                        named_type,
                    };
                    ++persistence_object_count;
                    break;
                }

                if (slot.parent == parent &&
                    slot.name == name) {
                    if (slot.identity !=
                            object.identity ||
                        slot.named_type !=
                            named_type) {
                        return {
                            status_code::semantic_conflict};
                    }
                    break;
                }

                position = (position + 1) & mask;
            }
        }
    }

    if (!runtime.member_slots.empty()) {
        const auto mask =
            runtime.member_slots.size() - 1;

        for (const auto& record : facts.records()) {
            if (record.declaration_kind !=
                source_record_declaration_kind::definition) {
                continue;
            }

            if (!record.identity ||
                record.identity.kind() !=
                    identity_kind::type ||
                record.members.begin >
                    facts.members().size() ||
                record.members.count >
                    facts.members().size() -
                        record.members.begin) {
                return {status_code::invalid_argument};
            }

            for (std::uint32_t index = 0;
                 index < record.members.count;
                 ++index) {

                const auto& member =
                    facts.members()[
                        record.members.begin + index];

                if (!member.name)
                    return {status_code::invalid_argument};

                auto position =
                    static_cast<std::size_t>(
                        binding_hash(
                            record.identity,
                            member.name)) &
                    mask;

                for (;;) {
                    auto& slot =
                        runtime.member_slots[position];

                    if (!slot.type) {
                        slot = {
                            record.identity,
                            member.name,
                            member_index::from_zero_based(
                                index),
                        };
                        ++persistence_member_count;
                        break;
                    }

                    if (slot.type == record.identity &&
                        slot.name == member.name) {
                        return {
                            status_code::semantic_conflict};
                    }

                    position = (position + 1) & mask;
                }
            }
        }
    }

    if (runtime.imports.size() != imports.size())
        return {status_code::initialization_failed};

    for (std::size_t index = 0;
         index < imports.size();
         ++index) {

        if (imports[index] == nullptr)
            return {status_code::invalid_argument};

        runtime.imports[index] = imports[index];
    }

    external_runtime_data = {
        runtime.local_types,
        runtime.type_slots,
        runtime.object_slots,
        runtime.member_slots,
        std::span<const source_interface* const>{
            runtime.imports.data(),
            runtime.imports.size()},
    };

    runtime_externalized_state = true;

    persistence_local_type_count =
        runtime.local_types.size();
    persistence_type_slot_count =
        persistence_type_count;
    persistence_object_slot_count =
        persistence_object_count;
    persistence_member_slot_count =
        persistence_member_count;
    persistence_compact_state = false;

    external_persistence_data = {};
    persistence_externalized_state = false;
    return {};
}

status source_interface::initialize_persisted(
    const build_cache_image_view& cache,
    source_id source,
    std::span<const source_interface* const> imports) noexcept {

    if (!source)
        return {status_code::invalid_argument};

    build_cache_source_record persisted;
    auto result = cache.source(source, persisted);
    if (!result.ok())
        return result;
    if (!persisted.frontend_present)
        return {status_code::not_found};

    try {
        const auto type_capacity =
            capacity_for(persisted.type_slots.count);
        const auto object_capacity =
            capacity_for(persisted.object_slots.count);
        const auto member_capacity =
            capacity_for(persisted.member_slots.count);

        if (type_capacity == 0 ||
            object_capacity == 0 ||
            member_capacity == 0) {
            return {status_code::not_available};
        }

        std::vector<identity_ref> new_types(
            persisted.local_types.count);
        std::vector<type_slot> new_type_slots(type_capacity);
        std::vector<object_slot> new_object_slots(object_capacity);
        std::vector<member_slot> new_member_slots(member_capacity);

        std::vector<type_slot> new_persistence_type_slots;
        std::vector<object_slot> new_persistence_object_slots;
        std::vector<member_slot> new_persistence_member_slots;
        std::vector<const source_interface*> new_imports;

        new_persistence_type_slots.reserve(
            persisted.type_slots.count);
        new_persistence_object_slots.reserve(
            persisted.object_slots.count);
        new_persistence_member_slots.reserve(
            persisted.member_slots.count);
        new_imports.reserve(imports.size());

        for (std::size_t index = 0; index < new_types.size(); ++index) {
            result = cache.frontend_local_type(
                source,
                index,
                new_types[index]);
            if (!result.ok() || !new_types[index])
                return {status_code::artifact_corrupt};
        }

        const auto type_mask = new_type_slots.size() - 1;
        for (std::size_t index = 0;
             index < persisted.type_slots.count;
             ++index) {

            type_slot value;
            result = cache.frontend_type_slot(
                source,
                index,
                value);
            if (!result.ok() ||
                !value.parent ||
                !value.name ||
                !value.identity) {
                return {status_code::artifact_corrupt};
            }

            auto position =
                static_cast<std::size_t>(
                    binding_hash(value.parent, value.name)) &
                type_mask;

            for (;;) {
                auto& slot = new_type_slots[position];
                if (!slot.identity) {
                    slot = value;
                    new_persistence_type_slots.push_back(value);
                    break;
                }

                if (slot.parent == value.parent &&
                    slot.name == value.name) {
                    return {status_code::artifact_corrupt};
                }

                position = (position + 1) & type_mask;
            }
        }

        const auto object_mask = new_object_slots.size() - 1;
        for (std::size_t index = 0;
             index < persisted.object_slots.count;
             ++index) {

            object_slot value;
            result = cache.frontend_object_slot(
                source,
                index,
                value);
            if (!result.ok() ||
                !value.parent ||
                !value.name ||
                !value.identity) {
                return {status_code::artifact_corrupt};
            }

            auto position =
                static_cast<std::size_t>(
                    binding_hash(value.parent, value.name)) &
                object_mask;

            for (;;) {
                auto& slot = new_object_slots[position];
                if (!slot.identity) {
                    slot = value;
                    new_persistence_object_slots.push_back(value);
                    break;
                }

                if (slot.parent == value.parent &&
                    slot.name == value.name) {
                    return {status_code::artifact_corrupt};
                }

                position = (position + 1) & object_mask;
            }
        }

        const auto member_mask = new_member_slots.size() - 1;
        for (std::size_t index = 0;
             index < persisted.member_slots.count;
             ++index) {

            member_slot value;
            result = cache.frontend_member_slot(
                source,
                index,
                value);
            if (!result.ok() ||
                !value.type ||
                !value.name ||
                !value.index) {
                return {status_code::artifact_corrupt};
            }

            auto position =
                static_cast<std::size_t>(
                    binding_hash(value.type, value.name)) &
                member_mask;

            for (;;) {
                auto& slot = new_member_slots[position];
                if (!slot.type) {
                    slot = value;
                    new_persistence_member_slots.push_back(value);
                    break;
                }

                if (slot.type == value.type &&
                    slot.name == value.name) {
                    return {status_code::artifact_corrupt};
                }

                position = (position + 1) & member_mask;
            }
        }

        for (const auto* imported : imports) {
            if (imported == nullptr)
                return {status_code::invalid_argument};
            new_imports.push_back(imported);
        }

        local_type_values.swap(new_types);
        type_slots.swap(new_type_slots);
        object_slots.swap(new_object_slots);
        member_slots.swap(new_member_slots);

        persistence_type_slots.swap(
            new_persistence_type_slots);
        persistence_object_slots.swap(
            new_persistence_object_slots);
        persistence_member_slots.swap(
            new_persistence_member_slots);

        persistence_local_type_count =
            local_type_values.size();
        persistence_type_slot_count =
            persistence_type_slots.size();
        persistence_object_slot_count =
            persistence_object_slots.size();
        persistence_member_slot_count =
            persistence_member_slots.size();
        persistence_compact_state = true;

        external_persistence_data = {};
        persistence_externalized_state = false;
        imported_interfaces.swap(new_imports);
        return {};
    }
    catch (const std::bad_alloc&) {
        return {status_code::not_available};
    }
    catch (const std::length_error&) {
        return {status_code::not_available};
    }
}

source_interface_owned_persistence_data
source_interface::release_persistence_data() noexcept {

    source_interface_owned_persistence_data output;
    if (persistence_externalized_state)
        return output;

    output.local_types = std::move(local_type_values);
    output.type_slots = std::move(persistence_type_slots);
    output.object_slots = std::move(persistence_object_slots);
    output.member_slots = std::move(persistence_member_slots);

    external_persistence_data = {};
    return output;
}

void source_interface::bind_persistence_data(
    source_interface_data_view data) noexcept {

    external_persistence_data = data;
    persistence_externalized_state = true;
}


identity_ref source_interface::find_type(
    identity_ref scope,
    string_id name) const noexcept {

    return find_type_recursive(scope, name, 0);
}

identity_ref source_interface::find_type_recursive(
    identity_ref scope,
    string_id name,
    std::uint32_t depth) const noexcept {

    if (!scope || !name || depth > 1024)
        return {};

    const auto runtime = runtime_data_view();
    const auto slots = runtime.type_slots;

    if (!slots.empty()) {
        const auto mask = slots.size() - 1;
        auto position =
            static_cast<std::size_t>(binding_hash(scope, name)) & mask;

        for (std::size_t probe = 0; probe < slots.size(); ++probe) {
            const auto& slot = slots[position];
            if (!slot.identity)
                break;

            if (slot.parent == scope && slot.name == name)
                return slot.identity;

            position = (position + 1) & mask;
        }
    }

    const auto imports = runtime_imports();
    for (auto iterator = imports.rbegin();
         iterator != imports.rend();
         ++iterator) {

        if (const auto identity =
                (*iterator)->find_type_recursive(scope, name, depth + 1);
            identity) {
            return identity;
        }
    }

    return {};
}

source_interface_object source_interface::find_object(
    identity_ref scope,
    string_id name) const noexcept {

    return find_object_recursive(scope, name, 0);
}

source_interface_object source_interface::find_object_recursive(
    identity_ref scope,
    string_id name,
    std::uint32_t depth) const noexcept {

    if (!scope || !name || depth > 1024)
        return {};

    const auto runtime = runtime_data_view();
    const auto slots = runtime.object_slots;

    if (!slots.empty()) {
        const auto mask = slots.size() - 1;
        auto position =
            static_cast<std::size_t>(binding_hash(scope, name)) & mask;

        for (std::size_t probe = 0; probe < slots.size(); ++probe) {
            const auto& slot = slots[position];
            if (!slot.identity)
                break;

            if (slot.parent == scope && slot.name == name)
                return {slot.identity, slot.named_type};

            position = (position + 1) & mask;
        }
    }

    const auto imports = runtime_imports();
    for (auto iterator = imports.rbegin();
         iterator != imports.rend();
         ++iterator) {

        const auto found =
            (*iterator)->find_object_recursive(scope, name, depth + 1);
        if (found.identity)
            return found;
    }

    return {};
}

member_index source_interface::find_member(
    identity_ref type,
    string_id name) const noexcept {

    return find_member_recursive(type, name, 0);
}

member_index source_interface::find_member_recursive(
    identity_ref type,
    string_id name,
    std::uint32_t depth) const noexcept {

    if (!type || !name || depth > 1024)
        return {};

    const auto runtime = runtime_data_view();
    const auto slots = runtime.member_slots;

    if (!slots.empty()) {
        const auto mask = slots.size() - 1;
        auto position =
            static_cast<std::size_t>(binding_hash(type, name)) & mask;

        for (std::size_t probe = 0; probe < slots.size(); ++probe) {
            const auto& slot = slots[position];
            if (!slot.type)
                break;

            if (slot.type == type && slot.name == name)
                return slot.index;

            position = (position + 1) & mask;
        }
    }

    const auto imports = runtime_imports();
    for (auto iterator = imports.rbegin();
         iterator != imports.rend();
         ++iterator) {

        const auto found =
            (*iterator)->find_member_recursive(type, name, depth + 1);
        if (found)
            return found;
    }

    return {};
}

identity_ref source_environment::find_type(
    identity_ref scope,
    string_id name,
    std::uint32_t source_offset) const noexcept {

    for (auto iterator = imports.rbegin(); iterator != imports.rend(); ++iterator) {
        if (iterator->visible_from > source_offset || iterator->interface == nullptr)
            continue;

        if (const auto identity = iterator->interface->find_type(scope, name);
            identity) {
            return identity;
        }
    }

    return {};
}

source_interface_object source_environment::find_object(
    identity_ref scope,
    string_id name,
    std::uint32_t source_offset) const noexcept {

    for (auto iterator = imports.rbegin(); iterator != imports.rend(); ++iterator) {
        if (iterator->visible_from > source_offset || iterator->interface == nullptr)
            continue;

        const auto found = iterator->interface->find_object(scope, name);
        if (found.identity)
            return found;
    }

    return {};
}

member_index source_environment::find_member(
    identity_ref type,
    string_id name,
    std::uint32_t source_offset) const noexcept {

    for (auto iterator = imports.rbegin(); iterator != imports.rend(); ++iterator) {
        if (iterator->visible_from > source_offset || iterator->interface == nullptr)
            continue;

        const auto found = iterator->interface->find_member(type, name);
        if (found)
            return found;
    }

    return {};
}

} // namespace cw::server
