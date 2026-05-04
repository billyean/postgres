/*-------------------------------------------------------------------------
 *
 * shared_plancache.c
 *	  Shared plan cache infrastructure: DSA area, dshash tables, backend
 *	  attach/detach lifecycle, and GUC-controlled sizing.
 *
 * This module creates the shared-memory substrate for the future shared
 * generic plan cache.  No lookup/store/serialization logic is implemented
 * here; that belongs in later patches.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/shared_plancache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "common/hashfn.h"
#include "lib/dshash.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "nodes/plannodes.h"
#include "nodes/primnodes.h"
#include "nodes/queryjumble.h"
#include "nodes/readfuncs.h"
#include "optimizer/planner.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/dsa.h"
#include "utils/memutils.h"
#include "utils/planner_guc_hash.h"
#include "utils/shared_plancache.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"

/* GUC variables */
int			shared_plan_cache_max_entries = 1000;
int			shared_plan_cache_max_memory = 32768;	/* KB */
int			shared_plan_cache_max_entry_size = 256;	/* KB */
bool		shared_plan_cache_enabled = false;

/* Shared control struct pointer (set by shmem framework) */
static SharedPlanCacheControl *shared_plan_ctl = NULL;

/* Backend-local state */
static bool shared_plan_cache_attached = false;
static bool shared_plan_cache_active = false;
static bool attach_warning_given = false;
static bool queryid_warning_given = false;
static dsa_area *shared_plan_dsa = NULL;
static dshash_table *shared_plan_hash = NULL;
static dshash_table *shared_plan_dep_hash = NULL;
static bool relcache_callback_registered = false;
static bool syscache_callbacks_registered = false;

/* Test-only hook for T7 store-during-generation-change test */
static bool shared_plan_test_force_generation_bump_before_publish = false;

/* Backend-local test counters (Patch 0007) */
static uint64 l2_hit_count = 0;
static uint64 l2_miss_count = 0;
static uint64 l2_store_count = 0;
static uint64 l2_error_count = 0;
static SharedPlanLookupStatus last_l2_status = SHARED_PLAN_LOOKUP_NONE;

/*
 * dshash parameters (tranche_id set at runtime).
 *
 * SharedPlanKey uses dshash_memhash/dshash_memcmp for bytewise hashing and
 * comparison.  This is safe only if SharedPlanKey is zero-initialized via
 * memset() before field population, making padding bytes deterministic.
 */
static const dshash_parameters shared_plan_hash_params = {
	.key_size = sizeof(SharedPlanKey),
	.entry_size = sizeof(SharedPlanEntry),
	.compare_function = dshash_memcmp,
	.hash_function = dshash_memhash,
	.copy_function = dshash_memcpy,
	.tranche_id = 0,
};

static const dshash_parameters shared_plan_dep_hash_params = {
	.key_size = sizeof(Oid),
	.entry_size = sizeof(SharedPlanDepEntry),
	.compare_function = dshash_memcmp,
	.hash_function = dshash_memhash,
	.copy_function = dshash_memcpy,
	.tranche_id = 0,
};

/* Forward declarations */
static void SharedPlanCacheShmemRequest(void *arg);
static void SharedPlanCacheShmemInit(void *arg);
static void SharedPlanCacheShutdown(int code, Datum arg);
static void SharedPlanCacheDetachLocal(void);
static Size shared_plan_cache_dsa_init_size(void);
static void SharedPlanCacheRelCallback(Datum arg, Oid relid);
static void SharedPlanCacheSysCallback(Datum arg, SysCacheIdentifier cacheid,
									   uint32 hashvalue);
static bool SharedPlanExtractRelationDeps(List *stmt_list, Oid **rel_oids_out,
										  int *num_rels_out,
										  SharedPlanStoreStatus *reject_out);

/* Shmem callbacks for subsystemlist.h */
const ShmemCallbacks SharedPlanCacheShmemCallbacks = {
	.request_fn = SharedPlanCacheShmemRequest,
	.init_fn = SharedPlanCacheShmemInit,
};

/*
 * SharedPlanCacheShmemSize - compute required shmem size.
 */
static Size
SharedPlanCacheShmemSize(void)
{
	Size		size;

	size = MAXALIGN(sizeof(SharedPlanCacheControl));
	size = add_size(size, shared_plan_cache_dsa_init_size());
	return size;
}

/*
 * shared_plan_cache_dsa_init_size - compute initial DSA segment size.
 */
static Size
shared_plan_cache_dsa_init_size(void)
{
	Size		max_bytes;
	Size		init_size;

	if (shared_plan_cache_max_entries == 0)
		return 0;

	max_bytes = (Size) shared_plan_cache_max_memory * 1024;
	init_size = Max((Size) (1024 * 1024), Min(max_bytes / 4, (Size) (8 * 1024 * 1024)));
	return MAXALIGN(init_size);
}

/*
 * SharedPlanCacheShmemRequest - request shared memory space.
 */
static void
SharedPlanCacheShmemRequest(void *arg)
{
	ShmemRequestStruct(.name = "shared plan cache",
					   .size = SharedPlanCacheShmemSize(),
					   .ptr = (void **) &shared_plan_ctl);
}

/*
 * SharedPlanCacheShmemInit - initialize shared memory structures.
 *
 * Called once in the postmaster during CreateSharedMemoryAndSemaphores().
 * Always initializes the control struct fields before checking whether the
 * cache is disabled (max_entries == 0).
 */
static void
SharedPlanCacheShmemInit(void *arg)
{
	dsa_area   *dsa;
	dshash_table *plan_hash;
	dshash_table *dep_hash;
	dshash_parameters params;
	int			tranche_id;

	Assert(!IsUnderPostmaster);
	Assert(shared_plan_ctl != NULL);

	/* Always initialize control struct first */
	shared_plan_ctl->max_entries = shared_plan_cache_max_entries;
	shared_plan_ctl->max_memory_kb = shared_plan_cache_max_memory;
	shared_plan_ctl->max_entry_size_kb = shared_plan_cache_max_entry_size;
	pg_atomic_init_u64(&shared_plan_ctl->generation, 0);
	pg_atomic_init_u64(&shared_plan_ctl->relcache_store_epoch, 0);
	pg_atomic_init_u32(&shared_plan_ctl->current_entries, 0);
	shared_plan_ctl->plan_hash_handle = DSHASH_HANDLE_INVALID;
	shared_plan_ctl->dep_hash_handle = DSHASH_HANDLE_INVALID;
	shared_plan_ctl->tranche_id = 0;

	if (shared_plan_cache_max_entries == 0)
		return;

	/* Create DSA area in-place */
	tranche_id = LWLockNewTrancheId("shared_plan_cache");
	shared_plan_ctl->tranche_id = tranche_id;

	dsa = dsa_create_in_place(shared_plan_ctl->raw_dsa_area,
							  shared_plan_cache_dsa_init_size(),
							  tranche_id, NULL);
	dsa_pin(dsa);
	dsa_set_size_limit(dsa, (Size) shared_plan_cache_max_memory * 1024);

	/* Request query ID computation */
	EnableQueryId();

	/* Create primary plan hash table */
	params = shared_plan_hash_params;
	params.tranche_id = tranche_id;
	plan_hash = dshash_create(dsa, &params, NULL);
	shared_plan_ctl->plan_hash_handle = dshash_get_hash_table_handle(plan_hash);
	dshash_detach(plan_hash);

	/* Create dependency hash table */
	params = shared_plan_dep_hash_params;
	params.tranche_id = tranche_id;
	dep_hash = dshash_create(dsa, &params, NULL);
	shared_plan_ctl->dep_hash_handle = dshash_get_hash_table_handle(dep_hash);
	dshash_detach(dep_hash);

	/* Postmaster does not use the DSA directly */
	dsa_detach(dsa);
}

/*
 * SharedPlanCacheDetachLocal - detach all local shared plan cache state.
 *
 * Detaches dshash tables before DSA (dshash requires a valid DSA).
 * Clears all backend-local pointers and flags.
 */
static void
SharedPlanCacheDetachLocal(void)
{
	if (shared_plan_dep_hash != NULL)
	{
		dshash_detach(shared_plan_dep_hash);
		shared_plan_dep_hash = NULL;
	}
	if (shared_plan_hash != NULL)
	{
		dshash_detach(shared_plan_hash);
		shared_plan_hash = NULL;
	}
	if (shared_plan_dsa != NULL)
	{
		dsa_detach(shared_plan_dsa);
		shared_plan_dsa = NULL;
	}
	shared_plan_cache_attached = false;
	shared_plan_cache_active = false;
}

/*
 * SharedPlanCacheShutdown - before_shmem_exit callback.
 */
static void
SharedPlanCacheShutdown(int code, Datum arg)
{
	SharedPlanCacheDetachLocal();
}

/*
 * SharedPlanCacheAttach - attach this backend to the shared plan cache.
 *
 * Called from InitPostgres() after InitPlanCache().  Idempotent: repeated
 * calls are safe and do nothing after the first successful attach.
 *
 * Deterministic prechecks handle expected disabled/unavailable states.
 * The actual DSA/dshash attach sequence lets errors propagate: the in-place
 * DSA is pinned and should always be attachable, so an ERROR here indicates
 * a serious condition (corruption, OOM) that should not be silently swallowed.
 *
 * dshash_attach() never returns NULL with valid DSA and handle (confirmed:
 * dshash.c:274-302 uses palloc + Assert, no NULL return path).
 */
void
SharedPlanCacheAttach(void)
{
	dsa_area   *dsa;
	dshash_parameters params;

	/*
	 * If Phase A (invalidation attach) is done and Phase B (lookup/store) is
	 * already active, nothing more to do.  If Phase A is done but Phase B is
	 * not yet active, skip to Phase B check in case compute_query_id changed.
	 */
	if (shared_plan_cache_attached && shared_plan_cache_active)
		return;

	if (shared_plan_ctl == NULL || shared_plan_ctl->max_entries == 0)
		return;

	if (!DsaPointerIsValid(shared_plan_ctl->plan_hash_handle) ||
		!DsaPointerIsValid(shared_plan_ctl->dep_hash_handle))
	{
		if (!attach_warning_given)
		{
			ereport(WARNING,
					(errmsg("shared plan cache disabled for this backend: invalid handles")));
			attach_warning_given = true;
		}
		return;
	}

	/*
	 * Phase A: Invalidation attach.  Attach DSA and dshash tables so that
	 * the relcache callback can perform precise dep-index invalidation.
	 * This happens for ALL backends regardless of compute_query_id.
	 */
	if (shared_plan_dsa == NULL)
	{
		dsa = dsa_attach_in_place(shared_plan_ctl->raw_dsa_area, NULL);
		dsa_pin_mapping(dsa);
		shared_plan_dsa = dsa;
	}

	if (shared_plan_hash == NULL)
	{
		params = shared_plan_hash_params;
		params.tranche_id = shared_plan_ctl->tranche_id;
		shared_plan_hash = dshash_attach(shared_plan_dsa, &params,
										 shared_plan_ctl->plan_hash_handle,
										 NULL);
		Assert(shared_plan_hash != NULL);
	}

	if (shared_plan_dep_hash == NULL)
	{
		params = shared_plan_dep_hash_params;
		params.tranche_id = shared_plan_ctl->tranche_id;
		shared_plan_dep_hash = dshash_attach(shared_plan_dsa, &params,
											 shared_plan_ctl->dep_hash_handle,
											 NULL);
		Assert(shared_plan_dep_hash != NULL);
	}

	/* Register relcache invalidation callback exactly once */
	if (!relcache_callback_registered)
	{
		CacheRegisterRelcacheCallback(SharedPlanCacheRelCallback, (Datum) 0);
		relcache_callback_registered = true;
	}

	/* Register syscache invalidation callbacks exactly once (Patch 0009) */
	if (!syscache_callbacks_registered)
	{
		CacheRegisterSyscacheCallback(PROCOID,
									  SharedPlanCacheSysCallback, (Datum) 0);
		CacheRegisterSyscacheCallback(TYPEOID,
									  SharedPlanCacheSysCallback, (Datum) 0);
		CacheRegisterSyscacheCallback(NAMESPACEOID,
									  SharedPlanCacheSysCallback, (Datum) 0);
		CacheRegisterSyscacheCallback(OPEROID,
									  SharedPlanCacheSysCallback, (Datum) 0);
		CacheRegisterSyscacheCallback(AMOPOPID,
									  SharedPlanCacheSysCallback, (Datum) 0);
		CacheRegisterSyscacheCallback(FOREIGNSERVEROID,
									  SharedPlanCacheSysCallback, (Datum) 0);
		CacheRegisterSyscacheCallback(FOREIGNDATAWRAPPEROID,
									  SharedPlanCacheSysCallback, (Datum) 0);
		syscache_callbacks_registered = true;
	}

	/* Register cleanup callback exactly once */
	if (!shared_plan_cache_attached)
	{
		before_shmem_exit(SharedPlanCacheShutdown, (Datum) 0);
		shared_plan_cache_attached = true;
	}

	/*
	 * Phase B: Lookup/store activation.  Requires compute_query_id.
	 * Without queryid, Phase A (invalidation) is already complete above.
	 * L2 lookup/store remain disabled; the warning is deferred to the
	 * first actual lookup/store attempt (see SharedPlanCacheIsActive).
	 */
	if (!IsQueryIdEnabled())
		return;

	shared_plan_cache_active = true;
}

/* Accessors for test modules */

/*
 * SharedPlanCacheIsActive - can this backend perform L2 lookup/store?
 *
 * Phase A (invalidation attach) is independent of queryid availability.
 * Phase B (lookup/store) requires queryid.  If compute_query_id is off
 * and Phase A succeeded (shared cache is configured), emit a one-time
 * warning on the first call.  This ensures DDL-only backends that never
 * call lookup/store code do not warn, while backends that attempt to
 * check L2 availability see the diagnostic once.
 */
bool
SharedPlanCacheIsActive(void)
{
	if (shared_plan_cache_active && IsQueryIdEnabled())
		return true;

	/* Phase A succeeded but queryid is off — warn once */
	if (shared_plan_cache_attached && !IsQueryIdEnabled() &&
		!queryid_warning_given)
	{
		ereport(WARNING,
				(errmsg("shared plan cache disabled for this backend: compute_query_id is off")));
		queryid_warning_given = true;
	}

	return false;
}

bool
SharedPlanCacheIsAttached(void)
{
	return shared_plan_cache_attached;
}

bool
SharedPlanCacheHandlesValid(void)
{
	if (shared_plan_ctl == NULL)
		return false;
	return DsaPointerIsValid(shared_plan_ctl->plan_hash_handle) &&
		DsaPointerIsValid(shared_plan_ctl->dep_hash_handle);
}

uint32
SharedPlanCacheCurrentEntries(void)
{
	if (shared_plan_ctl == NULL)
		return 0;
	return pg_atomic_read_u32(&shared_plan_ctl->current_entries);
}

/* ---- Plan Serialization (Patch 0005) ---- */

SharedPlanSerializeStatus
SharedPlanSerializeStmtList(List *stmt_list,
							Size max_total_size,
							char **out_data,
							Size *out_len)
{
	char	   *text;
	Size		text_len;
	Size		total_len;
	char	   *buf;
	SharedPlanSerializedHeader *hdr;

	Assert(out_data != NULL);
	Assert(out_len != NULL);

	*out_data = NULL;
	*out_len = 0;

	text = nodeToString(stmt_list);
	text_len = strlen(text);

	if (text_len > PG_UINT32_MAX)
	{
		pfree(text);
		return SHARED_PLAN_SERIALIZE_OVERSIZE;
	}

	total_len = add_size(sizeof(SharedPlanSerializedHeader), text_len);

	if (total_len > max_total_size)
	{
		pfree(text);
		return SHARED_PLAN_SERIALIZE_OVERSIZE;
	}

	buf = (char *) palloc(total_len);
	hdr = (SharedPlanSerializedHeader *) buf;
	hdr->magic = SHARED_PLAN_SERIAL_MAGIC;
	hdr->version = SHARED_PLAN_SERIAL_VERSION;
	hdr->format = SHARED_PLAN_FORMAT_TEXT;
	hdr->payload_len = (uint32) text_len;
	hdr->num_stmts = (uint32) list_length(stmt_list);

	memcpy(buf + sizeof(SharedPlanSerializedHeader), text, text_len);
	pfree(text);

	*out_data = buf;
	*out_len = total_len;
	return SHARED_PLAN_SERIALIZE_OK;
}

SharedPlanSerializeStatus
SharedPlanSerializeCachedPlan(CachedPlanSource *plansource,
							  CachedPlan *plan,
							  bool is_generic_plan,
							  Size max_total_size,
							  char **out_data,
							  Size *out_len,
							  SharedPlanRejectReason *reject_reason)
{
	SharedPlanSerializeStatus status;

	Assert(out_data != NULL);
	Assert(out_len != NULL);
	Assert(reject_reason != NULL);

	*out_data = NULL;
	*out_len = 0;

	if (!PlanIsShareable(plansource, plan, is_generic_plan, reject_reason))
		return SHARED_PLAN_SERIALIZE_NOT_SHAREABLE;

	Assert(plan != NULL);

	status = SharedPlanSerializeStmtList(plan->stmt_list,
										 max_total_size,
										 out_data, out_len);

	if (status == SHARED_PLAN_SERIALIZE_OK)
		*reject_reason = SHARED_PLAN_REJECT_NONE;
	else if (status == SHARED_PLAN_SERIALIZE_OVERSIZE)
		*reject_reason = SHARED_PLAN_REJECT_OVERSIZE;

	return status;
}

SharedPlanSerializeStatus
SharedPlanSerializeToDSA(List *stmt_list,
						 Size max_total_size,
						 dsa_area *area,
						 dsa_pointer *out_ptr,
						 Size *out_len)
{
	SharedPlanSerializeStatus status;
	char	   *local_buf;
	Size		local_len;
	dsa_pointer dp;

	Assert(area != NULL);
	Assert(out_ptr != NULL);
	Assert(out_len != NULL);

	*out_ptr = InvalidDsaPointer;
	*out_len = 0;

	status = SharedPlanSerializeStmtList(stmt_list, max_total_size,
										 &local_buf, &local_len);
	if (status != SHARED_PLAN_SERIALIZE_OK)
		return status;

	dp = dsa_allocate_extended(area, local_len, DSA_ALLOC_NO_OOM);
	if (!DsaPointerIsValid(dp))
	{
		pfree(local_buf);
		return SHARED_PLAN_SERIALIZE_OOM;
	}

	memcpy(dsa_get_address(area, dp), local_buf, local_len);
	pfree(local_buf);

	*out_ptr = dp;
	*out_len = local_len;
	return SHARED_PLAN_SERIALIZE_OK;
}

/*
 * SharedPlanDeserialize - deserialize a buffer into a List of PlannedStmt.
 *
 * Header-level corruption returns INVALID_INPUT without ERROR.
 * stringToNode() may elog(ERROR) on corrupt payload text; the context switch
 * is restored via PG_CATCH before re-throwing so CurrentMemoryContext is
 * always correct on the caller's error path.
 * Post-parse validation returns INVALID_INPUT without ERROR.
 */
SharedPlanSerializeStatus
SharedPlanDeserialize(const char *data,
					  Size len,
					  MemoryContext target_context,
					  List **out_stmt_list)
{
	const SharedPlanSerializedHeader *hdr;
	char	   *payload_buf;
	void	   *result;
	MemoryContext oldcontext;
	ListCell   *lc;

	Assert(data != NULL);
	Assert(target_context != NULL);
	Assert(out_stmt_list != NULL);

	*out_stmt_list = NIL;

	/* Header validation, returns status without ERROR */
	if (len < sizeof(SharedPlanSerializedHeader))
		return SHARED_PLAN_SERIALIZE_INVALID_INPUT;

	hdr = (const SharedPlanSerializedHeader *) data;

	if (hdr->magic != SHARED_PLAN_SERIAL_MAGIC)
		return SHARED_PLAN_SERIALIZE_INVALID_INPUT;
	if (hdr->version != SHARED_PLAN_SERIAL_VERSION)
		return SHARED_PLAN_SERIALIZE_INVALID_INPUT;
	if (hdr->format != SHARED_PLAN_FORMAT_TEXT)
		return SHARED_PLAN_SERIALIZE_INVALID_INPUT;
	if (hdr->payload_len != len - sizeof(SharedPlanSerializedHeader))
		return SHARED_PLAN_SERIALIZE_INVALID_INPUT;

	/* Copy payload to NUL-terminated buffer */
	payload_buf = (char *) palloc(add_size((Size) hdr->payload_len, 1));
	memcpy(payload_buf, data + sizeof(SharedPlanSerializedHeader),
		   hdr->payload_len);
	payload_buf[hdr->payload_len] = '\0';

	/* Deserialize into target context; clean up payload_buf on ERROR */
	oldcontext = MemoryContextSwitchTo(target_context);
	PG_TRY();
	{
		result = stringToNode(payload_buf);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(oldcontext);
		pfree(payload_buf);
		PG_RE_THROW();
	}
	PG_END_TRY();
	MemoryContextSwitchTo(oldcontext);

	pfree(payload_buf);

	/* Post-parse validation, returns status without ERROR */
	if (result == NULL || !IsA(result, List))
		return SHARED_PLAN_SERIALIZE_INVALID_INPUT;
	if (list_length((List *) result) != (int) hdr->num_stmts)
		return SHARED_PLAN_SERIALIZE_INVALID_INPUT;

	foreach(lc, (List *) result)
	{
		if (!IsA(lfirst(lc), PlannedStmt))
			return SHARED_PLAN_SERIALIZE_INVALID_INPUT;
	}

	*out_stmt_list = (List *) result;
	return SHARED_PLAN_SERIALIZE_OK;
}

SharedPlanSerializeStatus
SharedPlanDeserializeFromDSA(dsa_area *area,
							 dsa_pointer ptr,
							 Size len,
							 MemoryContext target_context,
							 List **out_stmt_list)
{
	const char *addr;

	Assert(area != NULL);
	Assert(DsaPointerIsValid(ptr));
	Assert(out_stmt_list != NULL);

	addr = (const char *) dsa_get_address(area, ptr);

	return SharedPlanDeserialize(addr, len, target_context, out_stmt_list);
}

/* ---- Key Computation + Store (Patch 0006) ---- */

/*
 * shared_plan_compute_generic_cost - compute generic_cost for a stmt_list.
 *
 * Mechanically matches cached_plan_cost(plan, false) from plancache.c:
 * sums planTree->total_cost across non-utility PlannedStmts.
 *
 * Zero is valid: a utility-only stmt_list has zero execution cost.
 * Returns -1 on invalid input: NaN, negative aggregate, or a non-utility
 * PlannedStmt with NULL planTree (which would indicate corruption).
 */
static double
shared_plan_compute_generic_cost(List *stmt_list)
{
	double		result = 0;
	ListCell   *lc;

	foreach(lc, stmt_list)
	{
		PlannedStmt *pstmt = lfirst_node(PlannedStmt, lc);

		if (pstmt->commandType == CMD_UTILITY)
			continue;

		if (pstmt->planTree == NULL)
			return -1;

		result += pstmt->planTree->total_cost;
	}

	if (isnan(result) || result < 0)
		return -1;

	return result;
}

/*
 * Test-module-only accessors; not part of production shared plan cache API.
 * These expose internal state for the test_shared_plan_cache_store module.
 */

dsa_area *
SharedPlanCacheGetDSA(void)
{
	return shared_plan_dsa;
}

dshash_table *
SharedPlanCacheGetHash(void)
{
	return shared_plan_hash;
}

SharedPlanCacheControl *
SharedPlanCacheGetControl(void)
{
	return shared_plan_ctl;
}

/* Collation hash walker context */
typedef struct CollationHashContext
{
	uint64		hash;
	bool		has_collation;
} CollationHashContext;

#define HASH_COLLATION_OID(ctx, oid) \
	do { \
		if (OidIsValid(oid)) \
		{ \
			(ctx)->hash = hash_combine64((ctx)->hash, \
				hash_bytes_uint32_extended((uint32) (oid), UINT64CONST(0))); \
			(ctx)->has_collation = true; \
		} \
	} while (0)

static bool
collation_hash_walker(Node *node, void *context)
{
	CollationHashContext *ctx = (CollationHashContext *) context;

	if (node == NULL)
		return false;

	switch (nodeTag(node))
	{
		case T_Var:
			HASH_COLLATION_OID(ctx, ((Var *) node)->varcollid);
			break;
		case T_Const:
			HASH_COLLATION_OID(ctx, ((Const *) node)->constcollid);
			break;
		case T_Param:
			HASH_COLLATION_OID(ctx, ((Param *) node)->paramcollid);
			break;
		case T_CollateExpr:
			HASH_COLLATION_OID(ctx, ((CollateExpr *) node)->collOid);
			break;
		case T_FuncExpr:
			HASH_COLLATION_OID(ctx, ((FuncExpr *) node)->funccollid);
			HASH_COLLATION_OID(ctx, ((FuncExpr *) node)->inputcollid);
			break;
		case T_OpExpr:
		case T_DistinctExpr:
		case T_NullIfExpr:
			HASH_COLLATION_OID(ctx, ((OpExpr *) node)->opcollid);
			HASH_COLLATION_OID(ctx, ((OpExpr *) node)->inputcollid);
			break;
		case T_ScalarArrayOpExpr:
			HASH_COLLATION_OID(ctx, ((ScalarArrayOpExpr *) node)->inputcollid);
			break;
		case T_SubscriptingRef:
			HASH_COLLATION_OID(ctx, ((SubscriptingRef *) node)->refcollid);
			break;
		case T_MergeSupportFunc:
			HASH_COLLATION_OID(ctx, ((MergeSupportFunc *) node)->msfcollid);
			break;
		case T_Aggref:
			HASH_COLLATION_OID(ctx, ((Aggref *) node)->aggcollid);
			HASH_COLLATION_OID(ctx, ((Aggref *) node)->inputcollid);
			break;
		case T_WindowFunc:
			HASH_COLLATION_OID(ctx, ((WindowFunc *) node)->wincollid);
			HASH_COLLATION_OID(ctx, ((WindowFunc *) node)->inputcollid);
			break;
		case T_RelabelType:
			HASH_COLLATION_OID(ctx, ((RelabelType *) node)->resultcollid);
			break;
		case T_CoerceViaIO:
			HASH_COLLATION_OID(ctx, ((CoerceViaIO *) node)->resultcollid);
			break;
		case T_ArrayCoerceExpr:
			HASH_COLLATION_OID(ctx, ((ArrayCoerceExpr *) node)->resultcollid);
			break;
		case T_FieldSelect:
			HASH_COLLATION_OID(ctx, ((FieldSelect *) node)->resultcollid);
			break;
		case T_CaseExpr:
			HASH_COLLATION_OID(ctx, ((CaseExpr *) node)->casecollid);
			break;
		case T_CoalesceExpr:
			HASH_COLLATION_OID(ctx, ((CoalesceExpr *) node)->coalescecollid);
			break;
		case T_MinMaxExpr:
			HASH_COLLATION_OID(ctx, ((MinMaxExpr *) node)->minmaxcollid);
			HASH_COLLATION_OID(ctx, ((MinMaxExpr *) node)->inputcollid);
			break;
		case T_ArrayExpr:
			HASH_COLLATION_OID(ctx, ((ArrayExpr *) node)->array_collid);
			break;
		case T_RowCompareExpr:
			{
				ListCell   *lc;

				foreach(lc, ((RowCompareExpr *) node)->inputcollids)
					HASH_COLLATION_OID(ctx, lfirst_oid(lc));
			}
			break;
		case T_SubPlan:
			HASH_COLLATION_OID(ctx, ((SubPlan *) node)->firstColCollation);
			break;
		default:
			break;
	}

	if (IsA(node, Query))
		return query_tree_walker((Query *) node,
								 collation_hash_walker,
								 context, 0);
	return expression_tree_walker(node, collation_hash_walker, context);
}

static uint64
compute_collation_hash(List *query_list)
{
	CollationHashContext ctx;
	ListCell   *lc;

	ctx.hash = UINT64CONST(0);
	ctx.has_collation = false;

	foreach(lc, query_list)
		collation_hash_walker((Node *) lfirst(lc), &ctx);

	if (!ctx.has_collation)
		return UINT64CONST(0);

	return ctx.hash;
}

/*
 * compute_param_signature_hash - hash the external parameter signature.
 *
 * v1 hashes parameter type OIDs and parameter count only.  It does not
 * separately hash typmod or collation per-parameter, because CachedPlanSource
 * stores only type OIDs in param_types[].  Expression-level typmod and
 * collation effects are captured through query_tree_hash and collation_hash.
 * Richer parameter metadata may be considered in v2 if needed.
 */
static uint64
compute_param_signature_hash(CachedPlanSource *plansource)
{
	uint64		hash;
	int			i;

	if (plansource->num_params == 0)
		return UINT64CONST(0x1);

	Assert(plansource->param_types != NULL);

	hash = hash_bytes_uint32_extended((uint32) plansource->num_params,
									  UINT64CONST(0));

	for (i = 0; i < plansource->num_params; i++)
	{
		uint64		type_hash;

		type_hash = hash_bytes_uint32_extended((uint32) plansource->param_types[i],
											   UINT64CONST(0));
		hash = hash_combine64(hash, type_hash);
	}

	return hash;
}

static uint64
compute_query_tree_hash(List *query_list)
{
	char	   *text;
	Size		text_len;
	uint64		hash;

	if (query_list == NIL)
		return UINT64CONST(0);

	text = nodeToString(query_list);
	text_len = strlen(text);

	if (text_len > PG_INT32_MAX)
	{
		pfree(text);
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("query tree serialization too large to hash (%zu bytes)",
						text_len)));
	}

	hash = hash_bytes_extended((const unsigned char *) text,
							   (int) text_len,
							   UINT64CONST(0));
	pfree(text);
	return hash;
}

/*
 * ComputeSharedPlanKey - compute a complete SharedPlanKey for the given plan.
 *
 * Returns true if a valid key was computed.  Returns false if the plan is
 * not shareable; reject_reason is populated in that case.
 *
 * The key is always zero-initialized at entry regardless of return value.
 */
bool
ComputeSharedPlanKey(CachedPlanSource *plansource,
					 CachedPlan *plan,
					 bool is_generic_plan,
					 SharedPlanKey *key,
					 SharedPlanRejectReason *reject_reason)
{
	Assert(plansource != NULL);
	Assert(key != NULL);
	Assert(reject_reason != NULL);

	memset(key, 0, sizeof(SharedPlanKey));

	if (!PlanIsShareable(plansource, plan, is_generic_plan, reject_reason))
		return false;

	Assert(plansource->query_list != NIL);
	Assert(IsA(linitial(plansource->query_list), Query));

	key->queryid = ((Query *) linitial(plansource->query_list))->queryId;
	key->dbid = MyDatabaseId;
	key->roleid = GetUserId();
	key->num_params = plansource->num_params;
	key->cursor_options = plansource->cursor_options;

	key->param_signature_hash = compute_param_signature_hash(plansource);
	key->query_tree_hash = compute_query_tree_hash(plansource->query_list);
	key->search_path_hash = GetSearchPathHash();
	key->planner_gucs_hash = GetPlannerGucHash();
	key->collation_hash = compute_collation_hash(plansource->query_list);

	return true;
}

/*
 * SharedPlanExtractRelationDeps - extract deduplicated relation OIDs from plan.
 *
 * Iterates PlannedStmt.relationOids, deduplicates, and returns a palloc'd
 * Oid array.  Rejects plans with runtime partition pruning (partPruneInfos)
 * or temp relations.
 *
 * Returns true if extraction succeeded.  On rejection, sets *reject_out
 * to the appropriate status and returns false.  Caller must pfree rel_oids
 * when done.
 */
static bool
SharedPlanExtractRelationDeps(List *stmt_list, Oid **rel_oids_out,
							  int *num_rels_out,
							  SharedPlanStoreStatus *reject_out)
{
	List	   *relid_list = NIL;
	ListCell   *lc;
	int			idx;

	*rel_oids_out = NULL;
	*num_rels_out = 0;

	foreach(lc, stmt_list)
	{
		PlannedStmt *pstmt = lfirst_node(PlannedStmt, lc);
		ListCell   *rlc;

		if (pstmt->commandType == CMD_UTILITY)
			continue;

		if (pstmt->partPruneInfos != NIL)
		{
			list_free(relid_list);
			*reject_out = SHARED_PLAN_STORE_REJECTED;
			return false;
		}

		foreach(rlc, pstmt->relationOids)
		{
			Oid			reloid = lfirst_oid(rlc);

			if (!OidIsValid(reloid))
				continue;

			if (isAnyTempNamespace(get_rel_namespace(reloid)))
			{
				list_free(relid_list);
				*reject_out = SHARED_PLAN_STORE_REJECTED;
				return false;
			}

			if (!list_member_oid(relid_list, reloid))
				relid_list = lappend_oid(relid_list, reloid);
		}
	}

	*num_rels_out = list_length(relid_list);
	if (*num_rels_out > 0)
	{
		*rel_oids_out = (Oid *) palloc(sizeof(Oid) * (*num_rels_out));
		idx = 0;
		foreach(lc, relid_list)
			(*rel_oids_out)[idx++] = lfirst_oid(lc);
	}
	list_free(relid_list);
	return true;
}

/*
 * SharedPlanPrepareStorePayloads - serialize plan and allocate DSA payloads.
 *
 * Serializes plan to local buffer, allocates DSA for serialized_plan and
 * relation_oids array, copies data.  Frees local buffer on completion.
 * No current_entries reservation.
 *
 * On success: *dp_out and *rel_oids_dp_out are valid DSA pointers (or
 * InvalidDsaPointer if no relation oids).  *local_len_out is the plan size.
 * Returns SHARED_PLAN_STORE_OK.
 *
 * On failure: returns appropriate status.  Caller owns no DSA payloads.
 */
static SharedPlanStoreStatus
SharedPlanPrepareStorePayloads(List *stmt_list, int max_entry_size_kb,
							   Oid *rel_oids, int num_rel_oids,
							   dsa_pointer *dp_out, Size *local_len_out,
							   dsa_pointer *rel_oids_dp_out)
{
	SharedPlanSerializeStatus ser_status;
	char	   *local_buf = NULL;
	Size		local_len = 0;
	dsa_pointer dp = InvalidDsaPointer;
	dsa_pointer rel_oids_dp = InvalidDsaPointer;

	*dp_out = InvalidDsaPointer;
	*local_len_out = 0;
	*rel_oids_dp_out = InvalidDsaPointer;

	/* Serialize to local buffer */
	ser_status = SharedPlanSerializeStmtList(stmt_list,
											 mul_size((Size) max_entry_size_kb, 1024),
											 &local_buf, &local_len);
	switch (ser_status)
	{
		case SHARED_PLAN_SERIALIZE_OK:
			break;
		case SHARED_PLAN_SERIALIZE_OVERSIZE:
			return SHARED_PLAN_STORE_OVERSIZE;
		case SHARED_PLAN_SERIALIZE_OOM:
			return SHARED_PLAN_STORE_OOM;
		case SHARED_PLAN_SERIALIZE_NOT_SHAREABLE:
		case SHARED_PLAN_SERIALIZE_INVALID_INPUT:
			elog(ERROR, "unexpected serialization status %d", ser_status);
			return SHARED_PLAN_STORE_OOM;
	}

	/* Allocate DSA for relation_oids array */
	if (num_rel_oids > 0)
	{
		Size		rel_oids_size = sizeof(Oid) * num_rel_oids;

		rel_oids_dp = dsa_allocate_extended(shared_plan_dsa, rel_oids_size,
											DSA_ALLOC_NO_OOM);
		if (!DsaPointerIsValid(rel_oids_dp))
		{
			pfree(local_buf);
			return SHARED_PLAN_STORE_OOM;
		}
		memcpy(dsa_get_address(shared_plan_dsa, rel_oids_dp),
			   rel_oids, rel_oids_size);
	}

	/* Allocate DSA for serialized plan */
	dp = dsa_allocate_extended(shared_plan_dsa, local_len, DSA_ALLOC_NO_OOM);
	if (!DsaPointerIsValid(dp))
	{
		if (DsaPointerIsValid(rel_oids_dp))
			dsa_free(shared_plan_dsa, rel_oids_dp);
		pfree(local_buf);
		return SHARED_PLAN_STORE_OOM;
	}
	memcpy(dsa_get_address(shared_plan_dsa, dp), local_buf, local_len);
	pfree(local_buf);

	*dp_out = dp;
	*local_len_out = local_len;
	*rel_oids_dp_out = rel_oids_dp;
	return SHARED_PLAN_STORE_OK;
}

/*
 * SharedPlanPopulateRelationDepIndex - populate relid -> SharedPlanKey[] index.
 *
 * For each relation OID, appends the SharedPlanKey to the dep-index array.
 * Suppresses duplicate SharedPlanKey entries.  Grows arrays without freeing
 * old arrays (conservative no-free policy).  Obeys dep_hash -> plan_hash
 * lock order: all dep_hash locks are released before return.
 *
 * Returns true on success, false on OOM.
 */
static bool
SharedPlanPopulateRelationDepIndex(SharedPlanKey *key, Oid *rel_oids,
								   int num_rel_oids)
{
	int			i;

	for (i = 0; i < num_rel_oids; i++)
	{
		SharedPlanDepEntry *dep_entry;
		bool		dep_found;
		SharedPlanKey *dep_keys;
		bool		already_tracked = false;
		int			j;

		dep_entry = dshash_find_or_insert_extended(shared_plan_dep_hash,
												   &rel_oids[i],
												   &dep_found,
												   DSHASH_INSERT_NO_OOM);
		if (dep_entry == NULL)
			return false;

		if (!dep_found)
		{
			dsa_pointer new_arr;
			Size		arr_size = sizeof(SharedPlanKey) * 16;

			new_arr = dsa_allocate_extended(shared_plan_dsa, arr_size,
										   DSA_ALLOC_NO_OOM);
			if (!DsaPointerIsValid(new_arr))
			{
				dshash_delete_entry(shared_plan_dep_hash, dep_entry);
				return false;
			}
			dep_entry->array_ptr = new_arr;
			dep_entry->num_entries = 0;
			dep_entry->capacity = 16;
		}

		dep_keys = (SharedPlanKey *) dsa_get_address(shared_plan_dsa,
													 dep_entry->array_ptr);
		for (j = 0; j < dep_entry->num_entries; j++)
		{
			if (memcmp(&dep_keys[j], key, sizeof(SharedPlanKey)) == 0)
			{
				already_tracked = true;
				break;
			}
		}

		if (already_tracked)
		{
			dshash_release_lock(shared_plan_dep_hash, dep_entry);
			continue;
		}

		/* Grow array if full (old array NOT freed) */
		if (dep_entry->num_entries >= dep_entry->capacity)
		{
			dsa_pointer new_arr;
			int32		new_cap = dep_entry->capacity * 2;
			Size		new_size = sizeof(SharedPlanKey) * new_cap;

			new_arr = dsa_allocate_extended(shared_plan_dsa, new_size,
										   DSA_ALLOC_NO_OOM);
			if (!DsaPointerIsValid(new_arr))
			{
				dshash_release_lock(shared_plan_dep_hash, dep_entry);
				return false;
			}

			memcpy(dsa_get_address(shared_plan_dsa, new_arr),
				   dsa_get_address(shared_plan_dsa, dep_entry->array_ptr),
				   sizeof(SharedPlanKey) * dep_entry->num_entries);

			dep_entry->array_ptr = new_arr;
			dep_entry->capacity = new_cap;

			dep_keys = (SharedPlanKey *) dsa_get_address(shared_plan_dsa,
														 dep_entry->array_ptr);
		}

		memcpy(&dep_keys[dep_entry->num_entries], key, sizeof(SharedPlanKey));
		dep_entry->num_entries++;

		dshash_release_lock(shared_plan_dep_hash, dep_entry);
	}

	return true;
}

/*
 * SharedPlanPublishEntry - insert/overwrite primary entry and publish.
 *
 * Reserves current_entries, performs dshash find/insert, handles valid
 * duplicate and stale overwrite, rechecks relcache_store_epoch and
 * global_generation, and publishes is_valid=1.
 *
 * On success: returns SHARED_PLAN_STORE_OK.  DSA payloads are owned by
 * the published entry.  Caller must not free them.
 *
 * On failure: returns appropriate status.  DSA payloads may or may not
 * be owned by the caller depending on the failure path.
 * *payloads_installed_out indicates whether payloads were installed into
 * a shared entry (true = caller must NOT free them).
 */
static SharedPlanStoreStatus
SharedPlanPublishEntry(SharedPlanKey *key, dsa_pointer dp, Size local_len,
					   dsa_pointer rel_oids_dp, int num_rel_oids,
					   double generic_cost,
					   uint64 saved_store_epoch, uint64 saved_global_generation,
					   bool *payloads_installed_out)
{
	SharedPlanEntry *entry;
	bool		found;
	bool		new_slot_reserved = false;

	*payloads_installed_out = false;

	/* Insert into dshash (acquires exclusive partition lock) */
	entry = dshash_find_or_insert_extended(shared_plan_hash, key, &found,
										   DSHASH_INSERT_NO_OOM);
	if (entry == NULL)
		return SHARED_PLAN_STORE_OOM;

	if (found)
	{
		uint64		cur_gen = pg_atomic_read_u64(&shared_plan_ctl->generation);
		bool		entry_is_stale = (pg_atomic_read_u32(&entry->is_valid) == 0 ||
									  entry->generation != cur_gen);

		if (!entry_is_stale)
		{
			/* Entry is valid and current — true duplicate */
			dshash_release_lock(shared_plan_hash, entry);
			return SHARED_PLAN_STORE_DUPLICATE;
		}

		/*
		 * Stale or invalidated entry — overwrite it.  Do NOT free old DSA
		 * payloads (conservative no-free-until-reset policy).
		 * No current_entries change: reusing the existing slot.
		 */
	}
	else
	{
		/*
		 * New entry — reserve a current_entries slot.  If the cache is full,
		 * delete the newly inserted (empty) dshash entry and return FULL.
		 */
		uint32		cur;

		for (;;)
		{
			cur = pg_atomic_read_u32(&shared_plan_ctl->current_entries);
			if (cur >= (uint32) shared_plan_ctl->max_entries)
			{
				dshash_delete_entry(shared_plan_hash, entry);
				return SHARED_PLAN_STORE_FULL;
			}
			if (pg_atomic_compare_exchange_u32(&shared_plan_ctl->current_entries,
											   &cur, cur + 1))
				break;
		}
		new_slot_reserved = true;
	}

	/*
	 * Initialize/overwrite entry with is_valid=0 (not yet published).
	 * For stale overwrites: must set is_valid=0 under exclusive lock because
	 * generation-stale entries may still have is_valid=1.
	 */
	if (!found)
	{
		pg_atomic_init_u32(&entry->refcount, 0);
		pg_atomic_init_u32(&entry->is_valid, 0);
	}
	else
	{
		pg_atomic_write_u32(&entry->is_valid, 0);
	}

	entry->serialized_plan = dp;
	entry->serialized_plan_len = local_len;
	entry->generic_cost = generic_cost;
	entry->relation_oids = rel_oids_dp;
	entry->num_relation_oids = num_rel_oids;
	entry->inval_items = InvalidDsaPointer;
	entry->num_inval_items = 0;

	*payloads_installed_out = true;

	/*
	 * Test-only hook: bump generation before the recheck to simulate
	 * a concurrent syscache invalidation during the store window.
	 */
	if (shared_plan_test_force_generation_bump_before_publish)
	{
		pg_atomic_fetch_add_u64(&shared_plan_ctl->generation, 1);
		shared_plan_test_force_generation_bump_before_publish = false;
	}

	/*
	 * Recheck relcache_store_epoch and global_generation before publishing.
	 *
	 * Case A (new insert): delete entry, undo reservation.
	 * Case B (stale overwrite): leave is_valid=0, do not delete/decrement.
	 */
	{
		uint64		current_epoch;
		uint64		current_gen;

		current_epoch = pg_atomic_read_u64(&shared_plan_ctl->relcache_store_epoch);
		current_gen = pg_atomic_read_u64(&shared_plan_ctl->generation);

		if (current_epoch != saved_store_epoch ||
			current_gen != saved_global_generation)
		{
			if (!found)
			{
				/* Case A: newly inserted — delete and free */
				dshash_delete_entry(shared_plan_hash, entry);
				if (new_slot_reserved)
					pg_atomic_fetch_sub_u32(&shared_plan_ctl->current_entries, 1);
				*payloads_installed_out = false;
			}
			else
			{
				/* Case B: stale overwrite — leave invalid, release lock */
				dshash_release_lock(shared_plan_hash, entry);
				/* payloads remain installed in invalid entry */
			}
			return SHARED_PLAN_STORE_INVALID;
		}
	}

	/* Publish — set generation and is_valid */
	entry->generation = saved_global_generation;
	pg_atomic_write_u32(&entry->is_valid, 1);
	dshash_release_lock(shared_plan_hash, entry);

	if (shared_plan_cache_enabled)
		l2_store_count++;

	return SHARED_PLAN_STORE_OK;
}

/*
 * SharedPlanFreeStorePayloads - free DSA payloads from a failed store.
 *
 * Frees only NEW payloads allocated by the current store attempt.
 * Must NOT free old stale-entry payloads (Patch 0010 safety).
 */
static void
SharedPlanFreeStorePayloads(dsa_pointer dp, dsa_pointer rel_oids_dp)
{
	if (DsaPointerIsValid(dp))
		dsa_free(shared_plan_dsa, dp);
	if (DsaPointerIsValid(rel_oids_dp))
		dsa_free(shared_plan_dsa, rel_oids_dp);
}

/*
 * SharedPlanCacheStore - store a serialized plan in the shared plan cache.
 *
 * Orchestrates: precondition checks, key computation, dependency extraction,
 * payload preparation, dep-index population, and entry publication.
 *
 * DUPLICATE takes precedence over FULL.  The dshash partition lock is never
 * held during serialization, DSA allocation, or memcpy.
 *
 * Returns the store status.  out_key is populated on all paths if non-NULL.
 */
SharedPlanStoreStatus
SharedPlanCacheStore(CachedPlanSource *plansource,
					 CachedPlan *plan,
					 bool is_generic_plan,
					 uint64 planning_start_generation,
					 SharedPlanKey *out_key,
					 SharedPlanRejectReason *reject_reason)
{
	SharedPlanKey key;
	SharedPlanEntry *existing;
	SharedPlanStoreStatus status;
	dsa_pointer dp = InvalidDsaPointer;
	dsa_pointer rel_oids_dp = InvalidDsaPointer;
	Size		local_len = 0;
	double		generic_cost;
	uint64		saved_store_epoch;
	bool		payloads_installed;

	/* Dependency extraction locals */
	Oid		   *rel_oids = NULL;
	int			num_rel_oids = 0;

	Assert(reject_reason != NULL);

	*reject_reason = SHARED_PLAN_REJECT_NONE;

	/* Step 1: Check preconditions */
	if (!SharedPlanCacheIsActive())
	{
		if (out_key)
			memset(out_key, 0, sizeof(SharedPlanKey));
		return SHARED_PLAN_STORE_DISABLED;
	}

	/* Step 2: Compute key (includes PlanIsShareable check) */
	if (!ComputeSharedPlanKey(plansource, plan, is_generic_plan,
							  &key, reject_reason))
	{
		if (out_key)
			memcpy(out_key, &key, sizeof(SharedPlanKey));
		return SHARED_PLAN_STORE_NOT_SHAREABLE;
	}

	if (out_key)
		memcpy(out_key, &key, sizeof(SharedPlanKey));

	/* Step 3: Compute generic_cost */
	generic_cost = shared_plan_compute_generic_cost(plan->stmt_list);
	if (generic_cost < 0)
		return SHARED_PLAN_STORE_NOT_SHAREABLE;

	/* Step 4: Duplicate/stale pre-check (shared lock, no reservation) */
	existing = dshash_find(shared_plan_hash, &key, false);
	if (existing != NULL)
	{
		uint64		cur_gen = pg_atomic_read_u64(&shared_plan_ctl->generation);
		bool		is_stale = (pg_atomic_read_u32(&existing->is_valid) == 0 ||
								existing->generation != cur_gen);

		dshash_release_lock(shared_plan_hash, existing);
		if (!is_stale)
			return SHARED_PLAN_STORE_DUPLICATE;
		/* Stale entry found: proceed — stale overwrite does not need a new slot */
	}

	/*
	 * Step 5: Racy FULL pre-check.  Skip if Step 4 found a stale entry for
	 * the same key — stale overwrite reuses the existing slot without
	 * consuming a new current_entries slot.
	 */
	if (existing == NULL &&
		pg_atomic_read_u32(&shared_plan_ctl->current_entries) >=
		(uint32) shared_plan_ctl->max_entries)
		return SHARED_PLAN_STORE_FULL;

	/*
	 * Step 6: Capture saved_store_epoch BEFORE dep-index work.
	 *
	 * planning_start_generation was captured by the caller before
	 * BuildCachedPlan() and passed in.  This closes the race where a
	 * concurrent syscache DDL bumps global_generation after planning but
	 * before this function starts.  The publish recheck (Step 10) compares
	 * current global_generation against planning_start_generation.
	 */
	saved_store_epoch = pg_atomic_read_u64(&shared_plan_ctl->relcache_store_epoch);

	/* Step 7: Extract relation dependencies and reject unsafe plans */
	{
		SharedPlanStoreStatus dep_reject;

		if (!SharedPlanExtractRelationDeps(plan->stmt_list, &rel_oids,
										   &num_rel_oids, &dep_reject))
			return dep_reject;
	}

	/* Step 8: Serialize plan and prepare DSA payloads */
	status = SharedPlanPrepareStorePayloads(plan->stmt_list,
											shared_plan_ctl->max_entry_size_kb,
											rel_oids, num_rel_oids,
											&dp, &local_len, &rel_oids_dp);
	if (status != SHARED_PLAN_STORE_OK)
	{
		if (rel_oids)
			pfree(rel_oids);
		return status;
	}

	/* Step 9: Populate relation dep-index */
	if (num_rel_oids > 0)
	{
		if (!SharedPlanPopulateRelationDepIndex(&key, rel_oids, num_rel_oids))
		{
			SharedPlanFreeStorePayloads(dp, rel_oids_dp);
			if (rel_oids)
				pfree(rel_oids);
			return SHARED_PLAN_STORE_OOM;
		}
	}

	/* Step 10: Publish entry (reserve, insert, recheck, publish) */
	status = SharedPlanPublishEntry(&key, dp, local_len, rel_oids_dp,
									num_rel_oids, generic_cost,
									saved_store_epoch, planning_start_generation,
									&payloads_installed);

	if (status != SHARED_PLAN_STORE_OK)
	{
		/* Free payloads only if not installed into a shared entry */
		if (!payloads_installed)
			SharedPlanFreeStorePayloads(dp, rel_oids_dp);
		if (rel_oids)
			pfree(rel_oids);
		return status;
	}

	if (rel_oids)
		pfree(rel_oids);

	return SHARED_PLAN_STORE_OK;
}

/* ---- Relcache Invalidation Callback (Patch 0008) ---- */

/*
 * SharedPlanCacheRelCallback - relcache invalidation callback.
 *
 * For specific relid: marks dependent entries is_valid=0 via dep-index.
 *   Bumps relcache_store_epoch but NOT generation.
 * For InvalidOid: bumps both generation and relcache_store_epoch.
 *   Does not scan dep-index.
 */
static void
SharedPlanCacheRelCallback(Datum arg, Oid relid)
{
	SharedPlanDepEntry *dep_entry;
	SharedPlanKey *keys;
	int32		nkeys;
	int			i;

	if (shared_plan_ctl == NULL || shared_plan_ctl->max_entries == 0)
		return;

	/* Always bump relcache_store_epoch for store-time race detection */
	pg_atomic_fetch_add_u64(&shared_plan_ctl->relcache_store_epoch, 1);

	if (relid == InvalidOid)
	{
		/* Broad invalidation: bump generation so all older entries miss */
		pg_atomic_fetch_add_u64(&shared_plan_ctl->generation, 1);
		return;
	}

	/*
	 * If dep_hash/plan_hash are not attached (should not happen after
	 * attach fix, but defensive), bump generation for safety so stale
	 * entries do not survive as false hits.
	 */
	if (shared_plan_dep_hash == NULL || shared_plan_hash == NULL)
	{
		pg_atomic_fetch_add_u64(&shared_plan_ctl->generation, 1);
		return;
	}

	/* Specific relation: use dep-index for precise invalidation */
	dep_entry = dshash_find(shared_plan_dep_hash, &relid, false);
	if (dep_entry == NULL)
		return;

	keys = (SharedPlanKey *) dsa_get_address(shared_plan_dsa,
											  dep_entry->array_ptr);
	nkeys = dep_entry->num_entries;

	for (i = 0; i < nkeys; i++)
	{
		SharedPlanEntry *plan_entry;

		plan_entry = dshash_find(shared_plan_hash, &keys[i], false);
		if (plan_entry != NULL)
		{
			pg_atomic_write_u32(&plan_entry->is_valid, 0);
			dshash_release_lock(shared_plan_hash, plan_entry);
		}
		/* stale key: dshash_find returns NULL, skip silently */
	}

	dshash_release_lock(shared_plan_dep_hash, dep_entry);
}

/*
 * SharedPlanCacheSysCallback - syscache invalidation callback (Patch 0009).
 *
 * Registered for PROCOID, TYPEOID, NAMESPACEOID, OPEROID, AMOPOPID,
 * FOREIGNSERVEROID, and FOREIGNDATAWRAPPEROID.  All classes receive identical
 * coarse treatment: bump global_generation so entries with older generation
 * become lookup-stale.  No per-object precision — future improvement.
 *
 * Must not ERROR, allocate memory, scan hash tables, or deserialize plans.
 */
static void
SharedPlanCacheSysCallback(Datum arg, SysCacheIdentifier cacheid,
						   uint32 hashvalue)
{
	if (shared_plan_ctl == NULL || shared_plan_ctl->max_entries == 0)
		return;

	pg_atomic_fetch_add_u64(&shared_plan_ctl->generation, 1);
}

/* ---- L2 Lookup (Patch 0007) ---- */

/*
 * ComputeSharedPlanKeyForLookup - compute SharedPlanKey from plansource only.
 *
 * This is a lookup-specific variant of ComputeSharedPlanKey() that does not
 * require a CachedPlan.  It performs plansource-level prechecks only and
 * computes the same 10 key dimensions.
 *
 * Returns true if a valid key was computed, false if plansource-level
 * prechecks failed.  May raise ERROR from nodeToString/hash paths.
 */
bool
ComputeSharedPlanKeyForLookup(CachedPlanSource *plansource,
							   SharedPlanKey *key,
							   SharedPlanRejectReason *reject_reason)
{
	Assert(plansource != NULL);
	Assert(key != NULL);
	Assert(reject_reason != NULL);

	memset(key, 0, sizeof(SharedPlanKey));
	*reject_reason = SHARED_PLAN_REJECT_NONE;

	/* Plansource-level prechecks (no CachedPlan required) */
	if (!plansource->is_complete)
	{
		*reject_reason = SHARED_PLAN_REJECT_INCOMPLETE;
		return false;
	}

	if (plansource->is_oneshot)
	{
		*reject_reason = SHARED_PLAN_REJECT_ONESHOT;
		return false;
	}

	if (plansource->postRewrite != NULL)
	{
		*reject_reason = SHARED_PLAN_REJECT_POST_REWRITE_HOOK;
		return false;
	}

	if (HasUnsafePlannerHooks())
	{
		*reject_reason = SHARED_PLAN_REJECT_PLANNER_HOOK;
		return false;
	}

	if (plansource->dependsOnRLS)
	{
		*reject_reason = SHARED_PLAN_REJECT_DEPENDS_ON_RLS;
		return false;
	}

	if (plansource->query_list == NIL)
	{
		*reject_reason = SHARED_PLAN_REJECT_INCOMPLETE;
		return false;
	}

	/* Compute key dimensions (same as ComputeSharedPlanKey) */
	Assert(plansource->query_list != NIL);
	Assert(IsA(linitial(plansource->query_list), Query));

	key->queryid = ((Query *) linitial(plansource->query_list))->queryId;
	key->dbid = MyDatabaseId;
	key->roleid = GetUserId();
	key->num_params = plansource->num_params;
	key->cursor_options = plansource->cursor_options;

	key->param_signature_hash = compute_param_signature_hash(plansource);
	key->query_tree_hash = compute_query_tree_hash(plansource->query_list);
	key->search_path_hash = GetSearchPathHash();
	key->planner_gucs_hash = GetPlannerGucHash();
	key->collation_hash = compute_collation_hash(plansource->query_list);

	return true;
}

/*
 * SharedPlanCacheLookup - low-level L2 lookup.
 *
 * Looks up a shared plan entry by key computed from plansource.
 * On HIT, deserializes the entry into target_mcxt and returns the
 * stmt_list and generic_cost.
 *
 * Does not increment refcount.  Does not mutate shared entries.
 * DSA pointer remains valid after lock release (no-free guarantee).
 */
SharedPlanLookupStatus
SharedPlanCacheLookup(CachedPlanSource *plansource,
					   MemoryContext target_mcxt,
					   List **out_stmt_list,
					   double *out_generic_cost)
{
	SharedPlanKey key;
	SharedPlanRejectReason reject;
	SharedPlanEntry *entry;
	dsa_pointer ser_plan;
	Size		ser_len;
	double		cost;
	SharedPlanSerializeStatus deser_status;

	Assert(out_stmt_list != NULL);
	Assert(out_generic_cost != NULL);

	*out_stmt_list = NIL;
	*out_generic_cost = -1;

	if (!SharedPlanCacheIsActive() ||
		shared_plan_hash == NULL ||
		shared_plan_dsa == NULL)
	{
		last_l2_status = SHARED_PLAN_LOOKUP_DISABLED;
		return SHARED_PLAN_LOOKUP_DISABLED;
	}

	if (!ComputeSharedPlanKeyForLookup(plansource, &key, &reject))
	{
		last_l2_status = SHARED_PLAN_LOOKUP_NOT_SHAREABLE;
		l2_miss_count++;
		return SHARED_PLAN_LOOKUP_NOT_SHAREABLE;
	}

	/* Find entry under shared lock */
	entry = dshash_find(shared_plan_hash, &key, false);
	if (entry == NULL)
	{
		last_l2_status = SHARED_PLAN_LOOKUP_MISS;
		l2_miss_count++;
		return SHARED_PLAN_LOOKUP_MISS;
	}

	/* Check validity */
	if (pg_atomic_read_u32(&entry->is_valid) == 0)
	{
		dshash_release_lock(shared_plan_hash, entry);
		last_l2_status = SHARED_PLAN_LOOKUP_INVALID;
		l2_miss_count++;
		return SHARED_PLAN_LOOKUP_INVALID;
	}

	/* Check generation (Patch 0008: broad relcache invalidation) */
	{
		uint64		current_gen = pg_atomic_read_u64(&shared_plan_ctl->generation);

		if (entry->generation != current_gen)
		{
			dshash_release_lock(shared_plan_hash, entry);
			last_l2_status = SHARED_PLAN_LOOKUP_INVALID;
			l2_miss_count++;
			return SHARED_PLAN_LOOKUP_INVALID;
		}
	}

	/* Copy fields while holding lock */
	ser_plan = entry->serialized_plan;
	ser_len = entry->serialized_plan_len;
	cost = entry->generic_cost;

	dshash_release_lock(shared_plan_hash, entry);

	/* Validate generic_cost */
	if (isnan(cost) || cost < 0)
	{
		last_l2_status = SHARED_PLAN_LOOKUP_MISS;
		l2_miss_count++;
		return SHARED_PLAN_LOOKUP_MISS;
	}

	/* Deserialize outside lock */
	deser_status = SharedPlanDeserializeFromDSA(shared_plan_dsa,
												ser_plan, ser_len,
												target_mcxt,
												out_stmt_list);
	if (deser_status != SHARED_PLAN_SERIALIZE_OK)
	{
		last_l2_status = SHARED_PLAN_LOOKUP_DESER_ERROR;
		l2_error_count++;
		return SHARED_PLAN_LOOKUP_DESER_ERROR;
	}

	*out_generic_cost = cost;
	last_l2_status = SHARED_PLAN_LOOKUP_HIT;
	l2_hit_count++;
	return SHARED_PLAN_LOOKUP_HIT;
}

/* Test-only counter accessors */

uint64
SharedPlanCacheL2HitCount(void)
{
	return l2_hit_count;
}

uint64
SharedPlanCacheL2MissCount(void)
{
	return l2_miss_count;
}

uint64
SharedPlanCacheL2StoreCount(void)
{
	return l2_store_count;
}

uint64
SharedPlanCacheL2ErrorCount(void)
{
	return l2_error_count;
}

void
SharedPlanCacheL2CountError(void)
{
	l2_error_count++;
	last_l2_status = SHARED_PLAN_LOOKUP_ERROR;
}

const char *
SharedPlanCacheLastL2StatusName(void)
{
	switch (last_l2_status)
	{
		case SHARED_PLAN_LOOKUP_NONE:
			return "NONE";
		case SHARED_PLAN_LOOKUP_HIT:
			return "HIT";
		case SHARED_PLAN_LOOKUP_MISS:
			return "MISS";
		case SHARED_PLAN_LOOKUP_DISABLED:
			return "DISABLED";
		case SHARED_PLAN_LOOKUP_NOT_SHAREABLE:
			return "NOT_SHAREABLE";
		case SHARED_PLAN_LOOKUP_DESER_ERROR:
			return "DESER_ERROR";
		case SHARED_PLAN_LOOKUP_INVALID:
			return "INVALID";
		case SHARED_PLAN_LOOKUP_ERROR:
			return "ERROR";
	}
	return "UNKNOWN";
}

/* ---- Patch 0008 test accessors ---- */

dshash_table *
SharedPlanCacheGetDepHash(void)
{
	return shared_plan_dep_hash;
}

uint64
SharedPlanCacheGeneration(void)
{
	if (shared_plan_ctl == NULL)
		return 0;
	return pg_atomic_read_u64(&shared_plan_ctl->generation);
}

uint64
SharedPlanCacheRelcacheStoreEpoch(void)
{
	if (shared_plan_ctl == NULL)
		return 0;
	return pg_atomic_read_u64(&shared_plan_ctl->relcache_store_epoch);
}

void
SharedPlanCacheTestArmGenerationBump(void)
{
	shared_plan_test_force_generation_bump_before_publish = true;
}
