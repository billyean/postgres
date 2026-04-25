# Copyright (c) 2026, PostgreSQL Global Development Group

# T13: Cross-backend determinism for search path hash.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE DATABASE testdb');
$node->safe_psql('testdb', 'CREATE EXTENSION test_search_path_hash');
$node->safe_psql('testdb', 'CREATE SCHEMA sp_tap_a');
$node->safe_psql('testdb', 'CREATE SCHEMA sp_tap_b');

# Separate backend sessions, default search_path.
my $hash_a = $node->safe_psql('testdb',
	'SELECT test_search_path_hash()');
my $hash_b = $node->safe_psql('testdb',
	'SELECT test_search_path_hash()');

is($hash_a, $hash_b,
	'same default search_path produces same hash across backends');

# Change search_path in one session.
my $hash_changed = $node->safe_psql('testdb',
	'SET search_path = sp_tap_a, sp_tap_b, public; SELECT test_search_path_hash()');
my $hash_default = $node->safe_psql('testdb',
	'SELECT test_search_path_hash()');

isnt($hash_changed, $hash_default,
	'different search_path produces different hash');

# Reset and verify equality restored.
my $hash_reset = $node->safe_psql('testdb',
	'SET search_path = sp_tap_a, sp_tap_b, public; RESET search_path; SELECT test_search_path_hash()');

is($hash_reset, $hash_default,
	'resetting search_path restores the original hash');

$node->safe_psql('testdb', 'DROP SCHEMA sp_tap_a');
$node->safe_psql('testdb', 'DROP SCHEMA sp_tap_b');
$node->stop;
done_testing();
