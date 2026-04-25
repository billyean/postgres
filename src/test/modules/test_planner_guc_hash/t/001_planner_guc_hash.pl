# Copyright (c) 2026, PostgreSQL Global Development Group

# T14: Cross-backend determinism for planner GUC hash.
#
# Verifies that two independent backends with the same GUC settings
# produce the same planner GUC hash, that changing a GUC in one backend
# makes the hashes differ, and that resetting restores equality.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

# Create the extension in a fresh database.
$node->safe_psql('postgres', 'CREATE DATABASE testdb');
$node->safe_psql('testdb', 'CREATE EXTENSION test_planner_guc_hash');

# Two independent connections to the same database, default GUC settings.
my $hash_a = $node->safe_psql('testdb',
	'SELECT test_planner_guc_hash()');
my $hash_b = $node->safe_psql('testdb',
	'SELECT test_planner_guc_hash()');

# Compare as strings — do not assume positive or impose ordering.
is($hash_a, $hash_b,
	'two backends with identical GUC settings produce the same hash');

# Change a planner GUC in one "backend" (session) and compare.
my $hash_changed = $node->safe_psql('testdb',
	'SET enable_seqscan = off; SELECT test_planner_guc_hash()');
my $hash_default = $node->safe_psql('testdb',
	'SELECT test_planner_guc_hash()');

isnt($hash_changed, $hash_default,
	'different enable_seqscan settings produce different hashes');

# Reset and verify equality is restored.
my $hash_reset = $node->safe_psql('testdb',
	'SET enable_seqscan = off; RESET enable_seqscan; SELECT test_planner_guc_hash()');

is($hash_reset, $hash_default,
	'resetting enable_seqscan restores the original hash');

# Test with a cost constant.
my $hash_cost_changed = $node->safe_psql('testdb',
	'SET random_page_cost = 2.0; SELECT test_planner_guc_hash()');

isnt($hash_cost_changed, $hash_default,
	'different random_page_cost produces different hash');

my $hash_cost_reset = $node->safe_psql('testdb',
	'SET random_page_cost = 2.0; RESET random_page_cost; SELECT test_planner_guc_hash()');

is($hash_cost_reset, $hash_default,
	'resetting random_page_cost restores the original hash');

$node->stop;
done_testing();
