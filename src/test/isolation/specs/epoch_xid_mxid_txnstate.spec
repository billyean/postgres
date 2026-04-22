# Test: Phase 4 MultiXact classification — locker-only

setup
{
    CREATE TABLE epoch_mxid_txnstate (id int PRIMARY KEY, val text);
    INSERT INTO epoch_mxid_txnstate VALUES (1, 'test');
}

teardown
{
    DROP TABLE IF EXISTS epoch_mxid_txnstate;
}

session s1
step lock1		{ BEGIN; SELECT * FROM epoch_mxid_txnstate WHERE id = 1 FOR KEY SHARE; }
step release1	{ ROLLBACK; }

session s2
step lock2		{ BEGIN; SELECT * FROM epoch_mxid_txnstate WHERE id = 1 FOR KEY SHARE; }
step release2	{ ROLLBACK; }

session s3
step inspect	{ SELECT xmax_status, tuple_state FROM epoch_xid_tuple_txn_state_info('epoch_mxid_txnstate'::regclass, '(0,1)'::tid); }

permutation lock1 lock2 inspect release1 release2
