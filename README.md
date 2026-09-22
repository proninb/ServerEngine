# Server Engine

Clean Architecture V3 bootstrap for the Curtiss-Wright Server Engine project.

This repository reuses proven implementation mechanisms from the former `Server-Entry_OLD` project only where they preserve V3 contracts. V2 stable-ID/String-Registry identity, Builder canonicalization, Graph identity, Runtime, and SHM implementations are not carried forward.

## Current foundation

- centralized diagnostics;
- strict transactional `server.json` loader;
- strict transactional `project.json` loader;
- Windows x64 / POSIX x64 ABI configuration;
- C++20 JSON SAX parser used by configuration loading;
- Project Context;
- Project-lifetime `string_id` String Table for canonical text atoms;
- Project Context-owned semantic identity space using `(parent, string_id, kind)`;
- stable-address `identity_node` objects returning `identity_ref` directly;
- one current Graph containing Types, Objects, and Links;
- no numeric semantic `stable_id`;
- no `std::mutex` or `std::unordered_map` in Project semantic identity;
- CMake and Visual Studio 18 / v145 builds.

## SE-V3-02 semantic identity contract

`project_context` owns Project-lifetime semantic identity.

```cpp
status project_context::resolve_declaration(
    identity_ref parent,
    std::string_view local_name,
    identity_kind kind,
    identity_ref& identity) noexcept;
```

The operation returns the canonical `identity_ref` directly. The caller does not receive and must not depend on whether storage allocation occurred. There is no `created` flag.

```text
source-language semantic resolution
              │
              ▼
        identity_node*
              │
              ├── Parser/source facts
              ├── dependencies
              └── Generation Builder
```

`identity_node*` means WHO. Graph-local handles mean WHERE in the single current compiled Graph. The Server retains no historical Graph versions.

`identity_node` never contains a Graph pointer, Graph-local handle, definition state, ABI layout, members, or other compiled Graph state.

The identity backing arena owns stable Project-lifetime nodes only. Identifier bytes are canonicalized once by the Project String Table and represented thereafter by 32-bit `string_id`.

## Build

### Visual Studio

Open:

```text
ServerEngine.sln
```

The project targets Visual Studio 18 toolset `v145`, C++20, x64.

### CMake

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## Run

From the repository root:

```text
server_engine [server-configuration-path]
```

If no path is supplied, `server.json` is used.

Current bootstrap flow:

```text
server.json
    -> validate Server configuration
    -> project_manager.load(project.json)
    -> build detached Project end to end
    -> publish one current Project/Graph
    -> project_manager.unload() on unload/exit
```

SE-V3-06R replaces the minimal 06B frontend mechanics with production-oriented Source Manager acquisition, SHA-256 snapshots, Lexer directive spans, quoted-include DAG discovery, dependency-ready parallel Parser scheduling, positional visibility, and direct `identity_ref` source interfaces. SE-V3-06P separates filesystem normalization from hot normalized-path identity lookup. SE-V3-07 adds flat SourceContribution provenance, direct-identity full Graph construction, and sparse incremental Graph updates. SE-V3-08 connects the complete filesystem → frontend → Builder → current Graph path. SE-V3-09 adds the Project String Table, canonical Objects and Links, hierarchical textual query, and detached Project LOAD/UNLOAD ownership. Read access is scoped by `project_read_guard`; only the short no-fail publication/replacement boundary excludes readers, and sparse headroom exhaustion falls back to detached full rebuild without invalidating the current Project on failure.

The Server owns one current Graph only. There is no Graph generation counter, retained Graph history, or MVCC version-control layer. Full and incremental builds prepare all fallible work before a short no-fail publication boundary.

## Current capability boundary and next implementation sequence

Persistent baselines, compiled images, build-cache images, source change tracking,
and sparse builds from mapped baselines are implemented. `project_manager` exposes
LOAD, BUILD, REBUILD, SAVE, and UNLOAD; construction starts from UNLOADED. The CLI
currently runs REBUILD, prints metadata, and unloads; it is not a TCP service.

Managed initialization is available through `project/runtime/managed_runtime.hpp`
and `server_engine server.json --managed`. Fields default to zero. Decimal integer,
boolean and real constants and local reference bindings are normalized into Graph
construction facts, persisted in compiled image v3 / build-cache v6. Older baselines
require REBUILD. Source spellings and ordinary method bodies remain transient.

Run the included example with `server_engine examples/managed/server.json --managed`.

```cpp
struct Device {
    int& IN;
    int OUT = 5;
    Device() : IN(OUT) {} // alternatively: Device() { IN = OUT; }
};
Device A;
Device B{};
B.IN = A.OUT;
```

This is managed syntax: reference assignments in a no-argument constructor bind
fields; scalar assignments set initial values. The engine does not execute native
C++ constructors. `= default` and empty constructors use field defaults. Repeated
identical bindings are accepted; conflicting constructor bindings are rejected.
An external reference link overrides the local default binding. Reference links
alias storage; value links copy in dependency order at construction and after writes
through the runtime API. Cycles, type mismatches, unbound references and out-of-range
constants fail construction without changing the previous runtime.

Nested by-value records support recursive managed initialization:

```cpp
struct A { int a = 12; int other = 6; };
struct B {
    A a;
    B() : a{0} {} // a.a = 0; a.other keeps its default 6
};
struct Pair { A left; A right; };
struct C { Pair pair{{1, 2}, {3}}; };
```

Braced lists supply direct members in declaration order; omitted members keep their
managed defaults. Empty `{}` applies those defaults. Use explicit nested braces for
nested members; brace elision and designated initializers are unsupported. Lists
contain decimal integer/real constants, booleans, null pointers or nested lists.
They are normalized into interned canonical text stored in the compiled string
table, so runtime initialization does not depend on source files.

The runtime read/write overloads accept a root `object_endpoint` and a span of
zero-based `member_index` values. For `b.a.a`, resolve `b.a` through the project,
then pass a path containing index 0. Existing Graph link syntax still addresses
top-level fields; whole-record links and references to records are unsupported.
Local scalar reference bindings inside nested records work independently per object.
Recursive value containment and nesting deeper than 64 records fail construction.

The runtime owns its storage and plan after Project unload. Endpoint handles belong
to that plan; reacquire them when constructing from another project generation.
Scalar fields, nested records and scalar lvalue references are supported, plus zero scalar pointers.
Arrays, unions, inheritance, constructor arguments, nonempty object
initializers, expressions/calls and native method execution are outside this runtime
slice. Access managed values through the API, never by casting storage to a C++ class.

Initializers are checked for nonempty expressions and matching `()`, `[]`, `{}`;
expressions are not evaluated or fully type-checked. Member initializers use `=` or
braces. Parenthesized object initialization currently supports literal-led forms
(including signed numeric literals); other parenthesized declarations must parse
as functions. Unsupported expression-led forms should use braces.

Next: extend managed layouts to arrays and nested Graph endpoints, then add SHM publication
and external TCP/query control.

## Provenance

Configuration schemas, diagnostics concepts, status/operation/source identifiers, and strict loader behavior originate from the former `proninb/Server-Entry` foundation. V3 semantic identity is a new architecture.

## SE-V3-03 identity scale benchmark

Build `server_engine_identity_benchmark` and run `--gate` before attempting the full 10K/100K/1M matrix. The full benchmark intentionally uses a flat semantic scope so superlinear name-resolution behavior cannot be hidden by workload shape. See `docs/SE_V3_03_IDENTITY_SCALE_BENCHMARK.md`.


## SE-V3-04 Semantic Scope Index

Project semantic resolution uses one lock-free fixed bucket index instead of sibling-linear traversal. The 1M identity scale gate and concurrent arena memory-amplification gate must pass before Parser/Builder integration proceeds.

## SE-V3-05 Identity Index Hardening

`identity_space` exposes diagnostic-only collision-chain statistics without instrumenting the semantic hot path. Run `server_engine_identity_benchmark --hardening` to validate 1M sequential, patterned, same-prefix, and long identifiers, including exact canonical-pointer replay and structural collision/probe gates. After this gate passes, the Project semantic identity foundation is frozen and SE-V3-06 begins Source Manager + Parser integration.

## SE-V3-06A Source Facts Contract

The Parser -> Generation Builder boundary is now represented by immutable `source_facts`: direct Project `identity_ref` values for semantic entities, intrinsic Language/ABI codes for builtins, flat member/modifier arrays, and Source byte ranges for non-identity names and diagnostics. Unresolved names and numeric semantic IDs do not cross this boundary. From SE-V3-09 onward, non-identity textual atoms such as member/enumerator names cross as canonical `string_id` values. See `docs/SE_V3_06A_SOURCE_FACTS_CONTRACT.md`.

## SE-V3-06R Production Source Frontend Reuse

The live G0 Source frontend now adapts proven mechanisms from `Server-Entry_OLD`: immutable acquisition jobs, SHA-256 Source snapshots, compact lexical tokens/directive spans, include-DAG discovery, and dependency-ready semantic scheduling. V3 replaces OLD textual canonical-name/String-Registry flow with Project-lifetime `identity_ref` and a direct Parser-visible `source_interface`. The scheduler uses deterministic coordinator waves rather than the OLD mutex/condition-variable queues. See `docs/SE_V3_06R_PRODUCTION_REUSE.md`.

Source Manager scaling can be checked independently with:

```text
server_engine_source_manager_benchmark
```

The default gate resolves and commits 100K and 1M normalized path identities and rejects superlinear scaling above exponent 1.50.

## SE-V3-06P Source Manager Performance

Filesystem path normalization is now an explicit cold boundary. Repeated Source identity work uses `resolve_normalized()` / `find()` over a compact 8-byte open-addressing bucket, dense 8-byte path records, a contiguous path arena, and XXH64 with a folded 32-bit fingerprint. The performance benchmark compares V3 directly against both the actual OLD production `unordered_map<filesystem::path, source_id>` shape and the separate compact OLD layout-study candidate. See `docs/SE_V3_06P_SOURCE_MANAGER_PERFORMANCE.md`.

The benchmark must show V3 faster than the actual OLD production map for both normalized insertion and random lookup. The experimental OLD compact-layout candidate is retained as a stricter near-parity guard.

## SE-V3-07A+07B SourceContribution + Generation Builder G0

Build provenance is now retained outside Graph in a flat `SourceContribution[source_id]` representation. Generation Builder consumes Parser-resolved `identity_ref` values directly, assigns dense generation-local `type_handle` values, materializes compact TypeRefs and definition arenas into detached G0 storage, validates it, and publishes through no-fail swaps. No V2 stable-ID/String-Registry/name-resolution path exists in Builder. See `docs/SE_V3_07AB_GENERATION_BUILDER_G0.md`.

Run:

```text
server_engine_generation_builder_benchmark
server_engine_generation_builder_benchmark --semantic 1000000
```

The default gate requires one million real Project identities to materialize into G0 in under one second before publication.

## SE-V3-08 Project Build Orchestration

`project_context` now owns the Source Manager, persistent COLD Parser-interface cache, SourceContribution cache, and one current Graph. `project_build_orchestrator` performs full and incremental builds end to end. Incremental builds reacquire only dirty physical Sources, collect the committed reverse-dependent closure before replacing include edges, reparse dependents from committed snapshots without rereading them, and send only semantically changed SourceContributions to Generation Builder.

Run `server_engine_project_build_benchmark --gate` to exercise real filesystem full build, one-file incremental update, and a common-header fanout case. Sparse gates require zero Source-graph/path-index/Builder full scans on normal incremental updates. See `docs/SE_V3_08_PROJECT_BUILD_ORCHESTRATION.md`.


## SE-V3-09 Project String Table + Complete Graph

`string_id` now denotes canonical text only; `identity_ref` remains semantic WHO. The one current Graph contains Types, Objects, and Links, while textual lookup is a boundary operation over String Table + Identity Space + Graph indexes. `project_manager` performs detached load/replace and full unload of Project-lifetime state.

Run:

```text
server_engine_string_table_benchmark --gate
server_engine_complete_graph_benchmark --gate
```

See `docs/SE_V3_09_COMPLETE_GRAPH.md`.
