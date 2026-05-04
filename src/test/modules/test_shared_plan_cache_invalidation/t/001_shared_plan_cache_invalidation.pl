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

# ---------------------------------------------------------------
# T10_gen: Cross-backend function DDL bumps generation
#
# Backend A (background_psql) stores an L2 entry referencing a
# function, captures the old key, then Backend B (safe_psql with
# compute_query_id=off) performs CREATE OR REPLACE FUNCTION.
# Backend A verifies the old captured key is now generation-stale.
#
# All validity checks run in the persistent Backend A session so
# shared invalidation messages are processed within the same backend.
# ---------------------------------------------------------------

$node->safe_psql('postgres', q{
    CREATE FUNCTION tap_gen_f1() RETURNS int AS 'SELECT 42' LANGUAGE SQL IMMUTABLE;
    CREATE TABLE tap_gen_t1 (a int);
    INSERT INTO tap_gen_t1 VALUES (1);
    ANALYZE tap_gen_t1;
});

my $psql_gen_a = $node->background_psql('postgres');
$psql_gen_a->query_safe("SET plan_cache_mode = force_generic_plan");
$psql_gen_a->query_safe("PREPARE tap_gen_s1 AS SELECT a, tap_gen_f1() FROM tap_gen_t1 WHERE a = \$1");
$psql_gen_a->query_safe("EXECUTE tap_gen_s1(1)");

# Capture old key
my $gen_old_key = $psql_gen_a->query_safe("SELECT test_spc_capture_key_for_prep('tap_gen_s1')");
chomp $gen_old_key;

# Verify old key is valid before function change
my $gen_valid_before = $psql_gen_a->query_safe(
    "SELECT test_spc_captured_key_is_valid('$gen_old_key'::bytea)");
chomp $gen_valid_before;

is($gen_valid_before, 't', 'T10_gen_a: old key is lookup-valid before function DDL');

# Record generation before DDL
my $gen_before_func = $psql_gen_a->query_safe("SELECT test_spc_current_generation()");
chomp $gen_before_func;

# Backend B: DDL-only, compute_query_id=off
$node->safe_psql('postgres', q{
    SET compute_query_id = off;
    CREATE OR REPLACE FUNCTION tap_gen_f1() RETURNS int AS 'SELECT 99' LANGUAGE SQL IMMUTABLE;
});

# Backend A: verify generation bumped
my $gen_after_func = $psql_gen_a->query_safe("SELECT test_spc_current_generation()");
chomp $gen_after_func;

cmp_ok($gen_after_func, '>', $gen_before_func,
       'T10_gen_b: function DDL from compute_query_id=off backend bumped generation');

# Backend A: verify old key is now generation-stale
my $gen_valid_after = $psql_gen_a->query_safe(
    "SELECT test_spc_captured_key_is_valid('$gen_old_key'::bytea)");
chomp $gen_valid_after;

is($gen_valid_after, 'f',
   'T10_gen_c: old captured key is generation-stale after function DDL');

$psql_gen_a->quit;

# ---------------------------------------------------------------
# T_full: Full-cache stale overwrite succeeds without FULL error
#
# Uses a separate cluster with max_entries=2 to force full-cache
# condition.  Stores two entries to fill cache, bumps generation,
# then re-stores the same key.  Must succeed (stale overwrite
# reuses existing slot).
# ---------------------------------------------------------------

my $node2 = PostgreSQL::Test::Cluster->new('spc_full');
$node2->init;
$node2->append_conf('postgresql.conf', <<CONF);
shared_plan_cache_max_entries = 3
compute_query_id = on
shared_plan_cache_enabled = on
plan_cache_mode = force_generic_plan
shared_preload_libraries = ''
CONF
$node2->start;

$node2->safe_psql('postgres', q{
    CREATE EXTENSION test_shared_plan_cache_invalidation;
    CREATE TABLE ft1 (a int); INSERT INTO ft1 VALUES (1); ANALYZE ft1;
    CREATE TABLE ft2 (a int); INSERT INTO ft2 VALUES (1); ANALYZE ft2;
    CREATE TABLE ft3 (a int); INSERT INTO ft3 VALUES (1); ANALYZE ft3;
});

# Use a single persistent session for all cache operations
my $psql_full = $node2->background_psql('postgres');

$psql_full->query_safe("PREPARE fs1 AS SELECT a FROM ft1 WHERE a = \$1");
$psql_full->query_safe("EXECUTE fs1(1)");
$psql_full->query_safe("PREPARE fs2 AS SELECT a FROM ft2 WHERE a = \$1");
$psql_full->query_safe("EXECUTE fs2(1)");
$psql_full->query_safe("PREPARE fs3 AS SELECT a FROM ft3 WHERE a = \$1");
$psql_full->query_safe("EXECUTE fs3(1)");

my $entries_filled = $psql_full->query_safe("SELECT test_spc_current_entries()");
chomp $entries_filled;

# Bump generation so all entries are stale
$psql_full->query_safe("SELECT test_spc_force_syscache_invalidation('PROCOID')");

# Re-store same key while cache is full — must succeed via stale overwrite
$psql_full->query_safe("DEALLOCATE fs1");
$psql_full->query_safe("PREPARE fs1 AS SELECT a FROM ft1 WHERE a = \$1");
$psql_full->query_safe("EXECUTE fs1(1)");

my $full_valid_after = $psql_full->query_safe("SELECT test_spc_entry_is_valid_for_prep('fs1')");
chomp $full_valid_after;
my $full_entries_after = $psql_full->query_safe("SELECT test_spc_current_entries()");
chomp $full_entries_after;

$psql_full->quit;

is($entries_filled, '3', 'T_full_a: cache filled to max_entries=3');
is($full_valid_after, 't', 'T_full_b: stale overwrite succeeded — entry valid');
is($full_entries_after, $entries_filled,
   'T_full_c: current_entries unchanged after stale overwrite');

$node2->safe_psql('postgres', q{ DROP TABLE ft1, ft2, ft3; });
$node2->stop;

# Cleanup
$node->safe_psql('postgres', q{
    DROP TABLE IF EXISTS tap_t1, tap_prec, tap_gen_t1;
    DROP FUNCTION IF EXISTS tap_gen_f1();
});

$node->stop;
done_testing();
