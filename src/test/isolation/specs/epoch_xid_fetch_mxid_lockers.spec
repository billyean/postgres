# Test: TID scan via heap_fetch with locker-only MultiXact
#
# Proves that heap_fetch's epoch branch handles locker-only MultiXact
# correctly (tuple remains visible).

setup
{
    CREATE TABLE epoch_fetch_mxid_lock (id int PRIMARY KEY, val text);
    INSERT INTO epoch_fetch_mxid_lock VALUES (1, 'locked');
}

teardown
{
    DROP TABLE IF EXISTS epoch_fetch_mxid_lock;
}

session s1
step lock1		{ BEGIN; SELECT * FROM epoch_fetch_mxid_lock WHERE id = 1 FOR KEY SHARE; }
step release1	{ ROLLBACK; }

session s2
step lock2		{ BEGIN; SELECT * FROM epoch_fetch_mxid_lock WHERE id = 1 FOR KEY SHARE; }
step release2	{ ROLLBACK; }

session s3
step tid_scan	{ SELECT * FROM epoch_fetch_mxid_lock WHERE ctid = '(0,1)'; }

# s1+s2 create MultiXact, s3 TID scan → must see the row
permutation lock1 lock2 tid_scan release1 release2
