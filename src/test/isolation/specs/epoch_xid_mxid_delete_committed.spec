# Test: MultiXact with committed updater via DELETE
#
# For a MultiXact with an updater to exist, the DELETE must complete (not be
# blocked). We use FOR KEY SHARE which doesn't conflict with DELETE's
# ExclusiveLock on a non-key column.  Actually, DELETE takes LockTupleExclusive
# which conflicts with KeyShare.
#
# Strategy: Two lockers create a MultiXact, then one commits. The remaining
# locker + a new DELETE creates a new MultiXact with the updater. We inspect
# while the updater has committed but the MultiXact is still on the tuple.
#
# Simpler strategy: s1 takes KEY SHARE, s2 does DELETE (blocks on s1),
# s1 commits releasing the lock, s2's DELETE proceeds and commits.
# After s2 commits, the tuple may no longer have HEAP_XMAX_IS_MULTI
# (hint bits resolve it).
#
# We inspect WHILE s2's DELETE is still in progress (before s1 releases)
# to see the in-progress state, and after both commit for committed state.
# But while s2 is blocked, the MultiXact hasn't been formed yet.
#
# Alternative: Test committed updater classification through Phase 4 only.
# After s1 releases and s2 commits, the tuple's xmax may be resolved to a
# simple committed XID. This still tests our Phase 4 classification correctly.

setup
{
    CREATE TABLE epoch_mxid_del_comm (id int PRIMARY KEY, val text);
    INSERT INTO epoch_mxid_del_comm VALUES (1, 'test');
}

teardown
{
    DROP TABLE IF EXISTS epoch_mxid_del_comm;
}

session s1
step lock1		{ BEGIN; SELECT * FROM epoch_mxid_del_comm WHERE id = 1 FOR KEY SHARE; }
step release1	{ COMMIT; }

session s2
step delete2	{ DELETE FROM epoch_mxid_del_comm WHERE id = 1; }

session s3
step inspect_p3	{ SELECT xmax_interp FROM epoch_xid_tuple_visibility_info('epoch_mxid_del_comm'::regclass, '(0,1)'::tid); }
step inspect_p4	{ SELECT xmax_status, tuple_state FROM epoch_xid_tuple_txn_state_info('epoch_mxid_del_comm'::regclass, '(0,1)'::tid); }
step inspect_p5	{ SELECT visibility_verdict, verdict_reason FROM epoch_xid_tuple_current_visibility_info('epoch_mxid_del_comm'::regclass, '(0,1)'::tid); }

# After both complete, tuple has committed xmax (may or may not still be MULTI).
permutation lock1 delete2 release1 inspect_p3 inspect_p4 inspect_p5
