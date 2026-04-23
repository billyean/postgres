# Test: TID scan via heap_fetch with in-progress deleter
#
# Proves that heap_fetch's epoch branch correctly determines
# in-progress xmax as visible (tuple not yet deleted from snapshot POV).

setup
{
    CREATE TABLE epoch_fetch_inprog_del (id int PRIMARY KEY, val text);
    INSERT INTO epoch_fetch_inprog_del VALUES (1, 'will_delete');
}

teardown
{
    DROP TABLE IF EXISTS epoch_fetch_inprog_del;
}

session s1
step delete1	{ BEGIN; DELETE FROM epoch_fetch_inprog_del WHERE id = 1; }
step release1	{ ROLLBACK; }

session s2
step tid_scan	{ SELECT * FROM epoch_fetch_inprog_del WHERE ctid = '(0,1)'; }

# s1 deletes (uncommitted), s2 does TID scan → must see the row
permutation delete1 tid_scan release1
