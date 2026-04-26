# Copyright (c) 2026, PostgreSQL Global Development Group

# Tests for shared plan cache shmem setup with non-default GUC configs.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# T4/T5: Server starts with max_entries=0 (disabled)
{
	my $node = PostgreSQL::Test::Cluster->new('disabled');
	$node->init;
	$node->append_conf('postgresql.conf',
		'shared_plan_cache_max_entries = 0');
	$node->start;

	$node->safe_psql('postgres',
		'CREATE EXTENSION test_shared_plan_cache_shmem');

	my $attached = $node->safe_psql('postgres',
		'SELECT test_shared_plan_cache_attached()');
	is($attached, 'f', 'not attached when max_entries=0');

	my $active = $node->safe_psql('postgres',
		'SELECT test_shared_plan_cache_active()');
	is($active, 'f', 'not active when max_entries=0');

	my $handles = $node->safe_psql('postgres',
		'SELECT test_shared_plan_cache_handles_valid()');
	is($handles, 'f', 'handles invalid when max_entries=0');

	$node->stop;
}

# T6: compute_query_id=off disables cache with WARNING
{
	my $node = PostgreSQL::Test::Cluster->new('queryid_off');
	$node->init;
	$node->append_conf('postgresql.conf',
		'compute_query_id = off');
	$node->start;

	$node->safe_psql('postgres',
		'CREATE EXTENSION test_shared_plan_cache_shmem');

	# Record log position before the query that triggers attach + WARNING.
	my $log_offset = -s $node->logfile;

	my $active = $node->safe_psql('postgres',
		'SELECT test_shared_plan_cache_active()');
	is($active, 'f', 'not active when compute_query_id=off');

	# Check only new log content for the WARNING.
	open(my $fh, '<', $node->logfile)
		or die "could not open logfile: $!";
	seek($fh, $log_offset, 0)
		or die "could not seek logfile: $!";
	my $new_log = do { local $/; <$fh> };
	close($fh);
	like($new_log, qr/shared plan cache disabled.*compute_query_id/,
		'WARNING about compute_query_id=off in log');

	$node->stop;
}

done_testing();
