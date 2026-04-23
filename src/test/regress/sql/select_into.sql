--
-- SELECT_INTO
--

SELECT *
   INTO TABLE sitmp1
   FROM onek
   WHERE onek.unique1 < 2;

DROP TABLE sitmp1;

SELECT *
   INTO TABLE sitmp1
   FROM onek2
   WHERE onek2.unique1 < 2;

DROP TABLE sitmp1;

--
-- SELECT INTO and INSERT permission, if owner is not allowed to insert.
--
CREATE SCHEMA selinto_schema;
CREATE USER regress_selinto_user;
ALTER DEFAULT PRIVILEGES FOR ROLE regress_selinto_user
	  REVOKE INSERT ON TABLES FROM regress_selinto_user;
GRANT ALL ON SCHEMA selinto_schema TO public;

SET SESSION AUTHORIZATION regress_selinto_user;
-- WITH DATA, passes.
CREATE TABLE selinto_schema.tbl_withdata1 (a)
  AS SELECT generate_series(1,3) WITH DATA;
INSERT INTO selinto_schema.tbl_withdata1 VALUES (4);
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF)
  CREATE TABLE selinto_schema.tbl_withdata2 (a) AS
  SELECT generate_series(1,3) WITH DATA;
-- WITH NO DATA, passes.
CREATE TABLE selinto_schema.tbl_nodata1 (a) AS
  SELECT generate_series(1,3) WITH NO DATA;
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF)
  CREATE TABLE selinto_schema.tbl_nodata2 (a) AS
  SELECT generate_series(1,3) WITH NO DATA;
-- EXECUTE and WITH DATA, passes.
PREPARE data_sel AS SELECT generate_series(1,3);
CREATE TABLE selinto_schema.tbl_withdata3 (a) AS
  EXECUTE data_sel WITH DATA;
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF)
  CREATE TABLE selinto_schema.tbl_withdata4 (a) AS
  EXECUTE data_sel WITH DATA;
-- EXECUTE and WITH NO DATA, passes.
CREATE TABLE selinto_schema.tbl_nodata3 (a) AS
  EXECUTE data_sel WITH NO DATA;
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF, BUFFERS OFF)
  CREATE TABLE selinto_schema.tbl_nodata4 (a) AS
  EXECUTE data_sel WITH NO DATA;
RESET SESSION AUTHORIZATION;

ALTER DEFAULT PRIVILEGES FOR ROLE regress_selinto_user
	  GRANT INSERT ON TABLES TO regress_selinto_user;

SET SESSION AUTHORIZATION regress_selinto_user;
RESET SESSION AUTHORIZATION;

DEALLOCATE data_sel;
DROP SCHEMA selinto_schema CASCADE;
DROP USER regress_selinto_user;

-- Tests for WITH NO DATA and column name consistency
CREATE TABLE ctas_base (i int, j int);
INSERT INTO ctas_base VALUES (1, 2);
CREATE TABLE ctas_nodata (ii, jj, kk) AS SELECT i, j FROM ctas_base; -- Error
CREATE TABLE ctas_nodata (ii, jj, kk) AS SELECT i, j FROM ctas_base WITH NO DATA; -- Error
CREATE TABLE ctas_nodata (ii, jj) AS SELECT i, j FROM ctas_base; -- OK
CREATE TABLE ctas_nodata_2 (ii, jj) AS SELECT i, j FROM ctas_base WITH NO DATA; -- OK
CREATE TABLE ctas_nodata_3 (ii) AS SELECT i, j FROM ctas_base; -- OK
CREATE TABLE ctas_nodata_4 (ii) AS SELECT i, j FROM ctas_base WITH NO DATA; -- OK
SELECT * FROM ctas_nodata;
SELECT * FROM ctas_nodata_2;
SELECT * FROM ctas_nodata_3;
SELECT * FROM ctas_nodata_4;
DROP TABLE ctas_base;
DROP TABLE ctas_nodata;
DROP TABLE ctas_nodata_2;
DROP TABLE ctas_nodata_3;
DROP TABLE ctas_nodata_4;

--
-- CREATE TABLE AS/SELECT INTO as last command in a SQL function
-- have been known to cause problems
--
CREATE FUNCTION make_table() RETURNS VOID
AS $$
  CREATE TABLE created_table AS SELECT * FROM int8_tbl;
$$ LANGUAGE SQL;

SELECT make_table();

SELECT * FROM created_table;

-- Try EXPLAIN ANALYZE SELECT INTO and EXPLAIN ANALYZE CREATE TABLE AS
-- WITH NO DATA, but hide the outputs since they won't be stable.
DO $$
BEGIN
	EXECUTE 'EXPLAIN ANALYZE SELECT * INTO TABLE easi FROM int8_tbl';
	EXECUTE 'EXPLAIN ANALYZE CREATE TABLE easi2 AS SELECT * FROM int8_tbl WITH NO DATA';
END$$;

DROP TABLE created_table;
DROP TABLE easi, easi2;

--
-- Disallowed uses of SELECT ... INTO.  All should fail
--
DECLARE foo CURSOR FOR SELECT 1 INTO int4_tbl;
COPY (SELECT 1 INTO frak UNION SELECT 2) TO 'blob';
SELECT * FROM (SELECT 1 INTO f) bar;
CREATE VIEW foo AS SELECT 1 INTO int4_tbl;
INSERT INTO int4_tbl SELECT 1 INTO f;

-- Test CREATE TABLE AS ... IF NOT EXISTS
CREATE TABLE ctas_ine_tbl AS SELECT 1;
CREATE TABLE ctas_ine_tbl AS SELECT 1 / 0; -- error
CREATE TABLE IF NOT EXISTS ctas_ine_tbl AS SELECT 1 / 0; -- ok
CREATE TABLE ctas_ine_tbl AS SELECT 1 / 0 WITH NO DATA; -- error
CREATE TABLE IF NOT EXISTS ctas_ine_tbl AS SELECT 1 / 0 WITH NO DATA; -- ok
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF)
  CREATE TABLE ctas_ine_tbl AS SELECT 1 / 0; -- error
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF)
  CREATE TABLE IF NOT EXISTS ctas_ine_tbl AS SELECT 1 / 0; -- ok
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF)
  CREATE TABLE ctas_ine_tbl AS SELECT 1 / 0 WITH NO DATA; -- error
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF)
  CREATE TABLE IF NOT EXISTS ctas_ine_tbl AS SELECT 1 / 0 WITH NO DATA; -- ok
PREPARE ctas_ine_query AS SELECT 1 / 0;
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF)
  CREATE TABLE ctas_ine_tbl AS EXECUTE ctas_ine_query; -- error
EXPLAIN (ANALYZE, COSTS OFF, SUMMARY OFF, TIMING OFF)
  CREATE TABLE IF NOT EXISTS ctas_ine_tbl AS EXECUTE ctas_ine_query; -- ok
DROP TABLE ctas_ine_tbl;

--
-- Tests for CTAS with buffered-insert path.
--
-- These tests verify correctness of CTAS results under both the buffered
-- path (no volatile functions) and the fallback single-row path (volatile
-- functions present, or EXECUTE).  Path selection is not directly observable
-- in SQL output; these tests validate that results are correct regardless.
--

-- Buffered path: small row count, verify contents.
CREATE TABLE ctas_buffered_1 AS SELECT g AS a, g * 10 AS b FROM generate_series(1, 5) g;
SELECT count(*) FROM ctas_buffered_1;
SELECT * FROM ctas_buffered_1 ORDER BY a;
DROP TABLE ctas_buffered_1;

-- Buffered path: enough rows to trigger auto-flush (>1000 slot threshold).
CREATE TABLE ctas_buffered_2 AS SELECT g AS a FROM generate_series(1, 2500) g;
SELECT count(*) FROM ctas_buffered_2;
SELECT min(a), max(a) FROM ctas_buffered_2;
DROP TABLE ctas_buffered_2;

-- Buffered path: wide tuples to exercise byte-threshold flushing.
CREATE TABLE ctas_buffered_wide AS
  SELECT g AS id,
         repeat('x', 200) AS col1,
         repeat('y', 200) AS col2,
         repeat('z', 200) AS col3
  FROM generate_series(1, 100) g;
SELECT count(*) FROM ctas_buffered_wide;
SELECT id, length(col1), length(col2), length(col3) FROM ctas_buffered_wide WHERE id IN (1, 50, 100) ORDER BY id;
DROP TABLE ctas_buffered_wide;

-- Fallback path: random() triggers the conservative volatile-function guard.
-- Result must still be correct through the single-row insertion path.
CREATE TABLE ctas_volatile AS SELECT g AS a, random() AS r FROM generate_series(1, 10) g;
SELECT count(*) FROM ctas_volatile;
SELECT a FROM ctas_volatile ORDER BY a;
DROP TABLE ctas_volatile;

-- WITH NO DATA: no insertion path exercised; verify unchanged behavior.
CREATE TABLE ctas_nodata AS SELECT 1 AS a WITH NO DATA;
SELECT count(*) FROM ctas_nodata;
DROP TABLE ctas_nodata;
