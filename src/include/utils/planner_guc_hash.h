/*-------------------------------------------------------------------------
 *
 * planner_guc_hash.h
 *	  Deterministic hash of planner-affecting GUC state.
 *
 * Provides GetPlannerGucHash(), which returns a uint64 hash of the current
 * values of all GUCs that affect plan generation.  Used as a component of
 * the shared generic plan cache key so that two backends only share a plan
 * when their optimizer environments are equivalent.
 *
 * The hash is lazily recomputed: GUC assign hooks call
 * InvalidatePlannerGucHash() to mark the cached value dirty, and the next
 * GetPlannerGucHash() call recomputes it.
 *
 * This module is backend-local and does not require shared memory.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/utils/planner_guc_hash.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANNER_GUC_HASH_H
#define PLANNER_GUC_HASH_H

#include "postgres.h"

extern uint64 GetPlannerGucHash(void);
extern void InvalidatePlannerGucHash(void);

/* GUC assign hooks - called from the GUC framework, must not recompute */
extern void assign_planner_bool_guc(bool newval, void *extra);
extern void assign_planner_int_guc(int newval, void *extra);
extern void assign_planner_real_guc(double newval, void *extra);
extern void assign_planner_enum_guc(int newval, void *extra);

#ifdef USE_ASSERT_CHECKING
extern uint64 GetPlannerGucHashComputeCount(void);
#endif

#endif							/* PLANNER_GUC_HASH_H */
