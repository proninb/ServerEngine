# SE-V3-06A Source Facts Contract

## Status

Implemented contract baseline between Parser and Generation Builder.

This milestone does **not** implement the Source Manager, a C++ Parser, Generation Builder, or Graph publication. It freezes the representation those components must use at their boundary.

## Architectural boundary

```text
immutable Source snapshot
        │
        ▼
      Parser
        │
        │ C++ semantic resolution exactly once
        ▼
 project_context::resolve_declaration()
        │
        ▼
    identity_ref
        │
        ▼
   source_facts
        │
        ▼
Generation Builder
```

Once an identifier has become `identity_ref`, downstream construction never receives an unresolved qualified name, `string_id`, numeric semantic ID, or a request to canonicalize it again.

## `source_facts`

`source_facts` is an immutable, zero-allocation, non-owning view. The producer owns the Source text and flat arrays for at least the duration of Generation Builder consumption.

The packet carries:

- `source_id` for one normalized Source;
- immutable Source bytes for exact spelling/range access without copying;
- namespace contribution facts with canonical `identity_ref`;
- C++ record declaration/definition facts with canonical `identity_ref`;
- non-static instance data-member facts;
- resolved type references;
- flat type-modifier storage.

`source_facts` is transient process memory. It is never serialized, persisted, or published through SHM.

## Type references

A source type base is exactly one of:

```text
Project type      -> identity_ref
intrinsic type    -> intrinsic Language/ABI code
```

Examples:

```text
B* value
    base       = B*
    modifiers  = [pointer]

const B* value
    base       = B*
    modifiers  = [const_qualified, pointer]

int count
    base       = intrinsic_type::signed_int
    modifiers  = []
```

Modifiers are applied from the base outward, left to right. This order is semantic and is already established by Parser; Generation Builder does not reparse declarator spelling.

Builtin types do not receive Project semantic identities.

## Flat-array contract

Nested vectors are forbidden at this boundary. Record members and type modifiers are represented by dense `{begin,count}` ranges.

For each record definition:

```text
record.members -> contiguous range in source_facts::members()
```

For each member type:

```text
member.type.modifiers -> contiguous range in source_facts::modifiers()
```

Record definitions collectively partition the member array exactly. Member type references collectively partition the modifier array exactly. Empty ranges still point at the current dense cursor.

Facts are emitted in deterministic source declaration order. Generation Builder must not sort a source packet to reconstruct source order.

The member owner is the enclosing `source_record_fact::identity`; it is deliberately not repeated in every member record.

## Source text and diagnostics

Names that are not Project semantic identities, such as instance-member names, are represented as byte ranges into the immutable Source snapshot.

```text
member.name = {offset,length}
```

This avoids a transient string registry and avoids copying member names. The same ranges provide exact diagnostic locations.

Build Context must keep the referenced Source snapshot alive until all consumers of its `source_facts` packet have finished.

## Validation boundary

`validate_source_facts()` is a fail-closed structural validator. It performs no semantic name resolution and is not part of the normal semantic lookup hot path.

It validates:

- valid `source_id` and 32-bit source-range bounds;
- namespace/type identity kinds;
- exact member/modifier partitions;
- deterministic source ordering;
- source name/type ranges;
- exactly one type base: semantic identity or intrinsic code;
- type identities are `identity_kind::type`;
- supported modifier kinds and structural payload rules.

It intentionally does **not** revalidate C++ language semantics such as redeclaration compatibility, reference legality, access rules, or declarator grammar. Parser owns those semantics; Generation Builder later owns cross-source merge and generation semantics.

Malformed packets can be translated into the `parser.invalid_source_facts` diagnostic without lookup or reparsing.

## Frozen SE-V3-06A contract

1. `source_facts` is immutable and non-owning.
2. Source snapshot/array ownership belongs to Build/Parser storage, not Generation Builder.
3. Every project type/namespace reference in facts is already an `identity_ref`.
4. Builtin types use intrinsic Language/ABI codes, not allocated semantic identities.
5. Unresolved names never cross the Parser -> Builder boundary.
6. Generation Builder performs no source-language name lookup or identity canonicalization.
7. Per-source facts preserve deterministic source order; Builder does not sort them to recover it.
8. Type modifiers are stored base-outward, left to right.
9. Members/modifiers use flat dense ranges; no nested owning containers are part of the contract.
10. Member names are Source byte ranges, not semantic IDs and not registry entries.
11. `identity_node` and the frozen Project identity layer are unchanged by this milestone.
12. `source_facts` pointers/spans are transient and are never serialized or exposed through SHM.

## Scope of contract v1

The first contract represents namespaces, C++ record declarations/definitions, non-static instance data members, intrinsic/project type bases, cv qualifiers, pointers, references, and bounded/unbounded arrays.

Base classes, bit-fields, static objects, functions, aliases, enumerations, templates, and other C++ constructs require additional orthogonal fact arrays in later milestones. They must not be encoded by overloading the existing fields.

## Next milestone

SE-V3-06B implements Source Manager immutable snapshot ownership and a minimal Parser producer that creates this exact `source_facts` representation while performing source-language semantic resolution exactly once.
