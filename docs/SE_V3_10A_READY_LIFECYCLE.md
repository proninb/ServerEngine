# SE-V3-10A READY Lifecycle Isolation

## Contract

Project construction is legal only while the Project is `UNLOADED`.

```text
UNLOADED
    |-- REBUILD --> CONSTRUCTING --> READY
    |                               |
    |                               | QUERY / RUN / SAVE
    |                               |
    +<----------- UNLOAD <----------+
```

`LOAD` and `BUILD` will join the same `UNLOADED -> CONSTRUCTING -> READY`
boundary when persisted section storage is implemented. This change does not
fake either operation before that storage exists.

A failed construction destroys the candidate and returns to `UNLOADED`.
There is no live Graph replacement and no rollback of a READY Graph.

## READY ownership

The Graph is immutable for the entire READY lifetime. Mutable execution state
belongs to Runtime, not Graph.

`project_access` is the only public lifetime token for READY Project work.
It is move-only. Every successful acquire increments the admission gate exactly
once and every token release decrements it exactly once. Pointers, spans and
string views obtained through a token must not escape that token lifetime.

Graph/type/object/link access takes no mutex and no reader lock.

## UNLOAD ordering

UNLOAD is coordinator-owned and does not hold a Project token.

```text
close admission
    -> request stop / wake Runtime and background work
    -> wait until all existing project_access tokens leave
    -> destroy Runtime (when Runtime is integrated)
    -> destroy Project
    -> release mappings/storage
    -> UNLOADED
```

The stop request precedes the drain wait, so a long-running RUN task can observe
stop and release its token instead of deadlocking UNLOAD.

The activity gate is one 64-bit atomic word: high bit is CLOSED, remaining bits
are the active task count. Synchronization occurs once per task boundary, never
inside Graph lookup.

## Builder boundary

`project_manager::rebuild()` creates a detached candidate and calls
`project_build_orchestrator::construct()`. No readers exist during construction,
so the orchestrator no longer owns a shared mutex/publication mutex.

The existing low-level incremental Builder path remains available only as a
construction primitive. It is not exposed as a READY Project mutation.

## Next storage constraint

Persistence must preserve this lifecycle:

- `LOAD`: mapped read-only committed sections -> READY.
- `BUILD`: read-only reuse plus section-selective MATERIALIZE/REBUILD -> READY.
- `SAVE`: writes a new committed baseline without mutating READY.
- `REBUILD`: restores compactness when persisted/storage pressure crosses policy.

No whole-artifact `FILE_MAP_COPY` assumption is permitted.
