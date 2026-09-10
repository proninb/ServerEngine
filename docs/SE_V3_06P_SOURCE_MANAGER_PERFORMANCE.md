# SE-V3-06P — Source Manager Performance Pass

Status: implemented and regression-tested.

## Purpose

SE-V3-06R proved Source Manager correctness and linear scaling, but its benchmark mixed filesystem path normalization with path-identity lookup. That made the reported ~1 microsecond/path cost unsuitable for comparing the V3 identity index with `Server-Entry_OLD`.

SE-V3-06P separates the cold filesystem boundary from the hot normalized-path identity boundary and applies the useful storage findings from the OLD Source Manager layout study without restoring OLD `unordered_map` ownership.

## Frozen semantic contracts

This pass does not modify Project semantic identity, Parser semantic identity, or `source_facts`.

```text
filesystem path
    |
    | normalize once
    v
normalized Source path
    |
    | hot path
    v
XXH64 -> 32-bit fingerprint -> open-addressed path index
    |
    v
source_id
```

Source identity is still one normalized path -> one stable `source_id` for the Project lifetime.

## API split

The cold boundary remains:

```cpp
status source_manager_update::resolve(
    const std::filesystem::path& path,
    source_id& output) noexcept;
```

It performs path normalization and then delegates to:

```cpp
status source_manager_update::resolve_normalized(
    std::string_view normalized_path,
    source_id& output) noexcept;
```

Known-source `resolve_normalized()` and `source_manager::find()` perform no filesystem operation, path normalization, sorting, mutex acquisition, or allocation.

External callers can normalize once with:

```cpp
status normalize_source_path(
    const std::filesystem::path& input,
    std::string& output) noexcept;
```

## Hot storage

Committed paths are split from cold Source state.

```text
source_id -> source_record { path_offset, path_length }   // 8 bytes

path bytes -> one contiguous arena

path index bucket:
    uint32 fingerprint
    source_id source
                                                    // 8 bytes
```

The successful lookup loop reads the compact bucket, directly indexes the dense 8-byte Source record, and performs the final byte comparison in the path arena. It does not route through the public checked `path()` accessor.

The 32-bit fingerprint is only a rejection accelerator. Exact path bytes remain authoritative, so fingerprint collisions cannot alias Source identity.

## Hash

The production path index now uses the XXH64 algorithm shape previously measured by the OLD Source Manager layout benchmark. The bucket stores a non-zero folded 32-bit fingerprint while the full 64-bit value selects the initial open-addressing position.

No hash value is persisted and Source identity does not depend on hash table iteration order.

## OLD comparison

`Server-Entry_OLD` production Source Manager used:

```cpp
std::unordered_map<std::filesystem::path, source_id> by_path;
```

The OLD repository also contained a separate layout microbenchmark for a better experimental `XXH64 + 8-byte bucket` index. That candidate was not the production Source Manager.

The V3 benchmark therefore reports both:

1. an OLD-production lower bound using `unordered_map<filesystem::path, source_id>` with pre-normalized path objects; and
2. the best compact OLD layout-study candidate using the same logical path dataset.

The compact candidate uses a separate copy of query strings so it does not receive the self-alias/cache advantage of comparing a query against the exact same `std::string` object used as stored data.

## Gates

Default benchmark matrix: 100K and 1M Source paths.

Required:

```text
construction scaling exponent                 <= 1.50
path index storage                            compact 8-byte bucket budget
V3 normalized insertion / OLD production      < 1.00
V3 random lookup / OLD production             < 1.00
V3 random lookup / OLD layout candidate       <= 1.20
```

The OLD-layout gate is intentionally a near-parity guard rather than the primary production comparison because that index was an isolated experiment, not the OLD Source Manager implementation.

## Verification result in the Linux reference environment

One full Release run at 1M paths produced:

```text
V3 resolve_unique_normalized       ~276 ns/op
V3 resolve_existing_normalized     ~106 ns/op
V3 find sequential                  ~96 ns/op
V3 find random                     ~458 ns/op
OLD compact layout candidate       ~393 ns/op
OLD production unordered path      ~1157 ns/op
OLD production unordered insert    ~2226 ns/op

V3 / OLD production insert          ~0.124
V3 / OLD production lookup          ~0.396
V3 / OLD layout candidate            ~1.166

path index bytes                  16,777,216
path storage bytes               67,780,000
```

Wall-clock values are machine-dependent; pass/fail ratios and structural contracts are the regression criteria.

## Validation

- Release CTest: 30/30 PASS.
- ASan + UBSan: 30/30 PASS.
- ThreadSanitizer: parallel Parser and parallel frontend root tests PASS.
- changed Source Manager and benchmark translation units: strict warning compile PASS with `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wundef`.
- frozen Project identity directory: byte-for-byte unchanged.
- frozen identity scale/memory gate: PASS.
- frozen identity hardening gate: PASS.

## Next milestone

SE-V3-07 implements Generation Builder + SourceContribution sparse delta using direct `identity_ref` input. Source Manager performance should now be treated as a regression-gated foundation unless a future real workload exposes a new bottleneck.
