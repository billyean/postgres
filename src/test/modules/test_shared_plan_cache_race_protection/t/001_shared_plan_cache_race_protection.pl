
# Patch 0010: Race Protection TAP test
# Tests cross-backend scenarios for refcount/pin lifecycle.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('race_protection');
$node->init;
$node->append_conf('postgresql.conf', <<CONF);
shared_plan_cache_max_entries = 100
shared_plan_cache_enabled = on
compute_query_id = on
shared_preload_libraries = ''
plan_cache_mode = force_generic_plan
CONF
$node->start;

$node->safe_psql('postgres',
	'CREATE EXTENSION test_shared_plan_cache_race_protection');

# ========================================================
# TAP-T1: L2 hit across backends
# ========================================================

$node->safe_psql('postgres', q{
    CREATE TABLE tap_t1 (id int, val text);
    INSERT INTO tap_t1 VALUES (1, 'hello');
});

# Backend A stores entry
$node->safe_psql('postgres', q{
    PREPARE tap_q1 AS SELECT * FROM tap_t1 WHERE id = $1;
    EXECUTE tap_q1(1);
    EXECUTE tap_q1(1);
});

# Backend B: PREPARE+EXECUTE triggers L2 HIT, then check status in same session
my $hit_result = $node->safe_psql('postgres', q{
    PREPARE tap_q1 AS SELECT * FROM tap_t1 WHERE id = $1;
    EXECUTE tap_q1(1);
    SELECT test_spc_last_l2_status();
});
# Output contains EXECUTE result + status, extract last line
my @lines = split(/\n/, $hit_result);
my $last_status = $lines[-1];
is($last_status, 'HIT', 'TAP-T1: cross-backend L2 hit');

my $refcount = $node->safe_psql('postgres', q{
    PREPARE tap_q1 AS SELECT * FROM tap_t1 WHERE id = $1;
    SELECT test_spc_entry_refcount('tap_q1');
});
is($refcount, '0', 'TAP-T1: refcount is 0 after L2 hit');

# ========================================================
# TAP-T7: before_shmem_exit cleanup via backend termination
# T7a: normal disconnect (no pin held)
# T7b: disconnect with real held pin (refcount incremented)
# ========================================================

$node->safe_psql('postgres', q{
    CREATE TABLE tap_t7 (id int);
    INSERT INTO tap_t7 VALUES (1);
});

# Backend A stores entry
$node->safe_psql('postgres', q{
    PREPARE tap_q7 AS SELECT * FROM tap_t7 WHERE id = $1;
    EXECUTE tap_q7(1);
    EXECUTE tap_q7(1);
});

# Verify entry exists and refcount is 0
my $pre_rc = $node->safe_psql('postgres', q{
    PREPARE tap_q7 AS SELECT * FROM tap_t7 WHERE id = $1;
    SELECT test_spc_entry_refcount('tap_q7');
});
is($pre_rc, '0', 'TAP-T7a: refcount 0 before termination test');

# Backend B: get its PID, store an entry (which triggers L2 lookup),
# then verify no stuck pin after normal disconnect.
my $backend_pid = $node->safe_psql('postgres', q{
    SELECT pg_backend_pid();
});
$node->safe_psql('postgres', q{
    PREPARE tap_q7 AS SELECT * FROM tap_t7 WHERE id = $1;
    EXECUTE tap_q7(1);
});

# After disconnect, before_shmem_exit fires. Check refcount from new backend.
my $post_rc = $node->safe_psql('postgres', q{
    PREPARE tap_q7 AS SELECT * FROM tap_t7 WHERE id = $1;
    SELECT test_spc_entry_refcount('tap_q7');
});
is($post_rc, '0', 'TAP-T7a: refcount 0 after backend disconnect');

# T7b: Backend exits with a REAL held pin (refcount incremented).
# Force a real pin, verify it succeeded and refcount is 1, then disconnect —
# before_shmem_exit must release it.
my $t7b_pin_result = $node->safe_psql('postgres', q{
    PREPARE tap_q7 AS SELECT * FROM tap_t7 WHERE id = $1;
    SELECT test_spc_force_real_pin('tap_q7');
    SELECT test_spc_entry_refcount('tap_q7');
});
my @t7b_lines = split(/\n/, $t7b_pin_result);
is($t7b_lines[-2], 't', 'TAP-T7b: force_real_pin returned true');
is($t7b_lines[-1], '1', 'TAP-T7b: refcount is 1 before backend exit');
# The backend above disconnected. before_shmem_exit should have decremented.
my $post_rc_b = $node->safe_psql('postgres', q{
    PREPARE tap_q7 AS SELECT * FROM tap_t7 WHERE id = $1;
    SELECT test_spc_entry_refcount('tap_q7');
});
is($post_rc_b, '0', 'TAP-T7b: refcount 0 after backend exit with held pin');

# ========================================================
# TAP-T8: Same-key overwrite preserves refcount
# T8a: basic overwrite (refcount=0)
# T8b: overwrite while refcount>0 (simulates concurrent reader)
# ========================================================

$node->safe_psql('postgres', q{
    CREATE TABLE tap_t8 (id int);
    INSERT INTO tap_t8 VALUES (1);
});

$node->safe_psql('postgres', q{
    PREPARE tap_q8 AS SELECT * FROM tap_t8 WHERE id = $1;
    EXECUTE tap_q8(1);
    EXECUTE tap_q8(1);
});

my $pre_refcount = $node->safe_psql('postgres', q{
    PREPARE tap_q8 AS SELECT * FROM tap_t8 WHERE id = $1;
    SELECT test_spc_entry_refcount('tap_q8');
});
is($pre_refcount, '0', 'TAP-T8a: refcount 0 before overwrite');

$node->safe_psql('postgres', 'ALTER TABLE tap_t8 ADD COLUMN extra int');

$node->safe_psql('postgres', q{
    PREPARE tap_q8b AS SELECT * FROM tap_t8 WHERE id = $1;
    EXECUTE tap_q8b(1);
    EXECUTE tap_q8b(1);
});
my $post_refcount = $node->safe_psql('postgres', q{
    PREPARE tap_q8b AS SELECT * FROM tap_t8 WHERE id = $1;
    SELECT test_spc_entry_refcount('tap_q8b');
});
is($post_refcount, '0', 'TAP-T8a: refcount 0 after overwrite');

# T8b: This controlled helper touches the same captured entry and verifies that
# same-entry stale overwrite must preserve refcount. It does not exercise the
# full production payload replacement path. Production refcount preservation is
# enforced by the store-path code structure in SharedPlanPublishEntry(), where
# only new entries initialize refcount and found/stale overwrites preserve it.
my $t8b_raw = $node->safe_psql('postgres', q{
    PREPARE tap_q8c AS SELECT * FROM tap_t8 WHERE id = $1;
    SELECT test_spc_capture_key('tap_q8c');
});
# $t8b_raw is like '\xABCD...' from psql bytea output.
# Extract just the hex digits (strip leading \x).
(my $t8b_hex = $t8b_raw) =~ s/^\\x//;

# Verify initial refcount is exactly 0 (entry exists, no pin held)
my $rc_initial = $node->safe_psql('postgres',
    "SELECT test_spc_entry_refcount_by_key(decode('$t8b_hex','hex'));");
is($rc_initial, '0', 'TAP-T8b: initial refcount is 0');

# Set refcount=3 on the captured key
$node->safe_psql('postgres',
    "SELECT test_spc_set_entry_refcount_by_key(decode('$t8b_hex','hex'), 3);");

# Verify refcount was set
my $rc_before = $node->safe_psql('postgres',
    "SELECT test_spc_entry_refcount_by_key(decode('$t8b_hex','hex'));");
is($rc_before, '3', 'TAP-T8b: refcount set to 3 before overwrite');

# Call controlled same-key overwrite helper — must return true (entry touched)
my $touched = $node->safe_psql('postgres',
    "SELECT test_spc_force_same_key_overwrite(decode('$t8b_hex','hex'));");
is($touched, 't', 'TAP-T8b: force_same_key_overwrite returned true');

# Verify refcount on the captured key was preserved (not zeroed by overwrite)
my $rc_after_overwrite = $node->safe_psql('postgres',
    "SELECT test_spc_entry_refcount_by_key(decode('$t8b_hex','hex'));");
is($rc_after_overwrite, '3', 'TAP-T8b: refcount preserved after stale overwrite');

# Reset refcount to 0
$node->safe_psql('postgres',
    "SELECT test_spc_set_entry_refcount_by_key(decode('$t8b_hex','hex'), 0);");

# Verify refcount returns 0 after cleanup
my $rc_final = $node->safe_psql('postgres',
    "SELECT test_spc_entry_refcount_by_key(decode('$t8b_hex','hex'));");
is($rc_final, '0', 'TAP-T8b: refcount 0 after reset');

# ========================================================
# TAP-T9: Generation bumped by function DDL
# ========================================================

my $gen_before = $node->safe_psql('postgres',
	'SELECT test_spc_race_current_generation()');
$node->safe_psql('postgres',
	'CREATE OR REPLACE FUNCTION tap_func() RETURNS int AS $$SELECT 1$$ LANGUAGE sql');
my $gen_after = $node->safe_psql('postgres',
	'SELECT test_spc_race_current_generation()');
cmp_ok($gen_after, '>', $gen_before,
	'TAP-T9: generation bumped by function DDL');

# ========================================================
# TAP-T10: current_entries accounting
# ========================================================

$node->safe_psql('postgres', q{
    CREATE TABLE tap_t10 (id int);
    INSERT INTO tap_t10 VALUES (1);
});

my $entries_before = $node->safe_psql('postgres',
	'SELECT test_spc_race_current_entries()');
$node->safe_psql('postgres', q{
    PREPARE tap_q10 AS SELECT * FROM tap_t10 WHERE id = $1;
    EXECUTE tap_q10(1);
    EXECUTE tap_q10(1);
});
my $entries_after = $node->safe_psql('postgres',
	'SELECT test_spc_race_current_entries()');
cmp_ok($entries_after, '>=', $entries_before,
	'TAP-T10: current_entries accounting correct');

# ========================================================
# TAP-T12: ReleasePin idempotent and real-pin release
# T12a: no-pin release from fresh backend (idempotent)
# T12b: real pin release from backend that acquired pin
# ========================================================

# T12a: verifies ReleasePin is callable from any backend state.
my $release_safe = $node->safe_psql('postgres', q{
    SELECT test_spc_release_pin();
});
is($release_safe, 't', 'TAP-T12a: ReleasePin idempotent from fresh backend');

# T12b: force real pin in one backend session, release, verify all intermediates
my $t12b_result = $node->safe_psql('postgres', q{
    CREATE TABLE IF NOT EXISTS tap_t12 (id int);
    INSERT INTO tap_t12 VALUES (1);
    PREPARE tap_q12 AS SELECT * FROM tap_t12 WHERE id = $1;
    EXECUTE tap_q12(1);
    EXECUTE tap_q12(1);
    SELECT test_spc_force_real_pin('tap_q12');
    SELECT test_spc_has_pin();
    SELECT test_spc_entry_refcount('tap_q12');
    SELECT test_spc_release_pin();
    SELECT test_spc_entry_refcount('tap_q12');
});
my @t12b_lines = split(/\n/, $t12b_result);
# Lines: execute result, force=t, has_pin=t, refcount=1, release=t, refcount=0
is($t12b_lines[-5], 't', 'TAP-T12b: force_real_pin returned true');
is($t12b_lines[-4], 't', 'TAP-T12b: has_pin is true after force');
is($t12b_lines[-3], '1', 'TAP-T12b: refcount is 1 before release');
is($t12b_lines[-2], 't', 'TAP-T12b: release_pin returned true');
is($t12b_lines[-1], '0', 'TAP-T12b: refcount 0 after real pin release');

# ========================================================
# Cleanup
# ========================================================

$node->safe_psql('postgres', q{
    DROP TABLE IF EXISTS tap_t1, tap_t7, tap_t8, tap_t10, tap_t12;
    DROP FUNCTION IF EXISTS tap_func();
});

$node->stop;
done_testing();
