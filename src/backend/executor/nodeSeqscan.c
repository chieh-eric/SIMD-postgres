/*-------------------------------------------------------------------------
 *
 * nodeSeqscan.c
 *	  Support routines for sequential scans of relations.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeSeqscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *		ExecSeqScan				sequentially scans a relation.
 *		ExecSeqNext				retrieve next tuple in sequential order.
 *		ExecInitSeqScan			creates and initializes a seqscan node.
 *		ExecEndSeqScan			releases any storage allocated.
 *		ExecReScanSeqScan		rescans the relation
 *
 *		ExecSeqScanEstimate		estimates DSM space needed for parallel scan
 *		ExecSeqScanInitializeDSM initialize DSM for parallel scan
 *		ExecSeqScanReInitializeDSM reinitialize DSM for fresh parallel scan
 *		ExecSeqScanInitializeWorker attach to DSM info in parallel worker
 */
#include "postgres.h"

#include "access/relscan.h"
#include "access/tableam.h"
#include "executor/execScan.h"
#include "executor/executor.h"
#include "executor/nodeSeqscan.h"
#include "utils/rel.h"
#include <immintrin.h>

#define INT4GTOID 521
#define INT4EQOID 96
#define INT4LTOID 97


static bool
analyze_simd_filter_qual(Expr *qual,
                         AttrNumber *attno,
                         SimdFilterOp *opkind,
                         bool *is_param,
                         int *param_id,
                         int32 *const_val)
{
    if (qual == NULL)
        return false;
    if (!IsA(qual, OpExpr))
        return false;

    OpExpr *op = (OpExpr *) qual;
    /* expect exactly 2 args: Var op Const/Param */
    if (list_length(op->args) != 2)
        return false;

    Expr *left  = linitial(op->args);
    Expr *right = lsecond(op->args);

    /* left must be Var(l_orderkey) */
    if (!IsA(left, Var))
        return false;

    Var *var = (Var *) left;
    if (var->varattno <= 0)
        return false;
    if (var->vartype != INT4OID)
        return false;

    /* map operator OID -> SimdFilterOp */
    SimdFilterOp opk;
    if (op->opno == INT4GTOID)
        opk = SIMD_FILTER_GT;
    else if (op->opno == INT4EQOID)
        opk = SIMD_FILTER_EQ;
    else if (op->opno == INT4LTOID)
        opk = SIMD_FILTER_LT;
    else
        return false;      /* unsupported operator */

    /* right is Const OR Param(int4) */
    if (IsA(right, Const))
    {
        Const *c = (Const *) right;

        if (c->constisnull || c->consttype != INT4OID)
            return false;

        *attno     = var->varattno;
        *opkind    = opk;
        *is_param  = false;
        *const_val = DatumGetInt32(c->constvalue);
        return true;
    }
    else if (IsA(right, Param))
    {
        Param *p = (Param *) right;

        if (p->paramtype != INT4OID)
            return false;

        *attno       = var->varattno;
        *opkind      = opk;
        *is_param    = true;
        *param_id    = p->paramid;
        *const_val   = 0;       /* filled at runtime */
        return true;
    }

    return false;
}

static void
update_simd_threshold_from_param(SeqScanState *node)
{
    if (!node->filter_from_param)
        return;

    EState         *estate = node->ss.ps.state;
    ParamExecData  *prm;

    /* For simple Param execution, you can use es_param_list_info or es_param_exec_vals
       depending on how your query is executed. For simplicity, assume exec params: */
    prm = &(estate->es_param_exec_vals[node->filter_param_id]);

    Assert(!prm->isnull);
    node->filter_threshold = DatumGetInt32(prm->value);
}

static int
simd_filter_int32(int32 *values, int n, int32 threshold,
                  SimdFilterOp op, int *match_idx)
{
    int match_count = 0;
	Assert(n >= 0);
    Assert(n <= 4096);

#ifdef __AVX2__
    __m256i thr = _mm256_set1_epi32(threshold);
    int i = 0;

    for (; i + 8 <= n; i += 8)
    {
        __m256i v = _mm256_loadu_si256((const __m256i *)&values[i]);
        __m256i cmp;

        switch (op)
        {
            case SIMD_FILTER_GT:
                cmp = _mm256_cmpgt_epi32(v, thr);             /* v > thr */
                break;
            case SIMD_FILTER_LT:
                /* v < thr  == thr > v */
                cmp = _mm256_cmpgt_epi32(thr, v);
                break;
            case SIMD_FILTER_EQ:
                cmp = _mm256_cmpeq_epi32(v, thr);             /* v == thr */
                break;
            default:
                /* shouldn't happen */
                cmp = _mm256_setzero_si256();
                break;
        }

        int mask = _mm256_movemask_ps(_mm256_castsi256_ps(cmp));

        while (mask)
        {
            int bit = __builtin_ctz(mask);
            match_idx[match_count++] = i + bit;
            mask &= (mask - 1);
        }
    }

    /* scalar tail */
    for (; i < n; i++)
    {
        bool pass = false;

        switch (op)
        {
            case SIMD_FILTER_GT: pass = (values[i] > threshold); break;
            case SIMD_FILTER_LT: pass = (values[i] < threshold); break;
            case SIMD_FILTER_EQ: pass = (values[i] == threshold); break;
            default:             pass = false; break;
        }

        if (pass)
            match_idx[match_count++] = i;
    }
#else
    /* pure scalar fallback */
    for (int i = 0; i < n; i++)
    {
        bool pass = false;

        switch (op)
        {
            case SIMD_FILTER_GT: pass = (values[i] > threshold); break;
            case SIMD_FILTER_LT: pass = (values[i] < threshold); break;
            case SIMD_FILTER_EQ: pass = (values[i] == threshold); break;
            default:             pass = false; break;
        }

        if (pass)
            match_idx[match_count++] = i;
    }
#endif

    return match_count;
}


/*
 * ExecSeqScanSIMD_Simple
 *
 * Specialized SIMD scan for queries like:
 *   SELECT l_orderkey FROM lineitem WHERE l_orderkey < const;
 *
 * Returns a 1-column slot (int4) using ps_ResultTupleSlot.
 */
static TupleTableSlot *
ExecSeqScanSIMD(SeqScanState *node)
{
    EState         *estate   = node->ss.ps.state;
    TableScanDesc   scandesc = node->ss.ss_currentScanDesc;
    ScanDirection   direction = estate->es_direction;
    TupleTableSlot *scanSlot  = node->ss.ss_ScanTupleSlot;
    TupleTableSlot *resultSlot = node->ss.ps.ps_ResultTupleSlot; /* 1-column */

    /* Initialize scan descriptor (like SeqNext) */
    if (scandesc == NULL)
    {
        scandesc = table_beginscan(node->ss.ss_currentRelation,
                                   estate->es_snapshot,
                                   0, NULL);
        node->ss.ss_currentScanDesc = scandesc;
    }

    /* Initialize threshold from Param once per scan, if needed */
    if (node->filter_from_param && !node->simd_threshold_inited)
    {
        update_simd_threshold_from_param(node);
        node->simd_threshold_inited = true;
    }

    for (;;)
    {
        /* 1) Serve pending matches from previous batch */
        if (node->simd_match_pos < node->simd_match_count)
        {
            int idx   = node->simd_match_idx[node->simd_match_pos++];
            int32 val = node->simd_values[idx];

            /* Build a 1-column virtual tuple: (l_orderkey) */
            ExecClearTuple(resultSlot);
            resultSlot->tts_values[0]  = Int32GetDatum(val);
            resultSlot->tts_isnull[0]  = false;
            ExecStoreVirtualTuple(resultSlot);

            return resultSlot;
        }

        /* 2) No pending matches: build a new batch of l_orderkey values */
        node->simd_batch_count = 0;
        node->simd_match_count = 0;
        node->simd_match_pos   = 0;

        while (node->simd_batch_count < node->simd_batch_size)
        {
            bool ok = table_scan_getnextslot(scandesc,
                                             direction,
                                             scanSlot);
            if (!ok)
                break; /* end of relation */

            /* Extract l_orderkey only */
            bool  isnull;
            int   attno = node->filter_attno;   /* 1-based */
            Datum d = slot_getattr(scanSlot, attno, &isnull);

            if (!isnull)
                node->simd_values[node->simd_batch_count] = DatumGetInt32(d);
            else
                node->simd_values[node->simd_batch_count] = 0; /* will fail cmp */

            node->simd_batch_count++;
        }

        /* No more input at all */
        if (node->simd_batch_count == 0)
        {
            return ExecClearTuple(resultSlot); /* EOF */
        }

        /* 3) SIMD filter on the batch of int32 values */
        node->simd_match_count =
            simd_filter_int32(node->simd_values,
                              node->simd_batch_count,
                              node->filter_threshold,
                              node->filter_op,
                              node->simd_match_idx);

        node->simd_match_pos = 0;

        /* If this batch had no matches, loop again and read more tuples */
        if (node->simd_match_count == 0)
            continue;

        /* Otherwise, the next loop iteration will return the first match */
    }
}





static TupleTableSlot *SeqNext(SeqScanState *node);

/* ----------------------------------------------------------------
 *						Scan Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		SeqNext
 *
 *		This is a workhorse for ExecSeqScan
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
SeqNext(SeqScanState *node)
{
	TableScanDesc scandesc;
	EState	   *estate;
	ScanDirection direction;
	TupleTableSlot *slot;

	/*
	 * get information from the estate and scan state
	 */
	scandesc = node->ss.ss_currentScanDesc;
	estate = node->ss.ps.state;
	direction = estate->es_direction;
	slot = node->ss.ss_ScanTupleSlot;

	if (scandesc == NULL)
	{
		/*
		 * We reach here if the scan is not parallel, or if we're serially
		 * executing a scan that was planned to be parallel.
		 */
		scandesc = table_beginscan(node->ss.ss_currentRelation,
								   estate->es_snapshot,
								   0, NULL);
		node->ss.ss_currentScanDesc = scandesc;
	}

	/*
	 * get the next tuple from the table
	 */
	if (table_scan_getnextslot(scandesc, direction, slot))
		return slot;
	return NULL;
}

/*
 * SeqRecheck -- access method routine to recheck a tuple in EvalPlanQual
 */
static bool
SeqRecheck(SeqScanState *node, TupleTableSlot *slot)
{
	/*
	 * Note that unlike IndexScan, SeqScan never use keys in heap_beginscan
	 * (and this is very bad) - so, here we do not check are keys ok or not.
	 */
	return true;
}

/* ----------------------------------------------------------------
 *		ExecSeqScan(node)
 *
 *		Scans the relation sequentially and returns the next qualifying
 *		tuple. This variant is used when there is no es_epq_active, no qual
 *		and no projection.  Passing const-NULLs for these to ExecScanExtended
 *		allows the compiler to eliminate the additional code that would
 *		ordinarily be required for the evaluation of these.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecSeqScan(PlanState *pstate)
{
	SeqScanState *node = castNode(SeqScanState, pstate);

	Assert(pstate->state->es_epq_active == NULL);
	Assert(pstate->qual == NULL);
	Assert(pstate->ps_ProjInfo == NULL);

	return ExecScanExtended(&node->ss,
							(ExecScanAccessMtd) SeqNext,
							(ExecScanRecheckMtd) SeqRecheck,
							NULL,
							NULL,
							NULL);
}

/*
 * Variant of ExecSeqScan() but when qual evaluation is required.
 */
static TupleTableSlot *
ExecSeqScanWithQual(PlanState *pstate)
{
	SeqScanState *node = castNode(SeqScanState, pstate);

	/*
	 * Use pg_assume() for != NULL tests to make the compiler realize no
	 * runtime check for the field is needed in ExecScanExtended().
	 */
	Assert(pstate->state->es_epq_active == NULL);
	pg_assume(pstate->qual != NULL);
	Assert(pstate->ps_ProjInfo == NULL);

	return ExecScanExtended(&node->ss,
							(ExecScanAccessMtd) SeqNext,
							(ExecScanRecheckMtd) SeqRecheck,
							NULL,
							pstate->qual,
							NULL);
}

/*
 * Variant of ExecSeqScan() but when projection is required.
 */
static TupleTableSlot *
ExecSeqScanWithProject(PlanState *pstate)
{
	SeqScanState *node = castNode(SeqScanState, pstate);

	Assert(pstate->state->es_epq_active == NULL);
	Assert(pstate->qual == NULL);
	pg_assume(pstate->ps_ProjInfo != NULL);

	return ExecScanExtended(&node->ss,
							(ExecScanAccessMtd) SeqNext,
							(ExecScanRecheckMtd) SeqRecheck,
							NULL,
							NULL,
							pstate->ps_ProjInfo);
}

/*
 * Variant of ExecSeqScan() but when qual evaluation and projection are
 * required.
 */
static TupleTableSlot *
ExecSeqScanWithQualProject(PlanState *pstate)
{
	SeqScanState *node = castNode(SeqScanState, pstate);

	Assert(pstate->state->es_epq_active == NULL);
	pg_assume(pstate->qual != NULL);
	pg_assume(pstate->ps_ProjInfo != NULL);
	if(node->use_simd_filter)
		return ExecSeqScanSIMD(node);

	return ExecScanExtended(&node->ss,
							(ExecScanAccessMtd) SeqNext,
							(ExecScanRecheckMtd) SeqRecheck,
							NULL,
							pstate->qual,
							pstate->ps_ProjInfo);
}

/*
 * Variant of ExecSeqScan for when EPQ evaluation is required.  We don't
 * bother adding variants of this for with/without qual and projection as
 * EPQ doesn't seem as exciting a case to optimize for.
 */
static TupleTableSlot *
ExecSeqScanEPQ(PlanState *pstate)
{
	SeqScanState *node = castNode(SeqScanState, pstate);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) SeqNext,
					(ExecScanRecheckMtd) SeqRecheck);
}

/* ----------------------------------------------------------------
 *		ExecInitSeqScan
 * ----------------------------------------------------------------
 */
SeqScanState *
ExecInitSeqScan(SeqScan *node, EState *estate, int eflags)
{
	SeqScanState *scanstate;

	/*
	 * Once upon a time it was possible to have an outerPlan of a SeqScan, but
	 * not any more.
	 */
	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * create state structure
	 */
	scanstate = makeNode(SeqScanState);
	scanstate->ss.ps.plan = (Plan *) node;
	scanstate->ss.ps.state = estate;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &scanstate->ss.ps);

	/*
	 * open the scan relation
	 */
	scanstate->ss.ss_currentRelation =
		ExecOpenScanRelation(estate,
							 node->scan.scanrelid,
							 eflags);

	/* and create slot with the appropriate rowtype */
	ExecInitScanTupleSlot(estate, &scanstate->ss,
						  RelationGetDescr(scanstate->ss.ss_currentRelation),
						  table_slot_callbacks(scanstate->ss.ss_currentRelation));

	/*
	 * Initialize result type and projection.
	 */
	ExecInitResultTypeTL(&scanstate->ss.ps);
	ExecAssignScanProjectionInfo(&scanstate->ss);

	/*
	 * initialize child expressions
	 */
	scanstate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) scanstate);

	/*
	 * When EvalPlanQual() is not in use, assign ExecProcNode for this node
	 * based on the presence of qual and projection. Each ExecSeqScan*()
	 * variant is optimized for the specific combination of these conditions.
	 */
	if (scanstate->ss.ps.state->es_epq_active != NULL)
		scanstate->ss.ps.ExecProcNode = ExecSeqScanEPQ;
	else if (scanstate->ss.ps.qual == NULL)
	{
		if (scanstate->ss.ps.ps_ProjInfo == NULL)
			scanstate->ss.ps.ExecProcNode = ExecSeqScan;
		else
			scanstate->ss.ps.ExecProcNode = ExecSeqScanWithProject;
	}
	else
	{
		if (scanstate->ss.ps.ps_ProjInfo == NULL)
			scanstate->ss.ps.ExecProcNode = ExecSeqScanWithQual;
		else
			scanstate->ss.ps.ExecProcNode = ExecSeqScanWithQualProject;
	}
	const char *flag = getenv("PG_FORCE_SIMD_FILTER");
	if (flag != NULL && strcmp(flag, "1") == 0)  // Disable SIMD for the final combine phase
	{
		Plan *plan = scanstate->ss.ps.plan;

		/* Plan quals are a List* of Expr* */
		List *qualList = plan->qual;

		/* We only handle one simple qual: WHERE col OP const/param */
		if (list_length(qualList) != 1){
			elog(NOTICE, "[SIMD] Disabled: Filter");
			scanstate->use_simd_filter = false;
			return scanstate;
		}

		Expr *expr = linitial(qualList);

		
		AttrNumber   attno;
		SimdFilterOp opk;
		bool         is_param;
		int          param_id;
		int32        const_val;

		if (analyze_simd_filter_qual(expr,
									&attno, &opk, &is_param, &param_id, &const_val))
		{
			elog(NOTICE, "[SIMD] Enabled: Filter");
			scanstate->use_simd_filter   = true;
			scanstate->filter_op         = opk;
			scanstate->filter_attno      = attno;
			scanstate->filter_from_param = is_param;
			scanstate->filter_param_id   = param_id;
			scanstate->filter_threshold  = const_val;

			scanstate->simd_batch_size  = 4096;
			scanstate->simd_values      = palloc(sizeof(int32) * scanstate->simd_batch_size);
			scanstate->simd_match_idx   = palloc(sizeof(int)   * scanstate->simd_batch_size);
			scanstate->simd_batch_count = 0;
			scanstate->simd_match_count = 0;
			scanstate->simd_match_pos   = 0;
			scanstate->simd_threshold_inited = false;
		}
		else
		{
			elog(NOTICE, "[SIMD] Disabled: Filter");
			scanstate->use_simd_filter = false;
		}
	}
	else
	{
		elog(NOTICE, "[SIMD] Disabled: Filter");
		scanstate->use_simd_filter = false;
	}
	return scanstate;
}

/* ----------------------------------------------------------------
 *		ExecEndSeqScan
 *
 *		frees any storage allocated through C routines.
 * ----------------------------------------------------------------
 */
void
ExecEndSeqScan(SeqScanState *node)
{
	TableScanDesc scanDesc;

	/*
	 * get information from node
	 */
	scanDesc = node->ss.ss_currentScanDesc;

	/*
	 * close heap scan
	 */
	if (scanDesc != NULL)
		table_endscan(scanDesc);
}

/* ----------------------------------------------------------------
 *						Join Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecReScanSeqScan
 *
 *		Rescans the relation.
 * ----------------------------------------------------------------
 */
void
ExecReScanSeqScan(SeqScanState *node)
{
	TableScanDesc scan;

	scan = node->ss.ss_currentScanDesc;

	if (scan != NULL)
		table_rescan(scan,		/* scan desc */
					 NULL);		/* new scan keys */

	ExecScanReScan((ScanState *) node);
}

/* ----------------------------------------------------------------
 *						Parallel Scan Support
 * ----------------------------------------------------------------
 */

/* ----------------------------------------------------------------
 *		ExecSeqScanEstimate
 *
 *		Compute the amount of space we'll need in the parallel
 *		query DSM, and inform pcxt->estimator about our needs.
 * ----------------------------------------------------------------
 */
void
ExecSeqScanEstimate(SeqScanState *node,
					ParallelContext *pcxt)
{
	EState	   *estate = node->ss.ps.state;

	node->pscan_len = table_parallelscan_estimate(node->ss.ss_currentRelation,
												  estate->es_snapshot);
	shm_toc_estimate_chunk(&pcxt->estimator, node->pscan_len);
	shm_toc_estimate_keys(&pcxt->estimator, 1);
}

/* ----------------------------------------------------------------
 *		ExecSeqScanInitializeDSM
 *
 *		Set up a parallel heap scan descriptor.
 * ----------------------------------------------------------------
 */
void
ExecSeqScanInitializeDSM(SeqScanState *node,
						 ParallelContext *pcxt)
{
	EState	   *estate = node->ss.ps.state;
	ParallelTableScanDesc pscan;

	pscan = shm_toc_allocate(pcxt->toc, node->pscan_len);
	table_parallelscan_initialize(node->ss.ss_currentRelation,
								  pscan,
								  estate->es_snapshot);
	shm_toc_insert(pcxt->toc, node->ss.ps.plan->plan_node_id, pscan);
	node->ss.ss_currentScanDesc =
		table_beginscan_parallel(node->ss.ss_currentRelation, pscan);
}

/* ----------------------------------------------------------------
 *		ExecSeqScanReInitializeDSM
 *
 *		Reset shared state before beginning a fresh scan.
 * ----------------------------------------------------------------
 */
void
ExecSeqScanReInitializeDSM(SeqScanState *node,
						   ParallelContext *pcxt)
{
	ParallelTableScanDesc pscan;

	pscan = node->ss.ss_currentScanDesc->rs_parallel;
	table_parallelscan_reinitialize(node->ss.ss_currentRelation, pscan);
}

/* ----------------------------------------------------------------
 *		ExecSeqScanInitializeWorker
 *
 *		Copy relevant information from TOC into planstate.
 * ----------------------------------------------------------------
 */
void
ExecSeqScanInitializeWorker(SeqScanState *node,
							ParallelWorkerContext *pwcxt)
{
	ParallelTableScanDesc pscan;

	pscan = shm_toc_lookup(pwcxt->toc, node->ss.ps.plan->plan_node_id, false);
	node->ss.ss_currentScanDesc =
		table_beginscan_parallel(node->ss.ss_currentRelation, pscan);
}
