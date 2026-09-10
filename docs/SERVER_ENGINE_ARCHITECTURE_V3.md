# Server Engine Architecture V3

**Status:** Active frozen compile-side baseline through SE-V3-09

## 1. Core model

One Server owns one loaded Project. The Project owns one current compiled Graph. There is no retained Graph history, MVCC version store, or Graph generation counter.

```text
string_id     = canonical text atom inside one loaded Project
identity_ref  = Project-lifetime semantic WHO
source_id     = normalized Source identity inside one loaded Project
Graph handles = WHERE in the one current Graph
Runtime refs  = future execution-specific offsets/addresses
```

## 2. Top-level ownership

```text
PROJECT MANAGER
    |
    +-- optional PROJECT CONTEXT
          |
          +-- Project Configuration
          +-- String Table
          +-- Identity Space
          +-- Source Manager
          +-- Frontend Interface Cache       COLD/reconstructable
          +-- SourceContribution Cache       build-side provenance
          +-- Current Graph
                +-- Types
                +-- Objects
                +-- Links
                +-- private acceleration indexes
```

`project_manager::load()` constructs and fully builds a detached Project before replacing the current one. `unload()` destroys the Project as one ownership unit.

## 3. Text / identity / location separation

```text
"Controller" -> string_id          WHAT TEXT
(parent, string_id, kind) -> identity_ref   WHO
identity_ref -> type/object handle          WHERE
```

`string_id` is not semantic identity. `identity_ref` is not a Graph handle. Graph handles are not persisted universal IDs.

## 4. String Table

The String Table canonicalizes immutable text atoms once per loaded Project. It is data-oriented: stable arena bytes, compact hash buckets, and direct paged `string_id` lookup. It has no `std::unordered_map`, `std::list`, version counter, or semantic responsibility.

`identity_node` contains only:

```cpp
identity_ref parent;
string_id name;
identity_kind kind;
```

No Graph pointer, handle, definition state, layout, value, or Runtime address is stored in Identity Space.

## 5. Source architecture

Source Manager owns filesystem/source identity and dependency state:

```text
normalized path -> source_id
source_id -> immutable committed snapshot
source_id -> includes
source_id -> reverse dependents
```

Only explicit Project roots seed discovery. There is no normal-path full project-directory scan.

Incremental updates receive dirty Sources. Changed files are reacquired; unchanged dependents are reparsed from committed snapshots/interfaces without rereading the filesystem. Reverse dependency traversal is sparse.

## 6. Parser contract

Parser performs source-language lookup and emits already-resolved semantic facts.

After Parser resolution:

```text
NO textual semantic lookup in Builder
NO stable-ID lookup
NO canonical-name lookup
```

Types use `identity_ref` or intrinsic codes. Objects use `identity_ref`. Link endpoints use object `identity_ref` plus local `member_index`.

## 7. SourceContribution

`SourceContribution[source_id]` is build-side provenance used for incremental replacement/removal and semantic-delta comparison. It is not part of the canonical Graph and is not Runtime data.

The cache is flat/dense for bulk locality and sparse on incremental replacement. Headroom is established by full build. Incremental capacity exhaustion fails closed and requests an explicit full rebuild instead of relocating O(N) committed arenas.

## 8. Current Graph

Graph is the canonical compiled Project model.

```text
CURRENT GRAPH
  |
  +-- Types
  |     +-- type entries
  |     +-- members
  |     +-- enum values
  |     +-- canonical TypeRefs
  |
  +-- Objects
  |     +-- object entries
  |     +-- object identities
  |
  +-- Links
        +-- resolved source endpoint
        +-- resolved target endpoint
```

Graph contains semantic/static Project information only.

Forbidden in Graph:

```text
Runtime addresses
native C++ reference addresses
live values
execution state
Task scheduler state
SHM mapping addresses
locks/cycle counters
```

## 9. Types

`type_handle` is a 4-byte current-Graph slot handle. `TypeRef` is a 4-byte current-Graph canonical type-expression handle. Intrinsic, named, and derived TypeRefs use direct compact tables/indexes.

Member names and enum names are `string_id`, not duplicated name bytes.

## 10. Objects

A declared Project object belongs in Graph because it exists independently of Runtime materialization.

```text
object identity -> object_handle
object_handle -> object_entry(TypeRef, flags)
```

`object_handle` is 4 bytes and invalid after Project unload.

## 11. Links

A connection such as:

```cpp
B.IN = A.OUT;
```

is compiled before Runtime and therefore belongs in Graph.

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

Canonical Link storage contains no names. One target endpoint has at most one incoming Link; one source may fan out to multiple targets.

## 12. Query by name

Textual query is a boundary service over existing canonical structures, not a second object database.

Examples:

```text
N::T      -> type_handle
N::A      -> object_handle
N::A.OUT  -> object_endpoint
N::B.IN   -> link_handle
```

Fully-qualified strings are not stored per Graph entity. Path segments are resolved through String Table + Identity Space, then mapped to Graph handles/indexes.

## 13. Build orchestration

Full build:

```text
Project configuration
 -> Source Manager acquisition
 -> Lexer/include discovery
 -> Parser
 -> Source facts/interfaces
 -> SourceContribution
 -> Graph Builder
 -> prepare all fallible work
 -> short no-fail publication
 -> current Graph
```

Incremental update:

```text
dirty Sources
 -> reacquire changed physical Sources
 -> old reverse-dependent closure
 -> reparse affected Sources
 -> semantic-delta filter
 -> sparse Builder replacement/removal
 -> prepare all fallible work
 -> short no-fail publication
```

Normal incremental gates require zero unrelated Parser work, zero unrelated Builder work, zero Source dependency full scan, zero Graph full scan, and zero SourceContribution full scan.

## 14. Failure contract

Before publication, failures may allocate diagnostics/candidates but must not modify committed Source Manager, SourceContribution, Graph, or frontend cache state.

Identity/String canonical atoms created during a failed parse may remain until Project unload. They carry no committed definition/runtime state and therefore do not corrupt the current Graph.

## 15. Publication contract

Every potentially failing or allocating operation occurs before publication. Publication performs only pre-reserved/swap/patch operations and is no-fail.

The Server does not retain the previous Graph as version history. Runtime synchronization around Graph publication will be defined at the Runtime integration milestone.

## 16. ABI / Runtime boundary

The next architectural layer is:

```text
Current Graph
   -> Implementation State
        type/member layout
        object storage offsets
        binding plans
   -> Runtime Materializer
        runtime storage
        native objects/references
        execution
   -> SHM/TCP publication/control
```

Implementation State contains deterministic ABI-derived facts only. Runtime owns live addresses and native references. Graph remains semantic-only.

## 17. Frozen rules

1. One loaded Project and one current Graph.
2. No Graph history or Graph generation counter.
3. `string_id` is text identity only.
4. `identity_ref` is Project-lifetime semantic WHO.
5. Types, Objects, and Links are canonical Graph state.
6. Source-language lookup ends in Parser.
7. Builder receives resolved identities/indexes only.
8. Names never participate in execution hot paths.
9. Full/incremental publication is prepared first and no-fail.
10. Project unload destroys all Project-lifetime IDs/handles/state as one ownership boundary.
