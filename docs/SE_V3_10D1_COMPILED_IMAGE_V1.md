# SE-V3-10D1 compiled.bin v1

## Scope

This milestone freezes the mmap-native READY semantic/query image.

It does not wire lifecycle `LOAD`/`SAVE` yet and it does not persist Builder-only
incremental state. `compiled.bin` is deliberately the smallest artifact required
to answer READY Project queries without Parser, Builder, identity reconstruction,
or heap Graph reconstruction.

The artifact preserves the numeric values of:

- `string_id`
- `identity_ref`
- `type_handle`
- `object_handle`
- `link_handle`
- `TypeRef`
- `member_index`

Normal SAVE therefore never remaps semantic or Graph-local IDs. Holes/tombstone
slots are retained. REBUILD remains the compaction boundary.

## Binary layout

The file is field-wise little-endian. Native C++ object layout is never dumped.

```text
256-byte header
16 x 32-byte section directory
64-byte aligned sections

 1  StringCore
 2  StringIndex
 3  StringBytes
 4  IdentityCore
 5  IdentityIndex
 6  Types
 7  TypeIdentities
 8  Members
 9  EnumValues
10  Objects
11  ObjectIdentities
12  Links
13  CanonicalTypes
14  GraphTypeIndex
15  GraphObjectIndex
16  GraphLinkIndex
```

The directory stores section kind, wire-record size, offset, count, and CRC64.

The fixed 256-byte header stores only schema/structural metadata and live counts.
There is no Graph generation counter or persistent history identifier.

## String representation

`StringCore` is dense by `string_id - 1`, including holes created by losing
concurrent intern candidates.

Each 16-byte record contains:

```text
uint64 byte_offset
uint32 byte_length
uint32 reserved
```

A zero length is the hole sentinel. Valid interned strings are non-empty.

`StringIndex` is an open-addressed `(fingerprint, string_id)` index rebuilt in
ascending `string_id` order during encoding. Runtime pointer buckets are never
serialized.

## Identity representation

`IdentityCore` is dense by `identity_ref.slot() - 1` and preserves D0 slots.

Each 12-byte record contains:

```text
uint32 identity_ref_value
uint32 parent_identity_ref_value
uint32 local_string_id
```

Slot 1 is the root. A zero `identity_ref_value` is a hole.

`IdentityIndex` is rebuilt from live identity records in ascending slot order and
maps `(parent identity_ref, local string_id)` to the packed 32-bit
`identity_ref`. Kind remains encoded in the top two bits of `identity_ref`.

No process address is persisted.

## Graph representation

READY semantic arrays preserve their existing handle/index numbering:

- Type record: 12 bytes
- Type identity: 4 bytes
- Member record: 12 bytes
- Enum value: 16 bytes
- Object record: 8 bytes
- Object identity: 4 bytes
- Link record: 16 bytes
- Canonical TypeRef record: 16 bytes

Links contain only resolved object/member endpoints. No textual names are stored
inside links.

The three Graph query indexes are rebuilt deterministically from live
type/object/link slots. Index layout itself is not semantic identity.

## What is intentionally not in compiled.bin

These current Graph fields are Builder/incremental acceleration state and belong
to `build_cache.bin` instead:

- intrinsic/named TypeRef construction caches
- derived TypeRef hash index
- dependency versions
- reverse dependency heads
- dependency edges
- SourceContribution
- Parser/front-end interfaces and source bytes

A normal READY LOAD must not fault build-only pages merely to answer runtime/HMI
queries.

## Fast LOAD contract

`compiled_image_view::bind()` validates only fixed metadata:

- magic/version/endian
- header CRC
- directory CRC
- exact section kinds and wire-record sizes
- canonical 64-byte section offsets/bounds
- section cardinality relationships
- live-count bounds
- query-index power-of-two capacities
- no trailing bytes

It does not scan section payloads. Bind complexity is O(1) with respect to
Project size.

`verify_contents()` is an explicit cold/full integrity pass. It checks:

- every section CRC64
- StringCore/StringIndex consistency
- IdentityCore/IdentityIndex consistency
- Graph identity/reference ranges
- member/enum/object/link references
- canonical TypeRef payloads
- live counts
- Graph query-index reachability

## Determinism

Encoding traverses numeric slots/handles only:

```text
string_id ascending
identity slot ascending
type_handle ascending
object_handle ascending
link_handle ascending
```

No sorting and no runtime hash-table iteration state are used.

String, identity, and Graph query indexes are rebuilt deterministically from
those numeric sequences.

## Lifecycle boundary

D1 is a codec/view foundation only.

Future SAVE:

```text
READY Project
  -> staged compiled.bin writer
  -> source_manager.bin
  -> build_cache.bin
  -> manifest
  -> atomic CURRENT replacement
```

Future LOAD:

```text
open baseline
  -> bind compiled.bin
  -> bind source_manager.bin
  -> attach Runtime
  -> READY
```

There is no Parser, Builder, semantic re-lookup, or Graph heap reconstruction on
that LOAD path.

## Performance follow-up

The current D1 encoder emits to `std::vector<std::byte>` because it is a
verification/staging implementation. The persistent schema is designed so a
later SAVE cut can write the same records directly to a staged file without an
extra whole-image memory copy.
