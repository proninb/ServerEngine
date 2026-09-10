#include "../server_engine/config/server_configuration_loader.hpp"
#include "../server_engine/project/project_configuration_loader.hpp"
#include "../server_engine/project/identity/identity_registry.hpp"
#include "../server_engine/project/string/string_registry.hpp"

#include <filesystem>
#include <string_view>

namespace {

using namespace cw::server;

bool test_server_configuration() {
    constexpr std::string_view text = R"({"version":1,"server":{"endpoints":[],"console":true},"logging":{"level":"info","console":true},"telemetry":{"metrics":true},"project":{"path":"project.json"}})";
    diagnostic_buffer diagnostics;
    server_configuration configuration;
    if (!load_server_configuration(text, "/tmp/server.json", operation_id{1}, diagnostics, configuration).ok())
        return false;
    return configuration.version == 1 && configuration.project.path.filename() == "project.json";
}

bool test_project_configuration() {
    constexpr std::string_view text = R"({"version":1,"name":"Example","project":[{"path":"types/a.hpp","role":"type"}],"configuration":{"abi":{"target":"windows-x64","pack":8}}})";
    diagnostic_buffer diagnostics;
    project_configuration configuration;
    if (!load_project_configuration(text, "/tmp/project.json", operation_id{2}, diagnostics, configuration).ok())
        return false;
    return configuration.name == "Example" && configuration.project.size() == 1 &&
           configuration.project.front().role == project_item_role::type;
}

bool test_identity_stability() {
    string_registry strings;
    identity_registry identities;

    string_id n_name;
    string_id a_name;
    if (!strings.intern("N", n_name).ok() || !strings.intern("A", a_name).ok()) return false;

    identity_ref n1 = nullptr;
    identity_ref n2 = nullptr;
    identity_ref a1 = nullptr;
    identity_ref a2 = nullptr;

    if (!identities.intern(identities.root(), n_name, identity_kind::namespace_scope, n1).ok()) return false;
    if (!identities.intern(identities.root(), n_name, identity_kind::namespace_scope, n2).ok()) return false;
    if (!identities.intern(n1, a_name, identity_kind::type, a1).ok()) return false;
    if (!identities.intern(n2, a_name, identity_kind::type, a2).ok()) return false;

    return n1 == n2 && a1 == a2 && a1->parent() == n1 && a1->name() == a_name;
}

} // namespace

int main() {
    return test_server_configuration() && test_project_configuration() && test_identity_stability() ? 0 : 1;
}
