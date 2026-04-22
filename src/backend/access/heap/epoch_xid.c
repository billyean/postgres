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

#include "access/epoch_xid.h"
#include "access/htup_details.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/storage_xlog.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/snapmgr.h"


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
	nblocks = smgrnblocks(smgr, EPOCH_FORKNUM);

	buf = InvalidBuffer;

	while (nblocks <= heapBlk)
	{
		Buffer		newbuf;
		Page		page;

		newbuf = ReadBufferExtended(rel, EPOCH_FORKNUM, P_NEW,
									RBM_ZERO_AND_LOCK, NULL);
		page = BufferGetPage(newbuf);
		EpochPageInit(page);
		MarkBufferDirty(newbuf);

		if (nblocks == heapBlk)
		{
			LockBuffer(newbuf, BUFFER_LOCK_UNLOCK);
			buf = newbuf;
		}
		else
		{
			UnlockReleaseBuffer(newbuf);
		}

		nblocks++;
	}

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
		case EPOCH_INTERP_MULTIXACT_UNSUPPORTED:
			return "multixact_unsupported";
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
		/* Read path: report unsupported, do not ERROR */
		result.full_xmax = InvalidFullTransactionId;
		result.xmax_interp = EPOCH_INTERP_MULTIXACT_UNSUPPORTED;
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
	}
	return "unknown";
}

/*
 * EpochClassifyXidStatus -- classify transaction state of one xid.
 * Uses hint bits first, then CLOG lookup.  Read-only: does not set hint bits.
 */
static EpochXidStatus
EpochClassifyXidStatus(TransactionId xid, HeapTupleHeader htup, bool is_xmin)
{
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
			return EPOCH_XID_MULTIXACT_UNSUPPORTED;
		if (htup->t_infomask & HEAP_XMAX_COMMITTED)
			return EPOCH_XID_COMMITTED;
	}

	if (!TransactionIdIsValid(xid))
		return EPOCH_XID_INVALID_UNSET;
	if (TransactionIdIsCurrentTransactionId(xid))
		return EPOCH_XID_IN_PROGRESS;
	if (TransactionIdIsInProgress(xid))
		return EPOCH_XID_IN_PROGRESS;
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
		case EPOCH_REASON_NO_SNAPSHOT:				return "no_snapshot";
	}
	return "unknown";
}

/*
 * EpochDeriveVisibility -- snapshot-relative visibility verdict.
 * Uses GetActiveSnapshot() + XidInMVCCSnapshot().
 */
static void
EpochDeriveVisibility(HeapTupleHeader htup,
					  TransactionId xmin_xid, TransactionId xmax_xid,
					  EpochXidStatus xmin_status, EpochXidStatus xmax_status,
					  EpochVisibilityVerdict *verdict,
					  EpochVerdictReason *reason)
{
	Snapshot	snapshot = GetActiveSnapshot();

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
				xmax_status == EPOCH_XID_ABORTED)
			{
				*verdict = EPOCH_VIS_VISIBLE;
				*reason = EPOCH_REASON_OWN_INSERT_VISIBLE;
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
		xmax_status == EPOCH_XID_ABORTED)
	{
		*verdict = EPOCH_VIS_VISIBLE;
		*reason = EPOCH_REASON_XMIN_COMMITTED_VISIBLE;
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
	xmin_status = EpochClassifyXidStatus(xmin_xid, htup, true);
	xmax_status = EpochClassifyXidStatus(xmax_xid, htup, false);
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

	/* Phase 4: status */
	xmin_xid = HeapTupleHeaderGetRawXmin(htup);
	xmax_xid = HeapTupleHeaderGetRawXmax(htup);
	xmin_status = EpochClassifyXidStatus(xmin_xid, htup, true);
	xmax_status = EpochClassifyXidStatus(xmax_xid, htup, false);

	/* Phase 5: snapshot-relative visibility */
	EpochDeriveVisibility(htup, xmin_xid, xmax_xid,
						  xmin_status, xmax_status,
						  &vis_verdict, &vis_reason);

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
