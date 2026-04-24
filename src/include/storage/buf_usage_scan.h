/*-------------------------------------------------------------------------
 *
 * buf_usage_scan.h
 *	  Chunk-based scan and decrement helpers for BufferUsageMap.
 *
 *	  Patch 2: scalar-only implementations.
 *	  Patch 3 will add SIMD variants and swap the function pointers
 *	  at startup via InitUsageScanDispatch().
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

/*
 * Default chunk size for usage-map scanning.  Matches SSE2/NEON width (16
 * bytes).  Patch 3 may increase pg_usage_scan_chunk_size to 32 for AVX2
 * at runtime; this constant is the compile-time baseline.
 */
#define USAGE_SCAN_CHUNK_SIZE	16

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
 * Patch 2 sets these to scalar implementations.
 * Patch 3 swaps them to SIMD at startup.
 */
typedef UsageScanResult (*UsageScanFn)(const uint8_t *map, int count);
typedef bool (*UsageDecrementFn)(uint8_t *map, int count);

/* Global dispatch pointers — set once at startup, never changed after. */
extern PGDLLIMPORT UsageScanFn		pg_usage_scan;
extern PGDLLIMPORT UsageDecrementFn	pg_usage_decrement;
extern PGDLLIMPORT int				pg_usage_scan_chunk_size;

/* Scalar reference implementations */
extern UsageScanResult usage_scan_scalar(const uint8_t *map, int count);
extern bool usage_decrement_scalar(uint8_t *map, int count);

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
