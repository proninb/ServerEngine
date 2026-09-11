# SE-V3-10D3B Sparse Mapped-Baseline BUILD

## Scope

D3B replaces D3A's changed-baseline `rebuild_required` boundary with a true sparse
BUILD over a persisted Project baseline. The existing incremental frontend and
Generation Builder remain authoritative. D3B changes the storage beneath them.

## BUILD path

```text
UNLOADED
   |
   | BUILD
   v
CURRENT + manifest
   |
   +-- mmap compiled.bin        RO
   +-- mmap source_manager.bin  RO
   +-- mmap build_cache.bin v2  RO
   |
   v
filesystem Source check
   |
   +-- no dirty Sources ----------------------> mmap READY reuse
   |
   `-- exact dirty source_id set
          |
          v
      existing build_incremental()
          |
      affected reverse closure only
          |
          +-- Source Manager sparse overlay
          +-- Parser interface sparse overlay
          +-- SourceContribution sparse overlay
          `-- Graph sparse overlay
                  |
                  v
                READY
```

There is no full baseline Graph/String/Identity/SourceContribution reconstruction.
Unchanged semantic and BUILD records remain in read-only mappings. Dirty detection
binds only `source_manager.bin`; full `verify_contents()`/cross-artifact scans are
kept on cold SAVE/test boundaries so BUILD does not fault the complete cache before
it knows whether a sparse update is needed.

## Storage rule

D3B uses a two-part storage contract during CONSTRUCTING:

```text
logical array = immutable mapped baseline + local append/patch storage
```

Baseline values may be decoded into temporary/materialized construction records
when the incremental Builder touches them. READY query and SAVE iteration use
allocation-free indexed reads and do not populate the construction cache.

This distinction is required by the frozen READY contract: first query of a mapped
record must never allocate or mutate hidden state.

## String and identity continuity

`string_id` and `identity_ref` baseline slots retain their exact numeric values.
New strings and identities allocate strictly after the persisted slot count.
Lookup first probes the local overlay and then the persisted mmap index.

No `stable_id`, text lookup in Builder, global identity registry, or pointer
serialization is introduced.

## Source Manager

The persisted Source Manager path index remains authoritative for baseline Sources.
Only newly discovered Sources receive local path-index entries.

Untouched baseline snapshots, include edges, and reverse edges are read directly
from the mapped images. Candidate records are materialized only for dirty/affected
Sources. Publication pre-materializes every baseline Source slot that will be
patched, including metadata-only updates, so `publish_prepared()` remains
allocation-free/no-fail.

SAVE uses indexed `include_count/include_at/dependent_count/dependent_at` access.
It never materializes all baseline edge vectors.

## Frontend cache

The baseline cache keeps Parser interface tables in `build_cache.bin`. There is no
dense `vector<unique_ptr<source_interface>>` for persisted Sources.

`build_incremental()` asks `cache.interface(source)` only for unaffected imports
that actually enter the affected parse closure. Those interfaces are restored
lazily with process-local import pointers. An unresolved sparse-overlay sentinel is
published before walking imports, so a corrupted persisted include cycle fails
immediately instead of relying on an arbitrary recursion-depth limit; shared imports
materialize once and remain pointer-stable through their heap-owned interface object.

SAVE uses a separate allocation-free persistence boundary. Untouched interface
ranges are copied/decode-read directly from `build_cache.bin`; changed/new Source
interfaces come from the sparse overlay. SAVE therefore does not recursively
materialize the baseline frontend cache.

## SourceContribution

Persisted SourceContribution append arenas and construction states are exposed as
mapped baseline arrays. Incremental replacement materializes only previous Source
ranges and type construction slots required by the changed set. New payload remains
append-only, preserving all existing range indices.

Normal SAVE emits the logical merged arrays in original numeric order.

## Graph

The Graph keeps the existing `prepared_graph_update` and Generation Builder.
Baseline semantic arrays and BUILD lineage arrays are mmap-backed; prepared sparse
patches/new records are local.

All writable baseline slots and hash buckets needed by publication are materialized
and all append capacity is reserved in `prepare_sparse_publication()`.
`publish_prepared()` performs no first-touch allocation.

Persisted historical indexes begin near 50% load. Sparse BUILD may consume that
reserved capacity up to 75% load; beyond that boundary BUILD fails closed with
`rebuild_required` rather than performing an O(Project) index rehash in the hot
incremental path. This gives ordinary add/change workloads bounded sparse cost while
keeping REBUILD as the compaction/index-resize boundary.

READY-facing Graph lookup uses allocation-free value reads instead of lazy pointer
materialization.

## build_cache.bin v2

D3B changes BUILD cache format to version 2 and 23 sections.

The first 20 D2 sections are retained. Three BUILD-only historical indexes are
added:

```text
21  GraphTypeIdentityIndex
22  GraphObjectIdentityIndex
23  GraphLinkTargetIndex
```

These indexes include tombstoned handles. `compiled.bin` READY indexes intentionally
contain live entities only and therefore cannot recover historical handles after a
new process starts.

The historical indexes are open-addressed and collision candidates are verified
against `compiled.bin` type/object identity slots or link target records before a
handle is accepted.

A removed link preserves its target endpoint in the tombstone; therefore
`GraphLinkTargetIndex` allows a later BUILD to reactivate exactly the original
`link_handle` after SAVE/UNLOAD.

The Project compatibility fingerprint includes `build_cache_image_format_version`
and D3B build-contract version 2. D2/D3A cache images are not silently interpreted
as D3B lineage.

## SAVE after sparse BUILD

A changed BUILD owns one logical READY snapshot consisting of:

```text
pinned baseline mappings + immutable sparse overlay
```

SAVE re-encodes the logical merged state into a new immutable transaction. The
active READY Project remains pinned to its original baseline transaction; CURRENT
moves to the newly committed transaction. There is no active-project rebind.

## Regression gates

D3B replaces the D3A fail-closed changed BUILD test with:

`project_build_changed_baseline_sparse_save_load`

It requires:
- changed persisted BUILD succeeds;
- `changed=true`, `rebuilt=false`;
- A/B/link handles are unchanged;
- a newly added member is queryable;
- SAVE creates a new transaction without rebinding the active Project;
- UNLOAD/LOAD of the new CURRENT retains the change and numeric handles.

D3B also adds:

`project_build_link_tombstone_handle_restore`

It requires:
- persist live link H;
- sparse BUILD removes it;
- SAVE the tombstone generation;
- restore the Source text in a later process BUILD;
- restored link handle equals H.

Expected suite size: 86 CTest cases.

## Frozen gates retained

D3B introduces no:
- `std::mutex` / `std::shared_mutex`;
- `std::unordered_map`;
- sorting in Builder or LOAD;
- textual Builder lookup;
- `stable_id`;
- process pointer persistence;
- Graph generations/history;
- implicit SAVE;
- Parser/Builder execution during LOAD.
