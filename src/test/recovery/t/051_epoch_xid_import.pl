
# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# Test Patch 26: imported-snapshot bridge re-derivation.
#
# Validates that after SET TRANSACTION SNAPSHOT, the bounded bridge
# state is re-derived from the imported 32-bit fields, the acquisition
# contract is validated with source='import', and the bounded bridge
# path produces correct visibility results.

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
max_prepared_transactions = 10
});

$node->start;

# ============================================================
# Setup: create an epoch-materialized table
# ============================================================

$node->safe_psql('postgres', qq{
CREATE TABLE epoch_p26 (id int PRIMARY KEY, val text);
INSERT INTO epoch_p26 VALUES (1, 'exported_row');
});

# ============================================================
# Test 1: Imported snapshot has contract source 'import'
#         and passes the acquisition contract check
# ============================================================

# Session A: export a snapshot
my $export_psql = $node->background_psql('postgres');
$export_psql->query_safe("BEGIN ISOLATION LEVEL REPEATABLE READ");
my $snap_id = $export_psql->query_safe("SELECT pg_export_snapshot()");
chomp $snap_id;
# strip any whitespace
$snap_id =~ s/^\s+|\s+$//g;

# Session B: import the snapshot and check bridge state
my $source = $node->safe_psql('postgres', qq{
BEGIN ISOLATION LEVEL REPEATABLE READ;
SET TRANSACTION SNAPSHOT '$snap_id';
SELECT epoch_xid_acquisition_contract_source();
});
is($source, 'import',
	'P26: contract source is import after SET TRANSACTION SNAPSHOT');

my $contract_ok = $node->safe_psql('postgres', qq{
BEGIN ISOLATION LEVEL REPEATABLE READ;
SET TRANSACTION SNAPSHOT '$snap_id';
SELECT epoch_xid_acquisition_contract_check();
});
is($contract_ok, 't',
	'P26: acquisition contract check passes after import');

# ============================================================
# Test 2: Imported snapshot supports the bounded bridge path
#         and produces correct visibility
# ============================================================

my $result = $node->safe_psql('postgres', qq{
BEGIN ISOLATION LEVEL REPEATABLE READ;
SET TRANSACTION SNAPSHOT '$snap_id';
SELECT * FROM epoch_p26 WHERE ctid = '(0,1)';
});
is($result, '1|exported_row',
	'P26: imported snapshot produces correct visibility result');

my $path = $node->safe_psql('postgres', qq{
BEGIN ISOLATION LEVEL REPEATABLE READ;
SET TRANSACTION SNAPSHOT '$snap_id';
SELECT * FROM epoch_p26 WHERE ctid = '(0,1)';
SELECT epoch_xid_mvcc_last_path();
});
# The result contains two rows: the data row and the path
my @lines = split /\n/, $path;
is($lines[-1], 'snapshot_bridge',
	'P26: bounded bridge path used on imported snapshot');

my $caller = $node->safe_psql('postgres', qq{
BEGIN ISOLATION LEVEL REPEATABLE READ;
SET TRANSACTION SNAPSHOT '$snap_id';
SELECT * FROM epoch_p26 WHERE ctid = '(0,1)';
SELECT epoch_xid_mvcc_last_caller();
});
@lines = split /\n/, $caller;
is($lines[-1], 'heap_fetch',
	'P26: caller is heap_fetch for TID scan on imported snapshot');

# ============================================================
# Test 3: Full bounded stack is exercised
# ============================================================

my $stack = $node->safe_psql('postgres', qq{
BEGIN ISOLATION LEVEL REPEATABLE READ;
SET TRANSACTION SNAPSHOT '$snap_id';
SELECT * FROM epoch_p26 WHERE ctid = '(0,1)';
SELECT epoch_xid_bridge_context_source();
});
@lines = split /\n/, $stack;
is($lines[-1], 'context',
	'P26: bridge context used on imported snapshot');

my $decision = $node->safe_psql('postgres', qq{
BEGIN ISOLATION LEVEL REPEATABLE READ;
SET TRANSACTION SNAPSHOT '$snap_id';
SELECT * FROM epoch_p26 WHERE ctid = '(0,1)';
SELECT epoch_xid_bridge_decision_source();
});
@lines = split /\n/, $decision;
is($lines[-1], 'decision',
	'P26: decision contract used on imported snapshot');

my $membership = $node->safe_psql('postgres', qq{
BEGIN ISOLATION LEVEL REPEATABLE READ;
SET TRANSACTION SNAPSHOT '$snap_id';
SELECT * FROM epoch_p26 WHERE ctid = '(0,1)';
SELECT epoch_xid_membership_last_source();
});
@lines = split /\n/, $membership;
is($lines[-1], 'view',
	'P26: membership view path used on imported snapshot');

# ============================================================
# Test 4: Source distinguishes acquire vs import vs copy
# ============================================================

my $acquire_source = $node->safe_psql('postgres', qq{
BEGIN;
SELECT epoch_xid_acquisition_contract_source();
});
is($acquire_source, 'acquire',
	'P26: normal acquisition still reports acquire');

my $copy_source = $node->safe_psql('postgres', qq{
BEGIN;
DECLARE epoch_p26_cur CURSOR FOR SELECT 1;
FETCH NEXT FROM epoch_p26_cur;
SELECT epoch_xid_acquisition_contract_source();
});
# Result has two rows: the fetched '1' and the source
@lines = split /\n/, $copy_source;
is($lines[-1], 'copy',
	'P26: cursor copy still reports copy');

# Clean up session A
$export_psql->query_safe("COMMIT");
$export_psql->quit;

# ============================================================
# Teardown
# ============================================================

$node->safe_psql('postgres', 'DROP TABLE epoch_p26');

$node->stop;
done_testing();
