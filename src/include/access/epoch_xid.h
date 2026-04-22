/*-------------------------------------------------------------------------
 *
 * epoch_xid.h
 *	  XID64 epoch fork: per-tuple-slot epoch metadata for 64-bit XIDs.
 *
 * The epoch fork stores the high 32 bits (epoch) of xmin/xmax for each
 * tuple slot on the corresponding heap page.  Heap tuple headers remain
 * unchanged at 32 bits; the full 64-bit XID is reconstructed on demand.
 *
 * Storage model:
 *   - 1:1 block correspondence: heap block N <-> epoch block N
 *   - Standard PageHeaderData for LSN/checksum/buffer manager
 *   - EpochPageOpaqueData + fixed slot array after the page header
 *   - One EpochSlotData per possible heap tuple slot
 *
 * Two-mode state model:
 *   - Implicit mode (fork absent): reconstruction uses EPOCH_DEFAULT_VALUE
 *   - Materialized mode (fork present): per-slot authoritative state
 *
 * EXPERIMENTAL / v1 research prototype.
 * See DESIGN.xid64_epoch_side_v1.md for the full design specification.
 *
 * v1 supported write surface:
 *   - heap_insert: whole-slot initialization with xmin epoch
 *   - heap_delete: additive xmax epoch update
 * All other DML paths are either not hooked or explicitly blocked.
 * This is a write-side metadata prototype; MVCC visibility is not
 * integrated and continues to use 32-bit XIDs.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/epoch_xid.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EPOCH_XID_H
#define EPOCH_XID_H

#include "access/htup_details.h"
#include "access/transam.h"
#include "common/relpath.h"
#include "storage/buf.h"
#include "storage/bufpage.h"
#include "utils/relcache.h"

/* ---------- Constants ---------- */

/*
 * Epoch fork format version.  Bump if the on-disk layout changes.
 */
#define EPOCH_PAGE_VERSION		1

/*
 * Default epoch value used when epoch metadata is absent.
 * In v1, this is unconditionally 0 (correct for first ~4B XIDs).
 */
#define EPOCH_DEFAULT_VALUE		0

/*
 * Flags for EpochSlotData.epoch_flags.
 *
 * EPOCH_FLAG_XMIN_SET: xmin_epoch was explicitly written by an
 *     epoch-aware DML path.  If unset, xmin_epoch is unknown.
 * EPOCH_FLAG_XMAX_SET: xmax_epoch was explicitly written by an
 *     epoch-aware DML path.  If unset, xmax_epoch is unknown.
 */
#define EPOCH_FLAG_NONE			0x0000
#define EPOCH_FLAG_XMIN_SET		0x0001
#define EPOCH_FLAG_XMAX_SET		0x0002

/* ---------- Data Structures ---------- */

/*
 * Per-slot epoch metadata.
 *
 * One entry per possible tuple slot on the corresponding heap page.
 * Indexed by (OffsetNumber - 1).
 *
 * IMPORTANT: On insert into a slot (including reused slots), the entire
 * struct must be overwritten to prevent stale state from prior occupants.
 * See EpochSlotInitForInsert().
 */
typedef struct EpochSlotData
{
	uint32		xmin_epoch;		/* high 32 bits of xmin */
	uint32		xmax_epoch;		/* high 32 bits of xmax */
	uint16		epoch_flags;	/* EPOCH_FLAG_* */
	uint16		padding;		/* alignment / reserved, must be 0 */
} EpochSlotData;

#define SizeOfEpochSlotData		sizeof(EpochSlotData)

/*
 * Epoch page opaque data, stored immediately after standard PageHeaderData.
 */
typedef struct EpochPageOpaqueData
{
	uint32		epoch_version;	/* EPOCH_PAGE_VERSION */
	uint32		num_slots;		/* high-water mark of used slots */
} EpochPageOpaqueData;

typedef EpochPageOpaqueData *EpochPageOpaque;

#define SizeOfEpochPageOpaque	sizeof(EpochPageOpaqueData)

/* Offset of the slot array from page start */
#define EpochSlotsOffset	(SizeOfPageHeaderData + SizeOfEpochPageOpaque)

/* Maximum slots per epoch page (must be >= MaxHeapTuplesPerPage) */
#define MaxEpochSlotsPerPage \
	((int) ((BLCKSZ - EpochSlotsOffset) / SizeOfEpochSlotData))

StaticAssertDecl(MaxEpochSlotsPerPage >= MaxHeapTuplesPerPage,
				 "epoch page cannot hold enough slots for MaxHeapTuplesPerPage");

/* ---------- Access Macros ---------- */

#define EpochPageGetOpaque(page) \
	((EpochPageOpaque) ((char *)(page) + SizeOfPageHeaderData))

#define EpochPageGetSlots(page) \
	((EpochSlotData *) ((char *)(page) + EpochSlotsOffset))

/* ---------- Mode Detection ---------- */

/*
 * Check whether a relation is in materialized epoch mode.
 *
 * A relation is materialized if and only if its epoch fork file exists.
 * If absent, the relation is in implicit default-epoch mode.
 *
 * Uses direct stat() on the fork's filesystem path instead of
 * smgrexists(), which calls mdclose() + mdopenfork() and can leave
 * md_num_open_segs inconsistent for a fork that does not yet exist,
 * causing assertion failures in a subsequent smgrcreate/mdcreate.
 */
extern bool EpochRelationIsMaterialized(Relation rel);

/* ---------- Public API ---------- */

/* Fork and page management */
extern void EpochEnsureFork(Relation rel);
extern void EpochPageInit(Page page);
extern Buffer EpochReadBuffer(Relation rel, BlockNumber heapBlk,
							  bool extend_ok);
extern void EpochPinBuffer(Relation rel, BlockNumber heapBlk,
						   Buffer *epochbuf);

/*
 * EpochReadBufferReadOnly -- read-only epoch buffer access (Phase 6).
 *
 * Returns InvalidBuffer if:
 *   - Relation is not materialized (fork absent)
 *   - Block is beyond EOF
 * Both are legal absence states per the storage contract.
 *
 * Contract guarantees:
 *   - NEVER creates the fork
 *   - NEVER extends the file
 *   - NEVER initializes, dirties, or WAL-logs any page
 *
 * Caller must treat PageIsNew pages as absent (slot = NULL).
 * Returns a pinned but NOT locked buffer (caller must lock).
 */
extern Buffer EpochReadBufferReadOnly(Relation rel, BlockNumber heapBlk);

/*
 * EpochPageValidate -- structural validation of a non-new epoch page (Phase 6).
 *
 * Returns true if the page has valid epoch structure; false if corrupt.
 * Caller must have already verified !PageIsNew(page).
 * Caller decides whether to ERROR or log a warning on false.
 */
extern bool EpochPageValidate(Page page);

/* Slot access */
extern EpochSlotData *EpochGetSlot(Page epochPage, OffsetNumber offnum);

/*
 * EpochSlotInitForInsert -- whole-slot-reset for heap insert.
 *
 * Writes the ENTIRE slot entry: sets xmin_epoch from fxid, clears
 * xmax_epoch to 0, sets flags to EPOCH_FLAG_XMIN_SET only, clears
 * padding.  This is the ONLY correct way to initialize a slot on
 * insert, because the slot may have been previously occupied by a
 * different tuple whose stale xmax_epoch/flags must not survive.
 *
 * Caller must hold exclusive lock on the epoch buffer.
 */
extern void EpochSlotInitForInsert(Page epochPage, OffsetNumber offnum,
								   FullTransactionId fxid);

/*
 * EpochSlotSetXmax -- set xmax epoch on an already-initialized slot.
 *
 * Adds xmax_epoch and sets EPOCH_FLAG_XMAX_SET.  Does NOT clear xmin.
 * Only valid when the slot is owned by the current tuple (was initialized
 * by the current tuple's insert via EpochSlotInitForInsert).
 *
 * Caller must hold exclusive lock on the epoch buffer.
 */
extern void EpochSlotSetXmax(Page epochPage, OffsetNumber offnum,
							 FullTransactionId fxid);

/* Full XID reconstruction */
extern FullTransactionId EpochReconstructXmin(HeapTupleHeader htup,
											  EpochSlotData *slot);
extern FullTransactionId EpochReconstructXmax(HeapTupleHeader htup,
											  EpochSlotData *slot);

/* ---------- WAL sub-record for epoch slot changes ---------- */

/*
 * xl_epoch_slot_update -- WAL payload for epoch slot modifications.
 *
 * Registered via XLogRegisterBufData on the epoch buffer's block reference.
 * During redo, if no FPI was applied (BLK_NEEDS_REDO), this data is used
 * to replay the epoch slot modification.  If an FPI was applied, the slot
 * state is already correct from the image and this data is not consulted.
 *
 * For insert: is_whole_entry_reset = true.  The entire slot is overwritten
 *     (xmin set, xmax cleared, flags assigned with =).
 * For delete/update-old: is_whole_entry_reset = false.  Only xmax fields
 *     are set additively (flags merged with |=).
 */
typedef struct xl_epoch_slot_update
{
	OffsetNumber offnum;			/* slot index (1-based, matches heap) */
	uint32		xmin_epoch;			/* high 32 bits of xmin */
	uint32		xmax_epoch;			/* high 32 bits of xmax */
	uint16		epoch_flags;		/* EPOCH_FLAG_* bits to set or assign */
	bool		is_whole_entry_reset; /* true = assign; false = additive */
	uint8		padding;
} xl_epoch_slot_update;

#define SizeOfEpochSlotUpdate	sizeof(xl_epoch_slot_update)

/*
 * Apply an xl_epoch_slot_update record to an epoch page during redo.
 * Caller must hold exclusive lock.  Page is NOT marked dirty by this
 * function; caller is responsible.
 */
extern void EpochRedoSlotUpdate(Page epochPage,
								xl_epoch_slot_update *xlrec);

/* ---------- Read-side interpretation (Phase 3) ---------- */

/*
 * Interpretation mode for a single xmin or xmax field.
 * Indicates the source/method used to produce the full xid value.
 */
typedef enum EpochInterpMode
{
	EPOCH_INTERP_MATERIALIZED,			/* from explicit epoch slot data */
	EPOCH_INTERP_IMPLICIT_DEFAULT,		/* from EPOCH_DEFAULT_VALUE (slot absent/unset) */
	EPOCH_INTERP_FROZEN,				/* HEAP_XMIN_FROZEN; epoch irrelevant */
	EPOCH_INTERP_INVALID,				/* TransactionId is invalid (0) */
	EPOCH_INTERP_INVALID_UNSET,			/* xmax: HEAP_XMAX_INVALID is set */
	EPOCH_INTERP_MULTIXACT_UNSUPPORTED	/* xmax: HEAP_XMAX_IS_MULTI; cannot interpret */
} EpochInterpMode;

/*
 * Full interpretation result for one tuple's xmin and xmax.
 */
typedef struct EpochTupleInterpResult
{
	FullTransactionId full_xmin;
	FullTransactionId full_xmax;
	EpochInterpMode xmin_interp;
	EpochInterpMode xmax_interp;
} EpochTupleInterpResult;

/*
 * EpochInterpretTuple -- produce a full xid interpretation for one tuple.
 *
 * htup: the heap tuple header (must be from an LP_NORMAL line pointer)
 * slot: pointer to the epoch slot data, or NULL if:
 *       - relation is in implicit mode (no epoch fork)
 *       - epoch page is uninitialized (PageIsNew)
 *       - offnum exceeds the epoch page's num_slots high-water mark
 * relation_is_materialized: whether the epoch fork exists for the relation
 *
 * This function does NOT raise ERROR for any input state.  It always
 * produces a result, using IMPLICIT_DEFAULT or MULTIXACT_UNSUPPORTED
 * labels where authoritative interpretation is not possible.
 */
extern EpochTupleInterpResult
EpochInterpretTuple(HeapTupleHeader htup, EpochSlotData *slot,
					bool relation_is_materialized);

/* Return the text label for an EpochInterpMode value */
extern const char *EpochInterpModeString(EpochInterpMode mode);

/* ---------- Transaction-state classification (Phase 4) ---------- */

/*
 * EpochXidStatus -- classification of a single xid's transaction state.
 * Produced by consulting hint bits and CLOG.
 */
typedef enum EpochXidStatus
{
	EPOCH_XID_COMMITTED,
	EPOCH_XID_ABORTED,
	EPOCH_XID_IN_PROGRESS,
	EPOCH_XID_FROZEN,
	EPOCH_XID_INVALID_UNSET,
	EPOCH_XID_MULTIXACT_UNSUPPORTED
} EpochXidStatus;

/*
 * EpochTupleState -- derived verdict from xmin_status + xmax_status.
 * Fixed vocabulary with no overlap.
 */
typedef enum EpochTupleState
{
	EPOCH_TUPLE_LIVE_COMMITTED,
	EPOCH_TUPLE_DEAD_COMMITTED,
	EPOCH_TUPLE_INSERTING_IN_PROGRESS,
	EPOCH_TUPLE_DELETING_IN_PROGRESS,
	EPOCH_TUPLE_ABORTED_INSERT,
	EPOCH_TUPLE_FROZEN_LIVE,
	EPOCH_TUPLE_FROZEN_DELETED,
	EPOCH_TUPLE_MULTIXACT_UNCLASSIFIABLE
} EpochTupleState;

extern const char *EpochXidStatusString(EpochXidStatus status);
extern const char *EpochTupleStateString(EpochTupleState state);

/* ---------- Snapshot-relative visibility (Phase 5) ---------- */

typedef enum EpochVisibilityVerdict
{
	EPOCH_VIS_VISIBLE,
	EPOCH_VIS_INVISIBLE,
	EPOCH_VIS_MULTIXACT_UNSUPPORTED,
	EPOCH_VIS_CANNOT_CLASSIFY
} EpochVisibilityVerdict;

typedef enum EpochVerdictReason
{
	EPOCH_REASON_XMIN_COMMITTED_VISIBLE,
	EPOCH_REASON_FROZEN,
	EPOCH_REASON_OWN_INSERT_VISIBLE,
	EPOCH_REASON_XMIN_IN_PROGRESS,
	EPOCH_REASON_XMIN_ABORTED,
	EPOCH_REASON_XMIN_COMMITTED_NOT_IN_SNAPSHOT,
	EPOCH_REASON_XMAX_COMMITTED_VISIBLE_IN_SNAPSHOT,
	EPOCH_REASON_XMAX_COMMITTED_NOT_IN_SNAPSHOT,
	EPOCH_REASON_XMAX_IN_PROGRESS,
	EPOCH_REASON_OWN_DELETE_INVISIBLE,
	EPOCH_REASON_MULTIXACT_UNSUPPORTED,
	EPOCH_REASON_NO_SNAPSHOT
} EpochVerdictReason;

extern const char *EpochVisibilityVerdictString(EpochVisibilityVerdict v);
extern const char *EpochVerdictReasonString(EpochVerdictReason r);

#endif							/* EPOCH_XID_H */
