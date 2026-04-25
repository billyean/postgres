/* src/test/modules/test_plan_shareability/test_plan_shareability--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_plan_shareability" to load this file. \quit

CREATE FUNCTION test_plan_shareability(stmt_name text)
RETURNS text
AS 'MODULE_PATHNAME', 'test_plan_shareability'
LANGUAGE C STRICT;

CREATE FUNCTION test_plan_shareability_force_reject(stmt_name text,
                                                    reject_reason text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'test_plan_shareability_force_reject'
LANGUAGE C STRICT;
