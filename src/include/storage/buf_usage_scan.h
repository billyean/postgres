/*-------------------------------------------------------------------------
 *
 * buf_usage_scan.h
 *	  Chunk-based scan and decrement helpers for BufferUsageMap.
 *
 *	  Scalar reference implementations plus ISA-specific SIMD variants
 *	  (SSE2, AVX2, NEON).  Dispatch is initialized lazily on first call;
 *	  see InitUsageScanDispatch() in buf_usage_scan.c.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/buf_usage_scan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUF_USAGE_SCAN_H
#define BUF_USAGE_SCAN_H

#include "c.h"
#include "port/atomics.h"

/*
 * Default chunk size for usage-map scanning.  Matches SSE2/NEON width (16
 * bytes).  pg_usage_scan_chunk_size is set to 32 for AVX2 at runtime;
 * this constant is the compile-time baseline.
 */
#define USAGE_SCAN_CHUNK_SIZE	16

/*
 * Dispatch mode selector for the chunk-based usage-map scan (Patch 5).
 *
 * Controls which scan/decrement implementation InitUsageScanDispatch()
 * selects.  AUTO preserves the Patch 3 best-available auto-detection.
 * Other values force a specific implementation; if unavailable, the system
 * logs a WARNING and falls back to SCALAR.
 *
 * The enum is always defined when USE_DECOUPLED_USAGE_COUNT is on.
 * The corresponding GUC pg_usage_scan_dispatch_mode is PGC_POSTMASTER.
 */
#ifdef USE_DECOUPLED_USAGE_COUNT

typedef enum UsageScanDispatchMode
{
	USAGE_SCAN_DISPATCH_AUTO,
	USAGE_SCAN_DISPATCH_SCALAR,
	USAGE_SCAN_DISPATCH_SSE2,
	USAGE_SCAN_DISPATCH_AVX2,
	USAGE_SCAN_DISPATCH_NEON
} UsageScanDispatchMode;

extern PGDLLIMPORT int pg_usage_scan_dispatch_mode;

#endif							/* USE_DECOUPLED_USAGE_COUNT */

/*
 * Result of scanning one contiguous segment of BufferUsageMap.
 * zero_mask: bit i is set if map[i] == 0 (candidate for victim selection).
 * count: number of entries actually scanned (may be < USAGE_SCAN_CHUNK_SIZE
 *        for partial segments at array boundaries).
 */
typedef struct UsageScanResult
{
	uint32		zero_mask;
	int			count;
} UsageScanResult;

/*
 * Function-pointer types for scan and decrement operations.
 */
typedef UsageScanResult (*UsageScanFn)(const uint8_t *map, int count);
typedef bool (*UsageDecrementFn)(uint8_t *map, int count);

/* Global dispatch pointers — replaced from chooser stubs on first call. */
extern PGDLLIMPORT UsageScanFn		pg_usage_scan;
extern PGDLLIMPORT UsageDecrementFn	pg_usage_decrement;
extern PGDLLIMPORT int				pg_usage_scan_chunk_size;

/* Scalar reference implementations */
extern UsageScanResult usage_scan_scalar(const uint8_t *map, int count);
extern bool usage_decrement_scalar(uint8_t *map, int count);

/*
 * Backend-local eviction instrumentation counters.
 *
 * The struct always exists.  dispatch_path is initialized to:
 *   "not_initialized" when USE_DECOUPLED_USAGE_COUNT is on (pre-dispatch)
 *   "not_enabled"     when USE_DECOUPLED_USAGE_COUNT is off
 * InitUsageScanDispatch() replaces it with the concrete ISA label.
 * Reset clears activity counters but preserves dispatch fields.
 */
typedef struct UsageScanStats
{
	/* Dispatch info — set once by InitUsageScanDispatch, not cleared by reset */
	const char *dispatch_path;
	int			dispatch_chunk_size;

	/* Sweep activity */
	int64		chunks_scanned;
	int64		segments_scanned;
	int64		scan_calls;
	int64		decrement_calls;

	/* Candidate flow */
	int64		candidates_examined;
	int64		rejected_refcount;
	int64		rejected_locked;
	int64		cas_failures;
	int64		victims_found;

	/* Decay/progress */
	int64		decrement_progress;
	int64		decrement_noprogress;
	int64		trycounter_resets;
} UsageScanStats;

extern PGDLLIMPORT UsageScanStats pg_usage_scan_stats;

/*
 * Shared-memory per-backend counter slot for cluster-wide aggregation (Patch 7).
 *
 * One slot per possible backend.  Each backend writes only its own slot
 * using plain stores; the aggregate SRF sums all slots on read.
 * Counter fields mirror the activity counters in UsageScanStats.
 */
#ifdef USE_DECOUPLED_USAGE_COUNT

typedef struct UsageScanSlot
{
	int64		chunks_scanned;
	int64		segments_scanned;
	int64		scan_calls;
	int64		decrement_calls;
	int64		candidates_examined;
	int64		rejected_refcount;
	int64		rejected_locked;
	int64		cas_failures;
	int64		victims_found;
	int64		decrement_progress;
	int64		decrement_noprogress;
	int64		trycounter_resets;
} UsageScanSlot;

/*
 * Shared dispatch identity — written once by the first backend to complete
 * InitUsageScanDispatch(), read by the aggregate SRF.
 *
 * Publication uses a three-state protocol via the state field:
 */
#define USAGE_SCAN_DISPATCH_ID_UNINITIALIZED	0	/* no backend has published */
#define USAGE_SCAN_DISPATCH_ID_PUBLISHING		1	/* writer won CAS, payload in flight */
#define USAGE_SCAN_DISPATCH_ID_READY			2	/* payload committed, safe to read */

typedef struct UsageScanDispatchIdentity
{
	char				dispatch_path[16];
	int					dispatch_chunk_size;
	pg_atomic_uint32	state;
} UsageScanDispatchIdentity;

extern PGDLLIMPORT UsageScanSlot *UsageScanSlots;
extern PGDLLIMPORT UsageScanDispatchIdentity *UsageScanDispatchId;

#endif							/* USE_DECOUPLED_USAGE_COUNT */

/*
 * usage_scan_valid_mask - Return a uint32 bitmask with the lowest 'n' bits
 * set.  Safe for n in [0, 32]; avoids undefined behavior from (1U << 32).
 */
static inline uint32
usage_scan_valid_mask(int n)
{
	Assert(n >= 0 && n <= 32);
	if (n == 32)
		return 0xFFFFFFFFU;
	return (1U << n) - 1;
}

#endif							/* BUF_USAGE_SCAN_H */
