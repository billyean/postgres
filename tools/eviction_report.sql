--
-- eviction_report.sql
--
-- Stats snapshot and invariant validation queries for the decoupled
-- usage-count buffer eviction prototype (Patches 1-5).
--
-- Usage:
--   1. Before the workload, reset counters:
--        SELECT pg_buffercache_eviction_stats_reset();
--
--   2. Capture a pre-workload snapshot (should show all zeros):
--        psql -d <dbname> -f tools/eviction_report.sql
--
--   3. Run the workload (e.g., pgbench).
--
--   4. Capture a post-workload snapshot:
--        psql -d <dbname> -f tools/eviction_report.sql
--
-- Requires: pg_buffercache extension, superuser role.
-- Does not create any objects.  All queries are read-only.
--

-- 1. Full stats snapshot (all 15 columns).
\echo === Eviction Stats Snapshot ===
SELECT requested_mode,
       dispatch_path,
       dispatch_chunk_size,
       chunks_scanned,
       segments_scanned,
       scan_calls,
       decrement_calls,
       candidates_examined,
       rejected_refcount,
       rejected_locked,
       cas_failures,
       victims_found,
       decrement_progress,
       decrement_noprogress,
       trycounter_resets
FROM pg_buffercache_eviction_stats();

-- 2. Counter invariant validation.
--    Returns true when all four invariants hold.
--    Baseline and compiled-out states pass unconditionally.
\echo === Invariant Check ===
SELECT CASE
    WHEN dispatch_path IN ('scalar', 'sse2', 'avx2', 'neon') THEN
        segments_scanned >= chunks_scanned
        AND scan_calls = segments_scanned
        AND rejected_refcount + rejected_locked + cas_failures + victims_found
            <= candidates_examined
        AND (decrement_calls = 0
             OR decrement_progress + decrement_noprogress = decrement_calls)
    ELSE true
END AS invariants_hold
FROM pg_buffercache_eviction_stats();

-- 3. Dispatch identity summary (compact one-liner).
\echo === Dispatch Identity ===
SELECT requested_mode, dispatch_path, dispatch_chunk_size
FROM pg_buffercache_eviction_stats();
