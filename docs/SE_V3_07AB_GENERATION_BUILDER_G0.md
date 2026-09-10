# SE-V3-07A+07B — SourceContribution + Generation Builder G0

## Contract

SE-V3-07A+07B closes the first Parser-to-Graph build path without reintroducing V2 semantic canonicalization. Parser supplies Project-lifetime `identity_ref` values in `source_facts`; Generation Builder materializes Graph-local handles and TypeRefs directly from those identities.

```text
source_facts
    -> SourceContribution[source_id]
    -> Generation Builder
    -> detached prepared G0
    -> allocation-free/no-fail publish
    -> immutable Graph G0
```

Builder and Graph construction contain no `stable_id`, `string_id`, String Registry, source-language name lookup, semantic identity lookup, semantic sort, `std::unordered_map`, or mutex-based synchronization.

## SourceContribution

Build provenance remains outside committed Graph. G0 uses a flat dense representation rather than one vector allocation per Source:

- dense `source_id -> source_contribution_state` slots;
- contiguous type declaration arena;
- contiguous member, modifier, enum-value, and name arenas;
- one rebuild preflight/reserve before Source capture;
- candidate state is detached until publication.

This layout is intended to remain practical for the `1M Sources x 1 type` case and forms the provenance base for sparse incremental Source replacement.

## Graph G0

`type_handle` and `TypeRef` are four-byte Graph-local addresses. `identity_ref` remains the Project-lifetime WHO and is stored only in a parallel identity array; hot `type_entry` records contain current compiled state only.

G0 construction uses a build-local pointer index only to map already-resolved `identity_ref` to one Graph-local `type_handle`. That operation is not semantic resolution: keys are exact identity pointers and equality is pointer equality.

Definitions are materialized into compact one-based member or enum-value ranges. TypeRef index zero is invalid. Named TypeRefs refer directly to Graph-local type handles; derived TypeRefs wrap an immediate child and preserve Parser modifier order.

## Publication

`prepare_g0()` performs all allocation, conflict checks, TypeRef materialization, definition materialization, and validation into detached storage. `publish_prepared()` performs only vector/storage swaps into Graph and SourceContribution cache and is therefore allocation-free/no-fail.

A failed prepare leaves both committed Graph and committed SourceContribution unchanged.

## Verification

Correctness includes SourceContribution capture, record/member materialization, enum materialization, legal redeclaration, and duplicate-definition fail-closed behavior.

`server_engine_generation_builder_benchmark` runs a G0 workload matrix through one million real Project identities. The required gate is `1M defined-empty types <= 1000 ms`. A stronger semantic workload is available with:

```text
server_engine_generation_builder_benchmark --semantic 1000000
```

That workload constructs one million types, one million self-pointer members, two million canonical TypeRefs, and one million derived pointer TypeRefs.

## Next milestone

SE-V3-07C adds sparse SourceContribution replacement and incremental publication into the single current Graph without retaining Graph history.
