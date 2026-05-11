/*-------------------------------------------------------------------------
 *
 * pg_pipeline_graph.c
 *		Diagnostic PlanState classifier / annotation prototype.
 *
 * VERSION 1 SCOPE:
 *
 * This is a conservative one-node-per-fragment diagnostic classifier.
 * It does NOT build true multi-node pipeline fragments.  Each PlanState
 * node produces one diagnostic fragment that records source kind, boundary
 * kind, motion kind, annotations, and parent-child dependencies.
 *
 * A later patch may build true multi-node fragments (merging streaming
 * nodes into maximal PipelineFragments) and a fuller dependency DAG.
 *
 * This extension:
 * - Does NOT change execution behavior.
 * - Does NOT modify ExecProcNode() or ExecScan().
 * - Does NOT implement executable morsel pipelines.
 * - Does NOT implement vectorized expression execution.
 * - Does NOT add Exchange / Shuffle / Repartition executor nodes.
 * - Does NOT expose a stable user-facing SQL API.
 *
 * LIBRARY LOADING:
 * This module is loaded via LOAD 'pg_pipeline_graph' which triggers
 * _PG_init().  CREATE EXTENSION is optional and only installs the
 * .control metadata.  The GUC pg_pipeline_graph.enabled and the
 * ExecutorStart_hook are registered in _PG_init().
 *
 * DEVELOPER PROTOTYPE - NOT A STABLE API
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/executor.h"
#include "fmgr.h"
#include "nodes/plannodes.h"
#include "nodes/execnodes.h"
#include "utils/guc.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC_EXT(
	.name = "pg_pipeline_graph",
	.version = PG_VERSION
);

/* ---------- Enums ---------- */

typedef enum SourceKind
{
	SOURCE_NONE = 0,
	SOURCE_BASE_SCAN,
	SOURCE_SYNTHETIC,
	SOURCE_VALUES,
	SOURCE_FUNCTION,
	SOURCE_MATERIALIZED,
	SOURCE_CTE_TUPLESTORE,
	SOURCE_WORKTABLE,
	SOURCE_BREAKER_OUTPUT,
	SOURCE_MOTION_OUTPUT,
	SOURCE_OPAQUE
} SourceKind;

typedef enum BoundaryKind
{
	BOUNDARY_NONE = 0,
	BOUNDARY_BLOCKING,
	BOUNDARY_CONSERVATIVE,
	BOUNDARY_STATEFUL,
	BOUNDARY_RESCAN,
	BOUNDARY_PARAMETERIZED,
	BOUNDARY_MATERIALIZATION,
	BOUNDARY_SIDE_EFFECT,
	BOUNDARY_OPAQUE,
	BOUNDARY_MOTION,
	BOUNDARY_ASYNC,
	BOUNDARY_BITMAP,
	BOUNDARY_FAN_IN,
	BOUNDARY_SUBQUERY,
	BOUNDARY_UNSUPPORTED
} BoundaryKind;

typedef enum DependencyKind
{
	DEP_NONE = 0,
	DEP_DATA,
	DEP_BLOCKING,
	DEP_PARAMETER,
	DEP_INITPLAN_PARAM,
	DEP_SUBPLAN_UNCORRELATED_ONCE,
	DEP_SUBPLAN_CORRELATED_PER_TUPLE,
	DEP_SUBPLAN_HASHED,
	DEP_SUBPLAN_UNKNOWN,
	DEP_MATERIALIZATION,
	DEP_MOTION,
	DEP_BITMAP,
	DEP_FAN_IN,
	DEP_SIDE_EFFECT,
	DEP_OPAQUE
} DependencyKind;

typedef enum MotionKind
{
	MOTION_NONE = 0,
	MOTION_GATHER,
	MOTION_GATHER_MERGE,
	MOTION_FUTURE_BROADCAST,
	MOTION_FUTURE_HASH_REPARTITION,
	MOTION_FUTURE_RANGE_REPARTITION,
	MOTION_FUTURE_ROUND_ROBIN,
	MOTION_FUTURE_LOCAL_EXCHANGE,
	MOTION_FUTURE_REMOTE_EXCHANGE
} MotionKind;

typedef enum TriState
{
	TRI_UNKNOWN = 0,
	TRI_YES,
	TRI_NO
} TriState;

typedef enum OrderingKind
{
	ORDERING_UNKNOWN = 0,
	ORDERING_SORTED,
	ORDERING_PARTIALLY_SORTED
} OrderingKind;

typedef enum DistributionKind
{
	DIST_UNKNOWN = 0,
	DIST_SINGLE,
	DIST_REPLICATED,
	DIST_HASH_PARTITIONED,
	DIST_RANGE_PARTITIONED,
	DIST_ROUND_ROBIN
} DistributionKind;

typedef enum ExecDomainKind
{
	DOMAIN_UNKNOWN = 0,
	DOMAIN_LEADER,
	DOMAIN_WORKER,
	DOMAIN_LOCAL_BACKEND,
	DOMAIN_FUTURE_REMOTE
} ExecDomainKind;

/* ---------- Data Structures ---------- */

typedef struct FragmentOutputProperties
{
	OrderingKind	ordering;
	DistributionKind distribution;
	ExecDomainKind	exec_domain;
	TriState		materialized;
	TriState		rewindable;
	TriState		rescannable;
	TriState		preserves_order;
} FragmentOutputProperties;

typedef struct PipelineFragment
{
	int				fragment_id;
	int				plan_node_id;
	NodeTag			node_tag;
	SourceKind		source_kind;
	BoundaryKind	boundary_kind;
	MotionKind		motion_kind;
	FragmentOutputProperties properties;

	/* Annotations */
	bool			has_qual;
	bool			has_projection;
	bool			has_initplan;
	bool			has_subplan;
	bool			async_capable;
	bool			parallel_aware;

	const char	   *node_type_name;
} PipelineFragment;

typedef struct PipelineDependency
{
	int				from_fragment_id;
	int				to_fragment_id;
	DependencyKind	dep_kind;
} PipelineDependency;

/*
 * Dynamic storage using Lists.  Avoids silent truncation of the previous
 * fixed-array approach.
 */
typedef struct PipelineGraph
{
	List		   *fragments;		/* List of PipelineFragment */
	List		   *dependencies;	/* List of PipelineDependency */
	int				num_unsupported;
} PipelineGraph;

typedef struct PipelineBuildContext
{
	PipelineGraph  *graph;
	int				next_fragment_id;
	MemoryContext	build_context;
} PipelineBuildContext;

typedef struct PipelineOutput
{
	int				fragment_id;
} PipelineOutput;

/* ---------- GUC Variables ---------- */

/*
 * PGC_SUSET: only superusers can enable diagnostic output.
 * This is developer/debug instrumentation that emits verbose DEBUG1
 * messages for every query.  Restricting to superuser prevents
 * unprivileged users from enabling noisy diagnostic output.
 */
static bool pgpg_enabled = false;

/* ---------- Hook State ---------- */

static ExecutorStart_hook_type prev_ExecutorStart = NULL;

/* ---------- Function Declarations ---------- */

void _PG_init(void);
void _PG_fini(void);

static void pgpg_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void pgpg_build_and_print(PlanState *planstate);
static PipelineOutput pgpg_build_pipeline(PlanState *node,
										  PipelineBuildContext *ctx,
										  int parent_fragment_id);
static PipelineFragment *pgpg_add_fragment(PipelineBuildContext *ctx,
										   PlanState *node,
										   SourceKind source_kind,
										   BoundaryKind boundary_kind);
static void pgpg_add_dependency(PipelineBuildContext *ctx,
								int from_id, int to_id,
								DependencyKind dep_kind);
static void pgpg_classify_node(PlanState *node,
							   SourceKind *source_kind,
							   BoundaryKind *boundary_kind,
							   MotionKind *motion_kind);
static void pgpg_print_graph(PipelineGraph *graph);
static const char *pgpg_source_kind_str(SourceKind sk);
static const char *pgpg_boundary_kind_str(BoundaryKind bk);
static const char *pgpg_dep_kind_str(DependencyKind dk);
static const char *pgpg_motion_kind_str(MotionKind mk);
static const char *pgpg_ordering_str(OrderingKind ok);
static const char *pgpg_distribution_str(DistributionKind dk);
static const char *pgpg_domain_str(ExecDomainKind ek);
static const char *pgpg_tristate_str(TriState ts);
static const char *pgpg_node_tag_name(NodeTag tag);

/* ---------- Module Initialization ---------- */

void
_PG_init(void)
{
	DefineCustomBoolVariable("pg_pipeline_graph.enabled",
							"Enable diagnostic PlanState classifier.",
							"When enabled, classifies each PlanState node and prints "
							"a one-node-per-fragment diagnostic graph via DEBUG1.",
							&pgpg_enabled,
							false,
							PGC_SUSET,
							0,
							NULL, NULL, NULL);

	MarkGUCPrefixReserved("pg_pipeline_graph");

	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = pgpg_ExecutorStart;
}

/*
 * _PG_fini: attempt safe hook restoration on unload.
 *
 * PostgreSQL normally does not unload C extension libraries during backend
 * lifetime.  If another extension has chained after this module, we cannot
 * safely repair the hook chain during unload; therefore _PG_fini only
 * restores the hook when this module is still the active (most-recently
 * installed) hook.
 */
void
_PG_fini(void)
{
	if (ExecutorStart_hook == pgpg_ExecutorStart)
		ExecutorStart_hook = prev_ExecutorStart;
}

/* ---------- Hook Implementation ---------- */

static void
pgpg_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	if (!pgpg_enabled)
		return;
	if (queryDesc == NULL || queryDesc->planstate == NULL)
		return;

	pgpg_build_and_print(queryDesc->planstate);
}

/* ---------- Builder Entry Point ---------- */

static void
pgpg_build_and_print(PlanState *planstate)
{
	MemoryContext oldcontext;
	MemoryContext graphcontext;
	PipelineGraph *graph;

	graphcontext = AllocSetContextCreate(CurrentMemoryContext,
										 "PipelineGraphContext",
										 ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(graphcontext);

	graph = palloc0(sizeof(PipelineGraph));

	{
		PipelineBuildContext ctx;

		ctx.graph = graph;
		ctx.next_fragment_id = 1;
		ctx.build_context = graphcontext;

		pgpg_build_pipeline(planstate, &ctx, 0);
	}

	pgpg_print_graph(graph);

	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(graphcontext);
}

/* ---------- Node Classification ---------- */

static void
pgpg_classify_node(PlanState *node,
				   SourceKind *source_kind,
				   BoundaryKind *boundary_kind,
				   MotionKind *motion_kind)
{
	Plan	   *plan = node->plan;
	NodeTag		tag = nodeTag(plan);

	*source_kind = SOURCE_NONE;
	*boundary_kind = BOUNDARY_NONE;
	*motion_kind = MOTION_NONE;

	switch (tag)
	{
		case T_SeqScan:
			*source_kind = SOURCE_BASE_SCAN;
			break;
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_TidScan:
		case T_TidRangeScan:
			*source_kind = SOURCE_BASE_SCAN;
			break;
		case T_BitmapIndexScan:
		case T_BitmapAnd:
		case T_BitmapOr:
			*boundary_kind = BOUNDARY_BITMAP;
			break;
		case T_BitmapHeapScan:
			*source_kind = SOURCE_BASE_SCAN;
			*boundary_kind = BOUNDARY_BITMAP;
			break;
		case T_ValuesScan:
			*source_kind = SOURCE_VALUES;
			break;
		case T_FunctionScan:
			/*
			 * SOURCE_FUNCTION with conservative boundary: SRFs, volatility,
			 * and side effects may be involved.  Boundary/conservative
			 * eligibility takes precedence over source-kind for future
			 * executable eligibility decisions.
			 */
			*source_kind = SOURCE_FUNCTION;
			*boundary_kind = BOUNDARY_CONSERVATIVE;
			break;
		case T_CteScan:
			*source_kind = SOURCE_CTE_TUPLESTORE;
			break;
		case T_WorkTableScan:
			*source_kind = SOURCE_WORKTABLE;
			break;
		case T_Result:
			if (outerPlanState(node) == NULL)
				*source_kind = SOURCE_SYNTHETIC;
			break;
		case T_SubqueryScan:
			*boundary_kind = BOUNDARY_SUBQUERY;
			break;
		case T_ForeignScan:
			*source_kind = SOURCE_OPAQUE;
			*boundary_kind = BOUNDARY_OPAQUE;
			break;
		case T_CustomScan:
			*source_kind = SOURCE_OPAQUE;
			*boundary_kind = BOUNDARY_OPAQUE;
			break;
		case T_ProjectSet:
			*boundary_kind = BOUNDARY_CONSERVATIVE;
			break;
		case T_Limit:
			break;
		case T_Append:
		case T_MergeAppend:
			*boundary_kind = BOUNDARY_FAN_IN;
			break;
		case T_RecursiveUnion:
			*boundary_kind = BOUNDARY_BLOCKING;
			break;
		case T_NestLoop:
		{
			NestLoop   *nl = (NestLoop *) plan;

			if (nl->nestParams != NIL)
				*boundary_kind = BOUNDARY_PARAMETERIZED;
			else
				*boundary_kind = BOUNDARY_FAN_IN;
			break;
		}
		case T_MergeJoin:
			*boundary_kind = BOUNDARY_FAN_IN;
			break;
		case T_HashJoin:
			/*
			 * v1: HashJoin classified as conservative fan-in boundary.
			 * Build/probe roles are not fully modeled as separate pipeline
			 * fragments in v1.  Hash child is independently classified as
			 * a blocking boundary.
			 */
			*boundary_kind = BOUNDARY_FAN_IN;
			break;
		case T_Hash:
			*boundary_kind = BOUNDARY_BLOCKING;
			break;
		case T_Sort:
			/*
			 * v1 dual annotation: source_kind=BREAKER_OUTPUT represents the
			 * logical output side; boundary_kind=BLOCKING represents the
			 * input-side blocking behavior.  A later patch may split these
			 * into separate input/output fragments.
			 */
			*source_kind = SOURCE_BREAKER_OUTPUT;
			*boundary_kind = BOUNDARY_BLOCKING;
			break;
		case T_IncrementalSort:
			*boundary_kind = BOUNDARY_CONSERVATIVE;
			break;
		case T_Material:
			*boundary_kind = BOUNDARY_MATERIALIZATION;
			break;
		case T_Memoize:
			*boundary_kind = BOUNDARY_MATERIALIZATION;
			break;
		case T_Agg:
		{
			Agg		   *agg = (Agg *) plan;

			switch (agg->aggstrategy)
			{
				case AGG_PLAIN:
				case AGG_HASHED:
					/* Full blocker - dual annotation like Sort */
					*source_kind = SOURCE_BREAKER_OUTPUT;
					*boundary_kind = BOUNDARY_BLOCKING;
					break;
				case AGG_SORTED:
					*boundary_kind = BOUNDARY_CONSERVATIVE;
					break;
				case AGG_MIXED:
					*boundary_kind = BOUNDARY_CONSERVATIVE;
					break;
			}
			break;
		}
		case T_WindowAgg:
			*boundary_kind = BOUNDARY_CONSERVATIVE;
			break;
		case T_Unique:
			*boundary_kind = BOUNDARY_STATEFUL;
			break;
		case T_SetOp:
			*source_kind = SOURCE_BREAKER_OUTPUT;
			*boundary_kind = BOUNDARY_BLOCKING;
			break;
		case T_LockRows:
			*boundary_kind = BOUNDARY_SIDE_EFFECT;
			break;
		case T_ModifyTable:
			*boundary_kind = BOUNDARY_SIDE_EFFECT;
			break;
		case T_Gather:
			*source_kind = SOURCE_MOTION_OUTPUT;
			*boundary_kind = BOUNDARY_MOTION;
			*motion_kind = MOTION_GATHER;
			break;
		case T_GatherMerge:
			*source_kind = SOURCE_MOTION_OUTPUT;
			*boundary_kind = BOUNDARY_MOTION;
			*motion_kind = MOTION_GATHER_MERGE;
			break;
		default:
			*boundary_kind = BOUNDARY_UNSUPPORTED;
			break;
	}
}

/* ---------- Builder Algorithm ---------- */

static PipelineFragment *
pgpg_add_fragment(PipelineBuildContext *ctx, PlanState *node,
				  SourceKind source_kind, BoundaryKind boundary_kind)
{
	PipelineFragment *frag;
	Plan	   *plan = node->plan;

	frag = palloc0(sizeof(PipelineFragment));
	frag->fragment_id = ctx->next_fragment_id++;
	frag->plan_node_id = plan->plan_node_id;
	frag->node_tag = nodeTag(plan);
	frag->source_kind = source_kind;
	frag->boundary_kind = boundary_kind;
	frag->motion_kind = MOTION_NONE;
	frag->node_type_name = pgpg_node_tag_name(nodeTag(plan));

	/* Annotations */
	frag->has_qual = (node->qual != NULL);
	frag->has_projection = (node->ps_ProjInfo != NULL);
	frag->has_initplan = (node->initPlan != NIL);
	frag->has_subplan = (node->subPlan != NIL);
	frag->async_capable = node->async_capable;
	frag->parallel_aware = plan->parallel_aware;

	/* Output properties - mostly UNKNOWN in v1 classifier */
	frag->properties.ordering = ORDERING_UNKNOWN;
	frag->properties.distribution = DIST_UNKNOWN;
	frag->properties.exec_domain = DOMAIN_UNKNOWN;
	frag->properties.materialized = TRI_UNKNOWN;
	frag->properties.rewindable = TRI_UNKNOWN;
	frag->properties.rescannable = TRI_UNKNOWN;
	frag->properties.preserves_order = TRI_UNKNOWN;

	/* Derive a few obvious properties */
	if (nodeTag(plan) == T_Sort)
	{
		frag->properties.ordering = ORDERING_SORTED;
		frag->properties.materialized = TRI_YES;
	}
	else if (nodeTag(plan) == T_Gather)
	{
		frag->properties.exec_domain = DOMAIN_LEADER;
		frag->properties.distribution = DIST_SINGLE;
	}
	else if (nodeTag(plan) == T_GatherMerge)
	{
		frag->properties.exec_domain = DOMAIN_LEADER;
		frag->properties.distribution = DIST_SINGLE;
		frag->properties.ordering = ORDERING_SORTED;
	}
	else if (nodeTag(plan) == T_Material)
	{
		frag->properties.materialized = TRI_YES;
		frag->properties.rewindable = TRI_YES;
	}

	ctx->graph->fragments = lappend(ctx->graph->fragments, frag);
	return frag;
}

static void
pgpg_add_dependency(PipelineBuildContext *ctx,
					int from_id, int to_id,
					DependencyKind dep_kind)
{
	PipelineDependency *dep;

	if (from_id == 0 || to_id == 0)
		return;

	dep = palloc0(sizeof(PipelineDependency));
	dep->from_fragment_id = from_id;
	dep->to_fragment_id = to_id;
	dep->dep_kind = dep_kind;

	ctx->graph->dependencies = lappend(ctx->graph->dependencies, dep);
}

static PipelineOutput
pgpg_build_pipeline(PlanState *node, PipelineBuildContext *ctx,
					int parent_fragment_id)
{
	PipelineOutput output = {0};
	SourceKind source_kind;
	BoundaryKind boundary_kind;
	MotionKind motion_kind;
	PipelineFragment *frag;
	ListCell   *lc;

	if (node == NULL)
		return output;

	pgpg_classify_node(node, &source_kind, &boundary_kind, &motion_kind);

	frag = pgpg_add_fragment(ctx, node, source_kind, boundary_kind);
	frag->motion_kind = motion_kind;
	output.fragment_id = frag->fragment_id;

	if (boundary_kind == BOUNDARY_UNSUPPORTED)
		ctx->graph->num_unsupported++;

	/* Process children based on boundary kind */
	switch (boundary_kind)
	{
		case BOUNDARY_FAN_IN:
		case BOUNDARY_PARAMETERIZED:
		{
			PlanState  *outer = outerPlanState(node);
			PlanState  *inner = innerPlanState(node);

			if (outer)
			{
				PipelineOutput child_out;

				child_out = pgpg_build_pipeline(outer, ctx,
											   frag->fragment_id);
				if (child_out.fragment_id > 0)
					pgpg_add_dependency(ctx, child_out.fragment_id,
										frag->fragment_id, DEP_DATA);
			}
			if (inner)
			{
				PipelineOutput child_out;
				DependencyKind dep = DEP_DATA;

				if (boundary_kind == BOUNDARY_PARAMETERIZED)
					dep = DEP_PARAMETER;
				child_out = pgpg_build_pipeline(inner, ctx,
											   frag->fragment_id);
				if (child_out.fragment_id > 0)
					pgpg_add_dependency(ctx, child_out.fragment_id,
										frag->fragment_id, dep);
			}

			/* Append/MergeAppend children via appendplans array */
			if (IsA(node->plan, Append) || IsA(node->plan, MergeAppend))
			{
				PlanState **appendplans = NULL;
				int			nplans = 0;

				if (IsA(node, AppendState))
				{
					AppendState *as = (AppendState *) node;

					appendplans = as->appendplans;
					nplans = as->as_nplans;
				}
				else if (IsA(node, MergeAppendState))
				{
					MergeAppendState *ms = (MergeAppendState *) node;

					appendplans = ms->mergeplans;
					nplans = ms->ms_nplans;
				}
				for (int i = 0; i < nplans; i++)
				{
					PipelineOutput child_out;

					child_out = pgpg_build_pipeline(appendplans[i], ctx,
												   frag->fragment_id);
					if (child_out.fragment_id > 0)
						pgpg_add_dependency(ctx, child_out.fragment_id,
											frag->fragment_id, DEP_FAN_IN);
				}
			}
			break;
		}

		default:
		{
			/* All other boundary kinds: process outer then inner */
			PlanState  *outer = outerPlanState(node);
			PlanState  *inner = innerPlanState(node);

			if (outer)
			{
				PipelineOutput child_out;
				DependencyKind dep = DEP_DATA;

				if (boundary_kind == BOUNDARY_BLOCKING)
					dep = DEP_BLOCKING;
				else if (boundary_kind == BOUNDARY_MOTION)
					dep = DEP_MOTION;
				else if (boundary_kind == BOUNDARY_BITMAP)
					dep = DEP_BITMAP;
				else if (boundary_kind == BOUNDARY_SIDE_EFFECT)
					dep = DEP_SIDE_EFFECT;
				else if (boundary_kind == BOUNDARY_OPAQUE)
					dep = DEP_OPAQUE;
				else if (boundary_kind == BOUNDARY_MATERIALIZATION)
					dep = DEP_MATERIALIZATION;

				child_out = pgpg_build_pipeline(outer, ctx,
											   frag->fragment_id);
				if (child_out.fragment_id > 0)
					pgpg_add_dependency(ctx, child_out.fragment_id,
										frag->fragment_id, dep);
			}
			if (inner)
			{
				PipelineOutput child_out;

				child_out = pgpg_build_pipeline(inner, ctx,
											   frag->fragment_id);
				if (child_out.fragment_id > 0)
					pgpg_add_dependency(ctx, child_out.fragment_id,
										frag->fragment_id, DEP_BLOCKING);
			}
			break;
		}
	}

	/* InitPlan dependencies */
	foreach(lc, node->initPlan)
	{
		SubPlanState *sps = (SubPlanState *) lfirst(lc);

		if (sps && sps->planstate)
		{
			PipelineOutput child_out;

			child_out = pgpg_build_pipeline(sps->planstate, ctx,
										   frag->fragment_id);
			if (child_out.fragment_id > 0)
				pgpg_add_dependency(ctx, child_out.fragment_id,
									frag->fragment_id, DEP_INITPLAN_PARAM);
		}
	}

	/*
	 * SubPlan handling: v1 only annotates has_subplan=true.
	 * Full SubPlan dependency classification (UNCORRELATED_ONCE,
	 * CORRELATED_PER_TUPLE, HASHED) is not implemented in v1.
	 * All would be DEP_SUBPLAN_UNKNOWN if traversed.  Left as TODO.
	 */

	return output;
}

/* ---------- Output Printing ---------- */

static void
pgpg_print_graph(PipelineGraph *graph)
{
	ListCell   *lc;
	int			nfrags = list_length(graph->fragments);
	int			ndeps = list_length(graph->dependencies);

	ereport(DEBUG1,
			(errmsg_internal("PIPELINE CLASSIFIER: %d fragments, %d dependencies, %d unsupported "
							 "(v1: one-node-per-fragment diagnostic classifier)",
							 nfrags, ndeps, graph->num_unsupported)));

	foreach(lc, graph->fragments)
	{
		PipelineFragment *f = (PipelineFragment *) lfirst(lc);

		ereport(DEBUG1,
				(errmsg_internal("  Fragment %d: node=%s plan_node_id=%d "
								 "source=%s boundary=%s motion=%s",
								 f->fragment_id,
								 f->node_type_name,
								 f->plan_node_id,
								 pgpg_source_kind_str(f->source_kind),
								 pgpg_boundary_kind_str(f->boundary_kind),
								 pgpg_motion_kind_str(f->motion_kind))));
		ereport(DEBUG1,
				(errmsg_internal("    qual=%s proj=%s initplan=%s subplan=%s "
								 "async=%s parallel_aware=%s",
								 f->has_qual ? "Y" : "N",
								 f->has_projection ? "Y" : "N",
								 f->has_initplan ? "Y" : "N",
								 f->has_subplan ? "Y" : "N",
								 f->async_capable ? "Y" : "N",
								 f->parallel_aware ? "Y" : "N")));
		if (f->has_subplan)
			ereport(DEBUG1,
					(errmsg_internal("    NOTE: has SubPlan (dep=SUBPLAN_UNKNOWN in v1; "
									 "expression-triggered, not modeled as child fragment)")));
		if (f->async_capable)
			ereport(DEBUG1,
					(errmsg_internal("    NOTE: async-capable (annotation only in v1; "
									 "async/readiness boundary not split)")));
		ereport(DEBUG1,
				(errmsg_internal("    properties: ordering=%s distribution=%s "
								 "domain=%s materialized=%s rewindable=%s "
								 "rescannable=%s preserves_order=%s",
								 pgpg_ordering_str(f->properties.ordering),
								 pgpg_distribution_str(f->properties.distribution),
								 pgpg_domain_str(f->properties.exec_domain),
								 pgpg_tristate_str(f->properties.materialized),
								 pgpg_tristate_str(f->properties.rewindable),
								 pgpg_tristate_str(f->properties.rescannable),
								 pgpg_tristate_str(f->properties.preserves_order))));
	}

	foreach(lc, graph->dependencies)
	{
		PipelineDependency *d = (PipelineDependency *) lfirst(lc);

		ereport(DEBUG1,
				(errmsg_internal("  Dependency: fragment %d -> fragment %d [%s]",
								 d->from_fragment_id,
								 d->to_fragment_id,
								 pgpg_dep_kind_str(d->dep_kind))));
	}

	if (graph->num_unsupported > 0)
		ereport(DEBUG1,
				(errmsg_internal("  WARNING: %d unsupported/unknown node(s)",
								 graph->num_unsupported)));
}

/* ---------- String Helpers ---------- */

static const char *
pgpg_source_kind_str(SourceKind sk)
{
	switch (sk)
	{
		case SOURCE_NONE: return "NONE";
		case SOURCE_BASE_SCAN: return "BASE_SCAN";
		case SOURCE_SYNTHETIC: return "SYNTHETIC";
		case SOURCE_VALUES: return "VALUES";
		case SOURCE_FUNCTION: return "FUNCTION";
		case SOURCE_MATERIALIZED: return "MATERIALIZED";
		case SOURCE_CTE_TUPLESTORE: return "CTE_TUPLESTORE";
		case SOURCE_WORKTABLE: return "WORKTABLE";
		case SOURCE_BREAKER_OUTPUT: return "BREAKER_OUTPUT";
		case SOURCE_MOTION_OUTPUT: return "MOTION_OUTPUT";
		case SOURCE_OPAQUE: return "OPAQUE";
	}
	return "UNKNOWN";
}

static const char *
pgpg_boundary_kind_str(BoundaryKind bk)
{
	switch (bk)
	{
		case BOUNDARY_NONE: return "NONE";
		case BOUNDARY_BLOCKING: return "BLOCKING";
		case BOUNDARY_CONSERVATIVE: return "CONSERVATIVE";
		case BOUNDARY_STATEFUL: return "STATEFUL";
		case BOUNDARY_RESCAN: return "RESCAN";
		case BOUNDARY_PARAMETERIZED: return "PARAMETERIZED";
		case BOUNDARY_MATERIALIZATION: return "MATERIALIZATION";
		case BOUNDARY_SIDE_EFFECT: return "SIDE_EFFECT";
		case BOUNDARY_OPAQUE: return "OPAQUE";
		case BOUNDARY_MOTION: return "MOTION";
		case BOUNDARY_ASYNC: return "ASYNC";
		case BOUNDARY_BITMAP: return "BITMAP";
		case BOUNDARY_FAN_IN: return "FAN_IN";
		case BOUNDARY_SUBQUERY: return "SUBQUERY";
		case BOUNDARY_UNSUPPORTED: return "UNSUPPORTED";
	}
	return "UNKNOWN";
}

static const char *
pgpg_dep_kind_str(DependencyKind dk)
{
	switch (dk)
	{
		case DEP_NONE: return "NONE";
		case DEP_DATA: return "DATA";
		case DEP_BLOCKING: return "BLOCKING";
		case DEP_PARAMETER: return "PARAMETER";
		case DEP_INITPLAN_PARAM: return "INITPLAN_PARAM";
		case DEP_SUBPLAN_UNCORRELATED_ONCE: return "SUBPLAN_UNCORRELATED_ONCE";
		case DEP_SUBPLAN_CORRELATED_PER_TUPLE: return "SUBPLAN_CORRELATED_PER_TUPLE";
		case DEP_SUBPLAN_HASHED: return "SUBPLAN_HASHED";
		case DEP_SUBPLAN_UNKNOWN: return "SUBPLAN_UNKNOWN";
		case DEP_MATERIALIZATION: return "MATERIALIZATION";
		case DEP_MOTION: return "MOTION";
		case DEP_BITMAP: return "BITMAP";
		case DEP_FAN_IN: return "FAN_IN";
		case DEP_SIDE_EFFECT: return "SIDE_EFFECT";
		case DEP_OPAQUE: return "OPAQUE";
	}
	return "UNKNOWN";
}

static const char *
pgpg_motion_kind_str(MotionKind mk)
{
	switch (mk)
	{
		case MOTION_NONE: return "NONE";
		case MOTION_GATHER: return "GATHER";
		case MOTION_GATHER_MERGE: return "GATHER_MERGE";
		case MOTION_FUTURE_BROADCAST: return "FUTURE_BROADCAST";
		case MOTION_FUTURE_HASH_REPARTITION: return "FUTURE_HASH_REPARTITION";
		case MOTION_FUTURE_RANGE_REPARTITION: return "FUTURE_RANGE_REPARTITION";
		case MOTION_FUTURE_ROUND_ROBIN: return "FUTURE_ROUND_ROBIN";
		case MOTION_FUTURE_LOCAL_EXCHANGE: return "FUTURE_LOCAL_EXCHANGE";
		case MOTION_FUTURE_REMOTE_EXCHANGE: return "FUTURE_REMOTE_EXCHANGE";
	}
	return "UNKNOWN";
}

static const char *
pgpg_ordering_str(OrderingKind ok)
{
	switch (ok)
	{
		case ORDERING_UNKNOWN: return "UNKNOWN";
		case ORDERING_SORTED: return "SORTED";
		case ORDERING_PARTIALLY_SORTED: return "PARTIALLY_SORTED";
	}
	return "UNKNOWN";
}

static const char *
pgpg_distribution_str(DistributionKind dk)
{
	switch (dk)
	{
		case DIST_UNKNOWN: return "UNKNOWN";
		case DIST_SINGLE: return "SINGLE";
		case DIST_REPLICATED: return "REPLICATED";
		case DIST_HASH_PARTITIONED: return "HASH_PARTITIONED";
		case DIST_RANGE_PARTITIONED: return "RANGE_PARTITIONED";
		case DIST_ROUND_ROBIN: return "ROUND_ROBIN";
	}
	return "UNKNOWN";
}

static const char *
pgpg_domain_str(ExecDomainKind ek)
{
	switch (ek)
	{
		case DOMAIN_UNKNOWN: return "UNKNOWN";
		case DOMAIN_LEADER: return "LEADER";
		case DOMAIN_WORKER: return "WORKER";
		case DOMAIN_LOCAL_BACKEND: return "LOCAL_BACKEND";
		case DOMAIN_FUTURE_REMOTE: return "FUTURE_REMOTE";
	}
	return "UNKNOWN";
}

static const char *
pgpg_tristate_str(TriState ts)
{
	switch (ts)
	{
		case TRI_UNKNOWN: return "UNKNOWN";
		case TRI_YES: return "YES";
		case TRI_NO: return "NO";
	}
	return "UNKNOWN";
}

static const char *
pgpg_node_tag_name(NodeTag tag)
{
	switch (tag)
	{
		case T_SeqScan: return "SeqScan";
		case T_IndexScan: return "IndexScan";
		case T_IndexOnlyScan: return "IndexOnlyScan";
		case T_BitmapIndexScan: return "BitmapIndexScan";
		case T_BitmapHeapScan: return "BitmapHeapScan";
		case T_BitmapAnd: return "BitmapAnd";
		case T_BitmapOr: return "BitmapOr";
		case T_TidScan: return "TidScan";
		case T_TidRangeScan: return "TidRangeScan";
		case T_SubqueryScan: return "SubqueryScan";
		case T_FunctionScan: return "FunctionScan";
		case T_ValuesScan: return "ValuesScan";
		case T_CteScan: return "CteScan";
		case T_WorkTableScan: return "WorkTableScan";
		case T_ForeignScan: return "ForeignScan";
		case T_CustomScan: return "CustomScan";
		case T_Result: return "Result";
		case T_ProjectSet: return "ProjectSet";
		case T_Limit: return "Limit";
		case T_Append: return "Append";
		case T_MergeAppend: return "MergeAppend";
		case T_RecursiveUnion: return "RecursiveUnion";
		case T_NestLoop: return "NestLoop";
		case T_MergeJoin: return "MergeJoin";
		case T_HashJoin: return "HashJoin";
		case T_Hash: return "Hash";
		case T_Sort: return "Sort";
		case T_IncrementalSort: return "IncrementalSort";
		case T_Material: return "Material";
		case T_Memoize: return "Memoize";
		case T_Agg: return "Agg";
		case T_WindowAgg: return "WindowAgg";
		case T_Unique: return "Unique";
		case T_SetOp: return "SetOp";
		case T_LockRows: return "LockRows";
		case T_ModifyTable: return "ModifyTable";
		case T_Gather: return "Gather";
		case T_GatherMerge: return "GatherMerge";
		default: return "Unknown";
	}
}
