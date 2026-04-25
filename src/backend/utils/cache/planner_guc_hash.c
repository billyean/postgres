/*-------------------------------------------------------------------------
 *
 * planner_guc_hash.c
 *	  Deterministic hash of planner-affecting GUC state.
 *
 * Computes a uint64 hash over 57 planner-affecting GUCs using explicit
 * field hashing with hash_combine64().  The hash is lazily recomputed:
 * GUC assign hooks mark it dirty, and the next GetPlannerGucHash() call
 * recomputes it.
 *
 * This is a backend-local cache - no shared memory is involved.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/planner_guc_hash.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <string.h>

#include "common/hashfn.h"
#include "jit/jit.h"
#include "miscadmin.h"
#include "optimizer/cost.h"
#include "optimizer/geqo.h"
#include "optimizer/optimizer.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "utils/planner_guc_hash.h"

/*
 * Snapshot of all 57 planner-affecting GUC values.
 *
 * This struct is populated from current GUC variables and then hashed
 * field-by-field using hash_combine64().  It is NOT byte-hashed.
 *
 * When adding a new planner GUC, add a field here, populate it in
 * PopulatePlannerGucState(), hash it in ComputePlannerGucHash(), and
 * add an assign_hook in guc_parameters.dat.
 */
typedef struct PlannerGucState
{
	/* Boolean enable_* flags packed into bitmasks */
	uint32		enable_flags;	/* bits 0-25: 26 enable_* switches */
	uint32		misc_bool_flags;	/* bit 0: jit, bit 1:
									 * parallel_leader_participation */

	/* Enum GUCs stored as int32 */
	int32		constraint_exclusion;
	int32		debug_parallel_query;

	/* Integer GUCs */
	int32		effective_cache_size;
	int32		max_parallel_workers_per_gather;
	int32		min_parallel_table_scan_size;
	int32		min_parallel_index_scan_size;
	int32		geqo_threshold;
	int32		geqo_effort;
	int32		geqo_pool_size;
	int32		geqo_generations;
	int32		from_collapse_limit;
	int32		join_collapse_limit;
	int32		work_mem;

	/* Double GUCs */
	double		seq_page_cost;
	double		random_page_cost;
	double		cpu_tuple_cost;
	double		cpu_index_tuple_cost;
	double		cpu_operator_cost;
	double		parallel_tuple_cost;
	double		parallel_setup_cost;
	double		recursive_worktable_factor;
	double		geqo_seed;
	double		geqo_selection_bias;
	double		jit_above_cost;
	double		jit_inline_above_cost;
	double		jit_optimize_above_cost;
	double		cursor_tuple_fraction;
	double		min_eager_agg_group_size;
	double		hash_mem_multiplier;
} PlannerGucState;

/* Backend-local cached hash state */
static uint64 cached_planner_guc_hash = 0;
static bool planner_guc_hash_valid = false;

#ifdef USE_ASSERT_CHECKING
static uint64 planner_guc_hash_compute_count = 0;
#endif

static void PopulatePlannerGucState(PlannerGucState *state);
static uint64 ComputePlannerGucHash(PlannerGucState *state);

/*
 * GetPlannerGucHash
 *		Return a deterministic hash of all planner-affecting GUC values.
 *
 * Lazily recomputes when any included GUC has changed since the last call.
 */
uint64
GetPlannerGucHash(void)
{
	if (!planner_guc_hash_valid)
	{
		PlannerGucState state;

		PopulatePlannerGucState(&state);
		cached_planner_guc_hash = ComputePlannerGucHash(&state);
		planner_guc_hash_valid = true;

#ifdef USE_ASSERT_CHECKING
		planner_guc_hash_compute_count++;
#endif
	}
	return cached_planner_guc_hash;
}

/*
 * InvalidatePlannerGucHash
 *		Mark the cached hash as dirty.
 *
 * Called from GUC assign hooks.  Must not recompute, allocate, or throw.
 */
void
InvalidatePlannerGucHash(void)
{
	planner_guc_hash_valid = false;
}

#ifdef USE_ASSERT_CHECKING
uint64
GetPlannerGucHashComputeCount(void)
{
	return planner_guc_hash_compute_count;
}
#endif

/*
 * GUC assign hooks - one per type.
 * Each simply invalidates the cached hash.
 */
void
assign_planner_bool_guc(bool newval, void *extra)
{
	InvalidatePlannerGucHash();
}

void
assign_planner_int_guc(int newval, void *extra)
{
	InvalidatePlannerGucHash();
}

void
assign_planner_real_guc(double newval, void *extra)
{
	InvalidatePlannerGucHash();
}

void
assign_planner_enum_guc(int newval, void *extra)
{
	InvalidatePlannerGucHash();
}

/*
 * PopulatePlannerGucState
 *		Snapshot all 57 planner-affecting GUC values into the struct.
 */
static void
PopulatePlannerGucState(PlannerGucState *state)
{
	memset(state, 0, sizeof(PlannerGucState));

	/* 26 enable_* booleans packed into a bitmask */
	state->enable_flags = 0;
	if (enable_seqscan)
		state->enable_flags |= (1U << 0);
	if (enable_indexscan)
		state->enable_flags |= (1U << 1);
	if (enable_indexonlyscan)
		state->enable_flags |= (1U << 2);
	if (enable_bitmapscan)
		state->enable_flags |= (1U << 3);
	if (enable_tidscan)
		state->enable_flags |= (1U << 4);
	if (enable_sort)
		state->enable_flags |= (1U << 5);
	if (enable_incremental_sort)
		state->enable_flags |= (1U << 6);
	if (enable_hashagg)
		state->enable_flags |= (1U << 7);
	if (enable_nestloop)
		state->enable_flags |= (1U << 8);
	if (enable_material)
		state->enable_flags |= (1U << 9);
	if (enable_memoize)
		state->enable_flags |= (1U << 10);
	if (enable_mergejoin)
		state->enable_flags |= (1U << 11);
	if (enable_hashjoin)
		state->enable_flags |= (1U << 12);
	if (enable_gathermerge)
		state->enable_flags |= (1U << 13);
	if (enable_partitionwise_join)
		state->enable_flags |= (1U << 14);
	if (enable_partitionwise_aggregate)
		state->enable_flags |= (1U << 15);
	if (enable_parallel_append)
		state->enable_flags |= (1U << 16);
	if (enable_parallel_hash)
		state->enable_flags |= (1U << 17);
	if (enable_partition_pruning)
		state->enable_flags |= (1U << 18);
	if (enable_presorted_aggregate)
		state->enable_flags |= (1U << 19);
	if (enable_async_append)
		state->enable_flags |= (1U << 20);
	if (enable_geqo)
		state->enable_flags |= (1U << 21);
	if (enable_eager_aggregate)
		state->enable_flags |= (1U << 22);
	if (enable_distinct_reordering)
		state->enable_flags |= (1U << 23);
	if (enable_group_by_reordering)
		state->enable_flags |= (1U << 24);
	if (enable_self_join_elimination)
		state->enable_flags |= (1U << 25);

	/* 2 miscellaneous booleans */
	state->misc_bool_flags = 0;
	if (jit_enabled)
		state->misc_bool_flags |= (1U << 0);
	if (parallel_leader_participation)
		state->misc_bool_flags |= (1U << 1);

	/* Enums */
	state->constraint_exclusion = constraint_exclusion;
	state->debug_parallel_query = debug_parallel_query;

	/* Integers */
	state->effective_cache_size = effective_cache_size;
	state->max_parallel_workers_per_gather = max_parallel_workers_per_gather;
	state->min_parallel_table_scan_size = min_parallel_table_scan_size;
	state->min_parallel_index_scan_size = min_parallel_index_scan_size;
	state->geqo_threshold = geqo_threshold;
	state->geqo_effort = Geqo_effort;
	state->geqo_pool_size = Geqo_pool_size;
	state->geqo_generations = Geqo_generations;
	state->from_collapse_limit = from_collapse_limit;
	state->join_collapse_limit = join_collapse_limit;
	state->work_mem = work_mem;

	/* Doubles */
	state->seq_page_cost = seq_page_cost;
	state->random_page_cost = random_page_cost;
	state->cpu_tuple_cost = cpu_tuple_cost;
	state->cpu_index_tuple_cost = cpu_index_tuple_cost;
	state->cpu_operator_cost = cpu_operator_cost;
	state->parallel_tuple_cost = parallel_tuple_cost;
	state->parallel_setup_cost = parallel_setup_cost;
	state->recursive_worktable_factor = recursive_worktable_factor;
	state->geqo_seed = Geqo_seed;
	state->geqo_selection_bias = Geqo_selection_bias;
	state->jit_above_cost = jit_above_cost;
	state->jit_inline_above_cost = jit_inline_above_cost;
	state->jit_optimize_above_cost = jit_optimize_above_cost;
	state->cursor_tuple_fraction = cursor_tuple_fraction;
	state->min_eager_agg_group_size = min_eager_agg_group_size;
	state->hash_mem_multiplier = hash_mem_multiplier;
}

/*
 * ComputePlannerGucHash
 *		Hash the populated state struct field-by-field using hash_combine64().
 *
 * Uses explicit field hashing (not raw-struct byte hashing) to eliminate
 * any dependence on struct padding.  The initial seed is a fixed constant (0).
 */
static uint64
ComputePlannerGucHash(PlannerGucState *state)
{
	uint64		hash = 0;		/* fixed seed - must not vary by backend */
	uint64		double_bits;

	/* Bitmask fields */
	hash = hash_combine64(hash, (uint64) state->enable_flags);
	hash = hash_combine64(hash, (uint64) state->misc_bool_flags);

	/* Enum fields */
	hash = hash_combine64(hash, (uint64) (uint32) state->constraint_exclusion);
	hash = hash_combine64(hash, (uint64) (uint32) state->debug_parallel_query);

	/* Integer fields */
	hash = hash_combine64(hash, (uint64) (uint32) state->effective_cache_size);
	hash = hash_combine64(hash, (uint64) (uint32) state->max_parallel_workers_per_gather);
	hash = hash_combine64(hash, (uint64) (uint32) state->min_parallel_table_scan_size);
	hash = hash_combine64(hash, (uint64) (uint32) state->min_parallel_index_scan_size);
	hash = hash_combine64(hash, (uint64) (uint32) state->geqo_threshold);
	hash = hash_combine64(hash, (uint64) (uint32) state->geqo_effort);
	hash = hash_combine64(hash, (uint64) (uint32) state->geqo_pool_size);
	hash = hash_combine64(hash, (uint64) (uint32) state->geqo_generations);
	hash = hash_combine64(hash, (uint64) (uint32) state->from_collapse_limit);
	hash = hash_combine64(hash, (uint64) (uint32) state->join_collapse_limit);
	hash = hash_combine64(hash, (uint64) (uint32) state->work_mem);

	/* Double fields - reinterpret as uint64 bits for hashing */
	memcpy(&double_bits, &state->seq_page_cost, sizeof(double));
	hash = hash_combine64(hash, double_bits);
	memcpy(&double_bits, &state->random_page_cost, sizeof(double));
	hash = hash_combine64(hash, double_bits);
	memcpy(&double_bits, &state->cpu_tuple_cost, sizeof(double));
	hash = hash_combine64(hash, double_bits);
	memcpy(&double_bits, &state->cpu_index_tuple_cost, sizeof(double));
	hash = hash_combine64(hash, double_bits);
	memcpy(&double_bits, &state->cpu_operator_cost, sizeof(double));
	hash = hash_combine64(hash, double_bits);
	memcpy(&double_bits, &state->parallel_tuple_cost, sizeof(double));
	hash = hash_combine64(hash, double_bits);
	memcpy(&double_bits, &state->parallel_setup_cost, sizeof(double));
	hash = hash_combine64(hash, double_bits);
	memcpy(&double_bits, &state->recursive_worktable_factor, sizeof(double));
	hash = hash_combine64(hash, double_bits);
	memcpy(&double_bits, &state->geqo_seed, sizeof(double));
	hash = hash_combine64(hash, double_bits);
	memcpy(&double_bits, &state->geqo_selection_bias, sizeof(double));
	hash = hash_combine64(hash, double_bits);
	memcpy(&double_bits, &state->jit_above_cost, sizeof(double));
	hash = hash_combine64(hash, double_bits);
	memcpy(&double_bits, &state->jit_inline_above_cost, sizeof(double));
	hash = hash_combine64(hash, double_bits);
	memcpy(&double_bits, &state->jit_optimize_above_cost, sizeof(double));
	hash = hash_combine64(hash, double_bits);
	memcpy(&double_bits, &state->cursor_tuple_fraction, sizeof(double));
	hash = hash_combine64(hash, double_bits);
	memcpy(&double_bits, &state->min_eager_agg_group_size, sizeof(double));
	hash = hash_combine64(hash, double_bits);
	memcpy(&double_bits, &state->hash_mem_multiplier, sizeof(double));
	hash = hash_combine64(hash, double_bits);

	return hash;
}
