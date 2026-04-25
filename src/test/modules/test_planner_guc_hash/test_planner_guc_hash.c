/*-------------------------------------------------------------------------
 *
 * test_planner_guc_hash.c
 *	  SQL wrappers for testing GetPlannerGucHash() and lazy recomputation.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_planner_guc_hash/test_planner_guc_hash.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "utils/planner_guc_hash.h"

PG_MODULE_MAGIC;

/*
 * test_planner_guc_hash - return current planner GUC hash as int8.
 *
 * The uint64 value is returned as int8 (signed 64-bit).  Tests must
 * only compare for equality/inequality, not assume positivity or ordering.
 */
PG_FUNCTION_INFO_V1(test_planner_guc_hash);
Datum
test_planner_guc_hash(PG_FUNCTION_ARGS)
{
	uint64		hash = GetPlannerGucHash();

	PG_RETURN_INT64((int64) hash);
}

/*
 * test_planner_guc_hash_recompute_count - return the number of times the
 * hash has been recomputed.
 *
 * Only meaningful in assert-enabled builds (USE_ASSERT_CHECKING).
 * In non-assert builds, always returns -1.
 */
PG_FUNCTION_INFO_V1(test_planner_guc_hash_recompute_count);
Datum
test_planner_guc_hash_recompute_count(PG_FUNCTION_ARGS)
{
#ifdef USE_ASSERT_CHECKING
	PG_RETURN_INT64((int64) GetPlannerGucHashComputeCount());
#else
	PG_RETURN_INT64(-1);
#endif
}
