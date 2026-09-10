# Server Engine Architecture V3

**Status:** Active architecture baseline
**Current implementation milestone:** SE-V3-05 Identity Index Hardening

## 1. Core decision

The Server separates semantic identity by lifetime and purpose.

```text
Project semantic identity = identity_node*
Generation location        = generation-local dense handle
Serialized reference       = file-local integer
SHM/runtime reference      = runtime-specific offset/index
```

No single numeric `stable_id` is used as the universal identity mechanism.

## 2. Top-level ownership

```text
                         PROJECT CONTEXT
                               │
             ┌─────────────────┼──────────────────┐
             │                 │                  │
      Project config    Semantic identity     Source Manager
                              space                │
                               │                  │
                               │             Parser workers
                               │                  │
                               └──────────┬───────┘
                                          │
                                          ▼
                               resolved source facts
                               carrying identity_ref
                                          │
                                          ▼
                                  Generation Builder
                                          │
                                          ▼
                                     immutable G_n
                                    ┌─────┴─────┐
                                    ▼           ▼
                              client queries   SHM materialization
```

`Project Context` owns every object whose lifetime is the Project lifetime. `G_n` owns generation-specific semantic state.

## 3. WHO / WHERE / WHAT

```text
identity_node* = WHO
handle in G_n  = WHERE
entry in G_n   = WHAT
```

For the same semantic entity:

```text
identity N::A*
      │
      ├── G10 -> type_handle 15
      ├── G11 -> type_handle 9
      └── G12 -> type_handle 21
```

Different generation handles are correct. The pointer identity remains Project-lifetime stable.

## 4. Project semantic identity

`project_context` owns one semantic identity space.

Conceptually:

```text
root*
  │
  ├── N*
  │    ├── A*
  │    └── B*
  │
  └── M*
```

An identity is structural:

```text
(parent identity, local identifier, semantic kind)
```

The hot path does not build fully qualified strings such as `N::A` merely to establish identity.

## 5. identity_node

The identity object stores only Project-lifetime WHO information.

Conceptually:

```cpp
class identity_node final {
public:
    identity_ref parent() const noexcept;
    name_ref name() const noexcept;
    identity_kind kind() const noexcept;
};
```

Forbidden inside `identity_node`:

```text
graph*
current generation handle
current definition state
ABI size/alignment
member layout
current values
current generation visibility
```

Those facts belong to `G_n`.

## 6. Project Context is the owner

Identity ownership is not a Builder-lifetime responsibility.

```text
Project Context lifetime
    ├── G0
    ├── G1
    ├── G2
    └── ...

identity_node lifetime
    └──────────────── same Project lifetime
```

Removal from a generation does not destroy semantic identity.

```text
G1: N::A exists  -> A*
G2: N::A absent
G3: N::A absent
G4: N::A exists  -> same A*
```

## 7. One semantic resolution operation

The Project Context exposes one declaration-resolution operation:

```cpp
status project_context::resolve_declaration(
    identity_ref parent,
    std::string_view local_name,
    identity_kind kind,
    identity_ref& identity) noexcept;
```

Its semantic result is only the identity pointer.

There is intentionally no:

```cpp
bool created;
```

Allocation history is not semantic state and must not influence Parser, Builder, diagnostics ordering, serialization, or generation construction.

For repeated declarations:

```cpp
struct A;
struct A;
struct A {};
```

all semantic resolutions produce the same `A*`.

## 8. Meaning of "no lookup"

V3 forbids repeated canonicalization lookup, not source-language name resolution itself.

A C++ identifier must be resolved according to language scope and visibility rules. That mandatory source-language operation is the one place where spelling becomes semantic identity.

```text
source spelling "A"
       │
       ▼
C++ semantic scope resolution      <- one mandatory semantic operation
       │
       ▼
identity_node* A
       │
       ├── Parser facts
       ├── dependency relations
       └── Generation Builder
```

After `identity_ref` exists:

```text
NO Identity Registry lookup
NO stable-ID lookup
NO canonical-name lookup
NO Builder name resolution
```

The Generation Builder consumes pointers and generation-local handles only.

## 9. No Identity Registry

There is no independent global Identity Registry that performs:

```text
(parent, name, kind)
       ↓
second hash-table lookup
       ↓
identity
```

That would duplicate semantic work already performed by source-language resolution.

Project semantic identity is part of the Project Context itself.

## 10. No String Registry in semantic identity

Semantic identity does not require:

```text
"A" -> String Registry -> string_id -> Identity Registry -> A*
```

The Project Context owns immutable identifier bytes through `name_ref`.

```text
identifier bytes
      │
      ▼
Project semantic resolution
      │
      ▼
identity_node*
```

A separate string system may later exist for diagnostics, serialization, client output, or Graph compaction, but it is not a prerequisite for semantic identity.

## 11. Identity storage

The backing `identity_arena` is a memory primitive only.

Responsibilities:

```text
allocate stable Project-lifetime bytes
preserve node addresses
preserve identifier bytes
```

It performs no:

```text
name lookup
identity lookup
canonicalization
hashing
sorting
semantic validation
```

The arena is hidden behind Project Context semantic identity; Parser and Builder do not call it directly.

## 12. Semantic resolution index and concurrency

Project semantic identity contains no `std::mutex` and no `std::unordered_map`.

`identity_space` implements the one mandatory source-language semantic resolution with a fixed, non-resizing atomic bucket index keyed by parent identity and local identifier bytes. Collision traversal belongs to that one semantic resolution operation; it is not a second canonicalization registry and Generation Builder never uses it.

Concurrent attempts to publish the same semantic declaration converge on one canonical published pointer through compare/exchange on the relevant bucket head. Published collision links are immutable.

Allocation is backed by concurrent page-based bump storage. A losing speculative identity candidate is not semantic state and may remain unused until Project teardown. Replacement arena pages become Project-owned only after they win `current_page` publication; losing page candidates are released immediately.

Creation/allocation order has no semantic meaning.

```text
worker scheduling != project semantics
pointer address    != semantic ordering
allocation order   != serialization ordering
```

## 13. Determinism

Pointer values and allocation order may differ between runs.

Deterministic output must therefore be defined independently of:

```text
pointer value
thread schedule
CAS winner
allocation order
```

Diagnostics, SAVE artifacts, and external enumeration must define their own deterministic ordering from semantic/source data.

## 14. Parser boundary

Parser owns C++ language semantics:

```text
lexical scope
namespace scope
visibility
redeclaration rules
nested types
declarator structure
source ranges
```

When Parser resolves a declaration/reference, the result is already an `identity_ref`.

Example:

```cpp
namespace N {
    struct B;
    struct A {
        B* value;
    };
}
```

Parser facts are conceptually:

```text
A.identity        = N::A*
value.type.base   = N::B*
value.modifiers   = [pointer]
```

Builder never asks again what `B` means.

## 15. Source facts

SE-V3-06A fixes the Parser -> Generation Builder boundary as an immutable non-owning `source_facts` view. Project semantic entities cross this boundary only as already-resolved `identity_ref` values. Builtin types use intrinsic Language/ABI codes and do not receive allocated Project identities.

The v1 packet contains flat arrays for namespace contributions, C++ record declarations/definitions, non-static instance members, and type modifiers. Members and modifiers are addressed by dense `{begin,count}` ranges rather than nested containers. Member names and diagnostic locations are byte ranges into the immutable Source snapshot held alive by Build Context.

Conceptually:

```cpp
struct source_type_ref {
    identity_ref identity = nullptr;
    intrinsic_type intrinsic = intrinsic_type::none;
    source_fact_range modifiers;
    source_span spelling;
};
```

Exactly one of `identity` or `intrinsic` identifies the base type. Modifier order is base-outward, left to right. Unresolved names, fully-qualified-name keys, `string_id`, and numeric semantic IDs are forbidden in this packet.

Per-source facts preserve deterministic source order. Generation Builder never sorts a packet to recover source order and never performs semantic name/identity lookup.

The facts and Source snapshot may be discarded after Generation Builder consumption. Referenced `identity_node` objects remain valid for the Project lifetime. `source_facts` itself is never serialized or published through SHM.

## 16. Build Context

A future Build/Generation Context owns only one build's transient construction state.

It does not own Project semantic identity.

```text
PROJECT CONTEXT
    │
    ├── identity A*                  WHO
    │
    └── BUILD CONTEXT
            │
            └── construct G_n        transient work
```

Failed build state may be discarded without invalidating Project identities.

## 17. Generation Builder

Generation Builder receives already-resolved identity references.

Responsibilities:

```text
merge source contributions
validate declarations/definitions
calculate affected dependency closure
apply ABI/layout rules
assign generation-local dense handles
publish immutable G_n
```

Forbidden responsibilities:

```text
source-language name lookup
identity canonicalization
stable-ID allocation
fully-qualified-name lookup
```

## 18. Construction representation

During construction, direct pointer relations are allowed:

```cpp
struct construction_member {
    identity_ref owner = nullptr;
    identity_ref type = nullptr;
};
```

Identity equality is pointer equality:

```cpp
left.identity == right.identity
```

Pointer numeric ordering is never semantic ordering.

## 19. Current Graph

Pointers are not a replacement for dense Graph handles. The Server owns exactly one
current compiled Graph. `type_handle` and `TypeRef` are compact addresses inside that
current Graph; they are not Project semantic identity and are not persisted as such.

```text
identity_ref = WHO for Project lifetime
type_handle  = WHERE in the current Graph
```

Full build prepares detached complete storage. Incremental update prepares only sparse
patches and append payload outside publication-sensitive code. No Graph history,
MVCC generation set, generation number, or version-control state is retained.

## 20. Publication

All filesystem, parsing, semantic validation, allocation, and capacity preparation
happens before publication. The Server then applies the already-prepared current-Graph
update in a short no-fail publication boundary.

```text
current Graph
     ^
     | runtime reads while build prepares elsewhere
     |
prepared build/update -> validate -> no-fail publish -> current Graph
```

The previous Graph state is not retained as a historical version after publication.

## 21. Failed builds

A failed build must not mutate the currently published Graph.

Project-level identity created while examining a failed build is semantically harmless because identity existence does not imply presence in the current Graph.

```text
identity exists in Project Context
        !=
entity exists in current Graph
```

No compiled Graph facts are stored in the identity object.

## 22. SAVE / LOAD

Pointers are never serialized.

SAVE maps Project semantic structure to file-local references.

Conceptually:

```cpp
struct serialized_identity {
    std::uint32_t parent;
    std::uint32_t name_offset;
    std::uint32_t name_size;
    std::uint8_t kind;
};
```

LOAD reconstructs Project semantic identities and then reconnects the loaded generation's identity sidecar.

```text
file identities
      ↓
Project Context semantic identity
      ↓
identity_node*
      ↓
G0 identity associations
```

No persisted pointer value is meaningful across process restart.

## 23. SHM boundary

Server-process pointers never cross the process boundary.

```text
Server
 identity_node*
      │
      ▼
 runtime materialization
      │
      ▼
 SHM offset/index/reference
      │
      ▼
Task process
```

Identity representation and SHM representation are intentionally different domains.

## 24. Current frozen contracts

1. One Server owns one Project Context.
2. `project_context` owns Project semantic identity.
3. `identity_node*` means WHO for the Project lifetime.
4. `identity_node` stores no generation-specific state.
5. Declaration resolution returns `identity_ref` directly.
6. There is no `created` output in the semantic API.
7. The backing arena is allocation-only and hidden from Parser/Builder.
8. There is no independent Identity Registry canonicalization lookup.
9. There is no String Registry dependency in semantic identity.
10. No `std::mutex` or `std::unordered_map` is used by Project semantic identity.
11. Builder performs zero name/identity lookup after semantic resolution.
12. `G_n` is immutable after publication.
13. Generation handles are local to one `G_n`.
14. Pointers are never serialized or published through SHM.
15. Determinism never depends on pointer/allocation/thread order.

## 25. Identity index hardening

SE-V3-05 freezes the Project semantic identity foundation only after the 1M semantic index passes structural collision/probe gates across sequential, patterned, same-prefix, and long identifier workloads. Collision telemetry is diagnostic-only and is collected by scanning immutable bucket chains after a workload; it does not instrument the resolution hot path.

The frozen structural gates are:

```text
collision_rate                        <= 0.45
max_chain_length                      <= 16
average successful comparisons       <= 2.00
p95 successful comparisons           <= 4
p99 successful comparisons           <= 6
canonical pointer replay              PASS
```

## 26. SE-V3-06 frontend boundary

SE-V3-06A freezes `source_facts`, the Parser -> Generation Builder representation. Its structural validator confirms that no unresolved semantic reference can cross the boundary and that source/member/modifier ordering and dense ranges are well formed without performing name resolution.

SE-V3-06R replaces the minimal SE-V3-06B frontend mechanics with production-oriented Source infrastructure adapted from `Server-Entry_OLD`: stable normalized-path `source_id`, immutable acquisition jobs, SHA-256 Source snapshots, compact Lexer tokens/directive spans, quoted-include DAG discovery, positional visibility, and dependency-ready parallel semantic waves. The OLD textual canonical-name/String Registry path is not imported.

Parser receives no filesystem or preprocessing API. Source frontend removes the supported `#include "..."` / `#pragma once` directives before Parser invocation; unsupported directives and include-inside-scope fail closed. Parser declaration resolution returns Project-lifetime `identity_ref` directly, and type references use direct Parser/source-interface language lookup without a second Project Context canonicalization.

The key integration path is:

```text
identifier token
    ↓
C++ semantic resolution exactly once
    ↓
identity_ref
    ↓
source_facts
    ↓
all downstream stages use direct identity
```

## SE-V3-06P Source Manager hot path

Source path canonicalization and Source identity lookup are separate operations. External filesystem paths are normalized once; repeated Source identity operations consume the normalized generic path directly. The hot Source Manager path is therefore:

```text
normalized path bytes
    -> XXH64
    -> compact open-addressed bucket
    -> source_id
```

The committed path index uses an 8-byte `{fingerprint, source_id}` bucket. Exact bytes in the contiguous path arena remain authoritative. A dense 8-byte `{offset,length}` record maps `source_id` back to its normalized spelling. Collision resolution performs exact byte comparison; the fingerprint is never identity.

`source_manager_update::resolve(std::filesystem::path)` is the cold compatibility/external boundary. `resolve_normalized(std::string_view)` is the build-side hot boundary and performs no filesystem operation or normalization for an already known Source. Source Manager remains coordinator-owned and contains no mutex.

SE-V3-06P performance gates compare this V3 implementation with the actual OLD production `unordered_map<std::filesystem::path, source_id>` path map and with the separate compact OLD layout-study candidate. V3 must beat the production OLD map; the experimental layout candidate is retained as a near-parity regression guard.
