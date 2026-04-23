CREATE EXTENSION test_buffered_insert;

-- Target table: single integer column.
CREATE TABLE buffered_insert_test (a INT);

-- Basic test: insert 5 rows via buffered-insert with NULL flush callback.
SELECT test_buffered_insert_basic('buffered_insert_test'::regclass, 5);
SELECT count(*) FROM buffered_insert_test;
SELECT a FROM buffered_insert_test ORDER BY a;

TRUNCATE buffered_insert_test;

-- Test with flush callback: insert 5 rows, verify callback count matches.
SELECT test_buffered_insert_with_callback('buffered_insert_test'::regclass, 5);
SELECT count(*) FROM buffered_insert_test;
SELECT a FROM buffered_insert_test ORDER BY a;

TRUNCATE buffered_insert_test;

-- Trigger auto-flush: insert more rows than HEAP_BUFFERED_INSERT_MAX_SLOTS
-- (1000) to verify auto-flush during put() works correctly.
SELECT test_buffered_insert_basic('buffered_insert_test'::regclass, 1500);
SELECT count(*) FROM buffered_insert_test;
-- Spot-check first and last values.
SELECT min(a), max(a) FROM buffered_insert_test;

TRUNCATE buffered_insert_test;

-- Verify flush callback fires for all rows including auto-flushed batches.
SELECT test_buffered_insert_with_callback('buffered_insert_test'::regclass, 1500);
SELECT count(*) FROM buffered_insert_test;

TRUNCATE buffered_insert_test;

-- Test explicit flush() mid-session: flush after first 5 rows, insert 5 more,
-- then end().  Callback count must equal total rows.
SELECT test_buffered_insert_flush_mid('buffered_insert_test'::regclass, 10);
SELECT count(*) FROM buffered_insert_test;
SELECT min(a), max(a) FROM buffered_insert_test;

DROP TABLE buffered_insert_test;
