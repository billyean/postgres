/*-------------------------------------------------------------------------
 *
 * test_shared_plan_cache_shmem.c
 *	  Test wrappers for shared plan cache shmem setup.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_shared_plan_cache_shmem/test_shared_plan_cache_shmem.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "utils/shared_plancache.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_shared_plan_cache_attached);
Datum
test_shared_plan_cache_attached(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(SharedPlanCacheIsAttached());
}

PG_FUNCTION_INFO_V1(test_shared_plan_cache_active);
Datum
test_shared_plan_cache_active(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(SharedPlanCacheIsActive());
}

PG_FUNCTION_INFO_V1(test_shared_plan_cache_handles_valid);
Datum
test_shared_plan_cache_handles_valid(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(SharedPlanCacheHandlesValid());
}

PG_FUNCTION_INFO_V1(test_shared_plan_cache_current_entries);
Datum
test_shared_plan_cache_current_entries(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32((int32) SharedPlanCacheCurrentEntries());
}

PG_FUNCTION_INFO_V1(test_shared_plan_cache_force_attach);
Datum
test_shared_plan_cache_force_attach(PG_FUNCTION_ARGS)
{
	SharedPlanCacheAttach();
	PG_RETURN_BOOL(SharedPlanCacheIsAttached());
}
