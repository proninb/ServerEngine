# Server Entry Architecture V3

**Status:** Proposed New Architecture Baseline  
**Scope:** Project identity, Source Manager, Parser, Generation Builder, MVCC graph `G`, SAVE/LOAD, client-query entry, SHM publication  
**Supersedes:** Numeric `stable_id` as the core in-process canonical identity mechanism  
**Out of scope:** Task execution architecture behind SHM, protocol details, UI/Studio design, implementation micro-optimizations not required by the contracts below

---

## 1. Architectural Decision

The Server uses **stable-address project-lifetime identity objects** as canonical in-process identity.

```text
semantic identity      = identity_node*
generation location    = uint32 generation handle
serialized reference   = file-local dense integer
SHM/runtime reference  = SHM-specific representation
```

There is no numeric project `stable_id` in the core construction architecture.

A single number is not required to simultaneously represent:

- semantic identity;
- cross-generation identity;
- location inside one `G`;
- serialization identity;
- cross-process identity.

Each identity domain has one representation appropriate to its lifetime and purpose.

---

## 2. Primary Goals

The architecture is optimized for:

1. one Server process owning one Project Context;
2. parallel source parsing;
3. one source-language name-resolution operation per reference;
4. no repeated canonical-name lookup in Generation Builder;
5. O(1) identity comparison by pointer equality;
6. deterministic semantics independent of worker scheduling;
7. immutable MVCC generations;
8. very fast sparse `Gₙ → Gₙ₊₁` construction;
9. very fast `SAVE G0` / `LOAD G0`;
10. no Server-process pointers crossing the SHM boundary;
11. one authoritative compiled project state: `G`.

---

## 3. Top-Level Architecture

```text
                           PROJECT CONTEXT
                                 │
          ┌──────────────────────┼──────────────────────┐
          │                      │                      │
   String Registry        Identity Registry       Source Manager
                                 │                      │
                                 │               Source dependency /
                                 │                visibility state
                                 │                      │
                                 │               parallel Parsers
                                 │                      │
                                 └──────────┬───────────┘
                                            │
                                            ▼
                                  resolved source facts
                                  with identity references
                                            │
                                            ▼
                                     Generation Builder
                                            │
                              merge / validate / ABI / layout
                                            │
                                            ▼
                                      immutable Gₙ
                                      (MVCC state)
                                      ┌─────┴─────┐
                                      ▼           ▼
                                client queries   SHM materialization
                                                  │
                                           process boundary
                                                  │
                                                  ▼
                                             Task processes
```

### Ownership

`Project Context` owns the project lifetime and therefore owns:

- project configuration;
- Source Manager;
- String Registry;
- Identity Registry and its stable storage;
- current and retained MVCC generations;
- SAVE/LOAD coordination;
- client-query access to `G`;
- SHM materialization/publication.

---

## 4. Authoritative State

The authoritative compiled project state remains:

```text
G = E ∪ R ∪ V
```

where:

- `E` = generation entities;
- `R` = generation relations;
- `V` = generation values.

`Gₙ` is immutable after publication.

The Identity Registry is **not** a second project database. It stores only project-lifetime identity atoms and acceleration metadata. It does not store generation-specific semantic state.

Fundamental separation:

```text
identity_node = WHO
Gₙ            = WHAT in generation n
```

---

## 5. Identity Domains

The architecture explicitly separates identity by lifetime.

| Domain | Representation | Lifetime | Meaning |
|---|---|---|---|
| Project semantic identity | `identity_node*` | Project Context | Which canonical entity/type |
| Project acceleration | `identity_slot` | Project Context | Dense process-local lookup slot; not identity |
| Generation location | `entity_handle`, `type_handle`, etc. | One `Gₙ` | Where the entity is in this generation |
| Serialized reference | file-local `uint32_t` | One graph file | Location/reference inside saved file |
| SHM/runtime reference | offset/index/runtime-specific ref | SHM/runtime contract | Cross-process runtime access |

No representation is promoted outside its domain.

---

## 6. Hierarchical Canonical Identity

A fully materialized string such as:

```text
company::control::devices::temperature_controller
```

is not the primary canonical key.

Identity is structural and hierarchical:

```text
root*
  │
  └── company*
        │
        └── control*
              │
              └── devices*
                    │
                    └── temperature_controller*
```

A child identity is interned by:

```text
(parent identity pointer, local string_id, identity category if required)
```

Conceptual representation:

```cpp
class identity_node final {
public:
    [[nodiscard]] const identity_node* parent() const noexcept;
    [[nodiscard]] string_id name() const noexcept;
    [[nodiscard]] std::uint32_t slot() const noexcept;

private:
    const identity_node* parent = nullptr;
    string_id local_name{};
    std::uint32_t identity_slot = 0;
};
```

The exact identity key may include a category when the language model permits different canonical entities with the same parent/name pair.

### Canonical full names

Full names are derived representations used only when needed for:

- diagnostics;
- logging;
- Studio/client display;
- text export;
- debugging.

The hot construction path must not require repeated construction, allocation, hashing, or comparison of fully qualified name strings.

---

## 7. Identity Registry

The Identity Registry performs interning:

```text
(parent*, local_name [, category])
              │
              ▼
        identity_node*
```

Required properties:

1. exactly one live `identity_node` for one project semantic identity;
2. node address remains stable for the entire Project Context lifetime;
3. node addresses are never reused for another identity during that lifetime;
4. equality of identity references is pointer equality;
5. creation order and address order have no semantic meaning;
6. the registry supports concurrent parser activity without serializing complete source publication;
7. nodes are allocated from non-moving storage.

Suitable storage is a stable arena, segmented arena, page pool, or equivalent non-moving allocation scheme.

A reallocating `std::vector<identity_node>` is not valid stable storage.

---

## 8. `identity_slot` Is an Acceleration Index, Not Identity

Each identity may receive a dense process-local slot:

```cpp
using identity_slot = std::uint32_t;
```

The slot may be assigned by a simple concurrent allocator such as `fetch_add()`.

The assignment is allowed to depend on discovery scheduling because:

```text
identity_slot ≠ semantic identity
```

The slot:

- is not persistent;
- is not serialized as semantic identity;
- is not exposed as an external project ID;
- is not required to be deterministic across runs;
- may be used for direct dense mappings in each generation.

Example:

```text
identity A* -> slot 417

G10.identity_to_type[417] -> type_handle 15
G11.identity_to_type[417] -> type_handle 9
G12.identity_to_type[417] -> type_handle 21
```

This enables O(1) identity-to-generation lookup without a hash table in the steady state.

---

## 9. Identity Must Never Point Authoritatively to a Generation

This is a mandatory invariant.

Allowed dependency:

```text
Gₙ ──► identity_node
```

Forbidden authoritative dependency:

```text
identity_node ──► Gₙ
```

`identity_node` must not contain generation-specific references such as:

```text
graph*
type_entry*
entity_entry*
current type_handle
current definition
current members
current layout
current ABI state
```

Reason:

```text
identity lifetime > generation lifetime
```

and multiple retained MVCC generations may map the same identity to different handles and different state simultaneously.

No mutable `current G` shortcut is part of the baseline.

---

## 10. Parser Responsibility

Parser owns source-language semantics and performs C++ name resolution using the source environment supplied by Source Manager.

Parser is responsible for:

- lexical structure;
- grammar;
- namespaces and lexical scopes;
- declaration visibility;
- supported C++ lookup;
- nested type scope;
- declarator structure;
- ordered TypeRef modifiers;
- source ranges and provenance;
- source-language diagnostics;
- supported-language capability validation.

### New V3 rule

Once Parser has resolved a source-language reference, it obtains the project canonical identity immediately:

```text
source spelling "B"
      │
      ▼
C++ source lookup
      │
      ▼
resolved semantic declaration
      │
      ▼
Identity Registry
      │
      ▼
identity_node* B
```

Generation Builder must not repeat name resolution or canonical-name lookup.

### Scheduling invariant

```text
parser scheduling ≠ source semantics
```

Parser must derive source properties only from the valid source semantic environment, not from mutable state being produced by another worker or by an unpublished generation.

---

## 11. Source Facts

`source_facts` is the transient result for one parsed Source and lives in reusable `source_context` storage.

It may contain:

- identity references;
- resolved source declarations;
- source TypeRefs;
- ordered modifiers;
- members;
- source order;
- ranges;
- provenance;
- diagnostics;
- source-semantic properties valid for that parse environment.

Example:

```cpp
struct source_type_ref {
    const identity_node* base = nullptr;
    type_modifiers modifiers;
};
```

For:

```cpp
namespace N {
    struct B;

    struct A {
        B* value;
    };
}
```

Parser facts are semantically equivalent to:

```text
A.identity          -> identity N::A*
A.value.type.base   -> identity N::B*
A.value.modifiers   -> [Pointer]
```

Builder does not later ask what `B` means.

---

## 12. Source Properties vs Generation Properties

Pointer identity does not authorize Parser to read arbitrary current project state.

Two property domains are distinct.

### Source-semantic properties

Parser may know immediately, according to its source environment:

- visible declaration;
- record/enum/type category;
- whether the declaration is complete at the relevant source point;
- language qualifiers;
- source declarations and members that are semantically visible;
- declarator properties.

### Generation properties

Only `Gₙ` owns:

- committed definition state;
- canonical relations;
- ABI size/alignment;
- member offsets;
- generation member tables;
- final canonical layout;
- committed values;
- generation-specific validation results.

`identity_node` stores neither domain as mutable project state.

---

## 13. Reusable `source_context`

Each parser worker owns reusable working storage.

Conceptually:

```cpp
struct source_context {
    token_buffer tokens;
    line_buffer lines;
    source_facts facts;
    diagnostic_buffer diagnostics;
    parser_scratch scratch;

    void reset() noexcept;
};
```

Execution model:

```text
Source A
   ↓
parse into source_context
   ↓
publish/consume source_facts
   ↓
source_context.reset()
   ↓
Source B
```

`reset()` clears logical contents while preserving reusable capacity.

The number of contexts follows parser concurrency, not source count.

---

## 14. Parser → Generation Builder Boundary

The boundary is now:

```text
Source
  ↓
Lexer
  ↓
Parser
  ↓
resolved source_facts containing identity_node*
  ↓
Generation Builder
```

The Generation Builder does not perform source-language lookup and does not translate a canonical name into numeric identity.

`source_facts` remains transient. Generation Builder must not retain references to reusable Parser-owned storage after synchronous consumption, except project-lifetime `identity_node*` references whose ownership belongs to Identity Registry.

---

## 15. Generation Builder Responsibility

The former canonical identity assignment role is removed.

Generation Builder owns generation construction:

- merge source contributions;
- declaration/definition reconciliation;
- affected dependency closure;
- canonical generation relations;
- ABI normalization;
- layout;
- generation validation;
- dense handle assignment;
- construction and publication of immutable `Gₙ₊₁`.

Conceptually:

```text
resolved source contributions
          │
          ▼
    Generation Builder
          │
          ├── merge
          ├── validate
          ├── affected closure
          ├── ABI
          ├── layout
          ├── compact/dense handles
          └── publish
          │
          ▼
         Gₙ₊₁
```

---

## 16. Construction Representation

During construction, identity relations may use direct identity references:

```cpp
struct construction_member {
    const identity_node* owner = nullptr;
    const identity_node* type = nullptr;
};
```

Advantages:

- O(1) identity equality;
- no stable-ID lookup;
- no canonical-name comparison;
- no collision-based semantic decision;
- no publication-order identity assignment;
- direct dependency edges between canonical identities.

Pointer numeric ordering is never semantic ordering.

---

## 17. Committed `G` Uses Dense Generation Handles

Pointers are not used as a replacement for all graph handles.

`Gₙ` uses compact generation-local handles for dense relations.

Conceptually:

```cpp
class type_handle final {
public:
    constexpr type_handle() noexcept = default;

    [[nodiscard]] constexpr std::uint32_t value() const noexcept {
        return slot;
    }

private:
    std::uint32_t slot = 0;
};
```

A generation type entry may contain its project identity:

```cpp
struct type_entry {
    const identity_node* identity = nullptr;
    std::uint32_t size = 0;
    std::uint32_t alignment = 0;
};
```

A member relation uses a dense generation handle:

```cpp
struct member_entry {
    string_id name{};
    type_handle type{};
    std::uint32_t offset = 0;
};
```

Therefore:

```text
identity_node* = WHO
type_handle    = WHERE in this G
type_entry     = WHAT in this G
```

---

## 18. MVCC Contract

A canonical identity may appear at different handles and with different state in different generations.

Example:

```text
identity N::A*
      │
      ├── G10 -> type_handle 15
      ├── G11 -> type_handle 9
      └── G12 -> type_handle 21
```

This is correct.

`G10`, `G11`, and `G12` may coexist for readers.

Publication of `Gₙ₊₁` must not mutate `Gₙ`.

Pointer identity provides cross-generation matching; it does not provide generation state.

---

## 19. Removal and Re-Addition

Identity Registry is monotonic for the Project Context lifetime.

Example:

```text
G1: N::A exists
G2: N::A removed
G3: N::A absent
G4: N::A exists again
```

The same `identity_node*` is reused when the same semantic identity is resolved again.

An identity node may exist without appearing in the current `G`.

Existence of an `identity_node` does not mean the entity exists in the published generation.

Identity node addresses are never reassigned to another semantic identity during the Project Context lifetime.

---

## 20. Failed Builds

A failed build may intern identities that never become part of a committed `G`.

This is safe because an identity node contains no authoritative generation state.

```text
identity exists in registry
        ≠
entity exists in G
```

Failed construction must not mutate the current committed `Gₙ`.

Unpublished generation storage is discarded or recycled according to ownership rules.

---

## 21. Parallelism and Determinism

Parser workers may discover identities in arbitrary scheduling order.

Example:

```text
Worker 7 -> C*
Worker 2 -> A*
Worker 5 -> B*
```

This does not affect semantics.

The following have no semantic meaning:

- node address order;
- `identity_slot` order;
- allocation order;
- worker completion order.

Semantic identity is the interned hierarchical key and, within the Project Context, its unique stable-address node.

No deterministic numeric stable-ID allocation phase is required.

Generation handle assignment may use whatever deterministic or construction-appropriate policy is required for `G` itself; generation handles are not cross-generation identity.

---

## 22. Concurrency Requirement for Identity Registry

Identity interning is now on the Parser hot path and must be designed accordingly.

The registry must not introduce a global lock around complete parsing or publication.

Implementation candidates include:

- sharded hash tables;
- concurrent open-addressing structures;
- parent-local child tables;
- segmented ownership by parent/hash;
- another design validated by benchmark.

The architectural requirement is:

```text
concurrent independent identity resolution
must not serialize the whole build
```

Exact data structure selection is an implementation/benchmark decision.

---

## 23. Structural Hashing

A cached structural hash may be stored as acceleration metadata.

Conceptually:

```text
hash(child) = combine(hash(parent), local_name [, category])
```

The full canonical name string need not be materialized to compute or compare identity keys.

Hash equality is never semantic equality by itself.

Exact key equality remains structural:

```text
parent pointer equality
+
local string_id equality
+
category equality if applicable
```

---

## 24. Builtin Types

Builtin types do not require allocated numeric stable IDs.

V3 may represent builtins as pre-created project identity atoms or another intrinsic representation, provided that:

- Parser can bind them without project-wide textual lookup;
- ABI/layout properties remain generation/configuration state, not mutable identity-node state;
- the representation composes cleanly with TypeRef;
- no external persistent numeric stable-ID requirement is introduced.

The exact builtin representation is intentionally left as a focused follow-up design decision.

---

## 25. SAVE `G0`

Native Server pointers are never serialized.

A graph file uses only file-local dense references plus serialized semantic identity structure.

Conceptual identity section:

```cpp
struct serialized_identity {
    std::uint32_t parent;
    string_id name{};
};
```

Conceptual file:

```text
HEADER
STRING TABLE
IDENTITY TABLE
ENTITY/TYPE TABLES
RELATIONS
VALUES
ABI/LAYOUT DATA
VALIDATION/FORMAT METADATA AS REQUIRED
```

Example identity table:

```text
0 : root
1 : parent=0, name="N"
2 : parent=1, name="A"
3 : parent=1, name="B"
```

A generation entry stores a file-local identity reference:

```text
type[0].identity = 2
type[1].identity = 3
```

Generation relations remain dense generation/file-local handles:

```text
member[0].type = 1
```

### SAVE performance contract

SAVE should be predominantly linear serialization of contiguous `G` storage.

It must not require:

- deterministic project stable-ID allocation;
- canonical-name sorting for identity assignment;
- rebuilding source semantics;
- repeated full-name lookup;
- pointer serialization.

---

## 26. LOAD `G0`

`LOAD` restores a compiled graph without Parser/Generation Builder semantic reconstruction.

Preferred sequence:

```text
read header / validate format
          ↓
read string table
          ↓
read identity table
          ↓
restore/intern identity nodes in parent-before-child order
          ↓
build file_identity_id -> identity_node* map
          ↓
read dense G arrays
          ↓
patch only generation entries that hold identity_node*
          ↓
build identity_slot -> generation-handle mappings
          ↓
publish G0
```

Relations represented by dense handles do not require pointer fixup.

For a graph with many relations, this avoids converting every relation from a serialized ID to a native pointer.

### LOAD correctness

After restart, pointer values are expected to differ:

```text
run 1: N::A -> address X
run 2: N::A -> address Y
```

Semantic continuity is restored from the serialized hierarchical identity structure, not from pointer values.

---

## 27. LOAD Followed by Incremental Build

After `LOAD G0`, Identity Registry contains the restored project identities.

When Parser later resolves the same semantic identity:

```text
source N::A
    ↓
Identity Registry
    ↓
existing identity_node* A
```

`G0 → G1` therefore uses the same project-lifetime identity pointer established by LOAD.

No persistent numeric stable ID is required for cross-generation matching inside that Server process.

---

## 28. Client Query Path

The Server may service Studio/client requests against a selected MVCC generation.

Canonical identity lookup and generation state lookup are separate:

```text
query semantic identity
        ↓
identity_node*
        ↓
selected Gₙ
        ↓
identity_slot -> handle
        ↓
generation entry
```

The query must explicitly operate on a generation/view. Identity itself must not contain an implicit mutable `current G` pointer.

---

## 29. SHM Boundary

Server canonical pointers are process-private.

```text
SERVER PROCESS

identity_node*
      ↓
Gₙ
      ↓
SHM materializer
=========================== process boundary
      ↓
SHM runtime representation
      ↓
Task processes
```

The following must never be published as cross-process references:

- `identity_node*`;
- `graph*`;
- `type_entry*`;
- other Server virtual addresses unless a separate explicitly fixed-address runtime contract defines them.

SHM uses the representation defined by the Task/runtime memory contract: offsets, runtime indices, fixed-region addresses, or another explicit cross-process format.

Canonical construction identity and SHM runtime identity are deliberately separate systems.

---

## 30. Performance Model

### Source/build hot path

```text
source name lookup
      ↓
identity intern/resolve once
      ↓
identity_node*
      ↓
pointer compare / pointer dependency edges
      ↓
Generation Builder
```

Avoided repeated work includes:

- fully qualified string materialization in the hot path;
- long canonical-name comparisons;
- numeric stable-ID allocation;
- stable-ID-to-record lookup;
- stable-ID deterministic ranking/sorting;
- collision resolution as semantic identity logic.

### Incremental build

Cross-generation identity comparison is:

```cpp
old_identity == new_identity
```

Generation lookup can be:

```text
identity->slot()
      ↓
G.identity_to_type[slot]
```

### SAVE

Predominantly sequential serialization of dense arrays.

### LOAD

Predominantly sequential reading, identity rematerialization/interning, and dense mapping construction.

The actual data structures must be benchmarked; architectural claims do not replace measurement.

---

## 31. Memory Trade-Off

Project-lifetime identity nodes are intentionally retained until Project Context destruction.

This trades bounded monotonic identity-memory growth for:

- stable pointer identity;
- simple lifetime rules;
- no ABA identity reuse;
- no cross-generation lifetime synchronization;
- cheap removal/re-addition;
- simple failed-build handling.

Identity nodes must remain small so this trade-off stays favorable at large scale.

Generation-heavy state must never be retained in identity nodes.

---

## 32. Explicitly Removed V2 Mechanisms

The following are removed from the core architecture:

```text
numeric stable_id as semantic identity
canonical_name -> stable_id allocation
stable_id deterministic ranking
stable_id sorting for clean-build reproducibility
stable_id as cross-generation match key
Builder source/canonical-name re-resolution
stable_id persistence as a requirement for G0 SAVE/LOAD
```

If a future external protocol requires a persistent public numeric identifier, it must be designed as a separate external identity contract and must not automatically become the core Server semantic identity representation.

---

## 33. Mandatory Invariants

```text
1. One Project Context owns one project identity space.
2. identity_node address is stable for the Project Context lifetime.
3. One semantic identity maps to one identity_node within that Project Context.
4. identity allocation order has no semantic meaning.
5. identity_slot is acceleration metadata, not identity.
6. identity_node contains no authoritative generation-specific state.
7. identity_node never authoritatively references Gₙ.
8. Gₙ may reference identity_node.
9. Gₙ is immutable after publication.
10. Multiple G generations may map one identity to different handles/state.
11. Parser resolves source-language meaning.
12. Parser binds resolved meaning to project identity once.
13. Generation Builder does not repeat source-language or canonical-name lookup.
14. Parser scheduling does not change source semantics.
15. source_facts is transient and reusable-context owned.
16. G is the only authoritative compiled project state.
17. Failed builds never mutate committed G.
18. Existence of identity_node does not imply existence in current G.
19. Native Server pointers are never serialized as persistent identity.
20. Native Server identity pointers never cross the SHM boundary.
21. G internal relations use dense generation handles where appropriate.
22. SAVE/LOAD use file-local references, not project stable IDs.
23. LOAD restores semantic identity from serialized structure.
24. Client state lookup is performed against an explicit G/view.
```

---

## 34. Architecture Summary

```text
                         PROJECT CONTEXT
                               │
                   ┌───────────┴───────────┐
                   │                       │
             String Registry        Identity Registry
                                           │
                                hierarchical interned atoms
                                           │
                                      identity_node*
                                           │
                                           │
Source Manager                              │
      │                                    │
      ▼                                    │
parallel Parser workers ───────────────────┘
      │
      │ source lookup + direct identity binding
      ▼
resolved source_facts
      │
      ▼
Generation Builder
      │
      ├── merge
      ├── validate
      ├── dependency closure
      ├── ABI/layout
      └── dense generation handles
      │
      ▼
immutable Gₙ
      │
      ├── identity_node* on canonical entries
      ├── uint32 handles for dense relations
      └── identity_slot -> handle acceleration maps
      │
      ├──────────────────────────────┐
      ▼                              ▼
client queries                   SAVE / LOAD
      │                              │
      │                         file-local IDs
      │                              │
      └──────────────┬───────────────┘
                     │
                     ▼
              SHM materialization
                     │
              process boundary
                     │
                     ▼
                Task processes
```

---

## 35. Short Contract

```text
Semantic identity inside one Project Context is a stable-address interned identity object.

Parser resolves source meaning and binds it directly to that identity.
Generation Builder builds state, not identity.

identity_node = WHO.
Gₙ            = WHAT.
handle        = WHERE in one G.
file ID       = WHERE in one saved file.
SHM ref       = runtime cross-process representation.

Identity never points authoritatively to a generation.
A generation may point to identity.

No numeric core stable_id is required.
No full canonical-name string is required in the hot path.
No Server identity pointer crosses SAVE/LOAD or SHM boundaries.

G remains the only authoritative compiled project state.
```
