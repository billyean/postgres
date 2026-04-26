# Buffer Eviction Prototype — Evaluation Guide

This guide documents the standard evaluation workflow for the decoupled
usage-count buffer eviction prototype (Patches 1–5).  It defines the
comparison scenarios, validation criteria, and result interpretation
so that every developer and reviewer follows the same procedure.

Patch 6 adds this guide and `tools/eviction_report.sql` to the source
tree.  A companion benchmark script (`eviction_bench.sh`) and report
template (`eviction_report_template.md`) are distributed alongside the
patch series on pgsql-hackers but are not committed to the tree.

---

## 1. Purpose and Scope

Patch 6 is the final patch in the series.  It contains no algorithmic
changes.  Its sole purpose is to provide:

- a reproducible evaluation workflow,
- SQL queries for stats capture and invariant checking,
- reviewer-facing documentation.

No backend C code, GUCs, extension objects, or Makefile targets are
introduced by this patch.

---

## 2. Prerequisites

- PostgreSQL built with `USE_DECOUPLED_USAGE_COUNT` enabled.
- A pgbench-initialized database (e.g., `pgbench -i -s 10 benchdb`).
- Superuser access (required for `pg_buffercache_eviction_stats()`).
- The `pg_buffercache` extension created in the test database.

---

## 3. Standard Scenarios

All scenarios use the same compiled binary.  Only runtime GUC settings
differ, requiring a server restart between scenarios (`PGC_POSTMASTER`).

| Scenario | `enable_decoupled_usage_count` | `pg_usage_scan_dispatch_mode` | Purpose |
|----------|------|------|---------|
| **baseline** | `off` | (irrelevant) | Reference — original eviction path, no prototype code active |
| **decoupled-scalar** | `on` | `scalar` | Prototype with scalar kernels — isolates algorithm effect |
| **decoupled-auto** | `on` | `auto` | Full prototype with best-available SIMD — end-to-end effect |

### Optional platform-specific scenarios

| Scenario | Config | When useful |
|----------|--------|-------------|
| decoupled-sse2 | `on` + `sse2` | x86-64: isolate SSE2 vs AVX2 |
| decoupled-avx2 | `on` + `avx2` | x86-64 with AVX2: confirm auto selects AVX2 |
| decoupled-neon | `on` + `neon` | AArch64: confirm auto selects NEON |

### Baseline sentinel expectation

The standard workflow uses a **compiled-in** build with the feature
turned **off** at runtime (`enable_decoupled_usage_count = off`).
Dispatch initialization never executes, so the stats function returns
sentinel values:

| Column | Expected value |
|--------|---------------|
| `requested_mode` | `'auto'` (default GUC value; irrelevant when feature is off) |
| `dispatch_path` | `'not_initialized'` |
| `dispatch_chunk_size` | `0` |
| All activity counters | `0` |

If the build was compiled **without** `USE_DECOUPLED_USAGE_COUNT`, both
`requested_mode` and `dispatch_path` return `'not_enabled'` instead.

---

## 4. Workflow

For each scenario:

```
1. Set GUC values in postgresql.conf (or via ALTER SYSTEM)
2. pg_ctl restart
3. Reset counters:
     SELECT pg_buffercache_eviction_stats_reset();
4. Capture pre-workload snapshot:
     psql -f tools/eviction_report.sql
5. Run workload:
     pgbench -c <clients> -T <duration> <dbname>
6. Capture post-workload snapshot:
     psql -f tools/eviction_report.sql
7. Record pgbench TPS and latency from stdout
```

Repeat for all three core scenarios, then compare.

### Environment identity

Record before the first scenario:

- `uname -a`
- CPU model (`/proc/cpuinfo` or `sysctl machdep.cpu.brand_string`)
- `pg_config --version`
- `SHOW shared_buffers`
- pgbench scale factor, duration, client count

---

## 5. Outputs

### Raw outputs per scenario

| Output | Source |
|--------|--------|
| TPS | pgbench stdout |
| Latency avg (ms) | pgbench stdout |
| Eviction stats pre-workload | `pg_buffercache_eviction_stats()` after reset |
| Eviction stats post-workload | `pg_buffercache_eviction_stats()` after pgbench |

### Derived metrics (from post-workload stats)

| Metric | Formula |
|--------|---------|
| Eviction rate | `victims_found / duration_seconds` |
| Rejection ratio | `(rejected_refcount + rejected_locked) / candidates_examined` |
| CAS failure ratio | `cas_failures / candidates_examined` |
| Decay efficiency | `decrement_progress / decrement_calls` |
| Scan density | `victims_found / scan_calls` |

### Comparison table

```
Scenario           | TPS    | Lat(ms) | Victims | Rej% | CAS% | Decay% | Mode   | Path       | Chunk
-------------------+--------+---------+---------+------+------+--------+--------+------------+------
baseline           | ...    | ...     | N/A     | N/A  | N/A  | N/A    | auto   | not_init'd | 0
decoupled-scalar   | ...    | ...     | ...     | ...  | ...  | ...    | scalar | scalar     | 16
decoupled-auto     | ...    | ...     | ...     | ...  | ...  | ...    | auto   | (platform) | (16|32)
```

---

## 6. Validation Criteria

### Run validity

1. **Mode identity confirmed.**  `requested_mode` and `dispatch_path`
   match the scenario configuration.  Baseline: `dispatch_path =
   'not_initialized'`, all counters zero.
2. **Counter invariants hold:**
   - `segments_scanned >= chunks_scanned`
   - `scan_calls = segments_scanned`
   - `rejected_refcount + rejected_locked + cas_failures + victims_found <= candidates_examined`
   - `decrement_progress + decrement_noprogress = decrement_calls` (when `decrement_calls > 0`)
3. **Eviction occurred** in active scenarios: `victims_found > 0`.
   If zero, the workload was too light — increase scale or duration.
4. **Baseline counters are zero** and `dispatch_path` is
   `'not_initialized'` (compiled-in) or `'not_enabled'` (compiled-out).

### No-drift criteria

- Counter invariants are identical across scalar and SIMD paths.
- Rejection and CAS-failure ratios are comparable across dispatch modes.
- Victim counts are similar.  Small timing-race variations are expected;
  large divergences indicate a bug.

### Performance interpretation

- Baseline vs decoupled-auto is the primary TPS comparison.
- Differences < 2–3% are within typical pgbench noise.
- SIMD benefit: compare decoupled-auto vs decoupled-scalar.
- Always report the environment.  Results are not transferable.

### Failed-run indicators

- `requested_mode` mismatch → configuration error.
- `dispatch_path = 'not_initialized'` in an active scenario → feature
  not active or no eviction occurred.
- Invariant violation → investigate before reporting.
- pgbench error → discard run.

---

## 7. Artifact Boundary

**In the source tree (Patch 6):**

| File | Purpose |
|------|---------|
| `docs/buffer_eviction_evaluation_guide.md` | This guide |
| `tools/eviction_report.sql` | Stats snapshot and invariant queries |

**Submission-side companions (not in tree):**

| Artifact | Purpose |
|----------|---------|
| `eviction_bench.sh` | Automated restart-driven A/B benchmark script |
| `eviction_report_template.md` | Markdown report template for pgsql-hackers |

The companion artifacts are distributed alongside the patch series
(e.g., as cover-letter attachments) and are not committed to the tree.

---

## 8. Series Summary

| Patch | Delivers |
|-------|----------|
| 1 | Metadata split: `BufferUsageMap` |
| 2 | Algorithm: chunk-based three-phase sweep |
| 3 | Kernels: scalar + SSE2 + AVX2 + NEON with runtime dispatch |
| 4 | Observability: `pg_buffercache_eviction_stats()` |
| 5 | Control: `pg_usage_scan_dispatch_mode` GUC |
| 6 | Evaluation: this guide + SQL queries |

Patch 6 closes the series.  No further patches are planned.
