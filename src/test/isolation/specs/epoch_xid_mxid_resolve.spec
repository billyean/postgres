# Test: Resolved updater full_xmax reconstruction (mandatory I4)
#
# Proves the core Patch 9 correctness claim: after a MultiXact-producing
# heap_update, when the MultiXact is resolved, EpochReconstructXmax
# produces a correct non-zero full 64-bit XID for the resolved updater.
#
# Uses epoch_xid_resolve_multixact (test helper) to force resolution,
# because PostgreSQL does not resolve committed-updater MultiXacts via
# simple hint-bit setting.

setup
{
    CREATE TABLE epoch_mxid_resolve (id int PRIMARY KEY, val text);
    INSERT INTO epoch_mxid_resolve VALUES (1, 'original');
}

teardown
{
    DROP TABLE IF EXISTS epoch_mxid_resolve;
}

session s1
step lock1		{ BEGIN; SELECT * FROM epoch_mxid_resolve WHERE id = 1 FOR KEY SHARE; }
step release1	{ COMMIT; }

session s2
step update2	{ UPDATE epoch_mxid_resolve SET val = 'updated' WHERE id = 1; }

session s3
step pre_check	{
    SELECT
        xmax_interp,
        full_xmax = 0 AS full_xmax_is_zero
    FROM epoch_xid_tuple_visibility_info('epoch_mxid_resolve'::regclass, '(0,1)'::tid);
}
step resolve	{ SELECT epoch_xid_resolve_multixact('epoch_mxid_resolve'::regclass, '(0,1)'::tid); }
step post_check	{
    SELECT
        xmax_interp,
        full_xmax <> 0 AS full_xmax_is_nonzero
    FROM epoch_xid_tuple_visibility_info('epoch_mxid_resolve'::regclass, '(0,1)'::tid);
}
step post_p4	{ SELECT xmax_status, tuple_state FROM epoch_xid_tuple_txn_state_info('epoch_mxid_resolve'::regclass, '(0,1)'::tid); }

# pre_check: MultiXact active — xmax_interp='multixact', full_xmax=0
# resolve: force MultiXact resolution
# post_check: resolved — xmax_interp='materialized', full_xmax non-zero
# post_p4: xmax_status='committed' via regular hint-bit path
permutation lock1 update2 release1 pre_check resolve post_check post_p4
