# Test: TID scan via heap_fetch with snapshot boundary
#
# Proves that heap_fetch's epoch branch respects snapshot boundaries:
# a tuple committed after the scanning transaction's snapshot is invisible.

setup
{
    CREATE TABLE epoch_fetch_snap (id int PRIMARY KEY, val text);
}

teardown
{
    DROP TABLE IF EXISTS epoch_fetch_snap;
}

session s1
step begin1		{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1; }
step tid_scan	{ SELECT * FROM epoch_fetch_snap WHERE ctid = '(0,1)'; }
step release1	{ COMMIT; }

session s2
step insert2	{ INSERT INTO epoch_fetch_snap VALUES (1, 'after_snapshot'); }

# s1 establishes snapshot, s2 inserts and commits, s1 does TID scan
# → s1 must NOT see the row (xmin committed after s1's snapshot)
permutation begin1 insert2 tid_scan release1
