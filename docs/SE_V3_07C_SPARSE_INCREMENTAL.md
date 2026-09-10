# SE-V3-07C — Sparse Incremental Generation Builder

Status: implemented and regression-tested.

## Contract

Incremental build replaces complete SourceContribution state for changed Sources and removes contributions for deleted Sources. The Builder subtracts the previous contribution, adds the candidate contribution, patches only touched Graph-local type slots, and validates only the reverse dependency closure rooted at touched types.

Builder consumes Parser-resolved `identity_ref` values directly. It performs no semantic/name lookup, stable-ID allocation, String Registry canonicalization, semantic sorting, or mutex-based Graph mutation.

## Sparse publication

Committed Graph and SourceContribution append arenas retain incremental headroom established by G0. Incremental prepare is forbidden from relocating committed O(N) arenas. If headroom is exhausted, the update fails closed with `status_code::not_available`; the caller must perform an explicit G0 rebuild. Publication uses only capacity-prepared append/patch operations and is no-fail.

Type handles remain mapped to Project-lifetime identities across incremental updates of the current Graph. Removing the final declaration tombstones its Graph-local type slot; reintroducing the same identity reactivates that handle. A full rebuild may assign different Graph-local handles.

Reverse dependency edges are append-only between explicit full rebuilds. Each owner type has a dependency version; replacing a definition increments the version, making prior outgoing edges stale in O(1). A full rebuild reclaims stale edge storage.

## Telemetry gates

`generation_build_telemetry` reports changed Sources/types, added/removed types, validation visited types/TypeRefs/dependency edges, Graph full scans, and contribution full scans.

The 1M sparse benchmark constructs 1,000,000 Sources with one type each, then performs modify/remove/add of exactly one Source. Required gate:

- `prepare <= 250 us`
- `publish <= 5 us`
- `changed_sources == 1`
- `changed_types == 1`
- `graph_full_scans == 0`
- `contribution_full_scans == 0`

## Verification

- Release functional regression: 39/39 PASS.
- ASan + UBSan: 39/39 PASS.
- ThreadSanitizer: parser and frontend parallel regression PASS.
- Strict changed-TU compile: `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wundef -Werror` PASS.
- Frozen identity scale/memory and hardening gates PASS.
