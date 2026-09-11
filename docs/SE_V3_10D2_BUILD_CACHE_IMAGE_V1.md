# SE-V3-10D2 build_cache.bin v1

## Scope

This milestone freezes the persisted BUILD-only image used by future baseline-aware
incremental construction.

`build_cache.bin` is not required by READY queries or runtime execution. Fast LOAD
must not map or fault this artifact merely to enter READY.

The image contains:

- immutable bytes for every currently present Source;
- Parser-local Source interface tables;
- complete physical SourceContribution append arenas, including stale slices;
- one-based per-type SourceContribution construction state;
- canonical TypeRef construction caches required by sparse Builder continuation;
- type dependency versions, reverse heads, and append-only dependency edges.

It deliberately does not duplicate the READY semantic/query indexes already stored
in `compiled.bin`.

## Three-artifact ownership

```text
compiled.bin
    READY semantic/query state
    string/identity/type/object/link/TypeRef records
    READY query indexes

source_manager.bin
    source_id/path identity
    filesystem observation + content hash
    forward/reverse include DAG
    roots + path index

build_cache.bin
    Source bytes
    Parser-local interface tables
    SourceContribution provenance
    Builder-only TypeRef/dependency lineage
```

## Process pointers are not persisted

`source_interface` keeps `imported_interfaces` as process-local pointers. Those
pointers are intentionally excluded.

Only the Source-local interface tables are persisted:

```text
local types
type lookup slots
object lookup slots
member lookup slots
```

The import relation is already authoritative in `source_manager.bin`. Future BUILD
activation reconstructs `imported_interfaces` from the Source Manager dependency DAG
after the affected interfaces are materialized.

This prevents a second persisted include graph and keeps the image relocatable.

## Binary layout

The image is field-wise little-endian. Native C++ object layout is never dumped.

```text
256-byte header
20 x 32-byte section directory
64-byte aligned sections

 1 SourceDirectory
 2 SourceBytes
 3 FrontendLocalTypes
 4 FrontendTypeSlots
 5 FrontendObjectSlots
 6 FrontendMemberSlots
 7 ContributionStates
 8 ContributionTypes
 9 ContributionMembers
10 ContributionModifiers
11 ContributionEnumValues
12 ContributionObjects
13 ContributionLinks
14 ConstructionStates
15 GraphIntrinsicRefs
16 GraphNamedRefs
17 GraphDerivedIndex
18 GraphDependencyVersions
19 GraphReverseDependencyHeads
20 GraphDependencyEdges
```

Each directory entry contains section kind, fixed wire-record size, offset, count,
and CRC64.

## Source directory

`SourceDirectory` is dense by `source_id - 1`.

Each record stores:

- `source_id`;
- snapshot-present flag;
- frontend-present flag;
- SourceBytes offset/length;
- ranges for each Source-local frontend table.

Missing Sources retain their dense `source_id` slot and have no Source bytes.

Source bytes are stored exactly as the immutable Source snapshot text. The
cross-artifact verifier compares their SHA-256 and length with `source_manager.bin`.

## Parser interface state

Persisted frontend tables contain only compact numeric values:

```text
identity_ref
string_id
member_index
```

No `source_interface*`, allocator pointer, container pointer, or virtual address is
encoded.

The persisted hash-table slots retain their current capacity and slot placement.
This lets future BUILD materialize only an affected Source interface without
recomputing its local table layout.

## SourceContribution provenance

The physical append arenas are persisted exactly in current arena order:

```text
ContributionStates
ContributionTypes
ContributionMembers
ContributionModifiers
ContributionEnumValues
ContributionObjects
ContributionLinks
ConstructionStates
```

Normal SAVE does not compact these arenas.

A sparse build may have stale payload from prior Source replacements. Current
SourceContribution ranges point to the live slices while stale physical records
remain available only as storage history. `source_contribution_statistics` records
the live logical counts separately.

This preserves all range indices and one-based `definition_type` references without
remapping.

REBUILD remains the compaction boundary.

## Builder lineage state

Sparse Builder requires the following current-Graph construction caches:

```text
intrinsic_refs
named_refs
derived_index + derived_index_entries
dependency_versions
reverse_dependency_heads
dependency_edges
```

The following Graph indexes are intentionally not duplicated:

```text
identity_index
object_identity_index
link_index
```

Equivalent READY lookup indexes already exist in `compiled.bin`. Future BUILD
activation may bind/use or materialize those from the compiled baseline rather than
carrying a second copy in `build_cache.bin`.

## Dependency representation

`dependency_versions` and `reverse_dependency_heads` are dense by
`type_handle - 1`.

`dependency_edges` is append-only. `reverse_dependency_heads` and
`next_for_target` are one-based edge indices; zero is the sentinel.

Owner versioning preserves the current sparse validation contract: replacing one
type definition invalidates prior outgoing edges by version instead of rewriting
the full reverse graph.

## Fast bind versus cold verification

`build_cache_image_view::bind()` checks only fixed structural metadata:

- magic/version/endian;
- header and directory CRC;
- exact 20-section schema;
- canonical alignment and bounds;
- required dense cardinality relationships;
- complete-cache flags.

`verify_contents()` is the explicit cold/full pass and validates:

- every section CRC64;
- SourceDirectory and SourceBytes ranges;
- frontend table ranges and empty-slot sentinels;
- SourceContribution ranges and live statistics;
- construction-state references;
- TypeRef cache slot consistency;
- derived-index occupancy count;
- dependency-head/edge bounds.

`verify_against(compiled.bin, source_manager.bin)` adds cross-artifact checks:

- source count agreement;
- SourceBytes hash/length versus Source Manager physical state;
- identity/string references versus compiled semantic tables;
- type-handle/TypeRef lineage versus compiled Graph;
- construction/dependency cardinality versus compiled type slots.

These full scans are SAVE/maintenance/diagnostic checks, not mandatory fast LOAD
work.

## Determinism

Encoding traverses only existing deterministic storage order:

```text
source_id ascending
SourceContribution physical arena order
type_handle order
existing derived-index slot order
existing dependency-edge append order
```

There is no sort, unordered_map, mutex, or runtime pointer serialization.

## D2 boundary

D2 defines and verifies the image only. It does not yet activate mapped
`build_cache.bin` as mutable incremental state.

The next persistence cut wires the lifecycle:

```text
LOAD
    map compiled.bin + source_manager.bin
    do not touch build_cache.bin
    -> READY

BUILD
    map all three artifacts
    bind immutable baseline views
    detect changed Sources
    materialize only affected Source/interface/contribution/Graph build units
    rebind process-local interface imports from Source Manager DAG
    -> READY

SAVE
    emit coherent compiled/source/build-cache images
    commit manifest
    atomically replace CURRENT
```

This keeps the D2 wire format independently testable before introducing sparse
mapped-baseline materialization.
