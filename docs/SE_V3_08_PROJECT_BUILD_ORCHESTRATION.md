# SE-V3-08 — Project Build Orchestration

Status: implemented and regression-tested.

## Frozen current-Graph contract

The Server owns exactly one current compiled Graph. It does not retain historical
Graph versions and Graph storage has no generation counter. `operation_id` identifies
build operations for diagnostics; it is not Graph identity.

```text
Project configuration
        -> Source Manager
        -> acquire/hash
        -> Lexer/include discovery
        -> Parser/source_interface
        -> source_facts
        -> SourceContribution
        -> Generation Builder
        -> prepared full/sparse update
        -> no-fail publication
        -> current Graph
```

A full build prepares detached complete Graph storage. An incremental build prepares
only changed SourceContribution and affected Graph patches. Published history is not
kept.

## Project ownership

`project_context` is the aggregate owner of Project configuration, Project-lifetime
semantic identity, Source Manager, reconstructable Parser interfaces,
SourceContribution provenance, and the current Graph.

`source_frontend_cache` is COLD acceleration state keyed by `source_id`. It owns
`source_interface` objects between builds so unchanged imported interfaces remain
available to the incremental Parser. It is not canonical compiled state and can be
reconstructed by a full build.

## Incremental Source path

Normal incremental work starts from explicit dirty `source_id` values. These values
can later come from a platform file watcher; the watcher is not part of the semantic
build contract.

For each build:

1. reacquire dirty Sources only;
2. discard physical metadata-only changes whose content hash is unchanged;
3. collect the committed transitive dependents of actually changed Sources before
   replacing their old include relationships;
4. reparse affected dependents from committed immutable snapshots without rereading
   their files;
5. acquire only genuinely new Sources discovered through new include directives;
6. validate only Source dependency paths rooted at changed include owners;
7. update reverse dependency relationships only for changed include edges.

Existing-path incremental publication does not rebuild the Source Manager path index.
Newly discovered paths are inserted into existing path-index headroom without an O(N)
rehash; exhausted headroom fails closed and requires a full rebuild.

## Semantic delta boundary

A textual/interface dependency change does not imply a Graph change. After reparsing,
`source_contribution_cache::equivalent()` compares the new Parser facts with the
committed SourceContribution using direct `identity_ref` and structural payload data.
No semantic name lookup or allocation is performed.

This means a changed common header may require many dependents to be reparsed while
Generation Builder still receives only the Sources whose compiled contribution truly
changed.

## Publication

All potentially failing work is completed before publication:

- filesystem acquisition and hashing;
- Lexer/Parser work;
- Source dependency validation;
- Source Manager capacity preparation;
- SourceContribution and Graph validation/materialization;
- frontend-cache capacity preparation.

Authoritative Source Manager/SourceContribution/Graph publication is no-fail. The
reconstructable Parser-interface cache publishes immediately afterward with prepared
capacity and no allocation. Runtime must not be disabled for the complete build; later
Runtime integration only needs synchronization around the short current-Graph
publication boundary.

A failed frontend or Builder candidate leaves committed Source state,
SourceContribution, current Graph, and frontend cache unchanged. New Project-lifetime
`identity_node` objects created while examining a failed build may remain because
identity existence does not mean presence in the current Graph.

## Architecture exclusions

The orchestration path contains no V2 semantic `stable_id`, `string_id`, String
Registry canonicalization, Builder semantic/name lookup, retained Graph history, or
Graph generation counter. The incremental coordinator does not use the OLD
mutex/condition-variable scheduler or `std::unordered_map`/`std::unordered_set`.

## End-to-end gate

`server_engine_project_build_benchmark --gate` uses actual temporary files and covers:

- full build of 8,192 independent Sources;
- one-file content modification;
- one changed common header with 1,024 dependent Sources.

Incremental structural gates require:

- no full path-index rebuild;
- no full Source dependency-graph scan;
- no full Graph scan;
- no full SourceContribution scan;
- only actually dirty physical Sources reacquired;
- dependent Sources reparsed from committed snapshots;
- Generation Builder receives only semantically changed SourceContributions.

The established 1M Builder full/sparse gates remain mandatory regressions.
