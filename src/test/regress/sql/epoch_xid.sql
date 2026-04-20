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
