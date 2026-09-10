# SE-V3-04 Semantic Scope Index

## Purpose

SE-V3-03 proved that the sibling-list implementation of Project semantic resolution was not viable at scale. A flat scope grew almost quadratically because every declaration walked previously published siblings.

SE-V3-04 changes only the internal acceleration structure. The external semantic contract remains unchanged:

```text
(parent identity, local identifier)
        ↓
one source-language semantic resolution
        ↓
canonical identity_node*
```

Generation Builder still performs zero name or identity canonicalization lookup after semantic resolution.

## Semantic index

`identity_space` owns a fixed, non-resizing array of atomic bucket heads. The key is the parent identity plus local identifier bytes. `identity_kind` is validated after a name match so a redeclaration with an incompatible kind reports `semantic_conflict` rather than creating another identity.

Each published record contains an immutable collision-chain link and a precomputed semantic hash. Publication is a compare/exchange on one bucket head. Readers load the bucket head with acquire semantics and then traverse immutable records.

This is the implementation of the one mandatory source-language scope resolution operation. It is not an independent Identity Registry and it is not visible to Parser facts or Generation Builder.

There is no table resizing or rehashing in the hot path. The current implementation uses 2^20 buckets, which is an implementation capacity choice for the 1M-type performance target and is not a serialized or semantic property.

## Identity storage correction

SE-V3-03 also exposed page amplification under concurrent creation. Racing workers could allocate and own multiple replacement arena pages even though only one page won publication as `current_page`.

SE-V3-04 changes page replacement to:

```text
allocate candidate page
        ↓
CAS current_page
   ├── win  -> own page
   └── lose -> delete candidate page
```

Only the winning page becomes Project-owned storage. This preserves stable identity addresses while removing thread-count-dependent page amplification.

## Gates

`server_engine_identity_benchmark --gate` now validates the real target scale:

```text
100K create_unique / 1 thread
1M   create_unique / 1 thread
1M   create_unique / 16 threads
```

It checks:

```text
scaling exponent <= 1.50
16-thread reserved-byte amplification <= 1.10
all identity counts exact
1M canonical identities successfully published
```

`--full` continues to run the complete matrix:

```text
10K / 100K / 1M
create_unique / hit_existing / mixed_50
1 / 2 / 4 / 8 / 16 threads
```

The benchmark reports semantic bucket count and bucket load in addition to timing and Project identity storage metrics.
