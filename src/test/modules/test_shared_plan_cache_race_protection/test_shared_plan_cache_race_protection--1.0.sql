/* src/test/modules/test_shared_plan_cache_race_protection/test_shared_plan_cache_race_protection--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_shared_plan_cache_race_protection" to load this file. \quit

CREATE FUNCTION test_spc_entry_refcount(stmt_name text)
RETURNS integer
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_set_entry_refcount(stmt_name text, value bigint)
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_l2_stale_after_deser_count()
RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_last_l2_status()
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_has_pin()
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_force_nested_pin(stmt_name text)
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_clear_nested_pin()
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_release_pin()
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_arm_race_is_valid(stmt_name text)
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_arm_race_generation(stmt_name text)
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_arm_race_epoch(stmt_name text)
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_arm_race_payload(stmt_name text)
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_entry_refcount_by_key(captured_key bytea)
RETURNS integer
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_race_l2_hit_count()
RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_race_l2_miss_count()
RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_race_l2_store_count()
RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_race_current_entries()
RETURNS integer
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_race_current_generation()
RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_race_current_store_epoch()
RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_race_entry_is_valid_for_prep(stmt_name text)
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_race_force_relcache_invalidation(relid oid)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_hook_fired_count()
RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_arm_deser_error()
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_arm_deser_elog_error()
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_force_real_pin(stmt_name text)
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_capture_key(stmt_name text)
RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_set_entry_refcount_by_key(captured_key bytea, value bigint)
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_force_same_key_overwrite(captured_key bytea)
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
