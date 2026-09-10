# V3 Bootstrap Status

## Implemented

- [x] Standalone repository layout
- [x] C++20 build
- [x] Diagnostics foundation
- [x] Server configuration contract and loader
- [x] Project configuration contract and loader
- [x] Project Context
- [x] Project Context-owned semantic identity space
- [x] Stable-address `identity_node` objects
- [x] Project-lifetime immutable identifier bytes
- [x] Direct `identity_ref` result from declaration resolution
- [x] Concurrent convergence of duplicate declarations to one published identity
- [x] Identity contains no compiled Graph state
- [x] Foundation tests
- [x] Sample Server/Project configuration
- [x] Parser -> Generation Builder `source_facts` contract
- [x] Direct `identity_ref` / intrinsic type representation in source facts
- [x] Structural source-facts validation and diagnostics

## Explicitly absent

- [ ] numeric semantic `stable_id`
- [ ] Identity Registry
- [ ] String Registry in semantic identity
- [x] Source Manager transactional path identity + immutable acquisition/snapshots
- [x] Lexer directive spans + quoted include DAG / positional visibility orchestration
- [x] Dependency-ready parallel Parser -> `source_facts` production
- [ ] Full Parser V3 language coverage
- [x] Generation Builder full build
- [x] sparse incremental current-Graph update
- [x] end-to-end Project build orchestration
- [ ] compiled Graph persistence
- [ ] Runtime publication
- [ ] SHM materialization
- [ ] TCP/query service

## Frozen invariant gates

1. `project_context` owns Project-lifetime semantic identity.
2. `identity_node*` is the canonical in-process semantic identity for the Project Context lifetime.
3. One semantic declaration resolution returns `identity_ref` directly.
4. The public resolution API has no `created` result and exposes no allocation history.
5. The identity backing arena performs allocation only; it does not canonicalize or resolve names.
6. Concurrent declarations of the same semantic entity converge on one published pointer.
7. `identity_node` contains no compiled Graph state and never authoritatively references the current Graph.
8. Graph-local handles are not Project semantic identity.
9. No Server pointer is serialized or used as SHM identity.
10. Configuration publication is transactional: failed load never modifies the previous output configuration.
11. No numeric Project `stable_id` exists in V3 core.
12. No `std::mutex` or `std::unordered_map` exists in the Project semantic identity path.

## Frozen SE-V3-06A source-facts gates

1. `source_facts` is immutable, non-owning, and transient.
2. Project semantic references cross the Parser boundary only as `identity_ref`.
3. Builtins cross the boundary only as intrinsic Language/ABI codes.
4. Unresolved names, `string_id`, `stable_id`, and identity-registry keys are forbidden in Builder input.
5. Member and modifier storage is flat and uses dense ranges.
6. Type modifiers are applied base-outward, left to right.
7. Source facts preserve source order; Generation Builder does not sort to recover it.
8. Member names are zero-copy Source ranges and are not semantic identities.
9. Validation is structural only and performs no name/identity lookup.
10. The frozen Project semantic identity implementation is unchanged.

## SE-V3-06B frontend integration gates

1. Source Manager owns filesystem acquisition; Parser consumes immutable `source_snapshot` only.
2. Publishing a new revision for one `source_id` does not invalidate existing snapshots.
3. Parser declaration resolution returns canonical Project `identity_ref` directly.
4. Type references perform one Parser language-scope lookup and do not invoke Project Context again.
5. Cross-source visibility is supplied as direct visible `identity_ref` values.
6. Parser output publication is transactional and is structurally validated before replacement.
7. Lexer reports directive spans; frontend accepts only quoted `#include` and `#pragma once`, removes directives before Parser, and rejects unsupported directives/include-inside-scope.
8. Dependency-ready semantic levels parse in parallel without a mutex/condition-variable scheduler; coordinator applies Source Manager changes deterministically.
9. Parser production code contains no filesystem/file-I/O API, `std::unordered_map`, or sorting canonicalization.
10. Frozen Project identity implementation remains byte-for-byte unchanged; source_facts is extended additively for enums and total declaration order.

## SE-V3-06R production reuse integration

1. Source acquisition/hash/include-DAG/Lexer scheduling mechanisms are adapted from the proven OLD implementation, not re-invented as a parallel semantic identity system.
2. Source Manager path lookup is coordinator-owned and independently scale-gated at 100K/1M Sources.
3. Shared/transitive includes resolve to one Source identity and direct canonical Project `identity_ref` values.
4. V2 `stable_id`, `string_id`, String Registry canonicalization, Builder lookup, and mutex-based frontend queues are not imported.
5. Source Manager persistence/change tracking and Graph/Builder reuse are deferred to subsequent milestones.
