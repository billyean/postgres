/* contrib/pg_buffercache/pg_buffercache--1.7.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_buffercache" to load this file. \quit

-- Register the function.
CREATE FUNCTION pg_buffercache_pages()
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME', 'pg_buffercache_pages'
LANGUAGE C PARALLEL SAFE;

-- Create a view for convenient access.
CREATE VIEW pg_buffercache AS
	SELECT P.* FROM pg_buffercache_pages() AS P
	(bufferid integer, relfilenode oid, reltablespace oid, reldatabase oid,
	 relforknumber int2, relblocknumber int8, isdirty bool, usagecount int2,
	 pinning_backends int4);

-- Don't want these to be available to public.
REVOKE ALL ON FUNCTION pg_buffercache_pages() FROM PUBLIC;
REVOKE ALL ON pg_buffercache FROM PUBLIC;

GRANT EXECUTE ON FUNCTION pg_buffercache_pages() TO pg_monitor;
GRANT SELECT ON pg_buffercache TO pg_monitor;

CREATE FUNCTION pg_buffercache_summary(
    OUT buffers_used int4,
    OUT buffers_unused int4,
    OUT buffers_dirty int4,
    OUT buffers_pinned int4,
    OUT usagecount_avg float8)
AS 'MODULE_PATHNAME', 'pg_buffercache_summary'
LANGUAGE C PARALLEL SAFE;

CREATE FUNCTION pg_buffercache_usage_counts(
    OUT usage_count int4,
    OUT buffers int4,
    OUT dirty int4,
    OUT pinned int4)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_buffercache_usage_counts'
LANGUAGE C PARALLEL SAFE;

REVOKE ALL ON FUNCTION pg_buffercache_summary() FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_buffercache_summary() TO pg_monitor;
REVOKE ALL ON FUNCTION pg_buffercache_usage_counts() FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_buffercache_usage_counts() TO pg_monitor;

CREATE FUNCTION pg_buffercache_evict(
    IN int,
    OUT buffer_evicted boolean,
    OUT buffer_flushed boolean)
AS 'MODULE_PATHNAME', 'pg_buffercache_evict'
LANGUAGE C PARALLEL SAFE VOLATILE STRICT;

CREATE FUNCTION pg_buffercache_evict_relation(
    IN regclass,
    OUT buffers_evicted int4,
    OUT buffers_flushed int4,
    OUT buffers_skipped int4)
AS 'MODULE_PATHNAME', 'pg_buffercache_evict_relation'
LANGUAGE C PARALLEL SAFE VOLATILE STRICT;

CREATE FUNCTION pg_buffercache_evict_all(
    OUT buffers_evicted int4,
    OUT buffers_flushed int4,
    OUT buffers_skipped int4)
AS 'MODULE_PATHNAME', 'pg_buffercache_evict_all'
LANGUAGE C PARALLEL SAFE VOLATILE;

CREATE OR REPLACE FUNCTION pg_buffercache_numa_pages()
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME', 'pg_buffercache_numa_pages'
LANGUAGE C PARALLEL SAFE;

-- Function to retrieve information about OS pages, with optional NUMA
-- information.
CREATE FUNCTION pg_buffercache_os_pages(IN include_numa boolean,
    OUT bufferid integer,
    OUT os_page_num bigint,
    OUT numa_node integer)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_buffercache_os_pages'
LANGUAGE C PARALLEL SAFE;

-- View for OS page information, without NUMA.
CREATE VIEW pg_buffercache_os_pages AS
    SELECT bufferid, os_page_num
    FROM pg_buffercache_os_pages(false);

-- View for OS page information, with NUMA.
CREATE VIEW pg_buffercache_numa AS
    SELECT bufferid, os_page_num, numa_node
    FROM pg_buffercache_os_pages(true);

REVOKE ALL ON FUNCTION pg_buffercache_numa_pages() FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_buffercache_os_pages(boolean) FROM PUBLIC;
REVOKE ALL ON pg_buffercache_os_pages FROM PUBLIC;
REVOKE ALL ON pg_buffercache_numa FROM PUBLIC;

GRANT EXECUTE ON FUNCTION pg_buffercache_numa_pages() TO pg_monitor;
GRANT EXECUTE ON FUNCTION pg_buffercache_os_pages(boolean) TO pg_monitor;
GRANT SELECT ON pg_buffercache_os_pages TO pg_monitor;
GRANT SELECT ON pg_buffercache_numa TO pg_monitor;

-- Functions to mark buffers as dirty.
CREATE FUNCTION pg_buffercache_mark_dirty(
    IN int,
    OUT buffer_dirtied boolean,
    OUT buffer_already_dirty boolean)
AS 'MODULE_PATHNAME', 'pg_buffercache_mark_dirty'
LANGUAGE C PARALLEL SAFE VOLATILE STRICT;

CREATE FUNCTION pg_buffercache_mark_dirty_relation(
    IN regclass,
    OUT buffers_dirtied int4,
    OUT buffers_already_dirty int4,
    OUT buffers_skipped int4)
AS 'MODULE_PATHNAME', 'pg_buffercache_mark_dirty_relation'
LANGUAGE C PARALLEL SAFE VOLATILE STRICT;

CREATE FUNCTION pg_buffercache_mark_dirty_all(
    OUT buffers_dirtied int4,
    OUT buffers_already_dirty int4,
    OUT buffers_skipped int4)
AS 'MODULE_PATHNAME', 'pg_buffercache_mark_dirty_all'
LANGUAGE C PARALLEL SAFE VOLATILE;

-- Eviction instrumentation (Patch 4) with dispatch mode override (Patch 5).
CREATE FUNCTION pg_buffercache_eviction_stats(
    OUT requested_mode text,
    OUT dispatch_path text,
    OUT dispatch_chunk_size int4,
    OUT chunks_scanned int8,
    OUT segments_scanned int8,
    OUT scan_calls int8,
    OUT decrement_calls int8,
    OUT candidates_examined int8,
    OUT rejected_refcount int8,
    OUT rejected_locked int8,
    OUT cas_failures int8,
    OUT victims_found int8,
    OUT decrement_progress int8,
    OUT decrement_noprogress int8,
    OUT trycounter_resets int8)
AS 'MODULE_PATHNAME', 'pg_buffercache_eviction_stats'
LANGUAGE C PARALLEL SAFE VOLATILE;

CREATE FUNCTION pg_buffercache_eviction_stats_reset()
RETURNS void
AS 'MODULE_PATHNAME', 'pg_buffercache_eviction_stats_reset'
LANGUAGE C PARALLEL SAFE VOLATILE;
