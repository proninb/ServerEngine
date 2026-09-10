# V3 Bootstrap Status

## Implemented

- [x] Standalone repository layout
- [x] C++20 build
- [x] Diagnostics foundation
- [x] Server configuration contract and loader
- [x] Project configuration contract and loader
- [x] Project Context skeleton
- [x] String registry foundation
- [x] Pointer-based Identity Registry foundation
- [x] Stable-address identity nodes
- [x] Identity has no reference to Graph/generation state
- [x] Foundation tests
- [x] Sample Server/Project configuration

## Explicitly absent

- [ ] numeric semantic `stable_id`
- [ ] Source Manager V3
- [ ] Parser V3
- [ ] Generation Builder
- [ ] immutable Graph generation
- [ ] G0 persistence
- [ ] sparse MVCC update
- [ ] Runtime publication
- [ ] SHM materialization
- [ ] TCP/query service

## First invariant gates

1. `identity_node*` is valid for the Project Context lifetime.
2. Identity node addresses never move after publication.
3. One semantic key `(parent identity, local name, kind)` resolves to one node.
4. Identity Registry is monotonic during Project Context lifetime.
5. `identity_node` contains no generation-specific state.
6. `identity_node` never authoritatively references `G`.
7. No Server pointer is a serialized or SHM identity.
8. Configuration publication is transactional: failed load never modifies the previous output configuration.
9. No numeric project `stable_id` exists in V3 core.
