/* src/test/modules/test_buffered_insert/test_buffered_insert--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_buffered_insert" to load this file. \quit

--
-- Insert rows through the buffered-insert lifecycle with a NULL flush
-- callback (the CTAS/CMV/RMV pattern).  Returns the number of rows put().
--
CREATE FUNCTION test_buffered_insert_basic(pg_catalog.regclass, pg_catalog.int4)
	RETURNS pg_catalog.int4
	AS 'MODULE_PATHNAME' LANGUAGE C;

--
-- Insert rows through the buffered-insert lifecycle with a flush callback
-- that counts invocations.  Returns the total number of flush-callback
-- invocations (should equal the number of rows inserted).
--
CREATE FUNCTION test_buffered_insert_with_callback(pg_catalog.regclass, pg_catalog.int4)
	RETURNS pg_catalog.int4
	AS 'MODULE_PATHNAME' LANGUAGE C;

--
-- Exercise explicit flush() mid-session: inserts half the rows, calls
-- flush(), inserts the other half, then calls end().  Returns the total
-- number of flush-callback invocations (should equal nrows).
--
CREATE FUNCTION test_buffered_insert_flush_mid(pg_catalog.regclass, pg_catalog.int4)
	RETURNS pg_catalog.int4
	AS 'MODULE_PATHNAME' LANGUAGE C;
