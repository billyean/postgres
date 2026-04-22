# Test: epoch_xid read-side MultiXact — Phase 3 + Phase 4 + Phase 5 locker-only
#
# Validates that when a tuple has HEAP_XMAX_IS_MULTI (from concurrent
# FOR KEY SHARE lockers), the read-side inspection functions report
# real results for the supported locker-only subset.
#
# Phase 3: xmax_interp = 'multixact', full_xmax = 0
# Phase 4: xmax_status = 'multixact_lockers_only', tuple_state = 'live_locked'
# Phase 5: visibility_verdict = 'visible', verdict_reason = 'multixact_lockers_only'

setup
{
    CREATE TABLE epoch_mxid_read_test (id int PRIMARY KEY, val text);
    INSERT INTO epoch_mxid_read_test VALUES (1, 'test');
}

teardown
{
    DROP TABLE IF EXISTS epoch_mxid_read_test;
}

session s1
step lock1		{ BEGIN; SELECT * FROM epoch_mxid_read_test WHERE id = 1 FOR KEY SHARE; }
step release1	{ ROLLBACK; }

session s2
step lock2		{ BEGIN; SELECT * FROM epoch_mxid_read_test WHERE id = 1 FOR KEY SHARE; }
step release2	{ ROLLBACK; }

session s3
step inspect_p3	{ SELECT full_xmax, xmax_interp FROM epoch_xid_tuple_visibility_info('epoch_mxid_read_test'::regclass, '(0,1)'::tid); }
step inspect_p4	{ SELECT xmax_status, tuple_state FROM epoch_xid_tuple_txn_state_info('epoch_mxid_read_test'::regclass, '(0,1)'::tid); }
step inspect_p5	{ SELECT visibility_verdict, verdict_reason FROM epoch_xid_tuple_current_visibility_info('epoch_mxid_read_test'::regclass, '(0,1)'::tid); }

# s1 and s2 both hold FOR KEY SHARE → MultiXact xmax on the tuple.
# s3 inspects while both locks are held.
permutation lock1 lock2 inspect_p3 inspect_p4 inspect_p5 release1 release2
