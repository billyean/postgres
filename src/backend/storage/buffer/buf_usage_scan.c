/*-------------------------------------------------------------------------
 *
 * buf_usage_scan.c
 *	  Scalar chunk-scan and chunk-decrement helpers for BufferUsageMap,
 *	  plus dispatch initialization and debug cross-check wrappers.
 *
 *	  The scalar implementations are the correctness reference.
 *	  ISA-specific SIMD implementations live in separate source files
 *	  (buf_usage_scan_sse2.c, buf_usage_scan_avx2.c, buf_usage_scan_neon.c).
 *
 *	  On first call, the chooser stubs invoke InitUsageScanDispatch() which
 *	  detects CPU features and replaces the global function pointers with
 *	  the best available implementation.  Subsequent calls go directly to
 *	  the selected implementation with no dispatch overhead.
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

#include <string.h>

#include "port/atomics.h"
#include "storage/buf_usage_scan.h"

#ifdef USE_DECOUPLED_USAGE_COUNT
#include "utils/guc.h"
#endif

#ifdef USE_AVX2_USAGE_SCAN_WITH_RUNTIME_CHECK
#include "port/pg_cpu.h"
#endif

/*
 * Forward declarations for ISA-specific implementations.
 * These are defined in separate source files compiled conditionally.
 */
#ifdef USE_SSE2
extern UsageScanResult usage_scan_sse2(const uint8_t *map, int count);
extern bool usage_decrement_sse2(uint8_t *map, int count);
#endif

#ifdef USE_AVX2_USAGE_SCAN_WITH_RUNTIME_CHECK
extern UsageScanResult usage_scan_avx2(const uint8_t *map, int count);
extern bool usage_decrement_avx2(uint8_t *map, int count);
#endif

#ifdef USE_NEON
extern UsageScanResult usage_scan_neon(const uint8_t *map, int count);
extern bool usage_decrement_neon(uint8_t *map, int count);
#endif

/* Chooser stubs — forward declarations */
static UsageScanResult usage_scan_choose(const uint8_t *map, int count);
static bool usage_decrement_choose(uint8_t *map, int count);

/* Global dispatch pointers — chooser stubs until first call. */
UsageScanFn		pg_usage_scan = usage_scan_choose;
UsageDecrementFn	pg_usage_decrement = usage_decrement_choose;
int				pg_usage_scan_chunk_size = USAGE_SCAN_CHUNK_SIZE;

/*
 * Dispatch mode override GUC (Patch 5).
 *
 * Enum options array is referenced by guc_parameters.dat.
 * The variable is read once per backend in InitUsageScanDispatch().
 */
#ifdef USE_DECOUPLED_USAGE_COUNT

const struct config_enum_entry usage_scan_dispatch_mode_options[] = {
	{"auto", USAGE_SCAN_DISPATCH_AUTO, false},
	{"scalar", USAGE_SCAN_DISPATCH_SCALAR, false},
	{"sse2", USAGE_SCAN_DISPATCH_SSE2, false},
	{"avx2", USAGE_SCAN_DISPATCH_AVX2, false},
	{"neon", USAGE_SCAN_DISPATCH_NEON, false},
	{NULL, 0, false}
};

int			pg_usage_scan_dispatch_mode = USAGE_SCAN_DISPATCH_AUTO;

#endif							/* USE_DECOUPLED_USAGE_COUNT */

/*
 * Backend-local instrumentation counters (Patch 4).
 *
 * When USE_DECOUPLED_USAGE_COUNT is on, dispatch_path starts as
 * "not_initialized" and is updated by InitUsageScanDispatch() on first call.
 * When the feature is compiled out, dispatch_path is "not_enabled" and
 * the counters are never incremented.
 */
#ifdef USE_DECOUPLED_USAGE_COUNT
UsageScanStats	pg_usage_scan_stats = {.dispatch_path = "not_initialized"};
#else
UsageScanStats	pg_usage_scan_stats = {.dispatch_path = "not_enabled"};
#endif


/*
 * usage_scan_scalar
 *
 * Scan 'count' bytes starting at 'map' and return a bitmask with bit i set
 * if map[i] == 0.  Bits beyond 'count' are guaranteed zero.
 *
 * This is the scalar reference implementation used as the correctness
 * baseline and universal fallback.
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


/* ----------------------------------------------------------------
 *	Debug cross-check wrappers (assert-enabled builds only)
 *
 *	In debug builds, every SIMD scan/decrement call is verified against
 *	the scalar reference.  The wrappers are installed by
 *	InitUsageScanDispatch() and compiled out entirely in release builds.
 * ----------------------------------------------------------------
 */
#ifdef USE_ASSERT_CHECKING

static UsageScanFn		simd_scan_impl;
static UsageDecrementFn	simd_decrement_impl;

static UsageScanResult
usage_scan_cross_check(const uint8_t *map, int count)
{
	UsageScanResult simd_result   = simd_scan_impl(map, count);
	UsageScanResult scalar_result = usage_scan_scalar(map, count);

	Assert(simd_result.zero_mask == scalar_result.zero_mask);
	Assert(simd_result.count == scalar_result.count);

	return simd_result;
}

static bool
usage_decrement_cross_check(uint8_t *map, int count)
{
	uint8_t		simd_buf[32];
	uint8_t		scalar_buf[32];
	bool		simd_result;
	bool		scalar_result;

	/* Snapshot the live segment into two independent copies */
	memcpy(simd_buf, map, count);
	memcpy(scalar_buf, map, count);

	/* Run both on local copies — immune to concurrent pinner races */
	simd_result   = simd_decrement_impl(simd_buf, count);
	scalar_result = usage_decrement_scalar(scalar_buf, count);

	/* Validate: SIMD and scalar must agree on both output and return value */
	Assert(simd_result == scalar_result);
	Assert(memcmp(simd_buf, scalar_buf, count) == 0);

	/* Apply the validated SIMD result to the live map */
	if (simd_result)
		memcpy(map, simd_buf, count);

	return simd_result;
}

#endif							/* USE_ASSERT_CHECKING */


/* ----------------------------------------------------------------
 *	Dispatch initialization
 *
 *	Patch 5 addition: when pg_usage_scan_dispatch_mode is set to a
 *	value other than AUTO, the requested implementation is forced.
 *	If the requested ISA is not available on this platform/CPU,
 *	a WARNING is emitted and the system falls back to scalar.
 * ----------------------------------------------------------------
 */

/*
 * Map the dispatch mode enum to a human-readable string.
 * Used internally for dispatch_path labeling.
 */
#ifdef USE_DECOUPLED_USAGE_COUNT
static const char *
dispatch_mode_name(int mode)
{
	switch (mode)
	{
		case USAGE_SCAN_DISPATCH_AUTO:   return "auto";
		case USAGE_SCAN_DISPATCH_SCALAR: return "scalar";
		case USAGE_SCAN_DISPATCH_SSE2:   return "sse2";
		case USAGE_SCAN_DISPATCH_AVX2:   return "avx2";
		case USAGE_SCAN_DISPATCH_NEON:   return "neon";
	}
	return "unknown";
}
#endif

static void
InitUsageScanDispatch(void)
{
	/* Default: scalar (universal fallback). */
	pg_usage_scan = usage_scan_scalar;
	pg_usage_decrement = usage_decrement_scalar;
	pg_usage_scan_chunk_size = USAGE_SCAN_CHUNK_SIZE;

#ifdef USE_DECOUPLED_USAGE_COUNT
	switch ((UsageScanDispatchMode) pg_usage_scan_dispatch_mode)
	{
		case USAGE_SCAN_DISPATCH_AUTO:
			/* Fall through to Patch 3 auto-detection below. */
			break;

		case USAGE_SCAN_DISPATCH_SCALAR:
			/* Already set to scalar above. */
			goto dispatch_done;

		case USAGE_SCAN_DISPATCH_SSE2:
#if defined(__x86_64__) || defined(_M_AMD64)
			pg_usage_scan = usage_scan_sse2;
			pg_usage_decrement = usage_decrement_sse2;
#else
			ereport(WARNING,
					(errmsg("requested usage scan dispatch mode \"sse2\" "
							"is not available on this platform, "
							"falling back to \"scalar\"")));
#endif
			goto dispatch_done;

		case USAGE_SCAN_DISPATCH_AVX2:
#ifdef USE_AVX2_USAGE_SCAN_WITH_RUNTIME_CHECK
			if (x86_feature_available(PG_AVX2))
			{
				pg_usage_scan = usage_scan_avx2;
				pg_usage_decrement = usage_decrement_avx2;
				pg_usage_scan_chunk_size = 32;
			}
			else
			{
				ereport(WARNING,
						(errmsg("requested usage scan dispatch mode \"avx2\" "
								"is not supported by this CPU, "
								"falling back to \"scalar\"")));
			}
#else
			ereport(WARNING,
					(errmsg("requested usage scan dispatch mode \"avx2\" "
							"is not available on this platform, "
							"falling back to \"scalar\"")));
#endif
			goto dispatch_done;

		case USAGE_SCAN_DISPATCH_NEON:
#if defined(__aarch64__) || defined(_M_ARM64)
			pg_usage_scan = usage_scan_neon;
			pg_usage_decrement = usage_decrement_neon;
#else
			ereport(WARNING,
					(errmsg("requested usage scan dispatch mode \"neon\" "
							"is not available on this platform, "
							"falling back to \"scalar\"")));
#endif
			goto dispatch_done;
	}
#endif							/* USE_DECOUPLED_USAGE_COUNT */

	/* AUTO mode: Patch 3 best-available auto-detection, unchanged. */
#if defined(__x86_64__) || defined(_M_AMD64)

#ifdef USE_AVX2_USAGE_SCAN_WITH_RUNTIME_CHECK
	if (x86_feature_available(PG_AVX2))
	{
		pg_usage_scan = usage_scan_avx2;
		pg_usage_decrement = usage_decrement_avx2;
		pg_usage_scan_chunk_size = 32;
	}
	else
#endif
	{
		pg_usage_scan = usage_scan_sse2;
		pg_usage_decrement = usage_decrement_sse2;
	}

#elif defined(__aarch64__) || defined(_M_ARM64)

	pg_usage_scan = usage_scan_neon;
	pg_usage_decrement = usage_decrement_neon;

#endif

#ifdef USE_DECOUPLED_USAGE_COUNT
dispatch_done:
#endif

	/* Install debug cross-check wrappers (assert builds only). */
#ifdef USE_ASSERT_CHECKING
	if (pg_usage_scan != usage_scan_scalar)
	{
		simd_scan_impl = pg_usage_scan;
		simd_decrement_impl = pg_usage_decrement;
		pg_usage_scan = usage_scan_cross_check;
		pg_usage_decrement = usage_decrement_cross_check;
	}
#endif

	/* Record dispatch selection for observability (Patch 4). */
	pg_usage_scan_stats.dispatch_chunk_size = pg_usage_scan_chunk_size;

#if defined(__x86_64__) || defined(_M_AMD64)
#ifdef USE_AVX2_USAGE_SCAN_WITH_RUNTIME_CHECK
	if (pg_usage_scan_chunk_size == 32)
		pg_usage_scan_stats.dispatch_path = "avx2";
	else
#endif
	if (pg_usage_scan == usage_scan_sse2
#ifdef USE_ASSERT_CHECKING
		|| simd_scan_impl == usage_scan_sse2
#endif
		)
		pg_usage_scan_stats.dispatch_path = "sse2";
	else
		pg_usage_scan_stats.dispatch_path = "scalar";
#elif defined(__aarch64__) || defined(_M_ARM64)
	if (pg_usage_scan == usage_scan_neon
#ifdef USE_ASSERT_CHECKING
		|| simd_scan_impl == usage_scan_neon
#endif
		)
		pg_usage_scan_stats.dispatch_path = "neon";
	else
		pg_usage_scan_stats.dispatch_path = "scalar";
#else
	pg_usage_scan_stats.dispatch_path = "scalar";
#endif

	/*
	 * Publish dispatch identity to shared memory (Patch 7).
	 *
	 * Three-state protocol: CAS UNINITIALIZED→PUBLISHING (claim),
	 * write payload, barrier, then store READY.  Readers only trust
	 * payload when state == READY.
	 */
#ifdef USE_DECOUPLED_USAGE_COUNT
	if (UsageScanDispatchId != NULL)
	{
		uint32 expected = USAGE_SCAN_DISPATCH_ID_UNINITIALIZED;

		if (pg_atomic_compare_exchange_u32(&UsageScanDispatchId->state,
										   &expected,
										   USAGE_SCAN_DISPATCH_ID_PUBLISHING))
		{
			strlcpy(UsageScanDispatchId->dispatch_path,
					pg_usage_scan_stats.dispatch_path,
					sizeof(UsageScanDispatchId->dispatch_path));
			UsageScanDispatchId->dispatch_chunk_size =
					pg_usage_scan_stats.dispatch_chunk_size;
			pg_write_barrier();
			pg_atomic_write_u32(&UsageScanDispatchId->state,
								USAGE_SCAN_DISPATCH_ID_READY);
		}
	}
#endif
}


/* ----------------------------------------------------------------
 *	Chooser stubs
 *
 *	On first call, these trigger dispatch initialization and then
 *	tail-call the selected implementation.  All subsequent calls go
 *	directly to the selected function pointer.
 * ----------------------------------------------------------------
 */
static UsageScanResult
usage_scan_choose(const uint8_t *map, int count)
{
	InitUsageScanDispatch();
	return pg_usage_scan(map, count);
}

static bool
usage_decrement_choose(uint8_t *map, int count)
{
	InitUsageScanDispatch();
	return pg_usage_decrement(map, count);
}
