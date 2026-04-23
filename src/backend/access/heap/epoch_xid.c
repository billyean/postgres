/*-------------------------------------------------------------------------
 *
 * epoch_xid.c
 *	  XID64 epoch fork implementation.
 *
 * See epoch_xid.h for the storage layout and API documentation.
 * See DESIGN.xid64_epoch_side_v1.md for the full design specification.
 *
 * EXPERIMENTAL / v1 research prototype.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/epoch_xid.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/epoch_xid.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/storage_xlog.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/snapmgr.h"


/*
 * Test-only instrumentation for Patch 10 heap_fetch() epoch branch.
 *
 * Records the last path taken by EpochHeapTupleSatisfiesMVCC so tests
 * can directly prove which internal branch executed.
 *
 * Values:
 *   'n' = not yet called (initial)
 *   'm' = epoch branch, slot-backed materialized epoch data used
 *   'd' = epoch branch, no-slot fallback (PageIsNew / absent / beyond HWM)
 *   's' = epoch branch, slot-backed data + 64-bit snapshot bridge (Stage 1)
 *   'u' = epoch branch, returned CANNOT_DETERMINE (unresolvable)
 */
static char epoch_mvcc_last_path = 'n';

/*
 * Test-only override: when true, forces the Stage 1 bridge guard to false,
 * simulating a post-epoch-0 state without requiring an actual epoch wrap.
 * Used by epoch_xid_stage1_force_disable() to prove the negative side of
 * the Stage 1 boundary: that when the guard is off, the bridge is not used.
 */
static bool epoch_stage1_force_disabled = false;

/*
 * Test-only instrumentation: records which active-transaction membership
 * function was last used by EpochClassifyXidStatus.
 *
 * Values:
 *   'n' = not yet called (initial)
 *   '6' = 64-bit FullTransactionIdIsInProgress was used
 *   '3' = 32-bit TransactionIdIsInProgress was used (guard off or no full xid)
 */
static char epoch_classify_last_membership = 'n';

/*
 * Test-only instrumentation: records whether the 64-bit snapshot horizon
 * fast-reject was taken by the last EpochClassifyXidStatus call.
 *
 * Values:
 *   'n' = not taken (initial, or XID >= epoch_horizon, or epoch_horizon invalid)
 *   'h' = horizon fast-reject taken: XID preceded epoch_horizon, ProcArray
 *         membership scan was skipped
 *
 * The fast-reject is justified by snapshot semantics: epoch_horizon is the
 * 64-bit form of snapshot->xmin.  Any XID strictly preceding snapshot->xmin
 * had already completed before the snapshot was created, so it cannot be
 * in-progress.  This does not depend on RecentXmin == snapshot->xmin.
 */
static char epoch_classify_last_horizon = 'n';

/* Forward declaration: defined in the Phase 5 section, needed by Phase 4 */
static inline FullTransactionId
EpochFullXidRelativeTo(FullTransactionId ref, TransactionId xid);


/* ----------------------------------------------------------------
 *	Segment-aware sparse-file helpers
 *
 *	PostgreSQL relation forks are segmented: each segment file holds
 *	at most RELSEG_SIZE blocks.  Logical block N maps to:
 *
 *	  segment number    = N / RELSEG_SIZE
 *	  in-segment offset = N % RELSEG_SIZE
 *
 *	Segment 0 uses the fork's base relpath (e.g., "base/5/16384_epoch").
 *	Segment K>0 appends ".K" (e.g., "base/5/16384_epoch.1").
 *
 *	All sparse-extension and hole-inspection operations must use the
 *	correct physical segment file and in-segment offset.
 * ----------------------------------------------------------------
 */

#define EPOCH_SEG_PATH_MAXLEN (REL_PATH_STR_MAXLEN + 1 + 10 + 1)

/*
 * epoch_seg_path
 *
 * Build the filesystem path for epoch fork segment 'segno'.
 * Segment 0 = base relpath; segment K>0 = relpath + ".K".
 */
static void
epoch_seg_path(char *buf, size_t bufsz,
			   SMgrRelation smgr, BlockNumber segno)
{
	RelPathStr	base;

	base = relpath(smgr->smgr_rlocator, EPOCH_FORKNUM);
	if (segno == 0)
		strlcpy(buf, base.str, bufsz);
	else
		snprintf(buf, bufsz, "%s.%u", base.str, segno);
}


/*
 * epoch_seg_extend_sparse
 *
 * Extend a single epoch fork segment file to new_seg_blocks using
 * ftruncate().  This creates true filesystem holes for the new region:
 *
 *   Linux (ext4/XFS): ftruncate beyond EOF creates a file hole.
 *     The kernel does not allocate physical disk blocks for the gap.
 *     Reading the hole returns zeros.  This is POSIX-standard behavior.
 *
 *   macOS (APFS): ftruncate beyond EOF creates a sparse region.
 *     APFS does not allocate physical storage for the gap.  Reading
 *     returns zeros.  Supported since macOS 10.13 (High Sierra).
 *
 * Unlike smgrzeroextend (which uses posix_fallocate or pg_pwrite_zeros),
 * ftruncate is the only POSIX mechanism that creates true holes —
 * neither posix_fallocate nor pwrite preserves sparseness.
 *
 * If a platform or filesystem does not preserve holes (e.g., some
 * network filesystems), ftruncate still correctly extends the file
 * with zero-filled blocks.  Correctness is preserved because
 * PageIsNew (all-zero) is a legal absence state per Phase 6.
 * Sparseness is an optimization, not a correctness dependency.
 *
 * segno: physical segment number
 * new_seg_blocks: desired segment size in blocks
 */
static void
epoch_seg_extend_sparse(SMgrRelation smgr, BlockNumber segno,
						BlockNumber new_seg_blocks)
{
	char		segpath[EPOCH_SEG_PATH_MAXLEN];
	int			fd;

	epoch_seg_path(segpath, sizeof(segpath), smgr, segno);

	fd = BasicOpenFile(segpath, O_RDWR | O_CREAT | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open epoch fork segment \"%s\": %m",
						segpath)));

	if (ftruncate(fd, (off_t) new_seg_blocks * BLCKSZ) < 0)
	{
		int			save_errno = errno;

		close(fd);
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not extend epoch fork segment \"%s\" to %u blocks: %m",
						segpath, new_seg_blocks)));
	}

	close(fd);
}

/*
 * epoch_fork_extend_sparse
 *
 * Extend the epoch fork to cover logical block new_nblocks_total,
 * using segment-correct sparse extension via ftruncate.
 *
 * For each segment that needs to grow:
 *   - segment files before the target segment are extended to
 *     RELSEG_SIZE blocks (full segment, sparse holes)
 *   - the target segment file is extended to cover the target
 *     in-segment block (sparse holes for untouched blocks)
 *   - only the target block itself will later be materialized
 *     by the caller; all other blocks remain as filesystem holes
 *
 * After extension, the smgr cached nblocks is invalidated so
 * subsequent smgrnblocks() queries re-read from the kernel.
 *
 * Caller must hold the relation extension lock.
 */
static void
epoch_fork_extend_sparse(SMgrRelation smgr, BlockNumber new_nblocks_total)
{
	BlockNumber target_segno;
	BlockNumber target_seg_blocks;
	BlockNumber seg;
	BlockNumber current_nblocks;
	BlockNumber current_last_segno;
	BlockNumber current_seg_blocks_in_last;

	target_segno = new_nblocks_total / ((BlockNumber) RELSEG_SIZE);
	target_seg_blocks = new_nblocks_total % ((BlockNumber) RELSEG_SIZE);
	if (target_seg_blocks == 0 && new_nblocks_total > 0)
	{
		target_segno--;
		target_seg_blocks = (BlockNumber) RELSEG_SIZE;
	}

	/*
	 * Determine current fork size to avoid redundant segment operations.
	 * smgrnblocks is safe here because the caller holds the extension
	 * lock, preventing concurrent modification.  It ultimately calls
	 * lseek(SEEK_END) which returns the correct size even after our
	 * prior ftruncate calls in the same session.
	 */
	current_nblocks = smgrnblocks(smgr, EPOCH_FORKNUM);
	if (current_nblocks > 0)
	{
		current_last_segno = (current_nblocks - 1) / ((BlockNumber) RELSEG_SIZE);
		current_seg_blocks_in_last =
			current_nblocks - current_last_segno * ((BlockNumber) RELSEG_SIZE);
	}
	else
	{
		current_last_segno = 0;
		current_seg_blocks_in_last = 0;
	}

	/*
	 * Extend each segment that needs to grow.  Segments before the target
	 * segment are extended to full RELSEG_SIZE.  The target segment is
	 * extended to cover the target in-segment block.
	 */
	for (seg = 0; seg <= target_segno; seg++)
	{
		BlockNumber needed;
		BlockNumber have;

		if (seg < target_segno)
			needed = (BlockNumber) RELSEG_SIZE;
		else
			needed = target_seg_blocks;

		/* What does this segment currently have? */
		if (seg < current_last_segno)
			have = (BlockNumber) RELSEG_SIZE;  /* full prior segment */
		else if (seg == current_last_segno)
			have = current_seg_blocks_in_last;
		else
			have = 0;  /* segment does not exist yet */

		if (needed > have)
			epoch_seg_extend_sparse(smgr, seg, needed);
	}

	/* Invalidate smgr cache so next nblocks query re-reads from kernel */
	smgr->smgr_cached_nblocks[EPOCH_FORKNUM] = InvalidBlockNumber;
}


/* ----------------------------------------------------------------
 *	Fork and page management
 * ----------------------------------------------------------------
 */

/*
 * epoch_fork_exists_on_disk
 *
 * Low-level helper: check whether the epoch fork file exists by stat().
 *
 * We avoid smgrexists() because its implementation (mdexists) calls
 * mdclose() + mdopenfork().  When the fork does not yet exist,
 * mdopenfork() returns NULL but mdclose() has already executed,
 * and the resulting md_num_open_segs state can be inconsistent on
 * some platforms — tripping Assert(md_num_open_segs[forknum] == 0)
 * in a subsequent mdcreate().  Direct stat() has no side effects on
 * smgr internal state.
 */
static bool
epoch_fork_exists_on_disk(SMgrRelation smgr)
{
	RelPathStr	path;
	struct stat	st;

	path = relpath(smgr->smgr_rlocator, EPOCH_FORKNUM);
	return (stat(path.str, &st) == 0);
}

/*
 * EpochRelationIsMaterialized
 *
 * Check whether a relation has a materialized epoch fork.
 * Returns true iff the epoch fork file exists on disk.
 */
bool
EpochRelationIsMaterialized(Relation rel)
{
	return epoch_fork_exists_on_disk(RelationGetSmgr(rel));
}

/*
 * EpochEnsureFork
 *
 * Create the epoch fork for a relation if it does not already exist.
 * After this call, the relation is in materialized epoch mode.
 *
 * Safe to call multiple times; uses extension lock to serialize
 * concurrent fork creation.
 */
void
EpochEnsureFork(Relation rel)
{
	SMgrRelation smgr = RelationGetSmgr(rel);

	if (epoch_fork_exists_on_disk(smgr))
		return;

	LockRelationForExtension(rel, ExclusiveLock);

	/* Re-check after acquiring lock (another backend may have created it) */
	smgr = RelationGetSmgr(rel);
	if (epoch_fork_exists_on_disk(smgr))
	{
		UnlockRelationForExtension(rel, ExclusiveLock);
		return;
	}

	smgrcreate(smgr, EPOCH_FORKNUM, false);

	if (RelationNeedsWAL(rel))
		log_smgrcreate(&smgr->smgr_rlocator.locator, EPOCH_FORKNUM);

	UnlockRelationForExtension(rel, ExclusiveLock);
}

/*
 * EpochPageInit
 *
 * Initialize an epoch page using standard PageHeaderData plus epoch
 * opaque data.  All slots are zeroed (flags = 0, meaning "unset").
 */
void
EpochPageInit(Page page)
{
	EpochPageOpaque opaque;

	PageInit(page, BLCKSZ, 0);

	opaque = EpochPageGetOpaque(page);
	opaque->epoch_version = EPOCH_PAGE_VERSION;
	opaque->num_slots = 0;

	MemSet(EpochPageGetSlots(page), 0,
		   MaxEpochSlotsPerPage * SizeOfEpochSlotData);
}

/*
 * EpochReadBuffer
 *
 * Read (and optionally extend) the epoch buffer for a heap block.
 * Returns a pinned but NOT locked buffer.
 *
 * When extending, uses ftruncate-based sparse extension so that
 * intermediate blocks between the current EOF and the target become
 * true filesystem holes (Linux ext4/XFS, macOS APFS).  Only the
 * target block is materialized with epoch page metadata.
 *
 * Extension is segment-aware: logical block N maps to segment
 * N / RELSEG_SIZE at in-segment offset N % RELSEG_SIZE.  Each
 * segment file is independently extended via ftruncate.
 *
 * If extend_ok is false and the block does not exist, returns
 * InvalidBuffer.
 */
Buffer
EpochReadBuffer(Relation rel, BlockNumber heapBlk, bool extend_ok)
{
	SMgrRelation smgr;
	BlockNumber	nblocks;
	Buffer		buf;

	EpochEnsureFork(rel);

	smgr = RelationGetSmgr(rel);
	nblocks = smgrnblocks(smgr, EPOCH_FORKNUM);

	if (heapBlk < nblocks)
	{
		buf = ReadBufferExtended(rel, EPOCH_FORKNUM, heapBlk,
								 RBM_NORMAL, NULL);
		return buf;
	}

	if (!extend_ok)
		return InvalidBuffer;

	/* Extend the fork to cover heapBlk */
	LockRelationForExtension(rel, ExclusiveLock);
	smgr = RelationGetSmgr(rel);
	nblocks = smgrnblocks(smgr, EPOCH_FORKNUM);

	/* Re-check: another backend may have extended past our target */
	if (heapBlk < nblocks)
	{
		UnlockRelationForExtension(rel, ExclusiveLock);
		return ReadBufferExtended(rel, EPOCH_FORKNUM, heapBlk,
								 RBM_NORMAL, NULL);
	}

	/*
	 * Sparse extension via ftruncate.
	 *
	 * Grow the fork's segment file(s) to cover block heapBlk.
	 * ftruncate creates true filesystem holes for all new blocks —
	 * no physical disk allocation occurs on Linux (ext4/XFS) or
	 * macOS (APFS).
	 *
	 * Only the target block is then materialized in shared_buffers.
	 * Intermediate hole-backed blocks remain unmaterialized until
	 * their first DML write touch, at which point the existing
	 * PageIsNew guards in heapam.c initialize them on demand.
	 */
	epoch_fork_extend_sparse(smgr, heapBlk + 1);

	buf = ReadBufferExtended(rel, EPOCH_FORKNUM, heapBlk,
							 RBM_ZERO_AND_LOCK, NULL);
	{
		Page		page = BufferGetPage(buf);

		EpochPageInit(page);
		MarkBufferDirty(buf);
	}
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	UnlockRelationForExtension(rel, ExclusiveLock);
	Assert(BufferIsValid(buf));
	return buf;
}

/*
 * EpochPinBuffer
 *
 * Pin the epoch buffer for heapBlk, reusing *epochbuf if it already
 * covers the right block.  Follows the visibilitymap_pin() pattern.
 */
void
EpochPinBuffer(Relation rel, BlockNumber heapBlk, Buffer *epochbuf)
{
	if (BufferIsValid(*epochbuf))
	{
		if (BufferGetBlockNumber(*epochbuf) == heapBlk)
			return;
		ReleaseBuffer(*epochbuf);
	}

	*epochbuf = EpochReadBuffer(rel, heapBlk, true);
}

/*
 * EpochReadBufferReadOnly
 *
 * Read-only epoch buffer access for inspection / read paths (Phase 6).
 *
 * Uses the backend's current view of the relation and fork state to
 * determine whether the fork exists and whether the requested block
 * is within EOF.
 *
 * Returns InvalidBuffer if:
 *   - Fork does not exist (relation in implicit mode)
 *   - Block is beyond EOF
 * Both are legal absence states per the storage contract.
 *
 * Contract guarantees:
 *   - Never creates the epoch fork
 *   - Never extends the epoch fork
 *   - Never initializes, dirties, or WAL-logs any page
 *
 * Caller must lock the returned buffer and treat PageIsNew pages
 * as absent (slot = NULL).  Returns a pinned but NOT locked buffer.
 */
Buffer
EpochReadBufferReadOnly(Relation rel, BlockNumber heapBlk)
{
	SMgrRelation smgr = RelationGetSmgr(rel);

	/* Fork absent → implicit mode, legal absence */
	if (!smgrexists(smgr, EPOCH_FORKNUM))
		return InvalidBuffer;

	/* Block beyond EOF → legal absence, no extension */
	if (heapBlk >= smgrnblocks(smgr, EPOCH_FORKNUM))
		return InvalidBuffer;

	return ReadBufferExtended(rel, EPOCH_FORKNUM, heapBlk,
							 RBM_NORMAL, NULL);
}

/*
 * EpochPageValidate
 *
 * Structural validation of a non-new epoch page (Phase 6).
 *
 * Returns true if the page is structurally valid as an epoch page;
 * false if corrupt (wrong version or impossible num_slots).
 *
 * Caller must have already verified !PageIsNew(page).
 * Caller decides whether to ERROR or log a warning on false.
 *
 * This enforces the corruption boundary: a non-new page that fails
 * this check must not be silently treated as absent/new.
 */
bool
EpochPageValidate(Page page)
{
	EpochPageOpaque opaque;

	Assert(!PageIsNew(page));

	opaque = EpochPageGetOpaque(page);

	if (opaque->epoch_version != EPOCH_PAGE_VERSION)
		return false;

	if (opaque->num_slots > MaxEpochSlotsPerPage)
		return false;

	return true;
}


/* ----------------------------------------------------------------
 *	Slot access
 * ----------------------------------------------------------------
 */

/*
 * EpochGetSlot
 *
 * Return a pointer to the EpochSlotData for a given OffsetNumber.
 * Caller must hold at least a shared lock on the epoch buffer.
 */
EpochSlotData *
EpochGetSlot(Page epochPage, OffsetNumber offnum)
{
	EpochSlotData *slots;

	if (offnum < 1 || offnum > MaxEpochSlotsPerPage)
		elog(ERROR, "epoch slot offset %u out of range [1, %d]",
			 offnum, MaxEpochSlotsPerPage);

	slots = EpochPageGetSlots(epochPage);
	return &slots[offnum - 1];
}

/*
 * EpochSlotInitForInsert
 *
 * Initialize a slot for a freshly inserted tuple.  This is the ONLY
 * correct entry point for writing epoch data during heap_insert.
 *
 * Writes the ENTIRE EpochSlotData struct:
 *   - xmin_epoch = high 32 bits of fxid
 *   - xmax_epoch = 0  (no xmax yet)
 *   - epoch_flags = EPOCH_FLAG_XMIN_SET  (not |=, this is assignment)
 *   - padding = 0
 *
 * This whole-entry write is critical for correctness: a reused slot
 * (LP_UNUSED -> LP_NORMAL) may contain stale xmax_epoch and
 * EPOCH_FLAG_XMAX_SET from a prior occupant.  Those must be cleared.
 *
 * Caller must hold exclusive lock on the epoch buffer.
 */
void
EpochSlotInitForInsert(Page epochPage, OffsetNumber offnum,
					   FullTransactionId fxid)
{
	EpochSlotData *slot = EpochGetSlot(epochPage, offnum);
	EpochPageOpaque opaque = EpochPageGetOpaque(epochPage);

	/* Whole-entry write: no field from a prior occupant survives */
	slot->xmin_epoch = EpochFromFullTransactionId(fxid);
	slot->xmax_epoch = 0;
	slot->epoch_flags = EPOCH_FLAG_XMIN_SET;	/* = not |= */
	slot->padding = 0;

	if (offnum > opaque->num_slots)
		opaque->num_slots = offnum;
}

/*
 * EpochSlotSetXmax
 *
 * Set the xmax epoch on a slot that is already owned by the current
 * tuple (was initialized by this tuple's insert).
 *
 * This is an additive operation: it sets xmax_epoch and adds
 * EPOCH_FLAG_XMAX_SET without disturbing xmin fields.
 *
 * Caller must hold exclusive lock on the epoch buffer.
 */
void
EpochSlotSetXmax(Page epochPage, OffsetNumber offnum,
				 FullTransactionId fxid)
{
	EpochSlotData *slot = EpochGetSlot(epochPage, offnum);
	EpochPageOpaque opaque = EpochPageGetOpaque(epochPage);

	slot->xmax_epoch = EpochFromFullTransactionId(fxid);
	slot->epoch_flags |= EPOCH_FLAG_XMAX_SET;

	if (offnum > opaque->num_slots)
		opaque->num_slots = offnum;
}


/* ----------------------------------------------------------------
 *	Full XID reconstruction
 * ----------------------------------------------------------------
 */

/*
 * EpochReconstructXmin
 *
 * Reconstruct the full 64-bit xmin from heap tuple header + epoch slot.
 *
 * If slot is NULL (e.g., epoch fork absent for this page), or
 * EPOCH_FLAG_XMIN_SET is not set, returns {EPOCH_DEFAULT_VALUE, xmin}.
 *
 * Frozen xmin returns {0, FrozenTransactionId}.
 * Invalid xmin returns InvalidFullTransactionId.
 */
FullTransactionId
EpochReconstructXmin(HeapTupleHeader htup, EpochSlotData *slot)
{
	TransactionId xmin = HeapTupleHeaderGetRawXmin(htup);

	if (htup->t_infomask & HEAP_XMIN_FROZEN)
		return FullTransactionIdFromEpochAndXid(0, FrozenTransactionId);

	if (!TransactionIdIsValid(xmin))
		return InvalidFullTransactionId;

	if (slot == NULL || !(slot->epoch_flags & EPOCH_FLAG_XMIN_SET))
		return FullTransactionIdFromEpochAndXid(EPOCH_DEFAULT_VALUE, xmin);

	return FullTransactionIdFromEpochAndXid(slot->xmin_epoch, xmin);
}

/*
 * EpochReconstructXmax
 *
 * Reconstruct the full 64-bit xmax from heap tuple header + epoch slot.
 *
 * HEAP_XMAX_INVALID -> InvalidFullTransactionId
 * HEAP_XMAX_IS_MULTI -> ERROR (unsupported in v1)
 * slot NULL or EPOCH_FLAG_XMAX_SET unset -> {EPOCH_DEFAULT_VALUE, xmax}
 */
FullTransactionId
EpochReconstructXmax(HeapTupleHeader htup, EpochSlotData *slot)
{
	TransactionId xmax = HeapTupleHeaderGetRawXmax(htup);

	if (htup->t_infomask & HEAP_XMAX_INVALID)
		return InvalidFullTransactionId;

	if (htup->t_infomask & HEAP_XMAX_IS_MULTI)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("epoch fork v1 does not support multixact xmax reconstruction")));

	if (!TransactionIdIsValid(xmax))
		return InvalidFullTransactionId;

	if (slot == NULL || !(slot->epoch_flags & EPOCH_FLAG_XMAX_SET))
		return FullTransactionIdFromEpochAndXid(EPOCH_DEFAULT_VALUE, xmax);

	return FullTransactionIdFromEpochAndXid(slot->xmax_epoch, xmax);
}


/* ----------------------------------------------------------------
 *	WAL redo support
 * ----------------------------------------------------------------
 */

/*
 * EpochRedoSlotUpdate
 *
 * Apply an xl_epoch_slot_update record to an epoch page during redo.
 *
 * If is_whole_entry_reset is true (insert path), the entire slot is
 * overwritten: xmin set, xmax cleared, flags assigned with =.
 *
 * If is_whole_entry_reset is false (delete path), only xmax fields
 * are set additively: xmax_epoch written, XMAX_SET flag merged with |=.
 */
void
EpochRedoSlotUpdate(Page epochPage, xl_epoch_slot_update *xlrec)
{
	EpochSlotData *slot;
	EpochPageOpaque opaque;

	slot = EpochGetSlot(epochPage, xlrec->offnum);
	opaque = EpochPageGetOpaque(epochPage);

	if (xlrec->is_whole_entry_reset)
	{
		/* Insert path: whole-entry rewrite clears stale prior state */
		slot->xmin_epoch = xlrec->xmin_epoch;
		slot->xmax_epoch = xlrec->xmax_epoch;
		slot->epoch_flags = xlrec->epoch_flags;	/* = not |= */
		slot->padding = 0;
	}
	else
	{
		/* Delete path: additive merge */
		if (xlrec->epoch_flags & EPOCH_FLAG_XMIN_SET)
			slot->xmin_epoch = xlrec->xmin_epoch;
		if (xlrec->epoch_flags & EPOCH_FLAG_XMAX_SET)
			slot->xmax_epoch = xlrec->xmax_epoch;
		slot->epoch_flags |= xlrec->epoch_flags;
	}

	if (xlrec->offnum > opaque->num_slots)
		opaque->num_slots = xlrec->offnum;
}


/* ----------------------------------------------------------------
 *	MultiXact read-side classification helper (Patch 8)
 *
 *	EpochClassifyMultiXactXmax -- classify a MultiXact xmax for Phase 4.
 *
 *	Decomposes the MultiXact, identifies the shape (locker-only vs
 *	updater-containing), and returns the effective EpochXidStatus plus the
 *	effective TransactionId that downstream Phase 4/5 logic should use.
 *
 *	Read-only: does not set hint bits, does not create MultiXacts, does
 *	not modify any page or buffer.
 *
 *	Supported cases:
 *	  - HEAP_LOCKED_UPGRADED: locker-only (skip decomposition)
 *	  - HEAP_XMAX_IS_LOCKED_ONLY: locker-only (skip decomposition)
 *	  - Resolvable updater: classify updater XID via CLOG
 *	  - Current xid is the updater: classified as IN_PROGRESS
 *
 *	Not separately supported in Patch 8:
 *	  - Current xid is a locker: no behavioral difference from any other
 *	    locker-only case, so not worth decomposing to detect
 *
 *	Unsupported cases (return EPOCH_XID_MULTIXACT_UNSUPPORTED):
 *	  - GetMultiXactIdMembers returns <= 0 (truncated SLRU)
 *	  - No updater found despite !LOCKED_ONLY (contradictory state)
 * ----------------------------------------------------------------
 */
static EpochXidStatus
EpochClassifyMultiXactXmax(HeapTupleHeader htup,
						   FullTransactionId epoch_horizon,
						   TransactionId *effective_xid_out)
{
	MultiXactId multi;
	MultiXactMember *members;
	int			nmembers;
	TransactionId updater_xid = InvalidTransactionId;
	int			i;

	Assert(htup->t_infomask & HEAP_XMAX_IS_MULTI);

	if (effective_xid_out)
		*effective_xid_out = InvalidTransactionId;

	/*
	 * Locker-only fast paths.  We intentionally do not decompose locker-only
	 * MultiXacts to check whether the current xid is a member.  For locker-only
	 * MultiXacts, current-xid membership does not change any Phase 4 or Phase 5
	 * output: the tuple is live_locked / visible / multixact_lockers_only
	 * regardless.  Decomposing would add SLRU access cost with no behavioral
	 * difference.  Current-xid-as-locker is therefore not a separately supported
	 * case in Patch 8.  (Current-xid-as-updater IS supported, because there the
	 * membership determines the visibility verdict.)
	 */

	/* Pre-9.3 pg_upgrade'd share-lock: always locker-only */
	if (HEAP_LOCKED_UPGRADED(htup->t_infomask))
		return EPOCH_XID_MULTIXACT_LOCKERS_ONLY;

	/* infomask guarantees no updater: locker-only */
	if (HEAP_XMAX_IS_LOCKED_ONLY(htup->t_infomask))
		return EPOCH_XID_MULTIXACT_LOCKERS_ONLY;

	/* !LOCKED_ONLY: decompose to find the updater */
	multi = HeapTupleHeaderGetRawXmax(htup);
	nmembers = GetMultiXactIdMembers(multi, &members, false, false);

	if (nmembers <= 0)
		return EPOCH_XID_MULTIXACT_UNSUPPORTED;

	for (i = 0; i < nmembers; i++)
	{
		if (ISUPDATE_from_mxstatus(members[i].status))
		{
			updater_xid = members[i].xid;
			break;
		}
	}

	pfree(members);

	if (!TransactionIdIsValid(updater_xid))
		return EPOCH_XID_MULTIXACT_UNSUPPORTED;

	/* Classify the updater's XID via CLOG (no hint bits available) */
	if (effective_xid_out)
		*effective_xid_out = updater_xid;

	if (TransactionIdIsCurrentTransactionId(updater_xid))
		return EPOCH_XID_IN_PROGRESS;

	/*
	 * 64-bit horizon fast-reject for the MultiXact updater (Patch 14).
	 *
	 * When epoch_horizon is valid, reconstruct the updater's full 64-bit XID
	 * using epoch_horizon as the anchor (within epoch 0, any FullTransactionId
	 * from the same epoch is a valid anchor for EpochFullXidRelativeTo).
	 * If the updater completed before the snapshot, skip the ProcArray scan.
	 */
	if (FullTransactionIdIsValid(epoch_horizon))
	{
		FullTransactionId full_updater =
			EpochFullXidRelativeTo(epoch_horizon, updater_xid);

		if (FullTransactionIdPrecedes(full_updater, epoch_horizon))
		{
			epoch_classify_last_horizon = 'h';

			if (TransactionIdDidCommit(updater_xid))
				return EPOCH_XID_COMMITTED;
			return EPOCH_XID_ABORTED;
		}
		epoch_classify_last_horizon = 'n';
	}

	if (TransactionIdIsInProgress(updater_xid))
		return EPOCH_XID_IN_PROGRESS;
	if (TransactionIdDidCommit(updater_xid))
		return EPOCH_XID_COMMITTED;

	return EPOCH_XID_ABORTED;
}


/* ----------------------------------------------------------------
 *	Read-side interpretation (Phase 3)
 * ----------------------------------------------------------------
 */

/*
 * EpochInterpModeString -- return text label for an interpretation mode.
 */
const char *
EpochInterpModeString(EpochInterpMode mode)
{
	switch (mode)
	{
		case EPOCH_INTERP_MATERIALIZED:
			return "materialized";
		case EPOCH_INTERP_IMPLICIT_DEFAULT:
			return "implicit_default";
		case EPOCH_INTERP_FROZEN:
			return "frozen";
		case EPOCH_INTERP_INVALID:
			return "invalid";
		case EPOCH_INTERP_INVALID_UNSET:
			return "invalid_unset";
		case EPOCH_INTERP_MULTIXACT:
			return "multixact";
	}
	return "unknown";
}

/*
 * EpochInterpretTuple -- produce full xid interpretation for one tuple.
 *
 * See epoch_xid.h for the full contract.  This function never raises ERROR.
 */
EpochTupleInterpResult
EpochInterpretTuple(HeapTupleHeader htup, EpochSlotData *slot,
					bool relation_is_materialized)
{
	EpochTupleInterpResult result;
	TransactionId xmin = HeapTupleHeaderGetRawXmin(htup);
	TransactionId xmax = HeapTupleHeaderGetRawXmax(htup);

	/* --- xmin interpretation --- */

	if (htup->t_infomask & HEAP_XMIN_FROZEN)
	{
		result.full_xmin = FullTransactionIdFromEpochAndXid(0, FrozenTransactionId);
		result.xmin_interp = EPOCH_INTERP_FROZEN;
	}
	else if (!TransactionIdIsValid(xmin))
	{
		result.full_xmin = InvalidFullTransactionId;
		result.xmin_interp = EPOCH_INTERP_INVALID;
	}
	else if (slot != NULL && (slot->epoch_flags & EPOCH_FLAG_XMIN_SET))
	{
		result.full_xmin = FullTransactionIdFromEpochAndXid(slot->xmin_epoch, xmin);
		result.xmin_interp = EPOCH_INTERP_MATERIALIZED;
	}
	else
	{
		result.full_xmin = FullTransactionIdFromEpochAndXid(EPOCH_DEFAULT_VALUE, xmin);
		result.xmin_interp = EPOCH_INTERP_IMPLICIT_DEFAULT;
	}

	/* --- xmax interpretation --- */

	if (htup->t_infomask & HEAP_XMAX_INVALID)
	{
		result.full_xmax = InvalidFullTransactionId;
		result.xmax_interp = EPOCH_INTERP_INVALID_UNSET;
	}
	else if (htup->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		/* Read path: MultiXact detected, decomposition deferred to Phase 4 */
		result.full_xmax = InvalidFullTransactionId;
		result.xmax_interp = EPOCH_INTERP_MULTIXACT;
	}
	else if (!TransactionIdIsValid(xmax))
	{
		result.full_xmax = InvalidFullTransactionId;
		result.xmax_interp = EPOCH_INTERP_INVALID;
	}
	else if (slot != NULL && (slot->epoch_flags & EPOCH_FLAG_XMAX_SET))
	{
		result.full_xmax = FullTransactionIdFromEpochAndXid(slot->xmax_epoch, xmax);
		result.xmax_interp = EPOCH_INTERP_MATERIALIZED;
	}
	else
	{
		result.full_xmax = FullTransactionIdFromEpochAndXid(EPOCH_DEFAULT_VALUE, xmax);
		result.xmax_interp = EPOCH_INTERP_IMPLICIT_DEFAULT;
	}

	return result;
}


/* ----------------------------------------------------------------
 *	Transaction-state classification (Phase 4)
 * ----------------------------------------------------------------
 */

const char *
EpochXidStatusString(EpochXidStatus status)
{
	switch (status)
	{
		case EPOCH_XID_COMMITTED:		return "committed";
		case EPOCH_XID_ABORTED:			return "aborted";
		case EPOCH_XID_IN_PROGRESS:		return "in_progress";
		case EPOCH_XID_FROZEN:			return "frozen";
		case EPOCH_XID_INVALID_UNSET:	return "invalid_unset";
		case EPOCH_XID_MULTIXACT_UNSUPPORTED: return "multixact_unsupported";
		case EPOCH_XID_MULTIXACT_LOCKERS_ONLY: return "multixact_lockers_only";
	}
	return "unknown";
}

const char *
EpochTupleStateString(EpochTupleState state)
{
	switch (state)
	{
		case EPOCH_TUPLE_LIVE_COMMITTED:		return "live_committed";
		case EPOCH_TUPLE_DEAD_COMMITTED:		return "dead_committed";
		case EPOCH_TUPLE_INSERTING_IN_PROGRESS:	return "inserting_in_progress";
		case EPOCH_TUPLE_DELETING_IN_PROGRESS:	return "deleting_in_progress";
		case EPOCH_TUPLE_ABORTED_INSERT:		return "aborted_insert";
		case EPOCH_TUPLE_FROZEN_LIVE:			return "frozen_live";
		case EPOCH_TUPLE_FROZEN_DELETED:		return "frozen_deleted";
		case EPOCH_TUPLE_MULTIXACT_UNCLASSIFIABLE: return "multixact_unclassifiable";
		case EPOCH_TUPLE_LIVE_LOCKED:		return "live_locked";
		case EPOCH_TUPLE_FROZEN_LOCKED:		return "frozen_locked";
	}
	return "unknown";
}

/*
 * EpochClassifyXidStatus -- classify transaction state of one xid.
 * Uses hint bits first, then CLOG lookup.  Read-only: does not set hint bits.
 *
 * full_xid: when valid, the 64-bit-aware FullTransactionIdIsInProgress is used
 * for the active-transaction membership check instead of the 32-bit
 * TransactionIdIsInProgress.  Pass InvalidFullTransactionId to use the 32-bit
 * path (e.g., from debug functions that lack the epoch-0 guard context).
 *
 * epoch_horizon: when valid, a 64-bit fast-reject is applied before the
 * ProcArray membership check.  If full_xid precedes epoch_horizon, the XID
 * was completed before the snapshot and is definitely not in-progress.
 * This is justified directly by snapshot semantics: epoch_horizon is the
 * 64-bit form of snapshot->xmin, and any XID preceding snapshot->xmin had
 * already completed before the snapshot was created.  This correctness
 * argument does not depend on any relationship with RecentXmin.
 * Pass InvalidFullTransactionId to skip the fast-reject (e.g., from debug
 * functions or when the epoch-0 guard is off).
 *
 * effective_xid_out: for MultiXact xmax with a resolvable updater, set to the
 * updater's TransactionId; otherwise set to the input xid.  Callers that need
 * the effective xid for Phase 5 snapshot checks must pass a non-NULL pointer.
 */
static EpochXidStatus
EpochClassifyXidStatus(TransactionId xid, FullTransactionId full_xid,
					   FullTransactionId epoch_horizon,
					   HeapTupleHeader htup, bool is_xmin,
					   TransactionId *effective_xid_out)
{
	if (effective_xid_out)
		*effective_xid_out = xid;

	if (is_xmin)
	{
		if (htup->t_infomask & HEAP_XMIN_FROZEN)
			return EPOCH_XID_FROZEN;
		if (htup->t_infomask & HEAP_XMIN_COMMITTED)
			return EPOCH_XID_COMMITTED;
		if (htup->t_infomask & HEAP_XMIN_INVALID)
			return EPOCH_XID_ABORTED;
	}
	else
	{
		if (htup->t_infomask & HEAP_XMAX_INVALID)
			return EPOCH_XID_INVALID_UNSET;
		if (htup->t_infomask & HEAP_XMAX_IS_MULTI)
			return EpochClassifyMultiXactXmax(htup, epoch_horizon,
											  effective_xid_out);
		if (htup->t_infomask & HEAP_XMAX_COMMITTED)
			return EPOCH_XID_COMMITTED;
	}

	if (!TransactionIdIsValid(xid))
		return EPOCH_XID_INVALID_UNSET;
	if (TransactionIdIsCurrentTransactionId(xid))
		return EPOCH_XID_IN_PROGRESS;

	/*
	 * 64-bit snapshot horizon fast-reject (Patch 14).
	 *
	 * epoch_horizon is the 64-bit form of snapshot->xmin, derived from
	 * epoch_anchor.  Any XID strictly preceding snapshot->xmin had already
	 * completed (committed or aborted) before the snapshot was created.
	 * A completed transaction cannot re-enter the ProcArray, so it is
	 * definitely not in-progress.  We can skip the ProcArray membership
	 * scan and proceed directly to the CLOG lookup for committed/aborted.
	 *
	 * This is justified directly by snapshot semantics and does not depend
	 * on any relationship between epoch_horizon and RecentXmin.
	 */
	if (FullTransactionIdIsValid(full_xid) &&
		FullTransactionIdIsValid(epoch_horizon) &&
		FullTransactionIdPrecedes(full_xid, epoch_horizon))
	{
		epoch_classify_last_horizon = 'h';

		if (TransactionIdDidCommit(xid))
			return EPOCH_XID_COMMITTED;
		return EPOCH_XID_ABORTED;
	}
	epoch_classify_last_horizon = 'n';

	/*
	 * Active-transaction membership check: use the 64-bit-aware function
	 * when a full XID is available (Patch 13), fall back to 32-bit otherwise.
	 */
	{
		bool		in_progress;

		if (FullTransactionIdIsValid(full_xid))
		{
			epoch_classify_last_membership = '6';
			in_progress = FullTransactionIdIsInProgress(full_xid);
		}
		else
		{
			epoch_classify_last_membership = '3';
			in_progress = TransactionIdIsInProgress(xid);
		}

		if (in_progress)
			return EPOCH_XID_IN_PROGRESS;
	}

	if (TransactionIdDidCommit(xid))
		return EPOCH_XID_COMMITTED;

	return EPOCH_XID_ABORTED;
}

/*
 * EpochDeriveTupleState -- derive tuple-state verdict from xmin/xmax status.
 * Priority-ordered derivation per the accepted Phase 4 contract.
 */
static EpochTupleState
EpochDeriveTupleState(EpochXidStatus xmin_st, EpochXidStatus xmax_st)
{
	/* Locker-only MultiXact: tuple not deleted/updated by xmax */
	if (xmax_st == EPOCH_XID_MULTIXACT_LOCKERS_ONLY)
	{
		if (xmin_st == EPOCH_XID_FROZEN)
			return EPOCH_TUPLE_FROZEN_LOCKED;
		if (xmin_st == EPOCH_XID_IN_PROGRESS)
			return EPOCH_TUPLE_INSERTING_IN_PROGRESS;
		if (xmin_st == EPOCH_XID_ABORTED)
			return EPOCH_TUPLE_ABORTED_INSERT;
		return EPOCH_TUPLE_LIVE_LOCKED;
	}

	/* Unresolvable MultiXact: members unavailable or contradictory state */
	if (xmax_st == EPOCH_XID_MULTIXACT_UNSUPPORTED)
		return EPOCH_TUPLE_MULTIXACT_UNCLASSIFIABLE;

	if (xmin_st == EPOCH_XID_FROZEN)
	{
		if (xmax_st == EPOCH_XID_COMMITTED)
			return EPOCH_TUPLE_FROZEN_DELETED;
		return EPOCH_TUPLE_FROZEN_LIVE;
	}

	if (xmin_st == EPOCH_XID_IN_PROGRESS)
		return EPOCH_TUPLE_INSERTING_IN_PROGRESS;
	if (xmin_st == EPOCH_XID_ABORTED)
		return EPOCH_TUPLE_ABORTED_INSERT;

	if (xmin_st == EPOCH_XID_COMMITTED)
	{
		if (xmax_st == EPOCH_XID_INVALID_UNSET || xmax_st == EPOCH_XID_ABORTED)
			return EPOCH_TUPLE_LIVE_COMMITTED;
		if (xmax_st == EPOCH_XID_IN_PROGRESS)
			return EPOCH_TUPLE_DELETING_IN_PROGRESS;
		if (xmax_st == EPOCH_XID_COMMITTED)
			return EPOCH_TUPLE_DEAD_COMMITTED;
		return EPOCH_TUPLE_LIVE_COMMITTED;
	}

	return EPOCH_TUPLE_LIVE_COMMITTED;
}


/* ----------------------------------------------------------------
 *	Snapshot-relative visibility (Phase 5)
 * ----------------------------------------------------------------
 */

const char *
EpochVisibilityVerdictString(EpochVisibilityVerdict v)
{
	switch (v)
	{
		case EPOCH_VIS_VISIBLE:					return "visible";
		case EPOCH_VIS_INVISIBLE:				return "invisible";
		case EPOCH_VIS_MULTIXACT_UNSUPPORTED:	return "multixact_unsupported";
		case EPOCH_VIS_CANNOT_CLASSIFY:			return "cannot_classify";
	}
	return "unknown";
}

const char *
EpochVerdictReasonString(EpochVerdictReason r)
{
	switch (r)
	{
		case EPOCH_REASON_XMIN_COMMITTED_VISIBLE:	return "xmin_committed_visible";
		case EPOCH_REASON_FROZEN:					return "frozen";
		case EPOCH_REASON_OWN_INSERT_VISIBLE:		return "own_insert_visible";
		case EPOCH_REASON_XMIN_IN_PROGRESS:			return "xmin_in_progress";
		case EPOCH_REASON_XMIN_ABORTED:				return "xmin_aborted";
		case EPOCH_REASON_XMIN_COMMITTED_NOT_IN_SNAPSHOT: return "xmin_committed_not_in_snapshot";
		case EPOCH_REASON_XMAX_COMMITTED_VISIBLE_IN_SNAPSHOT: return "xmax_committed_visible_in_snapshot";
		case EPOCH_REASON_XMAX_COMMITTED_NOT_IN_SNAPSHOT: return "xmax_committed_not_in_snapshot";
		case EPOCH_REASON_XMAX_IN_PROGRESS:			return "xmax_in_progress";
		case EPOCH_REASON_OWN_DELETE_INVISIBLE:		return "own_delete_invisible";
		case EPOCH_REASON_MULTIXACT_UNSUPPORTED:	return "multixact_unsupported";
		case EPOCH_REASON_MULTIXACT_LOCKERS_ONLY: return "multixact_lockers_only";
		case EPOCH_REASON_NO_SNAPSHOT:				return "no_snapshot";
	}
	return "unknown";
}

/*
 * EpochDeriveVisibility -- snapshot-relative visibility verdict.
 * Caller provides the snapshot explicitly.
 */
static void
EpochDeriveVisibility(HeapTupleHeader htup,
					  TransactionId xmin_xid, TransactionId xmax_xid,
					  EpochXidStatus xmin_status, EpochXidStatus xmax_status,
					  Snapshot snapshot,
					  EpochVisibilityVerdict *verdict,
					  EpochVerdictReason *reason)
{
	if (snapshot == NULL)
	{
		*verdict = EPOCH_VIS_CANNOT_CLASSIFY;
		*reason = EPOCH_REASON_NO_SNAPSHOT;
		return;
	}

	if (xmax_status == EPOCH_XID_MULTIXACT_UNSUPPORTED)
	{
		*verdict = EPOCH_VIS_MULTIXACT_UNSUPPORTED;
		*reason = EPOCH_REASON_MULTIXACT_UNSUPPORTED;
		return;
	}

	if (xmin_status == EPOCH_XID_FROZEN)
	{
		*verdict = EPOCH_VIS_VISIBLE;
		*reason = EPOCH_REASON_FROZEN;
		return;
	}

	if (xmin_status == EPOCH_XID_ABORTED)
	{
		*verdict = EPOCH_VIS_INVISIBLE;
		*reason = EPOCH_REASON_XMIN_ABORTED;
		return;
	}

	if (xmin_status == EPOCH_XID_IN_PROGRESS)
	{
		if (TransactionIdIsCurrentTransactionId(xmin_xid))
		{
			if (xmax_status == EPOCH_XID_INVALID_UNSET ||
				xmax_status == EPOCH_XID_ABORTED ||
				xmax_status == EPOCH_XID_MULTIXACT_LOCKERS_ONLY)
			{
				*verdict = EPOCH_VIS_VISIBLE;
				*reason = (xmax_status == EPOCH_XID_MULTIXACT_LOCKERS_ONLY)
					? EPOCH_REASON_MULTIXACT_LOCKERS_ONLY
					: EPOCH_REASON_OWN_INSERT_VISIBLE;
				return;
			}
			if (TransactionIdIsValid(xmax_xid) &&
				TransactionIdIsCurrentTransactionId(xmax_xid))
			{
				*verdict = EPOCH_VIS_INVISIBLE;
				*reason = EPOCH_REASON_OWN_DELETE_INVISIBLE;
				return;
			}
			*verdict = EPOCH_VIS_VISIBLE;
			*reason = EPOCH_REASON_OWN_INSERT_VISIBLE;
			return;
		}
		*verdict = EPOCH_VIS_INVISIBLE;
		*reason = EPOCH_REASON_XMIN_IN_PROGRESS;
		return;
	}

	/* Committed xmin: check snapshot */
	Assert(xmin_status == EPOCH_XID_COMMITTED);

	if (XidInMVCCSnapshot(xmin_xid, snapshot))
	{
		*verdict = EPOCH_VIS_INVISIBLE;
		*reason = EPOCH_REASON_XMIN_COMMITTED_NOT_IN_SNAPSHOT;
		return;
	}

	/* xmin visible in snapshot → check xmax */
	if (xmax_status == EPOCH_XID_INVALID_UNSET ||
		xmax_status == EPOCH_XID_ABORTED ||
		xmax_status == EPOCH_XID_MULTIXACT_LOCKERS_ONLY)
	{
		*verdict = EPOCH_VIS_VISIBLE;
		*reason = (xmax_status == EPOCH_XID_MULTIXACT_LOCKERS_ONLY)
			? EPOCH_REASON_MULTIXACT_LOCKERS_ONLY
			: EPOCH_REASON_XMIN_COMMITTED_VISIBLE;
		return;
	}

	if (xmax_status == EPOCH_XID_IN_PROGRESS)
	{
		if (TransactionIdIsValid(xmax_xid) &&
			TransactionIdIsCurrentTransactionId(xmax_xid))
		{
			*verdict = EPOCH_VIS_INVISIBLE;
			*reason = EPOCH_REASON_OWN_DELETE_INVISIBLE;
			return;
		}
		*verdict = EPOCH_VIS_VISIBLE;
		*reason = EPOCH_REASON_XMAX_IN_PROGRESS;
		return;
	}

	Assert(xmax_status == EPOCH_XID_COMMITTED);

	if (XidInMVCCSnapshot(xmax_xid, snapshot))
	{
		*verdict = EPOCH_VIS_VISIBLE;
		*reason = EPOCH_REASON_XMAX_COMMITTED_NOT_IN_SNAPSHOT;
		return;
	}

	*verdict = EPOCH_VIS_INVISIBLE;
	*reason = EPOCH_REASON_XMAX_COMMITTED_VISIBLE_IN_SNAPSHOT;
}


/* ----------------------------------------------------------------
 *	Stage 1 snapshot bridge (Patch 11)
 *
 *	FullXidInMVCCSnapshot -- 64-bit-aware snapshot membership check.
 *
 *	Given a full 64-bit XID (from epoch-fork Phase 3 reconstruction) and
 *	an MVCC snapshot with an epoch_anchor, determines whether the XID is
 *	"in the snapshot" (i.e., was in-progress when the snapshot was taken).
 *
 *	Uses the snapshot's epoch_anchor to reconstruct 64-bit boundaries from
 *	the existing 32-bit xmin/xmax/xip arrays via signed 32-bit arithmetic.
 *	This is safe because PostgreSQL's VACUUM wraparound protection guarantees
 *	the active XID range stays within 2^31.
 *
 *	Operational boundary: Stage 1 is validated for pre-wrap / epoch-0
 *	operation.  Cross-epoch safety requires Stages 2-3 (ProcArray/horizon
 *	64-bit awareness).  Fallback to native XidInMVCCSnapshot is safe only
 *	within epoch 0.
 * ----------------------------------------------------------------
 */

/*
 * Reconstruct a FullTransactionId from a 32-bit XID relative to a reference
 * FullTransactionId.  Same logic as procarray.c's FullXidRelativeTo (which
 * is static inline there).
 */
static inline FullTransactionId
EpochFullXidRelativeTo(FullTransactionId ref, TransactionId xid)
{
	TransactionId ref_xid = XidFromFullTransactionId(ref);

	return FullTransactionIdFromU64(
		U64FromFullTransactionId(ref) + (int32) (xid - ref_xid));
}

/*
 * FullXidInMVCCSnapshot -- 64-bit XID snapshot membership check.
 *
 * Returns true if fxid is "in the snapshot" (in-progress at snapshot time).
 * Returns false if fxid committed before the snapshot.
 *
 * Mirrors XidInMVCCSnapshot logic but with 64-bit comparisons.
 */
static bool
FullXidInMVCCSnapshot(FullTransactionId fxid, Snapshot snapshot)
{
	FullTransactionId anchor = snapshot->epoch_anchor;
	FullTransactionId full_xmin;
	FullTransactionId full_xmax;
	uint32		i;

	Assert(FullTransactionIdIsValid(anchor));

	/* Reconstruct 64-bit boundaries from anchor + 32-bit fields */
	full_xmin = EpochFullXidRelativeTo(anchor, snapshot->xmin);
	full_xmax = EpochFullXidRelativeTo(anchor, snapshot->xmax);

	/* Quick range checks using 64-bit comparison */
	if (FullTransactionIdPrecedes(fxid, full_xmin))
		return false;		/* committed before snapshot */
	if (FullTransactionIdFollowsOrEquals(fxid, full_xmax))
		return true;		/* started after snapshot */

	/*
	 * Check subxip array (if not overflowed).
	 * Promote each 32-bit entry to 64-bit via the anchor.
	 */
	if (!snapshot->suboverflowed)
	{
		int32		j;

		for (j = 0; j < snapshot->subxcnt; j++)
		{
			FullTransactionId full_subxid =
				EpochFullXidRelativeTo(anchor, snapshot->subxip[j]);

			if (FullTransactionIdEquals(full_subxid, fxid))
				return true;
		}
		/* fxid is not a known-running subtransaction in this range */
	}

	/* Check main xip array */
	for (i = 0; i < snapshot->xcnt; i++)
	{
		FullTransactionId full_xip =
			EpochFullXidRelativeTo(anchor, snapshot->xip[i]);

		if (FullTransactionIdEquals(full_xip, fxid))
			return true;
	}

	/*
	 * If subxip overflowed, check whether fxid's toplevel parent is in xip.
	 * SubTransGetTopmostTransaction uses 32-bit XID, which is correct for
	 * Stage 1 (epoch-0 operation).
	 */
	if (snapshot->suboverflowed)
	{
		TransactionId xid32 = XidFromFullTransactionId(fxid);
		TransactionId parentXid = SubTransGetTopmostTransaction(xid32);
		FullTransactionId full_parent = EpochFullXidRelativeTo(anchor, parentXid);

		for (i = 0; i < snapshot->xcnt; i++)
		{
			FullTransactionId full_xip =
				EpochFullXidRelativeTo(anchor, snapshot->xip[i]);

			if (FullTransactionIdEquals(full_xip, full_parent))
				return true;
		}
	}

	return false;	/* not in snapshot: committed before snapshot boundary */
}


/* ----------------------------------------------------------------
 *	Real heap visibility consumer (Patch 10)
 *
 *	EpochHeapTupleSatisfiesMVCC -- MVCC visibility using epoch fork.
 *
 *	Called from the guarded branch in heap_fetch() for epoch-materialized
 *	relations with MVCC snapshots.  This is the first real heap visibility
 *	consumer of the epoch fork prototype.
 *
 *	Genuinely consumes epoch-fork metadata:
 *	  - Calls Phase 3 (EpochInterpretTuple) to reconstruct full 64-bit
 *	    XIDs from the epoch slot data + heap tuple header
 *	  - Uses the epoch-reconstructed XIDs (not raw header XIDs) as the
 *	    basis for the MVCC visibility logic
 *	  - Uses Phase 3 interpretation modes to handle special cases
 *	    (FROZEN, MULTIXACT, INVALID) from epoch-fork context
 *
 *	For Patch 10 (32-bit snapshots), XidInMVCCSnapshot still uses the
 *	low 32 bits extracted from the reconstructed 64-bit XID.  The result
 *	is provably identical to the native path for epoch 0.  Future patches
 *	with 64-bit snapshots will use the full reconstructed value.
 *
 *	Read-only: does not set hint bits, does not modify any page.
 *
 *	Returns CANNOT_DETERMINE for unresolvable MultiXact cases; the
 *	caller (heap_fetch) falls back to native HeapTupleSatisfiesVisibility.
 * ----------------------------------------------------------------
 */
EpochMVCCResult
EpochHeapTupleSatisfiesMVCC(Relation rel, HeapTuple htup,
							Snapshot snapshot, Buffer heapbuf)
{
	HeapTupleHeader tuple = htup->t_data;
	BlockNumber blkno = ItemPointerGetBlockNumber(&htup->t_self);
	OffsetNumber offnum = ItemPointerGetOffsetNumber(&htup->t_self);
	bool		is_materialized;
	EpochSlotData *slot = NULL;
	Buffer		epochbuf = InvalidBuffer;
	EpochTupleInterpResult interp;
	EpochXidStatus xmin_status,
				xmax_status;
	TransactionId xmin_xid,
				xmax_xid,
				effective_xmax;
	bool		use_64bit_snapshot;

	Assert(snapshot != NULL);
	Assert(snapshot->snapshot_type == SNAPSHOT_MVCC);

	/* Reset per-invocation instrumentation */
	epoch_classify_last_horizon = 'n';

	/* Read epoch slot data (read-only: no create/extend/dirty) */
	is_materialized = EpochRelationIsMaterialized(rel);
	if (is_materialized)
	{
		epochbuf = EpochReadBufferReadOnly(rel, blkno);
		if (BufferIsValid(epochbuf))
		{
			Page		epochpage;

			LockBuffer(epochbuf, BUFFER_LOCK_SHARE);
			epochpage = BufferGetPage(epochbuf);
			if (!PageIsNew(epochpage) && EpochPageValidate(epochpage))
			{
				EpochPageOpaque opaque = EpochPageGetOpaque(epochpage);

				if (offnum <= opaque->num_slots)
					slot = EpochGetSlot(epochpage, offnum);
			}
		}
	}

	/*
	 * Phase 3: reconstruct full 64-bit XIDs from epoch slot + tuple header.
	 * This is the real epoch-fork consumption point — the full XIDs produced
	 * here depend on the epoch slot's xmin_epoch / xmax_epoch / epoch_flags.
	 */
	interp = EpochInterpretTuple(tuple, slot, is_materialized);

	/* Release epoch buffer now that Phase 3 has consumed the slot */
	if (BufferIsValid(epochbuf))
		UnlockReleaseBuffer(epochbuf);

	/*
	 * Use the epoch-reconstructed XIDs for the MVCC logic below.
	 * For non-MultiXact xmax, the XID comes from Phase 3 reconstruction.
	 * For MultiXact xmax, Phase 4 decomposes to find the effective updater.
	 */
	xmin_xid = XidFromFullTransactionId(interp.full_xmin);

	/*
	 * Stage 1 bounded guard (Patch 11, unchanged by Patch 12): determine
	 * whether the 64-bit epoch-aware paths are available.  This gates both
	 * the Phase 4 membership check (Patch 12) and the Phase 5 snapshot
	 * comparison (Patch 11).  It requires:
	 *
	 * (1) A valid epoch_anchor in the snapshot (set by GetSnapshotData).
	 *     Imported/special snapshots may have epoch_anchor = 0; for those
	 *     we fall back to 32-bit comparison.
	 *
	 * (2) The anchor must be in epoch 0 (pre-wrap).  Stage 1 is validated
	 *     ONLY for pre-wrap / epoch-0 operation.  Once the system crosses
	 *     into epoch 1+, the fallback-to-native path is no longer safe for
	 *     CANNOT_DETERMINE cases and other non-epoch-wired visibility paths.
	 *     Cross-epoch safety requires further ProcArray/horizon work.
	 *
	 * This is a real code boundary, not just a comment.
	 */
	use_64bit_snapshot = !epoch_stage1_force_disabled &&
		FullTransactionIdIsValid(snapshot->epoch_anchor) &&
		(EpochFromFullTransactionId(snapshot->epoch_anchor) == 0);

	/*
	 * Phase 4: classify xmin and xmax status (hint bits + CLOG).
	 * For xmax, also obtain the effective updater XID for MultiXact cases.
	 *
	 * When the Stage 1 guard passes, pass the full 64-bit XIDs so Phase 4
	 * uses FullTransactionIdIsInProgress for the membership check (Patch 13),
	 * and derive a 64-bit snapshot horizon for the fast-reject (Patch 14).
	 *
	 * epoch_horizon is the 64-bit form of snapshot->xmin: any XID preceding
	 * it had already completed before the snapshot was created, so the
	 * ProcArray membership scan can be skipped.  This is justified directly
	 * by snapshot semantics; it does not depend on RecentXmin.
	 *
	 * When the guard is off, pass InvalidFullTransactionId for both full_xid
	 * and epoch_horizon so Phase 4 falls back to the 32-bit path.
	 */
	{
		FullTransactionId epoch_horizon = InvalidFullTransactionId;

		if (use_64bit_snapshot)
			epoch_horizon = EpochFullXidRelativeTo(snapshot->epoch_anchor,
												   snapshot->xmin);

		xmin_status = EpochClassifyXidStatus(xmin_xid,
						use_64bit_snapshot ? interp.full_xmin : InvalidFullTransactionId,
						epoch_horizon,
						tuple, true, NULL);
		xmax_status = EpochClassifyXidStatus(
						HeapTupleHeaderGetRawXmax(tuple),
						use_64bit_snapshot ? interp.full_xmax : InvalidFullTransactionId,
						epoch_horizon,
						tuple, false,
						&effective_xmax);
	}

	/* Unresolvable MultiXact: fall back to native path */
	if (xmax_status == EPOCH_XID_MULTIXACT_UNSUPPORTED)
	{
		epoch_mvcc_last_path = 'u';
		return EPOCH_MVCC_CANNOT_DETERMINE;
	}

	/*
	 * Determine the xmax XID to use for snapshot comparison.
	 * For MultiXact: use the effective updater from Phase 4 decomposition.
	 * For regular xmax: use the epoch-reconstructed XID from Phase 3.
	 */
	if (interp.xmax_interp == EPOCH_INTERP_MULTIXACT)
		xmax_xid = effective_xmax;
	else
		xmax_xid = XidFromFullTransactionId(interp.full_xmax);

	/* Test-only: record which internal path was taken */
	if (interp.xmin_interp == EPOCH_INTERP_MATERIALIZED)
		epoch_mvcc_last_path = use_64bit_snapshot ? 's' : 'm';
	else
		epoch_mvcc_last_path = 'd';

	/*
	 * MVCC visibility logic with complete CID checks.
	 * Uses epoch-reconstructed xmin_xid and xmax_xid from above.
	 */

	/* --- xmin checks --- */

	if (xmin_status == EPOCH_XID_FROZEN)
		goto check_xmax;

	if (xmin_status == EPOCH_XID_ABORTED)
		return EPOCH_MVCC_INVISIBLE;

	if (xmin_status == EPOCH_XID_IN_PROGRESS)
	{
		if (TransactionIdIsCurrentTransactionId(xmin_xid))
		{
			if (HeapTupleHeaderGetCmin(tuple) >= snapshot->curcid)
				return EPOCH_MVCC_INVISIBLE;	/* inserted after scan started */
			goto check_xmax;
		}
		return EPOCH_MVCC_INVISIBLE;	/* other xact's uncommitted insert */
	}

	/* xmin committed: snapshot check uses 64-bit bridge when available */
	Assert(xmin_status == EPOCH_XID_COMMITTED);

	if (use_64bit_snapshot
		? FullXidInMVCCSnapshot(interp.full_xmin, snapshot)
		: XidInMVCCSnapshot(xmin_xid, snapshot))
		return EPOCH_MVCC_INVISIBLE;	/* inserter committed after snapshot */

check_xmax:

	if (xmax_status == EPOCH_XID_INVALID_UNSET ||
		xmax_status == EPOCH_XID_ABORTED ||
		xmax_status == EPOCH_XID_MULTIXACT_LOCKERS_ONLY)
		return EPOCH_MVCC_VISIBLE;

	if (xmax_status == EPOCH_XID_IN_PROGRESS)
	{
		if (TransactionIdIsCurrentTransactionId(xmax_xid))
		{
			if (HeapTupleHeaderGetCmax(tuple) >= snapshot->curcid)
				return EPOCH_MVCC_VISIBLE;	/* deleted after scan started */
			return EPOCH_MVCC_INVISIBLE;	/* deleted before scan started */
		}
		return EPOCH_MVCC_VISIBLE;	/* other xact's uncommitted delete */
	}

	/* xmax committed: snapshot check uses 64-bit bridge when available */
	Assert(xmax_status == EPOCH_XID_COMMITTED);

	{
		bool		xmax_in_snapshot;

		if (use_64bit_snapshot)
		{
			FullTransactionId full_xmax_for_snap;

			if (interp.xmax_interp == EPOCH_INTERP_MULTIXACT)
				full_xmax_for_snap = EpochFullXidRelativeTo(
					snapshot->epoch_anchor, effective_xmax);
			else
				full_xmax_for_snap = interp.full_xmax;

			xmax_in_snapshot = FullXidInMVCCSnapshot(full_xmax_for_snap,
													 snapshot);
		}
		else
		{
			xmax_in_snapshot = XidInMVCCSnapshot(xmax_xid, snapshot);
		}

		if (xmax_in_snapshot)
			return EPOCH_MVCC_VISIBLE;	/* deleter committed after snapshot */
	}

	return EPOCH_MVCC_INVISIBLE;	/* deleter committed, visible in snapshot */
}


/* ----------------------------------------------------------------
 *	SQL-callable debug/inspection functions
 *
 *	These are for development and testing of the epoch fork prototype.
 *	They clearly distinguish materialized epoch data from implicit
 *	default-epoch reconstruction.
 * ----------------------------------------------------------------
 */
#include "funcapi.h"
#include "access/heapam.h"
#include "access/table.h"
#include "utils/builtins.h"

/*
 * epoch_xid_inspect(regclass, int4) -> SETOF record
 *
 * Returns epoch slot data for all used slots on the given page.
 * Returns 0 rows if the epoch fork does not exist (implicit mode)
 * or the requested page does not exist in the epoch fork.
 */
PG_FUNCTION_INFO_V1(epoch_xid_inspect);

Datum
epoch_xid_inspect(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int32		blkno = PG_GETARG_INT32(1);
	FuncCallContext *funcctx;
	EpochSlotData *slots;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldctx;
		TupleDesc	tupdesc;
		Relation	rel;
		Buffer		buf;
		Page		page;
		int			num_slots = 0;

		funcctx = SRF_FIRSTCALL_INIT();
		oldctx = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		rel = table_open(relid, AccessShareLock);

		/* Check if relation is in materialized epoch mode */
		if (!EpochRelationIsMaterialized(rel))
		{
			table_close(rel, AccessShareLock);
			funcctx->max_calls = 0;
			MemoryContextSwitchTo(oldctx);
			SRF_RETURN_DONE(funcctx);
		}

		buf = EpochReadBufferReadOnly(rel, (BlockNumber) blkno);

		if (!BufferIsValid(buf))
		{
			table_close(rel, AccessShareLock);
			funcctx->max_calls = 0;
			MemoryContextSwitchTo(oldctx);
			SRF_RETURN_DONE(funcctx);
		}

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);

		slots = NULL;
		if (!PageIsNew(page))
		{
			EpochPageOpaque opaque;

			if (!EpochPageValidate(page))
			{
				UnlockReleaseBuffer(buf);
				table_close(rel, AccessShareLock);
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("epoch page %u has invalid structure",
								(unsigned) blkno)));
			}

			opaque = EpochPageGetOpaque(page);

			num_slots = opaque->num_slots;
			if (num_slots > 0)
			{
				EpochSlotData *page_slots = EpochPageGetSlots(page);

				slots = (EpochSlotData *)
					palloc(num_slots * SizeOfEpochSlotData);
				memcpy(slots, page_slots,
					   num_slots * SizeOfEpochSlotData);
			}
			else
				slots = NULL;
		}

		UnlockReleaseBuffer(buf);
		table_close(rel, AccessShareLock);

		funcctx->max_calls = num_slots;
		funcctx->user_fctx = slots;
		MemoryContextSwitchTo(oldctx);
	}

	funcctx = SRF_PERCALL_SETUP();
	slots = (EpochSlotData *) funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		Datum		values[5];
		bool		nulls[5] = {false, false, false, false, false};
		HeapTuple	tuple;
		int			idx = funcctx->call_cntr;

		values[0] = Int32GetDatum(idx + 1);
		values[1] = Int64GetDatum((int64) slots[idx].xmin_epoch);
		values[2] = Int64GetDatum((int64) slots[idx].xmax_epoch);
		values[3] = Int32GetDatum((int32) slots[idx].epoch_flags);
		values[4] = BoolGetDatum(true);		/* always materialized here */

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * epoch_xid_relation_mode(regclass) -> text
 *
 * Returns 'materialized' or 'implicit' depending on whether the
 * relation has an epoch fork.
 */
PG_FUNCTION_INFO_V1(epoch_xid_relation_mode);

Datum
epoch_xid_relation_mode(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;
	bool		materialized;

	rel = table_open(relid, AccessShareLock);
	materialized = EpochRelationIsMaterialized(rel);
	table_close(rel, AccessShareLock);

	if (materialized)
		PG_RETURN_TEXT_P(cstring_to_text("materialized"));
	else
		PG_RETURN_TEXT_P(cstring_to_text("implicit"));
}

/*
 * epoch_xid_tuple_visibility_info(regclass, tid) RETURNS TABLE(...)
 *
 * Phase 3 read-side inspection function.  Returns a structured
 * interpretation of one tuple's full xid state with explicit mode labels.
 *
 * This is a narrow debug/inspection function.  It does NOT change query
 * semantics and does NOT integrate with executor-level visibility.
 *
 * Raises ERROR if the target TID is not LP_NORMAL.
 */
PG_FUNCTION_INFO_V1(epoch_xid_tuple_visibility_info);

Datum
epoch_xid_tuple_visibility_info(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	ItemPointer tid = (ItemPointer) PG_GETARG_POINTER(1);
	Relation	rel;
	Buffer		heapbuf;
	Page		heappage;
	ItemId		lp;
	HeapTupleHeader htup;
	BlockNumber blkno;
	OffsetNumber offnum;
	bool		is_materialized;
	EpochSlotData *slot = NULL;
	Buffer		epochbuf = InvalidBuffer;
	EpochTupleInterpResult interp;
	TupleDesc	tupdesc;
	Datum		values[5];
	bool		nulls[5] = {false, false, false, false, false};
	HeapTuple	result_tuple;

	blkno = ItemPointerGetBlockNumber(tid);
	offnum = ItemPointerGetOffsetNumber(tid);

	rel = table_open(relid, AccessShareLock);

	/* Read heap buffer */
	heapbuf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno,
								 RBM_NORMAL, NULL);
	LockBuffer(heapbuf, BUFFER_LOCK_SHARE);
	heappage = BufferGetPage(heapbuf);

	/* Validate offset is in range */
	if (offnum < 1 || offnum > PageGetMaxOffsetNumber(heappage))
	{
		UnlockReleaseBuffer(heapbuf);
		table_close(rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("offset number %u is out of range for page %u",
						offnum, blkno)));
	}

	/* Check line pointer state — must be LP_NORMAL */
	lp = PageGetItemId(heappage, offnum);
	if (!ItemIdIsNormal(lp))
	{
		const char *lp_state;

		if (ItemIdIsDead(lp))
			lp_state = "LP_DEAD";
		else if (ItemIdIsRedirected(lp))
			lp_state = "LP_REDIRECT";
		else
			lp_state = "LP_UNUSED";

		UnlockReleaseBuffer(heapbuf);
		table_close(rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot inspect tuple at (%u,%u): line pointer is %s",
						blkno, offnum, lp_state)));
	}

	htup = (HeapTupleHeader) PageGetItem(heappage, lp);

	/* Determine relation epoch mode */
	is_materialized = EpochRelationIsMaterialized(rel);

	/* If materialized, read epoch slot (read-only: no create/extend/dirty) */
	if (is_materialized)
	{
		epochbuf = EpochReadBufferReadOnly(rel, blkno);
		if (BufferIsValid(epochbuf))
		{
			Page		epochpage;

			LockBuffer(epochbuf, BUFFER_LOCK_SHARE);
			epochpage = BufferGetPage(epochbuf);

			if (!PageIsNew(epochpage))
			{
				EpochPageOpaque opaque;

				if (!EpochPageValidate(epochpage))
				{
					UnlockReleaseBuffer(epochbuf);
					UnlockReleaseBuffer(heapbuf);
					table_close(rel, AccessShareLock);
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("epoch page %u has invalid structure",
									blkno)));
				}

				opaque = EpochPageGetOpaque(epochpage);
				if (offnum <= opaque->num_slots)
					slot = EpochGetSlot(epochpage, offnum);
				/* else: offnum beyond high-water mark, slot stays NULL */
			}
			/* else: PageIsNew, slot stays NULL (legal absence per contract) */
		}
		/* else: epoch block beyond EOF, slot NULL (legal absence) */
	}

	/* Interpret */
	interp = EpochInterpretTuple(htup, slot, is_materialized);

	/* Release buffers */
	if (BufferIsValid(epochbuf))
		UnlockReleaseBuffer(epochbuf);
	UnlockReleaseBuffer(heapbuf);
	table_close(rel, AccessShareLock);

	/* Build result tuple */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	values[0] = Int64GetDatum((int64) U64FromFullTransactionId(interp.full_xmin));
	values[1] = Int64GetDatum((int64) U64FromFullTransactionId(interp.full_xmax));
	values[2] = CStringGetTextDatum(EpochInterpModeString(interp.xmin_interp));
	values[3] = CStringGetTextDatum(EpochInterpModeString(interp.xmax_interp));
	values[4] = CStringGetTextDatum(is_materialized ? "materialized" : "implicit");

	result_tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(result_tuple));
}

/*
 * epoch_xid_tuple_txn_state_info(regclass, tid) RETURNS TABLE(...)
 *
 * Phase 4: Combines Phase 3 full-xid interpretation with transaction-status
 * classification (hint bits + CLOG) to produce a tuple-state verdict.
 *
 * This is a narrow inspection function.  It does NOT set hint bits,
 * does NOT consult snapshots, and does NOT change query semantics.
 */
PG_FUNCTION_INFO_V1(epoch_xid_tuple_txn_state_info);

Datum
epoch_xid_tuple_txn_state_info(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	ItemPointer tid = (ItemPointer) PG_GETARG_POINTER(1);
	Relation	rel;
	Buffer		heapbuf;
	Page		heappage;
	ItemId		lp;
	HeapTupleHeader htup;
	BlockNumber blkno;
	OffsetNumber offnum;
	bool		is_materialized;
	EpochSlotData *slot = NULL;
	Buffer		epochbuf = InvalidBuffer;
	EpochTupleInterpResult interp;
	EpochXidStatus xmin_status;
	EpochXidStatus xmax_status;
	EpochTupleState tuple_state;
	TransactionId xmin_xid, xmax_xid;
	TupleDesc	tupdesc;
	Datum		values[6];
	bool		nulls[6] = {false, false, false, false, false, false};
	HeapTuple	result_tuple;

	blkno = ItemPointerGetBlockNumber(tid);
	offnum = ItemPointerGetOffsetNumber(tid);

	rel = table_open(relid, AccessShareLock);
	heapbuf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, NULL);
	LockBuffer(heapbuf, BUFFER_LOCK_SHARE);
	heappage = BufferGetPage(heapbuf);

	if (offnum < 1 || offnum > PageGetMaxOffsetNumber(heappage))
	{
		UnlockReleaseBuffer(heapbuf);
		table_close(rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("offset number %u is out of range for page %u",
						offnum, blkno)));
	}

	lp = PageGetItemId(heappage, offnum);
	if (!ItemIdIsNormal(lp))
	{
		const char *lp_state;

		if (ItemIdIsDead(lp))
			lp_state = "LP_DEAD";
		else if (ItemIdIsRedirected(lp))
			lp_state = "LP_REDIRECT";
		else
			lp_state = "LP_UNUSED";

		UnlockReleaseBuffer(heapbuf);
		table_close(rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot inspect tuple at (%u,%u): line pointer is %s",
						blkno, offnum, lp_state)));
	}

	htup = (HeapTupleHeader) PageGetItem(heappage, lp);

	/* Phase 3 interpretation (read-only: no create/extend/dirty) */
	is_materialized = EpochRelationIsMaterialized(rel);
	if (is_materialized)
	{
		epochbuf = EpochReadBufferReadOnly(rel, blkno);
		if (BufferIsValid(epochbuf))
		{
			Page epochpage;

			LockBuffer(epochbuf, BUFFER_LOCK_SHARE);
			epochpage = BufferGetPage(epochbuf);
			if (!PageIsNew(epochpage))
			{
				EpochPageOpaque opaque;

				if (!EpochPageValidate(epochpage))
				{
					UnlockReleaseBuffer(epochbuf);
					UnlockReleaseBuffer(heapbuf);
					table_close(rel, AccessShareLock);
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("epoch page %u has invalid structure",
									blkno)));
				}

				opaque = EpochPageGetOpaque(epochpage);
				if (offnum <= opaque->num_slots)
					slot = EpochGetSlot(epochpage, offnum);
			}
		}
	}

	interp = EpochInterpretTuple(htup, slot, is_materialized);

	/* Phase 4: classify transaction status */
	xmin_xid = HeapTupleHeaderGetRawXmin(htup);
	xmax_xid = HeapTupleHeaderGetRawXmax(htup);
	xmin_status = EpochClassifyXidStatus(xmin_xid, InvalidFullTransactionId, InvalidFullTransactionId, htup, true, NULL);
	xmax_status = EpochClassifyXidStatus(xmax_xid, InvalidFullTransactionId, InvalidFullTransactionId, htup, false, NULL);
	tuple_state = EpochDeriveTupleState(xmin_status, xmax_status);

	if (BufferIsValid(epochbuf))
		UnlockReleaseBuffer(epochbuf);
	UnlockReleaseBuffer(heapbuf);
	table_close(rel, AccessShareLock);

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	values[0] = Int64GetDatum((int64) U64FromFullTransactionId(interp.full_xmin));
	values[1] = Int64GetDatum((int64) U64FromFullTransactionId(interp.full_xmax));
	values[2] = CStringGetTextDatum(EpochXidStatusString(xmin_status));
	values[3] = CStringGetTextDatum(EpochXidStatusString(xmax_status));
	values[4] = CStringGetTextDatum(EpochTupleStateString(tuple_state));
	values[5] = CStringGetTextDatum(is_materialized ? "materialized" : "implicit");

	result_tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(result_tuple));
}

/*
 * epoch_xid_tuple_current_visibility_info(regclass, tid) RETURNS TABLE(...)
 *
 * Phase 5: Snapshot-relative visibility verdict using GetActiveSnapshot()
 * + XidInMVCCSnapshot().  This is a narrow inspection/debug function.
 */
PG_FUNCTION_INFO_V1(epoch_xid_tuple_current_visibility_info);

Datum
epoch_xid_tuple_current_visibility_info(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	ItemPointer tid = (ItemPointer) PG_GETARG_POINTER(1);
	Relation	rel;
	Buffer		heapbuf;
	Page		heappage;
	ItemId		lp;
	HeapTupleHeader htup;
	BlockNumber blkno;
	OffsetNumber offnum;
	bool		is_materialized;
	EpochSlotData *slot = NULL;
	Buffer		epochbuf = InvalidBuffer;
	EpochTupleInterpResult interp;
	EpochXidStatus xmin_status, xmax_status;
	TransactionId xmin_xid, xmax_xid;
	EpochVisibilityVerdict vis_verdict;
	EpochVerdictReason vis_reason;
	TupleDesc	tupdesc;
	Datum		values[7];
	bool		nulls[7] = {false, false, false, false, false, false, false};
	HeapTuple	result_tuple;

	blkno = ItemPointerGetBlockNumber(tid);
	offnum = ItemPointerGetOffsetNumber(tid);

	rel = table_open(relid, AccessShareLock);
	heapbuf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, NULL);
	LockBuffer(heapbuf, BUFFER_LOCK_SHARE);
	heappage = BufferGetPage(heapbuf);

	if (offnum < 1 || offnum > PageGetMaxOffsetNumber(heappage))
	{
		UnlockReleaseBuffer(heapbuf);
		table_close(rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("offset number %u is out of range for page %u",
						offnum, blkno)));
	}

	lp = PageGetItemId(heappage, offnum);
	if (!ItemIdIsNormal(lp))
	{
		const char *lp_state;
		if (ItemIdIsDead(lp))			lp_state = "LP_DEAD";
		else if (ItemIdIsRedirected(lp)) lp_state = "LP_REDIRECT";
		else							lp_state = "LP_UNUSED";

		UnlockReleaseBuffer(heapbuf);
		table_close(rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot inspect tuple at (%u,%u): line pointer is %s",
						blkno, offnum, lp_state)));
	}

	htup = (HeapTupleHeader) PageGetItem(heappage, lp);

	/* Phase 3: interpretation (read-only: no create/extend/dirty) */
	is_materialized = EpochRelationIsMaterialized(rel);
	if (is_materialized)
	{
		epochbuf = EpochReadBufferReadOnly(rel, blkno);
		if (BufferIsValid(epochbuf))
		{
			Page epochpage;
			LockBuffer(epochbuf, BUFFER_LOCK_SHARE);
			epochpage = BufferGetPage(epochbuf);
			if (!PageIsNew(epochpage))
			{
				EpochPageOpaque opaque;

				if (!EpochPageValidate(epochpage))
				{
					UnlockReleaseBuffer(epochbuf);
					UnlockReleaseBuffer(heapbuf);
					table_close(rel, AccessShareLock);
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("epoch page %u has invalid structure",
									blkno)));
				}

				opaque = EpochPageGetOpaque(epochpage);
				if (offnum <= opaque->num_slots)
					slot = EpochGetSlot(epochpage, offnum);
			}
		}
	}
	interp = EpochInterpretTuple(htup, slot, is_materialized);

	/* Phase 4: status (effective_xmax captures updater XID for MultiXact) */
	xmin_xid = HeapTupleHeaderGetRawXmin(htup);
	xmax_xid = HeapTupleHeaderGetRawXmax(htup);
	{
		TransactionId effective_xmax;

		xmin_status = EpochClassifyXidStatus(xmin_xid, InvalidFullTransactionId, InvalidFullTransactionId, htup, true, NULL);
		xmax_status = EpochClassifyXidStatus(xmax_xid, InvalidFullTransactionId, InvalidFullTransactionId, htup, false, &effective_xmax);

		/* Phase 5: snapshot-relative visibility using effective xmax */
		EpochDeriveVisibility(htup, xmin_xid, effective_xmax,
							  xmin_status, xmax_status, GetActiveSnapshot(),
							  &vis_verdict, &vis_reason);
	}

	if (BufferIsValid(epochbuf))
		UnlockReleaseBuffer(epochbuf);
	UnlockReleaseBuffer(heapbuf);
	table_close(rel, AccessShareLock);

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	values[0] = Int64GetDatum((int64) U64FromFullTransactionId(interp.full_xmin));
	values[1] = Int64GetDatum((int64) U64FromFullTransactionId(interp.full_xmax));
	values[2] = CStringGetTextDatum(EpochXidStatusString(xmin_status));
	values[3] = CStringGetTextDatum(EpochXidStatusString(xmax_status));
	values[4] = CStringGetTextDatum(EpochVisibilityVerdictString(vis_verdict));
	values[5] = CStringGetTextDatum(EpochVerdictReasonString(vis_reason));
	values[6] = CStringGetTextDatum(is_materialized ? "materialized" : "implicit");

	result_tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(result_tuple));
}


/* ----------------------------------------------------------------
 *	Phase 6: Storage contract inspection / test helpers
 * ----------------------------------------------------------------
 */

/*
 * epoch_xid_page_state(regclass, int4) -> text
 *
 * Returns the storage-contract state of an epoch block:
 *   'fork_absent'    - relation has no epoch fork (implicit mode)
 *   'beyond_eof'     - epoch fork exists but block is past EOF
 *   'page_new'       - block is within EOF, page is PageIsNew / all-zero
 *   'valid'          - block is within EOF, page passes structural validation
 *   'corrupt'        - block is within EOF, page is non-new but fails validation
 *
 * This is a read-only inspection function (Phase 6).  It never creates,
 * extends, initializes, or dirties the epoch fork.
 */
PG_FUNCTION_INFO_V1(epoch_xid_page_state);

Datum
epoch_xid_page_state(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int32		blkno = PG_GETARG_INT32(1);
	Relation	rel;
	Buffer		buf;
	const char *state;

	rel = table_open(relid, AccessShareLock);

	if (!EpochRelationIsMaterialized(rel))
	{
		table_close(rel, AccessShareLock);
		PG_RETURN_TEXT_P(cstring_to_text("fork_absent"));
	}

	buf = EpochReadBufferReadOnly(rel, (BlockNumber) blkno);

	if (!BufferIsValid(buf))
	{
		table_close(rel, AccessShareLock);
		PG_RETURN_TEXT_P(cstring_to_text("beyond_eof"));
	}

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	{
		Page		page = BufferGetPage(buf);

		if (PageIsNew(page))
			state = "page_new";
		else if (EpochPageValidate(page))
			state = "valid";
		else
			state = "corrupt";
	}
	UnlockReleaseBuffer(buf);

	table_close(rel, AccessShareLock);
	PG_RETURN_TEXT_P(cstring_to_text(state));
}

/*
 * epoch_xid_corrupt_page(regclass, int4) -> void
 *
 * TEST-ONLY function.  Writes an invalid epoch_version to the specified
 * epoch block to produce a corrupt page state for Phase 6 corruption
 * boundary testing.
 *
 * The page must already exist and be a valid initialized epoch page.
 * After this call, the page will fail EpochPageValidate().
 */
PG_FUNCTION_INFO_V1(epoch_xid_corrupt_page);

Datum
epoch_xid_corrupt_page(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int32		blkno = PG_GETARG_INT32(1);
	Relation	rel;
	Buffer		buf;
	Page		page;
	EpochPageOpaque opaque;

	rel = table_open(relid, RowExclusiveLock);

	if (!EpochRelationIsMaterialized(rel))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("relation is not in materialized epoch mode")));

	buf = EpochReadBufferReadOnly(rel, (BlockNumber) blkno);
	if (!BufferIsValid(buf))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("epoch block %d does not exist", blkno)));

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);

	if (PageIsNew(page))
	{
		UnlockReleaseBuffer(buf);
		table_close(rel, RowExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("epoch block %d is PageIsNew, cannot corrupt", blkno)));
	}

	/* Write an invalid version to make the page corrupt */
	opaque = EpochPageGetOpaque(page);
	opaque->epoch_version = 0xDEAD;

	MarkBufferDirty(buf);
	UnlockReleaseBuffer(buf);

	table_close(rel, RowExclusiveLock);
	PG_RETURN_VOID();
}

/*
 * epoch_xid_reset_page(regclass, int4) -> void
 *
 * TEST-ONLY function.  Zeros out the specified epoch block, resetting it
 * to the PageIsNew / all-zero state.  This creates the "within-EOF but
 * PageIsNew" condition that the Phase 6 storage contract treats as a
 * legal absence state.
 *
 * The block must already exist within the epoch fork's EOF.
 * After this call, the page will pass PageIsNew() and read paths must
 * fall back rather than error.
 */
PG_FUNCTION_INFO_V1(epoch_xid_reset_page);

Datum
epoch_xid_reset_page(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int32		blkno = PG_GETARG_INT32(1);
	Relation	rel;
	Buffer		buf;
	Page		page;

	rel = table_open(relid, RowExclusiveLock);

	if (!EpochRelationIsMaterialized(rel))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("relation is not in materialized epoch mode")));

	buf = EpochReadBufferReadOnly(rel, (BlockNumber) blkno);
	if (!BufferIsValid(buf))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("epoch block %d does not exist", blkno)));

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);

	/* Zero out the entire page → PageIsNew state */
	MemSet(page, 0, BLCKSZ);

	MarkBufferDirty(buf);
	UnlockReleaseBuffer(buf);

	table_close(rel, RowExclusiveLock);
	PG_RETURN_VOID();
}

/*
 * epoch_xid_set_num_slots(regclass, int4, int4) -> void
 *
 * TEST-ONLY function.  Artificially sets num_slots (the high-water mark)
 * on a valid initialized epoch page.  This creates the condition where
 * heap tuples exist at offsets beyond num_slots, exercising the
 * slot-beyond-HWM fallback contract.
 */
PG_FUNCTION_INFO_V1(epoch_xid_set_num_slots);

Datum
epoch_xid_set_num_slots(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int32		blkno = PG_GETARG_INT32(1);
	int32		new_num_slots = PG_GETARG_INT32(2);
	Relation	rel;
	Buffer		buf;
	Page		page;
	EpochPageOpaque opaque;

	rel = table_open(relid, RowExclusiveLock);

	if (!EpochRelationIsMaterialized(rel))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("relation is not in materialized epoch mode")));

	buf = EpochReadBufferReadOnly(rel, (BlockNumber) blkno);
	if (!BufferIsValid(buf))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("epoch block %d does not exist", blkno)));

	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);

	if (PageIsNew(page))
	{
		UnlockReleaseBuffer(buf);
		table_close(rel, RowExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("epoch block %d is PageIsNew", blkno)));
	}

	opaque = EpochPageGetOpaque(page);
	opaque->num_slots = new_num_slots;

	MarkBufferDirty(buf);
	UnlockReleaseBuffer(buf);

	table_close(rel, RowExclusiveLock);
	PG_RETURN_VOID();
}


/* ----------------------------------------------------------------
 *	Patch 7: Segment-aware sparse-file introspection (test-only)
 * ----------------------------------------------------------------
 */

/*
 * epoch_xid_block_status(regclass, int4) -> text
 *
 * TEST-ONLY function.  Reports the physical allocation status of a
 * logical epoch block by mapping it to the correct segment file and
 * in-segment offset, then using SEEK_HOLE / SEEK_DATA.
 *
 * Logical block N maps to:
 *   segment file  = relpath + "." + (N / RELSEG_SIZE), or base for seg 0
 *   in-seg offset = (N % RELSEG_SIZE) * BLCKSZ
 *
 * Returns:
 *   'hole'        - block within EOF, is a sparse hole (SEEK_HOLE)
 *   'data'        - block within EOF, contains allocated data
 *   'beyond_eof'  - block past the segment file size, or segment absent
 *   'fork_absent' - epoch fork does not exist
 *   'unsupported' - SEEK_HOLE/SEEK_DATA not available
 *
 * Platform assumptions:
 *   Linux:  SEEK_HOLE/SEEK_DATA available since kernel 3.1 (2011).
 *           Works on ext4, XFS, btrfs, tmpfs.
 *   macOS:  SEEK_HOLE/SEEK_DATA available since 10.4 (2005).
 *           Works on APFS and HFS+.
 *   If a filesystem does not support SEEK_HOLE (returns ENXIO or
 *   treats the entire file as data), this function returns 'data'
 *   for all within-EOF blocks.  That is correct: the semantics still
 *   hold (PageIsNew = legal absence), only the sparse optimization
 *   is not active.
 */
PG_FUNCTION_INFO_V1(epoch_xid_block_status);

Datum
epoch_xid_block_status(PG_FUNCTION_ARGS)
{
#if defined(SEEK_HOLE) && defined(SEEK_DATA)
	Oid			relid = PG_GETARG_OID(0);
	int32		blkno = PG_GETARG_INT32(1);
	Relation	rel;
	SMgrRelation smgr;
	BlockNumber	segno;
	BlockNumber	seg_offset;
	char		segpath[EPOCH_SEG_PATH_MAXLEN];
	int			fd;
	off_t		file_size;
	off_t		byte_offset;
	off_t		hole_start;
	const char *status;

	rel = table_open(relid, AccessShareLock);
	smgr = RelationGetSmgr(rel);

	if (!epoch_fork_exists_on_disk(smgr))
	{
		table_close(rel, AccessShareLock);
		PG_RETURN_TEXT_P(cstring_to_text("fork_absent"));
	}

	segno = (BlockNumber) blkno / ((BlockNumber) RELSEG_SIZE);
	seg_offset = (BlockNumber) blkno % ((BlockNumber) RELSEG_SIZE);

	epoch_seg_path(segpath, sizeof(segpath), smgr, segno);
	table_close(rel, AccessShareLock);

	fd = BasicOpenFile(segpath, O_RDONLY | PG_BINARY);
	if (fd < 0)
		PG_RETURN_TEXT_P(cstring_to_text("beyond_eof"));

	file_size = lseek(fd, 0, SEEK_END);
	byte_offset = (off_t) seg_offset * BLCKSZ;

	if (byte_offset >= file_size)
	{
		close(fd);
		PG_RETURN_TEXT_P(cstring_to_text("beyond_eof"));
	}

	hole_start = lseek(fd, byte_offset, SEEK_HOLE);

	if (hole_start < 0)
	{
		close(fd);
		PG_RETURN_TEXT_P(cstring_to_text("unsupported"));
	}

	if (hole_start <= byte_offset)
		status = "hole";
	else
		status = "data";

	close(fd);
	PG_RETURN_TEXT_P(cstring_to_text(status));
#else
	PG_RETURN_TEXT_P(cstring_to_text("unsupported"));
#endif
}

/*
 * epoch_xid_block_segment_info(regclass, int4) -> text
 *
 * TEST-ONLY function.  Exposes the logical-block -> physical-segment
 * mapping for the epoch fork.  Returns a text string:
 *   "seg=<N> offset=<M> path=<path>"
 *
 * Validates that the segment-aware helpers compute the correct
 * mapping, especially for blocks beyond the first segment boundary.
 */
PG_FUNCTION_INFO_V1(epoch_xid_block_segment_info);

Datum
epoch_xid_block_segment_info(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int32		blkno = PG_GETARG_INT32(1);
	Relation	rel;
	SMgrRelation smgr;
	BlockNumber	segno;
	BlockNumber	seg_offset;
	char		segpath[EPOCH_SEG_PATH_MAXLEN];
	char		result[EPOCH_SEG_PATH_MAXLEN + 64];

	rel = table_open(relid, AccessShareLock);
	smgr = RelationGetSmgr(rel);

	segno = (BlockNumber) blkno / ((BlockNumber) RELSEG_SIZE);
	seg_offset = (BlockNumber) blkno % ((BlockNumber) RELSEG_SIZE);

	epoch_seg_path(segpath, sizeof(segpath), smgr, segno);
	table_close(rel, AccessShareLock);

	snprintf(result, sizeof(result), "seg=%u offset=%u path=%s",
			 segno, seg_offset, segpath);

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

/*
 * epoch_xid_force_materialize_block(regclass, int4) -> void
 *
 * TEST-ONLY function.  Directly materializes a specific logical epoch
 * block using the real sparse extension code path (EpochEnsureFork +
 * epoch_fork_extend_sparse + buffer init).  This allows testing
 * cross-segment sparse materialization without needing a heap that
 * actually spans >1GB.
 *
 * After this call:
 *   - the epoch fork exists and covers logical block 'blkno'
 *   - the target block is initialized with epoch page metadata
 *   - all intermediate blocks are sparse holes (ftruncate-backed)
 *   - segment files are created as needed for cross-segment blocks
 */
PG_FUNCTION_INFO_V1(epoch_xid_force_materialize_block);

Datum
epoch_xid_force_materialize_block(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int32		blkno = PG_GETARG_INT32(1);
	Relation	rel;
	SMgrRelation smgr;
	Buffer		buf;
	Page		page;

	rel = table_open(relid, RowExclusiveLock);

	EpochEnsureFork(rel);

	smgr = RelationGetSmgr(rel);

	LockRelationForExtension(rel, ExclusiveLock);

	epoch_fork_extend_sparse(smgr, (BlockNumber) blkno + 1);

	buf = ReadBufferExtended(rel, EPOCH_FORKNUM, (BlockNumber) blkno,
							 RBM_ZERO_AND_LOCK, NULL);
	page = BufferGetPage(buf);
	EpochPageInit(page);
	MarkBufferDirty(buf);
	UnlockReleaseBuffer(buf);

	UnlockRelationForExtension(rel, ExclusiveLock);

	table_close(rel, RowExclusiveLock);
	PG_RETURN_VOID();
}

/*
 * epoch_xid_sparse_platform_check() -> text
 *
 * TEST-ONLY function.  Validates that SEEK_HOLE/SEEK_DATA is
 * functional on the current platform by creating a temporary file,
 * extending it via ftruncate, and probing with SEEK_HOLE.
 *
 * Returns:
 *   'sparse_supported' - SEEK_HOLE detects holes created by ftruncate
 *   'no_seek_hole'     - SEEK_HOLE/SEEK_DATA not available at compile time
 *   'seek_hole_broken' - SEEK_HOLE available but does not detect holes
 *
 * This function is intended for Linux and macOS, where SEEK_HOLE
 * must detect ftruncate-created holes for the sparse-file tests to
 * be meaningful.  If this returns anything other than
 * 'sparse_supported' on Linux or macOS, the filesystem may not
 * support sparse files (e.g., some NFS or virtualized mounts).
 */
PG_FUNCTION_INFO_V1(epoch_xid_sparse_platform_check);

Datum
epoch_xid_sparse_platform_check(PG_FUNCTION_ARGS)
{
#if defined(SEEK_HOLE) && defined(SEEK_DATA)
	char		tmppath[MAXPGPATH];
	int			fd;
	off_t		hole_pos;

	snprintf(tmppath, sizeof(tmppath), "%s/epoch_sparse_check.tmp",
			 DataDir);

	fd = BasicOpenFile(tmppath, O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);
	if (fd < 0)
		PG_RETURN_TEXT_P(cstring_to_text("seek_hole_broken"));

	/* Extend to 2 blocks via ftruncate — should create a hole */
	if (ftruncate(fd, 2 * BLCKSZ) < 0)
	{
		close(fd);
		unlink(tmppath);
		PG_RETURN_TEXT_P(cstring_to_text("seek_hole_broken"));
	}

	/* SEEK_HOLE from offset 0: if the file is sparse, returns 0 */
	hole_pos = lseek(fd, 0, SEEK_HOLE);
	close(fd);
	unlink(tmppath);

	if (hole_pos == 0)
		PG_RETURN_TEXT_P(cstring_to_text("sparse_supported"));
	else
		PG_RETURN_TEXT_P(cstring_to_text("seek_hole_broken"));
#else
	PG_RETURN_TEXT_P(cstring_to_text("no_seek_hole"));
#endif
}


/*
 * epoch_xid_resolve_multixact(regclass, tid) -> bool
 *
 * TEST-ONLY function.  Forces resolution of a committed-updater MultiXact
 * on a specific tuple: clears HEAP_XMAX_IS_MULTI and rewrites xmax to the
 * updater's TransactionId with HEAP_XMAX_COMMITTED.
 *
 * Returns true if resolution occurred, false if the tuple was not a
 * committed-updater MultiXact (nothing changed).
 *
 * This exists because PostgreSQL does not resolve committed-updater
 * MultiXacts via simple hint-bit setting — resolution only happens
 * through VACUUM freeze (which also prunes) or new locking operations
 * (which expand into a new Multi).  Neither path leaves the old tuple
 * accessible with a resolved non-Multi xmax for inspection.
 */
PG_FUNCTION_INFO_V1(epoch_xid_resolve_multixact);

Datum
epoch_xid_resolve_multixact(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	ItemPointer tid = (ItemPointer) PG_GETARG_POINTER(1);
	Relation	rel;
	Buffer		buf;
	Page		page;
	ItemId		lp;
	HeapTupleHeader htup;
	BlockNumber blkno;
	OffsetNumber offnum;
	TransactionId updater_xid;
	MultiXactMember *members;
	int			nmembers;
	int			i;
	bool		resolved = false;

	blkno = ItemPointerGetBlockNumber(tid);
	offnum = ItemPointerGetOffsetNumber(tid);

	rel = table_open(relid, RowExclusiveLock);
	buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, NULL);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buf);

	if (offnum < 1 || offnum > PageGetMaxOffsetNumber(page))
	{
		UnlockReleaseBuffer(buf);
		table_close(rel, RowExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("offset %u out of range for page %u", offnum, blkno)));
	}

	lp = PageGetItemId(page, offnum);
	if (!ItemIdIsNormal(lp))
	{
		UnlockReleaseBuffer(buf);
		table_close(rel, RowExclusiveLock);
		PG_RETURN_BOOL(false);
	}

	htup = (HeapTupleHeader) PageGetItem(page, lp);

	if (!(htup->t_infomask & HEAP_XMAX_IS_MULTI) ||
		HEAP_XMAX_IS_LOCKED_ONLY(htup->t_infomask))
	{
		UnlockReleaseBuffer(buf);
		table_close(rel, RowExclusiveLock);
		PG_RETURN_BOOL(false);
	}

	/* Find the committed updater */
	nmembers = GetMultiXactIdMembers(HeapTupleHeaderGetRawXmax(htup),
									 &members, false, false);
	updater_xid = InvalidTransactionId;

	if (nmembers > 0)
	{
		for (i = 0; i < nmembers; i++)
		{
			if (ISUPDATE_from_mxstatus(members[i].status))
			{
				updater_xid = members[i].xid;
				break;
			}
		}
		pfree(members);
	}

	if (TransactionIdIsValid(updater_xid) &&
		TransactionIdDidCommit(updater_xid))
	{
		htup->t_infomask &= ~HEAP_XMAX_BITS;
		htup->t_infomask |= HEAP_XMAX_COMMITTED;
		HeapTupleHeaderSetXmax(htup, updater_xid);
		MarkBufferDirty(buf);
		resolved = true;
	}

	UnlockReleaseBuffer(buf);
	table_close(rel, RowExclusiveLock);
	PG_RETURN_BOOL(resolved);
}


/*
 * epoch_xid_mvcc_last_path() -> text
 *
 * TEST-ONLY function.  Returns the internal path taken by the last call to
 * EpochHeapTupleSatisfiesMVCC from heap_fetch's epoch branch.
 *
 * Returns:
 *   'materialized'     - epoch branch used slot-backed data, 32-bit snapshot
 *   'snapshot_bridge'  - epoch branch used slot-backed data + 64-bit snapshot bridge
 *   'default_fallback' - epoch branch executed but slot was absent (PageIsNew etc.)
 *   'cannot_determine' - epoch branch returned CANNOT_DETERMINE
 *   'not_called'       - EpochHeapTupleSatisfiesMVCC has not been called yet
 */
PG_FUNCTION_INFO_V1(epoch_xid_mvcc_last_path);

Datum
epoch_xid_mvcc_last_path(PG_FUNCTION_ARGS)
{
	const char *result;

	switch (epoch_mvcc_last_path)
	{
		case 'm':
			result = "materialized";
			break;
		case 's':
			result = "snapshot_bridge";
			break;
		case 'd':
			result = "default_fallback";
			break;
		case 'u':
			result = "cannot_determine";
			break;
		default:
			result = "not_called";
			break;
	}

	PG_RETURN_TEXT_P(cstring_to_text(result));
}


/*
 * epoch_xid_classify_last_membership() -> text
 *
 * TEST-ONLY function.  Returns which active-transaction membership function
 * was last used by EpochClassifyXidStatus during Phase 4 classification.
 *
 * Returns:
 *   '64bit_membership' - FullTransactionIdIsInProgress was used (Stage 1 guard passed)
 *   '32bit_membership' - TransactionIdIsInProgress was used (guard off or no full xid)
 *   'not_called'       - EpochClassifyXidStatus has not reached the membership check yet
 *
 * This makes the Patch 12 64-bit membership improvement observable in tests.
 */
PG_FUNCTION_INFO_V1(epoch_xid_classify_last_membership);

Datum
epoch_xid_classify_last_membership(PG_FUNCTION_ARGS)
{
	const char *result;

	switch (epoch_classify_last_membership)
	{
		case '6':
			result = "64bit_membership";
			break;
		case '3':
			result = "32bit_membership";
			break;
		default:
			result = "not_called";
			break;
	}

	PG_RETURN_TEXT_P(cstring_to_text(result));
}


/*
 * epoch_xid_classify_last_horizon() -> text
 *
 * TEST-ONLY function.  Returns whether the 64-bit snapshot horizon fast-reject
 * (Patch 14) was taken by the last EpochClassifyXidStatus call.
 *
 * Returns:
 *   'horizon_fastpath' - fast-reject fired: full_xid < epoch_horizon,
 *                        ProcArray membership scan was skipped
 *   'not_used'         - fast-reject did not fire (XID >= horizon, or
 *                        epoch_horizon invalid, or resolved by hint bits /
 *                        own-xid check before reaching the horizon test)
 *
 * This complements epoch_xid_classify_last_membership() (Patch 13) and
 * epoch_xid_mvcc_last_path() (Patch 12), providing complete observability
 * of the epoch visibility pipeline's fast paths.
 */
PG_FUNCTION_INFO_V1(epoch_xid_classify_last_horizon);

Datum
epoch_xid_classify_last_horizon(PG_FUNCTION_ARGS)
{
	const char *result;

	switch (epoch_classify_last_horizon)
	{
		case 'h':
			result = "horizon_fastpath";
			break;
		default:
			result = "not_used";
			break;
	}

	PG_RETURN_TEXT_P(cstring_to_text(result));
}


/*
 * epoch_xid_stage1_bridge_enabled() -> bool
 *
 * TEST-ONLY function.  Returns true if the Stage 1 snapshot bridge guard
 * would pass for the current active snapshot: epoch_anchor is valid AND
 * the anchor's epoch is 0 (pre-wrap).
 *
 * Returns false if no active snapshot, epoch_anchor is invalid, or
 * epoch_anchor has epoch > 0 (system crossed into epoch 1+).
 *
 * This makes the Stage 1 operational boundary observable in tests.
 */
PG_FUNCTION_INFO_V1(epoch_xid_stage1_bridge_enabled);

Datum
epoch_xid_stage1_bridge_enabled(PG_FUNCTION_ARGS)
{
	Snapshot	snapshot = GetActiveSnapshot();
	bool		enabled;

	if (snapshot == NULL)
		PG_RETURN_BOOL(false);

	enabled = !epoch_stage1_force_disabled &&
		FullTransactionIdIsValid(snapshot->epoch_anchor) &&
		(EpochFromFullTransactionId(snapshot->epoch_anchor) == 0);

	PG_RETURN_BOOL(enabled);
}


/*
 * epoch_xid_stage1_force_disable(bool) -> void
 *
 * TEST-ONLY function.  Sets or clears the test-only override that forces
 * the Stage 1 bridge guard to false, simulating a post-epoch-0 state.
 * When set to true, the bridge is disabled even though the real epoch
 * anchor is valid and in epoch 0.  Pass false to re-enable.
 */
PG_FUNCTION_INFO_V1(epoch_xid_stage1_force_disable);

Datum
epoch_xid_stage1_force_disable(PG_FUNCTION_ARGS)
{
	epoch_stage1_force_disabled = PG_GETARG_BOOL(0);
	PG_RETURN_VOID();
}
