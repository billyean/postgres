/*-------------------------------------------------------------------------
 *
 * freelist.c
 *	  routines for managing the buffer pool's replacement strategy.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/buffer/freelist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "pgstat.h"
#include "port/atomics.h"
#include "port/pg_bitutils.h"
#include "storage/buf_internals.h"
#include "storage/buf_usage_scan.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "storage/subsystems.h"

#define INT_ACCESS_ONCE(var)	((int)(*((volatile int *)&(var))))


/*
 * The shared freelist control information.
 */
typedef struct
{
	/* Spinlock: protects the values below */
	slock_t		buffer_strategy_lock;

	/*
	 * clock-sweep hand: index of next buffer to consider grabbing. Note that
	 * this isn't a concrete buffer - we only ever increase the value. So, to
	 * get an actual buffer, it needs to be used modulo NBuffers.
	 */
	pg_atomic_uint32 nextVictimBuffer;

	/*
	 * Statistics.  These counters should be wide enough that they can't
	 * overflow during a single bgwriter cycle.
	 */
	uint32		completePasses; /* Complete cycles of the clock-sweep */
	pg_atomic_uint32 numBufferAllocs;	/* Buffers allocated since last reset */

	/*
	 * Bgworker process to be notified upon activity or -1 if none. See
	 * StrategyNotifyBgWriter.
	 */
	int			bgwprocno;
} BufferStrategyControl;

/* Pointers to shared state */
static BufferStrategyControl *StrategyControl = NULL;

static void StrategyCtlShmemRequest(void *arg);
static void StrategyCtlShmemInit(void *arg);

const ShmemCallbacks StrategyCtlShmemCallbacks = {
	.request_fn = StrategyCtlShmemRequest,
	.init_fn = StrategyCtlShmemInit,
};

/*
 * Private (non-shared) state for managing a ring of shared buffers to re-use.
 * This is currently the only kind of BufferAccessStrategy object, but someday
 * we might have more kinds.
 */
typedef struct BufferAccessStrategyData
{
	/* Overall strategy type */
	BufferAccessStrategyType btype;
	/* Number of elements in buffers[] array */
	int			nbuffers;

	/*
	 * Index of the "current" slot in the ring, ie, the one most recently
	 * returned by GetBufferFromRing.
	 */
	int			current;

	/*
	 * Array of buffer numbers.  InvalidBuffer (that is, zero) indicates we
	 * have not yet selected a buffer for this ring slot.  For allocation
	 * simplicity this is palloc'd together with the fixed fields of the
	 * struct.
	 */
	Buffer		buffers[FLEXIBLE_ARRAY_MEMBER];
}			BufferAccessStrategyData;


/* Prototypes for internal functions */
static BufferDesc *GetBufferFromRing(BufferAccessStrategy strategy,
									 uint64 *buf_state);
static void AddBufferToRing(BufferAccessStrategy strategy,
							BufferDesc *buf);

/*
 * ClockSweepTick - Helper routine for StrategyGetBuffer()
 *
 * Move the clock hand one buffer ahead of its current position and return the
 * id of the buffer now under the hand.
 */
static inline uint32
ClockSweepTick(void)
{
	uint32		victim;

	/*
	 * Atomically move hand ahead one buffer - if there's several processes
	 * doing this, this can lead to buffers being returned slightly out of
	 * apparent order.
	 */
	victim =
		pg_atomic_fetch_add_u32(&StrategyControl->nextVictimBuffer, 1);

	if (victim >= NBuffers)
	{
		uint32		originalVictim = victim;

		/* always wrap what we look up in BufferDescriptors */
		victim = victim % NBuffers;

		/*
		 * If we're the one that just caused a wraparound, force
		 * completePasses to be incremented while holding the spinlock. We
		 * need the spinlock so StrategySyncStart() can return a consistent
		 * value consisting of nextVictimBuffer and completePasses.
		 */
		if (victim == 0)
		{
			uint32		expected;
			uint32		wrapped;
			bool		success = false;

			expected = originalVictim + 1;

			while (!success)
			{
				/*
				 * Acquire the spinlock while increasing completePasses. That
				 * allows other readers to read nextVictimBuffer and
				 * completePasses in a consistent manner which is required for
				 * StrategySyncStart().  In theory delaying the increment
				 * could lead to an overflow of nextVictimBuffers, but that's
				 * highly unlikely and wouldn't be particularly harmful.
				 */
				SpinLockAcquire(&StrategyControl->buffer_strategy_lock);

				wrapped = expected % NBuffers;

				success = pg_atomic_compare_exchange_u32(&StrategyControl->nextVictimBuffer,
														 &expected, wrapped);
				if (success)
					StrategyControl->completePasses++;
				SpinLockRelease(&StrategyControl->buffer_strategy_lock);
			}
		}
	}
	return victim;
}

#ifdef USE_DECOUPLED_USAGE_COUNT

/*
 * ClockSweepTickChunk - Chunk-based variant of ClockSweepTick().
 *
 * Move the clock hand 'chunk_size' buffers ahead of its current position and
 * return the id of the buffer now under the hand.  The caller processes the
 * range [returned_id, returned_id + chunk_size) modulo NBuffers, potentially
 * split into a tail segment and a head segment.
 *
 * The wraparound and completePasses protocol is identical to ClockSweepTick();
 * only the stride passed to fetch_add differs.
 */
static inline uint32
ClockSweepTickChunk(int chunk_size)
{
	uint32		victim;

	/*
	 * Atomically move hand ahead by chunk_size buffers - if there's several
	 * processes doing this, this can lead to buffers being returned slightly
	 * out of apparent order.
	 */
	victim =
		pg_atomic_fetch_add_u32(&StrategyControl->nextVictimBuffer,
								chunk_size);

	if (victim >= NBuffers)
	{
		uint32		originalVictim = victim;

		/* always wrap what we look up in BufferDescriptors */
		victim = victim % NBuffers;

		/*
		 * If we're the one that just caused a wraparound, force
		 * completePasses to be incremented while holding the spinlock. We
		 * need the spinlock so StrategySyncStart() can return a consistent
		 * value consisting of nextVictimBuffer and completePasses.
		 */
		if (victim == 0)
		{
			uint32		expected;
			uint32		wrapped;
			bool		success = false;

			expected = originalVictim + chunk_size;

			while (!success)
			{
				/*
				 * Acquire the spinlock while increasing completePasses. That
				 * allows other readers to read nextVictimBuffer and
				 * completePasses in a consistent manner which is required for
				 * StrategySyncStart().  In theory delaying the increment
				 * could lead to an overflow of nextVictimBuffers, but that's
				 * highly unlikely and wouldn't be particularly harmful.
				 */
				SpinLockAcquire(&StrategyControl->buffer_strategy_lock);

				wrapped = expected % NBuffers;

				success = pg_atomic_compare_exchange_u32(
					&StrategyControl->nextVictimBuffer,
					&expected, wrapped);
				if (success)
					StrategyControl->completePasses++;
				SpinLockRelease(&StrategyControl->buffer_strategy_lock);
			}
		}
	}
	return victim;
}

/*
 * process_chunk_segment - Run phases 1-3 on one contiguous segment.
 *
 * Scans seg_count entries of BufferUsageMap starting at seg_map (physical
 * buffer IDs starting at seg_base), validates zero-usage candidates against
 * the hot state word, attempts to CAS-pin, then bulk-decrements non-zero
 * entries.
 *
 * Returns a victim BufferDesc if one was pinned, or NULL.
 * Sets *made_progress to true if Phase 3 decremented at least one entry.
 */
static BufferDesc *
process_chunk_segment(uint8_t *seg_map, int seg_base, int seg_count,
					  BufferAccessStrategy strategy, uint64 *buf_state_out,
					  bool *made_progress)
{
	UsageScanResult scan;
	uint32			mask;

	Assert(seg_count > 0 && seg_count <= 32);

	/* Phase 1: scan segment for zero-usage candidates */
	scan = pg_usage_scan(seg_map, seg_count);
	Assert((scan.zero_mask & ~usage_scan_valid_mask(seg_count)) == 0);

	/* Phase 2: validate candidates against hot state, try to pin */
	mask = scan.zero_mask;
	while (mask != 0)
	{
		int			idx = pg_rightmost_one_pos32(mask);
		int			buf_id = seg_base + idx;
		BufferDesc *cand = GetBufferDescriptor(buf_id);
		uint64		cand_state;

		cand_state = pg_atomic_read_u64(&cand->state);

		/*
		 * Hot-state checks — must match the existing StrategyGetBuffer()
		 * path.  Check refcount and BM_LOCKED only; BM_IO_IN_PROGRESS is
		 * handled later by the caller via StartBufferIO().
		 *
		 * Settled Patch 2 policy: skip locked candidates rather than
		 * waiting, because other candidates within the same chunk may be
		 * available.  The locked buffer will be revisited on a future pass.
		 */
		if (BUF_STATE_GET_REFCOUNT(cand_state) == 0 &&
			!(cand_state & BM_LOCKED))
		{
			uint64		new_state = cand_state + BUF_REFCOUNT_ONE;

#ifdef USE_DECOUPLED_USAGE_COUNT
			AssertUsageCountDecoupled(cand_state);
#endif

			if (pg_atomic_compare_exchange_u64(&cand->state,
											   &cand_state,
											   new_state))
			{
				if (strategy != NULL)
					AddBufferToRing(strategy, cand);
				*buf_state_out = new_state;

				TrackNewBufferPin(BufferDescriptorGetBuffer(cand));

				return cand;
			}
		}

		mask &= (mask - 1);		/* clear lowest set bit */
	}

	/*
	 * Phase 3: bulk decrement non-zero entries.
	 *
	 * This runs after Phase 2 so that entries pinned in Phase 2 are not
	 * pointlessly decremented.  The decrement is a plain store, not atomic —
	 * same heuristic-level race as Patch 1 (see design doc Section 7.4).
	 */
	*made_progress = pg_usage_decrement(seg_map, seg_count);

	return NULL;
}

#endif							/* USE_DECOUPLED_USAGE_COUNT */

/*
 * StrategyGetBuffer
 *
 *	Called by the bufmgr to get the next candidate buffer to use in
 *	GetVictimBuffer(). The only hard requirement GetVictimBuffer() has is that
 *	the selected buffer must not currently be pinned by anyone.
 *
 *	strategy is a BufferAccessStrategy object, or NULL for default strategy.
 *
 *	It is the callers responsibility to ensure the buffer ownership can be
 *	tracked via TrackNewBufferPin().
 *
 *	The buffer is pinned and marked as owned, using TrackNewBufferPin(),
 *	before returning.
 */
BufferDesc *
StrategyGetBuffer(BufferAccessStrategy strategy, uint64 *buf_state, bool *from_ring)
{
	BufferDesc *buf;
	int			bgwprocno;
	int			trycounter;

	*from_ring = false;

	/*
	 * If given a strategy object, see whether it can select a buffer. We
	 * assume strategy objects don't need buffer_strategy_lock.
	 */
	if (strategy != NULL)
	{
		buf = GetBufferFromRing(strategy, buf_state);
		if (buf != NULL)
		{
			*from_ring = true;
			return buf;
		}
	}

	/*
	 * If asked, we need to waken the bgwriter. Since we don't want to rely on
	 * a spinlock for this we force a read from shared memory once, and then
	 * set the latch based on that value. We need to go through that length
	 * because otherwise bgwprocno might be reset while/after we check because
	 * the compiler might just reread from memory.
	 *
	 * This can possibly set the latch of the wrong process if the bgwriter
	 * dies in the wrong moment. But since PGPROC->procLatch is never
	 * deallocated the worst consequence of that is that we set the latch of
	 * some arbitrary process.
	 */
	bgwprocno = INT_ACCESS_ONCE(StrategyControl->bgwprocno);
	if (bgwprocno != -1)
	{
		/* reset bgwprocno first, before setting the latch */
		StrategyControl->bgwprocno = -1;

		/*
		 * Not acquiring ProcArrayLock here which is slightly icky. It's
		 * actually fine because procLatch isn't ever freed, so we just can
		 * potentially set the wrong process' (or no process') latch.
		 */
		SetLatch(&GetPGProcByNumber(bgwprocno)->procLatch);
	}

	/*
	 * We count buffer allocation requests so that the bgwriter can estimate
	 * the rate of buffer consumption.  Note that buffers recycled by a
	 * strategy object are intentionally not counted here.
	 */
	pg_atomic_fetch_add_u32(&StrategyControl->numBufferAllocs, 1);

#ifdef USE_DECOUPLED_USAGE_COUNT
	/*
	 * Chunk-based clock sweep when decoupled usage_count is active (Patch 2).
	 *
	 * Instead of examining one buffer per tick, advance the clock hand by
	 * effective_chunk positions and process the entire chunk in three phases:
	 *   Phase 1 — scan map for zero-usage candidates (bitmask)
	 *   Phase 2 — validate candidates against hot state, CAS-pin
	 *   Phase 3 — bulk-decrement non-zero entries
	 *
	 * A logical chunk may span two physical segments when it crosses NBuffers.
	 * Both segments are processed within the same loop iteration so that no
	 * buffer position is skipped.
	 */
	if (enable_decoupled_usage_count)
	{
		int			effective_chunk = Min(pg_usage_scan_chunk_size, NBuffers);

		trycounter = NBuffers;
		for (;;)
		{
			uint32		start;
			int			tail_count;
			int			head_count;
			bool		progress_tail = false;
			bool		progress_head = false;

			start = ClockSweepTickChunk(effective_chunk);
			tail_count = Min(effective_chunk, NBuffers - (int) start);
			head_count = effective_chunk - tail_count;
			Assert(tail_count > 0 && tail_count <= effective_chunk);
			Assert(head_count >= 0);
			Assert(tail_count + head_count == effective_chunk);

			/* Process tail segment: [start, start + tail_count) */
			buf = process_chunk_segment(BufferUsageMap + start, (int) start,
										tail_count, strategy, buf_state,
										&progress_tail);
			if (buf != NULL)
				return buf;

			/* Process head segment if the chunk wrapped: [0, head_count) */
			if (head_count > 0)
			{
				buf = process_chunk_segment(BufferUsageMap, 0,
											head_count, strategy, buf_state,
											&progress_head);
				if (buf != NULL)
					return buf;
			}

			if (progress_tail || progress_head)
				trycounter = NBuffers;
			else
				trycounter -= effective_chunk;

			if (trycounter <= 0)
				elog(ERROR, "no unpinned buffers available");
		}
	}
#endif							/* USE_DECOUPLED_USAGE_COUNT */

	/* Per-buffer clock sweep — original path (upstream + Patch 1 fallback) */
	trycounter = NBuffers;
	for (;;)
	{
		uint64		old_buf_state;
		uint64		local_buf_state;

		buf = GetBufferDescriptor(ClockSweepTick());

		/*
		 * Check whether the buffer can be used and pin it if so. Do this
		 * using a CAS loop, to avoid having to lock the buffer header.
		 */
		old_buf_state = pg_atomic_read_u64(&buf->state);
		for (;;)
		{
			local_buf_state = old_buf_state;

			/*
			 * If the buffer is pinned or has a nonzero usage_count, we cannot
			 * use it; decrement the usage_count (unless pinned) and keep
			 * scanning.
			 */

			if (BUF_STATE_GET_REFCOUNT(local_buf_state) != 0)
			{
				if (--trycounter == 0)
				{
					elog(ERROR, "no unpinned buffers available");
				}
				break;
			}

			/* See equivalent code in PinBuffer() */
			if (unlikely(local_buf_state & BM_LOCKED))
			{
				old_buf_state = WaitBufHdrUnlocked(buf);
				continue;
			}

			if (BUF_STATE_GET_USAGECOUNT(local_buf_state) != 0)
			{
				local_buf_state -= BUF_USAGECOUNT_ONE;

				if (pg_atomic_compare_exchange_u64(&buf->state, &old_buf_state,
												   local_buf_state))
				{
					trycounter = NBuffers;
					break;
				}
			}
			else
			{
				/* pin the buffer if the CAS succeeds */
				local_buf_state += BUF_REFCOUNT_ONE;

				if (pg_atomic_compare_exchange_u64(&buf->state, &old_buf_state,
												   local_buf_state))
				{
					/* Found a usable buffer */
					if (strategy != NULL)
						AddBufferToRing(strategy, buf);
					*buf_state = local_buf_state;

					TrackNewBufferPin(BufferDescriptorGetBuffer(buf));

					return buf;
				}
			}
		}
	}
}

/*
 * StrategySyncStart -- tell BgBufferSync where to start syncing
 *
 * The result is the buffer index of the best buffer to sync first.
 * BgBufferSync() will proceed circularly around the buffer array from there.
 *
 * In addition, we return the completed-pass count (which is effectively
 * the higher-order bits of nextVictimBuffer) and the count of recent buffer
 * allocs if non-NULL pointers are passed.  The alloc count is reset after
 * being read.
 */
int
StrategySyncStart(uint32 *complete_passes, uint32 *num_buf_alloc)
{
	uint32		nextVictimBuffer;
	int			result;

	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
	nextVictimBuffer = pg_atomic_read_u32(&StrategyControl->nextVictimBuffer);
	result = nextVictimBuffer % NBuffers;

	if (complete_passes)
	{
		*complete_passes = StrategyControl->completePasses;

		/*
		 * Additionally add the number of wraparounds that happened before
		 * completePasses could be incremented. C.f. ClockSweepTick().
		 */
		*complete_passes += nextVictimBuffer / NBuffers;
	}

	if (num_buf_alloc)
	{
		*num_buf_alloc = pg_atomic_exchange_u32(&StrategyControl->numBufferAllocs, 0);
	}
	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
	return result;
}

/*
 * StrategyNotifyBgWriter -- set or clear allocation notification latch
 *
 * If bgwprocno isn't -1, the next invocation of StrategyGetBuffer will
 * set that latch.  Pass -1 to clear the pending notification before it
 * happens.  This feature is used by the bgwriter process to wake itself up
 * from hibernation, and is not meant for anybody else to use.
 */
void
StrategyNotifyBgWriter(int bgwprocno)
{
	/*
	 * We acquire buffer_strategy_lock just to ensure that the store appears
	 * atomic to StrategyGetBuffer.  The bgwriter should call this rather
	 * infrequently, so there's no performance penalty from being safe.
	 */
	SpinLockAcquire(&StrategyControl->buffer_strategy_lock);
	StrategyControl->bgwprocno = bgwprocno;
	SpinLockRelease(&StrategyControl->buffer_strategy_lock);
}


/*
 * StrategyCtlShmemRequest -- request shared memory for the buffer
 *		cache replacement strategy.
 */
static void
StrategyCtlShmemRequest(void *arg)
{
	ShmemRequestStruct(.name = "Buffer Strategy Status",
					   .size = sizeof(BufferStrategyControl),
					   .ptr = (void **) &StrategyControl
		);
}

/*
 * StrategyCtlShmemInit -- initialize the buffer cache replacement strategy.
 */
static void
StrategyCtlShmemInit(void *arg)
{
	SpinLockInit(&StrategyControl->buffer_strategy_lock);

	/* Initialize the clock-sweep pointer */
	pg_atomic_init_u32(&StrategyControl->nextVictimBuffer, 0);

	/* Clear statistics */
	StrategyControl->completePasses = 0;
	pg_atomic_init_u32(&StrategyControl->numBufferAllocs, 0);

	/* No pending notification */
	StrategyControl->bgwprocno = -1;
}


/* ----------------------------------------------------------------
 *				Backend-private buffer ring management
 * ----------------------------------------------------------------
 */


/*
 * GetAccessStrategy -- create a BufferAccessStrategy object
 *
 * The object is allocated in the current memory context.
 */
BufferAccessStrategy
GetAccessStrategy(BufferAccessStrategyType btype)
{
	int			ring_size_kb;

	/*
	 * Select ring size to use.  See buffer/README for rationales.
	 *
	 * Note: if you change the ring size for BAS_BULKREAD, see also
	 * SYNC_SCAN_REPORT_INTERVAL in access/heap/syncscan.c.
	 */
	switch (btype)
	{
		case BAS_NORMAL:
			/* if someone asks for NORMAL, just give 'em a "default" object */
			return NULL;

		case BAS_BULKREAD:
			{
				int			ring_max_kb;

				/*
				 * The ring always needs to be large enough to allow some
				 * separation in time between providing a buffer to the user
				 * of the strategy and that buffer being reused. Otherwise the
				 * user's pin will prevent reuse of the buffer, even without
				 * concurrent activity.
				 *
				 * We also need to ensure the ring always is large enough for
				 * SYNC_SCAN_REPORT_INTERVAL, as noted above.
				 *
				 * Thus we start out a minimal size and increase the size
				 * further if appropriate.
				 */
				ring_size_kb = 256;

				/*
				 * There's no point in a larger ring if we won't be allowed to
				 * pin sufficiently many buffers.  But we never limit to less
				 * than the minimal size above.
				 */
				ring_max_kb = GetPinLimit() * (BLCKSZ / 1024);
				ring_max_kb = Max(ring_size_kb, ring_max_kb);

				/*
				 * We would like the ring to additionally have space for the
				 * configured degree of IO concurrency. While being read in,
				 * buffers can obviously not yet be reused.
				 *
				 * Each IO can be up to io_combine_limit blocks large, and we
				 * want to start up to effective_io_concurrency IOs.
				 *
				 * Note that effective_io_concurrency may be 0, which disables
				 * AIO.
				 */
				ring_size_kb += (BLCKSZ / 1024) *
					io_combine_limit * effective_io_concurrency;

				if (ring_size_kb > ring_max_kb)
					ring_size_kb = ring_max_kb;
				break;
			}
		case BAS_BULKWRITE:
			ring_size_kb = 16 * 1024;
			break;
		case BAS_VACUUM:
			ring_size_kb = 2048;
			break;

		default:
			elog(ERROR, "unrecognized buffer access strategy: %d",
				 (int) btype);
			return NULL;		/* keep compiler quiet */
	}

	return GetAccessStrategyWithSize(btype, ring_size_kb);
}

/*
 * GetAccessStrategyWithSize -- create a BufferAccessStrategy object with a
 *		number of buffers equivalent to the passed in size.
 *
 * If the given ring size is 0, no BufferAccessStrategy will be created and
 * the function will return NULL.  ring_size_kb must not be negative.
 */
BufferAccessStrategy
GetAccessStrategyWithSize(BufferAccessStrategyType btype, int ring_size_kb)
{
	int			ring_buffers;
	BufferAccessStrategy strategy;

	Assert(ring_size_kb >= 0);

	/* Figure out how many buffers ring_size_kb is */
	ring_buffers = ring_size_kb / (BLCKSZ / 1024);

	/* 0 means unlimited, so no BufferAccessStrategy required */
	if (ring_buffers == 0)
		return NULL;

	/* Cap to 1/8th of shared_buffers */
	ring_buffers = Min(NBuffers / 8, ring_buffers);

	/* NBuffers should never be less than 16, so this shouldn't happen */
	Assert(ring_buffers > 0);

	/* Allocate the object and initialize all elements to zeroes */
	strategy = (BufferAccessStrategy)
		palloc0(offsetof(BufferAccessStrategyData, buffers) +
				ring_buffers * sizeof(Buffer));

	/* Set fields that don't start out zero */
	strategy->btype = btype;
	strategy->nbuffers = ring_buffers;

	return strategy;
}

/*
 * GetAccessStrategyBufferCount -- an accessor for the number of buffers in
 *		the ring
 *
 * Returns 0 on NULL input to match behavior of GetAccessStrategyWithSize()
 * returning NULL with 0 size.
 */
int
GetAccessStrategyBufferCount(BufferAccessStrategy strategy)
{
	if (strategy == NULL)
		return 0;

	return strategy->nbuffers;
}

/*
 * GetAccessStrategyPinLimit -- get cap of number of buffers that should be pinned
 *
 * When pinning extra buffers to look ahead, users of a ring-based strategy are
 * in danger of pinning too much of the ring at once while performing look-ahead.
 * For some strategies, that means "escaping" from the ring, and in others it
 * means forcing dirty data to disk very frequently with associated WAL
 * flushing.  Since external code has no insight into any of that, allow
 * individual strategy types to expose a clamp that should be applied when
 * deciding on a maximum number of buffers to pin at once.
 *
 * Callers should combine this number with other relevant limits and take the
 * minimum.
 */
int
GetAccessStrategyPinLimit(BufferAccessStrategy strategy)
{
	if (strategy == NULL)
		return NBuffers;

	switch (strategy->btype)
	{
		case BAS_BULKREAD:

			/*
			 * Since BAS_BULKREAD uses StrategyRejectBuffer(), dirty buffers
			 * shouldn't be a problem and the caller is free to pin up to the
			 * entire ring at once.
			 */
			return strategy->nbuffers;

		default:

			/*
			 * Tell caller not to pin more than half the buffers in the ring.
			 * This is a trade-off between look ahead distance and deferring
			 * writeback and associated WAL traffic.
			 */
			return strategy->nbuffers / 2;
	}
}

/*
 * FreeAccessStrategy -- release a BufferAccessStrategy object
 *
 * A simple pfree would do at the moment, but we would prefer that callers
 * don't assume that much about the representation of BufferAccessStrategy.
 */
void
FreeAccessStrategy(BufferAccessStrategy strategy)
{
	/* don't crash if called on a "default" strategy */
	if (strategy != NULL)
		pfree(strategy);
}

/*
 * GetBufferFromRing -- returns a buffer from the ring, or NULL if the
 *		ring is empty / not usable.
 *
 * The buffer is pinned and marked as owned, using TrackNewBufferPin(), before
 * returning.
 */
static BufferDesc *
GetBufferFromRing(BufferAccessStrategy strategy, uint64 *buf_state)
{
	BufferDesc *buf;
	Buffer		bufnum;
	uint64		old_buf_state;
	uint64		local_buf_state;	/* to avoid repeated (de-)referencing */


	/* Advance to next ring slot */
	if (++strategy->current >= strategy->nbuffers)
		strategy->current = 0;

	/*
	 * If the slot hasn't been filled yet, tell the caller to allocate a new
	 * buffer with the normal allocation strategy.  He will then fill this
	 * slot by calling AddBufferToRing with the new buffer.
	 */
	bufnum = strategy->buffers[strategy->current];
	if (bufnum == InvalidBuffer)
		return NULL;

	buf = GetBufferDescriptor(bufnum - 1);

	/*
	 * Check whether the buffer can be used and pin it if so. Do this using a
	 * CAS loop, to avoid having to lock the buffer header.
	 */
	old_buf_state = pg_atomic_read_u64(&buf->state);
	for (;;)
	{
		local_buf_state = old_buf_state;

		/*
		 * If the buffer is pinned we cannot use it under any circumstances.
		 *
		 * If usage_count is 0 or 1 then the buffer is fair game (we expect 1,
		 * since our own previous usage of the ring element would have left it
		 * there, but it might've been decremented by clock-sweep since then).
		 * A higher usage_count indicates someone else has touched the buffer,
		 * so we shouldn't re-use it.
		 */
		if (BUF_STATE_GET_REFCOUNT(local_buf_state) != 0)
			break;

		{
#ifdef USE_DECOUPLED_USAGE_COUNT
			uint32		uc = enable_decoupled_usage_count
				? BufUsageMapGet(buf->buf_id)
				: BUF_STATE_GET_USAGECOUNT(local_buf_state);
#else
			uint32		uc = BUF_STATE_GET_USAGECOUNT(local_buf_state);
#endif

			if (uc > 1)
				break;
		}

		/* See equivalent code in PinBuffer() */
		if (unlikely(local_buf_state & BM_LOCKED))
		{
			old_buf_state = WaitBufHdrUnlocked(buf);
			continue;
		}

		/* pin the buffer if the CAS succeeds */
		local_buf_state += BUF_REFCOUNT_ONE;

		if (pg_atomic_compare_exchange_u64(&buf->state, &old_buf_state,
										   local_buf_state))
		{
			*buf_state = local_buf_state;

			TrackNewBufferPin(BufferDescriptorGetBuffer(buf));
			return buf;
		}
	}

	/*
	 * Tell caller to allocate a new buffer with the normal allocation
	 * strategy.  He'll then replace this ring element via AddBufferToRing.
	 */
	return NULL;
}

/*
 * AddBufferToRing -- add a buffer to the buffer ring
 *
 * Caller must hold the buffer header spinlock on the buffer.  Since this
 * is called with the spinlock held, it had better be quite cheap.
 */
static void
AddBufferToRing(BufferAccessStrategy strategy, BufferDesc *buf)
{
	strategy->buffers[strategy->current] = BufferDescriptorGetBuffer(buf);
}

/*
 * Utility function returning the IOContext of a given BufferAccessStrategy's
 * strategy ring.
 */
IOContext
IOContextForStrategy(BufferAccessStrategy strategy)
{
	if (!strategy)
		return IOCONTEXT_NORMAL;

	switch (strategy->btype)
	{
		case BAS_NORMAL:

			/*
			 * Currently, GetAccessStrategy() returns NULL for
			 * BufferAccessStrategyType BAS_NORMAL, so this case is
			 * unreachable.
			 */
			pg_unreachable();
			return IOCONTEXT_NORMAL;
		case BAS_BULKREAD:
			return IOCONTEXT_BULKREAD;
		case BAS_BULKWRITE:
			return IOCONTEXT_BULKWRITE;
		case BAS_VACUUM:
			return IOCONTEXT_VACUUM;
	}

	elog(ERROR, "unrecognized BufferAccessStrategyType: %d", strategy->btype);
	pg_unreachable();
}

/*
 * StrategyRejectBuffer -- consider rejecting a dirty buffer
 *
 * When a nondefault strategy is used, the buffer manager calls this function
 * when it turns out that the buffer selected by StrategyGetBuffer needs to
 * be written out and doing so would require flushing WAL too.  This gives us
 * a chance to choose a different victim.
 *
 * Returns true if buffer manager should ask for a new victim, and false
 * if this buffer should be written and re-used.
 */
bool
StrategyRejectBuffer(BufferAccessStrategy strategy, BufferDesc *buf, bool from_ring)
{
	/* We only do this in bulkread mode */
	if (strategy->btype != BAS_BULKREAD)
		return false;

	/* Don't muck with behavior of normal buffer-replacement strategy */
	if (!from_ring ||
		strategy->buffers[strategy->current] != BufferDescriptorGetBuffer(buf))
		return false;

	/*
	 * Remove the dirty buffer from the ring; necessary to prevent infinite
	 * loop if all ring members are dirty.
	 */
	strategy->buffers[strategy->current] = InvalidBuffer;

	return true;
}
