/*-------------------------------------------------------------------------
 *
 * shared_plancache.h
 *	  Shared plan cache infrastructure definitions.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/shared_plancache.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SHARED_PLANCACHE_H
#define SHARED_PLANCACHE_H

#include "lib/dshash.h"
#include "port/atomics.h"
#include "postgres_ext.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/dsa.h"
#include "utils/plancache.h"

/*
 * SharedPlanKey - flat, pointer-free, memcmp-compatible cache key.
 *
 * Must be zero-initialized with memset() before population to ensure
 * padding bytes are deterministic for memcmp/memhash.
 *
 * Total size: 64 bytes (no implicit padding on typical 64-bit builds).
 *
 * Fields query_tree_hash, param_signature_hash, and collation_hash are
 * defined here but computed in Patch 0006 (ComputeSharedPlanKey).
 * Patch 0004 only defines the struct skeleton.
 */
typedef struct SharedPlanKey
{
	int64		queryid;				/* Query.queryId, retained for grouping/debug */
	Oid			dbid;					/* database OID */
	Oid			roleid;					/* role OID */
	int32		num_params;				/* number of external parameters */
	int32		cursor_options;			/* planner cursor options */

	uint64		param_signature_hash;	/* parameter signature hash */
	uint64		query_tree_hash;		/* analyzed/rewrite query tree fingerprint */
	uint64		search_path_hash;		/* from Patch 0002 */
	uint64		planner_gucs_hash;		/* from Patch 0001 */
	uint64		collation_hash;			/* collation resolution/dependency hash */
} SharedPlanKey;

StaticAssertDecl(sizeof(SharedPlanKey) == 64,
				 "SharedPlanKey must be exactly 64 bytes");

/*
 * SharedPlanEntry - dshash entry for a cached plan.
 * Key (SharedPlanKey) must be the first field.
 */
typedef struct SharedPlanEntry
{
	SharedPlanKey key;

	/* Serialized plan (populated by Patch 0005+) */
	dsa_pointer serialized_plan;
	Size		serialized_plan_len;

	/* Validity and lifecycle */
	pg_atomic_uint32 refcount;
	pg_atomic_uint32 is_valid;		/* 0 = invalid, 1 = valid */
	uint64		generation;			/* snapshot of global generation */

	/* Dependency tracking (populated by Patch 0008+) */
	dsa_pointer relation_oids;		/* DSA-allocated Oid array */
	int32		num_relation_oids;
	dsa_pointer inval_items;		/* DSA-allocated PlanInvalItem array */
	int32		num_inval_items;
} SharedPlanEntry;

/*
 * SharedPlanDepEntry - dshash entry for relation-to-plan dependency tracking.
 * Key (Oid relid) must be the first field.
 */
typedef struct SharedPlanDepEntry
{
	Oid			relid;

	dsa_pointer array_ptr;			/* DSA-allocated SharedPlanKey array */
	int32		num_entries;
	int32		capacity;
} SharedPlanDepEntry;

/*
 * SharedPlanCacheControl - fixed-size control struct in named shared memory.
 *
 * The raw_dsa_area trailing bytes hold the in-place DSA control block.
 * Backends attach via dsa_attach_in_place(ctl->raw_dsa_area, NULL).
 */
typedef struct SharedPlanCacheControl
{
	dshash_table_handle plan_hash_handle;
	dshash_table_handle dep_hash_handle;

	int			tranche_id;

	int			max_entries;
	int			max_memory_kb;
	int			max_entry_size_kb;

	pg_atomic_uint64 generation;
	pg_atomic_uint32 current_entries;

	char		raw_dsa_area[FLEXIBLE_ARRAY_MEMBER];
} SharedPlanCacheControl;

/* GUC variables */
extern PGDLLIMPORT int shared_plan_cache_max_entries;
extern PGDLLIMPORT int shared_plan_cache_max_memory;
extern PGDLLIMPORT int shared_plan_cache_max_entry_size;

/* Shmem callbacks (registered via subsystemlist.h) */
extern const ShmemCallbacks SharedPlanCacheShmemCallbacks;

/* Backend lifecycle */
extern void SharedPlanCacheAttach(void);

/* Accessors for test modules */
extern bool SharedPlanCacheIsActive(void);
extern bool SharedPlanCacheIsAttached(void);
extern bool SharedPlanCacheHandlesValid(void);
extern uint32 SharedPlanCacheCurrentEntries(void);

/* ---- Plan Serialization (Patch 0005) ---- */

#define SHARED_PLAN_SERIAL_MAGIC    0x5350434C	/* "SPCL" */
#define SHARED_PLAN_SERIAL_VERSION  1

typedef enum SharedPlanSerialFormat
{
	SHARED_PLAN_FORMAT_TEXT = 1,
} SharedPlanSerialFormat;

typedef struct SharedPlanSerializedHeader
{
	uint32		magic;
	uint16		version;
	uint16		format;
	uint32		payload_len;
	uint32		num_stmts;
} SharedPlanSerializedHeader;

typedef enum SharedPlanSerializeStatus
{
	SHARED_PLAN_SERIALIZE_OK = 0,
	SHARED_PLAN_SERIALIZE_NOT_SHAREABLE,
	SHARED_PLAN_SERIALIZE_OVERSIZE,
	SHARED_PLAN_SERIALIZE_OOM,
	SHARED_PLAN_SERIALIZE_INVALID_INPUT,
} SharedPlanSerializeStatus;

extern SharedPlanSerializeStatus SharedPlanSerializeStmtList(List *stmt_list,
															 Size max_total_size,
															 char **out_data,
															 Size *out_len);

extern SharedPlanSerializeStatus SharedPlanSerializeCachedPlan(CachedPlanSource *plansource,
															   CachedPlan *plan,
															   bool is_generic_plan,
															   Size max_total_size,
															   char **out_data,
															   Size *out_len,
															   SharedPlanRejectReason *reject_reason);

extern SharedPlanSerializeStatus SharedPlanSerializeToDSA(List *stmt_list,
														  Size max_total_size,
														  dsa_area *area,
														  dsa_pointer *out_ptr,
														  Size *out_len);

extern SharedPlanSerializeStatus SharedPlanDeserialize(const char *data,
													   Size len,
													   MemoryContext target_context,
													   List **out_stmt_list);

extern SharedPlanSerializeStatus SharedPlanDeserializeFromDSA(dsa_area *area,
															  dsa_pointer ptr,
															  Size len,
															  MemoryContext target_context,
															  List **out_stmt_list);

#endif							/* SHARED_PLANCACHE_H */
