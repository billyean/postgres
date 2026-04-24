# Test: In-progress tuple via index scan proves second consumer path
#
# Patch 16 proof: heap_hot_search_buffer() correctly handles an in-progress
# tuple through the epoch-aware bridge, and the caller instrumentation
# directly identifies it as the second consumer path.
#
# Session A inserts an uncommitted row (in-progress xmin).
# Session B does an index scan — goes through heap_hot_search_buffer().
# The tuple must be invisible, and the caller must be heap_hot_search_buffer.

setup
{
    CREATE TABLE epoch_p16_inprog (id int PRIMARY KEY, val text);
    SET enable_seqscan = off;
}

teardown
{
    RESET enable_seqscan;
    DROP TABLE IF EXISTS epoch_p16_inprog;
}

session s1
step insert1	{ BEGIN; INSERT INTO epoch_p16_inprog VALUES (1, 'uncommitted'); }
step release1	{ ROLLBACK; }

session s2
setup		{ SET enable_seqscan = off; }
step idx_scan	{ SELECT * FROM epoch_p16_inprog WHERE id = 1; }
step check_caller	{ SELECT epoch_xid_mvcc_last_caller(); }
step check_membership	{ SELECT epoch_xid_classify_last_membership(); }
step check_path	{ SELECT epoch_xid_mvcc_last_path(); }

# s1 inserts uncommitted, s2 probes via index scan
permutation insert1 idx_scan check_caller check_membership check_path release1
