/*-------------------------------------------------------------------------
 *
 * test_shared_plan_cache_store.c
 *	  Test wrappers for shared plan cache key computation and store.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_shared_plan_cache_store/test_shared_plan_cache_store.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/prepare.h"
#include "fmgr.h"
#include "lib/dshash.h"
#include "miscadmin.h"
#include "nodes/plannodes.h"
#include "port/atomics.h"
#include "utils/builtins.h"
#include "utils/dsa.h"
#include "utils/plancache.h"
#include "utils/shared_plancache.h"

PG_MODULE_MAGIC;

static const char *
store_status_to_string(SharedPlanStoreStatus status)
{
	switch (status)
	{
		case SHARED_PLAN_STORE_OK:
			return "OK";
		case SHARED_PLAN_STORE_DISABLED:
			return "DISABLED";
		case SHARED_PLAN_STORE_NOT_SHAREABLE:
			return "NOT_SHAREABLE";
		case SHARED_PLAN_STORE_OVERSIZE:
			return "OVERSIZE";
		case SHARED_PLAN_STORE_OOM:
			return "OOM";
		case SHARED_PLAN_STORE_DUPLICATE:
			return "DUPLICATE";
		case SHARED_PLAN_STORE_FULL:
			return "FULL";
	}
	return "UNKNOWN";
}

static const char *
reject_reason_to_string(SharedPlanRejectReason reason)
{
	switch (reason)
	{
		case SHARED_PLAN_REJECT_NONE:
			return "NONE";
		case SHARED_PLAN_REJECT_NOT_GENERIC:
			return "NOT_GENERIC";
		case SHARED_PLAN_REJECT_INCOMPLETE:
			return "INCOMPLETE";
		case SHARED_PLAN_REJECT_ONESHOT:
			return "ONESHOT";
		case SHARED_PLAN_REJECT_POST_REWRITE_HOOK:
			return "POST_REWRITE_HOOK";
		case SHARED_PLAN_REJECT_PLANNER_HOOK:
			return "PLANNER_HOOK";
		case SHARED_PLAN_REJECT_DEPENDS_ON_RLS:
			return "DEPENDS_ON_RLS";
		case SHARED_PLAN_REJECT_DEPENDS_ON_ROLE:
			return "DEPENDS_ON_ROLE";
		case SHARED_PLAN_REJECT_TEMP_OBJECT:
			return "TEMP_OBJECT";
		case SHARED_PLAN_REJECT_SAVED_XMIN:
			return "SAVED_XMIN";
		case SHARED_PLAN_REJECT_CUSTOM_SCAN:
			return "CUSTOM_SCAN";
		case SHARED_PLAN_REJECT_FOREIGN_SCAN:
			return "FOREIGN_SCAN";
		case SHARED_PLAN_REJECT_EXTENSION_STATE:
			return "EXTENSION_STATE";
		case SHARED_PLAN_REJECT_OVERSIZE:
			return "OVERSIZE";
		case SHARED_PLAN_REJECT_UNKNOWN_UNSHAREABLE:
			return "UNKNOWN_UNSHAREABLE";
	}
	return "UNKNOWN";
}

/*
 * Helper: compute key for a named prepared statement.
 * Caller must pfree nothing; key is populated on success.
 * Returns false and populates reason on failure.
 */
static bool
compute_key_for_stmt(const char *stmt_name, SharedPlanKey *key,
					 SharedPlanRejectReason *reason)
{
	PreparedStatement *pstmt;
	CachedPlanSource *plansource;
	CachedPlan *plan;
	bool		ok;

	pstmt = FetchPreparedStatement(stmt_name, true);
	plansource = pstmt->plansource;
	plan = GetCachedPlan(plansource, NULL, NULL, NULL);

	PG_TRY();
	{
		ok = ComputeSharedPlanKey(plansource, plan, true, key, reason);
	}
	PG_FINALLY();
	{
		ReleaseCachedPlan(plan, NULL);
	}
	PG_END_TRY();

	return ok;
}

/* ---- Full key text ---- */

PG_FUNCTION_INFO_V1(test_shared_plan_compute_key);
Datum
test_shared_plan_compute_key(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	SharedPlanKey key;
	SharedPlanRejectReason reason = SHARED_PLAN_REJECT_NONE;
	char		buf[512];

	if (!compute_key_for_stmt(stmt_name, &key, &reason))
		snprintf(buf, sizeof(buf), "NOT_SHAREABLE:%s",
				 reject_reason_to_string(reason));
	else
		snprintf(buf, sizeof(buf),
				 "queryid=" INT64_FORMAT
				 " dbid=%u roleid=%u num_params=%d cursor_options=%d"
				 " param_signature_hash=" UINT64_FORMAT
				 " query_tree_hash=" UINT64_FORMAT
				 " search_path_hash=" UINT64_FORMAT
				 " planner_gucs_hash=" UINT64_FORMAT
				 " collation_hash=" UINT64_FORMAT,
				 key.queryid,
				 key.dbid, key.roleid,
				 key.num_params, key.cursor_options,
				 key.param_signature_hash,
				 key.query_tree_hash,
				 key.search_path_hash,
				 key.planner_gucs_hash,
				 key.collation_hash);

	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

/* ---- Per-field key accessors ---- */

PG_FUNCTION_INFO_V1(test_shared_plan_key_field);
Datum
test_shared_plan_key_field(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	const char *field_name = text_to_cstring(PG_GETARG_TEXT_PP(1));
	SharedPlanKey key;
	SharedPlanRejectReason reason = SHARED_PLAN_REJECT_NONE;
	char		buf[64];

	if (!compute_key_for_stmt(stmt_name, &key, &reason))
		PG_RETURN_NULL();

	if (strcmp(field_name, "queryid") == 0)
		snprintf(buf, sizeof(buf), INT64_FORMAT, key.queryid);
	else if (strcmp(field_name, "dbid") == 0)
		snprintf(buf, sizeof(buf), "%u", key.dbid);
	else if (strcmp(field_name, "roleid") == 0)
		snprintf(buf, sizeof(buf), "%u", key.roleid);
	else if (strcmp(field_name, "num_params") == 0)
		snprintf(buf, sizeof(buf), "%d", key.num_params);
	else if (strcmp(field_name, "cursor_options") == 0)
		snprintf(buf, sizeof(buf), "%d", key.cursor_options);
	else if (strcmp(field_name, "param_signature_hash") == 0)
		snprintf(buf, sizeof(buf), UINT64_FORMAT, key.param_signature_hash);
	else if (strcmp(field_name, "query_tree_hash") == 0)
		snprintf(buf, sizeof(buf), UINT64_FORMAT, key.query_tree_hash);
	else if (strcmp(field_name, "search_path_hash") == 0)
		snprintf(buf, sizeof(buf), UINT64_FORMAT, key.search_path_hash);
	else if (strcmp(field_name, "planner_gucs_hash") == 0)
		snprintf(buf, sizeof(buf), UINT64_FORMAT, key.planner_gucs_hash);
	else if (strcmp(field_name, "collation_hash") == 0)
		snprintf(buf, sizeof(buf), UINT64_FORMAT, key.collation_hash);
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unknown key field: %s", field_name)));

	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

/* ---- Store operations ---- */

PG_FUNCTION_INFO_V1(test_shared_plan_store);
Datum
test_shared_plan_store(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	PreparedStatement *pstmt;
	CachedPlanSource *plansource;
	CachedPlan *plan = NULL;
	SharedPlanStoreStatus status;
	SharedPlanRejectReason reason = SHARED_PLAN_REJECT_NONE;
	char		result_buf[128];

	pstmt = FetchPreparedStatement(stmt_name, true);
	plansource = pstmt->plansource;
	plan = GetCachedPlan(plansource, NULL, NULL, NULL);

	PG_TRY();
	{
		status = SharedPlanCacheStore(plansource, plan, true, NULL, &reason);
	}
	PG_FINALLY();
	{
		ReleaseCachedPlan(plan, NULL);
	}
	PG_END_TRY();

	if (status == SHARED_PLAN_STORE_NOT_SHAREABLE)
		snprintf(result_buf, sizeof(result_buf), "NOT_SHAREABLE:%s",
				 reject_reason_to_string(reason));
	else
		snprintf(result_buf, sizeof(result_buf), "%s",
				 store_status_to_string(status));

	PG_RETURN_TEXT_P(cstring_to_text(result_buf));
}

PG_FUNCTION_INFO_V1(test_shared_plan_store_duplicate);
Datum
test_shared_plan_store_duplicate(PG_FUNCTION_ARGS)
{
	return test_shared_plan_store(fcinfo);
}

PG_FUNCTION_INFO_V1(test_shared_plan_store_count);
Datum
test_shared_plan_store_count(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32((int32) SharedPlanCacheCurrentEntries());
}

/* ---- Entry inspection ---- */

PG_FUNCTION_INFO_V1(test_shared_plan_store_entry_valid);
Datum
test_shared_plan_store_entry_valid(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	SharedPlanKey key;
	SharedPlanRejectReason reason = SHARED_PLAN_REJECT_NONE;
	SharedPlanEntry *entry;
	dshash_table *hash;
	bool		valid = false;

	if (compute_key_for_stmt(stmt_name, &key, &reason))
	{
		hash = SharedPlanCacheGetHash();
		if (hash != NULL)
		{
			entry = dshash_find(hash, &key, false);
			if (entry != NULL)
			{
				valid = (pg_atomic_read_u32(&entry->is_valid) == 1);
				dshash_release_lock(hash, entry);
			}
		}
	}

	PG_RETURN_BOOL(valid);
}

PG_FUNCTION_INFO_V1(test_shared_plan_store_serialized_size);
Datum
test_shared_plan_store_serialized_size(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	SharedPlanKey key;
	SharedPlanRejectReason reason = SHARED_PLAN_REJECT_NONE;
	SharedPlanEntry *entry;
	dshash_table *hash;
	int64		result = -1;

	if (compute_key_for_stmt(stmt_name, &key, &reason))
	{
		hash = SharedPlanCacheGetHash();
		if (hash != NULL)
		{
			entry = dshash_find(hash, &key, false);
			if (entry != NULL)
			{
				result = (int64) entry->serialized_plan_len;
				dshash_release_lock(hash, entry);
			}
		}
	}

	PG_RETURN_INT64(result);
}

PG_FUNCTION_INFO_V1(test_shared_plan_store_dependency_counts);
Datum
test_shared_plan_store_dependency_counts(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	SharedPlanKey key;
	SharedPlanRejectReason reason = SHARED_PLAN_REJECT_NONE;
	SharedPlanEntry *entry;
	dshash_table *hash;
	char		buf[64];

	if (!compute_key_for_stmt(stmt_name, &key, &reason))
		PG_RETURN_TEXT_P(cstring_to_text("NOT_FOUND"));

	hash = SharedPlanCacheGetHash();
	if (hash == NULL)
		PG_RETURN_TEXT_P(cstring_to_text("NOT_FOUND"));

	entry = dshash_find(hash, &key, false);
	if (entry == NULL)
		PG_RETURN_TEXT_P(cstring_to_text("NOT_FOUND"));

	snprintf(buf, sizeof(buf), "relation_oids=%d inval_items=%d",
			 entry->num_relation_oids, entry->num_inval_items);
	dshash_release_lock(hash, entry);

	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

/* ---- Reset ---- */

PG_FUNCTION_INFO_V1(test_shared_plan_store_reset);
Datum
test_shared_plan_store_reset(PG_FUNCTION_ARGS)
{
	dshash_table *hash;
	dsa_area   *dsa;
	SharedPlanCacheControl *ctl;
	dshash_seq_status seqstatus;
	SharedPlanEntry *entry;

	hash = SharedPlanCacheGetHash();
	dsa = SharedPlanCacheGetDSA();
	ctl = SharedPlanCacheGetControl();

	if (hash == NULL || dsa == NULL || ctl == NULL)
		PG_RETURN_VOID();

	dshash_seq_init(&seqstatus, hash, true);
	PG_TRY();
	{
		while ((entry = dshash_seq_next(&seqstatus)) != NULL)
		{
			if (DsaPointerIsValid(entry->serialized_plan))
				dsa_free(dsa, entry->serialized_plan);
			dshash_delete_current(&seqstatus);
		}
	}
	PG_FINALLY();
	{
		dshash_seq_term(&seqstatus);
	}
	PG_END_TRY();

	pg_atomic_write_u32(&ctl->current_entries, 0);

	PG_RETURN_VOID();
}
