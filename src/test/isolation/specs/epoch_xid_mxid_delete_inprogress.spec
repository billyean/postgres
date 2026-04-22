# Test: MultiXact with in-progress updater
#
# Validates Phase 4/5 when a tuple's MultiXact xmax contains an updater
# whose transaction is still in progress.

setup
{
    CREATE TABLE epoch_mxid_del_inprog (id int PRIMARY KEY, val text);
    INSERT INTO epoch_mxid_del_inprog VALUES (1, 'test');
}

teardown
{
    DROP TABLE IF EXISTS epoch_mxid_del_inprog;
}

session s1
step lock1		{ BEGIN; SELECT * FROM epoch_mxid_del_inprog WHERE id = 1 FOR KEY SHARE; }
step release1	{ ROLLBACK; }

session s2
step delete2	{ BEGIN; DELETE FROM epoch_mxid_del_inprog WHERE id = 1; }
step release2	{ ROLLBACK; }

session s3
step inspect_p4	{ SELECT xmax_status, tuple_state FROM epoch_xid_tuple_txn_state_info('epoch_mxid_del_inprog'::regclass, '(0,1)'::tid); }
step inspect_p5	{ SELECT visibility_verdict, verdict_reason FROM epoch_xid_tuple_current_visibility_info('epoch_mxid_del_inprog'::regclass, '(0,1)'::tid); }

# s1 holds lock, s2 deletes inside BEGIN (stays open), s3 inspects.
permutation lock1 delete2 inspect_p4 inspect_p5 release1 release2
