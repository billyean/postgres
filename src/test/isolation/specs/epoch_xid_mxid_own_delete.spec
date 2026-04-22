# Test: Current xid is the updater (own delete via MultiXact)
#
# s1 holds FOR KEY SHARE, s2 does DELETE (blocks on s1).
# s1 releases, s2's DELETE completes.  s2 inspects from within its own txn.
# At this point s2's DELETE changed xmax — inspect before s2 commits.

setup
{
    CREATE TABLE epoch_mxid_own_del (id int PRIMARY KEY, val text);
    INSERT INTO epoch_mxid_own_del VALUES (1, 'test');
}

teardown
{
    DROP TABLE IF EXISTS epoch_mxid_own_del;
}

session s1
step lock1		{ BEGIN; SELECT * FROM epoch_mxid_own_del WHERE id = 1 FOR KEY SHARE; }
step release1	{ COMMIT; }

session s2
step delete2	{ BEGIN; DELETE FROM epoch_mxid_own_del WHERE id = 1; }
step inspect_p4	{ SELECT xmax_status, tuple_state FROM epoch_xid_tuple_txn_state_info('epoch_mxid_own_del'::regclass, '(0,1)'::tid); }
step inspect_p5	{ SELECT visibility_verdict, verdict_reason FROM epoch_xid_tuple_current_visibility_info('epoch_mxid_own_del'::regclass, '(0,1)'::tid); }
step release2	{ ROLLBACK; }

# s1 locks, s2 tries to DELETE (blocks), s1 releases, s2's DELETE completes.
# s2 then inspects — it IS the deleter.
permutation lock1 delete2 release1 inspect_p4 inspect_p5 release2
