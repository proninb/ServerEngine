# SE-V3-10D3C Sparse BUILD Telemetry and Scale Benchmark

## Scope

D3C is an instrumentation/benchmark cut on top of the Windows-verified D3B
sparse mapped-baseline BUILD.

It does not change the D3B build algorithm, persistence format, publication
contract, handle identity rules, or READY representation.

D3C answers one question:

> When a persisted Project is large and one independent Source changes, which
> part of BUILD scales with baseline size and which part scales only with the
> affected semantic set?

## Existing telemetry reused

The existing orchestration pipeline already records:

- frontend time;
- Builder prepare time;
- Source prepare-publish time;
- interface prepare-publish time;
- publication time;
- total orchestrator time;
- dirty / changed / affected / acquired / lexed / parsed Sources;
- reused Source interfaces;
- Source graph visits;
- reverse edge patches;
- changed Builder Sources / Types;
- validation visits;
- Graph full scans;
- contribution full scans;
- construction storage pressure before/after.

D3C does not duplicate those counters.

## New Project Manager BUILD telemetry

`project_build_telemetry` adds:

```cpp
std::uint64_t baseline_open_ns;
std::uint64_t dirty_detection_ns;
std::uint64_t manager_total_ns;
std::uint64_t baseline_sources;
std::uint64_t dirty_sources;
```

These fields are populated by `project_manager::build()` when a persisted
baseline exists.

They intentionally remain zero for direct low-level
`project_build_orchestrator` tests.

## Timing boundaries

```text
project_manager::build
│
├─ configuration + compatibility
│
├─ baseline_open_ns
│    baseline_store::open
│    mmap compiled.bin
│    mmap source_manager.bin
│    mmap build_cache.bin
│
├─ dirty_detection_ns
│    bind Source Manager image
│    filesystem metadata/hash checks
│
└─ if dirty
     project_build_orchestrator::update
     │
     ├─ frontend_ns
     ├─ builder_prepare_ns
     ├─ source_prepare_publish_ns
     ├─ interface_prepare_publish_ns
     └─ publication_ns
```

`manager_total_ns` measures the full persisted BUILD command through READY.

## Important expected scaling behavior

Dirty detection currently checks persisted Sources and therefore is allowed to
grow with `baseline_sources`.

The semantic rebuild must not do that.

For one independent changed Source:

```text
frontend.dirty                 = 1
frontend.changed               = 1
frontend.affected              = 1
frontend.parsed                = 1
builder.changed_sources        = 1
builder.changed_types          = 1
source_graph_full_scans        = 0
builder.graph_full_scans       = 0
builder.contribution_full_scans= 0
```

Therefore:

- `dirty_detection_ns` may increase from 1K -> 10K -> 100K -> 1M Sources;
- `frontend_ns` should remain approximately affected-set dependent;
- `builder_prepare_ns` should remain approximately affected-set dependent;
- construction heap growth should not scale linearly with the persisted
  baseline.

## Persisted sparse BUILD benchmark

D3C adds:

```text
server_engine_persisted_sparse_build_benchmark
```

The benchmark creates N independent Source files, performs:

```text
REBUILD
SAVE
UNLOAD
modify exactly one Source
BUILD
```

and prints CSV telemetry for the actual `project_manager::build()` path.

It then optionally performs:

```text
SAVE changed READY state
UNLOAD
BUILD with no file changes
```

to prove the no-change mapped reuse path performs no Parser/Builder work.

### Commands

Fast structural gate:

```text
server_engine_persisted_sparse_build_benchmark --gate
```

The gate compares 1,024 and 8,192 Source persisted baselines.

Manual scale:

```text
server_engine_persisted_sparse_build_benchmark --scale 100000
server_engine_persisted_sparse_build_benchmark --scale 1000000
```

Convenience matrix:

```text
server_engine_persisted_sparse_build_benchmark --matrix
```

The matrix runs:

```text
1,000
10,000
100,000
```

Sources.

## Gate

The D3C gate requires:

- persisted BUILD succeeds;
- exactly one dirty Source;
- exactly one affected Source;
- exactly one parsed Source;
- exactly one changed Builder Source;
- exactly one changed Type;
- no Source graph full scan;
- no Graph full scan;
- no contribution full scan;
- 8x larger persisted baseline does not create 8x larger sparse construction
  heap for the same one-Source change;
- no-change persisted BUILD performs zero frontend / Builder work.

Timing values are reported but are not used as brittle absolute CI thresholds.

## Frozen contracts retained

D3C introduces no:

- mutex/shared_mutex;
- unordered_map;
- semantic sort;
- textual Builder lookup;
- stable_id;
- Graph history;
- implicit SAVE;
- public raw-handle constructor;
- persistence format revision.

D3C is observational: D3B semantics remain frozen.
