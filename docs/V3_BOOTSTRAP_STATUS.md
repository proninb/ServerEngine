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
- [x] Identity contains no Graph/generation state
- [x] Foundation tests
- [x] Sample Server/Project configuration
- [x] Parser -> Generation Builder `source_facts` contract
- [x] Direct `identity_ref` / intrinsic type representation in source facts
- [x] Structural source-facts validation and diagnostics

## Explicitly absent

- [ ] numeric semantic `stable_id`
- [ ] Identity Registry
- [ ] String Registry in semantic identity
- [ ] Source Manager V3
- [ ] Parser V3
- [ ] Generation Builder
- [ ] immutable Graph generation
- [ ] G0 persistence
- [ ] sparse MVCC update
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
7. `identity_node` contains no generation-specific state and never authoritatively references `G`.
8. Generation-local handles are not Project semantic identity.
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
