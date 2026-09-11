# SE-V3-10C source_manager.bin v1

## Scope

This milestone freezes the mmap-native persisted representation of Source Manager
state used by future LOAD and BUILD. It does not serialize Parser interfaces or
Source text; those belong to `build_cache.bin`.

The image contains all known dense `source_id` slots, normalized paths, physical
change state, SHA-256 content hashes, forward/reverse dependency CSR, Project
roots with `project_item_role`, a persisted Source Manager generation marker, and
a mmap-ready normalized-path index.

No semantic identity, Graph handle, Parser state, or Runtime state is stored here.

## Binary layout

The file is field-wise little-endian. Native C++ struct layout is never serialized.

```text
128-byte header
9-entry directory
64-byte aligned sections

1 SourceCore
2 PhysicalState
3 ForwardOffsets
4 ForwardEdges
5 ReverseOffsets
6 ReverseEdges
7 Roots
8 PathIndex
9 PathBytes
```

The directory entry size is 32 bytes and records section kind, record size,
offset, count, and CRC64.

`SourceCore` is dense by `source_id - 1`. `ForwardOffsets` and
`ReverseOffsets` have `source_count + 1` entries. Edges are uint32 source IDs.

`PathIndex` uses the same deterministic XXH64-derived fingerprint/probing rule as
the in-memory Source Manager, but it is reconstructed in dense source order during
encoding. Runtime hash-table iteration order is never serialized and no sort is
performed.

## Fast LOAD

`source_manager_image_view::bind()` validates only:

- magic/version/endian/header;
- header CRC;
- directory CRC;
- section bounds and record sizes;
- dense section cardinalities;
- final CSR offsets.

It does not read all section payloads. This keeps mmap LOAD proportional to
metadata rather than file size.

`verify_contents()` is an explicit cold integrity pass. It validates each section
CRC and then validates paths, dependency IDs, roots, and persisted path-index
reachability.

## Access

Known Sources are always addressed directly:

```text
source_id
  -> SourceCore[source_id - 1]
  -> PhysicalState[source_id - 1]
  -> CSR offsets[source_id - 1]
```

No path lookup is used for known-source BUILD work.

`find(normalized_path)` exists only as the hot already-normalized path identity
boundary used for new include discovery/external path resolution. It performs no
filesystem operation, normalization, allocation, or sorting.

## Generation field

The persisted `generation` field is storage correlation metadata supplied by the
caller. It is not a Graph generation counter, does not create history, and is not
a semantic identity.

## BUILD cache separation

`source_manager.bin` intentionally does not contain Source text. Future
`build_cache.bin` owns immutable Source bytes and Parser interface data required
to reparse affected dependents without rereading unchanged files.

## Performance contract

- encode is O(Sources + dependency edges + path bytes);
- no sorting;
- no unordered_map;
- no mutex/shared_mutex;
- bind is O(1) with respect to Source count;
- known Source access is O(1);
- path lookup is expected O(1) through the persisted open-addressed index;
- full CRC/content validation is never on the mandatory fast LOAD path.
