# Test: epoch_xid_tuple_visibility_info read-side MultiXact behavior
#
# Validates that when a tuple has HEAP_XMAX_IS_MULTI (from concurrent
# FOR KEY SHARE lockers), the read-side inspection function reports
# xmax_interp = 'multixact_unsupported' and full_xmax = 0 without ERROR.
#
# This is the read-side contract: unlike the write path which ERRORs
# on MultiXact, the inspection function gracefully reports the state.

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
step inspect	{ SELECT full_xmax, xmax_interp FROM epoch_xid_tuple_visibility_info('epoch_mxid_read_test'::regclass, '(0,1)'::tid); }

# s1 and s2 both hold FOR KEY SHARE → MultiXact xmax on the tuple.
# s3 inspects while both locks are held.
permutation lock1 lock2 inspect release1 release2
