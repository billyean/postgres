-- Patch 0010: Race Protection and Two-Phase Validation Tests (T1–T14)
--
-- Tests run with shared_plan_cache_enabled=off initially.
-- Enabled per-test via SET.

CREATE EXTENSION test_shared_plan_cache_race_protection;

SET shared_plan_cache_enabled = on;
SET plan_cache_mode = force_generic_plan;

-- ========================================================
-- T1: Basic L2 hit with refcount
-- L2 hit requires DEALLOCATE + re-PREPARE to clear L1 gplan.
-- ========================================================

CREATE TABLE t1_race (id int, val text);
INSERT INTO t1_race VALUES (1, 'hello');

-- First prepare+execute stores entry into L2 (MISS → store)
PREPARE t1_q AS SELECT * FROM t1_race WHERE id = $1;
EXECUTE t1_q(1);
EXECUTE t1_q(1);

-- Entry stored and valid
SELECT test_spc_race_entry_is_valid_for_prep('t1_q') AS t1_stored;

-- DEALLOCATE clears L1. Re-PREPARE creates fresh plansource with no gplan.
-- Next EXECUTE must go to L2 → HIT.
SELECT test_spc_race_l2_hit_count() AS t1_hits_before
\gset
DEALLOCATE t1_q;
PREPARE t1_q AS SELECT * FROM t1_race WHERE id = $1;
EXECUTE t1_q(1);

-- Verify real L2 HIT
SELECT test_spc_race_l2_hit_count() > :t1_hits_before AS t1_hit_increased;
SELECT test_spc_last_l2_status() AS t1_last_status;
SELECT test_spc_entry_refcount('t1_q') = 0 AS t1_refcount_zero;
SELECT test_spc_has_pin() = false AS t1_no_pin;

-- ========================================================
-- T2: Invalidation during deserialization (is_valid=0)
-- ========================================================

CREATE TABLE t2_race (id int, val text);
INSERT INTO t2_race VALUES (1, 'world');

PREPARE t2_q AS SELECT * FROM t2_race WHERE id = $1;
EXECUTE t2_q(1);
EXECUTE t2_q(1);

SELECT test_spc_race_entry_is_valid_for_prep('t2_q') AS t2_pre_valid;
SELECT test_spc_l2_stale_after_deser_count() AS t2_stale_before
\gset
SELECT test_spc_hook_fired_count() AS t2_hook_before
\gset

-- Arm race: mark is_valid=0 between pin and deserialization
SELECT test_spc_arm_race_is_valid('t2_q') AS t2_armed;

-- DEALLOCATE+PREPARE clears L1. EXECUTE hits L2, hook fires, second validation fails.
DEALLOCATE t2_q;
PREPARE t2_q AS SELECT * FROM t2_race WHERE id = $1;
EXECUTE t2_q(1);

-- Verify all conditions: hook fired, stale detected, status, refcount, no pin, fallback
SELECT test_spc_hook_fired_count() > :t2_hook_before AS t2_hook_fired;
SELECT test_spc_l2_stale_after_deser_count() > :t2_stale_before AS t2_stale_detected;
SELECT test_spc_last_l2_status() AS t2_last_status;
SELECT test_spc_entry_refcount('t2_q') = 0 AS t2_refcount_zero;
SELECT test_spc_has_pin() = false AS t2_no_pin;
EXECUTE t2_q(1);

-- ========================================================
-- T3: Generation bump during deserialization
-- ========================================================

CREATE TABLE t3_race (id int);
INSERT INTO t3_race VALUES (1);

PREPARE t3_q AS SELECT * FROM t3_race WHERE id = $1;
EXECUTE t3_q(1);
EXECUTE t3_q(1);

SELECT test_spc_l2_stale_after_deser_count() AS t3_stale_before
\gset
SELECT test_spc_hook_fired_count() AS t3_hook_before
\gset

SELECT test_spc_arm_race_generation('t3_q') AS t3_armed;

DEALLOCATE t3_q;
PREPARE t3_q AS SELECT * FROM t3_race WHERE id = $1;
EXECUTE t3_q(1);

SELECT test_spc_hook_fired_count() > :t3_hook_before AS t3_hook_fired;
SELECT test_spc_l2_stale_after_deser_count() > :t3_stale_before AS t3_stale_detected;
SELECT test_spc_last_l2_status() AS t3_last_status;
SELECT test_spc_entry_refcount('t3_q') = 0 AS t3_refcount_zero;
SELECT test_spc_has_pin() = false AS t3_no_pin;
EXECUTE t3_q(1);

-- ========================================================
-- T4: Relcache is_valid=0 during deserialization
-- ========================================================

CREATE TABLE t4_race (id int);
INSERT INTO t4_race VALUES (1);

PREPARE t4_q AS SELECT * FROM t4_race WHERE id = $1;
EXECUTE t4_q(1);
EXECUTE t4_q(1);

SELECT test_spc_l2_stale_after_deser_count() AS t4_stale_before
\gset
SELECT test_spc_hook_fired_count() AS t4_hook_before
\gset

SELECT test_spc_arm_race_is_valid('t4_q') AS t4_armed;

DEALLOCATE t4_q;
PREPARE t4_q AS SELECT * FROM t4_race WHERE id = $1;
EXECUTE t4_q(1);

SELECT test_spc_hook_fired_count() > :t4_hook_before AS t4_hook_fired;
SELECT test_spc_l2_stale_after_deser_count() > :t4_stale_before AS t4_stale_detected;
SELECT test_spc_last_l2_status() AS t4_last_status;
SELECT test_spc_entry_refcount('t4_q') = 0 AS t4_refcount_zero;
SELECT test_spc_has_pin() = false AS t4_no_pin;
EXECUTE t4_q(1);

-- ========================================================
-- T5: Deserialization ERROR cleanup
-- T5a: controlled return path (DESER_ERROR inside SharedPlanCacheLookup)
-- T5b: elog(ERROR) path (exercises PG_CATCH in SharedPlanCacheTryLookup)
-- ========================================================

CREATE TABLE t5_race (id int);
INSERT INTO t5_race VALUES (1);

PREPARE t5_q AS SELECT * FROM t5_race WHERE id = $1;
EXECUTE t5_q(1);
EXECUTE t5_q(1);

-- T5a: Arm forced deser error (controlled return)
-- Capture key before arming to verify shared refcount after ERROR
SELECT test_spc_capture_key('t5_q') AS t5_captured_key
\gset
SELECT test_spc_arm_deser_error() AS t5a_armed;

DEALLOCATE t5_q;
PREPARE t5_q AS SELECT * FROM t5_race WHERE id = $1;
EXECUTE t5_q(1);

SELECT test_spc_has_pin() = false AS t5a_no_pin;
SELECT test_spc_entry_refcount('t5_q') = 0 AS t5a_refcount_zero;
-- Verify shared refcount for the exact captured key is 0
SELECT test_spc_entry_refcount_by_key(:'t5_captured_key'::bytea) = 0 AS t5a_shared_refcount_zero;

-- T5b: Arm elog(ERROR) deser error (exercises PG_CATCH pin release)
-- The ERROR is thrown inside SharedPlanCacheLookup after pin acquisition.
-- PG_CATCH in SharedPlanCacheTryLookup catches it, releases pin, counts error,
-- and falls back to local planning.  The query succeeds via fallback.
-- Capture key before arming for verification
SELECT test_spc_capture_key('t5_q') AS t5b_captured_key
\gset
SELECT test_spc_arm_deser_elog_error() AS t5b_armed;

DEALLOCATE t5_q;
PREPARE t5_q AS SELECT * FROM t5_race WHERE id = $1;
EXECUTE t5_q(1);

-- PG_CATCH in SharedPlanCacheTryLookup releases pin, falls back to local plan
SELECT test_spc_has_pin() = false AS t5b_no_pin;
SELECT test_spc_entry_refcount('t5_q') = 0 AS t5b_refcount_zero;
-- Verify shared refcount for the exact captured key is 0 after ERROR path
SELECT test_spc_entry_refcount_by_key(:'t5b_captured_key'::bytea) = 0 AS t5b_shared_refcount_zero;
-- PG_CATCH path sets status to ERROR via SharedPlanCacheL2CountError()
SELECT test_spc_last_l2_status() AS t5b_last_status;
EXECUTE t5_q(1);

-- ========================================================
-- T6: Refcount overflow protection (CAS loop)
-- ========================================================

CREATE TABLE t6_race (id int);
INSERT INTO t6_race VALUES (1);

PREPARE t6_q AS SELECT * FROM t6_race WHERE id = $1;
EXECUTE t6_q(1);
EXECUTE t6_q(1);

-- Set refcount to near-max to test overflow
SELECT test_spc_set_entry_refcount('t6_q', 4294967294) AS t6_set_max;

SELECT test_spc_race_l2_miss_count() AS t6_miss_before
\gset

-- This lookup should detect overflow via CAS and return MISS
DEALLOCATE t6_q;
PREPARE t6_q AS SELECT * FROM t6_race WHERE id = $1;
EXECUTE t6_q(1);

SELECT test_spc_race_l2_miss_count() > :t6_miss_before AS t6_overflow_miss;

-- Reset refcount and verify normal operation resumes
SELECT test_spc_set_entry_refcount('t6_q', 0) AS t6_reset;

DEALLOCATE t6_q;
PREPARE t6_q AS SELECT * FROM t6_race WHERE id = $1;
EXECUTE t6_q(1);
EXECUTE t6_q(1);

-- ========================================================
-- T7: before_shmem_exit cleanup with held pin
-- T7a: basic sanity (no pin held)
-- T7b: force real pin, then release via SharedPlanCacheShutdown path
--       (SQL-level: force pin, then explicitly release to verify cleanup)
-- ========================================================

SELECT test_spc_has_pin() = false AS t7a_no_pin;

-- T7b: Force a real pin (refcount incremented), then release it
CREATE TABLE t7_race (id int);
INSERT INTO t7_race VALUES (1);
PREPARE t7_q AS SELECT * FROM t7_race WHERE id = $1;
EXECUTE t7_q(1);
EXECUTE t7_q(1);

SELECT test_spc_entry_refcount('t7_q') = 0 AS t7b_pre_refcount;
SELECT test_spc_force_real_pin('t7_q') AS t7b_pin_forced;
SELECT test_spc_has_pin() AS t7b_pin_held;
SELECT test_spc_entry_refcount('t7_q') = 1 AS t7b_refcount_incremented;

-- Release pin (simulates what before_shmem_exit would do)
SELECT test_spc_release_pin() AS t7b_released;
SELECT test_spc_has_pin() = false AS t7b_pin_cleared;
SELECT test_spc_entry_refcount('t7_q') = 0 AS t7b_refcount_restored;

-- ========================================================
-- T8: Same-key stale overwrite with refcount
-- T8a: basic overwrite (refcount=0)
-- T8b: overwrite while refcount>0 using controlled test helper
--      that proves the exact captured key is touched
-- ========================================================

CREATE TABLE t8_race (id int, val text);
INSERT INTO t8_race VALUES (1, 'original');

PREPARE t8_q AS SELECT * FROM t8_race WHERE id = $1;
EXECUTE t8_q(1);
EXECUTE t8_q(1);

-- T8a: basic overwrite
SELECT test_spc_race_entry_is_valid_for_prep('t8_q') AS t8a_pre_valid;
SELECT test_spc_entry_refcount('t8_q') = 0 AS t8a_pre_refcount;

ALTER TABLE t8_race ADD COLUMN extra int;
DEALLOCATE t8_q;
PREPARE t8_q AS SELECT * FROM t8_race WHERE id = $1;
EXECUTE t8_q(1);
EXECUTE t8_q(1);

SELECT test_spc_race_entry_is_valid_for_prep('t8_q') AS t8a_post_valid;
SELECT test_spc_entry_refcount('t8_q') = 0 AS t8a_post_refcount;

-- T8b: overwrite while refcount>0 using controlled same-key overwrite helper
-- Capture the SharedPlanKey so we can prove the exact entry is touched
SELECT test_spc_capture_key('t8_q') AS t8b_captured_key
\gset

-- Verify the captured key entry exists with refcount exactly 0 (no pin held)
SELECT test_spc_entry_refcount_by_key(:'t8b_captured_key'::bytea) = 0 AS t8b_entry_refcount_zero;

-- Set refcount=2 on the captured key to simulate concurrent readers
SELECT test_spc_set_entry_refcount_by_key(:'t8b_captured_key'::bytea, 2) AS t8b_set_rc;

-- Verify refcount before overwrite
SELECT test_spc_entry_refcount_by_key(:'t8b_captured_key'::bytea) AS t8b_rc_before_overwrite;

-- This controlled helper touches the same captured entry and verifies that
-- same-entry stale overwrite must preserve refcount. It does not exercise the
-- full production payload replacement path. Production refcount preservation is
-- enforced by the store-path code structure in SharedPlanPublishEntry(), where
-- only new entries initialize refcount and found/stale overwrites preserve it.
SELECT test_spc_force_same_key_overwrite(:'t8b_captured_key'::bytea) AS t8b_overwrite_touched;

-- Verify: refcount for the same captured key remains 2 after overwrite
SELECT test_spc_entry_refcount_by_key(:'t8b_captured_key'::bytea) = 2 AS t8b_refcount_preserved;

-- Clean up: reset refcount on captured key
SELECT test_spc_set_entry_refcount_by_key(:'t8b_captured_key'::bytea, 0) AS t8b_reset;

-- Verify refcount returns 0 after cleanup
SELECT test_spc_entry_refcount_by_key(:'t8b_captured_key'::bytea) = 0 AS t8b_refcount_after_reset;

-- ========================================================
-- T9/T10: Regression sanity
-- ========================================================

SELECT test_spc_race_current_generation() >= 0 AS t9_gen_ok;
SELECT test_spc_race_current_store_epoch() >= 0 AS t9_epoch_ok;
SELECT test_spc_race_current_entries() >= 0 AS t10_entries_ok;

-- ========================================================
-- T11: Nested pin fail-closed
-- Simulates held pin via test helper, then verifies lookup returns MISS.
-- ========================================================

CREATE TABLE t11_race (id int);
INSERT INTO t11_race VALUES (1);

PREPARE t11_q AS SELECT * FROM t11_race WHERE id = $1;
EXECUTE t11_q(1);
EXECUTE t11_q(1);

-- Force nested pin state
SELECT test_spc_force_nested_pin('t11_q') AS t11_pin_forced;
SELECT test_spc_has_pin() AS t11_pin_held;

SELECT test_spc_race_l2_miss_count() AS t11_miss_before
\gset

-- DEALLOCATE+PREPARE+EXECUTE: lookup sees current_refcount_held=true → MISS
DEALLOCATE t11_q;
PREPARE t11_q AS SELECT * FROM t11_race WHERE id = $1;
EXECUTE t11_q(1);

-- Verify: missed due to nested pin, query still succeeded via local planning
SELECT test_spc_race_l2_miss_count() > :t11_miss_before AS t11_nested_miss;

-- Clean up the fake pin (don't decrement, it was never incremented)
SELECT test_spc_clear_nested_pin();
SELECT test_spc_has_pin() = false AS t11_pin_cleared;

-- ========================================================
-- T12: ReleasePin idempotent and real-pin release
-- T12a: Release when no pin is held (safe, idempotent)
-- T12b: Release with actual held pin (real refcount decrement)
-- ========================================================

-- T12a: idempotent no-op
SELECT test_spc_release_pin() AS t12a_release_noop;
SELECT test_spc_has_pin() = false AS t12a_no_pin;

-- T12b: force real pin then release
CREATE TABLE t12_race (id int);
INSERT INTO t12_race VALUES (1);
PREPARE t12_q AS SELECT * FROM t12_race WHERE id = $1;
EXECUTE t12_q(1);
EXECUTE t12_q(1);

SELECT test_spc_entry_refcount('t12_q') = 0 AS t12b_pre_refcount;
SELECT test_spc_force_real_pin('t12_q') AS t12b_pin_forced;
SELECT test_spc_has_pin() AS t12b_pin_held;
SELECT test_spc_entry_refcount('t12_q') = 1 AS t12b_refcount_is_1;

SELECT test_spc_release_pin() AS t12b_released;
SELECT test_spc_has_pin() = false AS t12b_pin_cleared;
SELECT test_spc_entry_refcount('t12_q') = 0 AS t12b_refcount_decremented;

-- ========================================================
-- T13: Specific-relid invalidation + same-key overwrite during deser
-- ========================================================

CREATE TABLE t13_race (id int, val text);
INSERT INTO t13_race VALUES (1, 'epoch_test');

PREPARE t13_q AS SELECT * FROM t13_race WHERE id = $1;
EXECUTE t13_q(1);
EXECUTE t13_q(1);

SELECT test_spc_l2_stale_after_deser_count() AS t13_stale_before
\gset
SELECT test_spc_hook_fired_count() AS t13_hook_before
\gset

SELECT test_spc_arm_race_epoch('t13_q') AS t13_armed;

DEALLOCATE t13_q;
PREPARE t13_q AS SELECT * FROM t13_race WHERE id = $1;
EXECUTE t13_q(1);

SELECT test_spc_hook_fired_count() > :t13_hook_before AS t13_hook_fired;
SELECT test_spc_l2_stale_after_deser_count() > :t13_stale_before AS t13_stale_detected;
SELECT test_spc_last_l2_status() AS t13_last_status;
SELECT test_spc_entry_refcount('t13_q') = 0 AS t13_refcount_zero;
SELECT test_spc_has_pin() = false AS t13_no_pin;
EXECUTE t13_q(1);

-- ========================================================
-- T14: Payload identity mismatch during deserialization
-- Verifies: detection, pin release, and fallback to local plan.
-- Uses a test-only forced mismatch flag that does not corrupt the
-- real entry.  Second-validation failure is read-only with respect
-- to the shared entry (does not invalidate it).
-- ========================================================

CREATE TABLE t14_race (id int, val text);
INSERT INTO t14_race VALUES (1, 'payload_test');

PREPARE t14_q AS SELECT * FROM t14_race WHERE id = $1;
EXECUTE t14_q(1);
EXECUTE t14_q(1);

SELECT test_spc_l2_stale_after_deser_count() AS t14_stale_before
\gset
SELECT test_spc_hook_fired_count() AS t14_hook_before
\gset

SELECT test_spc_arm_race_payload('t14_q') AS t14_armed;

DEALLOCATE t14_q;
PREPARE t14_q AS SELECT * FROM t14_race WHERE id = $1;
EXECUTE t14_q(1);

SELECT test_spc_hook_fired_count() > :t14_hook_before AS t14_hook_fired;
SELECT test_spc_l2_stale_after_deser_count() > :t14_stale_before AS t14_stale_detected;
SELECT test_spc_last_l2_status() AS t14_last_status;
SELECT test_spc_entry_refcount('t14_q') = 0 AS t14_refcount_zero;
SELECT test_spc_has_pin() = false AS t14_no_pin;
-- Entry must remain valid: second-validation failure does not invalidate
SELECT test_spc_race_entry_is_valid_for_prep('t14_q') AS t14_entry_still_valid;
EXECUTE t14_q(1);

-- ========================================================
-- Final
-- ========================================================

SELECT test_spc_has_pin() = false AS final_no_pin;

DEALLOCATE ALL;
DROP TABLE t1_race, t2_race, t3_race, t4_race, t5_race, t6_race,
           t7_race, t8_race, t11_race, t12_race, t13_race, t14_race;
