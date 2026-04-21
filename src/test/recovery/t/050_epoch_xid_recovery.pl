
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

# ============================================================
# Test 4: Same-page update epoch recovery
# ============================================================

$node->safe_psql('postgres', qq{
CREATE TABLE epoch_update_recovery (id int, val text);
INSERT INTO epoch_update_recovery VALUES (1, 'original');
});

$node->safe_psql('postgres', 'CHECKPOINT');
$node->safe_psql('postgres',
	"UPDATE epoch_update_recovery SET val = 'updated' WHERE id = 1");

# Crash
$node->stop('immediate');
$node->start;

# Old slot (offnum 1) should have flags = 3 (XMIN_SET|XMAX_SET)
my $update_old_flags = $node->safe_psql('postgres',
	"SELECT epoch_flags FROM epoch_xid_inspect('epoch_update_recovery'::regclass, 0) WHERE offnum = 1");
is($update_old_flags, '3',
	'same-page update: old slot xmax survived recovery');

# New slot (offnum 2) should have flags = 1 (XMIN_SET only)
my $update_new_flags = $node->safe_psql('postgres',
	"SELECT epoch_flags FROM epoch_xid_inspect('epoch_update_recovery'::regclass, 0) WHERE offnum = 2");
is($update_new_flags, '1',
	'same-page update: new slot xmin survived recovery');

$node->safe_psql('postgres', 'DROP TABLE epoch_update_recovery');

# ============================================================
# Test 5: Cross-page update non-FPI recovery
# ============================================================

$node->safe_psql('postgres', qq{
CREATE TABLE epoch_xpage_recovery (id int, val bytea);
INSERT INTO epoch_xpage_recovery
  SELECT g, decode(repeat(lpad(to_hex(g), 2, '0'), 100), 'hex')
  FROM generate_series(1, 30) g;
});

# Record original tuple location before update
my $orig_loc = $node->safe_psql('postgres',
	"SELECT (ctid::text::point)[0]::int, (ctid::text::point)[1]::int
	 FROM epoch_xpage_recovery WHERE id = 1");
my ($orig_page, $orig_offnum) = split(/\|/, $orig_loc);
$orig_page =~ s/^\s+|\s+$//g;
$orig_offnum =~ s/^\s+|\s+$//g;

# Checkpoint establishes clean base; subsequent modifications use BufData
$node->safe_psql('postgres', 'CHECKPOINT');

# Update with large non-compressible value to force cross-page
$node->safe_psql('postgres',
	"UPDATE epoch_xpage_recovery SET val = decode(repeat('deadbeef', 500), 'hex') WHERE id = 1");

# Crash
$node->stop('immediate');
$node->start;

# Old slot at original location should have xmax set (flags = 3)
my $xpage_old = $node->safe_psql('postgres',
	"SELECT epoch_flags FROM epoch_xid_inspect('epoch_xpage_recovery'::regclass, $orig_page)
	 WHERE offnum = $orig_offnum");
is($xpage_old, '3',
	'cross-page update non-FPI: old slot xmax survived recovery');

# Find where new tuple actually landed after recovery
my $new_loc = $node->safe_psql('postgres',
	"SELECT (ctid::text::point)[0]::int, (ctid::text::point)[1]::int
	 FROM epoch_xpage_recovery WHERE id = 1");
my ($new_page, $new_offnum) = split(/\|/, $new_loc);
$new_page =~ s/^\s+|\s+$//g;
$new_offnum =~ s/^\s+|\s+$//g;

# Verify it moved to a different page
ok($new_page != $orig_page,
	'cross-page update non-FPI: new tuple is on different page after recovery');

# New slot at actual destination should have xmin set (flags = 1)
my $xpage_new = $node->safe_psql('postgres',
	"SELECT epoch_flags FROM epoch_xid_inspect('epoch_xpage_recovery'::regclass, $new_page)
	 WHERE offnum = $new_offnum");
is($xpage_new, '1',
	'cross-page update non-FPI: new slot xmin survived recovery');

$node->safe_psql('postgres', 'DROP TABLE epoch_xpage_recovery');

# ============================================================
# Test 6: Implicit→materialized via update recovery
# ============================================================

$node->safe_psql('postgres', qq{
CREATE TABLE epoch_implicit_upd_rec (id int, val text);
COPY epoch_implicit_upd_rec FROM stdin;
1\toriginal
2\talso original
\\.
});

# Confirm implicit
my $mode_pre = $node->safe_psql('postgres',
	"SELECT epoch_xid_relation_mode('epoch_implicit_upd_rec'::regclass)");
is($mode_pre, 'implicit',
	'relation is implicit before update');

$node->safe_psql('postgres', 'CHECKPOINT');
$node->safe_psql('postgres',
	"UPDATE epoch_implicit_upd_rec SET val = 'updated' WHERE id = 1");

# Crash
$node->stop('immediate');
$node->start;

# Should be materialized after recovery
my $mode_post = $node->safe_psql('postgres',
	"SELECT epoch_xid_relation_mode('epoch_implicit_upd_rec'::regclass)");
is($mode_post, 'materialized',
	'implicit->materialized via update survived recovery');

# Epoch data should exist
my $upd_flags = $node->safe_psql('postgres',
	"SELECT count(*) FROM epoch_xid_inspect('epoch_implicit_upd_rec'::regclass, 0) WHERE epoch_flags != 0");
ok($upd_flags > 0,
	'epoch data from update materialization survived recovery');

$node->safe_psql('postgres', 'DROP TABLE epoch_implicit_upd_rec');

# ============================================================
# Test 7: Phase 3 — interpretation function stable after recovery
# ============================================================

$node->safe_psql('postgres', qq{
CREATE TABLE epoch_interp_recovery (id int, val text);
INSERT INTO epoch_interp_recovery VALUES (1, 'test');
});

# Verify interpretation before crash
my $interp_before = $node->safe_psql('postgres',
	"SELECT xmin_interp, xmax_interp, relation_mode
	 FROM epoch_xid_tuple_visibility_info('epoch_interp_recovery'::regclass, '(0,1)'::tid)");
is($interp_before, 'materialized|invalid_unset|materialized',
	'Phase 3: interpretation correct before crash');

$node->safe_psql('postgres', 'CHECKPOINT');
$node->safe_psql('postgres',
	"DELETE FROM epoch_interp_recovery WHERE id = 1");

# Crash
$node->stop('immediate');
$node->start;

# After recovery: interpretation should show materialized xmax
my $interp_after = $node->safe_psql('postgres',
	"SELECT xmin_interp, xmax_interp, relation_mode
	 FROM epoch_xid_tuple_visibility_info('epoch_interp_recovery'::regclass, '(0,1)'::tid)");
is($interp_after, 'materialized|materialized|materialized',
	'Phase 3: interpretation stable after crash/recovery');

$node->safe_psql('postgres', 'DROP TABLE epoch_interp_recovery');

# ============================================================
# Test 8: Phase 3 — implicit/materialized distinction after restart
# ============================================================

$node->safe_psql('postgres', qq{
CREATE TABLE epoch_mode_recovery_a (id int);
CREATE TABLE epoch_mode_recovery_b (id int);
});

# Table A: implicit (COPY only)
$node->safe_psql('postgres',
	"COPY epoch_mode_recovery_a FROM stdin;\n1\n\\.\n");

# Table B: materialized (INSERT)
$node->safe_psql('postgres',
	"INSERT INTO epoch_mode_recovery_b VALUES (1)");

$node->safe_psql('postgres', 'CHECKPOINT');

# Crash
$node->stop('immediate');
$node->start;

# Verify modes survived recovery
my $mode_a = $node->safe_psql('postgres',
	"SELECT relation_mode FROM epoch_xid_tuple_visibility_info('epoch_mode_recovery_a'::regclass, '(0,1)'::tid)");
is($mode_a, 'implicit',
	'Phase 3: implicit relation stays implicit after recovery');

my $mode_b = $node->safe_psql('postgres',
	"SELECT relation_mode FROM epoch_xid_tuple_visibility_info('epoch_mode_recovery_b'::regclass, '(0,1)'::tid)");
is($mode_b, 'materialized',
	'Phase 3: materialized relation stays materialized after recovery');

$node->safe_psql('postgres', 'DROP TABLE epoch_mode_recovery_a, epoch_mode_recovery_b');

# ============================================================
# Test 9: Phase 4 — txn-state classification stable after recovery
# ============================================================

$node->safe_psql('postgres', qq{
CREATE TABLE epoch_txnstate_recovery (id int);
INSERT INTO epoch_txnstate_recovery VALUES (1);
DELETE FROM epoch_txnstate_recovery WHERE id = 1;
});

$node->safe_psql('postgres', 'CHECKPOINT');
$node->stop('immediate');
$node->start;

my $ts_after = $node->safe_psql('postgres',
	"SELECT tuple_state FROM epoch_xid_tuple_txn_state_info('epoch_txnstate_recovery'::regclass, '(0,1)'::tid)");
is($ts_after, 'dead_committed',
	'Phase 4: dead_committed classification stable after recovery');

$node->safe_psql('postgres', 'DROP TABLE epoch_txnstate_recovery');

# ============================================================
# Test 10: Phase 4 — live_committed stable after recovery
# ============================================================

$node->safe_psql('postgres', qq{
CREATE TABLE epoch_txnlive_recovery (id int);
INSERT INTO epoch_txnlive_recovery VALUES (1);
});

$node->safe_psql('postgres', 'CHECKPOINT');
$node->stop('immediate');
$node->start;

my $live_state = $node->safe_psql('postgres',
	"SELECT tuple_state, relation_mode
	 FROM epoch_xid_tuple_txn_state_info('epoch_txnlive_recovery'::regclass, '(0,1)'::tid)");
is($live_state, 'live_committed|materialized',
	'Phase 4: live_committed + materialized stable after recovery');

$node->safe_psql('postgres', 'DROP TABLE epoch_txnlive_recovery');

# ============================================================
# Test 11: Phase 4 — implicit mode classification after restart
# ============================================================

$node->safe_psql('postgres', qq{
CREATE TABLE epoch_txn_implicit_rec (id int);
});
$node->safe_psql('postgres',
	"COPY epoch_txn_implicit_rec FROM stdin;\n1\n\\.\n");

$node->safe_psql('postgres', 'CHECKPOINT');
$node->stop('immediate');
$node->start;

my $implicit_state = $node->safe_psql('postgres',
	"SELECT tuple_state, relation_mode
	 FROM epoch_xid_tuple_txn_state_info('epoch_txn_implicit_rec'::regclass, '(0,1)'::tid)");
is($implicit_state, 'live_committed|implicit',
	'Phase 4: implicit mode classification stable after recovery');

$node->safe_psql('postgres', 'DROP TABLE epoch_txn_implicit_rec');

$node->stop;

done_testing();
