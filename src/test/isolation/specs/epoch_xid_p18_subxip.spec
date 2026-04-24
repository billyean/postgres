# Test: Committed subtransaction membership via pre-promoted full_subxip[]
#
# Patch 18 proof: the pre-promoted full_subxip[] array is actually exercised.
#
# For Phase 5 to consult subxip[], we need a committed subtransaction's XID
# to be in the snapshot's subxip[].  This happens when:
#   1. s2 takes a snapshot (s1's subtxn is in-progress → in subxip[])
#   2. s1 commits (subtxn xid now committed)
#   3. s2 reads the tuple — xmin is committed, but was in subxip[] at
#      snapshot time → FullXidInMVCCSnapshot returns true → tuple invisible
#      (inserter committed after snapshot)
#
# The bridge source must report 'precomputed' for the Phase 5 check.

setup
{
    CREATE TABLE epoch_p18_subxip (id int PRIMARY KEY, val text);
}

teardown
{
    DROP TABLE IF EXISTS epoch_p18_subxip;
}

session s1
step begin1         { BEGIN; }
step savepoint1     { SAVEPOINT sp1; }
step insert_sub     { INSERT INTO epoch_p18_subxip VALUES (1, 'from_subtxn'); }
step commit1        { COMMIT; }

session s2
step begin2         { BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1; }
step tid_scan       { SELECT * FROM epoch_p18_subxip WHERE ctid = '(0,1)'; }
step check_source   { SELECT epoch_xid_bridge_last_source(); }
step check_path     { SELECT epoch_xid_mvcc_last_path(); }
step commit2        { COMMIT; }

# s1 opens subtxn and inserts, s2 takes snapshot, s1 commits, s2 scans
# s2's snapshot has s1's subtxn XID in subxip[] as in-progress at snapshot time
# After s1 commits, Phase 4 classifies xmin as COMMITTED
# Phase 5 checks FullXidInMVCCSnapshot → finds xmin in subxip[] → invisible
permutation begin1 savepoint1 insert_sub begin2 commit1 tid_scan check_source check_path commit2
