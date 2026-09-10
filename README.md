# Server Engine

Clean Architecture V3 bootstrap for the Curtiss-Wright Server Engine project.

This repository starts from the proven infrastructure contracts in `proninb/Server-Entry` (`main`) but deliberately does **not** carry the V2 Graph/Builder/Parser/stable-ID implementation forward.

## V3 foundation included

- centralized diagnostics and deterministic diagnostic ordering;
- strict transactional `server.json` loader;
- strict transactional `project.json` loader;
- Windows x64 / POSIX x64 ABI configuration;
- minimal C++20 JSON SAX parser used by configuration loading;
- Project Context skeleton;
- Project-lifetime string registry;
- Project-lifetime pointer identity registry;
- stable-address `identity_node` atoms;
- no numeric semantic `stable_id`;
- architecture baseline in `docs/SERVER_ENGINE_ARCHITECTURE_V3.md`;
- CMake build/test path and Visual Studio 18 / v145 solution/project.

## Fundamental identity contract

```text
identity_node*   = semantic identity for one Project Context lifetime
identity slot    = process-local acceleration only
generation handle = location inside one immutable G generation
serialized id    = file-local identity/reference during SAVE/LOAD
SHM reference    = runtime-specific cross-process representation
```

`identity_node` never contains a `graph*`, generation handle, definition, members, ABI layout, or any other generation-specific state.

## Build

### Visual Studio

Open:

```text
ServerEngine.sln
```

The project currently targets Visual Studio 18 toolset `v145`, C++20, x64.

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

The bootstrap currently performs:

```text
server.json
    -> validate Server configuration
    -> resolve project.json
    -> validate Project configuration
    -> create Project Context
    -> create empty Project Identity Registry
```

It intentionally does not yet create Source Manager, Parser, Generation Builder, Graph, persistence, Runtime, or SHM.

## Next implementation sequence

1. Harden and benchmark Identity Registry / String Registry storage and concurrency.
2. Define Source Manager V3 contracts and decide which V2 acquisition/persistence code is reusable without semantic coupling.
3. Define Parser V3 output using direct `identity_ref` references.
4. Implement Generation Builder and immutable `G0`.
5. Implement dense generation handles and identity-slot-to-handle acceleration.
6. Implement `SAVE G0` / `LOAD G0` using file-local IDs; never serialize pointers.
7. Implement sparse `Gn -> Gn+1` MVCC publication.
8. Add client-query and SHM materialization boundaries.

## Provenance

The Server configuration and Project configuration schemas, diagnostics concepts, status/operation/source identifiers, and strict loader behavior are based on the existing `proninb/Server-Entry` project. V3 source has been reorganized and cleaned so the new repository does not depend on the V2 `stable_id`, Builder, Graph, Frontend, Parser, Runtime, or SHM implementation.
