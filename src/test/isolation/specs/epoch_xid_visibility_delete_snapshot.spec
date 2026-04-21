# committed-after-my-snapshot delete → tuple still visible

setup
{
    CREATE TABLE epoch_vis_del_snapshot (id int);
    INSERT INTO epoch_vis_del_snapshot VALUES (1);
}

teardown
{
    DROP TABLE IF EXISTS epoch_vis_del_snapshot;
}

session s1
step s1_delete	{ DELETE FROM epoch_vis_del_snapshot WHERE id = 1; }

session s2
step s2_begin	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1; }
step s2_inspect	{ SELECT visibility_verdict, verdict_reason FROM epoch_xid_tuple_current_visibility_info('epoch_vis_del_snapshot'::regclass, '(0,1)'::tid); }
step s2_end		{ COMMIT; }

permutation s2_begin s1_delete s2_inspect s2_end
