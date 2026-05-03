-- Test shared plan cache relation invalidation (Patch 0008)
--
-- Server config:
--   shared_plan_cache_max_entries = 100
--   compute_query_id = on
--   shared_plan_cache_enabled = off

CREATE EXTENSION test_shared_plan_cache_invalidation;

SET shared_plan_cache_enabled = on;
SET plan_cache_mode = force_generic_plan;

-- ============================================================
-- T1: Basic relation invalidation via test helper
-- ============================================================
CREATE TABLE inv_t1 (a int);
INSERT INTO inv_t1 VALUES (1), (2), (3);
ANALYZE inv_t1;

PREPARE inv_s1 AS SELECT a FROM inv_t1 WHERE a = $1;
EXECUTE inv_s1(1);

SELECT test_spc_entry_is_valid_for_prep('inv_s1') AS t1_before;

-- Force specific-relid relcache invalidation
SELECT test_spc_force_relcache_invalidation('inv_t1'::regclass::oid);

SELECT test_spc_entry_is_valid_for_prep('inv_s1') AS t1_after;

DEALLOCATE inv_s1;

-- ============================================================
-- T1b: Old-key invalidation proof via captured key
-- Proves the actual old SharedPlanKey entry is invalidated,
-- independent of whether DDL changes the query_tree_hash.
-- ============================================================
CREATE TABLE inv_t1b (a int);
INSERT INTO inv_t1b VALUES (1);
ANALYZE inv_t1b;

PREPARE inv_s1b AS SELECT a FROM inv_t1b WHERE a = $1;
EXECUTE inv_s1b(1);

-- Capture the exact SharedPlanKey BEFORE any DDL
SELECT test_spc_capture_key_for_prep('inv_s1b') AS old_key
\gset

-- Verify old key is currently lookup-valid
SELECT test_spc_captured_key_is_valid(:'old_key'::bytea) AS t1b_old_key_before;

-- Force relcache invalidation via test helper (dep-index precise path)
SELECT test_spc_force_relcache_invalidation('inv_t1b'::regclass::oid);

-- The exact old key must now be invalid — this does not recompute the key
SELECT test_spc_captured_key_is_valid(:'old_key'::bytea) AS t1b_old_key_after;

DEALLOCATE inv_s1b;

-- ============================================================
-- T3: Precision — unrelated relid DDL preserves other entries
-- ============================================================
CREATE TABLE inv_target (a int);
CREATE TABLE inv_other (b int);
INSERT INTO inv_target VALUES (1);
INSERT INTO inv_other VALUES (1);
ANALYZE inv_target;
ANALYZE inv_other;

PREPARE inv_p1 AS SELECT a FROM inv_target WHERE a = $1;
PREPARE inv_p2 AS SELECT b FROM inv_other WHERE b = $1;
EXECUTE inv_p1(1);
EXECUTE inv_p2(1);

SELECT test_spc_entry_is_valid_for_prep('inv_p1') AS t3_p1_before;
SELECT test_spc_entry_is_valid_for_prep('inv_p2') AS t3_p2_before;

SELECT test_spc_current_generation() AS gen_before
\gset
SELECT test_spc_current_store_epoch() AS epoch_before
\gset

SELECT test_spc_force_relcache_invalidation('inv_target'::regclass::oid);

SELECT test_spc_entry_is_valid_for_prep('inv_p1') AS t3_p1_after;
SELECT test_spc_entry_is_valid_for_prep('inv_p2') AS t3_p2_after;
SELECT test_spc_current_generation() = :gen_before AS t3_gen_unchanged;
SELECT test_spc_current_store_epoch() > :epoch_before AS t3_epoch_changed;

DEALLOCATE inv_p1;
DEALLOCATE inv_p2;

-- ============================================================
-- T4: Broad invalidation makes old entries generation-stale
-- ============================================================
CREATE TABLE inv_broad (a int);
INSERT INTO inv_broad VALUES (1);
ANALYZE inv_broad;

PREPARE inv_broad_s AS SELECT a FROM inv_broad WHERE a = $1;
EXECUTE inv_broad_s(1);

SELECT test_spc_entry_is_valid_for_prep('inv_broad_s') AS t4_before;

SELECT test_spc_current_generation() AS gen_before_broad
\gset

-- InvalidOid broad invalidation
SELECT test_spc_force_relcache_invalidation(0);

SELECT test_spc_current_generation() > :gen_before_broad AS t4_gen_bumped;

-- Entry should be generation-stale now
SELECT test_spc_entry_is_valid_for_prep('inv_broad_s') AS t4_after_inval;

DEALLOCATE inv_broad_s;

-- ============================================================
-- T5: Stale entry refresh after invalidation
-- ============================================================
CREATE TABLE inv_refresh (a int);
INSERT INTO inv_refresh VALUES (1);
ANALYZE inv_refresh;

PREPARE inv_ref_s AS SELECT a FROM inv_refresh WHERE a = $1;
EXECUTE inv_ref_s(1);

SELECT test_spc_entry_is_valid_for_prep('inv_ref_s') AS t5_before;

-- Invalidate via dep-index
SELECT test_spc_force_relcache_invalidation('inv_refresh'::regclass::oid);
SELECT test_spc_entry_is_valid_for_prep('inv_ref_s') AS t5_after_inval;

-- Re-execute to trigger re-store (stale slot overwrite)
DEALLOCATE inv_ref_s;
PREPARE inv_ref_s AS SELECT a FROM inv_refresh WHERE a = $1;
EXECUTE inv_ref_s(1);

SELECT test_spc_entry_is_valid_for_prep('inv_ref_s') AS t5_after_refresh;

DEALLOCATE inv_ref_s;

-- ============================================================
-- T5b: Broad invalidation + refresh
-- ============================================================
CREATE TABLE inv_bref (a int);
INSERT INTO inv_bref VALUES (1);
ANALYZE inv_bref;

PREPARE inv_bref_s AS SELECT a FROM inv_bref WHERE a = $1;
EXECUTE inv_bref_s(1);

SELECT test_spc_entry_is_valid_for_prep('inv_bref_s') AS t5b_before;

SELECT test_spc_force_relcache_invalidation(0);

SELECT test_spc_entry_is_valid_for_prep('inv_bref_s') AS t5b_after_inval;

-- Refresh: re-store should overwrite generation-stale entry
DEALLOCATE inv_bref_s;
PREPARE inv_bref_s AS SELECT a FROM inv_bref WHERE a = $1;
EXECUTE inv_bref_s(1);

SELECT test_spc_entry_is_valid_for_prep('inv_bref_s') AS t5b_after_refresh;

DEALLOCATE inv_bref_s;

-- ============================================================
-- T7: Dependency count
-- ============================================================
CREATE TABLE inv_t4 (a int);
CREATE TABLE inv_t5 (b int);
INSERT INTO inv_t4 VALUES (1);
INSERT INTO inv_t5 VALUES (1);
ANALYZE inv_t4;
ANALYZE inv_t5;

PREPARE inv_s2 AS SELECT inv_t4.a, inv_t5.b FROM inv_t4 JOIN inv_t5 ON inv_t4.a = inv_t5.b;
EXECUTE inv_s2;

SELECT test_spc_dep_key_count('inv_t4') >= 1 AS t7_dep_t4;
SELECT test_spc_dep_key_count('inv_t5') >= 1 AS t7_dep_t5;

DEALLOCATE inv_s2;

-- ============================================================
-- T8: num_relation_oids populated
-- ============================================================
CREATE TABLE inv_t6 (a int);
INSERT INTO inv_t6 VALUES (1);
ANALYZE inv_t6;

PREPARE inv_s3 AS SELECT a FROM inv_t6 WHERE a = $1;
EXECUTE inv_s3(1);

SELECT test_spc_entry_num_rel_oids('inv_s3') > 0 AS t8_has_rel_oids;

DEALLOCATE inv_s3;

-- ============================================================
-- T9: Temp table rejected
-- ============================================================
CREATE TEMP TABLE inv_temp (a int);
INSERT INTO inv_temp VALUES (1);

SELECT test_spc_l2_store_count() AS stores_before_temp
\gset

PREPARE inv_s5 AS SELECT * FROM inv_temp WHERE a = $1;
EXECUTE inv_s5(1);

SELECT test_spc_l2_store_count() = :stores_before_temp AS t9_no_temp_store;

DEALLOCATE inv_s5;
DROP TABLE inv_temp;

-- ============================================================
-- T10: ALTER TABLE DDL invalidation with stable column list
-- Uses explicit column 'a' so SharedPlanKey is stable across ADD COLUMN.
-- ============================================================
CREATE TABLE inv_t7 (a int);
INSERT INTO inv_t7 VALUES (1);
ANALYZE inv_t7;

PREPARE inv_s6 AS SELECT a FROM inv_t7 WHERE a = $1;
EXECUTE inv_s6(1);

SELECT test_spc_entry_is_valid_for_prep('inv_s6') AS t10_before;

ALTER TABLE inv_t7 ADD COLUMN b text;

-- Entry must be invalid despite same query shape (relcache invalidation)
SELECT test_spc_entry_is_valid_for_prep('inv_s6') AS t10_after;

DEALLOCATE inv_s6;

-- ============================================================
-- T11: Stale dep-key tolerance
-- ============================================================
CREATE TABLE inv_t8 (a int);
INSERT INTO inv_t8 VALUES (1);
ANALYZE inv_t8;

PREPARE inv_s7a AS SELECT a FROM inv_t8 WHERE a = $1;
EXECUTE inv_s7a(1);

SELECT test_spc_force_relcache_invalidation('inv_t8'::regclass::oid);

DEALLOCATE inv_s7a;
PREPARE inv_s7b AS SELECT a FROM inv_t8 WHERE a = $1;
EXECUTE inv_s7b(1);

-- Second invalidation with stale key from first plan
SELECT test_spc_force_relcache_invalidation('inv_t8'::regclass::oid);

DEALLOCATE inv_s7b;
PREPARE inv_s7c AS SELECT a FROM inv_t8 WHERE a = $1;
EXECUTE inv_s7c(1);

DEALLOCATE inv_s7c;

-- ============================================================
-- T12: current_entries increases after store
-- ============================================================
SELECT test_spc_current_entries() AS entries_snapshot
\gset

CREATE TABLE inv_t9 (a int);
INSERT INTO inv_t9 VALUES (1);
ANALYZE inv_t9;

PREPARE inv_s8 AS SELECT a FROM inv_t9 WHERE a = $1;
EXECUTE inv_s8(1);

SELECT test_spc_current_entries() > :entries_snapshot AS t12_entry_counted;

DEALLOCATE inv_s8;

-- ============================================================
-- T12b: Duplicate store does not increase current_entries
-- ============================================================
CREATE TABLE inv_t10 (a int);
INSERT INTO inv_t10 VALUES (1);
ANALYZE inv_t10;

PREPARE inv_s9 AS SELECT a FROM inv_t10 WHERE a = $1;
EXECUTE inv_s9(1);

SELECT test_spc_current_entries() AS entries_after_first
\gset

-- Re-execute same prepared statement — should hit L1 or L2, not re-store
EXECUTE inv_s9(2);

SELECT test_spc_current_entries() = :entries_after_first AS t12b_no_dup_count;

DEALLOCATE inv_s9;

-- ============================================================
-- T12c: Stale invalidation + refresh does not double-count
-- ============================================================
CREATE TABLE inv_t11 (a int);
INSERT INTO inv_t11 VALUES (1);
ANALYZE inv_t11;

PREPARE inv_s10 AS SELECT a FROM inv_t11 WHERE a = $1;
EXECUTE inv_s10(1);

SELECT test_spc_current_entries() AS entries_before_inval
\gset

-- Invalidate via dep-index
SELECT test_spc_force_relcache_invalidation('inv_t11'::regclass::oid);

-- Refresh: stale overwrite
DEALLOCATE inv_s10;
PREPARE inv_s10 AS SELECT a FROM inv_t11 WHERE a = $1;
EXECUTE inv_s10(1);

-- current_entries should not have grown
SELECT test_spc_current_entries() = :entries_before_inval AS t12c_no_double_count;

DEALLOCATE inv_s10;

-- ============================================================
-- T12d: Broad generation-stale + refresh does not double-count
-- ============================================================
CREATE TABLE inv_t12 (a int);
INSERT INTO inv_t12 VALUES (1);
ANALYZE inv_t12;

PREPARE inv_s11 AS SELECT a FROM inv_t12 WHERE a = $1;
EXECUTE inv_s11(1);

SELECT test_spc_current_entries() AS entries_before_broad
\gset

-- Broad invalidation makes entry generation-stale
SELECT test_spc_force_relcache_invalidation(0);

-- Refresh: generation-stale overwrite
DEALLOCATE inv_s11;
PREPARE inv_s11 AS SELECT a FROM inv_t12 WHERE a = $1;
EXECUTE inv_s11(1);

-- current_entries should not have grown
SELECT test_spc_current_entries() = :entries_before_broad AS t12d_no_double_count;

DEALLOCATE inv_s11;

-- ============================================================
-- Cleanup
-- ============================================================
RESET plan_cache_mode;
RESET shared_plan_cache_enabled;

DROP TABLE IF EXISTS inv_t1, inv_t1b, inv_target, inv_other, inv_broad,
                     inv_refresh, inv_bref, inv_t4, inv_t5,
                     inv_t6, inv_t7, inv_t8, inv_t9, inv_t10, inv_t11, inv_t12;
