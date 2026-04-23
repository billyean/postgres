# Test: TID scan proves 64-bit horizon fast-reject and membership check paths
#
# Patch 14 core proof: the 64-bit snapshot horizon fast-reject correctly
# skips the ProcArray scan for committed XIDs older than the snapshot horizon,
# and the 64-bit membership path is still used for in-progress XIDs.
#
# Session A holds an uncommitted INSERT.
# Session B does TID scan on both committed and in-progress rows.
#
# For committed row (xmin < snapshot->xmin):
#   - epoch_xid_classify_last_horizon() reports 'horizon_fastpath'
#   - ProcArray scan is skipped
#
# For in-progress row (xmin >= snapshot->xmin):
#   - epoch_xid_classify_last_horizon() reports 'not_used'
#   - epoch_xid_classify_last_membership() reports '64bit_membership'

setup
{
    CREATE TABLE epoch_p14_horizon (id int PRIMARY KEY, val text);
    INSERT INTO epoch_p14_horizon VALUES (1, 'committed_row');
}

teardown
{
    DROP TABLE IF EXISTS epoch_p14_horizon;
}

session s1
step insert_inprogress	{ BEGIN; INSERT INTO epoch_p14_horizon VALUES (2, 'inprogress_row'); }
step release1	{ ROLLBACK; }

session s2
# TID scan on committed row: horizon fast-reject must fire
step tid_committed	{ SELECT * FROM epoch_p14_horizon WHERE ctid = '(0,1)'; }
step check_horizon_committed	{ SELECT epoch_xid_classify_last_horizon(); }
step check_membership_committed	{ SELECT epoch_xid_classify_last_membership(); }

# TID scan on in-progress row: horizon fast-reject must NOT fire
step tid_inprogress	{ SELECT * FROM epoch_p14_horizon WHERE ctid = '(0,2)'; }
step check_horizon_inprogress	{ SELECT epoch_xid_classify_last_horizon(); }
step check_membership_inprogress	{ SELECT epoch_xid_classify_last_membership(); }

# s1 inserts (uncommitted), s2 probes both rows
permutation insert_inprogress tid_committed check_horizon_committed check_membership_committed tid_inprogress check_horizon_inprogress check_membership_inprogress release1
