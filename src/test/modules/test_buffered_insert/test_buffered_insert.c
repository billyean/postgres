/*--------------------------------------------------------------------------
 *
 * test_buffered_insert.c
 *		Minimal validation for the Table AM buffered-insert lifecycle API.
 *
 *		Exercises begin/put/flush/end directly at the Table AM layer on a
 *		real heap relation, without going through any higher-level caller
 *		(CTAS, COPY, etc.).  This keeps the test within Patch 0001 scope.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/test/modules/test_buffered_insert/test_buffered_insert.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/tableam.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;

/*
 * test_buffered_insert_basic(regclass, int4)
 *
 * Opens the given relation, inserts nrows tuples through the buffered-insert
 * API with a NULL flush callback, and returns the number of rows put().
 * The tuples inserted have the form (i) for a single-integer-column table.
 */
PG_FUNCTION_INFO_V1(test_buffered_insert_basic);
Datum
test_buffered_insert_basic(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int32		nrows = PG_GETARG_INT32(1);
	Relation	rel;
	TableBufferedInsertState state;
	TupleTableSlot *slot;
	TupleDesc	tupdesc;
	int			i;

	rel = table_open(relid, RowExclusiveLock);
	tupdesc = RelationGetDescr(rel);

	state = table_buffered_insert_begin(rel,
										GetCurrentCommandId(true),
										TABLE_INSERT_BAS_BULKWRITE,
										NULL, NULL);

	if (state == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("AM does not support buffered inserts")));

	slot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);

	for (i = 0; i < nrows; i++)
	{
		ExecClearTuple(slot);
		slot->tts_values[0] = Int32GetDatum(i + 1);
		slot->tts_isnull[0] = false;
		ExecStoreVirtualTuple(slot);

		table_buffered_insert_put(state, slot);
	}

	table_buffered_insert_end(state);

	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, RowExclusiveLock);

	PG_RETURN_INT32(nrows);
}

/*
 * Flush callback context: counts invocations and verifies each slot has
 * a valid TID.
 */
typedef struct FlushCbContext
{
	int			count;
} FlushCbContext;

static void
test_flush_callback(void *context, TupleTableSlot *slot)
{
	FlushCbContext *ctx = (FlushCbContext *) context;

	if (!ItemPointerIsValid(&slot->tts_tid))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("flush callback received slot with invalid TID")));

	ctx->count++;
}

/*
 * test_buffered_insert_with_callback(regclass, int4)
 *
 * Same as basic, but passes a flush callback that counts invocations and
 * validates TIDs.  Returns the total flush-callback invocation count.
 */
PG_FUNCTION_INFO_V1(test_buffered_insert_with_callback);
Datum
test_buffered_insert_with_callback(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int32		nrows = PG_GETARG_INT32(1);
	Relation	rel;
	TableBufferedInsertState state;
	TupleTableSlot *slot;
	TupleDesc	tupdesc;
	FlushCbContext ctx;
	int			i;

	ctx.count = 0;

	rel = table_open(relid, RowExclusiveLock);
	tupdesc = RelationGetDescr(rel);

	state = table_buffered_insert_begin(rel,
										GetCurrentCommandId(true),
										TABLE_INSERT_BAS_BULKWRITE,
										test_flush_callback,
										&ctx);

	if (state == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("AM does not support buffered inserts")));

	slot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);

	for (i = 0; i < nrows; i++)
	{
		ExecClearTuple(slot);
		slot->tts_values[0] = Int32GetDatum(i + 1);
		slot->tts_isnull[0] = false;
		ExecStoreVirtualTuple(slot);

		table_buffered_insert_put(state, slot);
	}

	table_buffered_insert_end(state);

	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, RowExclusiveLock);

	PG_RETURN_INT32(ctx.count);
}

/*
 * test_buffered_insert_flush_mid(regclass, int4)
 *
 * Exercises explicit flush() mid-session: inserts half the rows, calls
 * flush(), inserts the other half, then calls end().  Returns the total
 * flush-callback count, which should equal nrows.
 */
PG_FUNCTION_INFO_V1(test_buffered_insert_flush_mid);
Datum
test_buffered_insert_flush_mid(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int32		nrows = PG_GETARG_INT32(1);
	Relation	rel;
	TableBufferedInsertState state;
	TupleTableSlot *slot;
	TupleDesc	tupdesc;
	FlushCbContext ctx;
	int			half = nrows / 2;
	int			i;

	ctx.count = 0;

	rel = table_open(relid, RowExclusiveLock);
	tupdesc = RelationGetDescr(rel);

	state = table_buffered_insert_begin(rel,
										GetCurrentCommandId(true),
										TABLE_INSERT_BAS_BULKWRITE,
										test_flush_callback,
										&ctx);

	if (state == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("AM does not support buffered inserts")));

	slot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);

	/* First half */
	for (i = 0; i < half; i++)
	{
		ExecClearTuple(slot);
		slot->tts_values[0] = Int32GetDatum(i + 1);
		slot->tts_isnull[0] = false;
		ExecStoreVirtualTuple(slot);

		table_buffered_insert_put(state, slot);
	}

	/* Explicit flush mid-session */
	table_buffered_insert_flush(state);

	/* Second half */
	for (i = half; i < nrows; i++)
	{
		ExecClearTuple(slot);
		slot->tts_values[0] = Int32GetDatum(i + 1);
		slot->tts_isnull[0] = false;
		ExecStoreVirtualTuple(slot);

		table_buffered_insert_put(state, slot);
	}

	/* end() flushes the remaining tuples */
	table_buffered_insert_end(state);

	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, RowExclusiveLock);

	PG_RETURN_INT32(ctx.count);
}
