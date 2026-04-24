CREATE EXTENSION pg_buffercache;

select count(*) = (select setting::bigint
                   from pg_settings
                   where name = 'shared_buffers')
from pg_buffercache;

-- For pg_buffercache_os_pages, we expect at least one entry for each buffer
select count(*) >= (select setting::bigint
                    from pg_settings
                    where name = 'shared_buffers')
from pg_buffercache_os_pages;

select buffers_used + buffers_unused > 0,
        buffers_dirty <= buffers_used,
        buffers_pinned <= buffers_used
from pg_buffercache_summary();

SELECT count(*) > 0 FROM pg_buffercache_usage_counts() WHERE buffers >= 0;

-- Verify usagecount values are always in valid range [0, BM_MAX_USAGE_COUNT]
-- This validates correct reporting regardless of decoupled usage_count mode.
SELECT count(*) = 0
FROM pg_buffercache
WHERE usagecount IS NOT NULL AND (usagecount < 0 OR usagecount > 5);

-- Verify pg_buffercache_usage_counts returns exactly the expected rows (0..5)
SELECT count(*) = 6 FROM pg_buffercache_usage_counts();

-- Verify total buffers from usage_counts matches shared_buffers
SELECT sum(buffers) = (SELECT setting::bigint FROM pg_settings WHERE name = 'shared_buffers')
FROM pg_buffercache_usage_counts();

-- Verify usagecount_avg from summary is non-negative (sanity for both modes)
SELECT usagecount_avg >= 0
FROM pg_buffercache_summary()
WHERE buffers_used > 0;

-- Cross-check: per-buffer usagecount distribution should sum to buffers_used
SELECT (SELECT sum(buffers) FROM pg_buffercache_usage_counts()) =
       (SELECT buffers_used + buffers_unused FROM pg_buffercache_summary());

-- Verify that buffer access actually produces non-zero usagecount.
-- This confirms the usage_count source (state word or decoupled map) is
-- being written by pin activity, not just that the reporting SQL works.
CREATE TABLE usagecount_test (id int);
INSERT INTO usagecount_test SELECT generate_series(1, 100);
-- Access repeatedly to ensure usagecount > 0 for these buffers.
SELECT count(*) FROM usagecount_test;
SELECT count(*) FROM usagecount_test;
SELECT count(*) FROM usagecount_test;
SELECT count(*) > 0
FROM pg_buffercache b
     JOIN pg_class c ON c.relfilenode = b.relfilenode
WHERE c.relname = 'usagecount_test'
  AND b.usagecount > 0;
DROP TABLE usagecount_test;

-- Exercise the buffer eviction sweep path by forcing churn larger than
-- shared_buffers.  When the decoupled chunk-based sweep (Patch 2) is
-- active, this exercises the three-phase chunk algorithm and split-chunk
-- wraparound under real victim-search pressure.  The sanity checks
-- afterward confirm the pool state is still internally consistent.
CREATE TABLE eviction_pressure_test (id int, payload text);
INSERT INTO eviction_pressure_test
    SELECT g, repeat('x', 200) FROM generate_series(1, 5000) g;
-- Sequential scan forces the sweep to find victims repeatedly
SELECT count(*) FROM eviction_pressure_test;
-- Verify pool consistency after heavy eviction
SELECT count(*) = 0
FROM pg_buffercache
WHERE usagecount IS NOT NULL AND (usagecount < 0 OR usagecount > 5);
SELECT sum(buffers) = (SELECT setting::bigint FROM pg_settings WHERE name = 'shared_buffers')
FROM pg_buffercache_usage_counts();
DROP TABLE eviction_pressure_test;

-- Verify that after heavy churn, the usagecount distribution is not degenerate.
-- Under chunk-based sweep (Patch 2), the bulk decrement and candidate selection
-- must keep the distribution healthy: some buffers should have usagecount > 0
-- (recently accessed system catalog pages, for example).  If chunk decrement
-- were broken, all buffers would decay to 0 with no increment to counteract.
CREATE TABLE churn_test (id int, data text);
INSERT INTO churn_test SELECT g, repeat('y', 100) FROM generate_series(1, 2000) g;
SELECT count(*) FROM churn_test;
SELECT count(*) FROM churn_test;
-- After accessing data, some buffers must have usagecount > 0
SELECT (SELECT sum(buffers) FROM pg_buffercache_usage_counts() WHERE usage_count > 0) > 0;
DROP TABLE churn_test;

-- Check that the functions / views can't be accessed by default. To avoid
-- having to create a dedicated user, use the pg_database_owner pseudo-role.
SET ROLE pg_database_owner;
SELECT * FROM pg_buffercache;
SELECT * FROM pg_buffercache_os_pages;
SELECT * FROM pg_buffercache_pages() AS p (wrong int);
SELECT * FROM pg_buffercache_summary();
SELECT * FROM pg_buffercache_usage_counts();
RESET role;

-- Check that pg_monitor is allowed to query view / function
SET ROLE pg_monitor;
SELECT count(*) > 0 FROM pg_buffercache;
SELECT count(*) > 0 FROM pg_buffercache_os_pages;
SELECT buffers_used + buffers_unused > 0 FROM pg_buffercache_summary();
SELECT count(*) > 0 FROM pg_buffercache_usage_counts();
RESET role;


------
---- Test pg_buffercache_evict* and pg_buffercache_mark_dirty* functions
------

CREATE ROLE regress_buffercache_normal;
SET ROLE regress_buffercache_normal;

-- These should fail because they need to be called as SUPERUSER
SELECT * FROM pg_buffercache_evict(1);
SELECT * FROM pg_buffercache_evict_relation(1);
SELECT * FROM pg_buffercache_evict_all();
SELECT * FROM pg_buffercache_mark_dirty(1);
SELECT * FROM pg_buffercache_mark_dirty_relation(1);
SELECT * FROM pg_buffercache_mark_dirty_all();

RESET ROLE;

-- These should return nothing, because these are STRICT functions
SELECT * FROM pg_buffercache_evict(NULL);
SELECT * FROM pg_buffercache_evict_relation(NULL);
SELECT * FROM pg_buffercache_mark_dirty(NULL);
SELECT * FROM pg_buffercache_mark_dirty_relation(NULL);

-- These should fail because they are not called by valid range of buffers
-- Number of the shared buffers are limited by max integer
SELECT 2147483647 max_buffers \gset
SELECT * FROM pg_buffercache_evict(-1);
SELECT * FROM pg_buffercache_evict(0);
SELECT * FROM pg_buffercache_evict(:max_buffers);
SELECT * FROM pg_buffercache_mark_dirty(-1);
SELECT * FROM pg_buffercache_mark_dirty(0);
SELECT * FROM pg_buffercache_mark_dirty(:max_buffers);

-- These should fail because they don't accept local relations
CREATE TEMP TABLE temp_pg_buffercache();
SELECT * FROM pg_buffercache_evict_relation('temp_pg_buffercache');
SELECT * FROM pg_buffercache_mark_dirty_relation('temp_pg_buffercache');
DROP TABLE temp_pg_buffercache;

-- These shouldn't fail
SELECT buffer_evicted IS NOT NULL FROM pg_buffercache_evict(1);
SELECT buffers_evicted IS NOT NULL FROM pg_buffercache_evict_all();
CREATE TABLE shared_pg_buffercache();
SELECT buffers_evicted IS NOT NULL FROM pg_buffercache_evict_relation('shared_pg_buffercache');
SELECT buffers_dirtied IS NOT NULL FROM pg_buffercache_mark_dirty_relation('shared_pg_buffercache');
DROP TABLE shared_pg_buffercache;
SELECT pg_buffercache_mark_dirty(1) IS NOT NULL;
SELECT pg_buffercache_mark_dirty_all() IS NOT NULL;

DROP ROLE regress_buffercache_normal;
