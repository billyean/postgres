/*-------------------------------------------------------------------------
 *
 * buf_usage_scan_avx2.c
 *	  AVX2 chunk-scan and chunk-decrement for BufferUsageMap.
 *
 *	  Compiled only when USE_AVX2_USAGE_SCAN_WITH_RUNTIME_CHECK is defined (x86-64
 *	  with compiler support for __attribute__((target("avx2")))).
 *	  Full SIMD fast path for count == 32; scalar fallback otherwise.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/buf_usage_scan_avx2.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <immintrin.h>

#include "storage/buf_usage_scan.h"

pg_attribute_target("avx2")
UsageScanResult
usage_scan_avx2(const uint8_t *map, int count)
{
	UsageScanResult result;

	Assert(count > 0 && count <= 32);

	if (likely(count == 32))
	{
		__m256i vec  = _mm256_loadu_si256((const __m256i *) map);
		__m256i zero = _mm256_setzero_si256();
		__m256i cmp  = _mm256_cmpeq_epi8(vec, zero);
		int		mask = _mm256_movemask_epi8(cmp);

		result.zero_mask = (uint32) mask;
		result.count = 32;
		return result;
	}

	return usage_scan_scalar(map, count);
}

pg_attribute_target("avx2")
bool
usage_decrement_avx2(uint8_t *map, int count)
{
	Assert(count > 0 && count <= 32);

	if (likely(count == 32))
	{
		__m256i vec  = _mm256_loadu_si256((const __m256i *) map);
		__m256i zero = _mm256_setzero_si256();
		__m256i cmp  = _mm256_cmpeq_epi8(vec, zero);
		int		mask = _mm256_movemask_epi8(cmp);

		if (mask == (int) 0xFFFFFFFF)
			return false;

		{
			__m256i ones   = _mm256_set1_epi8(1);
			__m256i result = _mm256_subs_epu8(vec, ones);

			_mm256_storeu_si256((__m256i *) map, result);
		}

		return true;
	}

	return usage_decrement_scalar(map, count);
}
