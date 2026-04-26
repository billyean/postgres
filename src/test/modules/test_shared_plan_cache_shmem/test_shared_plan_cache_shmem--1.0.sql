/* src/test/modules/test_shared_plan_cache_shmem/test_shared_plan_cache_shmem--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_shared_plan_cache_shmem" to load this file. \quit

CREATE FUNCTION test_shared_plan_cache_attached()
RETURNS boolean
AS 'MODULE_PATHNAME', 'test_shared_plan_cache_attached'
LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_cache_active()
RETURNS boolean
AS 'MODULE_PATHNAME', 'test_shared_plan_cache_active'
LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_cache_handles_valid()
RETURNS boolean
AS 'MODULE_PATHNAME', 'test_shared_plan_cache_handles_valid'
LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_cache_current_entries()
RETURNS int4
AS 'MODULE_PATHNAME', 'test_shared_plan_cache_current_entries'
LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_cache_force_attach()
RETURNS boolean
AS 'MODULE_PATHNAME', 'test_shared_plan_cache_force_attach'
LANGUAGE C STRICT;
