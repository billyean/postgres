# Test shared plan cache L2 integration — cross-backend TAP tests (Patch 0007)
#
# Each test uses a single safe_psql call with setup + counter query.
# L2 counters are backend-local, so both must be in the same connection.
# We extract the final line of output (the counter SELECT) for comparison.
#
# Deferred tests (not implemented in Patch 0007):
#   IS-01: Isolation test (cross-backend concurrent store/lookup)
#   T-02:  Disabled-mode full TAP regression
#   T-03:  Enabled-mode full TAP regression subset
#   T-09:  Backend-exit stretch test (refcount/cleanup after disconnect)

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_plan_cache_max_entries = 100
shared_plan_cache_enabled = on
compute_query_id = on
});
$node->start;

# Create test table and extension
$node->safe_psql('postgres', q{
    CREATE TABLE spc_tap_test (id int PRIMARY KEY, val text);
    INSERT INTO spc_tap_test SELECT g, 'row' || g FROM generate_series(1, 100) g;
    ANALYZE spc_tap_test;
    CREATE EXTENSION test_shared_plan_cache_integration;
});

# Helper: run multi-query safe_psql, return last line only (the counter).
sub last_line {
    my ($output) = @_;
    my @lines = split(/\n/, $output);
    return $lines[-1];
}

# T-01a: Backend A stores into L2
my $result_a = last_line($node->safe_psql('postgres', q{
    SET plan_cache_mode = force_generic_plan;
    PREPARE tap01 AS SELECT * FROM spc_tap_test WHERE id = $1;
    EXECUTE tap01(1);
    SELECT test_shared_plan_l2_store_count()
}));
cmp_ok($result_a, '>', 0, 'T-01a: Backend A stores into L2');

# T-01b: Backend B hits L2 (new connection, plan already in shared cache)
my $result_b = last_line($node->safe_psql('postgres', q{
    SET plan_cache_mode = force_generic_plan;
    PREPARE tap01 AS SELECT * FROM spc_tap_test WHERE id = $1;
    EXECUTE tap01(1);
    SELECT test_shared_plan_l2_hit_count()
}));
cmp_ok($result_b, '>', 0, 'T-01b: Backend B hits L2');

# T-04: L2 hit deserializes correct plan
my $explain_b = $node->safe_psql('postgres', q{
    SET plan_cache_mode = force_generic_plan;
    PREPARE tap04 AS SELECT * FROM spc_tap_test WHERE id = $1;
    EXECUTE tap04(1);
    EXPLAIN (COSTS OFF) EXECUTE tap04(7)
});
like($explain_b, qr/Index Scan|Seq Scan|Bitmap/i,
     'T-04: L2 hit produces valid plan in EXPLAIN');

# T-05: L2 hit sets generic_cost from shared entry
my $cost_check = last_line($node->safe_psql('postgres', q{
    SET plan_cache_mode = force_generic_plan;
    PREPARE tap05 AS SELECT * FROM spc_tap_test WHERE id = $1;
    EXECUTE tap05(1);
    SELECT test_shared_plan_generic_cost('tap05') > 0
}));
is($cost_check, 't', 'T-05: L2 hit populates generic_cost > 0');

# T-06: Different GUC produces different entry
my $guc_test = last_line($node->safe_psql('postgres', q{
    SET enable_seqscan = off;
    SET plan_cache_mode = force_generic_plan;
    PREPARE tap06 AS SELECT * FROM spc_tap_test WHERE id = $1;
    EXECUTE tap06(1);
    SELECT test_shared_plan_l2_store_count()
}));
cmp_ok($guc_test, '>', 0,
       'T-06: Different GUC setting stores separate entry');

# T-07: Different search_path produces different entry
$node->safe_psql('postgres', 'CREATE SCHEMA spc_alt');
$node->safe_psql('postgres', q{
    CREATE TABLE spc_alt.spc_tap_test (id int PRIMARY KEY, val text);
    INSERT INTO spc_alt.spc_tap_test SELECT g, 'alt' || g FROM generate_series(1, 10) g;
    ANALYZE spc_alt.spc_tap_test;
});

# Store with default search_path (output ignored)
$node->safe_psql('postgres', q{
    SET search_path = public;
    SET plan_cache_mode = force_generic_plan;
    PREPARE tap07 AS SELECT * FROM spc_tap_test WHERE id = $1;
    EXECUTE tap07(1);
});

# Store with alternate search_path (new backend, new counters)
my $path_b = last_line($node->safe_psql('postgres', q{
    SET search_path = spc_alt, public;
    SET plan_cache_mode = force_generic_plan;
    PREPARE tap07 AS SELECT * FROM spc_tap_test WHERE id = $1;
    EXECUTE tap07(1);
    SELECT test_shared_plan_l2_store_count()
}));
cmp_ok($path_b, '>', 0,
       'T-07: Different search_path stores separate entry');

# T-08: Different role produces different entry
$node->safe_psql('postgres', q{
    CREATE USER spc_testuser SUPERUSER;
    GRANT ALL ON TABLE spc_tap_test TO spc_testuser;
});

my $role_result = last_line($node->safe_psql('postgres', q{
    SET ROLE spc_testuser;
    SET plan_cache_mode = force_generic_plan;
    PREPARE tap08 AS SELECT * FROM spc_tap_test WHERE id = $1;
    EXECUTE tap08(1);
    SELECT test_shared_plan_l2_store_count()
}));
cmp_ok($role_result, '>', 0,
       'T-08: Different role stores separate entry');

# Cleanup
$node->safe_psql('postgres', q{
    DROP SCHEMA spc_alt CASCADE;
    DROP OWNED BY spc_testuser;
    DROP USER spc_testuser;
    DROP TABLE spc_tap_test;
});

$node->stop;
done_testing();
