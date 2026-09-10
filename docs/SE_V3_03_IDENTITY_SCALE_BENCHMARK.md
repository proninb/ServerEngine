# SE-V3-03 Identity Scale Benchmark

## Purpose

SE-V3-03 establishes the scale/performance gate for Project-lifetime semantic identity before Parser and Generation Builder are added.

Correctness tests remain small and are registered as independent CTest cases. Scale testing is a separate executable and is never part of the ordinary CTest path.

## Matrix

The full matrix is:

- identities: 10,000 / 100,000 / 1,000,000;
- scenarios: `create_unique`, `hit_existing`, `mixed_50`;
- workers: 1 / 2 / 4 / 8 / 16.

Output columns:

```text
count,scenario,threads,total_ms,ns_per_op,mops,identity_count,expected_identity_count,reserved_bytes,reserved_pages,status
```

All semantic scenarios use one namespace scope. This is intentional: the benchmark must expose scope-resolution complexity rather than hide it by distributing declarations into artificial scopes.

## Fail-fast scaling gate

Before the full 1M matrix, `--full` runs a 5K -> 20K single-thread `create_unique` probe and estimates the empirical scaling exponent:

```text
time ~= N^p
```

The full matrix is skipped when `p > 1.50`. `--force-full` exists only for explicit investigation and bypasses this protection.

This is a performance gate, not a semantic contract. It prevents a known superlinear implementation from spending a very long time on a 1M run.

## Commands

```text
server_engine_identity_benchmark --gate
server_engine_identity_benchmark --full
server_engine_identity_benchmark --force-full
server_engine_identity_benchmark --single 1000000 1
```

## Current SE-V3-02 expectation

The SE-V3-02 sibling list performs linear child-name resolution. A flat scope therefore has quadratic unique-declaration construction. SE-V3-03 is expected to expose this and block the 1M full matrix until semantic-scope resolution is redesigned.

## Resolution in SE-V3-04

SE-V3-04 replaces sibling-linear semantic resolution with the Project Context semantic scope index and promotes the gate to 100K -> 1M. The historical SE-V3-03 failure remains the baseline proving why the index is required.
