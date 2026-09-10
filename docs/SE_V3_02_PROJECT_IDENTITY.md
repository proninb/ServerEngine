# SE-V3-02 — Project Semantic Identity

## Decision

`project_context` owns semantic identity for the Project lifetime.

```text
Project Context
    │
    ├── semantic identity space
    │      ├── root*
    │      └── identity_node*
    │
    └── G0, G1, ... Gn
```

`identity_node*` means WHO. Generation state remains in `G`.

## Resolution contract

```cpp
status project_context::resolve_declaration(
    identity_ref parent,
    std::string_view local_name,
    identity_kind kind,
    identity_ref& identity) noexcept;
```

The operation returns the canonical identity directly. The caller does not receive and must not depend on a `created` flag.

```text
source-language semantic resolution
              │
              ▼
        identity_node*
              │
              ├── parser facts
              ├── dependencies
              └── Generation Builder
```

After this operation there is no Identity Registry lookup, stable-ID lookup, or Builder canonical-name lookup.

## Concurrency

There is no `std::mutex` and no `std::unordered_map` in the semantic identity path. Publication of a newly discovered child identity uses compare/exchange. Concurrent declarations of the same semantic name converge on one published `identity_node*`.

The backing arena is Project-owned, stable-address, and concurrent. It performs allocation only; it has no semantic lookup or canonicalization API.

## Generation boundary

Forbidden inside `identity_node`:

- Graph pointer;
- current generation handle;
- definition state;
- ABI size/alignment;
- members or layout;
- current generation visibility.

Those facts belong to immutable `G_n`.
