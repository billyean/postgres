/* src/test/modules/test_shared_plan_cache_integration/test_shared_plan_cache_integration--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_shared_plan_cache_integration" to load this file. \quit

CREATE FUNCTION test_shared_plan_last_l2_status()
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_l2_hit_count()
RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_l2_miss_count()
RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_l2_store_count()
RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_l2_error_count()
RETURNS bigint
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_generic_cost(stmt_name text)
RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_entry_generic_cost(stmt_name text)
RETURNS float8
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_cache_is_active()
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_cache_is_attached()
RETURNS boolean
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
