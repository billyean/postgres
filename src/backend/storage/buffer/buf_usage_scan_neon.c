/*-------------------------------------------------------------------------
 *
 * buf_usage_scan_neon.c
 *	  NEON chunk-scan and chunk-decrement for BufferUsageMap.
 *
 *	  Compiled only on AArch64 where NEON is always available.
 *	  Full SIMD fast path for count == 16; scalar fallback otherwise.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/buf_usage_scan_neon.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <arm_neon.h>

#include "storage/buf_usage_scan.h"

/*
 * Bit-position constants for NEON movemask emulation.  Lane i within each
 * 8-byte half maps to bit 2^(i%8).
 */
static const uint8_t pg_attribute_aligned(16)
neon_bit_select[16] = {
	1, 2, 4, 8, 16, 32, 64, 128,
	1, 2, 4, 8, 16, 32, 64, 128
};

UsageScanResult
usage_scan_neon(const uint8_t *map, int count)
{
	UsageScanResult result;

	Assert(count > 0 && count <= 16);

	if (likely(count == 16))
	{
		uint8x16_t vec    = vld1q_u8(map);
		uint8x16_t zero   = vdupq_n_u8(0);
		uint8x16_t cmp    = vceqq_u8(vec, zero);

		uint8x16_t bits   = vld1q_u8(neon_bit_select);
		uint8x16_t masked = vandq_u8(cmp, bits);

		uint16x8_t pairs  = vpaddlq_u8(masked);
		uint32x4_t quads  = vpaddlq_u16(pairs);
		uint64x2_t octs   = vpaddlq_u32(quads);

		uint32_t lo = (uint32_t) vgetq_lane_u64(octs, 0);
		uint32_t hi = (uint32_t) vgetq_lane_u64(octs, 1);

		result.zero_mask = lo | (hi << 8);
		result.count = 16;
		return result;
	}

	return usage_scan_scalar(map, count);
}

bool
usage_decrement_neon(uint8_t *map, int count)
{
	Assert(count > 0 && count <= 16);

	if (likely(count == 16))
	{
		uint8x16_t vec = vld1q_u8(map);

		if (vmaxvq_u8(vec) == 0)
			return false;

		{
			uint8x16_t ones   = vdupq_n_u8(1);
			uint8x16_t result = vqsubq_u8(vec, ones);

			vst1q_u8(map, result);
		}

		return true;
	}

	return usage_decrement_scalar(map, count);
}
