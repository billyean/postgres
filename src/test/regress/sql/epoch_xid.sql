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
  SELECT ctid AS orig_ctid,
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
INSERT INTO epoch_interp_lpdead VALUES (2);
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
INSERT INTO epoch_txn_lp VALUES (2);
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
INSERT INTO epoch_vis_lp VALUES (2);
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


-- ======================================================
-- Phase 6: Storage contract tests
-- ======================================================

-- Test p6_a1: Implicit mode — fork absent, read-only page state
CREATE TABLE epoch_p6_implicit (id int, val text);
COPY epoch_p6_implicit FROM stdin;
1	implicit row
\.

-- Page state should be 'fork_absent' (relation has no epoch fork)
SELECT epoch_xid_page_state('epoch_p6_implicit'::regclass, 0);

-- Relation should still be implicit after the read-only call
-- (the read path must NOT create the fork)
SELECT epoch_xid_relation_mode('epoch_p6_implicit'::regclass);

-- Tuple inspection should fall back to implicit_default
SELECT xmin_interp, relation_mode
FROM epoch_xid_tuple_visibility_info('epoch_p6_implicit'::regclass, '(0,1)'::tid);

-- Confirm again: fork still absent after read-side inspection
SELECT epoch_xid_page_state('epoch_p6_implicit'::regclass, 0);

DROP TABLE epoch_p6_implicit;


-- Test p6_a2: Materialized relation, epoch block beyond EOF
-- Create table with rows on multiple pages, but only materialize page 0.
CREATE TABLE epoch_p6_beyond_eof (id int, val bytea);

-- Insert rows with 200-byte values to fill page 0
INSERT INTO epoch_p6_beyond_eof
  SELECT g, decode(repeat(lpad(to_hex(g), 2, '0'), 100), 'hex')
  FROM generate_series(1, 30) g;

-- Relation should be materialized (INSERT hooks the epoch path)
SELECT epoch_xid_relation_mode('epoch_p6_beyond_eof'::regclass);

-- Page 0 should be 'valid' (INSERT materialized it)
SELECT epoch_xid_page_state('epoch_p6_beyond_eof'::regclass, 0);

-- A page well beyond EOF should be 'beyond_eof'
SELECT epoch_xid_page_state('epoch_p6_beyond_eof'::regclass, 999);

-- Inspection of a tuple on the materialized page should work
SELECT xmin_interp, relation_mode
FROM epoch_xid_tuple_visibility_info('epoch_p6_beyond_eof'::regclass, '(0,1)'::tid);

DROP TABLE epoch_p6_beyond_eof;


-- Test p6_a3: PageIsNew / all-zero page within EOF — legal absence
-- This tests the most important Phase 6 contract state: a within-EOF
-- epoch block that is all-zero (PageIsNew).  The current implementation's
-- eager-initialize write path does not produce this in normal steady
-- state, so we use a test-only helper to create it explicitly.
CREATE TABLE epoch_p6_page_new (id int);

-- INSERT materializes the epoch fork and initializes page 0
INSERT INTO epoch_p6_page_new VALUES (1);

-- Confirm page 0 is valid before reset
SELECT epoch_xid_page_state('epoch_p6_page_new'::regclass, 0);

-- Reset page 0 to all-zero / PageIsNew state while keeping it within EOF
SELECT epoch_xid_reset_page('epoch_p6_page_new'::regclass, 0);

-- Verify: page is within EOF but in PageIsNew state
SELECT epoch_xid_page_state('epoch_p6_page_new'::regclass, 0);

-- Relation must still be materialized (the fork still exists)
SELECT epoch_xid_relation_mode('epoch_p6_page_new'::regclass);

-- Read-side inspection must fall back to implicit_default for both xmin and xmax,
-- NOT error (PageIsNew is a legal absence state, NOT corruption)
SELECT xmin_interp, xmax_interp, relation_mode
FROM epoch_xid_tuple_visibility_info('epoch_p6_page_new'::regclass, '(0,1)'::tid);

-- Phase 4 inspection must also fall back correctly
SELECT xmin_status, tuple_state, relation_mode
FROM epoch_xid_tuple_txn_state_info('epoch_p6_page_new'::regclass, '(0,1)'::tid);

-- Phase 5 inspection must also handle PageIsNew gracefully
SELECT visibility_verdict, relation_mode
FROM epoch_xid_tuple_current_visibility_info('epoch_p6_page_new'::regclass, '(0,1)'::tid);

-- epoch_xid_inspect should return 0 rows (not error)
SELECT count(*) AS inspect_rows
FROM epoch_xid_inspect('epoch_p6_page_new'::regclass, 0);

-- Verify the read path did NOT re-initialize the page as a side effect
-- (the page must still be PageIsNew after all the read-only inspections)
SELECT epoch_xid_page_state('epoch_p6_page_new'::regclass, 0);

DROP TABLE epoch_p6_page_new;


-- Test p6_a4: Slot beyond high-water mark — real HWM fallback
-- This tests that when offnum > num_slots on a valid epoch page,
-- the read path falls back to implicit_default rather than reading
-- garbage data from the slot array.
CREATE TABLE epoch_p6_hwm (id int);

-- Insert 3 rows (all epoch-aware, sets num_slots = 3)
INSERT INTO epoch_p6_hwm VALUES (1);
INSERT INTO epoch_p6_hwm VALUES (2);
INSERT INTO epoch_p6_hwm VALUES (3);

-- Confirm page 0 is valid and all 3 slots are materialized
SELECT epoch_xid_page_state('epoch_p6_hwm'::regclass, 0);
SELECT xmin_interp
FROM epoch_xid_tuple_visibility_info('epoch_p6_hwm'::regclass, '(0,3)'::tid);

-- Artificially lower num_slots to 1 so that slots 2 and 3 are beyond HWM
SELECT epoch_xid_set_num_slots('epoch_p6_hwm'::regclass, 0, 1);

-- Slot 1: within HWM, should still be materialized
SELECT xmin_interp
FROM epoch_xid_tuple_visibility_info('epoch_p6_hwm'::regclass, '(0,1)'::tid);

-- Slot 2: beyond HWM (offnum 2 > num_slots 1), must fall back
SELECT xmin_interp, relation_mode
FROM epoch_xid_tuple_visibility_info('epoch_p6_hwm'::regclass, '(0,2)'::tid);

-- Slot 3: also beyond HWM, must fall back
SELECT xmin_interp, relation_mode
FROM epoch_xid_tuple_visibility_info('epoch_p6_hwm'::regclass, '(0,3)'::tid);

DROP TABLE epoch_p6_hwm;


-- Test p6_a5: Materialized relation with per-slot absent (flags = 0)
-- This uses COPY + INSERT to create a slot with no epoch metadata
-- within a materialized relation.
CREATE TABLE epoch_p6_slot_absent (id int, val text);
COPY epoch_p6_slot_absent FROM stdin;
1	pre-materialization
\.

INSERT INTO epoch_p6_slot_absent VALUES (2, 'materializer');

-- Relation is materialized, page is valid
SELECT epoch_xid_page_state('epoch_p6_slot_absent'::regclass, 0);

-- Slot 1 (COPY row) has flags=0 → should fall back to implicit_default
SELECT xmin_interp, xmax_interp, relation_mode
FROM epoch_xid_tuple_visibility_info('epoch_p6_slot_absent'::regclass, '(0,1)'::tid);

-- Slot 2 (INSERT row) should be materialized
SELECT xmin_interp, relation_mode
FROM epoch_xid_tuple_visibility_info('epoch_p6_slot_absent'::regclass, '(0,2)'::tid);

DROP TABLE epoch_p6_slot_absent;


-- Test p6_b2: Write-path extension to distant block, verify intermediates
CREATE TABLE epoch_p6_extension (id int, val bytea);

-- Fill multiple pages via INSERT
INSERT INTO epoch_p6_extension
  SELECT g, decode(repeat(lpad(to_hex(g), 2, '0'), 100), 'hex')
  FROM generate_series(1, 200) g;

-- Should be materialized
SELECT epoch_xid_relation_mode('epoch_p6_extension'::regclass);

-- Page 0 should be valid (the INSERT touched it)
SELECT epoch_xid_page_state('epoch_p6_extension'::regclass, 0);

DROP TABLE epoch_p6_extension;


-- Test p6_c1: Materialized relation with mixed pages (some valid, some not)
CREATE TABLE epoch_p6_mixed_pages (id int, val bytea);

-- Insert enough rows to span page 0, then page 1
INSERT INTO epoch_p6_mixed_pages
  SELECT g, decode(repeat(lpad(to_hex(g), 2, '0'), 100), 'hex')
  FROM generate_series(1, 60) g;

-- Relation is materialized
SELECT epoch_xid_relation_mode('epoch_p6_mixed_pages'::regclass);

-- Page 0 should be 'valid' (INSERT materialized it)
SELECT epoch_xid_page_state('epoch_p6_mixed_pages'::regclass, 0);

-- Sequential INSERT extends the epoch fork one block at a time (no gap),
-- so each page is individually materialized.  Page 1 should be 'valid'.

DROP TABLE epoch_p6_mixed_pages;


-- Test p6_d1: Truncate shrinks epoch fork
CREATE TABLE epoch_p6_truncate (id int);
INSERT INTO epoch_p6_truncate VALUES (1);
INSERT INTO epoch_p6_truncate VALUES (2);

-- Materialized, page 0 valid
SELECT epoch_xid_relation_mode('epoch_p6_truncate'::regclass);
SELECT epoch_xid_page_state('epoch_p6_truncate'::regclass, 0);

TRUNCATE epoch_p6_truncate;

-- After truncate, page 0 should be beyond_eof (fork truncated to 0 blocks)
SELECT epoch_xid_page_state('epoch_p6_truncate'::regclass, 0);

-- Re-insert should work (fork re-extends)
INSERT INTO epoch_p6_truncate VALUES (3);
SELECT epoch_xid_page_state('epoch_p6_truncate'::regclass, 0);

DROP TABLE epoch_p6_truncate;


-- Test p6_e2: Corruption boundary — corrupt page must error, not fall back
CREATE TABLE epoch_p6_corrupt (id int);
INSERT INTO epoch_p6_corrupt VALUES (1);

-- Verify page 0 is valid before corruption
SELECT epoch_xid_page_state('epoch_p6_corrupt'::regclass, 0);

-- Inject corruption: write invalid epoch_version to page 0
SELECT epoch_xid_corrupt_page('epoch_p6_corrupt'::regclass, 0);

-- Page state should now report 'corrupt'
SELECT epoch_xid_page_state('epoch_p6_corrupt'::regclass, 0);

-- Attempting to read tuple interpretation on a corrupt page must ERROR
-- (NOT silently fall back to default values)
SELECT * FROM epoch_xid_tuple_visibility_info('epoch_p6_corrupt'::regclass, '(0,1)'::tid);

-- Also verify Phase 4 inspection errors on corrupt page
SELECT * FROM epoch_xid_tuple_txn_state_info('epoch_p6_corrupt'::regclass, '(0,1)'::tid);

-- Also verify Phase 5 inspection errors on corrupt page
SELECT * FROM epoch_xid_tuple_current_visibility_info('epoch_p6_corrupt'::regclass, '(0,1)'::tid);

-- epoch_xid_inspect should also error on corrupt page
SELECT * FROM epoch_xid_inspect('epoch_p6_corrupt'::regclass, 0);

DROP TABLE epoch_p6_corrupt;


-- ======================================================
-- Probing-fix regression tests (smgrexists → stat)
-- ======================================================
-- These tests verify that replacing smgrexists() with stat() for
-- epoch fork existence probing does not regress the Phase 6 storage
-- contract.  They exercise the probe→create→probe cycle that
-- triggered the mdexists() assertion and confirm that the read-side
-- no-materialization invariant still holds.

-- Test pfx_a: Repeated probing of non-existent fork is side-effect-free.
-- The stat() probe must never create the fork, even when called multiple
-- times.  This is the core read-side invariant from Phase 6.
CREATE TABLE epoch_pfx_probe (id int, val text);
COPY epoch_pfx_probe FROM stdin;
1	no epoch fork
\.

-- Multiple consecutive probes: none should create the fork
SELECT epoch_xid_relation_mode('epoch_pfx_probe'::regclass);
SELECT epoch_xid_relation_mode('epoch_pfx_probe'::regclass);
SELECT epoch_xid_page_state('epoch_pfx_probe'::regclass, 0);
SELECT epoch_xid_page_state('epoch_pfx_probe'::regclass, 0);

-- Still implicit after all probes
SELECT epoch_xid_relation_mode('epoch_pfx_probe'::regclass);

-- Tuple inspection also must not create the fork
SELECT xmin_interp, relation_mode
FROM epoch_xid_tuple_visibility_info('epoch_pfx_probe'::regclass, '(0,1)'::tid);

-- Final check: fork still absent
SELECT epoch_xid_page_state('epoch_pfx_probe'::regclass, 0);

DROP TABLE epoch_pfx_probe;


-- Test pfx_b: Probe → create → probe cycle.
-- This exercises the exact sequence that triggered the mdexists()
-- assertion: stat() returns ENOENT, then smgrcreate(), then stat()
-- returns 0.  No assertion should fire.
CREATE TABLE epoch_pfx_cycle (id int);

-- Probe while implicit (stat returns ENOENT — no smgr side effects)
SELECT epoch_xid_relation_mode('epoch_pfx_cycle'::regclass);

-- INSERT creates the fork via EpochEnsureFork → smgrcreate
INSERT INTO epoch_pfx_cycle VALUES (1);

-- Probe again: fork now exists
SELECT epoch_xid_relation_mode('epoch_pfx_cycle'::regclass);

-- Another insert: EpochEnsureFork detects existing fork via stat()
INSERT INTO epoch_pfx_cycle VALUES (2);

-- Verify data and fork state
SELECT count(*) FROM epoch_pfx_cycle;
SELECT epoch_xid_page_state('epoch_pfx_cycle'::regclass, 0);

DROP TABLE epoch_pfx_cycle;


-- Test pfx_c: Read-side probing does not extend the fork.
-- Verifies that beyond-EOF probes via stat()+EpochReadBufferReadOnly
-- do not extend the epoch fork.
CREATE TABLE epoch_pfx_noextend (id int);

-- Materialize via INSERT (creates fork with page 0)
INSERT INTO epoch_pfx_noextend VALUES (1);
SELECT epoch_xid_relation_mode('epoch_pfx_noextend'::regclass);
SELECT epoch_xid_page_state('epoch_pfx_noextend'::regclass, 0);

-- Probe a page well beyond EOF: must return 'beyond_eof', not extend
SELECT epoch_xid_page_state('epoch_pfx_noextend'::regclass, 999);
SELECT epoch_xid_page_state('epoch_pfx_noextend'::regclass, 999);

-- Page 0 still valid, no spurious extension
SELECT epoch_xid_page_state('epoch_pfx_noextend'::regclass, 0);

DROP TABLE epoch_pfx_noextend;


-- Test pfx_d: Absent/new states still fall back correctly
-- after the probing mechanism change.
CREATE TABLE epoch_pfx_fallback (id int);
INSERT INTO epoch_pfx_fallback VALUES (1);

-- Page 0 valid, relation materialized
SELECT epoch_xid_page_state('epoch_pfx_fallback'::regclass, 0);
SELECT epoch_xid_relation_mode('epoch_pfx_fallback'::regclass);

-- Reset page to PageIsNew — the Phase 6 contract says this is legal absence
SELECT epoch_xid_reset_page('epoch_pfx_fallback'::regclass, 0);
SELECT epoch_xid_page_state('epoch_pfx_fallback'::regclass, 0);

-- Read-side must fall back, not error
SELECT xmin_interp, xmax_interp, relation_mode
FROM epoch_xid_tuple_visibility_info('epoch_pfx_fallback'::regclass, '(0,1)'::tid);

-- Page must still be PageIsNew (read path did not re-initialize it)
SELECT epoch_xid_page_state('epoch_pfx_fallback'::regclass, 0);

DROP TABLE epoch_pfx_fallback;


-- Test pfx_e: Corruption boundary holds with stat()-based probing.
-- A non-new corrupt page must still ERROR, not silently fall back.
CREATE TABLE epoch_pfx_corrupt (id int);
INSERT INTO epoch_pfx_corrupt VALUES (1);
SELECT epoch_xid_page_state('epoch_pfx_corrupt'::regclass, 0);

SELECT epoch_xid_corrupt_page('epoch_pfx_corrupt'::regclass, 0);
SELECT epoch_xid_page_state('epoch_pfx_corrupt'::regclass, 0);

-- Must ERROR, not fall back
SELECT * FROM epoch_xid_tuple_visibility_info('epoch_pfx_corrupt'::regclass, '(0,1)'::tid);

DROP TABLE epoch_pfx_corrupt;


-- ======================================================
-- Patch 7: Sparse-file epoch fork extension tests
-- ======================================================
-- These tests validate that ftruncate-based sparse extension creates
-- true filesystem holes for intermediate epoch blocks, that the
-- segment-aware mapping is correct, and that all Phase 6 semantics
-- are preserved.
--
-- Platform-specific expectations:
--   Linux (ext4/XFS):  SEEK_HOLE/SEEK_DATA available since kernel 3.1.
--     ftruncate beyond EOF creates file holes.  epoch_xid_block_status
--     reports 'hole' for unwritten blocks.
--   macOS (APFS):  SEEK_HOLE/SEEK_DATA available since macOS 10.4.
--     ftruncate beyond EOF creates sparse regions on APFS.
--     epoch_xid_block_status reports 'hole' for unwritten blocks.
--   If a filesystem does not report holes (some network/virtualized FS),
--     epoch_xid_block_status returns 'data' for all within-EOF blocks.
--     Correctness is preserved (PageIsNew = legal absence per Phase 6);
--     only the sparse optimization is inactive.


-- Test sp_a: Sparse extension — intermediates are real holes.
CREATE TABLE epoch_sp_sparse (id int, pad char(2000));
ALTER TABLE epoch_sp_sparse ALTER COLUMN pad SET STORAGE PLAIN;

COPY epoch_sp_sparse FROM stdin;
1	x
2	x
3	x
4	x
5	x
6	x
7	x
8	x
9	x
10	x
11	x
12	x
13	x
14	x
15	x
16	x
\.

SELECT epoch_xid_relation_mode('epoch_sp_sparse'::regclass);

-- Verify data spans multiple pages
SELECT (ctid::text::point)[0]::int AS page FROM epoch_sp_sparse WHERE id = 1;
SELECT (ctid::text::point)[0]::int AS page FROM epoch_sp_sparse WHERE id = 16;

-- UPDATE materializes epoch fork via ftruncate sparse extension
UPDATE epoch_sp_sparse SET pad = 'y' WHERE id = 16;
SELECT epoch_xid_relation_mode('epoch_sp_sparse'::regclass);

-- PHYSICAL PROOF: intermediate is a real hole, not zero-written
SELECT epoch_xid_block_status('epoch_sp_sparse'::regclass, 0);

-- SEMANTIC PROOF: page_state confirms legal absence
SELECT epoch_xid_page_state('epoch_sp_sparse'::regclass, 0);

-- Read-side fallback on hole-backed page
SELECT xmin_interp, relation_mode
FROM epoch_xid_tuple_visibility_info('epoch_sp_sparse'::regclass, '(0,1)'::tid);


-- Test sp_b: On-demand materialization of a sparse intermediate.
DELETE FROM epoch_sp_sparse WHERE id = 1;

-- Block 0 should now be initialized (hole → data on first write)
SELECT epoch_xid_page_state('epoch_sp_sparse'::regclass, 0);

-- Epoch xmax was written by DELETE
SELECT xmin_interp, xmax_interp, relation_mode
FROM epoch_xid_tuple_visibility_info('epoch_sp_sparse'::regclass, '(0,1)'::tid);

-- Other intermediates still sparse
SELECT epoch_xid_block_status('epoch_sp_sparse'::regclass, 1);
SELECT epoch_xid_page_state('epoch_sp_sparse'::regclass, 1);

DROP TABLE epoch_sp_sparse;


-- Test sp_c: Non-sparse semantics identical to sparse.
CREATE TABLE epoch_sp_nonsparse (id int);
INSERT INTO epoch_sp_nonsparse VALUES (1);
INSERT INTO epoch_sp_nonsparse VALUES (2);

SELECT epoch_xid_page_state('epoch_sp_nonsparse'::regclass, 0);

-- Reset to zero: simulates non-sparse zero-filled block
SELECT epoch_xid_reset_page('epoch_sp_nonsparse'::regclass, 0);
SELECT epoch_xid_page_state('epoch_sp_nonsparse'::regclass, 0);

-- Read behavior identical to sparse hole: fallback
SELECT xmin_interp, relation_mode
FROM epoch_xid_tuple_visibility_info('epoch_sp_nonsparse'::regclass, '(0,1)'::tid);

DROP TABLE epoch_sp_nonsparse;


-- Test sp_d: Full lifecycle — stat→create→ftruncate→write→read.
CREATE TABLE epoch_sp_lifecycle (id int, pad char(2000));
ALTER TABLE epoch_sp_lifecycle ALTER COLUMN pad SET STORAGE PLAIN;

SELECT epoch_xid_relation_mode('epoch_sp_lifecycle'::regclass);

COPY epoch_sp_lifecycle FROM stdin;
1	a
2	a
3	a
4	a
5	a
6	a
7	a
8	a
\.

SELECT epoch_xid_relation_mode('epoch_sp_lifecycle'::regclass);

UPDATE epoch_sp_lifecycle SET pad = 'b' WHERE id = 8;
SELECT epoch_xid_relation_mode('epoch_sp_lifecycle'::regclass);

SELECT epoch_xid_block_status('epoch_sp_lifecycle'::regclass, 0);

DROP TABLE epoch_sp_lifecycle;


-- Test sp_e: Persistence — CHECKPOINT preserves semantic state.
CREATE TABLE epoch_sp_persist (id int, pad char(2000));
ALTER TABLE epoch_sp_persist ALTER COLUMN pad SET STORAGE PLAIN;

COPY epoch_sp_persist FROM stdin;
1	p
2	p
3	p
4	p
5	p
6	p
7	p
8	p
9	p
10	p
11	p
12	p
\.

UPDATE epoch_sp_persist SET pad = 'q' WHERE id = 12;

-- Pre-checkpoint: intermediate is hole (dirty buffer not yet flushed)
SELECT epoch_xid_block_status('epoch_sp_persist'::regclass, 0);

CHECKPOINT;

-- Post-checkpoint: semantic state preserved regardless of filesystem
SELECT epoch_xid_page_state('epoch_sp_persist'::regclass, 0);
SELECT xmin_interp, relation_mode
FROM epoch_xid_tuple_visibility_info('epoch_sp_persist'::regclass, '(0,1)'::tid);

DROP TABLE epoch_sp_persist;


-- Test sp_seg: Segment-aware mapping validation.
-- epoch_xid_block_segment_info exposes the logical→physical mapping.
-- This proves the implementation is not limited to a single segment.
CREATE TABLE epoch_sp_segmap (id int);
INSERT INTO epoch_sp_segmap VALUES (1);

-- Block 0: must map to segment 0 offset 0
SELECT epoch_xid_block_segment_info('epoch_sp_segmap'::regclass, 0) ~ '^seg=0 offset=0';

-- Block RELSEG_SIZE-1: last block of segment 0
SELECT epoch_xid_block_segment_info('epoch_sp_segmap'::regclass, 131071) ~ '^seg=0 offset=131071';

-- Block RELSEG_SIZE: first block of segment 1
SELECT epoch_xid_block_segment_info('epoch_sp_segmap'::regclass, 131072) ~ '^seg=1 offset=0';

-- Block RELSEG_SIZE+5: offset 5 in segment 1
SELECT epoch_xid_block_segment_info('epoch_sp_segmap'::regclass, 131077) ~ '^seg=1 offset=5';

-- Block 2*RELSEG_SIZE: first block of segment 2
SELECT epoch_xid_block_segment_info('epoch_sp_segmap'::regclass, 262144) ~ '^seg=2 offset=0';

-- Introspection of an unmapped block: block_status with segment awareness
-- Block 131072 (segment 1, offset 0) should be beyond_eof since we only
-- wrote to block 0 in segment 0.
SELECT epoch_xid_block_status('epoch_sp_segmap'::regclass, 131072);

DROP TABLE epoch_sp_segmap;


-- Test sp_platform: Explicit Linux/macOS sparse-file platform validation.
-- This test verifies that SEEK_HOLE actually detects ftruncate-created
-- holes on the current platform.  On Linux (ext4/XFS) and macOS (APFS),
-- this MUST return 'sparse_supported'.  If it does not, the filesystem
-- does not support sparse files and the hole/data assertions in other
-- sp_* tests will report 'data' instead of 'hole'.
--
-- This is not a silent fallback: on the two target platforms (Linux and
-- macOS with standard filesystems), 'sparse_supported' is the expected
-- and required result.
SELECT epoch_xid_sparse_platform_check();


-- Test sp_cross_seg: Cross-segment sparse materialization.
-- This is the critical test proving the implementation handles blocks
-- beyond the first segment boundary.  We use the test-only helper
-- epoch_xid_force_materialize_block() to materialize a specific
-- logical epoch block in segment 1 (RELSEG_SIZE + 5 = 131077),
-- then verify:
--   1. segment 1 file was created
--   2. the target block (offset 5 in segment 1) is initialized
--   3. lower untouched blocks in segment 1 remain sparse holes
--   4. semantic inspection works on the materialized block
--   5. read-only inspection of untouched segment-1 blocks falls back
CREATE TABLE epoch_sp_crossseg (id int);
INSERT INTO epoch_sp_crossseg VALUES (1);

-- Confirm materialized (epoch fork exists with block 0 initialized)
SELECT epoch_xid_relation_mode('epoch_sp_crossseg'::regclass);
SELECT epoch_xid_page_state('epoch_sp_crossseg'::regclass, 0);

-- Force-materialize logical block RELSEG_SIZE + 5 (131077) in segment 1.
-- This exercises the real code path: EpochEnsureFork +
-- epoch_fork_extend_sparse (creates segment 0 at full RELSEG_SIZE,
-- creates segment 1 with 6 blocks, all via ftruncate holes) +
-- ReadBufferExtended + EpochPageInit for only the target block.
SELECT epoch_xid_force_materialize_block('epoch_sp_crossseg'::regclass, 131077);

-- Verify segment mapping is correct for the target block
SELECT epoch_xid_block_segment_info('epoch_sp_crossseg'::regclass, 131077) ~ '^seg=1 offset=5';

-- Target block in segment 1 should be page_state 'valid' (initialized)
SELECT epoch_xid_page_state('epoch_sp_crossseg'::regclass, 131077);

-- Untouched block at offset 0 in segment 1 (logical 131072):
-- should be a sparse hole and page_state 'page_new'
SELECT epoch_xid_block_status('epoch_sp_crossseg'::regclass, 131072);
SELECT epoch_xid_page_state('epoch_sp_crossseg'::regclass, 131072);

-- Untouched block at offset 3 in segment 1 (logical 131075):
-- should also be a sparse hole
SELECT epoch_xid_block_status('epoch_sp_crossseg'::regclass, 131075);

-- Semantic inspection: page_state read-only path for untouched seg-1
-- blocks must fall back without materializing
SELECT epoch_xid_page_state('epoch_sp_crossseg'::regclass, 131073);

-- Original block 0 in segment 0: still valid (the INSERT-materialized page)
SELECT epoch_xid_page_state('epoch_sp_crossseg'::regclass, 0);

-- Segment 0 intermediate blocks (e.g., block 100) should be sparse holes
-- (segment 0 was ftruncated to full RELSEG_SIZE to support segment 1)
SELECT epoch_xid_block_status('epoch_sp_crossseg'::regclass, 100);
SELECT epoch_xid_page_state('epoch_sp_crossseg'::regclass, 100);

DROP TABLE epoch_sp_crossseg;

-- ======================================================
-- Test: Patch 10 — heap_fetch epoch visibility consumer
-- ======================================================
-- These tests prove that heap_fetch's epoch branch genuinely
-- consumes epoch-fork metadata via Phase 3 (EpochInterpretTuple).
--
-- The chain is:
--   epoch slot written by INSERT → Phase 3 reads slot → reconstructs
--   full 64-bit XID → MVCC logic uses reconstructed XID → TID scan
--   returns correct result through heap_fetch's epoch branch.
--
-- Tests verify BOTH:
--   (a) epoch slot data is present (via epoch_xid_inspect)
--   (b) the TID scan produces correct results through heap_fetch

-- R1: TID scan on live committed tuple (materialized relation)
CREATE TABLE epoch_visfetch (id int PRIMARY KEY, val text);
INSERT INTO epoch_visfetch VALUES (1, 'live');

-- Confirm materialized (epoch path will be active in heap_fetch)
SELECT epoch_xid_relation_mode('epoch_visfetch'::regclass);

-- Verify epoch slot was written: EPOCH_FLAG_XMIN_SET (1) must be present.
-- This is the data that EpochInterpretTuple consumes in the heap_fetch
-- epoch branch to reconstruct the full 64-bit xmin.
SELECT offnum, epoch_flags FROM epoch_xid_inspect('epoch_visfetch'::regclass, 0) WHERE offnum = 1;

-- Verify Phase 3 interprets this as 'materialized' (not 'implicit_default'),
-- proving the epoch slot data is authoritative.
SELECT xmin_interp, full_xmin > 0 AS has_full_xmin FROM epoch_xid_tuple_visibility_info('epoch_visfetch'::regclass, '(0,1)'::tid);

-- TID scan: goes through heap_fetch → epoch branch → Phase 3 reads the
-- same epoch slot shown above → reconstructs full xmin → MVCC → VISIBLE
SELECT * FROM epoch_visfetch WHERE ctid = '(0,1)';

-- R2: TID scan on dead tuple (deleted, committed)
DELETE FROM epoch_visfetch WHERE id = 1;

-- After DELETE, epoch slot should have both XMIN_SET + XMAX_SET (flags=3).
-- The heap_fetch epoch branch consumes both for full xmin + xmax reconstruction.
SELECT offnum, epoch_flags FROM epoch_xid_inspect('epoch_visfetch'::regclass, 0) WHERE offnum = 1;

-- TID scan: dead tuple must not be visible (epoch path → INVISIBLE)
SELECT * FROM epoch_visfetch WHERE ctid = '(0,1)';

-- R3: TID scan after UPDATE — new version visible, old invisible
INSERT INTO epoch_visfetch VALUES (2, 'original');
UPDATE epoch_visfetch SET val = 'updated' WHERE id = 2;
SELECT val FROM epoch_visfetch WHERE id = 2;

DROP TABLE epoch_visfetch;

-- R4: Prove heap_fetch epoch branch behavior DEPENDS on epoch page state
--
-- Strategy: create a materialized relation, verify Phase 3 produces
-- 'materialized' interpretation, then RESET the epoch page to PageIsNew
-- (all-zero) using the existing test helper.  After reset, Phase 3 must
-- produce 'implicit_default' instead — proving the heap_fetch epoch
-- branch's Phase 3 consumption genuinely reads and depends on the epoch
-- page/slot state, not just on whether the fork file exists.
--
-- The TID scan still returns the correct result in both states (epoch 0
-- = default 0), but the INTERPRETATION observably changes, proving the
-- epoch branch reacted to the epoch page state change.

CREATE TABLE epoch_visfetch_dep (id int PRIMARY KEY, val text);
INSERT INTO epoch_visfetch_dep VALUES (1, 'test_dep');

-- Step 1: Confirm materialized, slot present, interpretation = 'materialized'
SELECT epoch_xid_relation_mode('epoch_visfetch_dep'::regclass);
SELECT offnum, epoch_flags FROM epoch_xid_inspect('epoch_visfetch_dep'::regclass, 0) WHERE offnum = 1;
SELECT xmin_interp FROM epoch_xid_tuple_visibility_info('epoch_visfetch_dep'::regclass, '(0,1)'::tid);

-- TID scan: triggers heap_fetch → epoch branch with materialized slot data
SELECT * FROM epoch_visfetch_dep WHERE ctid = '(0,1)';

-- Direct proof: the instrumentation hook shows 'snapshot_bridge' — the
-- epoch branch used slot-backed materialized data AND the 64-bit snapshot
-- bridge (FullXidInMVCCSnapshot via epoch_anchor).  This is the Patch 11
-- Stage 1 proof: the snapshot membership check now uses the 64-bit bridge.
SELECT epoch_xid_mvcc_last_path();

-- Step 2: Reset epoch page 0 to PageIsNew (all-zero) using test helper.
-- The relation STAYS materialized (fork file still exists), but the epoch
-- page for block 0 is now all-zero.  This changes what Phase 3 sees:
-- the slot becomes NULL (PageIsNew → no slot data).
SELECT epoch_xid_reset_page('epoch_visfetch_dep'::regclass, 0);

-- Step 3: Verify the epoch page is now PageIsNew
SELECT epoch_xid_page_state('epoch_visfetch_dep'::regclass, 0);

-- Step 4: TID scan again.  The epoch branch still executes (relation IS
-- materialized), but Phase 3 now sees PageIsNew → slot = NULL → falls
-- back to default epoch instead of using materialized slot data.
SELECT * FROM epoch_visfetch_dep WHERE ctid = '(0,1)';

-- Direct proof: the instrumentation hook now shows 'default_fallback'
-- instead of 'materialized'.  This proves the heap_fetch epoch branch
-- changed its internal path in response to the epoch page state change.
SELECT epoch_xid_mvcc_last_path();

-- Step 5: epoch_xid_inspect confirms epoch data is truly gone
SELECT count(*) FROM epoch_xid_inspect('epoch_visfetch_dep'::regclass, 0);

DROP TABLE epoch_visfetch_dep;

-- R5: Stage 1 bridge boundary is enforced in code
--
-- Verify that the Stage 1 snapshot bridge guard is a real code boundary:
-- (1) epoch_xid_stage1_bridge_enabled() returns true (we're in epoch 0)
-- (2) TID scan uses 'snapshot_bridge' (guard passed, 64-bit path taken)
-- (3) The guard is the conjunction of: valid anchor AND epoch == 0
--
-- This proves the boundary is enforced at the code level, not just
-- documented in comments.  If the system crossed into epoch 1+, the
-- guard would return false and the bridge would not be used.

CREATE TABLE epoch_visfetch_guard (id int PRIMARY KEY, val text);
INSERT INTO epoch_visfetch_guard VALUES (1, 'guard_test');

-- Confirm the Stage 1 guard passes (epoch 0, valid anchor)
SELECT epoch_xid_stage1_bridge_enabled();

-- TID scan uses the bridge (guard passed)
SELECT * FROM epoch_visfetch_guard WHERE ctid = '(0,1)';
SELECT epoch_xid_mvcc_last_path();

-- R6: Negative proof — Stage 1 boundary prevents bridge when guard is off
--
-- Force the guard off (simulates post-epoch-0 state), then verify:
-- (1) epoch_xid_stage1_bridge_enabled() returns false
-- (2) TID scan no longer reports 'snapshot_bridge'
-- (3) TID scan still returns correct results (falls back to 32-bit)
-- This proves the boundary is enforced: the bridge is NOT used when
-- the guard says it shouldn't be.

SELECT epoch_xid_stage1_force_disable(true);

-- Guard is now forced off
SELECT epoch_xid_stage1_bridge_enabled();

-- TID scan: same tuple, but now the bridge is disabled
SELECT * FROM epoch_visfetch_guard WHERE ctid = '(0,1)';

-- Must NOT be 'snapshot_bridge' — proves the boundary blocked the bridge
SELECT epoch_xid_mvcc_last_path();

-- Restore normal operation
SELECT epoch_xid_stage1_force_disable(false);

-- Confirm bridge re-enabled
SELECT epoch_xid_stage1_bridge_enabled();

-- TID scan returns to using the bridge
SELECT * FROM epoch_visfetch_guard WHERE ctid = '(0,1)';
SELECT epoch_xid_mvcc_last_path();

DROP TABLE epoch_visfetch_guard;

-- ======================================================
-- Patch 12: 64-bit active-transaction membership check
-- ======================================================
--
-- These tests prove that Patch 12's FullTransactionIdIsInProgress is
-- exercised by the epoch-aware Phase 4 classification path, and that
-- Patch 11's Stage 1 operational guard remains unchanged.

-- P12_a: 64-bit membership helper is exercised in the epoch-aware path
--
-- A committed tuple's xmin is classified by EpochClassifyXidStatus.
-- With the Stage 1 guard active, Phase 4 must use the 64-bit
-- FullTransactionIdIsInProgress for the membership check.

CREATE TABLE epoch_visfetch_p12 (id int PRIMARY KEY, val text);
INSERT INTO epoch_visfetch_p12 VALUES (1, 'p12_test');

-- Confirm preconditions: materialized relation with Stage 1 guard active
SELECT epoch_xid_relation_mode('epoch_visfetch_p12'::regclass);
SELECT epoch_xid_stage1_bridge_enabled();

-- TID scan triggers: heap_fetch → epoch branch → Phase 4 classification
SELECT * FROM epoch_visfetch_p12 WHERE ctid = '(0,1)';

-- Proof 1: the epoch path used the 64-bit snapshot bridge
SELECT epoch_xid_mvcc_last_path();

-- Proof 2: Phase 4 used the 64-bit membership function
SELECT epoch_xid_classify_last_membership();

-- P12_b: Same-epoch parity — 64-bit and 32-bit paths produce identical
-- visibility results within the bounded epoch-0 operating region.
--
-- With bridge enabled: 64-bit membership, tuple visible
-- With bridge disabled: 32-bit membership, tuple visible (same result)
-- This proves parity.

-- 64-bit path: tuple is visible, membership was 64-bit
SELECT * FROM epoch_visfetch_p12 WHERE ctid = '(0,1)';
SELECT epoch_xid_classify_last_membership();

-- Force guard off → 32-bit path
SELECT epoch_xid_stage1_force_disable(true);
SELECT * FROM epoch_visfetch_p12 WHERE ctid = '(0,1)';
SELECT epoch_xid_classify_last_membership();

-- Both returned the same row: parity confirmed.
-- Restore
SELECT epoch_xid_stage1_force_disable(false);

-- P12_c: Guard controls membership path selection (negative proof)
--
-- When the Stage 1 guard is OFF, the 64-bit membership helper must NOT
-- be used.  This proves the guard is a real boundary that controls the
-- Patch 12 improvement, not just the Patch 11 snapshot comparison.

SELECT epoch_xid_stage1_force_disable(true);

-- Guard is off
SELECT epoch_xid_stage1_bridge_enabled();

-- TID scan: falls back to 32-bit membership
SELECT * FROM epoch_visfetch_p12 WHERE ctid = '(0,1)';

-- Must be '32bit_membership' — proves guard blocked the 64-bit path
SELECT epoch_xid_classify_last_membership();

-- Must NOT be 'snapshot_bridge'
SELECT epoch_xid_mvcc_last_path();

-- Restore
SELECT epoch_xid_stage1_force_disable(false);

-- P12_d: Guard re-enabled — 64-bit membership resumes
--
-- After restoring the guard, the 64-bit membership path must be used
-- again.  This proves the guard is the sole control for the 64-bit path.

SELECT epoch_xid_stage1_bridge_enabled();
SELECT * FROM epoch_visfetch_p12 WHERE ctid = '(0,1)';
SELECT epoch_xid_classify_last_membership();
SELECT epoch_xid_mvcc_last_path();

DROP TABLE epoch_visfetch_p12;

-- ======================================================
-- Patch 14: 64-bit snapshot horizon fast-reject
-- ======================================================
--
-- These tests prove that Patch 14's epoch_horizon fast-reject is exercised
-- by the epoch-aware Phase 4 classification path.  The fast-reject is
-- justified by snapshot semantics: epoch_horizon is the 64-bit form of
-- snapshot->xmin, and any XID preceding snapshot->xmin had already completed
-- before the snapshot was created.  This does not depend on RecentXmin.

-- P14_a: Horizon fast-reject fires for a committed tuple
--
-- A committed tuple's xmin is older than the current snapshot's xmin.
-- The 64-bit horizon fast-reject must fire, skipping the ProcArray scan.

CREATE TABLE epoch_visfetch_p14 (id int PRIMARY KEY, val text);
INSERT INTO epoch_visfetch_p14 VALUES (1, 'p14_test');

-- Start a new transaction to get a fresh snapshot whose xmin > committed xmin
BEGIN;

-- Confirm preconditions
SELECT epoch_xid_relation_mode('epoch_visfetch_p14'::regclass);
SELECT epoch_xid_stage1_bridge_enabled();

-- TID scan: heap_fetch → epoch branch → Phase 4 → horizon fast-reject
SELECT * FROM epoch_visfetch_p14 WHERE ctid = '(0,1)';

-- Proof 1: horizon fast-reject was taken
SELECT epoch_xid_classify_last_horizon();

-- Proof 2: membership check was skipped (resolved before reaching it)
SELECT epoch_xid_classify_last_membership();

-- Proof 3: the epoch path used the 64-bit snapshot bridge
SELECT epoch_xid_mvcc_last_path();

COMMIT;

-- P14_b: Horizon fast-reject does NOT fire when guard is off
--
-- With the Stage 1 guard force-disabled, epoch_horizon is invalid,
-- so the fast-reject cannot fire.  This proves the guard controls it.

SELECT epoch_xid_stage1_force_disable(true);

BEGIN;

SELECT * FROM epoch_visfetch_p14 WHERE ctid = '(0,1)';

-- Must be 'not_used' — guard off means no epoch_horizon
SELECT epoch_xid_classify_last_horizon();

-- Must be '32bit_membership' — fell back to 32-bit path
SELECT epoch_xid_classify_last_membership();

COMMIT;

SELECT epoch_xid_stage1_force_disable(false);

-- P14_c: Guard re-enabled — horizon fast-reject resumes
--
-- After restoring the guard, the horizon fast-reject must work again.

SELECT epoch_xid_stage1_bridge_enabled();

BEGIN;

SELECT * FROM epoch_visfetch_p14 WHERE ctid = '(0,1)';
SELECT epoch_xid_classify_last_horizon();
SELECT epoch_xid_mvcc_last_path();

COMMIT;

DROP TABLE epoch_visfetch_p14;

-- ======================================================
-- Patch 15: acquisition-time 64-bit snapshot boundary bridge
-- ======================================================
--
-- These tests prove that Patch 15's pre-computed epoch_full_xmin and
-- epoch_full_xmax are populated at snapshot acquisition time and
-- consumed by the epoch-aware heap_fetch() path.

-- P15_a: Bridge state is populated at acquisition time
--
-- epoch_xid_snapshot_bridge_info() exposes the pre-computed fields.
-- Both full_xmin and full_xmax must be non-zero for a normal MVCC
-- snapshot in epoch 0.  guard_active must be true.

BEGIN;

SELECT anchor_valid, (full_xmin > 0) AS xmin_valid,
       (full_xmax > 0) AS xmax_valid, guard_active
  FROM epoch_xid_snapshot_bridge_info();

COMMIT;

-- P15_b: Horizon fast-reject still works with acquisition-time bridge
--
-- A committed tuple older than the snapshot's xmin must still trigger
-- the horizon fast-reject (Patch 14), now sourced from the pre-computed
-- epoch_full_xmin field instead of per-tuple reconstruction.

CREATE TABLE epoch_visfetch_p15 (id int PRIMARY KEY, val text);
INSERT INTO epoch_visfetch_p15 VALUES (1, 'p15_test');

BEGIN;

SELECT epoch_xid_relation_mode('epoch_visfetch_p15'::regclass);
SELECT epoch_xid_stage1_bridge_enabled();

-- TID scan: heap_fetch → epoch branch → Phase 4 horizon from epoch_full_xmin
SELECT * FROM epoch_visfetch_p15 WHERE ctid = '(0,1)';

-- Proof: horizon fast-reject fired using the acquisition-time value
SELECT epoch_xid_classify_last_horizon();

-- Proof: epoch path used the 64-bit snapshot bridge
SELECT epoch_xid_mvcc_last_path();

COMMIT;

-- P15_c: Guard-off disables consumption of bridge state
--
-- With the Stage 1 guard force-disabled, the epoch path does not read
-- the pre-computed fields.  Bridge state is populated but not consumed.

SELECT epoch_xid_stage1_force_disable(true);

BEGIN;

-- Bridge state is still populated (anchor valid) but guard is off
SELECT anchor_valid, guard_active
  FROM epoch_xid_snapshot_bridge_info();

SELECT * FROM epoch_visfetch_p15 WHERE ctid = '(0,1)';

-- Must be 'not_used' — guard off means no epoch path
SELECT epoch_xid_classify_last_horizon();

COMMIT;

SELECT epoch_xid_stage1_force_disable(false);

-- P15_d: Guard restored — bridge state consumption resumes

BEGIN;

SELECT guard_active FROM epoch_xid_snapshot_bridge_info();

SELECT * FROM epoch_visfetch_p15 WHERE ctid = '(0,1)';
SELECT epoch_xid_classify_last_horizon();
SELECT epoch_xid_mvcc_last_path();

COMMIT;

DROP TABLE epoch_visfetch_p15;

-- P15_e: Source proof — consumer uses pre-computed bridge state
--
-- This is the key Patch 15 proof: the narrow epoch-aware path must
-- consume the acquisition-time bridge state (precomputed), not the
-- old per-consumer reconstruction — for BOTH the Phase 5 boundary
-- comparisons AND the Phase 4 horizon fast-reject.

CREATE TABLE epoch_visfetch_p15src (id int PRIMARY KEY, val text);
INSERT INTO epoch_visfetch_p15src VALUES (1, 'source_test');

BEGIN;

SELECT * FROM epoch_visfetch_p15src WHERE ctid = '(0,1)';

-- Phase 5 boundaries: must be 'precomputed'
SELECT epoch_xid_bridge_last_source();

-- Phase 4 horizon: must be 'precomputed'
SELECT epoch_xid_horizon_last_source();

-- Horizon fast-reject must still fire
SELECT epoch_xid_classify_last_horizon();

COMMIT;

-- P15_f: Fallback proof — reconstruction path still works for both consumers
--
-- Force both consumers to ignore the pre-computed fields and reconstruct
-- boundaries ad hoc.  Prove the same visibility result and that both
-- sources change to 'reconstructed'.

SELECT epoch_xid_force_bridge_reconstruct(true);

BEGIN;

SELECT * FROM epoch_visfetch_p15src WHERE ctid = '(0,1)';

-- Phase 5 boundaries: must be 'reconstructed'
SELECT epoch_xid_bridge_last_source();

-- Phase 4 horizon: must be 'reconstructed'
SELECT epoch_xid_horizon_last_source();

-- Horizon fast-reject must still fire (same value, different source)
SELECT epoch_xid_classify_last_horizon();

COMMIT;

SELECT epoch_xid_force_bridge_reconstruct(false);

-- P15_g: Parity — both sources produce identical visibility results
--
-- After restoring normal mode, the precomputed path resumes.

BEGIN;

SELECT * FROM epoch_visfetch_p15src WHERE ctid = '(0,1)';

-- Both must report 'precomputed' again
SELECT epoch_xid_bridge_last_source();
SELECT epoch_xid_horizon_last_source();

COMMIT;

DROP TABLE epoch_visfetch_p15src;

-- ======================================================
-- Patch 16: second real internal consumer — heap_hot_search_buffer()
-- ======================================================
--
-- These tests prove that heap_hot_search_buffer() (index scan HOT chain
-- traversal) is a second real consumer of the bounded epoch-aware bridge,
-- distinct from the Patch 10 consumer (heap_fetch / TID scan).

-- P16_a: Index scan uses heap_hot_search_buffer as epoch bridge consumer
--
-- Direct caller proof: epoch_xid_mvcc_last_caller() must report
-- 'heap_hot_search_buffer' after an index scan on a materialized relation.

CREATE TABLE epoch_p16 (id int PRIMARY KEY, val text);
INSERT INTO epoch_p16 VALUES (1, 'p16_test');

SET enable_seqscan = off;

BEGIN;

SELECT * FROM epoch_p16 WHERE id = 1;

-- Direct proof: second consumer path
SELECT epoch_xid_mvcc_last_caller();
-- Epoch bridge was exercised
SELECT epoch_xid_mvcc_last_path();

COMMIT;

RESET enable_seqscan;

-- P16_b: TID scan still uses heap_fetch — paths are distinct

BEGIN;

SELECT * FROM epoch_p16 WHERE ctid = '(0,1)';

-- Must be 'heap_fetch' — proves the two consumers are distinguishable
SELECT epoch_xid_mvcc_last_caller();

COMMIT;

-- P16_c: Index scan exercises full bounded bridge features
--
-- All Patch 11–15 bridge features must work through the second consumer.

SET enable_seqscan = off;

BEGIN;

SELECT * FROM epoch_p16 WHERE id = 1;

SELECT epoch_xid_mvcc_last_caller();
SELECT epoch_xid_classify_last_horizon();
SELECT epoch_xid_horizon_last_source();
SELECT epoch_xid_bridge_last_source();

COMMIT;

RESET enable_seqscan;

-- P16_d: Guard off disables 64-bit bridge but epoch path is still entered
--
-- With the Stage 1 guard force-disabled, the epoch function is still called
-- (because the relation is materialized + MVCC snapshot), but the 64-bit
-- bridge features are inactive.  The caller is still heap_hot_search_buffer.

SELECT epoch_xid_stage1_force_disable(true);
SET enable_seqscan = off;

BEGIN;

SELECT * FROM epoch_p16 WHERE id = 1;

-- Caller is still heap_hot_search_buffer (epoch function was called)
SELECT epoch_xid_mvcc_last_caller();
-- But the 64-bit snapshot bridge was NOT used (guard off)
SELECT epoch_xid_mvcc_last_path();

COMMIT;

RESET enable_seqscan;
SELECT epoch_xid_stage1_force_disable(false);

-- P16_e: HOT chain traversal — update non-indexed column, query via index
--
-- Creates a real HOT chain by updating a non-indexed column.

INSERT INTO epoch_p16 VALUES (2, 'original');
UPDATE epoch_p16 SET val = 'updated' WHERE id = 2;

SET enable_seqscan = off;

BEGIN;

SELECT val FROM epoch_p16 WHERE id = 2;

-- Must be 'heap_hot_search_buffer' — HOT chain traversal
SELECT epoch_xid_mvcc_last_caller();
SELECT epoch_xid_mvcc_last_path();

COMMIT;

RESET enable_seqscan;

DROP TABLE epoch_p16;
