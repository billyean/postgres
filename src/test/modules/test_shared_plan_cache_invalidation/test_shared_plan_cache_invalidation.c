/*-------------------------------------------------------------------------
 *
 * test_shared_plan_cache_invalidation.c
 *	  Test code for shared plan cache relation invalidation (Patch 0008).
 *
 * Provides SQL-callable functions for regression testing of:
 * - relation dependency tracking via reverse dep-index
 * - relcache invalidation callback behavior
 * - generation / epoch counter behavior
 * - partition pruning rejection
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/test/modules/test_shared_plan_cache_invalidation/test_shared_plan_cache_invalidation.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/prepare.h"
#include "fmgr.h"
#include "lib/dshash.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/plancache.h"
#include "utils/shared_plancache.h"
#include "varatt.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_spc_entry_is_valid_for_prep);
PG_FUNCTION_INFO_V1(test_spc_dep_key_count);
PG_FUNCTION_INFO_V1(test_spc_current_generation);
PG_FUNCTION_INFO_V1(test_spc_current_store_epoch);
PG_FUNCTION_INFO_V1(test_spc_l2_hit_count);
PG_FUNCTION_INFO_V1(test_spc_l2_miss_count);
PG_FUNCTION_INFO_V1(test_spc_l2_store_count);
PG_FUNCTION_INFO_V1(test_spc_entry_num_rel_oids);
PG_FUNCTION_INFO_V1(test_spc_force_relcache_invalidation);
PG_FUNCTION_INFO_V1(test_spc_current_entries);
PG_FUNCTION_INFO_V1(test_spc_capture_key_for_prep);
PG_FUNCTION_INFO_V1(test_spc_captured_key_is_valid);

/*
 * test_spc_entry_is_valid_for_prep - check if L2 entry for a prepared stmt
 * would be considered valid by lookup (is_valid=1 AND generation current).
 * Computes SharedPlanKey via the same internal path as lookup.
 */
Datum
test_spc_entry_is_valid_for_prep(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	PreparedStatement *pstmt;
	CachedPlanSource *plansource;
	SharedPlanKey key;
	SharedPlanRejectReason reject;
	SharedPlanEntry *entry;
	bool		result = false;
	dshash_table *hash;

	pstmt = FetchPreparedStatement(stmt_name, true);
	plansource = pstmt->plansource;

	if (!SharedPlanCacheIsAttached())
		PG_RETURN_BOOL(false);

	if (plansource->query_list == NIL)
		PG_RETURN_BOOL(false);

	if (!ComputeSharedPlanKeyForLookup(plansource, &key, &reject))
		PG_RETURN_BOOL(false);

	hash = SharedPlanCacheGetHash();
	if (hash == NULL)
		PG_RETURN_BOOL(false);

	entry = dshash_find(hash, &key, false);
	if (entry == NULL)
		PG_RETURN_BOOL(false);

	/* Match lookup semantics: is_valid AND generation current */
	result = (pg_atomic_read_u32(&entry->is_valid) == 1 &&
			  entry->generation == SharedPlanCacheGeneration());
	dshash_release_lock(hash, entry);

	PG_RETURN_BOOL(result);
}

/*
 * test_spc_dep_key_count - count dep-index keys for a relation OID.
 */
Datum
test_spc_dep_key_count(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	dshash_table *dep_hash;
	SharedPlanDepEntry *dep_entry;
	int32		count = 0;

	dep_hash = SharedPlanCacheGetDepHash();
	if (dep_hash == NULL)
		PG_RETURN_INT32(0);

	dep_entry = dshash_find(dep_hash, &relid, false);
	if (dep_entry == NULL)
		PG_RETURN_INT32(0);

	count = dep_entry->num_entries;
	dshash_release_lock(dep_hash, dep_entry);

	PG_RETURN_INT32(count);
}

/*
 * test_spc_current_generation - read global generation counter.
 */
Datum
test_spc_current_generation(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) SharedPlanCacheGeneration());
}

/*
 * test_spc_current_store_epoch - read relcache_store_epoch counter.
 */
Datum
test_spc_current_store_epoch(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) SharedPlanCacheRelcacheStoreEpoch());
}

Datum
test_spc_l2_hit_count(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(SharedPlanCacheL2HitCount());
}

Datum
test_spc_l2_miss_count(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(SharedPlanCacheL2MissCount());
}

Datum
test_spc_l2_store_count(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(SharedPlanCacheL2StoreCount());
}

/*
 * test_spc_entry_num_rel_oids - return num_relation_oids from shared entry.
 */
Datum
test_spc_entry_num_rel_oids(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	PreparedStatement *pstmt;
	CachedPlanSource *plansource;
	SharedPlanKey key;
	SharedPlanRejectReason reject;
	SharedPlanEntry *entry;
	int32		result = -1;
	dshash_table *hash;

	pstmt = FetchPreparedStatement(stmt_name, true);
	plansource = pstmt->plansource;

	if (!SharedPlanCacheIsActive())
		PG_RETURN_INT32(-1);

	if (plansource->query_list == NIL)
		PG_RETURN_INT32(-1);

	if (!ComputeSharedPlanKeyForLookup(plansource, &key, &reject))
		PG_RETURN_INT32(-1);

	hash = SharedPlanCacheGetHash();
	if (hash == NULL)
		PG_RETURN_INT32(-1);

	entry = dshash_find(hash, &key, false);
	if (entry == NULL)
		PG_RETURN_INT32(-1);

	result = entry->num_relation_oids;
	dshash_release_lock(hash, entry);

	PG_RETURN_INT32(result);
}

/*
 * test_spc_force_relcache_invalidation - invoke the relcache invalidation
 * path for testing.  Pass 0 for InvalidOid (broad invalidation).
 */
Datum
test_spc_force_relcache_invalidation(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);

	/*
	 * CacheInvalidateRelcacheByRelid fires the relcache invalidation callbacks
	 * in this backend.  For InvalidOid we use CacheInvalidateRelcacheAll().
	 */
	if (relid == InvalidOid)
		CacheInvalidateRelcacheAll();
	else
		CacheInvalidateRelcacheByRelid(relid);

	PG_RETURN_VOID();
}

/*
 * test_spc_current_entries - read current_entries counter.
 */
Datum
test_spc_current_entries(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(SharedPlanCacheCurrentEntries());
}

/*
 * test_spc_capture_key_for_prep - capture the current SharedPlanKey for a
 * prepared statement as a bytea value.  This key can later be checked with
 * test_spc_captured_key_is_valid() even after DDL changes the query tree,
 * which would produce a different key if recomputed.
 */
Datum
test_spc_capture_key_for_prep(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	PreparedStatement *pstmt;
	CachedPlanSource *plansource;
	SharedPlanKey key;
	SharedPlanRejectReason reject;
	bytea	   *result;

	pstmt = FetchPreparedStatement(stmt_name, true);
	plansource = pstmt->plansource;

	if (plansource->query_list == NIL)
		ereport(ERROR, (errmsg("prepared statement has no query list")));

	if (!ComputeSharedPlanKeyForLookup(plansource, &key, &reject))
		ereport(ERROR, (errmsg("cannot compute shared plan key")));

	result = (bytea *) palloc(VARHDRSZ + sizeof(SharedPlanKey));
	SET_VARSIZE(result, VARHDRSZ + sizeof(SharedPlanKey));
	memcpy(VARDATA(result), &key, sizeof(SharedPlanKey));

	PG_RETURN_BYTEA_P(result);
}

/*
 * test_spc_captured_key_is_valid - check if a previously captured
 * SharedPlanKey's L2 entry is lookup-valid (is_valid=1 AND generation current).
 * This does NOT recompute the key — it uses the exact bytes from capture time.
 */
Datum
test_spc_captured_key_is_valid(PG_FUNCTION_ARGS)
{
	bytea	   *key_bytes = PG_GETARG_BYTEA_PP(0);
	SharedPlanKey key;
	SharedPlanEntry *entry;
	bool		result = false;
	dshash_table *hash;

	if (VARSIZE_ANY_EXHDR(key_bytes) != sizeof(SharedPlanKey))
		ereport(ERROR, (errmsg("invalid key size")));

	memcpy(&key, VARDATA_ANY(key_bytes), sizeof(SharedPlanKey));

	hash = SharedPlanCacheGetHash();
	if (hash == NULL)
		PG_RETURN_BOOL(false);

	entry = dshash_find(hash, &key, false);
	if (entry == NULL)
		PG_RETURN_BOOL(false);

	result = (pg_atomic_read_u32(&entry->is_valid) == 1 &&
			  entry->generation == SharedPlanCacheGeneration());
	dshash_release_lock(hash, entry);

	PG_RETURN_BOOL(result);
}
