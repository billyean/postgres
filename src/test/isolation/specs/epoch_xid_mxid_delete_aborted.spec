# Test: MultiXact with aborted updater
#
# Validates Phase 4/5 when a tuple's MultiXact xmax contains an updater
# whose transaction aborted.

setup
{
    CREATE TABLE epoch_mxid_del_abort (id int PRIMARY KEY, val text);
    INSERT INTO epoch_mxid_del_abort VALUES (1, 'test');
}

teardown
{
    DROP TABLE IF EXISTS epoch_mxid_del_abort;
}

session s1
step lock1		{ BEGIN; SELECT * FROM epoch_mxid_del_abort WHERE id = 1 FOR KEY SHARE; }
step release1	{ COMMIT; }

session s2
step delete2	{ BEGIN; DELETE FROM epoch_mxid_del_abort WHERE id = 1; }
step abort2		{ ROLLBACK; }

session s3
step inspect_p4	{ SELECT xmax_status, tuple_state FROM epoch_xid_tuple_txn_state_info('epoch_mxid_del_abort'::regclass, '(0,1)'::tid); }
step inspect_p5	{ SELECT visibility_verdict, verdict_reason FROM epoch_xid_tuple_current_visibility_info('epoch_mxid_del_abort'::regclass, '(0,1)'::tid); }

# s1 holds lock, s2 deletes (blocked), s1 releases, s2 aborts, s3 inspects.
permutation lock1 delete2 release1 abort2 inspect_p4 inspect_p5
