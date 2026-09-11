# SE-V3-10D0 Compact Project-Local Identity Reference

## Decision

`identity_ref` is no longer a process pointer.

```text
31            30 29                              0
+---------------+---------------------------------+
| identity kind |         Project-local slot      |
+---------------+---------------------------------+
      2 bits                  30 bits
```

The value is exactly 4 bytes. It identifies semantic WHO only inside one
compiled Project baseline/current build. It is not stable across REBUILD or
UNLOAD and is not a `stable_id`.

## Why

The pointer representation prevented a mmap-native `compiled.bin` because every
persisted identity edge had to be relocated or rebuilt after mapping. A compact
local reference makes semantic edges directly relocatable:

- equality is one 32-bit compare;
- hashing uses the numeric value;
- Graph and build-cache identity fields shrink from 8 bytes to 4 bytes on x64;
- persisted identity references can use the same 32-bit representation;
- no process address is semantic identity.

## Identity Space

The semantic index remains construction-only and concurrent. It canonicalizes:

```text
(parent identity_ref, local string_id) -> identity_ref
```

A same-parent/same-name declaration with a different kind is a semantic
conflict.

Metadata is stored in 64 KiB lazy pages. Each slot record is 16 bytes:

```text
identity_node {
    identity_ref parent;     // 4
    string_id    local_name; // 4
}

identity_record {
    identity_node node;      // 8
    uint32 fingerprint;      // 4
    uint32 next_bucket;      // 4, encoded identity_ref
}
```

A two-level lazy page directory avoids reserving a huge flat page table. Bucket
heads are 32-bit encoded `identity_ref` values instead of 64-bit pointers.

Losing concurrent duplicate candidates may consume a slot but never become
published. Such holes are construction pressure, not semantic state, and are
naturally removed by REBUILD.

## Metadata Access

`identity_ref` intentionally has no `operator->`.

Code that needs parent/name metadata carries an explicit `identity_view`. This
prevents a hidden global registry, TLS dependency, or process-global identity
base.

The Parser and Parser interface indexes store their lookup keys explicitly, so
their hot probe paths use only 32-bit identity equality and string_id equality.

`identity_kind` is encoded in the reference and therefore does not require a
metadata lookup for validation.

## Frozen Performance Contracts

- no mutex/shared_mutex in Identity Space;
- no sort;
- no unordered_map;
- no process-pointer hashing;
- no Builder semantic lookup;
- no global identity registry;
- direct `identity_ref -> metadata` is O(1);
- Parser hash probe does not dereference identity metadata;
- Graph identity indexes hash the 32-bit reference directly.

The identity benchmark adds a 1M-identity memory gate of 32 MiB for
`identity_space::bytes_reserved()`.

## Persistence Consequence

This milestone is the prerequisite for `compiled.bin`. D0 does not yet define
the full compiled image format. It only guarantees that semantic identity
references crossing Graph, SourceContribution, Parser interfaces, and future
persisted sections are relocatable numeric values.

The next milestone can therefore encode identity records and Graph/build-cache
references without pointer relocation.
