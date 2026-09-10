# SE-V3-06R — Production Source Frontend Reuse Integration

Status: implementation candidate.

## Purpose

SE-V3-06R replaces the minimal SE-V3-06B frontend mechanics with production-oriented Source and Parser infrastructure adapted from `Server-Entry_OLD`, while preserving the V3 Project semantic identity contract.

The integration intentionally reuses proven architecture rather than V2 canonical identity. V2 `stable_id`, `string_id`, String Registry canonicalization, and Builder name resolution are not imported.

## Pipeline

```text
Project root paths
    -> Source Manager candidate
    -> immutable acquisition jobs
    -> immutable source_snapshot
    -> Lexer + sparse directive spans
    -> quoted include discovery
    -> stable source_id dependency DAG
    -> dependency-ready semantic waves
    -> Parser language-scope resolution
    -> direct identity_ref / intrinsic types
    -> source_facts
```

## Source Manager contract

- One normalized path maps to one stable `source_id` for Project Context lifetime.
- Included files are Sources regardless of filename extension.
- Candidate mutation is coordinator-owned; filesystem workers receive immutable `source_acquire_job` values only.
- File acquisition checks metadata before read, rechecks after read, and hashes acquired bytes with SHA-256.
- Existing immutable snapshots survive publication of later revisions.
- Include edges are candidate state and the complete candidate DAG is validated before semantic parsing.
- Missing Sources and dependency cycles fail closed.

Persistence and filesystem watcher/change-tracker reuse are deliberately deferred; this milestone establishes the live G0 frontend contract first.

## Lexer and preprocessing boundary

Lexer reports preprocessing directives as sparse token spans. Source frontend interprets only:

- `#include "..."`
- `#pragma once`

Other directives fail closed. Quoted includes inside a namespace or any brace scope are rejected. Directive tokens are removed before Parser invocation. Parser has no filesystem or preprocessing API.

## Semantic environment

Each parsed Source publishes an immutable `source_interface` containing direct project `identity_ref` values. Dependency interfaces are referenced recursively rather than copied. Include position controls when an imported interface participates in lookup within the including Source.

Declaration spelling is resolved once through `project_context::resolve_declaration()`. Type references resolve through Parser-local/source-interface language scope and enter `source_facts` as direct `identity_ref`; Generation Builder must not resolve names again.

## Scheduling

SE-V3-06R keeps the dependency-ready semantics of the OLD frontend but does not port its mutex/condition-variable scheduler. G0 uses deterministic coordinator waves:

1. prepare acquisition jobs in coordinator order;
2. execute independent filesystem jobs in parallel;
3. apply results and include edges coordinator-side;
4. validate the complete dependency DAG;
5. parse each dependency-ready level in parallel;
6. publish immutable Source interfaces after the level joins.

No scheduler mutex is required. Worker/thread identity does not affect semantic identity or publication order.

## `source_facts` extension

The existing V3 boundary is extended additively for functionality present in the OLD parser:

- named enum declaration/definition facts;
- enum values and Parser-interpreted integral constants;
- explicit intrinsic enum underlying type;
- one total lexical `source_declaration_ref` sequence across namespaces, records, and enums.

The declaration sequence prevents Generation Builder from sorting separate fact arrays to reconstruct source order.

## Explicit exclusions

This milestone does not port V2 Graph/Builder identity machinery, Source Manager persistence, filesystem change tracking, macros/conditionals, anonymous records/enums, qualified type names, or the final incremental frontend scheduler.

Graph/Builder reuse is a separate migration milestone.

## Architectural gates

- frozen `project/identity` files unchanged;
- no `stable_id` or `string_id` in Parser/source_facts;
- no filesystem API in Parser;
- no `std::unordered_map` in Parser semantic environment;
- no `std::mutex` or `std::condition_variable` in frontend semantic scheduler;
- no directive reaches Parser;
- `#define` and include-inside-scope fail closed;
- transitive and shared quoted includes preserve direct canonical identity;
- strict warning compile on the new Source/Parser/frontend implementation.

## Scale gate

`server_engine_source_manager_benchmark` measures coordinator-owned normalized path identity independently from filesystem I/O. The default gate compares 100K and 1M unique path resolution and then resolves the same committed paths again. Total unique-resolution scaling must remain at exponent <= 1.50. This gate exists to prevent an accidental linear scan from turning project discovery into O(N^2).
