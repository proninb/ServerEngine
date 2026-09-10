# Migration from Server Entry V2

V3 is a clean architectural restart, not an in-place rewrite of the V2 Graph.

## Retained as foundation concepts

- `status`
- `operation_id`
- `source_id`
- diagnostics catalog/buffer model
- Server configuration schema
- Project configuration schema
- explicit target ABI configuration
- strict JSON schema validation and transactional configuration publication

## Deliberately not migrated

- `stable_id.hpp`
- V2 canonical stable-ID allocation
- V2 Builder
- V2 Construction layer
- V2 Graph implementation
- V2 Frontend publication implementation
- V2 Parser implementation
- V2 Runtime implementation
- V2 SHM implementation

Those subsystems remain useful as behavioral/reference material but must not become dependencies of the V3 core.

## V3 architectural replacement

```text
V2
canonical name -> numeric stable_id -> generation-local handle

V3
source lookup -> project-lifetime identity_node* -> generation-local handle
```

The pointer value itself is not persisted. SAVE/LOAD restores semantic identities from serialized structural identity records and creates new process-local addresses.
