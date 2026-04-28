/*-------------------------------------------------------------------------
 *
 * test_shared_plan_cache_integration.c
 *	  Test code for shared plan cache L2 integration (Patch 0007).
 *
 * Provides SQL-callable functions that expose backend-local L2 counters
 * and plansource/entry generic_cost values for regression testing.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/test/modules/test_shared_plan_cache_integration/test_shared_plan_cache_integration.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/prepare.h"
#include "fmgr.h"
#include "lib/dshash.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/plancache.h"
#include "utils/shared_plancache.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_shared_plan_last_l2_status);
PG_FUNCTION_INFO_V1(test_shared_plan_l2_hit_count);
PG_FUNCTION_INFO_V1(test_shared_plan_l2_miss_count);
PG_FUNCTION_INFO_V1(test_shared_plan_l2_store_count);
PG_FUNCTION_INFO_V1(test_shared_plan_l2_error_count);
PG_FUNCTION_INFO_V1(test_shared_plan_generic_cost);
PG_FUNCTION_INFO_V1(test_shared_plan_entry_generic_cost);
PG_FUNCTION_INFO_V1(test_shared_plan_cache_is_active);
PG_FUNCTION_INFO_V1(test_shared_plan_cache_is_attached);

Datum
test_shared_plan_last_l2_status(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(SharedPlanCacheLastL2StatusName()));
}

Datum
test_shared_plan_l2_hit_count(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(SharedPlanCacheL2HitCount());
}

Datum
test_shared_plan_l2_miss_count(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(SharedPlanCacheL2MissCount());
}

Datum
test_shared_plan_l2_store_count(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(SharedPlanCacheL2StoreCount());
}

Datum
test_shared_plan_l2_error_count(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(SharedPlanCacheL2ErrorCount());
}

Datum
test_shared_plan_generic_cost(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	PreparedStatement *pstmt;
	CachedPlanSource *plansource;

	pstmt = FetchPreparedStatement(stmt_name, true);
	plansource = pstmt->plansource;

	PG_RETURN_FLOAT8(plansource->generic_cost);
}

Datum
test_shared_plan_entry_generic_cost(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	PreparedStatement *pstmt;
	CachedPlanSource *plansource;
	SharedPlanKey key;
	SharedPlanRejectReason reject;
	SharedPlanEntry *entry;
	double		cost = -1;
	dshash_table *hash;

	pstmt = FetchPreparedStatement(stmt_name, true);
	plansource = pstmt->plansource;

	if (!SharedPlanCacheIsActive())
		PG_RETURN_FLOAT8(-1);

	if (plansource->query_list == NIL)
		PG_RETURN_FLOAT8(-1);

	if (!ComputeSharedPlanKeyForLookup(plansource, &key, &reject))
		PG_RETURN_FLOAT8(-1);

	hash = SharedPlanCacheGetHash();
	if (hash == NULL)
		PG_RETURN_FLOAT8(-1);

	entry = dshash_find(hash, &key, false);
	if (entry == NULL)
		PG_RETURN_FLOAT8(-1);

	PG_TRY();
	{
		if (pg_atomic_read_u32(&entry->is_valid) == 1)
			cost = entry->generic_cost;
	}
	PG_FINALLY();
	{
		dshash_release_lock(hash, entry);
	}
	PG_END_TRY();

	PG_RETURN_FLOAT8(cost);
}

Datum
test_shared_plan_cache_is_active(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(SharedPlanCacheIsActive());
}

Datum
test_shared_plan_cache_is_attached(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(SharedPlanCacheIsAttached());
}
