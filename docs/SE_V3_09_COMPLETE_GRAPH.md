# SE-V3-09 — Project String Table + Complete Graph

## Status

Implemented and regression-tested.

## Goal

Complete the compiled Project model before ABI/runtime work. The current Graph now contains semantic Types, Objects, and Links. Text is canonicalized once per loaded Project through `string_id`; semantic identity remains `identity_ref`.

## Identity model

```text
string_id    = canonical text atom for the loaded Project
identity_ref = Project-lifetime semantic WHO
Graph handle = WHERE in the one current Graph
```

`string_id` is not semantic identity and is not persisted as a universal ID.

## String Table

`string_table` owns immutable Project-lifetime text atoms. It uses fixed bucket arrays, stable arena storage, paged direct `string_id -> record` lookup, and no `std::unordered_map`, `std::list`, generation counter, or tombstone lifecycle.

`identity_node` stores only:

```text
parent identity_ref
local string_id
identity_kind
```

The same local text is stored once even when used by many identities or Graph records.

## Complete Graph

The one current Graph owns compiled semantic state:

```text
Types
  type_entry[]
  type identity[]
  member_record[]
  enum_value_record[]
  canonical TypeRef[]

Objects
  object_entry[]
  object identity[]

Links
  link_record[]
```

Graph stores no Runtime addresses, live values, task state, SHM pointers, or native references.

### Objects

A Project object is declared in source and therefore exists before Runtime materialization. `object_handle` is a 4-byte Graph-local location. `object_entry` stores its canonical `TypeRef`; identity remains in a parallel identity array.

### Links

Parser resolves assignment syntax such as:

```cpp
B.IN = A.OUT;
```

into semantic endpoints before Builder:

```text
source = { identity_ref(A), member_index(OUT) }
target = { identity_ref(B), member_index(IN) }
```

Builder converts only resolved identities/indexes to Graph handles. No textual lookup occurs in Builder.

Graph link storage is:

```cpp
struct object_endpoint {
    object_handle object;
    member_index member;
};

struct link_record {
    object_endpoint source;
    object_endpoint target;
};
```

One target endpoint has at most one incoming canonical Link. One source endpoint may feed multiple targets.

## Query

`project_context` provides textual boundary lookup without storing fully-qualified strings in Graph:

```text
"N::T"      -> type_handle
"N::A"      -> object_handle
"N::A.OUT"  -> object_endpoint
"N::B.IN"   -> link_handle (by target)
```

Resolution is:

```text
string_view path
  -> string_id segments
  -> identity hierarchy
  -> Graph identity index
  -> Graph-local handle/index
```

Text lookup is a control/query path only. Runtime execution will use direct handles, offsets, and resolved addresses.

## Incremental contract

Incremental object/link updates preserve the SE-V3-07/08 sparse rules:

- touched Sources only;
- no full Graph or SourceContribution scans;
- historical object/link slots may be tombstoned and reactivated;
- duplicate target Links fail before publication;
- index/headroom exhaustion fails closed and requires an explicit full rebuild;
- publication is allocation-free/no-fail.

## Project lifecycle

`project_manager` owns the optional loaded `project_context`.

`load()` builds a detached candidate Project and replaces the current Project only after successful full build. Failed replacement leaves the old Project untouched. `unload()` destroys the entire Project context, invalidating all Project-lifetime `string_id`, `identity_ref`, Graph handles, and compiled state.

External readers use `project_read_guard`. Full LOAD/REBUILD work is detached while the current Project remains readable; UPDATE prepares against current state while readers remain active. Only the short no-fail publication/replacement boundary takes the exclusive lifecycle lock. Any pointer/span/string_view obtained through a read guard is valid only while that guard remains held.

Sparse headroom exhaustion returns `status_code::rebuild_required`. `project_manager::update()` then performs a detached full rebuild and replaces the current Project only after that rebuild succeeds. A failed fallback therefore preserves the current compiled Project unchanged. Storage-pressure telemetry tracks retained, reserve, and stale bytes and may proactively request the same detached rebuild path.

## Measured gates

Reference Linux verifier results from the SE-V3-09 implementation run:

```text
60/60 functional tests PASS
ASan+UBSan 60/60 PASS
TSAN 60/60 PASS
strict changed/new SE-V3-09 translation units PASS

String Table, 1M unique strings:
  intern       ~155 ns/op
  hit          ~87 ns/op
  direct get   ~7 ns/op
  memory       ~60.9 B/string

Complete Graph:
  1M Objects + 1M Links full prepare  ~355 ms
  full publish                         ~1.3 us
  100K sparse one-Link retarget       ~11 us prepare
  sparse publish                       ~1.5 us
  full scans                           0
```

The 1M-unique-name identity benchmark is intentionally a worst-case workload for the new architecture because it creates both one canonical text atom and one semantic identity per declaration. Real projects with repeated identifiers amortize the String Table by design.

## Frozen boundary after SE-V3-09

```text
Source Manager -> Parser -> SourceContribution -> Current Graph
                                              Types + Objects + Links
```

The next layer is ABI/Implementation layout. Graph remains semantic-only.
