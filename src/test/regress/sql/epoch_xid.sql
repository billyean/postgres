-- epoch_xid.sql
-- Focused regression tests for XID64 epoch fork v1 gate fixes.
--
-- Tests:
--   a. COPY / heap_multi_insert policy (implicit vs materialized mode)
--   b. Rewrite-path rejection on materialized relations
--   c. Truncate behavior for materialized relations
--   d. Whole-slot-reset on LP reuse (insert after delete+vacuum)

-- ======================================================
-- Test a: COPY / heap_multi_insert policy
-- ======================================================

-- a1: COPY into a brand-new table (implicit mode) must succeed
CREATE TABLE epoch_copy_implicit (id int, val text);
SELECT epoch_xid_relation_mode('epoch_copy_implicit'::regclass);

-- In implicit mode (no epoch fork), COPY is allowed
COPY epoch_copy_implicit FROM stdin;
1	hello
2	world
\.

-- Verify data arrived
SELECT count(*) FROM epoch_copy_implicit;

-- Still implicit mode (COPY does not create epoch fork)
SELECT epoch_xid_relation_mode('epoch_copy_implicit'::regclass);
DROP TABLE epoch_copy_implicit;

-- a2: COPY into an epoch-materialized table must ERROR
CREATE TABLE epoch_copy_materialized (id int, val text);

-- Force materialization by doing a regular INSERT (which hooks the epoch path)
INSERT INTO epoch_copy_materialized VALUES (1, 'materializes the epoch fork');

-- Should now be materialized
SELECT epoch_xid_relation_mode('epoch_copy_materialized'::regclass);

-- COPY into a materialized relation must ERROR explicitly
COPY epoch_copy_materialized FROM stdin;
2	should fail
\.

DROP TABLE epoch_copy_materialized;


-- ======================================================
-- Test b: Rewrite-path rejection
-- ======================================================

CREATE TABLE epoch_rewrite_test (id int, val text);

-- Materialize the epoch fork
INSERT INTO epoch_rewrite_test VALUES (1, 'epoch data here');

-- Confirm materialized
SELECT epoch_xid_relation_mode('epoch_rewrite_test'::regclass);

-- CLUSTER on a materialized relation must ERROR
-- (CLUSTER requires an index, so create one first)
CREATE INDEX ON epoch_rewrite_test (id);
CLUSTER epoch_rewrite_test USING epoch_rewrite_test_id_idx;

-- VACUUM FULL on a materialized relation must ERROR
VACUUM FULL epoch_rewrite_test;

DROP TABLE epoch_rewrite_test;


-- ======================================================
-- Test c: Truncate behavior
-- ======================================================

CREATE TABLE epoch_truncate_test (id int);

-- Materialize with insert
INSERT INTO epoch_truncate_test VALUES (1);
INSERT INTO epoch_truncate_test VALUES (2);

SELECT epoch_xid_relation_mode('epoch_truncate_test'::regclass);

-- Verify epoch data exists on page 0
SELECT count(*) > 0 AS has_epoch_data
FROM epoch_xid_inspect('epoch_truncate_test'::regclass, 0)
WHERE epoch_flags != 0;

-- Truncate should succeed and clear epoch fork
TRUNCATE epoch_truncate_test;

-- After truncate, epoch fork may still exist but page 0 should not
-- (the fork is truncated to 0 blocks)
SELECT count(*) AS rows_after_truncate
FROM epoch_xid_inspect('epoch_truncate_test'::regclass, 0);

DROP TABLE epoch_truncate_test;


-- ======================================================
-- Test d: Whole-slot-reset on LP reuse
-- ======================================================

-- This test verifies that inserting into a reused OffsetNumber
-- does not inherit stale epoch metadata from the prior occupant.

CREATE TABLE epoch_reuse_test (id int);

-- Insert to materialize and populate slot 1
INSERT INTO epoch_reuse_test VALUES (1);

-- Inspect: slot 1 should have XMIN_SET (flags = 1), xmax_epoch = 0
SELECT offnum, xmin_epoch, xmax_epoch, epoch_flags
FROM epoch_xid_inspect('epoch_reuse_test'::regclass, 0)
WHERE offnum = 1;

-- Delete the row (sets xmax epoch on slot 1)
DELETE FROM epoch_reuse_test WHERE id = 1;

-- Inspect: slot 1 should now have XMIN_SET|XMAX_SET (flags = 3)
SELECT offnum, xmin_epoch, xmax_epoch, epoch_flags
FROM epoch_xid_inspect('epoch_reuse_test'::regclass, 0)
WHERE offnum = 1;

-- VACUUM to make slot 1 LP_UNUSED (available for reuse)
VACUUM epoch_reuse_test;

-- Insert again — the new tuple may reuse OffsetNumber 1
INSERT INTO epoch_reuse_test VALUES (2);

-- If the new tuple landed at offnum 1, verify the whole-slot-reset:
-- xmax_epoch must be 0 and flags must be exactly XMIN_SET (1),
-- NOT XMIN_SET|XMAX_SET (3) which would indicate stale state.
SELECT offnum, xmin_epoch, xmax_epoch, epoch_flags,
       CASE WHEN epoch_flags = 1 THEN 'CLEAN (whole-slot-reset correct)'
            WHEN epoch_flags = 3 THEN 'STALE (BUG: inherited xmax from prior occupant)'
            ELSE 'UNEXPECTED'
       END AS slot_state
FROM epoch_xid_inspect('epoch_reuse_test'::regclass, 0)
WHERE offnum = 1;

-- Explicit assertion: xmax_epoch must be exactly 0 after slot reuse.
-- A nonzero value here would mean the prior occupant's xmax leaked through.
SELECT xmax_epoch = 0 AS xmax_epoch_cleared,
       (epoch_flags & 2) = 0 AS xmax_flag_cleared
FROM epoch_xid_inspect('epoch_reuse_test'::regclass, 0)
WHERE offnum = 1;

DROP TABLE epoch_reuse_test;


-- ======================================================
-- Test e: Same-page non-HOT update epoch behavior
-- ======================================================

-- To guarantee a non-HOT update: create an index on the column being updated.
-- Updating an indexed column prevents HOT optimization.
-- Use a small tuple so it stays on the same page.
CREATE TABLE epoch_update_test (id int, val int);
CREATE INDEX epoch_update_test_val_idx ON epoch_update_test (val);
INSERT INTO epoch_update_test VALUES (1, 100);

-- Old slot should have XMIN_SET only
SELECT offnum, epoch_flags FROM epoch_xid_inspect('epoch_update_test'::regclass, 0) WHERE offnum = 1;

-- Update the indexed column: forces non-HOT, but tuple is small so stays same-page
UPDATE epoch_update_test SET val = 200 WHERE id = 1;

-- Old slot (offnum 1) should now have XMIN_SET|XMAX_SET (flags = 3)
SELECT offnum, epoch_flags FROM epoch_xid_inspect('epoch_update_test'::regclass, 0) WHERE offnum = 1;

-- New slot (offnum 2) should have XMIN_SET only (flags = 1), xmax_epoch = 0
SELECT offnum, xmax_epoch, epoch_flags FROM epoch_xid_inspect('epoch_update_test'::regclass, 0) WHERE offnum = 2;

DROP TABLE epoch_update_test;


-- ======================================================
-- Test f: Cross-page update epoch behavior
-- ======================================================

-- Strategy: fill page 0 nearly full with fixed-size rows, then update
-- one row to be much larger so it cannot fit on the same page.
-- Use non-compressible random-looking data to avoid TOAST compression.
CREATE TABLE epoch_crosspage_test (id int, val bytea);

-- Insert rows with 200-byte values to fill page 0
INSERT INTO epoch_crosspage_test
  SELECT g, decode(repeat(lpad(to_hex(g), 2, '0'), 100), 'hex')
  FROM generate_series(1, 30) g;

-- Verify materialized
SELECT epoch_xid_relation_mode('epoch_crosspage_test'::regclass);

-- Record original tuple location before update
CREATE TEMP TABLE _xpage_before AS
  SELECT ctid,
         (ctid::text::point)[0]::int AS orig_page,
         (ctid::text::point)[1]::int AS orig_offnum
  FROM epoch_crosspage_test WHERE id = 1;

-- Update row 1 with a large non-compressible value to force cross-page
UPDATE epoch_crosspage_test
  SET val = decode(repeat('deadbeef', 500), 'hex')
  WHERE id = 1;

-- Verify old tuple's epoch slot on its original page has XMIN_SET|XMAX_SET
SELECT epoch_flags
FROM epoch_xid_inspect('epoch_crosspage_test'::regclass,
                       (SELECT orig_page FROM _xpage_before))
WHERE offnum = (SELECT orig_offnum FROM _xpage_before);

-- Verify new tuple landed on a DIFFERENT page than the original
SELECT (ctid::text::point)[0]::int != (SELECT orig_page FROM _xpage_before)
       AS moved_to_different_page
FROM epoch_crosspage_test WHERE id = 1;

-- Inspect the new tuple's epoch slot on its actual destination page.
-- The new slot should have XMIN_SET only (flags = 1) with xmax_epoch = 0.
SELECT epoch_flags, xmax_epoch
FROM epoch_xid_inspect('epoch_crosspage_test'::regclass,
                       (SELECT (ctid::text::point)[0]::int
                        FROM epoch_crosspage_test WHERE id = 1))
WHERE offnum = (SELECT (ctid::text::point)[1]::int
                FROM epoch_crosspage_test WHERE id = 1);

DROP TABLE _xpage_before;
DROP TABLE epoch_crosspage_test;


-- ======================================================
-- Test g: HOT update produces same epoch behavior as non-HOT
-- ======================================================

CREATE TABLE epoch_hot_test (id int, val text);
CREATE INDEX ON epoch_hot_test (id);
INSERT INTO epoch_hot_test VALUES (1, 'original');

-- Update non-indexed column to trigger HOT
UPDATE epoch_hot_test SET val = 'hot-updated' WHERE id = 1;

-- Old slot (offnum 1): XMIN_SET|XMAX_SET (flags = 3)
SELECT offnum, epoch_flags FROM epoch_xid_inspect('epoch_hot_test'::regclass, 0) WHERE offnum = 1;

-- New slot (offnum 2): XMIN_SET only (flags = 1), xmax_epoch = 0
SELECT offnum, xmax_epoch, epoch_flags FROM epoch_xid_inspect('epoch_hot_test'::regclass, 0) WHERE offnum = 2;

DROP TABLE epoch_hot_test;


-- ======================================================
-- Test h: Implicit→materialized transition via UPDATE
-- ======================================================

CREATE TABLE epoch_implicit_update (id int, val text);
-- Use COPY to insert rows without materializing the epoch fork
COPY epoch_implicit_update FROM stdin;
1	original
2	also original
\.

-- Confirm implicit mode
SELECT epoch_xid_relation_mode('epoch_implicit_update'::regclass);

-- UPDATE should materialize the epoch fork
UPDATE epoch_implicit_update SET val = 'updated' WHERE id = 1;

-- Now materialized
SELECT epoch_xid_relation_mode('epoch_implicit_update'::regclass);

-- Old slot should have ONLY XMAX_SET (flags = 2), NOT XMIN_SET|XMAX_SET.
-- This is correct: the old tuple was inserted via COPY before the epoch fork
-- existed, so its xmin epoch was never recorded.  The update can only add
-- xmax epoch.  EPOCH_FLAG_XMIN_SET absent means "xmin epoch unknown, use
-- default epoch for reconstruction."
-- New slot gets whole-entry-reset with XMIN_SET (flags = 1).
SELECT offnum, epoch_flags
FROM epoch_xid_inspect('epoch_implicit_update'::regclass, 0)
WHERE epoch_flags != 0
ORDER BY offnum;

DROP TABLE epoch_implicit_update;


-- ======================================================
-- Test i: Update chain (INSERT + 3 UPDATEs)
-- ======================================================

CREATE TABLE epoch_chain_test (id int, val text);
INSERT INTO epoch_chain_test VALUES (1, 'v1');
UPDATE epoch_chain_test SET val = 'v2' WHERE id = 1;
UPDATE epoch_chain_test SET val = 'v3' WHERE id = 1;
UPDATE epoch_chain_test SET val = 'v4' WHERE id = 1;

-- Slots 1-3 should have XMIN_SET|XMAX_SET (flags = 3)
-- Slot 4 should have XMIN_SET only (flags = 1)
SELECT offnum, epoch_flags
FROM epoch_xid_inspect('epoch_chain_test'::regclass, 0)
WHERE offnum <= 4
ORDER BY offnum;

DROP TABLE epoch_chain_test;


-- ======================================================
-- Test j: LP reuse after update-driven invalidation
-- ======================================================

CREATE TABLE epoch_update_reuse (id int);
INSERT INTO epoch_update_reuse VALUES (1);
UPDATE epoch_update_reuse SET id = 2 WHERE id = 1;

-- Slot 1 now has xmax (flags = 3 from original insert + update)
SELECT offnum, epoch_flags FROM epoch_xid_inspect('epoch_update_reuse'::regclass, 0) WHERE offnum = 1;

VACUUM epoch_update_reuse;

-- Insert into reused slot: should get whole-slot-reset
INSERT INTO epoch_update_reuse VALUES (3);

-- Slot 1 should now have flags = 1 (XMIN_SET only), xmax_epoch = 0
SELECT offnum, xmax_epoch, epoch_flags,
       CASE WHEN epoch_flags = 1 THEN 'CLEAN'
            WHEN epoch_flags = 3 THEN 'STALE (BUG)'
            ELSE 'UNEXPECTED'
       END AS slot_state
FROM epoch_xid_inspect('epoch_update_reuse'::regclass, 0)
WHERE offnum = 1;

DROP TABLE epoch_update_reuse;


-- ======================================================
-- Phase 3: Read-side interpretation tests
-- ======================================================

-- Test k: Materialized tuple → 'materialized' interpretation
CREATE TABLE epoch_interp_mat (id int, val text);
INSERT INTO epoch_interp_mat VALUES (1, 'hello');

SELECT full_xmin > 0 AS has_xmin, full_xmax, xmin_interp, xmax_interp, relation_mode
FROM epoch_xid_tuple_visibility_info('epoch_interp_mat'::regclass, '(0,1)'::tid);

DROP TABLE epoch_interp_mat;

-- Test l: Materialized tuple after DELETE → 'materialized' xmax
CREATE TABLE epoch_interp_del (id int);
INSERT INTO epoch_interp_del VALUES (1);
DELETE FROM epoch_interp_del WHERE id = 1;

-- Use ctid (0,1) which is the now-dead-but-still-LP_NORMAL tuple
SELECT xmin_interp, xmax_interp, relation_mode
FROM epoch_xid_tuple_visibility_info('epoch_interp_del'::regclass, '(0,1)'::tid);

DROP TABLE epoch_interp_del;

-- Test m: Implicit-mode tuple → 'implicit_default'
CREATE TABLE epoch_interp_implicit (id int, val text);
COPY epoch_interp_implicit FROM stdin;
1	implicit row
\.

SELECT full_xmin > 0 AS has_xmin, xmin_interp, xmax_interp, relation_mode
FROM epoch_xid_tuple_visibility_info('epoch_interp_implicit'::regclass, '(0,1)'::tid);

DROP TABLE epoch_interp_implicit;

-- Test n: Mixed old-slot from implicit→materialized-via-UPDATE
CREATE TABLE epoch_interp_mixed (id int, val text);
COPY epoch_interp_mixed FROM stdin;
1	before materialization
\.

UPDATE epoch_interp_mixed SET val = 'after materialization' WHERE id = 1;

-- Old slot (0,1): xmin should be implicit_default, xmax materialized
SELECT xmin_interp, xmax_interp, relation_mode
FROM epoch_xid_tuple_visibility_info('epoch_interp_mixed'::regclass, '(0,1)'::tid);

DROP TABLE epoch_interp_mixed;

-- Test o: Frozen tuple → xmin_interp = 'frozen'
CREATE TABLE epoch_interp_frozen (id int);
INSERT INTO epoch_interp_frozen VALUES (1);
VACUUM FREEZE epoch_interp_frozen;

SELECT full_xmin, xmin_interp, xmax_interp
FROM epoch_xid_tuple_visibility_info('epoch_interp_frozen'::regclass, '(0,1)'::tid);

DROP TABLE epoch_interp_frozen;

-- Test p: Live tuple unset xmax → 'invalid_unset'
CREATE TABLE epoch_interp_live (id int);
INSERT INTO epoch_interp_live VALUES (1);

SELECT full_xmax, xmax_interp
FROM epoch_xid_tuple_visibility_info('epoch_interp_live'::regclass, '(0,1)'::tid);

DROP TABLE epoch_interp_live;

-- Test q: Non-LP_NORMAL → ERROR
CREATE TABLE epoch_interp_lpdead (id int);
INSERT INTO epoch_interp_lpdead VALUES (1);
DELETE FROM epoch_interp_lpdead WHERE id = 1;
VACUUM epoch_interp_lpdead;

-- After VACUUM, slot (0,1) should be LP_UNUSED or LP_DEAD
-- This should raise ERROR
SELECT * FROM epoch_xid_tuple_visibility_info('epoch_interp_lpdead'::regclass, '(0,1)'::tid);

DROP TABLE epoch_interp_lpdead;

-- Test r: Update chain intermediate tuple → both materialized
CREATE TABLE epoch_interp_chain (id int, val text);
INSERT INTO epoch_interp_chain VALUES (1, 'v1');
UPDATE epoch_interp_chain SET val = 'v2' WHERE id = 1;
UPDATE epoch_interp_chain SET val = 'v3' WHERE id = 1;

-- Slot (0,2) is the intermediate: xmin from first update, xmax from second
SELECT xmin_interp, xmax_interp
FROM epoch_xid_tuple_visibility_info('epoch_interp_chain'::regclass, '(0,2)'::tid);

DROP TABLE epoch_interp_chain;


-- Test s: Materialized relation with per-slot fallback to implicit_default
-- This proves the per-slot-absence prototype rule: within a materialized
-- relation, slots that were never explicitly written fall back to
-- implicit_default interpretation rather than claiming materialized state.
CREATE TABLE epoch_interp_fallback (id int, val text);

-- Insert row 1 via COPY (does not write epoch metadata)
COPY epoch_interp_fallback FROM stdin;
1	pre-materialization row
\.

-- Insert row 2 via regular INSERT (materializes the epoch fork)
INSERT INTO epoch_interp_fallback VALUES (2, 'materializer');

-- Now: relation is materialized, but slot for (0,1) has flags=0
-- (COPY did not write epoch data, only INSERT into slot 2 did)
SELECT relation_mode
FROM epoch_xid_tuple_visibility_info('epoch_interp_fallback'::regclass, '(0,1)'::tid);

-- Row 1 should get implicit_default interpretation despite materialized mode
SELECT xmin_interp, xmax_interp
FROM epoch_xid_tuple_visibility_info('epoch_interp_fallback'::regclass, '(0,1)'::tid);

-- Row 2 (the materializer) should get materialized interpretation
SELECT xmin_interp, xmax_interp
FROM epoch_xid_tuple_visibility_info('epoch_interp_fallback'::regclass, '(0,2)'::tid);

DROP TABLE epoch_interp_fallback;


-- ======================================================
-- Phase 4: Transaction-state classification tests
-- ======================================================

-- Test t1: Committed live tuple
CREATE TABLE epoch_txn_live (id int);
INSERT INTO epoch_txn_live VALUES (1);

SELECT xmin_status, xmax_status, tuple_state, relation_mode
FROM epoch_xid_tuple_txn_state_info('epoch_txn_live'::regclass, '(0,1)'::tid);

DROP TABLE epoch_txn_live;

-- Test t2: Dead committed tuple
CREATE TABLE epoch_txn_dead (id int);
INSERT INTO epoch_txn_dead VALUES (1);
DELETE FROM epoch_txn_dead WHERE id = 1;

SELECT xmin_status, xmax_status, tuple_state
FROM epoch_xid_tuple_txn_state_info('epoch_txn_dead'::regclass, '(0,1)'::tid);

DROP TABLE epoch_txn_dead;

-- Test t3: Implicit-mode tuple
CREATE TABLE epoch_txn_implicit (id int);
COPY epoch_txn_implicit FROM stdin;
1
\.

SELECT xmin_status, xmax_status, tuple_state, relation_mode
FROM epoch_xid_tuple_txn_state_info('epoch_txn_implicit'::regclass, '(0,1)'::tid);

DROP TABLE epoch_txn_implicit;

-- Test t4: Mixed old-slot
CREATE TABLE epoch_txn_mixed (id int, val text);
COPY epoch_txn_mixed FROM stdin;
1	before
\.

UPDATE epoch_txn_mixed SET val = 'after' WHERE id = 1;

SELECT xmin_status, xmax_status, tuple_state, relation_mode
FROM epoch_xid_tuple_txn_state_info('epoch_txn_mixed'::regclass, '(0,1)'::tid);

DROP TABLE epoch_txn_mixed;

-- Test t5: Frozen tuple
CREATE TABLE epoch_txn_frozen (id int);
INSERT INTO epoch_txn_frozen VALUES (1);
VACUUM FREEZE epoch_txn_frozen;

SELECT xmin_status, xmax_status, tuple_state
FROM epoch_xid_tuple_txn_state_info('epoch_txn_frozen'::regclass, '(0,1)'::tid);

DROP TABLE epoch_txn_frozen;

-- Test t6: Unset xmax
CREATE TABLE epoch_txn_unset (id int);
INSERT INTO epoch_txn_unset VALUES (1);

SELECT xmax_status, tuple_state
FROM epoch_xid_tuple_txn_state_info('epoch_txn_unset'::regclass, '(0,1)'::tid);

DROP TABLE epoch_txn_unset;

-- Test t7: Non-LP_NORMAL → ERROR
CREATE TABLE epoch_txn_lp (id int);
INSERT INTO epoch_txn_lp VALUES (1);
DELETE FROM epoch_txn_lp WHERE id = 1;
VACUUM epoch_txn_lp;

SELECT * FROM epoch_xid_tuple_txn_state_info('epoch_txn_lp'::regclass, '(0,1)'::tid);

DROP TABLE epoch_txn_lp;

-- Test t8: Aborted insert (PROVES CLOG consulted)
CREATE TABLE epoch_txn_abort (id int);
BEGIN;
INSERT INTO epoch_txn_abort VALUES (1);
ROLLBACK;

SELECT xmin_status, tuple_state
FROM epoch_xid_tuple_txn_state_info('epoch_txn_abort'::regclass, '(0,1)'::tid);

DROP TABLE epoch_txn_abort;

-- Test t9: In-progress insert
CREATE TABLE epoch_txn_inprog (id int);
BEGIN;
INSERT INTO epoch_txn_inprog VALUES (1);

SELECT xmin_status, tuple_state
FROM epoch_xid_tuple_txn_state_info('epoch_txn_inprog'::regclass, '(0,1)'::tid);

COMMIT;
DROP TABLE epoch_txn_inprog;

-- Test t10: Materialized + per-slot fallback
CREATE TABLE epoch_txn_fallback (id int, val text);
COPY epoch_txn_fallback FROM stdin;
1	pre-materialization row
\.

INSERT INTO epoch_txn_fallback VALUES (2, 'materializer');

SELECT xmin_status, xmax_status, tuple_state, relation_mode
FROM epoch_xid_tuple_txn_state_info('epoch_txn_fallback'::regclass, '(0,1)'::tid);

DROP TABLE epoch_txn_fallback;


-- ======================================================
-- Phase 5: Current-snapshot visibility tests
-- ======================================================

-- Test v1: Committed live → visible
CREATE TABLE epoch_vis_live (id int);
INSERT INTO epoch_vis_live VALUES (1);

SELECT visibility_verdict, verdict_reason, relation_mode
FROM epoch_xid_tuple_current_visibility_info('epoch_vis_live'::regclass, '(0,1)'::tid);

DROP TABLE epoch_vis_live;

-- Test v2: Deleted committed → invisible
CREATE TABLE epoch_vis_dead (id int);
INSERT INTO epoch_vis_dead VALUES (1);
DELETE FROM epoch_vis_dead WHERE id = 1;

SELECT visibility_verdict, verdict_reason
FROM epoch_xid_tuple_current_visibility_info('epoch_vis_dead'::regclass, '(0,1)'::tid);

DROP TABLE epoch_vis_dead;

-- Test v3: Frozen → visible / frozen
CREATE TABLE epoch_vis_frozen (id int);
INSERT INTO epoch_vis_frozen VALUES (1);
VACUUM FREEZE epoch_vis_frozen;

SELECT visibility_verdict, verdict_reason
FROM epoch_xid_tuple_current_visibility_info('epoch_vis_frozen'::regclass, '(0,1)'::tid);

DROP TABLE epoch_vis_frozen;

-- Test v4: Aborted insert → invisible / xmin_aborted
CREATE TABLE epoch_vis_abort (id int);
BEGIN;
INSERT INTO epoch_vis_abort VALUES (1);
ROLLBACK;

SELECT visibility_verdict, verdict_reason
FROM epoch_xid_tuple_current_visibility_info('epoch_vis_abort'::regclass, '(0,1)'::tid);

DROP TABLE epoch_vis_abort;

-- Test v5: Own insert → visible / own_insert_visible
CREATE TABLE epoch_vis_own (id int);
BEGIN;
INSERT INTO epoch_vis_own VALUES (1);

SELECT visibility_verdict, verdict_reason
FROM epoch_xid_tuple_current_visibility_info('epoch_vis_own'::regclass, '(0,1)'::tid);

COMMIT;
DROP TABLE epoch_vis_own;

-- Test v6: Own delete → invisible / own_delete_invisible
CREATE TABLE epoch_vis_owndel (id int);
INSERT INTO epoch_vis_owndel VALUES (1);
BEGIN;
DELETE FROM epoch_vis_owndel WHERE id = 1;

SELECT visibility_verdict, verdict_reason
FROM epoch_xid_tuple_current_visibility_info('epoch_vis_owndel'::regclass, '(0,1)'::tid);

COMMIT;
DROP TABLE epoch_vis_owndel;

-- Test v7: Implicit mode → visible
CREATE TABLE epoch_vis_implicit (id int);
COPY epoch_vis_implicit FROM stdin;
1
\.

SELECT visibility_verdict, verdict_reason, relation_mode
FROM epoch_xid_tuple_current_visibility_info('epoch_vis_implicit'::regclass, '(0,1)'::tid);

DROP TABLE epoch_vis_implicit;

-- Test v8: Non-LP_NORMAL → ERROR
CREATE TABLE epoch_vis_lp (id int);
INSERT INTO epoch_vis_lp VALUES (1);
DELETE FROM epoch_vis_lp WHERE id = 1;
VACUUM epoch_vis_lp;

SELECT * FROM epoch_xid_tuple_current_visibility_info('epoch_vis_lp'::regclass, '(0,1)'::tid);

DROP TABLE epoch_vis_lp;

-- Test v9: Mixed old-slot transitional case
CREATE TABLE epoch_vis_mixed (id int, val text);
COPY epoch_vis_mixed FROM stdin;
1	before materialization
\.

UPDATE epoch_vis_mixed SET val = 'after' WHERE id = 1;

SELECT xmin_status, xmax_status, visibility_verdict, verdict_reason, relation_mode
FROM epoch_xid_tuple_current_visibility_info('epoch_vis_mixed'::regclass, '(0,1)'::tid);

DROP TABLE epoch_vis_mixed;

-- Test v10: Materialized + per-slot fallback
CREATE TABLE epoch_vis_fallback (id int, val text);
COPY epoch_vis_fallback FROM stdin;
1	pre-materialization row
\.

INSERT INTO epoch_vis_fallback VALUES (2, 'materializer');

SELECT xmin_status, xmax_status, visibility_verdict, verdict_reason, relation_mode
FROM epoch_xid_tuple_current_visibility_info('epoch_vis_fallback'::regclass, '(0,1)'::tid);

DROP TABLE epoch_vis_fallback;
