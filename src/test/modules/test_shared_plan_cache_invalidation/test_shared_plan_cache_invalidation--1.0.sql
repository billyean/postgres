/* src/test/modules/test_shared_plan_cache_invalidation/test_shared_plan_cache_invalidation--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_shared_plan_cache_invalidation" to load this file. \quit

CREATE FUNCTION test_spc_entry_is_valid_for_prep(stmt_name text)
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_dep_key_count(relid regclass)
RETURNS integer
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_current_generation()
RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_current_store_epoch()
RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_l2_hit_count()
RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_l2_miss_count()
RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_l2_store_count()
RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_entry_num_rel_oids(stmt_name text)
RETURNS integer
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_force_relcache_invalidation(relid oid)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_current_entries()
RETURNS integer
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_capture_key_for_prep(stmt_name text)
RETURNS bytea
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_captured_key_is_valid(captured_key bytea)
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_force_syscache_invalidation(name text)
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_spc_arm_generation_bump()
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
