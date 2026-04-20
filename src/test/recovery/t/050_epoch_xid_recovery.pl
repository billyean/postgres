
# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# Test XID64 epoch fork WAL/recovery correctness.
#
# Validates that:
# 1. Epoch slot updates from heap_insert survive crash recovery
# 2. Epoch slot updates from heap_delete survive crash recovery
# 3. Epoch fork truncation survives crash recovery
#
# Uses immediate shutdown (pg_ctl stop -m immediate) to simulate crash,
# then restart and verify epoch metadata is intact.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;

$node->append_conf(
	'postgresql.conf', qq{
autovacuum = off
wal_level = replica
});

$node->start;

# ============================================================
# Test 1: Epoch slot update from INSERT survives recovery
# ============================================================

$node->safe_psql('postgres', qq{
CREATE TABLE epoch_recovery_test (id int, val text);
INSERT INTO epoch_recovery_test VALUES (1, 'survives crash');
INSERT INTO epoch_recovery_test VALUES (2, 'also survives');
});

# Verify epoch data exists before crash
my $mode_before = $node->safe_psql('postgres',
	"SELECT epoch_xid_relation_mode('epoch_recovery_test'::regclass)");
is($mode_before, 'materialized',
	'relation is materialized after INSERT');

my $flags_before = $node->safe_psql('postgres',
	"SELECT epoch_flags FROM epoch_xid_inspect('epoch_recovery_test'::regclass, 0) WHERE offnum = 1");
is($flags_before, '1',
	'slot 1 has EPOCH_FLAG_XMIN_SET before crash');

# Issue a checkpoint to ensure a clean base state, then do one more
# insert whose epoch update will need non-FPI redo
$node->safe_psql('postgres', 'CHECKPOINT');
$node->safe_psql('postgres',
	"INSERT INTO epoch_recovery_test VALUES (3, 'post-checkpoint insert')");

# Crash (immediate shutdown = no clean shutdown checkpoint)
$node->stop('immediate');

# Restart (recovery replays WAL including epoch slot updates)
$node->start;

# Verify epoch data survived recovery
my $mode_after = $node->safe_psql('postgres',
	"SELECT epoch_xid_relation_mode('epoch_recovery_test'::regclass)");
is($mode_after, 'materialized',
	'relation is still materialized after recovery');

my $flags_slot1 = $node->safe_psql('postgres',
	"SELECT epoch_flags FROM epoch_xid_inspect('epoch_recovery_test'::regclass, 0) WHERE offnum = 1");
is($flags_slot1, '1',
	'slot 1 epoch_flags survived recovery (XMIN_SET)');

my $flags_slot3 = $node->safe_psql('postgres',
	"SELECT epoch_flags FROM epoch_xid_inspect('epoch_recovery_test'::regclass, 0) WHERE offnum = 3");
is($flags_slot3, '1',
	'slot 3 (post-checkpoint insert) epoch_flags survived recovery via non-FPI redo');

# Verify actual data is intact
my $count = $node->safe_psql('postgres',
	"SELECT count(*) FROM epoch_recovery_test");
is($count, '3', 'all rows survived recovery');

# ============================================================
# Test 2: Epoch slot update from DELETE survives recovery
# ============================================================

$node->safe_psql('postgres', 'CHECKPOINT');
$node->safe_psql('postgres',
	"DELETE FROM epoch_recovery_test WHERE id = 1");

my $flags_deleted = $node->safe_psql('postgres',
	"SELECT epoch_flags FROM epoch_xid_inspect('epoch_recovery_test'::regclass, 0) WHERE offnum = 1");
is($flags_deleted, '3',
	'slot 1 has XMIN_SET|XMAX_SET after delete (pre-crash)');

# Crash
$node->stop('immediate');
$node->start;

# Verify delete epoch survived recovery
my $flags_after_delete = $node->safe_psql('postgres',
	"SELECT epoch_flags FROM epoch_xid_inspect('epoch_recovery_test'::regclass, 0) WHERE offnum = 1");
is($flags_after_delete, '3',
	'slot 1 XMAX_SET survived recovery after delete');

# ============================================================
# Test 3: Truncate recovery
# ============================================================

# Verify epoch data exists
my $epoch_rows_before = $node->safe_psql('postgres',
	"SELECT count(*) FROM epoch_xid_inspect('epoch_recovery_test'::regclass, 0) WHERE epoch_flags != 0");
ok($epoch_rows_before > 0,
	'epoch data exists before truncate');

$node->safe_psql('postgres', 'CHECKPOINT');
$node->safe_psql('postgres', 'TRUNCATE epoch_recovery_test');

# Crash
$node->stop('immediate');
$node->start;

# After recovery, truncated table should have no epoch data
my $epoch_rows_after = $node->safe_psql('postgres',
	"SELECT count(*) FROM epoch_xid_inspect('epoch_recovery_test'::regclass, 0)");
is($epoch_rows_after, '0',
	'epoch fork truncation survived recovery - no stale data');

# Cleanup
$node->safe_psql('postgres', 'DROP TABLE epoch_recovery_test');

$node->stop;

done_testing();
