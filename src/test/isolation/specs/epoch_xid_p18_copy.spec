# Test: Copied snapshot preserves pre-promoted membership arrays
#
# Patch 18 proof: a snapshot that goes through CopySnapshot() still has
# valid pre-promoted full_xip[]/full_subxip[] arrays and the epoch-aware
# consumer path reports 'precomputed'.
#
# Strategy: s1 holds an open transaction.  s2 declares a cursor (which
# copies the snapshot via PushActiveSnapshot → CopySnapshot), then
# fetches through the cursor.  The fetch exercises the epoch path using
# the COPIED snapshot.  bridge_last_source must report 'precomputed'.
#
# s1's open transaction ensures s2's snapshot has a non-empty xip[] array
# (s1's XID is in xip[]), so the pre-promoted full_xip[] is exercised
# on the copied snapshot.

setup
{
    CREATE TABLE epoch_p18_copy (id int PRIMARY KEY, val text);
    INSERT INTO epoch_p18_copy VALUES (1, 'copy_test');
}

teardown
{
    DROP TABLE IF EXISTS epoch_p18_copy;
}

session s1
step begin_hold     { BEGIN; SELECT txid_current() IS NOT NULL AS has_xid; }
step release_hold   { COMMIT; }

session s2
step declare_cur    { BEGIN; DECLARE epoch_cur CURSOR FOR SELECT * FROM epoch_p18_copy WHERE ctid = '(0,1)'; }
step fetch_cur      { FETCH NEXT FROM epoch_cur; }
step check_source   { SELECT epoch_xid_bridge_last_source(); }
step check_path     { SELECT epoch_xid_mvcc_last_path(); }
step close_cur      { CLOSE epoch_cur; COMMIT; }

# s1 holds open txn (puts its XID in s2's xip[]), s2 uses cursor (copied snapshot)
permutation begin_hold declare_cur fetch_cur check_source check_path close_cur release_hold
