/*-------------------------------------------------------------------------
 *
 * test_plan_shareability.c
 *	  SQL wrappers for testing PlanIsShareable().
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_plan_shareability/test_plan_shareability.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/transam.h"
#include "commands/prepare.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "nodes/plannodes.h"
#include "optimizer/planner.h"
#include "utils/builtins.h"
#include "utils/plancache.h"

PG_MODULE_MAGIC;

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

static PlannedStmt *
find_first_plannedstmt(CachedPlan *cplan)
{
	ListCell   *lc;

	foreach(lc, cplan->stmt_list)
	{
		PlannedStmt *ps = lfirst_node(PlannedStmt, lc);

		if (ps->commandType != CMD_UTILITY)
			return ps;
	}
	return NULL;
}

/*
 * test_plan_shareability(stmt_name text) -> text
 *
 * Evaluate PlanIsShareable() on the generic plan of a prepared statement.
 * Returns the rejection reason as text, or 'NONE' if shareable.
 */
PG_FUNCTION_INFO_V1(test_plan_shareability);
Datum
test_plan_shareability(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	PreparedStatement *pstmt;
	CachedPlanSource *plansource;
	CachedPlan *plan;
	SharedPlanRejectReason reason;
	const char *result;

	pstmt = FetchPreparedStatement(stmt_name, true);
	plansource = pstmt->plansource;

	plan = GetCachedPlan(plansource, NULL, NULL, NULL);

	PlanIsShareable(plansource, plan, true, &reason);

	result = reject_reason_to_string(reason);

	ReleaseCachedPlan(plan, NULL);

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

static void
dummy_post_rewrite_hook(List *querytree_list, void *arg)
{
	/* do nothing */
}

static PlannedStmt *
dummy_planner_hook(Query *parse, const char *query_string,
				   int cursorOptions, ParamListInfo boundParams,
				   ExplainState *es)
{
	/* never called; used only to simulate hook-active state */
	return standard_planner(parse, query_string, cursorOptions, boundParams, es);
}

/*
 * test_plan_shareability_force_reject(stmt_name text, reject_reason text)
 *		-> boolean
 *
 * Temporarily mutates a prepared statement's plansource/plan to trigger a
 * specific rejection reason, calls PlanIsShareable(), verifies the result,
 * then restores the original state.  Returns true if the predicate returned
 * the expected rejection reason.
 *
 * An outer PG_TRY/PG_FINALLY ensures ReleaseCachedPlan() is always called
 * exactly once.  Inner PG_TRY/PG_FINALLY blocks restore mutated fields
 * even if PlanIsShareable() throws an error.
 *
 * Supported reject_reason values:
 *   NOT_GENERIC, ONESHOT, INCOMPLETE, POST_REWRITE_HOOK,
 *   DEPENDS_ON_ROLE, SAVED_XMIN, EXTENSION_STATE,
 *   CUSTOM_SCAN, FOREIGN_SCAN
 */
PG_FUNCTION_INFO_V1(test_plan_shareability_force_reject);
Datum
test_plan_shareability_force_reject(PG_FUNCTION_ARGS)
{
	const char *stmt_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	const char *reason_name = text_to_cstring(PG_GETARG_TEXT_PP(1));
	PreparedStatement *pstmt;
	CachedPlanSource *plansource;
	CachedPlan *plan;
	SharedPlanRejectReason reason = SHARED_PLAN_REJECT_NONE;
	SharedPlanRejectReason expected = SHARED_PLAN_REJECT_NONE;
	bool		matched = false;

	pstmt = FetchPreparedStatement(stmt_name, true);
	plansource = pstmt->plansource;
	plan = GetCachedPlan(plansource, NULL, NULL, NULL);

	PG_TRY();
	{
		if (strcmp(reason_name, "NOT_GENERIC") == 0)
		{
			expected = SHARED_PLAN_REJECT_NOT_GENERIC;
			PlanIsShareable(plansource, plan, false, &reason);
			matched = (reason == expected);
		}
		else if (strcmp(reason_name, "ONESHOT") == 0)
		{
			bool		save_val = plansource->is_oneshot;

			expected = SHARED_PLAN_REJECT_ONESHOT;
			plansource->is_oneshot = true;
			PG_TRY();
			{
				PlanIsShareable(plansource, plan, true, &reason);
			}
			PG_FINALLY();
			{
				plansource->is_oneshot = save_val;
			}
			PG_END_TRY();
			matched = (reason == expected);
		}
		else if (strcmp(reason_name, "INCOMPLETE") == 0)
		{
			bool		save_val = plansource->is_complete;

			expected = SHARED_PLAN_REJECT_INCOMPLETE;
			plansource->is_complete = false;
			PG_TRY();
			{
				PlanIsShareable(plansource, plan, true, &reason);
			}
			PG_FINALLY();
			{
				plansource->is_complete = save_val;
			}
			PG_END_TRY();
			matched = (reason == expected);
		}
		else if (strcmp(reason_name, "POST_REWRITE_HOOK") == 0)
		{
			PostRewriteHook save_val = plansource->postRewrite;

			expected = SHARED_PLAN_REJECT_POST_REWRITE_HOOK;
			plansource->postRewrite = dummy_post_rewrite_hook;
			PG_TRY();
			{
				PlanIsShareable(plansource, plan, true, &reason);
			}
			PG_FINALLY();
			{
				plansource->postRewrite = save_val;
			}
			PG_END_TRY();
			matched = (reason == expected);
		}
		else if (strcmp(reason_name, "DEPENDS_ON_ROLE") == 0)
		{
			bool		save_val = plan->dependsOnRole;

			expected = SHARED_PLAN_REJECT_DEPENDS_ON_ROLE;
			plan->dependsOnRole = true;
			PG_TRY();
			{
				PlanIsShareable(plansource, plan, true, &reason);
			}
			PG_FINALLY();
			{
				plan->dependsOnRole = save_val;
			}
			PG_END_TRY();
			matched = (reason == expected);
		}
		else if (strcmp(reason_name, "SAVED_XMIN") == 0)
		{
			TransactionId save_val = plan->saved_xmin;

			expected = SHARED_PLAN_REJECT_SAVED_XMIN;
			plan->saved_xmin = FirstNormalTransactionId;
			PG_TRY();
			{
				PlanIsShareable(plansource, plan, true, &reason);
			}
			PG_FINALLY();
			{
				plan->saved_xmin = save_val;
			}
			PG_END_TRY();
			matched = (reason == expected);
		}
		else if (strcmp(reason_name, "EXTENSION_STATE") == 0)
		{
			PlannedStmt *target_pstmt;
			List	   *save_val;
			List	   *dummy_list;

			expected = SHARED_PLAN_REJECT_EXTENSION_STATE;
			target_pstmt = find_first_plannedstmt(plan);
			if (target_pstmt == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("no non-utility PlannedStmt found")));

			save_val = target_pstmt->extension_state;
			dummy_list = list_make1(makeDefElem("test_dummy", NULL, -1));
			target_pstmt->extension_state = dummy_list;
			PG_TRY();
			{
				PlanIsShareable(plansource, plan, true, &reason);
			}
			PG_FINALLY();
			{
				target_pstmt->extension_state = save_val;
				list_free_deep(dummy_list);
			}
			PG_END_TRY();
			matched = (reason == expected);
		}
		else if (strcmp(reason_name, "CUSTOM_SCAN") == 0)
		{
			PlannedStmt *target_pstmt;
			Plan	   *save_val;
			CustomScan *fake_node;

			expected = SHARED_PLAN_REJECT_CUSTOM_SCAN;
			target_pstmt = find_first_plannedstmt(plan);
			if (target_pstmt == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("no non-utility PlannedStmt found")));

			save_val = target_pstmt->planTree;
			fake_node = makeNode(CustomScan);
			target_pstmt->planTree = (Plan *) fake_node;
			PG_TRY();
			{
				PlanIsShareable(plansource, plan, true, &reason);
			}
			PG_FINALLY();
			{
				target_pstmt->planTree = save_val;
				pfree(fake_node);
			}
			PG_END_TRY();
			matched = (reason == expected);
		}
		else if (strcmp(reason_name, "FOREIGN_SCAN") == 0)
		{
			PlannedStmt *target_pstmt;
			Plan	   *save_val;
			ForeignScan *fake_node;

			expected = SHARED_PLAN_REJECT_FOREIGN_SCAN;
			target_pstmt = find_first_plannedstmt(plan);
			if (target_pstmt == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("no non-utility PlannedStmt found")));

			save_val = target_pstmt->planTree;
			fake_node = makeNode(ForeignScan);
			target_pstmt->planTree = (Plan *) fake_node;
			PG_TRY();
			{
				PlanIsShareable(plansource, plan, true, &reason);
			}
			PG_FINALLY();
			{
				target_pstmt->planTree = save_val;
				pfree(fake_node);
			}
			PG_END_TRY();
			matched = (reason == expected);
		}
		else if (strcmp(reason_name, "PLANNER_HOOK") == 0)
		{
			planner_hook_type save_val = planner_hook;

			expected = SHARED_PLAN_REJECT_PLANNER_HOOK;
			planner_hook = dummy_planner_hook;
			PG_TRY();
			{
				PlanIsShareable(plansource, plan, true, &reason);
			}
			PG_FINALLY();
			{
				planner_hook = save_val;
			}
			PG_END_TRY();
			matched = (reason == expected);
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unsupported reject reason: %s", reason_name)));
		}
	}
	PG_FINALLY();
	{
		ReleaseCachedPlan(plan, NULL);
	}
	PG_END_TRY();

	PG_RETURN_BOOL(matched);
}
