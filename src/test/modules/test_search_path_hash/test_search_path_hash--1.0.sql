/* src/test/modules/test_search_path_hash/test_search_path_hash--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_search_path_hash" to load this file. \quit

CREATE FUNCTION test_search_path_hash()
RETURNS int8
AS 'MODULE_PATHNAME', 'test_search_path_hash'
LANGUAGE C STRICT;

CREATE FUNCTION test_search_path_hash_recompute_count()
RETURNS int8
AS 'MODULE_PATHNAME', 'test_search_path_hash_recompute_count'
LANGUAGE C STRICT;
