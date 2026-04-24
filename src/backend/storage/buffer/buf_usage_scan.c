/*-------------------------------------------------------------------------
 *
 * buf_usage_scan.c
 *	  Scalar chunk-scan and chunk-decrement helpers for BufferUsageMap.
 *
 *	  Patch 2: scalar-only implementations and global dispatch pointers.
 *	  Patch 3 will add SIMD variants and swap these pointers at startup.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/buf_usage_scan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/buf_usage_scan.h"

/* Global dispatch pointers — scalar in Patch 2, SIMD in Patch 3. */
UsageScanFn		pg_usage_scan = usage_scan_scalar;
UsageDecrementFn	pg_usage_decrement = usage_decrement_scalar;
int				pg_usage_scan_chunk_size = USAGE_SCAN_CHUNK_SIZE;


/*
 * usage_scan_scalar
 *
 * Scan 'count' bytes starting at 'map' and return a bitmask with bit i set
 * if map[i] == 0.  Bits beyond 'count' are guaranteed zero.
 *
 * This is the scalar reference implementation.  Patch 3 adds SIMD variants.
 */
UsageScanResult
usage_scan_scalar(const uint8_t *map, int count)
{
	UsageScanResult result;

	Assert(count > 0 && count <= 32);

	result.zero_mask = 0;
	result.count = count;

	for (int i = 0; i < count; i++)
	{
		if (map[i] == 0)
			result.zero_mask |= (1U << i);
	}

	return result;
}

/*
 * usage_decrement_scalar
 *
 * Decrement each non-zero byte in map[0..count-1] by 1 (saturating at 0).
 * Returns true if at least one entry was decremented (map-decay progress).
 *
 * The stores are plain (non-atomic).  A concurrent pinner writing to the
 * same byte can race with this loop — this is the same heuristic-level
 * race accepted in Patch 1, applied to a chunk instead of a single byte.
 */
bool
usage_decrement_scalar(uint8_t *map, int count)
{
	bool	decremented = false;

	Assert(count > 0 && count <= 32);

	for (int i = 0; i < count; i++)
	{
		if (map[i] > 0)
		{
			map[i]--;
			decremented = true;
		}
	}

	return decremented;
}
