# SE-V3-10D3D Fast Dirty Detection

## Scope

D3D replaces the normal O(Project Source count) filesystem dirty scan introduced
by D3A/D3B with a persisted Windows NTFS USN change-journal fast path.

D3D does not change:

- Graph semantics;
- Builder semantics;
- sparse mapped materialization;
- numeric handle preservation;
- READY ownership;
- LOAD behavior;
- explicit SAVE semantics.

`LOAD` still maps only `compiled.bin` and `source_manager.bin`.
The D3D state is BUILD-only and lives in `build_cache.bin`.

## Why this cut exists

D3C measured:

```text
1,000,000 Sources, modify one
dirty detection     38.918 s
frontend             0.842 ms
Builder              0.259 ms

1,000,000 Sources, no change
dirty detection     34.366 s
Parser/Builder        0
```

The sparse semantic pipeline is already proportional to the affected set.
The remaining dominant cost is the full filesystem metadata scan.

## build_cache.bin v3

D3D increments:

```text
build_cache_image_format_version: 2 -> 3
build contract:                   2 -> 3
directory sections:             23 -> 25
```

New sections:

```text
24 source_file_identity_index
25 tracked_directory_identity_index
```

Directory-index slots carry separate watch flags:

```text
topology  = ancestor rename/delete/reparse protection
arrival   = create/rename-in protection only for a missing tracked Source's
            immediate parent directory
```

This avoids turning unrelated file creation elsewhere in a project ancestor
directory into an O(N) fallback.

The existing 256-byte header uses previously reserved bytes:

```text
136  backend                 u32
140  reserved                u32 = 0
144  volume serial           u64
152  USN journal ID          u64
160  checkpoint NextUsn      i64
168..239 reserved            zero
240  directory CRC64
248  header CRC64
```

No `source_manager.bin` or `compiled.bin` format change is required.

The compatibility fingerprint already contains the build-cache format and D3
build-contract version, therefore a pre-D3D persisted baseline is rejected
fail-closed and requires one REBUILD + SAVE.

## SAVE capture

SAVE remains the explicit persistence boundary.

Before baseline encoding, D3D captures a journal position `J0` and then validates
each tracked Source against the READY Source snapshot:

```text
capture J0
  -> validate path metadata
  -> obtain filesystem file identity
  -> require one volume
  -> build file-reference -> source_id index
  -> build tracked directory identity set
  -> encode immutable baseline carrying J0
```

Capturing `J0` before validation closes the save race:

```text
J0
Source validated
Source changes
baseline encoding/commit
```

The post-J0 Source change remains in the journal and is observed by the next
BUILD.

If the tracker cannot be established safely, SAVE still succeeds with tracker
backend `none`. The next BUILD uses the existing full scan.

## Windows fast path

For a D3D baseline:

```text
BUILD
  -> baseline_store.open()
  -> bind source_manager.bin
  -> bind build_cache.bin
  -> validate persisted journal checkpoint
  -> FSCTL_QUERY_USN_JOURNAL
  -> FSCTL_READ_USN_JOURNAL from saved NextUsn
  -> probe file reference in mmap index
  -> exact dirty source_id set
  -> existing D3B sparse frontend/Builder
```

The persisted file-reference hash table is open-addressed and power-of-two.
Lookup is allocation-free and does not use `unordered_map`, sorting, or textual
Builder lookup.

## Fail-closed fallback

The fast path returns to the existing complete Source scan when:

- no persisted tracker exists;
- platform/backend is unsupported;
- volume identity changes;
- journal identifier changes;
- saved USN was truncated from the journal;
- journal query/read is unavailable;
- Sources span multiple volumes;
- a Source is missing during SAVE tracker preparation;
- duplicate tracked file identities are observed;
- a tracked directory changes;
- a tracked Source receives a topology-changing event such as rename/delete,
  hard-link change, create, or reparse-point change.

The fallback preserves the existing content-hash semantics.

Allocation failures and artifact corruption remain hard failures rather than
silently becoming a fallback.

## Directory tracking

A file-reference-only map is sufficient for in-place Source writes but not for
ancestor directory renames.

SAVE therefore stores a compact set of directory file references for ancestors
of tracked Sources. Ancestors carry the `topology` watch flag, so rename/delete
of the directory itself falls back to the full path-based scan.

Only the immediate parent of an already-missing tracked Source receives the
`arrival` flag. A create/rename-in event under that directory falls back so the
stable Source identity can be rediscovered. Unrelated creates in ordinary
ancestor directories do not invalidate the fast path.

This keeps the normal content-edit path O(journal changes) while making path
topology changes fail closed.

## Windows privilege boundary

Microsoft documents that change-journal operations require administrator
privileges. D3D therefore treats an unavailable USN journal as a normal fallback,
not as a Project failure.

The benchmark reports:

```text
dirty_backend
dirty_fast
dirty_fallback
journal_records
journal_matched
```

`--fast-gate` is intended to be run from an elevated PowerShell on NTFS. If the
fast backend cannot be activated, it reports `UNAVAILABLE` rather than pretending
that the O(N) fallback met the fast-path target.

## Performance gate

The dedicated Windows fast gate uses a persisted 100,000-Source project and a
no-change BUILD.

Target:

```text
dirty_detection <= 100 ms
dirty_fast       = 1
dirty_fallback   = 0
dirty_sources    = 0
```

The threshold applies only to the dirty detector, not to the synthetic
benchmark's project-configuration parsing. The D3C benchmark intentionally
lists every generated Source as a root, which is useful for Source scale but
also adds a separate O(root count) configuration/fingerprint cost.

## Frozen gates retained

D3D introduces no:

- `std::mutex`;
- `std::shared_mutex`;
- `std::unordered_map`;
- sorting in Builder/dirty hot path;
- `stable_id`;
- Graph history/generation;
- implicit SAVE;
- LOAD mapping of `build_cache.bin`;
- semantic re-resolution during LOAD.

The existing D3B sparse Builder remains unchanged.
