#include "project/runtime/managed_runtime.hpp"
#include "project/project_manager.hpp"
#include <chrono>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace cw::server;
namespace {
void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
struct fixture {
    std::filesystem::path directory = std::filesystem::temp_directory_path() /
        ("server_engine_managed_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    project_manager manager;
    project_access access;
    fixture() {
        std::filesystem::create_directories(directory);
        std::ofstream(directory / "project.json") << R"({"version":1,"name":"Managed","project":[{"path":"model.hpp","role":"type"}],"configuration":{"abi":{"target":"windows-x64","pack":8}}})";
    }
    ~fixture() {
        access.reset(); (void)manager.unload();
        std::error_code error; std::filesystem::remove_all(directory, error);
    }
    void source(const std::string& text) { std::ofstream(directory / "model.hpp") << text; }
    bool build(const std::string& text) {
        source(text);
        diagnostic_buffer diagnostics; project_build_result result;
        if (!manager.rebuild(directory / "project.json", operation_id{1}, diagnostics, result, 1).ok()) return false;
        return manager.acquire(access).ok();
    }
    object_endpoint endpoint(const char* name) {
        object_endpoint out; require(access->find_endpoint(name, out).ok(), name); return out;
    }
    void save_load() {
        access.reset(); baseline_commit_result saved;
        const auto saved_status = manager.save(saved);
        if (!saved_status.ok()) std::cerr << "save status " << static_cast<int>(saved_status.code)
            << " compiled " << saved.telemetry.generation_freeze_compiled_ns
            << " cache " << saved.telemetry.generation_freeze_build_cache_ns
            << " verify " << saved.telemetry.generation_freeze_verify_build_cache_ns << '\n';
        require(saved_status.ok(), "save");
        require(manager.unload().ok(), "unload");
        diagnostic_buffer diagnostics; project_load_result loaded;
        require(manager.load(directory / "project.json", operation_id{2}, diagnostics, loaded).ok(), "load");
        require(manager.acquire(access).ok(), "acquire loaded");
    }
};
std::int64_t integer(managed_runtime& runtime, object_endpoint endpoint) {
    std::int64_t value = -999; require(runtime.read_integer(endpoint, value).ok(), "read integer"); return value;
}
void success_case() {
    fixture f;
    require(f.build("struct Device { int& IN; int OUT = 1; int ZERO; bool ON = true; double SCALE = 1.5;"
        " Device() : IN(OUT), OUT(5) { IN = OUT; OUT = 5; } };"
        "struct Tap { int IN; }; Device A; Device B{}; Tap C; B.IN = A.OUT; C.IN = B.IN;"), "build managed source");
    managed_runtime runtime; std::string error;
    require(runtime.construct(f.access.get(), error).ok(), error.c_str());
    auto a = f.endpoint("A.OUT"), b = f.endpoint("B.IN"), c = f.endpoint("C.IN");
    require(integer(runtime, a) == 5 && integer(runtime, b) == 5 && integer(runtime, c) == 5, "initial links");
    require(integer(runtime, f.endpoint("A.ZERO")) == 0, "default zero");
    require(runtime.aliases(a, b) && runtime.aliases(a, f.endpoint("A.IN")), "reference alias");
    require(runtime.write_integer(b, 17).ok() && integer(runtime, a) == 17 && integer(runtime, c) == 17, "propagation");
    require(integer(runtime, f.endpoint("B.OUT")) == 5, "object isolation");
    require(!runtime.write_integer(f.endpoint("A.ON"), 2).ok(), "bool range");
    double scale = 0;
    require(runtime.read_real(f.endpoint("A.SCALE"), scale).ok() && scale == 1.5, "real initializer");
    require(runtime.write_real(f.endpoint("A.SCALE"), 2.25).ok(), "real write");
    f.save_load();
    require(integer(runtime, a) == 17, "runtime owns storage after unload");
    require(runtime.construct(f.access.get(), error).ok(), error.c_str());
    require(integer(runtime, f.endpoint("A.OUT")) == 5 && runtime.aliases(f.endpoint("A.OUT"), f.endpoint("B.IN")), "persisted construction plan");

    f.access.reset(); require(f.manager.unload().ok(), "unload before build");
    f.source("struct Device { int& IN = OUT; int OUT = 9; }; Device A; Device B; B.IN = A.OUT;");
    diagnostic_buffer diagnostics; project_build_result built;
    require(f.manager.build(f.directory / "project.json", operation_id{3}, diagnostics, built, 1).ok(), "changed build");
    require(f.manager.acquire(f.access).ok(), "changed acquire");
    require(runtime.construct(f.access.get(), error).ok(), error.c_str());
    require(integer(runtime, f.endpoint("B.IN")) == 9, "changed initializer");
    f.save_load();
    require(runtime.construct(f.access.get(), error).ok() && integer(runtime, f.endpoint("B.IN")) == 9, "changed persisted plan");
}
void rejection_cases() {
    fixture previous;
    require(previous.build("struct S { int x = 42; }; S A;"), "previous runtime source");
    managed_runtime runtime; std::string error;
    require(runtime.construct(previous.access.get(), error).ok(), "previous runtime");
    const auto retained = previous.endpoint("A.x");
    for (const auto* source : {
        "struct S { int& x; }; S A;",
        "struct S { int& x = y; int& y = x; }; S A;",
        "struct S { int x; int y; }; S A; A.x = A.y; A.y = A.x;",
        "struct S { int x; double y; }; S A; A.x = A.y;",
        "struct S { signed char x = 128; }; S A;",
        "struct S { int x = call(); }; S A;",
        "struct S { int x[2]; }; S A;",
        "struct S { int x; }; S A{5};"
    }) {
        fixture f; require(f.build(source), source);
        require(!runtime.construct(f.access.get(), error).ok() && !error.empty(), source);
        require(integer(runtime, retained) == 42, "failed construction preserves runtime");
        f.save_load();
        require(!runtime.construct(f.access.get(), error).ok(), "loaded unsupported plan");
    }
    fixture f;
    require(!f.build("struct S { int& x; int a; int b; S() : x(a) { x = b; } }; S A;"), "conflicting bindings rejected");
}
void defaults_and_constant_update() {
    fixture f;
    const std::string first = "struct S { int x = 3; const int& view = x; int& input = x; S() = default; }; S A; S B; B.input = A.x;";
    require(f.build(first), "default constructor");
    managed_runtime runtime; std::string error;
    require(runtime.construct(f.access.get(), error).ok(), error.c_str());
    require(integer(runtime, f.endpoint("A.view")) == 3, "const reference default");
    require(!runtime.write_integer(f.endpoint("A.view"), 4).ok(), "const reference write rejected");
    f.save_load();
    f.access.reset(); require(f.manager.unload().ok(), "unload constant update");
    auto second = first; second[second.find("= 3") + 2] = '7'; f.source(second);
    diagnostic_buffer diagnostics; project_build_result built;
    require(f.manager.build(f.directory / "project.json", operation_id{4}, diagnostics, built, 1).ok(), "constant-only build");
    require(f.manager.acquire(f.access).ok(), "constant-only acquire");
    require(runtime.construct(f.access.get(), error).ok() && integer(runtime, f.endpoint("B.input")) == 7, "constant-only semantic change");
    f.save_load();
    require(runtime.construct(f.access.get(), error).ok() && integer(runtime, f.endpoint("B.input")) == 7, "constant-only persisted change");
}
void nested_aggregates() {
    fixture f;
    const std::string source =
        "struct A { int a = 12; int rest = 6; };"
        "struct B { A a; B() : a{0} {} };"
        "struct Pair { A left{2, 3}; A right; };"
        "struct Root { B first; Pair pair; A empty; int scalar;"
        " Root() : pair{{4, 5}, {7}}, empty{}, scalar{9} {} };"
        "struct Ref { int x = 21; int& r = x; };"
        "struct Holder { Ref one; Ref two; };"
        "struct ConstHolder { const A a{8}; };"
        "B b; Root root; Holder refs; ConstHolder fixed;";
    require(f.build(source), "nested aggregate build");
    managed_runtime runtime; std::string error;
    const std::array p0{member_index::from_zero_based(0)};
    const std::array p1{member_index::from_zero_based(1)};
    const std::array p00{member_index::from_zero_based(0), member_index::from_zero_based(0)};
    const std::array p01{member_index::from_zero_based(0), member_index::from_zero_based(1)};
    const std::array p10{member_index::from_zero_based(1), member_index::from_zero_based(0)};
    const std::array p11{member_index::from_zero_based(1), member_index::from_zero_based(1)};
    const auto check = [&](const char* endpoint, auto path, std::int64_t expected) {
        std::int64_t actual;
        require(runtime.read_integer(f.endpoint(endpoint), path, actual).ok() && actual == expected, endpoint);
    };
    for (int pass = 0; pass < 2; ++pass) {
        require(runtime.construct(f.access.get(), error).ok(), error.c_str());
        check("b.a", p0, 0); check("b.a", p1, 6);
        check("root.first", p00, 0); check("root.first", p01, 6);
        check("root.pair", p00, 4); check("root.pair", p01, 5);
        check("root.pair", p10, 7); check("root.pair", p11, 6);
        check("root.empty", p0, 12); check("root.empty", p1, 6);
        require(integer(runtime, f.endpoint("root.scalar")) == 9, "scalar braces remain supported");
        require(runtime.write_integer(f.endpoint("refs.one"), p1, 33).ok(), "write nested reference");
        check("refs.one", p0, 33); check("refs.two", p0, 21);
        check("fixed.a", p0, 8);
        require(!runtime.write_integer(f.endpoint("fixed.a"), p0, 1).ok(), "nested const write rejected");
        std::int64_t unchanged = 123;
        require(!runtime.read_integer(f.endpoint("b.a"), unchanged).ok() && unchanged == 123, "record is not a scalar");
        require(!runtime.read_integer(f.endpoint("b.a"), p00, unchanged).ok(), "invalid nested path");
        if (!pass) f.save_load();
    }
    f.access.reset(); require(f.manager.unload().ok(), "unload nested update");
    auto changed = source;
    changed.replace(changed.find("a{0}"), 4, "a{11}");
    f.source(changed);
    diagnostic_buffer diagnostics; project_build_result built;
    require(f.manager.build(f.directory / "project.json", operation_id{5}, diagnostics, built, 1).ok(), "nested initializer-only build");
    require(f.manager.acquire(f.access).ok(), "nested changed acquire");
    require(runtime.construct(f.access.get(), error).ok(), error.c_str());
    check("b.a", p0, 11); check("root.first", p00, 11);
    f.save_load();
    require(runtime.construct(f.access.get(), error).ok(), error.c_str());
    check("b.a", p0, 11); check("root.first", p00, 11);

    for (const auto* invalid : {
        "struct A { int a; }; struct B { A a; B() : a{1, 2} {} }; B b;",
        "struct A { int a; }; struct B { A a = 1; }; B b;",
        "struct A { int a; }; struct B { A a; B() : a{{1, 2}} {} }; B b;",
        "struct A { signed char a; }; struct B { A a{128}; }; B b;",
        "struct A { int a; int& r = a; }; struct B { A a{1, 2}; }; B b;"
    }) {
        fixture bad;
        require(bad.build(invalid), "build invalid aggregate shape");
        require(!runtime.construct(bad.access.get(), error).ok(), invalid);
        check("b.a", p0, 11); // Failed recursive construction is transactional.
        bad.save_load();
        require(!runtime.construct(bad.access.get(), error).ok(), "persisted invalid aggregate rejected");
    }
    fixture cycle;
    if (cycle.build("struct A { A a; }; A a;"))
        require(!runtime.construct(cycle.access.get(), error).ok(), "value containment cycle rejected");
}
}
int main() {
    try { success_case(); defaults_and_constant_update(); rejection_cases(); nested_aggregates(); std::cout << "managed runtime passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
