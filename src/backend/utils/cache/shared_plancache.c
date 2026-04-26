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
#include "nodes/queryjumble.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/dsa.h"
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
