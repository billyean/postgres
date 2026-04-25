# Copyright (c) 2026, PostgreSQL Global Development Group

# Cross-backend determinism for plan shareability predicate.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE DATABASE testdb');
$node->safe_psql('testdb', 'CREATE EXTENSION test_plan_shareability');

# Separate backend sessions, same prepared statement.
my $result_a = $node->safe_psql('testdb',
	'SET plan_cache_mode = force_generic_plan; PREPARE sp1 AS SELECT 1; SELECT test_plan_shareability(\'sp1\')');
my $result_b = $node->safe_psql('testdb',
	'SET plan_cache_mode = force_generic_plan; PREPARE sp1 AS SELECT 1; SELECT test_plan_shareability(\'sp1\')');

is($result_a, 'NONE',
	'simple prepared statement is shareable in session A');
is($result_b, 'NONE',
	'simple prepared statement is shareable in session B');
is($result_a, $result_b,
	'shareability result is consistent across backend sessions');

# Temp table should be rejected in both sessions.
my $result_temp = $node->safe_psql('testdb',
	'SET plan_cache_mode = force_generic_plan; CREATE TEMP TABLE tap_tmp (x int); PREPARE sp2 AS SELECT * FROM tap_tmp; SELECT test_plan_shareability(\'sp2\')');

is($result_temp, 'TEMP_OBJECT',
	'temp table plan is rejected as TEMP_OBJECT');

$node->stop;
done_testing();
