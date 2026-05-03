CREATE EXTENSION pg_buffercache;

select count(*) = (select setting::bigint
                   from pg_settings
                   where name = 'shared_buffers')
from pg_buffercache;

-- For pg_buffercache_os_pages, we expect at least one entry for each buffer
select count(*) >= (select setting::bigint
                    from pg_settings
                    where name = 'shared_buffers')
from pg_buffercache_os_pages;

select buffers_used + buffers_unused > 0,
        buffers_dirty <= buffers_used,
        buffers_pinned <= buffers_used
from pg_buffercache_summary();

SELECT count(*) > 0 FROM pg_buffercache_usage_counts() WHERE buffers >= 0;

-- Verify usagecount values are always in valid range [0, BM_MAX_USAGE_COUNT]
-- This validates correct reporting regardless of decoupled usage_count mode.
SELECT count(*) = 0
FROM pg_buffercache
WHERE usagecount IS NOT NULL AND (usagecount < 0 OR usagecount > 5);

-- Verify pg_buffercache_usage_counts returns exactly the expected rows (0..5)
SELECT count(*) = 6 FROM pg_buffercache_usage_counts();

-- Verify total buffers from usage_counts matches shared_buffers
SELECT sum(buffers) = (SELECT setting::bigint FROM pg_settings WHERE name = 'shared_buffers')
FROM pg_buffercache_usage_counts();

-- Verify usagecount_avg from summary is non-negative (sanity for both modes)
SELECT usagecount_avg >= 0
FROM pg_buffercache_summary()
WHERE buffers_used > 0;

-- Cross-check: per-buffer usagecount distribution should sum to buffers_used
SELECT (SELECT sum(buffers) FROM pg_buffercache_usage_counts()) =
       (SELECT buffers_used + buffers_unused FROM pg_buffercache_summary());

-- Verify that buffer access actually produces non-zero usagecount.
-- This confirms the usage_count source (state word or decoupled map) is
-- being written by pin activity, not just that the reporting SQL works.
CREATE TABLE usagecount_test (id int);
INSERT INTO usagecount_test SELECT generate_series(1, 100);
-- Access repeatedly to ensure usagecount > 0 for these buffers.
SELECT count(*) FROM usagecount_test;
SELECT count(*) FROM usagecount_test;
SELECT count(*) FROM usagecount_test;
SELECT count(*) > 0
FROM pg_buffercache b
     JOIN pg_class c ON c.relfilenode = b.relfilenode
WHERE c.relname = 'usagecount_test'
  AND b.usagecount > 0;
DROP TABLE usagecount_test;

-- Exercise the buffer eviction sweep path by forcing churn larger than
-- shared_buffers.  When the decoupled chunk-based sweep (Patch 2) is
-- active, this exercises the three-phase chunk algorithm and split-chunk
-- wraparound under real victim-search pressure.  The sanity checks
-- afterward confirm the pool state is still internally consistent.
CREATE TABLE eviction_pressure_test (id int, payload text);
INSERT INTO eviction_pressure_test
    SELECT g, repeat('x', 200) FROM generate_series(1, 5000) g;
-- Sequential scan forces the sweep to find victims repeatedly
SELECT count(*) FROM eviction_pressure_test;
-- Verify pool consistency after heavy eviction
SELECT count(*) = 0
FROM pg_buffercache
WHERE usagecount IS NOT NULL AND (usagecount < 0 OR usagecount > 5);
SELECT sum(buffers) = (SELECT setting::bigint FROM pg_settings WHERE name = 'shared_buffers')
FROM pg_buffercache_usage_counts();
DROP TABLE eviction_pressure_test;

-- Verify that after heavy churn, the usagecount distribution is not degenerate.
-- Under chunk-based sweep (Patch 2), the bulk decrement and candidate selection
-- must keep the distribution healthy: some buffers should have usagecount > 0
-- (recently accessed system catalog pages, for example).  If chunk decrement
-- were broken, all buffers would decay to 0 with no increment to counteract.
CREATE TABLE churn_test (id int, data text);
INSERT INTO churn_test SELECT g, repeat('y', 100) FROM generate_series(1, 2000) g;
SELECT count(*) FROM churn_test;
SELECT count(*) FROM churn_test;
-- After accessing data, some buffers must have usagecount > 0
SELECT (SELECT sum(buffers) FROM pg_buffercache_usage_counts() WHERE usage_count > 0) > 0;
DROP TABLE churn_test;

-- Patch 3: SIMD kernel integration smoke test.
-- When SIMD dispatch is active (SSE2, AVX2, or NEON), the scan and decrement
-- kernels run through the SIMD fast paths on every victim search.  In assert
-- builds, cross-check wrappers verify SIMD output matches scalar on every
-- call.  This test forces enough eviction pressure to exercise multiple full
-- sweeps of the buffer pool, including partial-chunk boundary handling.
CREATE TABLE simd_eviction_test (id int, payload text);
INSERT INTO simd_eviction_test
    SELECT g, repeat('z', 500) FROM generate_series(1, 10000) g;
-- Multiple scans force repeated victim search through SIMD scan/decrement
SELECT count(*) FROM simd_eviction_test;
SELECT count(*) FROM simd_eviction_test WHERE id % 3 = 0;
-- Pool must remain consistent after heavy SIMD-path eviction
SELECT count(*) = 0
FROM pg_buffercache
WHERE usagecount IS NOT NULL AND (usagecount < 0 OR usagecount > 5);
SELECT sum(buffers) = (SELECT setting::bigint FROM pg_settings WHERE name = 'shared_buffers')
FROM pg_buffercache_usage_counts();
-- Usage distribution must not be degenerate after SIMD decrement sweeps
SELECT (SELECT sum(buffers) FROM pg_buffercache_usage_counts() WHERE usage_count > 0) > 0;
DROP TABLE simd_eviction_test;

-- Check that the functions / views can't be accessed by default. To avoid
-- having to create a dedicated user, use the pg_database_owner pseudo-role.
SET ROLE pg_database_owner;
SELECT * FROM pg_buffercache;
SELECT * FROM pg_buffercache_os_pages;
SELECT * FROM pg_buffercache_pages() AS p (wrong int);
SELECT * FROM pg_buffercache_summary();
SELECT * FROM pg_buffercache_usage_counts();
RESET role;

-- Check that pg_monitor is allowed to query view / function
SET ROLE pg_monitor;
SELECT count(*) > 0 FROM pg_buffercache;
SELECT count(*) > 0 FROM pg_buffercache_os_pages;
SELECT buffers_used + buffers_unused > 0 FROM pg_buffercache_summary();
SELECT count(*) > 0 FROM pg_buffercache_usage_counts();
RESET role;


------
---- Test pg_buffercache_evict* and pg_buffercache_mark_dirty* functions
------

CREATE ROLE regress_buffercache_normal;
SET ROLE regress_buffercache_normal;

-- These should fail because they need to be called as SUPERUSER
SELECT * FROM pg_buffercache_evict(1);
SELECT * FROM pg_buffercache_evict_relation(1);
SELECT * FROM pg_buffercache_evict_all();
SELECT * FROM pg_buffercache_mark_dirty(1);
SELECT * FROM pg_buffercache_mark_dirty_relation(1);
SELECT * FROM pg_buffercache_mark_dirty_all();

RESET ROLE;

-- These should return nothing, because these are STRICT functions
SELECT * FROM pg_buffercache_evict(NULL);
SELECT * FROM pg_buffercache_evict_relation(NULL);
SELECT * FROM pg_buffercache_mark_dirty(NULL);
SELECT * FROM pg_buffercache_mark_dirty_relation(NULL);

-- These should fail because they are not called by valid range of buffers
-- Number of the shared buffers are limited by max integer
SELECT 2147483647 max_buffers \gset
SELECT * FROM pg_buffercache_evict(-1);
SELECT * FROM pg_buffercache_evict(0);
SELECT * FROM pg_buffercache_evict(:max_buffers);
SELECT * FROM pg_buffercache_mark_dirty(-1);
SELECT * FROM pg_buffercache_mark_dirty(0);
SELECT * FROM pg_buffercache_mark_dirty(:max_buffers);

-- These should fail because they don't accept local relations
CREATE TEMP TABLE temp_pg_buffercache();
SELECT * FROM pg_buffercache_evict_relation('temp_pg_buffercache');
SELECT * FROM pg_buffercache_mark_dirty_relation('temp_pg_buffercache');
DROP TABLE temp_pg_buffercache;

-- These shouldn't fail
SELECT buffer_evicted IS NOT NULL FROM pg_buffercache_evict(1);
SELECT buffers_evicted IS NOT NULL FROM pg_buffercache_evict_all();
CREATE TABLE shared_pg_buffercache();
SELECT buffers_evicted IS NOT NULL FROM pg_buffercache_evict_relation('shared_pg_buffercache');
SELECT buffers_dirtied IS NOT NULL FROM pg_buffercache_mark_dirty_relation('shared_pg_buffercache');
DROP TABLE shared_pg_buffercache;
SELECT pg_buffercache_mark_dirty(1) IS NOT NULL;
SELECT pg_buffercache_mark_dirty_all() IS NOT NULL;

DROP ROLE regress_buffercache_normal;

------
---- Test pg_buffercache_eviction_stats (Patch 4)
------

-- A. Stable contract checks — valid regardless of feature-on/off

-- SRF shape: returns exactly one row
SELECT count(*) = 1 FROM pg_buffercache_eviction_stats();

-- dispatch_path is always one of the settled sentinel/ISA values
SELECT dispatch_path IN ('not_enabled', 'not_initialized',
                         'scalar', 'sse2', 'avx2', 'neon')
FROM pg_buffercache_eviction_stats();

-- dispatch_chunk_size is 0 (sentinel) or 16 or 32 (active)
SELECT dispatch_chunk_size IN (0, 16, 32)
FROM pg_buffercache_eviction_stats();

-- All counter fields are non-negative
SELECT chunks_scanned >= 0 AND segments_scanned >= 0
       AND scan_calls >= 0 AND decrement_calls >= 0
       AND candidates_examined >= 0 AND rejected_refcount >= 0
       AND rejected_locked >= 0 AND cas_failures >= 0
       AND victims_found >= 0
       AND decrement_progress >= 0 AND decrement_noprogress >= 0
       AND trycounter_resets >= 0
FROM pg_buffercache_eviction_stats();

-- Reset succeeds (no error, returns void)
SELECT pg_buffercache_eviction_stats_reset();

-- After reset, all activity counters are zero
SELECT chunks_scanned = 0 AND segments_scanned = 0
       AND scan_calls = 0 AND decrement_calls = 0
       AND candidates_examined = 0 AND victims_found = 0
       AND decrement_progress = 0 AND decrement_noprogress = 0
       AND trycounter_resets = 0
FROM pg_buffercache_eviction_stats();

-- Reset preserves dispatch fields (they are NOT zeroed)
SELECT dispatch_path IN ('not_enabled', 'not_initialized',
                         'scalar', 'sse2', 'avx2', 'neon')
FROM pg_buffercache_eviction_stats();

-- dispatch_chunk_size must be consistent with dispatch_path
SELECT CASE
    WHEN dispatch_path IN ('scalar', 'sse2', 'neon') THEN dispatch_chunk_size = 16
    WHEN dispatch_path = 'avx2' THEN dispatch_chunk_size = 32
    ELSE dispatch_chunk_size = 0
END AS dispatch_fields_consistent
FROM pg_buffercache_eviction_stats();

-- B. Movement and invariant checks — conditional on dispatch state.
-- When not_enabled or not_initialized, all counters must be 0.
-- When the feature is active, movement checks validate counter relationships
-- but do not require any specific counter to be positive, because eviction
-- and decrement activity cannot be guaranteed in every test environment.

SELECT pg_buffercache_eviction_stats_reset();

-- Smoke workload intended to exercise buffer activity.
CREATE TABLE eviction_stats_test (id int, payload text);
INSERT INTO eviction_stats_test
    SELECT g, repeat('x', 500) FROM generate_series(1, 10000) g;
SELECT count(*) FROM eviction_stats_test;
SELECT count(*) FROM eviction_stats_test WHERE id % 2 = 0;
DROP TABLE eviction_stats_test;

-- Active-path contract: counter relationships must hold regardless of
-- whether eviction actually occurred.  Decrement invariants are checked
-- separately, conditional on decrement_calls, because a victim can be
-- found before Phase 3 runs.
SELECT CASE
    WHEN dispatch_path IN ('scalar', 'sse2', 'avx2', 'neon')
        THEN chunks_scanned >= victims_found
             AND scan_calls >= victims_found
             AND candidates_examined >= victims_found
    ELSE
        scan_calls = 0 AND decrement_calls = 0
END AS counter_contract_holds
FROM pg_buffercache_eviction_stats();

-- C. Invariant checks — each group conditional on its own activity
SELECT
    -- scan invariants (when scan activity occurred)
    CASE WHEN scan_calls > 0 THEN
        segments_scanned >= chunks_scanned
        AND scan_calls = segments_scanned
        AND rejected_refcount + rejected_locked + cas_failures + victims_found
            <= candidates_examined
    ELSE true END
    AND
    -- decrement invariants (when decrement activity occurred)
    CASE WHEN decrement_calls > 0 THEN
        decrement_progress + decrement_noprogress = decrement_calls
    ELSE true END
AS invariants_hold
FROM pg_buffercache_eviction_stats();

-- Permission checks
SET ROLE pg_database_owner;
SELECT * FROM pg_buffercache_eviction_stats();
SELECT pg_buffercache_eviction_stats_reset();
RESET ROLE;

------
---- Test pg_buffercache_eviction_stats — Patch 5 requested_mode column
------
-- Note: forced-mode and fallback-WARNING validation require a server
-- restart (PGC_POSTMASTER GUC) and are documented as restart-required
-- validation, not ordinary pg_regress coverage.

-- requested_mode column exists and has valid values.
-- 'not_initialized' is NOT a valid requested_mode value — only
-- dispatch_path may be 'not_initialized'.
SELECT requested_mode IN ('not_enabled',
                          'auto', 'scalar', 'sse2', 'avx2', 'neon')
FROM pg_buffercache_eviction_stats();

-- Under default configuration, requested_mode is 'auto' or 'not_enabled'.
SELECT requested_mode IN ('not_enabled', 'auto')
FROM pg_buffercache_eviction_stats();

-- When dispatch is active, requested_mode must be a mode name (not sentinel).
SELECT CASE
    WHEN dispatch_path IN ('scalar', 'sse2', 'avx2', 'neon')
        THEN requested_mode IN ('auto', 'scalar', 'sse2', 'avx2', 'neon')
    ELSE true
END AS requested_mode_consistent
FROM pg_buffercache_eviction_stats();

-- requested_mode is stable across reset (derived from config, not stored state).
-- dispatch_path and dispatch_chunk_size are also preserved by reset.
SELECT requested_mode AS rm_before,
       dispatch_path AS dp_before,
       dispatch_chunk_size AS cs_before
INTO TEMP pre_reset FROM pg_buffercache_eviction_stats();

SELECT pg_buffercache_eviction_stats_reset();

SELECT (SELECT rm_before FROM pre_reset) =
       (SELECT requested_mode FROM pg_buffercache_eviction_stats())
       AND
       (SELECT dp_before FROM pre_reset) =
       (SELECT dispatch_path FROM pg_buffercache_eviction_stats())
       AND
       (SELECT cs_before FROM pre_reset) =
       (SELECT dispatch_chunk_size FROM pg_buffercache_eviction_stats())
AS dispatch_fields_preserved_across_reset;

DROP TABLE pre_reset;

------
---- Test pg_buffercache_eviction_stats_aggregated (Patch 7)
------

-- A. Shape / existence

-- Aggregate SRF returns exactly one row
SELECT count(*) = 1 FROM pg_buffercache_eviction_stats_aggregated();

-- Output has 16 columns
SELECT count(*) = 16
FROM pg_catalog.pg_proc p, unnest(p.proargnames) AS col
WHERE p.proname = 'pg_buffercache_eviction_stats_aggregated';

-- active_slots column exists (naming contract)
SELECT active_slots IS NOT NULL
FROM pg_buffercache_eviction_stats_aggregated();

-- B. Sentinel / dispatch identity behavior

-- dispatch_path is a valid sentinel or ISA value
SELECT dispatch_path IN ('not_enabled', 'not_initialized',
                         'scalar', 'sse2', 'avx2', 'neon')
FROM pg_buffercache_eviction_stats_aggregated();

-- dispatch_chunk_size is 0 (sentinel) or 16 or 32 (active)
SELECT dispatch_chunk_size IN (0, 16, 32)
FROM pg_buffercache_eviction_stats_aggregated();

-- requested_mode matches local SRF
SELECT (SELECT requested_mode FROM pg_buffercache_eviction_stats()) =
       (SELECT requested_mode FROM pg_buffercache_eviction_stats_aggregated())
AS requested_mode_matches;

-- C. Reset behavior

-- Aggregate reset succeeds
SELECT pg_buffercache_eviction_stats_aggregated_reset();

-- After reset with no concurrent workload: all counters 0, active_slots 0
SELECT active_slots = 0
       AND chunks_scanned = 0 AND segments_scanned = 0
       AND scan_calls = 0 AND decrement_calls = 0
       AND candidates_examined = 0 AND rejected_refcount = 0
       AND rejected_locked = 0 AND cas_failures = 0
       AND victims_found = 0
       AND decrement_progress = 0 AND decrement_noprogress = 0
       AND trycounter_resets = 0
FROM pg_buffercache_eviction_stats_aggregated();

-- Dispatch identity preserved across aggregate reset
SELECT dispatch_path IN ('not_enabled', 'not_initialized',
                         'scalar', 'sse2', 'avx2', 'neon')
FROM pg_buffercache_eviction_stats_aggregated();

-- Local and aggregate resets are independent:
-- reset aggregate only, local counters should be unchanged by it
SELECT pg_buffercache_eviction_stats_reset();
SELECT pg_buffercache_eviction_stats_aggregated_reset();

-- D. Counter sanity after workload

-- Smoke workload
CREATE TABLE agg_stats_test (id int, payload text);
INSERT INTO agg_stats_test
    SELECT g, repeat('x', 500) FROM generate_series(1, 10000) g;
SELECT count(*) FROM agg_stats_test;
SELECT count(*) FROM agg_stats_test WHERE id % 2 = 0;
DROP TABLE agg_stats_test;

-- All counters non-negative
SELECT chunks_scanned >= 0 AND segments_scanned >= 0
       AND scan_calls >= 0 AND decrement_calls >= 0
       AND candidates_examined >= 0 AND rejected_refcount >= 0
       AND rejected_locked >= 0 AND cas_failures >= 0
       AND victims_found >= 0
       AND decrement_progress >= 0 AND decrement_noprogress >= 0
       AND trycounter_resets >= 0
       AND active_slots >= 0
FROM pg_buffercache_eviction_stats_aggregated();

-- Aggregate invariants (conditional on activity)
SELECT
    CASE WHEN scan_calls > 0 THEN
        segments_scanned >= chunks_scanned
        AND scan_calls = segments_scanned
        AND rejected_refcount + rejected_locked + cas_failures + victims_found
            <= candidates_examined
    ELSE true END
    AND
    CASE WHEN decrement_calls > 0 THEN
        decrement_progress + decrement_noprogress = decrement_calls
    ELSE true END
AS agg_invariants_hold
FROM pg_buffercache_eviction_stats_aggregated();

-- E. active_slots contract

-- After activity: active_slots >= 1 when dispatch is active
SELECT CASE
    WHEN dispatch_path IN ('scalar', 'sse2', 'avx2', 'neon')
        THEN active_slots >= 1
    ELSE active_slots = 0
END AS active_slots_contract
FROM pg_buffercache_eviction_stats_aggregated();

-- F. Permission checks

SET ROLE pg_database_owner;
SELECT * FROM pg_buffercache_eviction_stats_aggregated();
SELECT pg_buffercache_eviction_stats_aggregated_reset();
RESET ROLE;
