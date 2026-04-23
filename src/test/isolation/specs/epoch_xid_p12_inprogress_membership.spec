# Test: TID scan proves 64-bit membership check on a real in-progress xmin
#
# Patch 12 core proof: the 64-bit FullTransactionIdIsInProgress helper
# correctly identifies an in-progress inserter's XID through the epoch-aware
# heap_fetch path, and the Stage 1 guard controls which membership function
# is used.
#
# Session A holds an uncommitted INSERT.
# Session B does TID scan → epoch path → Phase 4 membership check.
#
# With the Stage 1 guard active:
#   - FullTransactionIdIsInProgress detects session A as in-progress
#   - epoch_xid_classify_last_membership() reports '64bit_membership'
#   - tuple is invisible (correct)
#
# With the guard force-disabled:
#   - TransactionIdIsInProgress (32-bit) detects session A as in-progress
#   - epoch_xid_classify_last_membership() reports '32bit_membership'
#   - tuple is still invisible (parity: same result within epoch 0)

setup
{
    CREATE TABLE epoch_p12_inprog (id int PRIMARY KEY, val text);
}

teardown
{
    DROP TABLE IF EXISTS epoch_p12_inprog;
}

session s1
step insert1	{ BEGIN; INSERT INTO epoch_p12_inprog VALUES (1, 'uncommitted'); }
step release1	{ ROLLBACK; }

session s2
# TID scan with Stage 1 guard active (64-bit membership)
step tid_scan_64	{ SELECT * FROM epoch_p12_inprog WHERE ctid = '(0,1)'; }
step check_membership_64	{ SELECT epoch_xid_classify_last_membership(); }
step check_path_64	{ SELECT epoch_xid_mvcc_last_path(); }

# Force guard off, TID scan with 32-bit membership (parity check)
step disable_guard	{ SELECT epoch_xid_stage1_force_disable(true); }
step tid_scan_32	{ SELECT * FROM epoch_p12_inprog WHERE ctid = '(0,1)'; }
step check_membership_32	{ SELECT epoch_xid_classify_last_membership(); }

# Restore guard
step restore_guard	{ SELECT epoch_xid_stage1_force_disable(false); }

# s1 inserts (uncommitted), s2 proves both membership paths detect it
permutation insert1 tid_scan_64 check_membership_64 check_path_64 disable_guard tid_scan_32 check_membership_32 restore_guard release1
