# SE-V3-10D3A Persistent Lifecycle Wiring

## Scope

D3A connects the three already-frozen persistence images to the Project lifecycle.

This cut implements:

- `SAVE` from a construction-backed READY Project;
- mmap-native `LOAD` from `compiled.bin` + `source_manager.bin`;
- `BUILD` with no usable baseline as a normal full BUILD;
- `BUILD` with an unchanged compatible baseline by reusing the mapped baseline;
- SAVE-after-LOAD without rebinding the active READY Project;
- compatibility fingerprinting for Project configuration / ABI / persistence schema.

A persisted baseline whose Source contents changed is deliberately **not** rebuilt
implicitly in D3A. `BUILD` returns `rebuild_required` and leaves the Project
`UNLOADED`. D3B adds sparse mapped-baseline materialization.

This split is fail-closed: D3A does not pretend that REBUILD is incremental BUILD.

## Lifecycle

```text
UNLOADED
    ├── LOAD
    │     config
    │     fingerprint
    │     CURRENT + manifest
    │     mmap compiled.bin RO
    │     mmap source_manager.bin RO
    │     DO NOT OPEN build_cache.bin
    │     bind O(1) image views
    │     -> READY
    │
    ├── BUILD
    │     no baseline / incompatible baseline
    │         -> full construction -> READY
    │         -> no implicit SAVE
    │
    │     compatible unchanged baseline
    │         -> mmap all three artifacts
    │         -> validate build cache
    │         -> Source filesystem check
    │         -> mapped READY reuse
    │
    │     compatible changed baseline
    │         -> rebuild_required -> UNLOADED
    │
    └── REBUILD
          ignore baseline
          -> full Sources / Parser / Builder
          -> READY

READY
    ├── SAVE
    ├── QUERY / RUN
    └── UNLOAD -> DRAINING -> UNLOADED
```

## Fast LOAD invariant

`baseline_store::open_ready()` reads `CURRENT` and `manifest.bin`, validates the
fingerprint, and maps only:

```text
compiled.bin
source_manager.bin
```

It does not open `build_cache.bin`.

The D3A regression test deletes `build_cache.bin` after a successful SAVE and then
requires `LOAD` plus semantic object query to succeed. This is stronger than merely
checking a flag: accidental `CreateFile`/`open` of the build cache would fail the
test immediately.

`project_context` also has a dedicated mapped-baseline constructor that does not
construct an empty `compiled_project_state`. Therefore fast LOAD does not allocate
the large mutable String/Identity construction indexes only to discard them.

## Storage-neutral READY boundary

Construction code still owns the existing mutable objects:

```text
string_table
identity_space
source_manager
source_frontend_cache
source_contribution_cache
graph
```

Mapped LOAD instead owns:

```text
baseline_snapshot
compiled_image_view
source_manager_image_view
```

`project_read_view` no longer exposes raw construction-storage references for
Source/Graph summary access. It exposes storage-neutral façades:

```text
project_source_read_view
project_graph_read_view
```

High-level semantic queries (`find_type`, `find_object`, `find_endpoint`,
`find_link`) dispatch directly to either the in-memory Graph or `compiled_image_view`
without heap Graph reconstruction.

## SAVE

### Construction-backed READY

SAVE encodes one coherent triple:

```text
compiled.bin
source_manager.bin
build_cache.bin
```

All three images are bound and cold-verified before `baseline_store::commit()`.
`build_cache.bin` is cross-verified against the compiled/source-manager images.

The existing baseline transaction protocol remains unchanged:

```text
write immutable tx artifacts
flush
write manifest
atomic CURRENT replace
```

The READY Project remains bound to its current storage after SAVE.

### Baseline-backed READY

A Project loaded from transaction `tx-A` remains pinned to `tx-A`, even if CURRENT
later changes.

SAVE-after-LOAD opens the active transaction **by its transaction name**, verifies
all three artifacts, and commits those exact bytes as a new immutable transaction.
It never silently copies whatever CURRENT happens to reference.

After the commit:

```text
active READY Project -> tx-A
CURRENT              -> tx-B
```

This preserves the frozen no-rebind SAVE contract.

## Baseline compatibility fingerprint

The fingerprint contains configuration/schema compatibility, not Source content.

Input includes:

- baseline manifest version;
- `source_manager.bin` version;
- `compiled.bin` version;
- `build_cache.bin` version;
- D3 build-contract version;
- Project configuration version;
- ABI target;
- ABI pack;
- Project name;
- ordered Project roots;
- each root role;
- each normalized resolved root path.

The canonical byte sequence is SHA-256 hashed using the existing Source hash
implementation.

Source file contents are intentionally excluded. They are represented by
`source_manager.bin` physical/content state and are checked independently by BUILD.

## BUILD Source check

For each persisted Source:

1. use the persisted timestamp + size as the filesystem fast-path token;
2. if metadata still matches, Source is unchanged;
3. if metadata differs, acquire the Source and compare SHA-256;
4. content-equal metadata churn remains unchanged;
5. content change, disappearance, or reappearance marks the baseline changed.

No full project directory scan is introduced.

## D3A BUILD boundary

D3A supports two complete BUILD paths:

```text
no usable baseline
    -> full BUILD -> READY

compatible + unchanged baseline
    -> baseline reuse -> READY
```

For:

```text
compatible + changed baseline
```

D3A returns:

```text
status_code::rebuild_required
state = UNLOADED
```

It does not call REBUILD and does not publish stale READY state.

D3B will add:

```text
mapped build_cache
-> changed Source acquisition
-> affected reverse-dependency closure
-> selective Source/interface/provenance materialization
-> sparse Builder update
-> READY
```

## No implicit SAVE

Neither full BUILD nor baseline BUILD changes CURRENT.

Persistence remains explicit:

```text
BUILD -> READY
SAVE  -> new CURRENT
```

## Tests

D3A adds seven lifecycle tests:

```text
project_persistence_save_load
project_load_does_not_map_build_cache
project_load_fingerprint_guard
project_save_after_load_keeps_active_baseline
project_build_no_change_reuses_baseline
project_build_changed_baseline_fail_closed
project_build_without_baseline_full_no_save
```

Expected total after D3A: 85 CTest cases.

## Frozen gates retained

D3A does not introduce:

- `std::mutex`
- `std::shared_mutex`
- `std::unordered_map`
- sorting in Builder/LOAD
- `stable_id`
- process pointer serialization
- Graph history/generation
- implicit SAVE
- semantic re-resolution during LOAD

`LOAD` performs only configuration parsing, compatibility lookup, read-only mapping,
and image `bind()`. Parser and Builder are not invoked.
