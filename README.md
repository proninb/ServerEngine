# Server Engine

Clean Architecture V3 bootstrap for the Curtiss-Wright Server Engine project.

This repository reuses only proven foundation contracts from the former `Server-Entry` project. The V2 stable-ID, Builder, Graph, Parser, Runtime, and SHM implementations are not carried forward.

## Current foundation

- centralized diagnostics;
- strict transactional `server.json` loader;
- strict transactional `project.json` loader;
- Windows x64 / POSIX x64 ABI configuration;
- C++20 JSON SAX parser used by configuration loading;
- Project Context;
- Project Context-owned semantic identity space;
- stable-address `identity_node` objects with Project-lifetime names;
- one semantic declaration-resolution operation returning `identity_ref` directly;
- no numeric semantic `stable_id`;
- no Identity Registry or String Registry in the semantic path;
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

`identity_node*` means WHO. Generation-local handles mean WHERE in one `G`. Generation entries mean WHAT in that generation.

`identity_node` never contains a Graph pointer, generation handle, definition state, ABI layout, members, or other generation-specific state.

The backing arena is a storage primitive only. It owns stable Project-lifetime bytes and performs no name resolution, canonicalization, hashing, or identity lookup.

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
    -> resolve project.json
    -> validate Project configuration
    -> create Project Context
    -> create Project semantic identity root
```

Source Manager, Parser, Generation Builder, Graph, persistence, Runtime, and SHM are intentionally not implemented yet.

## Next implementation sequence

1. Define Source Manager V3 ownership and dependency contracts.
2. Integrate Parser semantic scope with `project_context::resolve_declaration()` so each source-language resolution returns `identity_ref` directly.
3. Define transient source facts carrying direct identity references.
4. Implement Generation Builder with zero name/identity lookup after semantic resolution.
5. Implement immutable `G0` with dense generation-local handles.
6. Implement `SAVE G0` / `LOAD G0` using file-local IDs; never serialize pointers.
7. Implement sparse `Gn -> Gn+1` MVCC publication.
8. Add client-query and SHM materialization boundaries.

## Provenance

Configuration schemas, diagnostics concepts, status/operation/source identifiers, and strict loader behavior originate from the former `proninb/Server-Entry` foundation. V3 semantic identity is a new architecture.

## SE-V3-03 identity scale benchmark

Build `server_engine_identity_benchmark` and run `--gate` before attempting the full 10K/100K/1M matrix. The full benchmark intentionally uses a flat semantic scope so superlinear name-resolution behavior cannot be hidden by workload shape. See `docs/SE_V3_03_IDENTITY_SCALE_BENCHMARK.md`.


## SE-V3-04 Semantic Scope Index

Project semantic resolution uses one lock-free fixed bucket index instead of sibling-linear traversal. The 1M identity scale gate and concurrent arena memory-amplification gate must pass before Parser/Builder integration proceeds.

## SE-V3-05 Identity Index Hardening

`identity_space` exposes diagnostic-only collision-chain statistics without instrumenting the semantic hot path. Run `server_engine_identity_benchmark --hardening` to validate 1M sequential, patterned, same-prefix, and long identifiers, including exact canonical-pointer replay and structural collision/probe gates. After this gate passes, the Project semantic identity foundation is frozen and SE-V3-06 begins Source Manager + Parser integration.
