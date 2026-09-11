# SE-V3-10B Persistent Baseline Storage

## Scope

This milestone freezes the persistent transaction and mapping boundary used by
future `LOAD`, `BUILD`, and `SAVE`. It does not serialize the semantic Project
yet and does not expose fake lifecycle operations before the compiled/source
image codecs exist.

One Project configuration owns one cache root:

```text
<project-dir>/.serverengine/<project-config-filename>/
    CURRENT
    tx-.../
        manifest.bin
        compiled.bin
        source_manager.bin
        build_cache.bin
```

Transaction directories are immutable after commit. `CURRENT` is the only
authoritative selector.

## Commit

SAVE-side storage follows:

```text
create unique tx directory
    -> write + flush compiled.bin
    -> write + flush source_manager.bin
    -> write + flush build_cache.bin
    -> write + flush manifest.bin
    -> flush transaction/root directory metadata where supported
    -> durable temporary CURRENT selector
    -> atomic replace CURRENT       <-- commit point
```

No artifact belonging to the previous committed baseline is modified in place.
A failure before the selector replacement leaves the previous baseline
authoritative.

Once selector replacement succeeds, the operation is committed. Later
best-effort directory flushing is not allowed to report the already-switched
transaction as a failed commit.

## Manifest

`manifest.bin` v1 is a fixed 256-byte field-wise little-endian header. Native
C++ object layout is never written.

It contains:

- magic and format version;
- endian marker;
- Project compatibility fingerprint supplied by the image layer;
- selected transaction name;
- exact sizes for `compiled.bin`, `source_manager.bin`, and `build_cache.bin`;
- CRC64 of the manifest header.

LOAD validates the 256-byte manifest and exact artifact sizes before exposing
the snapshot. It does not hash/read the full artifacts merely to open them.

Each artifact will own its own mmap-native header/section validation in the next
image milestone.

## Mapping

`baseline_snapshot` owns three independent read-only mappings. The mappings are
one lifetime unit and therefore cannot mix artifacts from different
transactions.

The old transaction may remain mapped after a later SAVE commits a new
`CURRENT`. This is expected backing-storage lifetime, not Project history.

There is no whole-artifact writable/COW mapping in this layer.

## Garbage collection

Cold cleanup retains:

1. the transaction selected by `CURRENT`;
2. an optional transaction pinned by the currently READY Project.

All other `tx-*` directories are reclaimable. A READY Project must release its
mappings before asking cleanup to remove its formerly pinned transaction.

## Next milestone

The next layer supplies mmap-native image formats for:

- `source_manager.bin`;
- `compiled.bin`;
- `build_cache.bin`.

Those codecs must use local numeric references/offsets, section-specific
REUSE/MATERIALIZE/REBUILD policy for BUILD, and must not reintroduce live Graph
publication or mutexes into Graph access.
