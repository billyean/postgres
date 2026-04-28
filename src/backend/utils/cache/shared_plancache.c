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
static bool queryid_warning_given = false;
static bool attach_warning_given = false;
static dsa_area *shared_plan_dsa = NULL;
static dshash_table *shared_plan_hash = NULL;
static dshash_table *shared_plan_dep_hash = NULL;

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

	if (shared_plan_cache_attached)
		return;

	if (shared_plan_ctl == NULL || shared_plan_ctl->max_entries == 0)
		return;

	if (!IsQueryIdEnabled())
	{
		if (!queryid_warning_given)
		{
			ereport(WARNING,
					(errmsg("shared plan cache disabled for this backend: compute_query_id is off")));
			queryid_warning_given = true;
		}
		return;
	}

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

	/* Attach to DSA area */
	dsa = dsa_attach_in_place(shared_plan_ctl->raw_dsa_area, NULL);
	dsa_pin_mapping(dsa);
	shared_plan_dsa = dsa;

	/* Attach to plan hash */
	params = shared_plan_hash_params;
	params.tranche_id = shared_plan_ctl->tranche_id;
	shared_plan_hash = dshash_attach(dsa, &params,
									 shared_plan_ctl->plan_hash_handle,
									 NULL);
	Assert(shared_plan_hash != NULL);

	/* Attach to dependency hash */
	params = shared_plan_dep_hash_params;
	params.tranche_id = shared_plan_ctl->tranche_id;
	shared_plan_dep_hash = dshash_attach(dsa, &params,
										 shared_plan_ctl->dep_hash_handle,
										 NULL);
	Assert(shared_plan_dep_hash != NULL);

	shared_plan_cache_attached = true;
	shared_plan_cache_active = true;

	before_shmem_exit(SharedPlanCacheShutdown, (Datum) 0);
}

/* Accessors for test modules */

bool
SharedPlanCacheIsActive(void)
{
	return shared_plan_cache_active;
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
 * SharedPlanCacheStore - store a serialized plan in the shared plan cache.
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
					 SharedPlanKey *out_key,
					 SharedPlanRejectReason *reject_reason)
{
	SharedPlanKey key;
	SharedPlanEntry *entry;
	SharedPlanEntry *existing;
	SharedPlanSerializeStatus ser_status;
	char	   *local_buf = NULL;
	Size		local_len = 0;
	dsa_pointer dp = InvalidDsaPointer;
	double		generic_cost;
	uint32		cur;
	bool		found;

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

	/* Step 3: Compute generic_cost before any shared resource commit */
	generic_cost = shared_plan_compute_generic_cost(plan->stmt_list);
	if (generic_cost < 0)
		return SHARED_PLAN_STORE_NOT_SHAREABLE;

	/* Step 4: Duplicate pre-check (shared lock, no reservation) */
	existing = dshash_find(shared_plan_hash, &key, false);
	if (existing != NULL)
	{
		dshash_release_lock(shared_plan_hash, existing);
		return SHARED_PLAN_STORE_DUPLICATE;
	}

	/* Step 5: Racy FULL pre-check */
	if (pg_atomic_read_u32(&shared_plan_ctl->current_entries) >=
		(uint32) shared_plan_ctl->max_entries)
		return SHARED_PLAN_STORE_FULL;

	/* Step 6: Serialize to local buffer */
	ser_status = SharedPlanSerializeStmtList(plan->stmt_list,
											 mul_size((Size) shared_plan_ctl->max_entry_size_kb, 1024),
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
			elog(ERROR, "unexpected serialization status %d from SharedPlanSerializeStmtList", ser_status);
			return SHARED_PLAN_STORE_OOM;	/* unreachable, keeps compiler quiet */
	}

	/* Step 7: Allocate DSA payload and copy (no dshash lock held) */
	dp = dsa_allocate_extended(shared_plan_dsa, local_len, DSA_ALLOC_NO_OOM);
	if (!DsaPointerIsValid(dp))
	{
		pfree(local_buf);
		return SHARED_PLAN_STORE_OOM;
	}
	memcpy(dsa_get_address(shared_plan_dsa, dp), local_buf, local_len);
	pfree(local_buf);

	/* Step 8: Reserve entry slot (atomic CAS loop) */
	for (;;)
	{
		cur = pg_atomic_read_u32(&shared_plan_ctl->current_entries);
		if (cur >= (uint32) shared_plan_ctl->max_entries)
		{
			dsa_free(shared_plan_dsa, dp);
			return SHARED_PLAN_STORE_FULL;
		}
		if (pg_atomic_compare_exchange_u32(&shared_plan_ctl->current_entries,
										   &cur, cur + 1))
			break;
	}

	/* Step 9: Insert into dshash (acquires exclusive partition lock) */
	entry = dshash_find_or_insert_extended(shared_plan_hash, &key, &found,
										   DSHASH_INSERT_NO_OOM);
	if (entry == NULL)
	{
		dsa_free(shared_plan_dsa, dp);
		pg_atomic_fetch_sub_u32(&shared_plan_ctl->current_entries, 1);
		return SHARED_PLAN_STORE_OOM;
	}

	if (found)
	{
		dshash_release_lock(shared_plan_hash, entry);
		dsa_free(shared_plan_dsa, dp);
		pg_atomic_fetch_sub_u32(&shared_plan_ctl->current_entries, 1);
		return SHARED_PLAN_STORE_DUPLICATE;
	}

	/* Step 10: Initialize entry and commit (partition lock held) */
	pg_atomic_init_u32(&entry->refcount, 0);
	pg_atomic_init_u32(&entry->is_valid, 0);
	entry->generation = pg_atomic_read_u64(&shared_plan_ctl->generation);
	entry->serialized_plan = dp;
	entry->serialized_plan_len = local_len;
	entry->generic_cost = generic_cost;
	entry->relation_oids = InvalidDsaPointer;
	entry->num_relation_oids = 0;
	entry->inval_items = InvalidDsaPointer;
	entry->num_inval_items = 0;

	pg_atomic_write_u32(&entry->is_valid, 1);
	dshash_release_lock(shared_plan_hash, entry);

	if (shared_plan_cache_enabled)
		l2_store_count++;

	return SHARED_PLAN_STORE_OK;
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
