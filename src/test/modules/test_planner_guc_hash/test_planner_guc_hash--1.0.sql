/* src/test/modules/test_planner_guc_hash/test_planner_guc_hash--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_planner_guc_hash" to load this file. \quit

CREATE FUNCTION test_planner_guc_hash()
RETURNS int8
AS 'MODULE_PATHNAME', 'test_planner_guc_hash'
LANGUAGE C STRICT;

CREATE FUNCTION test_planner_guc_hash_recompute_count()
RETURNS int8
AS 'MODULE_PATHNAME', 'test_planner_guc_hash_recompute_count'
LANGUAGE C STRICT;
