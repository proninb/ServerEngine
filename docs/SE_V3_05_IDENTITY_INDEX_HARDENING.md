# SE-V3-05 Identity Index Hardening

## Purpose

SE-V3-04 removed the quadratic sibling walk and passed the 1M scaling and memory-amplification gates. SE-V3-05 does not change the semantic identity architecture. It hardens the fixed semantic index against identifier-shape and collision-distribution failures before the identity layer is frozen.

The semantic contract remains:

```text
source-language spelling
        ↓
one Project semantic resolution
        ↓
canonical identity_ref
        ↓
Parser facts / Generation Builder use direct identity only
```

## Structural telemetry

`identity_space::index_statistics()` scans published immutable bucket chains after a workload and reports:

```text
entry_count
occupied_buckets
collision_entries
max_chain_length
average_chain_length
average_successful_lookup_comparisons
p95_successful_lookup_comparisons
p99_successful_lookup_comparisons
```

The scan is diagnostic/benchmark-only. It does not add counters, branches, locks, or mutation to `resolve_declaration()` and is never part of Generation Builder.

`average_successful_lookup_comparisons` is derived directly from chain positions. If a bucket contains `k` entries, resolving every existing entry once requires positions `1..k`, so the metric describes the actual structural comparison cost of successful lookup independent of CPU timing.

## 1M adversarial identifier workloads

Run:

```text
server_engine_identity_benchmark --hardening
```

The gate builds and re-resolves 1,000,000 identities for each identifier family:

```text
sequential   Type_0000000000 ...
patterned    repeated low-entropy numeric fields
same_prefix  long identical prefix with only the suffix changing
long_name    long identifiers with distributed content variation
```

Every second resolution must return the exact pointer from the first resolution. Identity count must remain exactly `1,000,001` including the root.

## Structural gates

For every 1M workload:

```text
collision_rate                       <= 0.45
max_chain_length                     <= 16
average_successful_lookup_comparisons <= 2.00
p95_successful_lookup_comparisons    <= 4
p99_successful_lookup_comparisons    <= 6
canonical pointer replay             PASS
```

These are structural gates, not wall-clock gates. They detect pathological clustering even when a particular machine happens to produce acceptable elapsed time.

## Frozen identity foundation after PASS

When both SE-V3-04 and SE-V3-05 gates pass, the Project semantic identity foundation is frozen:

```text
Project Context owns Project-lifetime identity
identity_node* is WHO
one semantic scope resolution operation
fixed non-resizing atomic semantic index
no Identity Registry canonicalization pass
no String Registry dependency
no std::mutex
no std::unordered_map
Builder receives identity_ref directly
pointer/allocation/thread order has no semantic meaning
```

The next milestone is SE-V3-06 Source Manager + Parser semantic-scope integration.
