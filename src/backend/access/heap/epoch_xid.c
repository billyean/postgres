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

#include "access/epoch_xid.h"
#include "access/htup_details.h"
#include "access/transam.h"
#include "catalog/storage_xlog.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"


/* ----------------------------------------------------------------
 *	Fork and page management
 * ----------------------------------------------------------------
 */

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
	if (!smgrexists(RelationGetSmgr(rel), EPOCH_FORKNUM))
	{
		LockRelationForExtension(rel, ExclusiveLock);

		/* Re-check after acquiring lock (another backend may have created it) */
		if (!smgrexists(RelationGetSmgr(rel), EPOCH_FORKNUM))
		{
			smgrcreate(RelationGetSmgr(rel), EPOCH_FORKNUM, false);

			if (RelationNeedsWAL(rel))
				log_smgrcreate(&RelationGetSmgr(rel)->smgr_rlocator.locator,
							   EPOCH_FORKNUM);
		}

		UnlockRelationForExtension(rel, ExclusiveLock);
	}
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

		tupdesc = CreateTemplateTupleDesc(5);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "offnum",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "xmin_epoch",
						   INT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "xmax_epoch",
						   INT8OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "epoch_flags",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 5, "is_materialized",
						   BOOLOID, -1, 0);
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

		buf = EpochReadBuffer(rel, (BlockNumber) blkno, false);

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
			EpochPageOpaque opaque = EpochPageGetOpaque(page);

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

	/* If materialized, read epoch slot */
	if (is_materialized)
	{
		epochbuf = EpochReadBuffer(rel, blkno, false);
		if (BufferIsValid(epochbuf))
		{
			Page		epochpage;
			EpochPageOpaque opaque;

			LockBuffer(epochbuf, BUFFER_LOCK_SHARE);
			epochpage = BufferGetPage(epochbuf);

			if (!PageIsNew(epochpage))
			{
				opaque = EpochPageGetOpaque(epochpage);
				if (offnum <= opaque->num_slots)
					slot = EpochGetSlot(epochpage, offnum);
				/* else: offnum beyond high-water mark, slot stays NULL */
			}
			/* else: PageIsNew, slot stays NULL (per-slot-absence rule) */
		}
		/* else: epoch block doesn't exist yet for this heap block, slot NULL */
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
