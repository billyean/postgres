# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# S8: Disabled cache (max_entries=0) => DISABLED
{
	my $node = PostgreSQL::Test::Cluster->new('disabled');
	$node->init;
	$node->append_conf('postgresql.conf',
		'shared_plan_cache_max_entries = 0');
	$node->start;

	$node->safe_psql('postgres',
		'CREATE EXTENSION test_shared_plan_cache_store');

	my $result = $node->safe_psql('postgres', q{
		SET plan_cache_mode = force_generic_plan;
		PREPARE d1 AS SELECT 1;
		SELECT test_shared_plan_store('d1');
	});
	is($result, 'DISABLED', 'store returns DISABLED when max_entries=0');

	$node->stop;
}

# S9: Full cache (max_entries=1) => FULL for second distinct plan
{
	my $node = PostgreSQL::Test::Cluster->new('full');
	$node->init;
	$node->append_conf('postgresql.conf',
		'shared_plan_cache_max_entries = 1');
	$node->start;

	$node->safe_psql('postgres',
		'CREATE EXTENSION test_shared_plan_cache_store');

	my $r1 = $node->safe_psql('postgres', q{
		SET plan_cache_mode = force_generic_plan;
		PREPARE f1 AS SELECT 1;
		SELECT test_shared_plan_store('f1');
	});
	is($r1, 'OK', 'first store succeeds with max_entries=1');

	my $count1 = $node->safe_psql('postgres',
		'SELECT test_shared_plan_store_count()');
	is($count1, '1', 'current_entries is 1 after first store');

	my $r2 = $node->safe_psql('postgres', q{
		SET plan_cache_mode = force_generic_plan;
		PREPARE f2 AS SELECT 2;
		SELECT test_shared_plan_store('f2');
	});
	is($r2, 'FULL', 'second distinct store returns FULL with max_entries=1');

	my $count2 = $node->safe_psql('postgres',
		'SELECT test_shared_plan_store_count()');
	is($count2, '1', 'current_entries unchanged after FULL');

	$node->stop;
}

# S9b: Duplicate when full returns DUPLICATE, not FULL
{
	my $node = PostgreSQL::Test::Cluster->new('dup_when_full');
	$node->init;
	$node->append_conf('postgresql.conf',
		'shared_plan_cache_max_entries = 1');
	$node->start;

	$node->safe_psql('postgres',
		'CREATE EXTENSION test_shared_plan_cache_store');

	my $result = $node->safe_psql('postgres', q{
		SET plan_cache_mode = force_generic_plan;
		PREPARE df1 AS SELECT 1;
		SELECT test_shared_plan_store('df1');
		SELECT test_shared_plan_store('df1');
		SELECT test_shared_plan_store_count();
	});
	my @lines = split(/\n/, $result);
	is($lines[0], 'OK', 'first store in dup-when-full succeeds');
	is($lines[1], 'DUPLICATE', 'duplicate store when full returns DUPLICATE, not FULL');
	is($lines[2], '1', 'current_entries unchanged after duplicate-when-full');

	$node->stop;
}

# S7: Oversize with very small max_entry_size
{
	my $node = PostgreSQL::Test::Cluster->new('oversize');
	$node->init;
	$node->append_conf('postgresql.conf',
		"shared_plan_cache_max_entry_size = '8kB'");
	$node->start;

	$node->safe_psql('postgres',
		'CREATE EXTENSION test_shared_plan_cache_store');

	# Build a query with many columns to reliably exceed 8KB serialized
	my @cols;
	for my $i (1..500) {
		push @cols, "$i AS c$i";
	}
	my $big_select = "SELECT " . join(", ", @cols);

	# Verify size exceeds limit, then attempt store
	my $r = $node->safe_psql('postgres', qq{
		SET plan_cache_mode = force_generic_plan;
		PREPARE os1 AS $big_select;
		SELECT test_shared_plan_store('os1');
	});
	is($r, 'OVERSIZE', 'store returns OVERSIZE with small max_entry_size');

	my $count = $node->safe_psql('postgres',
		'SELECT test_shared_plan_store_count()');
	is($count, '0', 'current_entries unchanged after OVERSIZE');

	$node->stop;
}

# C1: Explicit COLLATE test (if "C" and "POSIX" have different OIDs)
{
	my $node = PostgreSQL::Test::Cluster->new('collation');
	$node->init;
	$node->start;

	$node->safe_psql('postgres',
		'CREATE EXTENSION test_shared_plan_cache_store');

	# Check if "C" and "POSIX" are distinct collations
	my $distinct = $node->safe_psql('postgres', q{
		SELECT (SELECT oid FROM pg_collation WHERE collname = 'C' LIMIT 1) !=
		       (SELECT oid FROM pg_collation WHERE collname = 'POSIX' LIMIT 1)
		       AS are_distinct;
	});

	if ($distinct eq 't') {
		my $hash_c = $node->safe_psql('postgres', q{
			SET plan_cache_mode = force_generic_plan;
			PREPARE cc1 AS SELECT 'a' COLLATE "C";
			SELECT test_shared_plan_key_field('cc1', 'collation_hash');
		});
		my $hash_posix = $node->safe_psql('postgres', q{
			SET plan_cache_mode = force_generic_plan;
			PREPARE cc2 AS SELECT 'a' COLLATE "POSIX";
			SELECT test_shared_plan_key_field('cc2', 'collation_hash');
		});
		isnt($hash_c, $hash_posix,
			 'different COLLATE produces different collation_hash');
	}
	else {
		pass('skipped C1: C and POSIX have same OID');
	}

	$node->stop;
}

done_testing();
