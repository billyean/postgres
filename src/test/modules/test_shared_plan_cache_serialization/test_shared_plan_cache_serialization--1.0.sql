/* src/test/modules/test_shared_plan_cache_serialization/test_shared_plan_cache_serialization--1.0.sql */

\echo Use "CREATE EXTENSION test_shared_plan_cache_serialization" to load this file. \quit

CREATE FUNCTION test_shared_plan_roundtrip(stmt_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'test_shared_plan_roundtrip'
LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_serialized_size(stmt_name text)
RETURNS int8
AS 'MODULE_PATHNAME', 'test_shared_plan_serialized_size'
LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_serialize_oversize(stmt_name text, max_kb int)
RETURNS text
AS 'MODULE_PATHNAME', 'test_shared_plan_serialize_oversize'
LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_serialize_cached_plan(stmt_name text)
RETURNS text
AS 'MODULE_PATHNAME', 'test_shared_plan_serialize_cached_plan'
LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_deserialize_corrupt(test_case text)
RETURNS text
AS 'MODULE_PATHNAME', 'test_shared_plan_deserialize_corrupt'
LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_serialize_timing(stmt_name text, iterations int)
RETURNS TABLE(serialize_us float8, deserialize_us float8, serialized_bytes int8)
AS 'MODULE_PATHNAME', 'test_shared_plan_serialize_timing'
LANGUAGE C STRICT;

CREATE FUNCTION test_shared_plan_roundtrip_dsa(stmt_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'test_shared_plan_roundtrip_dsa'
LANGUAGE C STRICT;
