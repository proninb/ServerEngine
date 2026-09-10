#include "project_configuration_loader.hpp"

#include "../config/configuration_path.hpp"
#include "../diagnostics/diagnostic_descriptor.hpp"
#include "../json/json_parser.hpp"

#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace cw::server {
namespace {

enum class context : std::uint8_t {
    root,
    items,
    item,
    configuration,
    abi,
    invalid,
};

enum class field : std::uint8_t {
    none,
    version,
    name,
    project,
    configuration,
    path,
    role,
    abi,
    target,
    pack,
    unknown,
};

enum class schema_failure : std::uint8_t {
    none,
    wrong_root_type,
    unknown_property,
    duplicate_property,
    wrong_field_type,
    missing_required_field,
    invalid_name,
    invalid_path,
    invalid_role,
    invalid_target,
    invalid_pack,
    nesting_too_deep,
};

struct schema_error {
    schema_failure code = schema_failure::none;
    context owner = context::invalid;
    field member = field::none;
    std::size_t offset = 0;
};

[[nodiscard]] constexpr std::string_view failure_detail(const schema_error& error) noexcept {
    if (error.code == schema_failure::wrong_root_type)
        return "project configuration root must be an object";
    if (error.code == schema_failure::unknown_property)
        return "project configuration contains an unknown property";
    if (error.code == schema_failure::duplicate_property)
        return "project configuration contains a duplicate property";
    if (error.code == schema_failure::nesting_too_deep)
        return "project configuration nesting is too deep";
    if (error.owner == context::root && error.member == field::name)
        return "name: expected a non-empty string";
    if (error.owner == context::item && error.member == field::path)
        return "project[].path: expected a non-empty UTF-8 path string";
    if (error.owner == context::item && error.member == field::role)
        return "project[].role: expected type, source, or project";
    if (error.owner == context::abi && error.member == field::target)
        return "configuration.abi.target: expected windows-x64 or posix-x64";
    if (error.owner == context::abi && error.member == field::pack)
        return "configuration.abi.pack: expected 1, 2, 4, 8, or 16";
    if (error.code == schema_failure::missing_required_field)
        return "project configuration is missing a required field";
    if (error.code == schema_failure::wrong_field_type)
        return "project configuration field has the wrong JSON type";
    return "project configuration is invalid";
}

class project_configuration_handler final : public json_event_handler {
public:
    explicit project_configuration_handler(const std::filesystem::path& configuration_path) noexcept
        : configuration_path(configuration_path) {}

    void location(std::size_t offset) noexcept override {
        current_offset = offset;
    }

    void object_begin() noexcept override {
        if (stopped()) return;
        if (depth == 0) {
            root_object = true;
            push(context::root);
            return;
        }

        context next = context::invalid;
        if (current() == context::items) next = context::item;
        else if (current() == context::root && pending == field::configuration) next = context::configuration;
        else if (current() == context::configuration && pending == field::abi) next = context::abi;

        if (next == context::invalid) {
            fail(schema_failure::wrong_field_type);
            return;
        }

        pending = field::none;
        push(next);
        if (next == context::item) {
            item = {};
            item_path.clear();
            item_seen = 0;
        }
    }

    void object_end() noexcept override {
        if (stopped()) return;
        if (depth == 0) {
            fail(schema_failure::wrong_field_type);
            return;
        }
        const auto ending = current();
        validate_required(ending);
        if (stopped()) return;
        if (ending == context::item) finish_item();
        if (!stopped()) pop();
    }

    void array_begin() noexcept override {
        if (stopped()) return;
        if (depth == 0) {
            fail(schema_failure::wrong_root_type);
            return;
        }
        if (current() != context::root || pending != field::project) {
            fail(schema_failure::wrong_field_type);
            return;
        }
        pending = field::none;
        push(context::items);
    }

    void array_end() noexcept override {
        if (stopped()) return;
        if (depth == 0 || current() != context::items) {
            fail(schema_failure::wrong_field_type);
            return;
        }
        pop();
    }

    void key(std::string_view value) noexcept override {
        if (stopped()) return;
        if (depth == 0) {
            fail(schema_failure::wrong_field_type);
            return;
        }
        pending = identify(current(), value);
        if (pending == field::unknown) {
            fail(schema_failure::unknown_property);
            return;
        }
        auto& seen = seen_mask(current());
        const auto bit = field_bit(pending);
        if ((seen & bit) != 0) {
            fail(schema_failure::duplicate_property);
            return;
        }
        seen |= bit;
    }

    void string(std::string_view value) noexcept override {
        if (stopped()) return;
        if (depth == 0) {
            fail(schema_failure::wrong_root_type);
            return;
        }

        try {
            if (current() == context::root && pending == field::name) {
                if (value.empty()) fail(schema_failure::invalid_name, field::name);
                else candidate.name.assign(value);
            }
            else if (current() == context::item && pending == field::path) {
                if (value.empty()) fail(schema_failure::invalid_path, field::path);
                else item_path.assign(value);
            }
            else if (current() == context::item && pending == field::role) {
                if (value == "type") item.role = project_item_role::type;
                else if (value == "source") item.role = project_item_role::source;
                else if (value == "project") item.role = project_item_role::project;
                else fail(schema_failure::invalid_role, field::role);
            }
            else if (current() == context::abi && pending == field::target) {
                if (value == "windows-x64") candidate.abi.target = abi_target::windows_x64;
                else if (value == "posix-x64") candidate.abi.target = abi_target::posix_x64;
                else fail(schema_failure::invalid_target, field::target);
            }
            else {
                fail(schema_failure::wrong_field_type);
            }
        }
        catch (const std::bad_alloc&) {
            internal_failure = true;
        }
        catch (const std::length_error&) {
            internal_failure = true;
        }
        if (!stopped()) pending = field::none;
    }

    void integer(std::int64_t value) noexcept override {
        if (stopped()) return;
        if (depth == 0) {
            fail(schema_failure::wrong_root_type);
            return;
        }
        if (current() == context::root && pending == field::version) {
            if (value < 0 || value > std::numeric_limits<std::uint32_t>::max())
                fail(schema_failure::wrong_field_type);
            else
                candidate.version = static_cast<std::uint32_t>(value);
        }
        else if (current() == context::abi && pending == field::pack) {
            if (value < 0 || value > std::numeric_limits<std::uint32_t>::max() ||
                !is_supported_abi_pack(static_cast<std::uint32_t>(value))) {
                fail(schema_failure::invalid_pack, field::pack);
            }
            else {
                candidate.abi.pack = static_cast<std::uint32_t>(value);
            }
        }
        else {
            fail(schema_failure::wrong_field_type);
        }
        if (!stopped()) pending = field::none;
    }

    void number(double) noexcept override { reject_scalar(); }
    void boolean(bool) noexcept override { reject_scalar(); }
    void null() noexcept override { reject_scalar(); }

    [[nodiscard]] bool valid() const noexcept {
        return root_object && depth == 0 && failure.code == schema_failure::none && !internal_failure;
    }

    [[nodiscard]] bool failed_internally() const noexcept { return internal_failure; }
    [[nodiscard]] const schema_error& error() const noexcept { return failure; }
    [[nodiscard]] project_configuration take() { return std::move(candidate); }

private:
    [[nodiscard]] static field identify(context owner, std::string_view value) noexcept {
        if (owner == context::root) {
            if (value == "version") return field::version;
            if (value == "name") return field::name;
            if (value == "project") return field::project;
            if (value == "configuration") return field::configuration;
        }
        else if (owner == context::item) {
            if (value == "path") return field::path;
            if (value == "role") return field::role;
        }
        else if (owner == context::configuration) {
            if (value == "abi") return field::abi;
        }
        else if (owner == context::abi) {
            if (value == "target") return field::target;
            if (value == "pack") return field::pack;
        }
        return field::unknown;
    }

    [[nodiscard]] static constexpr std::uint32_t field_bit(field value) noexcept {
        return std::uint32_t{1} << static_cast<std::uint8_t>(value);
    }

    [[nodiscard]] std::uint32_t& seen_mask(context owner) noexcept {
        switch (owner) {
        case context::root: return root_seen;
        case context::item: return item_seen;
        case context::configuration: return configuration_seen;
        case context::abi: return abi_seen;
        default: return invalid_seen;
        }
    }

    void validate_required(context owner) noexcept {
        switch (owner) {
        case context::root:
            require(root_seen, field::version);
            require(root_seen, field::name);
            require(root_seen, field::project);
            require(root_seen, field::configuration);
            break;
        case context::item:
            require(item_seen, field::path);
            require(item_seen, field::role);
            break;
        case context::configuration:
            require(configuration_seen, field::abi);
            break;
        case context::abi:
            require(abi_seen, field::target);
            require(abi_seen, field::pack);
            break;
        default:
            break;
        }
    }

    void require(std::uint32_t actual, field member) noexcept {
        if ((actual & field_bit(member)) == 0)
            fail(schema_failure::missing_required_field, member);
    }

    void finish_item() noexcept {
        try {
            item.path = resolve_configuration_path(configuration_path, item_path);
            candidate.project.push_back(std::move(item));
        }
        catch (const std::bad_alloc&) {
            internal_failure = true;
        }
        catch (const std::length_error&) {
            internal_failure = true;
        }
        catch (...) {
            internal_failure = true;
        }
    }

    void reject_scalar() noexcept {
        if (depth == 0) fail(schema_failure::wrong_root_type);
        else fail(schema_failure::wrong_field_type);
    }

    [[nodiscard]] context current() const noexcept {
        return depth == 0 ? context::invalid : stack[depth - 1];
    }

    [[nodiscard]] bool stopped() const noexcept {
        return failure.code != schema_failure::none || internal_failure;
    }

    void push(context value) noexcept {
        if (depth == stack.size()) {
            fail(schema_failure::nesting_too_deep);
            return;
        }
        stack[depth++] = value;
    }

    void pop() noexcept {
        if (depth > 0) --depth;
    }

    void fail(
        schema_failure code,
        field member = field::none,
        context owner = context::invalid,
        std::size_t offset = std::numeric_limits<std::size_t>::max()) noexcept {
        if (failure.code != schema_failure::none) return;
        failure = {
            code,
            owner == context::invalid ? current() : owner,
            member == field::none ? pending : member,
            offset == std::numeric_limits<std::size_t>::max() ? current_offset : offset,
        };
    }

    const std::filesystem::path& configuration_path;
    project_configuration candidate;
    project_item_configuration item;
    std::string item_path;
    std::array<context, 8> stack{};
    std::size_t depth = 0;
    std::size_t current_offset = 0;
    field pending = field::none;
    schema_error failure;
    std::uint32_t root_seen = 0;
    std::uint32_t item_seen = 0;
    std::uint32_t configuration_seen = 0;
    std::uint32_t abi_seen = 0;
    std::uint32_t invalid_seen = 0;
    bool root_object = false;
    bool internal_failure = false;
};

void emit(
    diagnostic_buffer& diagnostics,
    const diagnostic_descriptor& descriptor,
    operation_id operation,
    std::size_t offset,
    std::string detail = {}) {
    diagnostics.emit({
        descriptor.id,
        descriptor.default_severity,
        operation,
        {source_id{}, static_cast<std::uint32_t>(
            offset > std::numeric_limits<std::uint32_t>::max() ? 0 : offset), 0},
        std::move(detail),
    });
}

[[nodiscard]] bool read_text_file(const std::filesystem::path& path, std::string& output) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) return false;
    output.assign(std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{});
    return stream.good() || stream.eof();
}

} // namespace

status load_project_configuration(
    std::string_view text,
    const std::filesystem::path& configuration_path,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    project_configuration& output) noexcept {
    try {
        project_configuration_handler handler{configuration_path};
        const auto parsed = parse_json(text, handler);
        if (!parsed.ok()) {
            const auto& descriptor = parsed.code == json_error_code::internal_failure
                ? diagnostics::project_initialization_failed
                : diagnostics::project_invalid_json;
            emit(diagnostics, descriptor, operation, parsed.offset);
            return {parsed.code == json_error_code::internal_failure
                ? status_code::initialization_failed
                : status_code::configuration_failed};
        }
        if (handler.failed_internally()) {
            emit(diagnostics, diagnostics::project_initialization_failed, operation, 0);
            return {status_code::initialization_failed};
        }
        if (!handler.valid()) {
            emit(diagnostics, diagnostics::project_invalid_configuration, operation,
                 handler.error().offset, std::string{failure_detail(handler.error())});
            return {status_code::configuration_failed};
        }

        auto candidate = handler.take();
        if (candidate.version != current_project_configuration_version) {
            emit(diagnostics, diagnostics::project_unsupported_configuration_version, operation, 0);
            return {status_code::configuration_failed};
        }
        output = std::move(candidate);
        return {};
    }
    catch (const std::bad_alloc&) {
        emit(diagnostics, diagnostics::project_initialization_failed, operation, 0);
        return {status_code::initialization_failed};
    }
    catch (...) {
        emit(diagnostics, diagnostics::project_initialization_failed, operation, 0);
        return {status_code::initialization_failed};
    }
}

status load_project_configuration_file(
    const std::filesystem::path& configuration_path,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    project_configuration& output) noexcept {
    try {
        std::string text;
        if (!read_text_file(configuration_path, text)) {
            emit(diagnostics, diagnostics::project_configuration_read_failed, operation, 0);
            return {status_code::io_failed};
        }
        return load_project_configuration(text, configuration_path, operation, diagnostics, output);
    }
    catch (...) {
        emit(diagnostics, diagnostics::project_configuration_read_failed, operation, 0);
        return {status_code::io_failed};
    }
}

} // namespace cw::server
