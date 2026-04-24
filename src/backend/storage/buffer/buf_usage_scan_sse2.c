/*-------------------------------------------------------------------------
 *
 * buf_usage_scan_sse2.c
 *	  SSE2 chunk-scan and chunk-decrement for BufferUsageMap.
 *
 *	  Compiled only on x86-64 where SSE2 is always available.
 *	  Full SIMD fast path for count == 16; scalar fallback otherwise.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/buf_usage_scan_sse2.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <emmintrin.h>

#include "storage/buf_usage_scan.h"

UsageScanResult
usage_scan_sse2(const uint8_t *map, int count)
{
	UsageScanResult result;

	Assert(count > 0 && count <= 16);

	if (likely(count == 16))
	{
		__m128i vec  = _mm_loadu_si128((const __m128i *) map);
		__m128i zero = _mm_setzero_si128();
		__m128i cmp  = _mm_cmpeq_epi8(vec, zero);
		int		mask = _mm_movemask_epi8(cmp);

		result.zero_mask = (uint32) mask;
		result.count = 16;
		return result;
	}

	return usage_scan_scalar(map, count);
}

bool
usage_decrement_sse2(uint8_t *map, int count)
{
	Assert(count > 0 && count <= 16);

	if (likely(count == 16))
	{
		__m128i vec  = _mm_loadu_si128((const __m128i *) map);
		__m128i zero = _mm_setzero_si128();
		__m128i cmp  = _mm_cmpeq_epi8(vec, zero);
		int		mask = _mm_movemask_epi8(cmp);

		if (mask == 0xFFFF)
			return false;

		{
			__m128i ones   = _mm_set1_epi8(1);
			__m128i result = _mm_subs_epu8(vec, ones);

			_mm_storeu_si128((__m128i *) map, result);
		}

		return true;
	}

	return usage_decrement_scalar(map, count);
}
