# committed-after-my-snapshot insert → invisible

setup
{
    CREATE TABLE epoch_vis_snapshot_test (id int);
}

teardown
{
    DROP TABLE IF EXISTS epoch_vis_snapshot_test;
}

session s1
step s1_insert	{ INSERT INTO epoch_vis_snapshot_test VALUES (1); }

session s2
step s2_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1; }
step s2_inspect	{ SELECT visibility_verdict, verdict_reason FROM epoch_xid_tuple_current_visibility_info('epoch_vis_snapshot_test'::regclass, '(0,1)'::tid); }
step s2_end		{ COMMIT; }

permutation s2_begin s1_insert s2_inspect s2_end
