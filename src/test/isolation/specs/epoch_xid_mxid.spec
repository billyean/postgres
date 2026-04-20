# Test: epoch fork MultiXact guard in heap_update
#
# Validates that concurrent tuple locking (which produces a MultiXact
# xmax outcome) causes the epoch fork prototype to ERROR explicitly,
# rather than silently producing incomplete epoch state.
#
# This test requires the relation to be in materialized epoch mode
# (epoch fork exists) before the concurrent locking scenario.

setup
{
    CREATE TABLE epoch_mxid_test (id int PRIMARY KEY, val text);
    -- INSERT materializes the epoch fork
    INSERT INTO epoch_mxid_test VALUES (1, 'test');
}

teardown
{
    DROP TABLE IF EXISTS epoch_mxid_test;
}

session s1
step lock	{ BEGIN; SELECT * FROM epoch_mxid_test WHERE id = 1 FOR KEY SHARE; }
step release	{ ROLLBACK; }

session s2
step update	{ UPDATE epoch_mxid_test SET val = 'updated' WHERE id = 1; }

# s1 holds FOR KEY SHARE lock, then s2 attempts UPDATE.
# compute_new_xmax_infomask will produce a MultiXact combining
# s1's locker with s2's updater.  The epoch fork guard must
# fire ERROR before the critical section.
permutation lock update release
