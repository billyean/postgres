# Test: Horizon fast-reject fires for committed MultiXact updater
#
# Patch 14 proof: the 64-bit snapshot horizon fast-reject in
# EpochClassifyMultiXactXmax correctly skips the ProcArray scan for a
# committed updater whose XID is older than the snapshot horizon.
#
# Setup: committed row with id=1.
# s1 takes FOR KEY SHARE → locks the tuple.
# s2 does DELETE → blocks on s1, will create MultiXact when it proceeds.
# s1 commits → s2's DELETE proceeds and commits (MultiXact with s2 as updater).
# s3 starts a new transaction → snapshot xmin > s2's XID.
# s3 does TID scan on (0,1) → epoch path → EpochClassifyXidStatus for xmax
#   → HEAP_XMAX_IS_MULTI → EpochClassifyMultiXactXmax → horizon fast-reject
#   fires for the committed updater.
#
# Expected:
#   - epoch_xid_classify_last_horizon() = 'horizon_fastpath'
#     (updater's full XID < epoch_horizon)
#   - epoch_xid_classify_last_membership() = 'not_called'
#     (ProcArray scan skipped because horizon resolved it)
#   - tuple is invisible (deleted by committed xmax before snapshot)
#   - epoch_xid_stage1_bridge_enabled() = true (guard unchanged)

setup
{
    CREATE TABLE epoch_p14_mxid (id int PRIMARY KEY, val text);
    INSERT INTO epoch_p14_mxid VALUES (1, 'mxid_test');
}

teardown
{
    DROP TABLE IF EXISTS epoch_p14_mxid;
}

session s1
step lock1		{ BEGIN; SELECT * FROM epoch_p14_mxid WHERE id = 1 FOR KEY SHARE; }
step release1	{ COMMIT; }

session s2
step delete2	{ DELETE FROM epoch_p14_mxid WHERE id = 1; }

session s3
# TID scan after s2's DELETE has committed — xmax is still MultiXact
# because the epoch path does not set hint bits
step tid_scan_mxid		{ SELECT * FROM epoch_p14_mxid WHERE ctid = '(0,1)'; }
step check_horizon		{ SELECT epoch_xid_classify_last_horizon(); }
step check_membership	{ SELECT epoch_xid_classify_last_membership(); }
step check_guard		{ SELECT epoch_xid_stage1_bridge_enabled(); }
step check_path			{ SELECT epoch_xid_mvcc_last_path(); }

# s1 locks, s2 deletes (blocks on s1), s1 releases → s2 commits.
# s3 probes: MultiXact updater is committed and older than snapshot horizon.
permutation lock1 delete2 release1 tid_scan_mxid check_horizon check_membership check_guard check_path
