/*-------------------------------------------------------------------------
 *
 * test_shared_plan_cache_serialization.c
 *	  Test wrappers for shared plan cache serialization.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_shared_plan_cache_serialization/test_shared_plan_cache_serialization.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/prepare.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodes.h"
#include "nodes/plannodes.h"
#include "nodes/readfuncs.h"
#include "portability/instr_time.h"
#include "utils/builtins.h"
#include "utils/dsa.h"
#include "utils/memutils.h"
#include "utils/plancache.h"
#include "utils/shared_plancache.h"
#include "utils/tuplestore.h"

PG_MODULE_MAGIC;

static const char *
serialize_status_to_string(SharedPlanSerializeStatus status)
{
	switch (status)
	{
		case SHARED_PLAN_SERIALIZE_OK:
			return "OK";
		case SHARED_PLAN_SERIALIZE_NOT_SHAREABLE:
			return "NOT_SHAREABLE";
		case SHARED_PLAN_SERIALIZE_OVERSIZE:
			return "OVERSIZE";
		case SHARED_PLAN_SERIALIZE_OOM:
			return "OOM";
		case SHARED_PLAN_SERIALIZE_INVALID_INPUT:
			return "INVALID_INPUT";
	}
	return "UNKNOWN";
}

static const char *
reject_reason_to_string(SharedPlanRejectReason reason)
{
	switch (reason)
	{
		case SHARED_PLAN_REJECT_NONE:
			return "NONE";
		case SHARED_PLAN_REJECT_NOT_GENERIC:
			return "NOT_GENERIC";
		case SHARED_PLAN_REJECT_INCOMPLETE:
			return "INCOMPLETE";
		case SHARED_PLAN_REJECT_ONESHOT:
			return "ONESHOT";
		case SHARED_PLAN_REJECT_POST_REWRITE_HOOK:
			return "POST_REWRITE_HOOK";
		case SHARED_PLAN_REJECT_PLANNER_HOOK:
			return "PLANNER_HOOK";
		case SHARED_PLAN_REJECT_DEPENDS_ON_RLS:
			return "DEPENDS_ON_RLS";
		case SHARED_PLAN_REJECT_DEPENDS_ON_ROLE:
			return "DEPENDS_ON_ROLE";
		case SHARED_PLAN_REJECT_TEMP_OBJECT:
			return "TEMP_OBJECT";
		case SHARED_PLAN_REJECT_SAVED_XMIN:
			return "SAVED_XMIN";
		case SHARED_PLAN_REJECT_CUSTOM_SCAN:
			return "CUSTOM_SCAN";
		case SHARED_PLAN_REJECT_FOREIGN_SCAN:
			return "FOREIGN_SCAN";
		case SHARED_PLAN_REJECT_EXTENSION_STATE:
			return "EXTENSION_STATE";
		case SHARED_PLAN_REJECT_OVERSIZE:
			return "OVERSIZE";
		case SHARED_PLAN_REJECT_UNKNOWN_UNSHAREABLE:
			return "UNKNOWN_UNSHAREABLE";
	}
	return "UNKNOWN";
}

#define BIG_LIMIT ((Size) 16 * 1024 * 1024)

PG_FUNCTION_INFO_V1(test_shared_plan_roundtrip);
Datum
test_shared_plan_roundtrip(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	PreparedStatement *pstmt;
	CachedPlanSource *plansource;
	CachedPlan *plan = NULL;
	char	   *orig_text = NULL;
	char	   *serialized = NULL;
	MemoryContext tmpctx = NULL;
	bool		match = false;

	pstmt = FetchPreparedStatement(stmt_name, true);
	plansource = pstmt->plansource;
	plan = GetCachedPlan(plansource, NULL, NULL, NULL);

	PG_TRY();
	{
		Size		serialized_len;
		SharedPlanSerializeStatus status;

		orig_text = nodeToString(plan->stmt_list);
		status = SharedPlanSerializeStmtList(plan->stmt_list, BIG_LIMIT,
											 &serialized, &serialized_len);
		if (status == SHARED_PLAN_SERIALIZE_OK)
		{
			List	   *deserialized = NIL;

			tmpctx = AllocSetContextCreate(CurrentMemoryContext,
										   "roundtrip test",
										   ALLOCSET_DEFAULT_SIZES);
			status = SharedPlanDeserialize(serialized, serialized_len,
										   tmpctx, &deserialized);
			if (status == SHARED_PLAN_SERIALIZE_OK)
			{
				char	   *round_text = nodeToString(deserialized);

				match = strcmp(orig_text, round_text) == 0;
				pfree(round_text);
			}
		}
	}
	PG_FINALLY();
	{
		if (tmpctx != NULL)
			MemoryContextDelete(tmpctx);
		if (serialized != NULL)
			pfree(serialized);
		if (orig_text != NULL)
			pfree(orig_text);
		ReleaseCachedPlan(plan, NULL);
	}
	PG_END_TRY();

	PG_RETURN_BOOL(match);
}

PG_FUNCTION_INFO_V1(test_shared_plan_serialized_size);
Datum
test_shared_plan_serialized_size(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	PreparedStatement *pstmt;
	CachedPlanSource *plansource;
	CachedPlan *plan = NULL;
	char	   *serialized = NULL;
	int64		result_size = -1;

	pstmt = FetchPreparedStatement(stmt_name, true);
	plansource = pstmt->plansource;
	plan = GetCachedPlan(plansource, NULL, NULL, NULL);

	PG_TRY();
	{
		Size		serialized_len;

		if (SharedPlanSerializeStmtList(plan->stmt_list, BIG_LIMIT,
										&serialized, &serialized_len)
			== SHARED_PLAN_SERIALIZE_OK)
			result_size = (int64) serialized_len;
	}
	PG_FINALLY();
	{
		if (serialized != NULL)
			pfree(serialized);
		ReleaseCachedPlan(plan, NULL);
	}
	PG_END_TRY();

	PG_RETURN_INT64(result_size);
}

PG_FUNCTION_INFO_V1(test_shared_plan_serialize_oversize);
Datum
test_shared_plan_serialize_oversize(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			max_kb = PG_GETARG_INT32(1);
	PreparedStatement *pstmt;
	CachedPlanSource *plansource;
	CachedPlan *plan = NULL;
	char	   *serialized = NULL;
	SharedPlanSerializeStatus status = SHARED_PLAN_SERIALIZE_INVALID_INPUT;

	if (max_kb <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("max_kb must be positive")));

	pstmt = FetchPreparedStatement(stmt_name, true);
	plansource = pstmt->plansource;
	plan = GetCachedPlan(plansource, NULL, NULL, NULL);

	PG_TRY();
	{
		Size		serialized_len;

		status = SharedPlanSerializeStmtList(plan->stmt_list,
											 (Size) max_kb * 1024,
											 &serialized, &serialized_len);
	}
	PG_FINALLY();
	{
		if (serialized != NULL)
			pfree(serialized);
		ReleaseCachedPlan(plan, NULL);
	}
	PG_END_TRY();

	PG_RETURN_TEXT_P(cstring_to_text(serialize_status_to_string(status)));
}

PG_FUNCTION_INFO_V1(test_shared_plan_serialize_cached_plan);
Datum
test_shared_plan_serialize_cached_plan(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	PreparedStatement *pstmt;
	CachedPlanSource *plansource;
	CachedPlan *plan = NULL;
	char	   *serialized = NULL;
	SharedPlanSerializeStatus status = SHARED_PLAN_SERIALIZE_INVALID_INPUT;
	SharedPlanRejectReason reason = SHARED_PLAN_REJECT_NONE;
	char		result_buf[128];

	pstmt = FetchPreparedStatement(stmt_name, true);
	plansource = pstmt->plansource;
	plan = GetCachedPlan(plansource, NULL, NULL, NULL);

	PG_TRY();
	{
		Size		serialized_len;

		status = SharedPlanSerializeCachedPlan(plansource, plan, true,
											   BIG_LIMIT,
											   &serialized, &serialized_len,
											   &reason);
	}
	PG_FINALLY();
	{
		if (serialized != NULL)
			pfree(serialized);
		ReleaseCachedPlan(plan, NULL);
	}
	PG_END_TRY();

	if (status == SHARED_PLAN_SERIALIZE_NOT_SHAREABLE)
		snprintf(result_buf, sizeof(result_buf), "NOT_SHAREABLE:%s",
				 reject_reason_to_string(reason));
	else
		snprintf(result_buf, sizeof(result_buf), "%s",
				 serialize_status_to_string(status));

	PG_RETURN_TEXT_P(cstring_to_text(result_buf));
}

PG_FUNCTION_INFO_V1(test_shared_plan_roundtrip_dsa);
Datum
test_shared_plan_roundtrip_dsa(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	PreparedStatement *pstmt;
	CachedPlanSource *plansource;
	CachedPlan *plan = NULL;
	dsa_area   *test_dsa = NULL;
	dsa_pointer dp = InvalidDsaPointer;
	char	   *orig_text = NULL;
	MemoryContext tmpctx = NULL;
	bool		match = false;

	pstmt = FetchPreparedStatement(stmt_name, true);
	plansource = pstmt->plansource;
	plan = GetCachedPlan(plansource, NULL, NULL, NULL);

	PG_TRY();
	{
		Size		dp_len = 0;
		SharedPlanSerializeStatus status;
		int			tranche_id;

		orig_text = nodeToString(plan->stmt_list);

		tranche_id = LWLockNewTrancheId("test_ser_dsa");
		test_dsa = dsa_create(tranche_id);
		dsa_pin(test_dsa);

		status = SharedPlanSerializeToDSA(plan->stmt_list, BIG_LIMIT,
										  test_dsa, &dp, &dp_len);
		if (status == SHARED_PLAN_SERIALIZE_OK)
		{
			List	   *deserialized = NIL;

			tmpctx = AllocSetContextCreate(CurrentMemoryContext,
										   "dsa roundtrip",
										   ALLOCSET_DEFAULT_SIZES);
			status = SharedPlanDeserializeFromDSA(test_dsa, dp, dp_len,
												  tmpctx, &deserialized);
			if (status == SHARED_PLAN_SERIALIZE_OK)
			{
				char	   *round_text = nodeToString(deserialized);

				match = strcmp(orig_text, round_text) == 0;
				pfree(round_text);
			}
		}
	}
	PG_FINALLY();
	{
		if (tmpctx != NULL)
			MemoryContextDelete(tmpctx);
		if (orig_text != NULL)
			pfree(orig_text);
		if (test_dsa != NULL)
		{
			if (DsaPointerIsValid(dp))
				dsa_free(test_dsa, dp);
			dsa_unpin(test_dsa);
			dsa_detach(test_dsa);
		}
		ReleaseCachedPlan(plan, NULL);
	}
	PG_END_TRY();

	PG_RETURN_BOOL(match);
}

PG_FUNCTION_INFO_V1(test_shared_plan_deserialize_corrupt);
Datum
test_shared_plan_deserialize_corrupt(PG_FUNCTION_ARGS)
{
	const char *test_case = text_to_cstring(PG_GETARG_TEXT_PP(0));
	SharedPlanSerializeStatus status = SHARED_PLAN_SERIALIZE_OK;
	List	   *deser_result = NIL;
	MemoryContext tmpctx = NULL;
	bool		parse_error = false;

	tmpctx = AllocSetContextCreate(CurrentMemoryContext,
								   "corrupt test",
								   ALLOCSET_DEFAULT_SIZES);

	PG_TRY();
	{
		if (strcmp(test_case, "too_short") == 0)
		{
			char		buf[4] = {0};

			status = SharedPlanDeserialize(buf, 4, tmpctx, &deser_result);
		}
		else if (strcmp(test_case, "wrong_magic") == 0)
		{
			SharedPlanSerializedHeader hdr;

			memset(&hdr, 0, sizeof(hdr));
			hdr.magic = 0xDEADBEEF;
			hdr.version = SHARED_PLAN_SERIAL_VERSION;
			hdr.format = SHARED_PLAN_FORMAT_TEXT;
			status = SharedPlanDeserialize((char *) &hdr, sizeof(hdr),
										   tmpctx, &deser_result);
		}
		else if (strcmp(test_case, "wrong_version") == 0)
		{
			SharedPlanSerializedHeader hdr;

			memset(&hdr, 0, sizeof(hdr));
			hdr.magic = SHARED_PLAN_SERIAL_MAGIC;
			hdr.version = 999;
			hdr.format = SHARED_PLAN_FORMAT_TEXT;
			status = SharedPlanDeserialize((char *) &hdr, sizeof(hdr),
										   tmpctx, &deser_result);
		}
		else if (strcmp(test_case, "wrong_format") == 0)
		{
			SharedPlanSerializedHeader hdr;

			memset(&hdr, 0, sizeof(hdr));
			hdr.magic = SHARED_PLAN_SERIAL_MAGIC;
			hdr.version = SHARED_PLAN_SERIAL_VERSION;
			hdr.format = 99;
			status = SharedPlanDeserialize((char *) &hdr, sizeof(hdr),
										   tmpctx, &deser_result);
		}
		else if (strcmp(test_case, "payload_len_mismatch") == 0)
		{
			SharedPlanSerializedHeader hdr;

			memset(&hdr, 0, sizeof(hdr));
			hdr.magic = SHARED_PLAN_SERIAL_MAGIC;
			hdr.version = SHARED_PLAN_SERIAL_VERSION;
			hdr.format = SHARED_PLAN_FORMAT_TEXT;
			hdr.payload_len = 999;
			status = SharedPlanDeserialize((char *) &hdr, sizeof(hdr),
										   tmpctx, &deser_result);
		}
		else if (strcmp(test_case, "num_stmts_mismatch") == 0)
		{
			char	   *serialized;
			Size		slen;
			List	   *dummy_list;

			dummy_list = list_make1(makeNode(PlannedStmt));
			if (SharedPlanSerializeStmtList(dummy_list, BIG_LIMIT,
											&serialized, &slen)
				== SHARED_PLAN_SERIALIZE_OK)
			{
				((SharedPlanSerializedHeader *) serialized)->num_stmts = 999;
				status = SharedPlanDeserialize(serialized, slen, tmpctx,
											   &deser_result);
				pfree(serialized);
			}
			list_free_deep(dummy_list);
		}
		else if (strcmp(test_case, "non_list_payload") == 0)
		{
			char	   *text;
			Size		text_len,
						total_len;
			char	   *buf;
			SharedPlanSerializedHeader *hdr_p;
			Result	   *fake = makeNode(Result);

			text = nodeToString(fake);
			text_len = strlen(text);
			total_len = sizeof(SharedPlanSerializedHeader) + text_len;
			buf = palloc(total_len);
			hdr_p = (SharedPlanSerializedHeader *) buf;
			hdr_p->magic = SHARED_PLAN_SERIAL_MAGIC;
			hdr_p->version = SHARED_PLAN_SERIAL_VERSION;
			hdr_p->format = SHARED_PLAN_FORMAT_TEXT;
			hdr_p->payload_len = (uint32) text_len;
			hdr_p->num_stmts = 1;
			memcpy(buf + sizeof(SharedPlanSerializedHeader), text, text_len);
			pfree(text);
			status = SharedPlanDeserialize(buf, total_len, tmpctx,
										   &deser_result);
			pfree(buf);
			pfree(fake);
		}
		else if (strcmp(test_case, "list_non_plannedstmt") == 0)
		{
			char	   *text;
			Size		text_len,
						total_len;
			char	   *buf;
			SharedPlanSerializedHeader *hdr_p;
			List	   *bad_list;
			Result	   *fake = makeNode(Result);

			bad_list = list_make1(fake);
			text = nodeToString(bad_list);
			text_len = strlen(text);
			total_len = sizeof(SharedPlanSerializedHeader) + text_len;
			buf = palloc(total_len);
			hdr_p = (SharedPlanSerializedHeader *) buf;
			hdr_p->magic = SHARED_PLAN_SERIAL_MAGIC;
			hdr_p->version = SHARED_PLAN_SERIAL_VERSION;
			hdr_p->format = SHARED_PLAN_FORMAT_TEXT;
			hdr_p->payload_len = (uint32) text_len;
			hdr_p->num_stmts = 1;
			memcpy(buf + sizeof(SharedPlanSerializedHeader), text, text_len);
			pfree(text);
			status = SharedPlanDeserialize(buf, total_len, tmpctx,
										   &deser_result);
			pfree(buf);
			pfree(fake);
			list_free(bad_list);
		}
		else if (strcmp(test_case, "unparseable_text") == 0)
		{
			const char *garbage = "{{GARBAGE NODE}}";
			Size		text_len = strlen(garbage);
			Size		total_len = sizeof(SharedPlanSerializedHeader) + text_len;
			char	   *buf = palloc(total_len);
			SharedPlanSerializedHeader *hdr_p;

			hdr_p = (SharedPlanSerializedHeader *) buf;
			hdr_p->magic = SHARED_PLAN_SERIAL_MAGIC;
			hdr_p->version = SHARED_PLAN_SERIAL_VERSION;
			hdr_p->format = SHARED_PLAN_FORMAT_TEXT;
			hdr_p->payload_len = (uint32) text_len;
			hdr_p->num_stmts = 0;
			memcpy(buf + sizeof(SharedPlanSerializedHeader), garbage, text_len);

			PG_TRY();
			{
				status = SharedPlanDeserialize(buf, total_len, tmpctx,
											   &deser_result);
			}
			PG_CATCH();
			{
				ErrorData  *edata = CopyErrorData();

				/*
				 * Only convert expected parser/readfuncs failures to
				 * PARSE_ERROR.  Rethrow OOM and other serious errors.
				 */
				if (edata->sqlerrcode == ERRCODE_OUT_OF_MEMORY ||
					edata->elevel >= FATAL)
				{
					FreeErrorData(edata);
					PG_RE_THROW();
				}
				FreeErrorData(edata);
				FlushErrorState();
				parse_error = true;
			}
			PG_END_TRY();
			pfree(buf);
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unknown test case: %s", test_case)));
		}
	}
	PG_FINALLY();
	{
		if (tmpctx != NULL)
			MemoryContextDelete(tmpctx);
	}
	PG_END_TRY();

	if (parse_error)
		PG_RETURN_TEXT_P(cstring_to_text("PARSE_ERROR"));

	PG_RETURN_TEXT_P(cstring_to_text(serialize_status_to_string(status)));
}

PG_FUNCTION_INFO_V1(test_shared_plan_serialize_timing);
Datum
test_shared_plan_serialize_timing(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			iterations = PG_GETARG_INT32(1);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	PreparedStatement *pstmt;
	CachedPlanSource *plansource;
	CachedPlan *plan = NULL;
	double		ser_total_us = 0;
	double		deser_total_us = 0;
	int64		serialized_bytes = 0;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};

	if (iterations <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("iterations must be positive")));

	InitMaterializedSRF(fcinfo, 0);

	pstmt = FetchPreparedStatement(stmt_name, true);
	plansource = pstmt->plansource;
	plan = GetCachedPlan(plansource, NULL, NULL, NULL);

	PG_TRY();
	{
		int			i;

		for (i = 0; i < iterations; i++)
		{
			char	   *serialized = NULL;
			Size		slen = 0;
			MemoryContext iter_ctx = NULL;
			SharedPlanSerializeStatus sstatus;
			instr_time	start,
						end;

			PG_TRY();
			{
				INSTR_TIME_SET_CURRENT(start);
				sstatus = SharedPlanSerializeStmtList(plan->stmt_list, BIG_LIMIT,
													  &serialized, &slen);
				INSTR_TIME_SET_CURRENT(end);
				INSTR_TIME_SUBTRACT(end, start);
				ser_total_us += INSTR_TIME_GET_MICROSEC(end);

				if (sstatus == SHARED_PLAN_SERIALIZE_OK)
				{
					List	   *deserialized = NIL;

					serialized_bytes = (int64) slen;
					iter_ctx = AllocSetContextCreate(CurrentMemoryContext,
													 "timing deser",
													 ALLOCSET_DEFAULT_SIZES);
					INSTR_TIME_SET_CURRENT(start);
					SharedPlanDeserialize(serialized, slen, iter_ctx,
										  &deserialized);
					INSTR_TIME_SET_CURRENT(end);
					INSTR_TIME_SUBTRACT(end, start);
					deser_total_us += INSTR_TIME_GET_MICROSEC(end);
				}
			}
			PG_FINALLY();
			{
				if (iter_ctx != NULL)
					MemoryContextDelete(iter_ctx);
				if (serialized != NULL)
					pfree(serialized);
			}
			PG_END_TRY();
		}
	}
	PG_FINALLY();
	{
		ReleaseCachedPlan(plan, NULL);
	}
	PG_END_TRY();

	values[0] = Float8GetDatum(ser_total_us / iterations);
	values[1] = Float8GetDatum(deser_total_us / iterations);
	values[2] = Int64GetDatum(serialized_bytes);

	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

	PG_RETURN_NULL();
}
