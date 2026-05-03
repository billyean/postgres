use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('spc_inval');
$node->init;
$node->append_conf('postgresql.conf', <<CONF);
shared_plan_cache_max_entries = 100
compute_query_id = on
shared_plan_cache_enabled = on
shared_preload_libraries = ''
CONF
$node->start;

$node->safe_psql('postgres', q{
    CREATE EXTENSION test_shared_plan_cache_invalidation;
    CREATE TABLE tap_t1 (a int);
    INSERT INTO tap_t1 VALUES (1), (2), (3);
    ANALYZE tap_t1;
});

# ---------------------------------------------------------------
# T2: Cross-backend DDL invalidation with compute_query_id=off DDL backend
#
# Uses captured-key approach to prove the OLD L2 entry is invalidated,
# independent of whether DDL changes the query_tree_hash / SharedPlanKey.
#
# Uses background_psql for persistent sessions.
# ---------------------------------------------------------------

# Backend A: store an L2 entry for tap_t1
my $psql_a = $node->background_psql('postgres');
$psql_a->query_safe("SET plan_cache_mode = force_generic_plan");
$psql_a->query_safe("PREPARE tap_a1 AS SELECT a FROM tap_t1 WHERE a = \$1");
$psql_a->query_safe("EXECUTE tap_a1(1)");

# Backend C (persistent): capture the old key and verify it is valid
my $psql_c = $node->background_psql('postgres');
$psql_c->query_safe("SET plan_cache_mode = force_generic_plan");
$psql_c->query_safe("PREPARE tap_c1 AS SELECT a FROM tap_t1 WHERE a = \$1");
$psql_c->query_safe("EXECUTE tap_c1(1)");

# Capture the exact old SharedPlanKey as bytea
my $old_key = $psql_c->query_safe("SELECT test_spc_capture_key_for_prep('tap_c1')");
chomp $old_key;

# Verify old key is currently lookup-valid
my $valid_before = $psql_c->query_safe(
    "SELECT test_spc_captured_key_is_valid('$old_key'::bytea)");
chomp $valid_before;

is($valid_before, 't', 'T2a: old key is lookup-valid before DDL');

# Record generation before DDL
my $gen_before_ddl = $psql_c->query_safe("SELECT test_spc_current_generation()");
chomp $gen_before_ddl;

# Backend B: DDL-only, compute_query_id=off.
# Phase A of SharedPlanCacheAttach() attaches dep_hash/plan_hash.
# The relcache callback fires precise dep-index invalidation.
$node->safe_psql('postgres', q{
    SET compute_query_id = off;
    ALTER TABLE tap_t1 ADD COLUMN b text;
});

# Verify global_generation did NOT increase (precise invalidation, not fallback)
my $gen_after_ddl = $psql_c->query_safe("SELECT test_spc_current_generation()");
chomp $gen_after_ddl;

is($gen_after_ddl, $gen_before_ddl,
   'T2b: specific-relid DDL did not bump global_generation (precise invalidation)');

# The exact old key must now be invalid — this checks the SAME key bytes,
# not a recomputed key that may have a different query_tree_hash after DDL.
my $valid_after = $psql_c->query_safe(
    "SELECT test_spc_captured_key_is_valid('$old_key'::bytea)");
chomp $valid_after;

is($valid_after, 'f',
   'T2c: old captured key is invalidated after DDL (proves old entry marked invalid)');

$psql_a->quit;
$psql_c->quit;

# ---------------------------------------------------------------
# T4: Broad invalidation via test helper bumps generation
#
# Must run in a single session: new backends skip pending shared
# invalidation messages (sinvaladt.c sets nextMsgNum = maxMsgNum),
# so a fresh safe_psql would never fire the relcache callback.
# ---------------------------------------------------------------

my $t4_result = $node->safe_psql('postgres', q{
    SELECT test_spc_current_generation();
    SELECT test_spc_force_relcache_invalidation(0);
    SELECT test_spc_current_generation();
});
my @t4_lines = split(/\n/, $t4_result);
my $gen_before = $t4_lines[0];
my $gen_after = $t4_lines[2];

cmp_ok($gen_after, '>', $gen_before,
       'T4: InvalidOid broad invalidation bumps global generation');

# ---------------------------------------------------------------
# T6: Specific relid DDL does NOT bump generation
# ---------------------------------------------------------------

$node->safe_psql('postgres', q{
    CREATE TABLE tap_prec (x int);
    INSERT INTO tap_prec VALUES (1);
    ANALYZE tap_prec;
});

my $gen_pre = $node->safe_psql('postgres', q{
    SELECT test_spc_current_generation();
});

$node->safe_psql('postgres', q{
    ALTER TABLE tap_prec ADD COLUMN y text;
});

my $gen_post = $node->safe_psql('postgres', q{
    SELECT test_spc_current_generation();
});

is($gen_post, $gen_pre,
   'T6: specific relid DDL does not bump global generation');

# Cleanup
$node->safe_psql('postgres', q{
    DROP TABLE IF EXISTS tap_t1, tap_prec;
});

$node->stop;
done_testing();
