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

#include "lib/dshash.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "nodes/plannodes.h"
#include "nodes/queryjumble.h"
#include "nodes/readfuncs.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/dsa.h"
#include "utils/memutils.h"
#include "utils/shared_plancache.h"

/* GUC variables */
int			shared_plan_cache_max_entries = 1000;
int			shared_plan_cache_max_memory = 32768;	/* KB */
int			shared_plan_cache_max_entry_size = 256;	/* KB */

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
