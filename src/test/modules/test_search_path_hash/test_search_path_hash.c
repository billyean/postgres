/*-------------------------------------------------------------------------
 *
 * test_search_path_hash.c
 *	  SQL wrappers for testing GetSearchPathHash().
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_search_path_hash/test_search_path_hash.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "fmgr.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_search_path_hash);
Datum
test_search_path_hash(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) GetSearchPathHash());
}

PG_FUNCTION_INFO_V1(test_search_path_hash_recompute_count);
Datum
test_search_path_hash_recompute_count(PG_FUNCTION_ARGS)
{
#ifdef USE_ASSERT_CHECKING
	PG_RETURN_INT64((int64) GetSearchPathHashComputeCount());
#else
	PG_RETURN_INT64(-1);
#endif
}
