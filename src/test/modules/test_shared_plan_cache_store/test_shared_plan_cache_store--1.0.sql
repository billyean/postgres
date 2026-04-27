/* src/test/modules/test_shared_plan_cache_store/test_shared_plan_cache_store--1.0.sql */

\echo Use "CREATE EXTENSION test_shared_plan_cache_store" to load this file. \quit

CREATE FUNCTION test_shared_plan_compute_key(stmt_name text)
RETURNS text
AS 'MODULE_PATHNAME', 'test_shared_plan_compute_key'
LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_key_field(stmt_name text, field text)
RETURNS text
AS 'MODULE_PATHNAME', 'test_shared_plan_key_field'
LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_store(stmt_name text)
RETURNS text
AS 'MODULE_PATHNAME', 'test_shared_plan_store'
LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_store_duplicate(stmt_name text)
RETURNS text
AS 'MODULE_PATHNAME', 'test_shared_plan_store_duplicate'
LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_store_count()
RETURNS int
AS 'MODULE_PATHNAME', 'test_shared_plan_store_count'
LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_store_entry_valid(stmt_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'test_shared_plan_store_entry_valid'
LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_store_serialized_size(stmt_name text)
RETURNS int8
AS 'MODULE_PATHNAME', 'test_shared_plan_store_serialized_size'
LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_store_dependency_counts(stmt_name text)
RETURNS text
AS 'MODULE_PATHNAME', 'test_shared_plan_store_dependency_counts'
LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_store_reset()
RETURNS void
AS 'MODULE_PATHNAME', 'test_shared_plan_store_reset'
LANGUAGE C STRICT;
