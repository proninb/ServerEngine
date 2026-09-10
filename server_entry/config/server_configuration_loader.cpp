#include "server_configuration_loader.hpp"

#include "configuration_path.hpp"
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
    server,
    endpoints,
    endpoint,
    logging,
    telemetry,
    project,
    invalid,
};

enum class field : std::uint8_t {
    none,
    version,
    server,
    logging,
    telemetry,
    project,
    endpoints,
    console,
    name,
    transport,
    protocol,
    address,
    port,
    level,
    metrics,
    path,
    unknown,
};

enum class schema_failure : std::uint8_t {
    none,
    wrong_root_type,
    unknown_property,
    duplicate_property,
    wrong_field_type,
    missing_required_field,
    invalid_endpoint,
    duplicate_endpoint_name,
    invalid_log_level,
    invalid_project_path,
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
        return "server configuration root must be an object";
    if (error.code == schema_failure::unknown_property)
        return "server configuration contains an unknown property";
    if (error.code == schema_failure::duplicate_property)
        return "server configuration contains a duplicate property";
    if (error.code == schema_failure::duplicate_endpoint_name)
        return "server endpoint names must be unique";
    if (error.code == schema_failure::nesting_too_deep)
        return "server configuration nesting is too deep";

    if (error.owner == context::endpoint) {
        if (error.member == field::name)
            return "server.endpoints[].name: expected a non-empty unique string";
        if (error.member == field::transport)
            return "server.endpoints[].transport: expected \"tcp\"";
        if (error.member == field::protocol)
            return "server.endpoints[].protocol: expected \"json\"";
        if (error.member == field::address)
            return "server.endpoints[].address: expected a non-empty string";
        if (error.member == field::port)
            return "server.endpoints[].port: expected integer 1..65535";
    }
    if (error.owner == context::logging && error.member == field::level)
        return "logging.level: expected trace, debug, info, warning, error, or critical";
    if (error.owner == context::project && error.member == field::path)
        return "project.path: expected a non-empty UTF-8 path string";

    if (error.code == schema_failure::missing_required_field)
        return "server configuration is missing a required field";
    if (error.code == schema_failure::wrong_field_type)
        return "server configuration field has the wrong JSON type";
    if (error.code == schema_failure::invalid_endpoint)
        return "server endpoint configuration is invalid";
    if (error.code == schema_failure::invalid_log_level)
        return "logging.level is not supported";
    if (error.code == schema_failure::invalid_project_path)
        return "project.path must not be empty";
    return "server configuration is invalid";
}

class server_configuration_handler final : public json_event_handler {
public:
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
        if (current() == context::root) {
            if (pending == field::server) next = context::server;
            else if (pending == field::logging) next = context::logging;
            else if (pending == field::telemetry) next = context::telemetry;
            else if (pending == field::project) next = context::project;
        }
        else if (current() == context::endpoints) {
            next = context::endpoint;
        }

        if (next == context::invalid) {
            fail(schema_failure::wrong_field_type);
            return;
        }

        pending = field::none;
        push(next);
        if (next == context::endpoint) {
            endpoint = {};
            endpoint_seen = 0;
            endpoint_name_offset = current_offset;
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
        if (ending == context::endpoint) finish_endpoint();
        if (!stopped()) pop();
    }

    void array_begin() noexcept override {
        if (stopped()) return;
        if (depth == 0) {
            fail(schema_failure::wrong_root_type);
            return;
        }
        if (current() != context::server || pending != field::endpoints) {
            fail(schema_failure::wrong_field_type);
            return;
        }
        pending = field::none;
        push(context::endpoints);
    }

    void array_end() noexcept override {
        if (stopped()) return;
        if (depth == 0 || current() != context::endpoints) {
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
            if (current() == context::endpoint) {
                if (pending == field::name) {
                    endpoint_name_offset = current_offset;
                    if (value.empty()) fail(schema_failure::invalid_endpoint, field::name);
                    else endpoint.name.assign(value);
                }
                else if (pending == field::transport) {
                    if (value != "tcp") fail(schema_failure::invalid_endpoint, field::transport);
                    else endpoint.transport = transport_kind::tcp;
                }
                else if (pending == field::protocol) {
                    if (value != "json") fail(schema_failure::invalid_endpoint, field::protocol);
                    else endpoint.protocol = protocol_kind::json;
                }
                else if (pending == field::address) {
                    if (value.empty()) fail(schema_failure::invalid_endpoint, field::address);
                    else endpoint.address.assign(value);
                }
                else fail(schema_failure::wrong_field_type);
            }
            else if (current() == context::logging && pending == field::level) {
                if (!parse_level(value, candidate.logging.minimum_level))
                    fail(schema_failure::invalid_log_level, field::level);
            }
            else if (current() == context::project && pending == field::path) {
                if (value.empty()) fail(schema_failure::invalid_project_path, field::path);
                else project_path.assign(value);
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
        else if (current() == context::endpoint && pending == field::port) {
            if (value < 1 || value > 65535)
                fail(schema_failure::invalid_endpoint, field::port);
            else
                endpoint.port = static_cast<std::uint16_t>(value);
        }
        else {
            fail(schema_failure::wrong_field_type);
        }
        if (!stopped()) pending = field::none;
    }

    void number(double) noexcept override {
        reject_scalar();
    }

    void boolean(bool value) noexcept override {
        if (stopped()) return;
        if (depth == 0) {
            fail(schema_failure::wrong_root_type);
            return;
        }
        if (current() == context::server && pending == field::console)
            candidate.communication.console = value;
        else if (current() == context::logging && pending == field::console)
            candidate.logging.console = value;
        else if (current() == context::telemetry && pending == field::metrics)
            candidate.telemetry.metrics = value;
        else
            fail(schema_failure::wrong_field_type);
        if (!stopped()) pending = field::none;
    }

    void null() noexcept override {
        reject_scalar();
    }

    [[nodiscard]] bool valid() const noexcept {
        return root_object && depth == 0 && failure.code == schema_failure::none && !internal_failure;
    }

    [[nodiscard]] bool failed_internally() const noexcept {
        return internal_failure;
    }

    [[nodiscard]] const schema_error& error() const noexcept {
        return failure;
    }

    [[nodiscard]] const std::string& configured_project_path() const noexcept {
        return project_path;
    }

    [[nodiscard]] server_configuration take() {
        return std::move(candidate);
    }

private:
    [[nodiscard]] static bool parse_level(std::string_view value, log_level& output) noexcept {
        if (value == "trace") output = log_level::trace;
        else if (value == "debug") output = log_level::debug;
        else if (value == "info") output = log_level::info;
        else if (value == "warning") output = log_level::warning;
        else if (value == "error") output = log_level::error;
        else if (value == "critical") output = log_level::critical;
        else return false;
        return true;
    }

    [[nodiscard]] static field identify(context owner, std::string_view value) noexcept {
        if (owner == context::root) {
            if (value == "version") return field::version;
            if (value == "server") return field::server;
            if (value == "logging") return field::logging;
            if (value == "telemetry") return field::telemetry;
            if (value == "project") return field::project;
        }
        else if (owner == context::server) {
            if (value == "endpoints") return field::endpoints;
            if (value == "console") return field::console;
        }
        else if (owner == context::endpoint) {
            if (value == "name") return field::name;
            if (value == "transport") return field::transport;
            if (value == "protocol") return field::protocol;
            if (value == "address") return field::address;
            if (value == "port") return field::port;
        }
        else if (owner == context::logging) {
            if (value == "level") return field::level;
            if (value == "console") return field::console;
        }
        else if (owner == context::telemetry) {
            if (value == "metrics") return field::metrics;
        }
        else if (owner == context::project) {
            if (value == "path") return field::path;
        }
        return field::unknown;
    }

    [[nodiscard]] static constexpr std::uint32_t field_bit(field value) noexcept {
        return std::uint32_t{1} << static_cast<std::uint8_t>(value);
    }

    [[nodiscard]] std::uint32_t& seen_mask(context owner) noexcept {
        switch (owner) {
        case context::root: return root_seen;
        case context::server: return server_seen;
        case context::endpoint: return endpoint_seen;
        case context::logging: return logging_seen;
        case context::telemetry: return telemetry_seen;
        case context::project: return project_seen;
        default: return invalid_seen;
        }
    }

    void validate_required(context owner) noexcept {
        switch (owner) {
        case context::root:
            require(root_seen, field::version);
            require(root_seen, field::server);
            require(root_seen, field::logging);
            require(root_seen, field::telemetry);
            require(root_seen, field::project);
            break;
        case context::server:
            require(server_seen, field::endpoints);
            require(server_seen, field::console);
            break;
        case context::endpoint:
            require(endpoint_seen, field::name);
            require(endpoint_seen, field::transport);
            require(endpoint_seen, field::protocol);
            require(endpoint_seen, field::address);
            require(endpoint_seen, field::port);
            break;
        case context::logging:
            require(logging_seen, field::level);
            require(logging_seen, field::console);
            break;
        case context::telemetry:
            require(telemetry_seen, field::metrics);
            break;
        case context::project:
            require(project_seen, field::path);
            break;
        default:
            break;
        }
    }

    void require(std::uint32_t actual, field member) noexcept {
        if ((actual & field_bit(member)) == 0)
            fail(schema_failure::missing_required_field, member);
    }

    void finish_endpoint() noexcept {
        for (const auto& existing : candidate.communication.endpoints) {
            if (existing.name == endpoint.name) {
                fail(schema_failure::duplicate_endpoint_name, field::name,
                     context::endpoint, endpoint_name_offset);
                return;
            }
        }
        try {
            candidate.communication.endpoints.push_back(std::move(endpoint));
        }
        catch (const std::bad_alloc&) {
            internal_failure = true;
        }
        catch (const std::length_error&) {
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

    server_configuration candidate;
    endpoint_configuration endpoint;
    std::string project_path;
    std::array<context, 8> stack{};
    std::size_t depth = 0;
    std::size_t current_offset = 0;
    std::size_t endpoint_name_offset = 0;
    field pending = field::none;
    schema_error failure;
    std::uint32_t root_seen = 0;
    std::uint32_t server_seen = 0;
    std::uint32_t endpoint_seen = 0;
    std::uint32_t logging_seen = 0;
    std::uint32_t telemetry_seen = 0;
    std::uint32_t project_seen = 0;
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

status load_server_configuration(
    std::string_view text,
    const std::filesystem::path& configuration_path,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    server_configuration& output) noexcept {
    try {
        server_configuration_handler handler;
        const auto parsed = parse_json(text, handler);
        if (!parsed.ok()) {
            const auto& descriptor = parsed.code == json_error_code::internal_failure
                ? diagnostics::server_initialization_failed
                : diagnostics::server_invalid_json;
            emit(diagnostics, descriptor, operation, parsed.offset);
            return {parsed.code == json_error_code::internal_failure
                ? status_code::initialization_failed
                : status_code::configuration_failed};
        }
        if (handler.failed_internally()) {
            emit(diagnostics, diagnostics::server_initialization_failed, operation, 0);
            return {status_code::initialization_failed};
        }
        if (!handler.valid()) {
            emit(diagnostics, diagnostics::server_invalid_configuration, operation,
                 handler.error().offset, std::string{failure_detail(handler.error())});
            return {status_code::configuration_failed};
        }

        auto candidate = handler.take();
        if (candidate.version != current_server_configuration_version) {
            emit(diagnostics, diagnostics::server_unsupported_configuration_version, operation, 0);
            return {status_code::configuration_failed};
        }

        candidate.project.path = resolve_configuration_path(
            configuration_path, handler.configured_project_path());
        output = std::move(candidate);
        return {};
    }
    catch (const std::bad_alloc&) {
        emit(diagnostics, diagnostics::server_initialization_failed, operation, 0);
        return {status_code::initialization_failed};
    }
    catch (...) {
        emit(diagnostics, diagnostics::server_initialization_failed, operation, 0);
        return {status_code::initialization_failed};
    }
}

status load_server_configuration_file(
    const std::filesystem::path& configuration_path,
    operation_id operation,
    diagnostic_buffer& diagnostics,
    server_configuration& output) noexcept {
    try {
        std::string text;
        if (!read_text_file(configuration_path, text)) {
            emit(diagnostics, diagnostics::server_configuration_read_failed, operation, 0);
            return {status_code::io_failed};
        }
        return load_server_configuration(text, configuration_path, operation, diagnostics, output);
    }
    catch (...) {
        emit(diagnostics, diagnostics::server_configuration_read_failed, operation, 0);
        return {status_code::io_failed};
    }
}

} // namespace cw::server
