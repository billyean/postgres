# Test: epoch fork write-side MultiXact support via heap_update (Patch 9)
#
# Previously, concurrent tuple locking + UPDATE produced an ERROR because
# the epoch fork could not represent a MultiXact xmax.  Patch 9 removes
# that guard.
#
# This test verifies:
#   I1: UPDATE succeeds (no ERROR) when concurrent locker creates MultiXact
#   I1: Phase 3/4/5 correctly classify the resulting MultiXact tuple

setup
{
    CREATE TABLE epoch_mxid_test (id int PRIMARY KEY, val text);
    INSERT INTO epoch_mxid_test VALUES (1, 'test');
}

teardown
{
    DROP TABLE IF EXISTS epoch_mxid_test;
}

session s1
step lock	{ BEGIN; SELECT * FROM epoch_mxid_test WHERE id = 1 FOR KEY SHARE; }
step release	{ COMMIT; }

session s2
step update	{ UPDATE epoch_mxid_test SET val = 'updated' WHERE id = 1; }

session s3
step classify_p3	{ SELECT full_xmax, xmax_interp FROM epoch_xid_tuple_visibility_info('epoch_mxid_test'::regclass, '(0,1)'::tid); }
step classify_p4	{ SELECT xmax_status, tuple_state FROM epoch_xid_tuple_txn_state_info('epoch_mxid_test'::regclass, '(0,1)'::tid); }
step classify_p5	{ SELECT visibility_verdict, verdict_reason FROM epoch_xid_tuple_current_visibility_info('epoch_mxid_test'::regclass, '(0,1)'::tid); }

# s1 holds FOR KEY SHARE, s2 UPDATE is blocked until s1 releases.
# After both finish, s3 classifies the old tuple (Phase 3/4/5).
permutation lock update release classify_p3 classify_p4 classify_p5
