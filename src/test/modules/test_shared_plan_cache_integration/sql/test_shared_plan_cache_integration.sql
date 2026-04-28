-- Test shared plan cache L2 integration (Patch 0007)
-- All tests are same-backend only; cross-backend tests are in TAP.
--
-- The regression server starts with:
--   shared_plan_cache_max_entries = 100
--   compute_query_id = on
--   shared_plan_cache_enabled = off
-- so SharedPlanCacheIsActive() should be true once we enable the GUC.
--
-- Deferred tests (not implemented in Patch 0007):
--   IS-01: Isolation test (cross-backend concurrent store/lookup)
--   T-02:  Disabled-mode full TAP regression
--   T-03:  Enabled-mode full TAP regression subset
--   T-09:  Backend-exit stretch test (refcount/cleanup after disconnect)

CREATE EXTENSION test_shared_plan_cache_integration;

-- Setup: create a test table
CREATE TABLE spc_test (id int PRIMARY KEY, val text);
INSERT INTO spc_test SELECT g, 'row' || g FROM generate_series(1, 100) g;
ANALYZE spc_test;

-- Environment check: cache must be active for enabled-mode tests
SELECT test_shared_plan_cache_is_attached() AS cache_attached;
SELECT test_shared_plan_cache_is_active() AS cache_active;
SHOW shared_plan_cache_max_entries;
SHOW compute_query_id;

-- ============================================================
-- I-01: Disabled mode normal path
-- ============================================================
SHOW shared_plan_cache_enabled;

PREPARE i01_stmt AS SELECT * FROM spc_test WHERE id = $1;
SET plan_cache_mode = force_generic_plan;
EXECUTE i01_stmt(1);
RESET plan_cache_mode;

SELECT test_shared_plan_l2_hit_count() = 0 AS no_hits;
SELECT test_shared_plan_l2_miss_count() = 0 AS no_misses;
SELECT test_shared_plan_l2_store_count() = 0 AS no_stores;

DEALLOCATE i01_stmt;

-- ============================================================
-- I-11: Status starts as NONE before any L2 operation
-- ============================================================
SELECT test_shared_plan_last_l2_status() AS initial_status;

-- ============================================================
-- I-02: Enabled mode — verify store happens
-- ============================================================
SET shared_plan_cache_enabled = on;
SHOW shared_plan_cache_enabled;

PREPARE i02_stmt AS SELECT * FROM spc_test WHERE id = $1;
SET plan_cache_mode = force_generic_plan;
EXECUTE i02_stmt(1);
RESET plan_cache_mode;

-- Must produce store_count > 0 when cache is active
SELECT test_shared_plan_l2_store_count() > 0 AS store_happened;

-- Status must have transitioned from NONE to MISS (first lookup misses L2,
-- then store occurs on the locally-built generic plan)
SELECT test_shared_plan_last_l2_status() AS l2_status_after_store;

DEALLOCATE i02_stmt;

-- ============================================================
-- I-03: L1 generic plan has priority
-- ============================================================
PREPARE i03_stmt AS SELECT * FROM spc_test WHERE id = $1;
SET plan_cache_mode = force_generic_plan;
EXECUTE i03_stmt(1);

SELECT test_shared_plan_l2_hit_count() AS hits_before_l1
\gset

EXECUTE i03_stmt(2);

SELECT test_shared_plan_l2_hit_count() = :hits_before_l1 AS l1_priority;
RESET plan_cache_mode;

DEALLOCATE i03_stmt;

-- ============================================================
-- I-04: generic_cost populated after local generic plan build
-- ============================================================
PREPARE i04_stmt AS SELECT * FROM spc_test WHERE id = $1;
SET plan_cache_mode = force_generic_plan;
EXECUTE i04_stmt(1);
RESET plan_cache_mode;

SELECT test_shared_plan_generic_cost('i04_stmt') > 0 AS cost_positive;

DEALLOCATE i04_stmt;

-- ============================================================
-- I-05: Shared entry generic_cost > 0
-- ============================================================
PREPARE i05_stmt AS SELECT * FROM spc_test WHERE id = $1;
SET plan_cache_mode = force_generic_plan;
EXECUTE i05_stmt(1);
RESET plan_cache_mode;

SELECT test_shared_plan_entry_generic_cost('i05_stmt') > 0 AS entry_cost_positive;

DEALLOCATE i05_stmt;

-- ============================================================
-- I-06: Shared entry cost matches plansource generic_cost
-- ============================================================
PREPARE i06_stmt AS SELECT * FROM spc_test WHERE id = $1;
SET plan_cache_mode = force_generic_plan;
EXECUTE i06_stmt(1);
RESET plan_cache_mode;

SELECT test_shared_plan_generic_cost('i06_stmt') AS ps_cost
\gset
SELECT test_shared_plan_entry_generic_cost('i06_stmt') AS entry_cost
\gset

SELECT :ps_cost > 0 AND :entry_cost > 0
       AND abs(:ps_cost - :entry_cost) < 0.001 AS costs_match;

DEALLOCATE i06_stmt;

-- ============================================================
-- I-07: Custom plan path unaffected
-- ============================================================
SET plan_cache_mode = force_custom_plan;
PREPARE i07_stmt AS SELECT * FROM spc_test WHERE id = $1;

SELECT test_shared_plan_l2_hit_count() AS hits_before_custom
\gset

EXECUTE i07_stmt(1);
EXECUTE i07_stmt(2);

SELECT test_shared_plan_l2_hit_count() = :hits_before_custom AS no_l2_for_custom;

DEALLOCATE i07_stmt;
RESET plan_cache_mode;

-- ============================================================
-- I-08: Fallback on PlanIsShareable rejection (temp table)
-- ============================================================
CREATE TEMP TABLE spc_temp_test (id int);
INSERT INTO spc_temp_test VALUES (1), (2), (3);

SELECT test_shared_plan_l2_store_count() AS stores_before_temp
\gset

PREPARE i08_stmt AS SELECT * FROM spc_temp_test WHERE id = $1;
SET plan_cache_mode = force_generic_plan;
EXECUTE i08_stmt(1);
RESET plan_cache_mode;

SELECT test_shared_plan_l2_store_count() = :stores_before_temp AS no_temp_store;

DEALLOCATE i08_stmt;
DROP TABLE spc_temp_test;

-- ============================================================
-- I-09: Fallback on L2 miss — local build succeeds
-- ============================================================
PREPARE i09_stmt AS SELECT * FROM spc_test WHERE id = $1 AND val IS NOT NULL;
SET plan_cache_mode = force_generic_plan;
EXECUTE i09_stmt(1);
RESET plan_cache_mode;

SELECT test_shared_plan_generic_cost('i09_stmt') > 0 AS miss_fallback_ok;

DEALLOCATE i09_stmt;

-- ============================================================
-- I-10: Distinct queries produce distinct store events
-- ============================================================
SELECT test_shared_plan_l2_store_count() AS stores_before_distinct
\gset

PREPARE i10_a AS SELECT val FROM spc_test WHERE id = $1;
PREPARE i10_b AS SELECT id FROM spc_test WHERE val = $1;
SET plan_cache_mode = force_generic_plan;
EXECUTE i10_a(1);
EXECUTE i10_b('row1');
RESET plan_cache_mode;

SELECT test_shared_plan_l2_store_count() - :stores_before_distinct >= 2 AS distinct_stores;

DEALLOCATE i10_a;
DEALLOCATE i10_b;

-- ============================================================
-- I-10c: Same-shape different-literal produces distinct entries
-- (covers query_tree_hash safety for literal constants)
-- ============================================================
SELECT test_shared_plan_l2_store_count() AS stores_before_lit
\gset

PREPARE i10_lit_a AS SELECT * FROM spc_test WHERE id = 1;
PREPARE i10_lit_b AS SELECT * FROM spc_test WHERE id = 2;
SET plan_cache_mode = force_generic_plan;
EXECUTE i10_lit_a;
EXECUTE i10_lit_b;
RESET plan_cache_mode;

SELECT test_shared_plan_l2_store_count() - :stores_before_lit >= 2 AS literal_distinct_stores;

DEALLOCATE i10_lit_a;
DEALLOCATE i10_lit_b;

-- ============================================================
-- I-10b: No errors from L2 operations
-- ============================================================
SELECT test_shared_plan_l2_error_count() AS l2_errors;

-- Cleanup
RESET shared_plan_cache_enabled;
DROP TABLE spc_test;
