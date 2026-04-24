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

/*
 * Default chunk size for usage-map scanning.  Matches SSE2/NEON width (16
 * bytes).  pg_usage_scan_chunk_size is set to 32 for AVX2 at runtime;
 * this constant is the compile-time baseline.
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
