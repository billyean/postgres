# Test: TID scan via heap_fetch with in-progress inserter
#
# Proves that heap_fetch's epoch branch correctly determines
# in-progress xmin as invisible.

setup
{
    CREATE TABLE epoch_fetch_inprog_ins (id int PRIMARY KEY, val text);
}

teardown
{
    DROP TABLE IF EXISTS epoch_fetch_inprog_ins;
}

session s1
step insert1	{ BEGIN; INSERT INTO epoch_fetch_inprog_ins VALUES (1, 'uncommitted'); }
step release1	{ ROLLBACK; }

session s2
step tid_scan	{ SELECT * FROM epoch_fetch_inprog_ins WHERE ctid = '(0,1)'; }

# s1 inserts (uncommitted), s2 does TID scan → must see 0 rows
permutation insert1 tid_scan release1
