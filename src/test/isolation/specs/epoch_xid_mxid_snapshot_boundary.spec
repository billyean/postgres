# Test: Updater commits after my snapshot (snapshot boundary)
#
# Validates that when a MultiXact updater commits AFTER the inspecting
# session's snapshot, Phase 5 reports visible / xmax_committed_not_in_snapshot.

setup
{
    CREATE TABLE epoch_mxid_snap (id int PRIMARY KEY, val text);
    INSERT INTO epoch_mxid_snap VALUES (1, 'test');
}

teardown
{
    DROP TABLE IF EXISTS epoch_mxid_snap;
}

session s1
step lock1		{ BEGIN; SELECT * FROM epoch_mxid_snap WHERE id = 1 FOR KEY SHARE; }
step release1	{ COMMIT; }

session s2
step delete2	{ DELETE FROM epoch_mxid_snap WHERE id = 1; }

session s3
step begin3		{ BEGIN; SELECT 1; }
step inspect_p5	{ SELECT visibility_verdict, verdict_reason FROM epoch_xid_tuple_current_visibility_info('epoch_mxid_snap'::regclass, '(0,1)'::tid); }
step release3	{ COMMIT; }

# s3 establishes snapshot, then s1 releases lock allowing s2 to DELETE and
# commit.  s3 inspects: updater committed after s3's snapshot.
permutation lock1 begin3 delete2 release1 inspect_p5 release3
