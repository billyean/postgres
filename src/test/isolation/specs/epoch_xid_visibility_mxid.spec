# Phase 5 MultiXact unsupported via current-visibility function

setup
{
    CREATE TABLE epoch_vis_mxid_test (id int PRIMARY KEY, val text);
    INSERT INTO epoch_vis_mxid_test VALUES (1, 'test');
}

teardown
{
    DROP TABLE IF EXISTS epoch_vis_mxid_test;
}

session s1
step lock1		{ BEGIN; SELECT * FROM epoch_vis_mxid_test WHERE id = 1 FOR KEY SHARE; }
step release1	{ ROLLBACK; }

session s2
step lock2		{ BEGIN; SELECT * FROM epoch_vis_mxid_test WHERE id = 1 FOR KEY SHARE; }
step release2	{ ROLLBACK; }

session s3
step inspect	{ SELECT xmin_status, xmax_status, visibility_verdict, verdict_reason FROM epoch_xid_tuple_current_visibility_info('epoch_vis_mxid_test'::regclass, '(0,1)'::tid); }

permutation lock1 lock2 inspect release1 release2
