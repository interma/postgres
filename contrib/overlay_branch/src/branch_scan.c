/*-------------------------------------------------------------------------
 *
 * branch_scan.c
 *    Transparent overlay read using PG CustomScan / set_rel_pathlist_hook.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "overlay_branch.h"
#include "branch_scan.h"

#include "access/heapam.h"
#include "access/table.h"
#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "executor/nodeCustom.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodes.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/typcache.h"

static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook = NULL;

static Plan *ob_branchscan_plan_custom_path(PlannerInfo *root,
                                            RelOptInfo *rel,
                                            CustomPath *cpath,
                                            List *tlist,
                                            List *clauses,
                                            List *custom_plans);
static Node *ob_branchscan_create_custom_scan_state(CustomScan *cscan);
static void  ob_branchscan_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *ob_branchscan_exec(CustomScanState *node);
static void  ob_branchscan_end(CustomScanState *node);
static void  ob_branchscan_rescan(CustomScanState *node);

static const CustomPathMethods  ob_branchscan_path_methods = {
    "overlay_branch_branchscan",
    ob_branchscan_plan_custom_path,
    NULL,
};

static const CustomScanMethods  ob_branchscan_scan_methods = {
    "overlay_branch_branchscan",
    ob_branchscan_create_custom_scan_state,
};

static const CustomExecMethods ob_branchscan_exec_methods = {
    .CustomName       = "overlay_branch_branchscan",
    .BeginCustomScan  = ob_branchscan_begin,
    .ExecCustomScan   = ob_branchscan_exec,
    .EndCustomScan    = ob_branchscan_end,
    .ReScanCustomScan = ob_branchscan_rescan,
};

/* ----- ExtendedCustomScanState: embed CustomScanState as 1st field per PG
 * design rule (ExecInitCustomScan nodeCustom.c L36-40).  BranchScanState
 * follows immediately so we can recover it with a simple container_of.
 * The PG-owned CustomScanState.custom_ps field MUST remain NIL (it is
 * a List of child PlanStates for plan nodes with subplans — we have none).
 * Never set custom_ps = private pointer: ExecShutdownNode_walker will
 * try to traverse it as List and SIGSEGV. */
typedef struct ExtendedCustomScanState
{
    CustomScanState css;          /* MUST be first field for IsA/castNode */
    /* our private state follows */
    List         *result_slots;
    ListCell     *cursor;
    Relation      rel;
    TupleDesc     rel_desc;
    Oid           relid;
    int32         branch_id;
    bool          materialized;
} ExtendedCustomScanState;

/* css MUST be a valid CustomScanState* whose embedding is ExtendedCustomScanState.
 * Since css is field offset 0 we could simply cast, but explicit arithmetic
 * makes the container_of pattern visible. */
#define CSS2BS(css)  ((ExtendedCustomScanState *)(css))


/* ================================================================
 * SHARED 2-pass helper (called by BOTH SRF and CustomScan Executor)
 * ================================================================ */
static List *
ob_compute_overlay_slots_internal(Oid relid, int32 branch_id, TupleDesc *out_tupdesc)
{
    Relation    rel;
    TupleDesc   reldesc;
    int         natts;
    List       *delta_list;
    List       *result_slots = NIL;
    const char *q_nspname;
    const char *q_relname;
    char       *q_qualified;
    StringInfoData sql;
    int         ret;

    rel = table_open(relid, AccessShareLock);
    reldesc = RelationGetDescr(rel);
    natts = reldesc->natts;

    delta_list = overlay_delta_list_for_rel(branch_id, relid);

    q_nspname = get_namespace_name(RelationGetNamespace(rel));
    q_relname = RelationGetRelationName(rel);
    q_qualified = quote_qualified_identifier(q_nspname, q_relname);
    initStringInfo(&sql);
    appendStringInfo(&sql, "SELECT * FROM %s", q_qualified);
    pfree(q_qualified);

    /* ----- Guard: run the internal MAIN seqscan *outside* the Planner
     * hook overlay (otherwise infinite recursion: helper → SPI SELECT
     * → Planner hook → CustomScan → helper …).  Use PG_TRY to restore
     * the flag on ANY error path — otherwise a single failed helper
     * call leaves the flag stuck true for the rest of the session and
     * *disables* transparent overlay reads permanently. */
    overlay_overlay_helper_enter();
    PG_TRY();
    {
        ret = ob_spi_one_shot(sql.data, true, 0);
        pfree(sql.data);

        if (ret != SPI_OK_SELECT)
        {
            SPI_finish();
            table_close(rel, AccessShareLock);
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("ob_compute_overlay_slots: SPI main seqscan failed ret=%d", ret)));
        }
    }
    PG_CATCH();
    {
        overlay_overlay_helper_exit();
        PG_RE_THROW();
    }
    PG_END_TRY();
    overlay_overlay_helper_exit();

    if (SPI_processed > 0 && SPI_tuptable != NULL && SPI_tuptable->vals != NULL)
    {
        HeapTuple  *htups = SPI_tuptable->vals;
        TupleDesc   td_spi = SPI_tuptable->tupdesc;
        uint64      n = SPI_processed;
        MemoryContext oldcxt;

        oldcxt = MemoryContextSwitchTo(TopMemoryContext);

        for (uint64 i = 0; i < n; i++)
        {
            HeapTuple        htup = htups[i];
            TupleTableSlot  *main_slot;
            TupleTableSlot  *output_slot;
            char           *pk_key;
            DeltaTuple     *match = NULL;
            ListCell       *lc;
            TupleDesc       slot_desc;

            slot_desc = CreateTupleDescCopy(reldesc);
            main_slot = MakeSingleTupleTableSlot(slot_desc, &TTSOpsVirtual);
            ExecClearTuple(main_slot);

            for (int a = 0; a < natts; a++)
            {
                bool                    isnull;
                Form_pg_attribute       ratt = TupleDescAttr(reldesc, a);
                Datum                   d;

                d = SPI_getbinval(htup, td_spi, a + 1, &isnull);
                main_slot->tts_isnull[a] = isnull;
                if (!isnull)
                {
                    int16   typlen;
                    bool    typbyval;
                    get_typlenbyval(ratt->atttypid, &typlen, &typbyval);
                    d = datumCopy(d, typbyval, typlen);
                }
                main_slot->tts_values[a] = d;
            }
            ExecStoreVirtualTuple(main_slot);
            main_slot->tts_nvalid = natts;

            pk_key = overlay_serialize_pk(rel, main_slot);

            foreach(lc, delta_list)
            {
                DeltaTuple *dt = (DeltaTuple *) lfirst(lc);
                if (strcmp(dt->key, pk_key) == 0)
                {
                    match = dt;
                    break;
                }
            }

            if (match == NULL)
            {
                output_slot = main_slot;
            }
            else if (match->op == DELTA_OP_DELETE)
            {
                ExecDropSingleTupleTableSlot(main_slot);
                match->emitted = true;
                output_slot = NULL;
            }
            else
            {
                if (match->tuple_data == NULL)
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("ob_compute_overlay_slots: delta op=%c key=%s has NULL tuple_data",
                                    match->op, match->key)));
                output_slot = reconstruct_slot_from_delta(rel, match->tuple_data);
                ExecDropSingleTupleTableSlot(main_slot);
                match->emitted = true;
            }
            pfree(pk_key);

            if (output_slot != NULL)
                result_slots = lappend(result_slots, output_slot);
        }

        {
            ListCell *lc;
            foreach(lc, delta_list)
            {
                DeltaTuple *dt = (DeltaTuple *) lfirst(lc);
                if (!dt->emitted && dt->op == DELTA_OP_INSERT)
                {
                    TupleTableSlot *new_slot;
                    if (dt->tuple_data == NULL)
                        ereport(ERROR,
                                (errcode(ERRCODE_DATA_CORRUPTED),
                                 errmsg("ob_compute_overlay_slots: delta INSERT key=%s has NULL tuple_data",
                                        dt->key)));
                    new_slot = reconstruct_slot_from_delta(rel, dt->tuple_data);
                    result_slots = lappend(result_slots, new_slot);
                    dt->emitted = true;
                }
            }
        }

        MemoryContextSwitchTo(oldcxt);
    }
    SPI_finish();

    if (out_tupdesc)
        *out_tupdesc = CreateTupleDescCopy(reldesc);

    table_close(rel, AccessShareLock);
    return result_slots;
}

/* ================================================================
 * Planner integration
 * ================================================================ */
static void
ob_branchscan_planner_hook(PlannerInfo *root, RelOptInfo *rel,
                           Index rti, RangeTblEntry *rte)
{
    if (prev_set_rel_pathlist_hook)
        prev_set_rel_pathlist_hook(root, rel, rti, rte);
    if (overlay_in_apply_operation())
        return;
    /* B1.25: Skip inside Step 4a write-redirection context.
     * The ExecutorRun write-redirection hook (overlay_ExecutorRun) runs
     * its own internal CMD_SELECT queries via SPI to locate WHERE-
     * matching rows for UPDATE / DELETE.  Those are CMD_SELECT (so
     * B-1 below passes them through) but they MUST scan raw MAIN heap
     * via standard nodes (SeqScan / IndexScan), NEVER through the
     * overlay CustomScan — otherwise the write-redir code hard-casts
     * scan states to SeqScanState (wrong offsets = SIGSEGV signal 11).
     * Guard flag is set / cleared around the entire intercept block
     * of overlay_ExecutorRun using PG_TRY / PG_CATCH so it is always
     * restored even on ereport(ERROR). */
    if (overlay_in_write_redirect())
        return;
    /* B1.5: Skip if we are *inside* the shared 2-pass helper itself.
     * CRITICAL for EXPLAIN / EXPLAIN ANALYZE as well as actual SELECT:
     * PG 17's ExecInitCustomScan calls `BeginCustomScan` unconditionally
     * (even for plain EXPLAIN, which needs to initialize every plan node
     * to build the ExplainState tree).  Inside BeginCustomScan we call
     * ob_compute_overlay_slots_internal which does SPI_execute("SELECT *
     * FROM <MAIN>") for Pass1 — if we did not bypass here, that *inner*
     * Planner invocation on the same MAIN relation would ALSO inject a
     * CustomPath, PG would pick it, re-enter ExecInitCustomScan, call
     * BeginCustomScan again → stack overflow in < 1 sec. */
    if (overlay_in_overlay_helper())
        return;

    /* B-1: MVP ONLY inject CustomScan for pure SELECT queries.
     * UPDATE / DELETE / INSERT have their own scan subplans, but
     * Step 4a's write-redirection Executor hook (overlay_ExecutorRun)
     * hard-casts scan state to standard nodes (SeqScanState etc.) to
     * extract PK / tuple locator information.  If we injected a
     * CustomScan here for the WHERE-match scan of an UPDATE, the
     * Executor hook would dereference wrong field offsets and
     * SIGSEGV (signal 11).  Step 4a writes go through the delta
     * table via the ExecutorRun hook regardless of scan plan type,
     * so we lose nothing by skipping CustomScan injection for
     * non-SELECT commands in V1. */
    if (root->parse != NULL && root->parse->type == T_Query)
    {
        CmdType ct = ((Query *) root->parse)->commandType;
        if (ct != CMD_SELECT)
            return;
    }
    if (!overlay_branch_is_active())
        return;
    if (rte->rtekind != RTE_RELATION)
        return;
    if (rte->relkind != RELKIND_RELATION)
        return;
    /* B0: Skip system / overlay_branch catalog schemas */
    {
        Oid            nspoid;
        const char    *nspname;

        nspoid = get_rel_namespace(rte->relid);
        nspname = get_namespace_name(nspoid);
        if (nspname != NULL &&
            (strcmp(nspname, "pg_catalog") == 0 ||
             strcmp(nspname, "information_schema") == 0 ||
             strncmp(nspname, "pg_toast", 8) == 0 ||
             strcmp(nspname, OBSCHEMA) == 0))
            return;
    }
    {
        Relation    reln = table_open(rte->relid, NoLock);
        bool        has_pk = overlay_relation_has_pk(reln);
        table_close(reln, NoLock);
        if (!has_pk)
            return;
    }
    {
        CustomPath *cpath = makeNode(CustomPath);
        Cost        min_cost = 1.0e-6;
        ListCell   *lc;
        foreach(lc, rel->pathlist)
        {
            Path *p = (Path *) lfirst(lc);
            if (p->total_cost < min_cost) min_cost = p->total_cost;
        }
        cpath->path.pathtype   = T_CustomScan;
        cpath->path.parent     = rel;
        cpath->path.pathtarget = rel->reltarget;
        cpath->path.param_info = NULL;
        cpath->path.parallel_aware = false;
        cpath->path.parallel_safe  = false;
        cpath->path.parallel_workers = 0;
        cpath->path.rows       = rel->rows;
        cpath->path.startup_cost = 0.0;
        cpath->path.total_cost = min_cost * 1.0e-5;
        cpath->path.pathkeys   = NIL;
        cpath->flags           = 0;
        cpath->custom_paths    = NIL;
        cpath->custom_restrictinfo = NIL;
        cpath->custom_private  = list_make1_oid(rte->relid);
        cpath->methods         = &ob_branchscan_path_methods;
        add_path(rel, (Path *) cpath);

        /* MVP override: in an active branch, our overlay merge result
         * MUST be used — the MAIN heap rows are only the baseline and
         * do NOT reflect the branch's writes (INSERT/UPDATE/DELETE
         * redirected to the delta table).  If the Planner is allowed
         * to pick an IndexScan or SeqScan on the bare heap (because
         * we only undercut their cost by a 1e-5 multiplicative
         * factor but index pathkeys match ORDER BY etc.), it will
         * silently ignore the delta table and return stale results.
         * Simplest safe fix for MVP: delete every non-CustomPath path
         * from rel->pathlist so our CustomPath is the ONLY candidate
         * (retaining any parallel-aware variants of CustomScan if
         * any — MVP has none).  Later optimizations can whitelist
         * specific MAIN-heap scans for inner uses (e.g. PK lookups
         * from write-redirection helper SELECTs already bypass here
         * via the B1.25 guard above). */
        {
            List     *newlist = NIL;
            ListCell *lc2;
            foreach(lc2, rel->pathlist)
            {
                Path *p = (Path *) lfirst(lc2);
                if (IsA(p, CustomPath))
                    newlist = lappend(newlist, p);
            }
            rel->pathlist = newlist;
        }
    }
}

static Plan *
ob_branchscan_plan_custom_path(PlannerInfo *root, RelOptInfo *rel,
                               CustomPath *cpath, List *tlist,
                               List *clauses, List *custom_plans)
{
    CustomScan *cscan = makeNode(CustomScan);
    Oid         relid = linitial_oid(cpath->custom_private);
    List       *custom_exprs_list;

    (void) clauses;  /* qual is NOT pushed down in MVP: we materialize the
                      * full overlay result set in ExecCustomScan and let PG
                      * wrap our CustomScan node in an upper Filter node to
                      * evaluate WHERE quals.  Otherwise storing raw
                      * RestrictInfo (T_RestrictInfo=315) into
                      * scan.plan.qual would cause "unrecognized node type"
                      * errors when the planner tries to treat them as Exprs
                      * (CustomScan qual expects already-planned expression
                      * nodes, not RestrictInfo wrappers). */

    cscan->scan.scanrelid  = rel->relid;
    cscan->flags           = cpath->flags;
    cscan->custom_plans    = custom_plans;

    /* Strip RestrictInfo wrappers: scan.plan.qual expects plain Exprs,
     * not RestrictInfo nodes (T_RestrictInfo=315 would be reported as
     * "unrecognized node type" by ExecQual).  Baserestrictinfo comes
     * in as clauses parameter; we assign the unwrapped Expr list to
     * scan.plan.qual so ExecQual in ob_branchscan_exec() evaluates
     * WHERE predicates against each materialized merged row.  MVP
     * does not push quals into the 2-pass helper SPI (they run in the
     * Filter-node-like scan-level qual); this is correct but may be
     * optimized later. */
    custom_exprs_list = NIL;
    {
        ListCell *lc;
        foreach(lc, clauses)
        {
            RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
            if (IsA(rinfo, RestrictInfo))
                custom_exprs_list = lappend(custom_exprs_list, copyObject(rinfo->clause));
        }
    }
    cscan->custom_exprs    = NIL;
    cscan->custom_private  = list_make1_oid(relid);
    cscan->custom_scan_tlist = NIL;
    cscan->custom_relids   = bms_make_singleton(rel->relid);
    cscan->methods         = &ob_branchscan_scan_methods;
    cscan->scan.plan.startup_cost = cpath->path.startup_cost;
    cscan->scan.plan.total_cost   = cpath->path.total_cost;
    cscan->scan.plan.plan_rows    = cpath->path.rows;
    cscan->scan.plan.plan_width   = rel->reltarget->width;
    cscan->scan.plan.targetlist   = copyObject(tlist);
    cscan->scan.plan.qual         = custom_exprs_list;
    cscan->scan.plan.lefttree     = NULL;
    cscan->scan.plan.righttree    = NULL;
    cscan->scan.plan.extParam     = NULL;
    cscan->scan.plan.allParam     = NULL;
    return (Plan *) cscan;
}

/* ================================================================
 * Executor integration
 * ================================================================ */
static Node *
ob_branchscan_create_custom_scan_state(CustomScan *cscan)
{
    ExtendedCustomScanState *ebs;

    (void) cscan;
    ebs = (ExtendedCustomScanState *) palloc0(sizeof(ExtendedCustomScanState));
    /* Tag the embedded CustomScanState as T_CustomScanState.  We
     * can't use makeNode() because it allocates exactly
     * sizeof(CustomScanState). */
    NodeSetTag(&ebs->css, T_CustomScanState);
    ebs->css.methods = &ob_branchscan_exec_methods;
    /* slotOps NULL lets ExecInitCustomScan default to TTSOpsVirtual
     * (nodeCustom.c L69-70), which is what we want. */
    ebs->css.slotOps = NULL;
    /* custom_ps starts NIL; no child PlanState subtrees.  Begin fills
     * ExtendedCustomScanState private fields. */
    ebs->css.custom_ps = NIL;
    return (Node *) ebs;
}

List *
ob_compute_overlay_slots(Oid relid, int32 branch_id)
{
    return ob_compute_overlay_slots_internal(relid, branch_id, NULL);
}

static void
ob_branchscan_begin(CustomScanState *node, EState *estate, int eflags)
{
    CustomScan      *cscan = castNode(CustomScan, node->ss.ps.plan);
    ExtendedCustomScanState *ebs = CSS2BS(node);
    Oid              relid;
    int32            branch_id;
    Relation         rel;

    (void) eflags;
    (void) estate;

    relid     = linitial_oid(cscan->custom_private);
    branch_id = overlay_branch_get_current_id();

    /* ----- LAZY MATERIALIZATION -----
     * PG 17's ExecInitCustomScan calls BeginCustomScan unconditionally
     * even for plain EXPLAIN (without ANALYZE) because ExplainState
     * needs every plan node's state structure fully initialized.
     * If we called ob_compute_overlay_slots_internal here, its SPI
     * Pass1 SELECT would re-enter the Planner hook and (despite the
     * B1.5 guard) could still cause stack overflow in some edge
     * cases.  The safe fix is *never* to do heavy work in
     * BeginCustomScan: just open the relation and hold the lock so
     * RelationGetDescr's TupleDesc remains valid (owned by relcache,
     * not the current Portal resource owner — avoids the "tupdesc
     * reference ... is not owned by resource owner Portal" error),
     * and defer the actual 2-pass overlay merge until the FIRST call
     * to ExecCustomScan (which EXPLAIN without ANALYZE never
     * executes). */
    rel = table_open(relid, AccessShareLock);

    /* NOTE: PG ExecInitCustomScan (nodeCustom.c L88) already called
     * ExecInitScanTupleSlot on node->ss using scan_rel's
     * RelationGetDescr, so node->ss.ss_ScanTupleSlot is already set
     * to a fully-initialized TTSOpsVirtual slot with the correct
     * tupdesc.  We MUST NOT overwrite it with ExecAllocTableSlot
     * here — double-init leaks resources and breaks ownership. */
    ebs->result_slots = NIL;
    ebs->cursor       = NULL;
    ebs->rel          = rel;
    ebs->rel_desc     = RelationGetDescr(rel);
    ebs->relid        = relid;
    ebs->branch_id    = branch_id;
    ebs->materialized = false;

    /* custom_ps (PG-owned field) = NIL: no child PlanState subtrees.
     * Never set custom_ps = private pointer; walker in
     * ExecShutdownNode/ExecEndNode treats it as List* and SIGSEGVs. */
    node->custom_ps = NIL;
}

static TupleTableSlot *
ob_branchscan_exec(CustomScanState *node)
{
    ExtendedCustomScanState *ebs = CSS2BS(node);
    TupleTableSlot     *dst = node->ss.ss_ScanTupleSlot;
    TupleTableSlot     *src;
    int                 natts;
    ExprContext        *econtext;

    econtext = node->ss.ps.ps_ExprContext;

    /* ----- First-tuple lazy materialization -----
     * Do the 2-pass overlay merge here, not in BeginCustomScan.
     * This guarantees that plain EXPLAIN (without ANALYZE) — which
     * calls BeginCustomScan but NEVER ExecCustomScan — cannot
     * trigger any SPI or Planner recursion. */
    if (!ebs->materialized)
    {
        TupleDesc  helper_tdesc = NULL;
        List      *slots;

        slots = ob_compute_overlay_slots_internal(ebs->relid,
                                                   ebs->branch_id,
                                                   &helper_tdesc);
        ebs->result_slots = slots;
        ebs->cursor       = list_head(slots);
        if (helper_tdesc != NULL)
            FreeTupleDesc(helper_tdesc);
        ebs->materialized = true;
    }

next_tuple:
    if (ebs->cursor == NULL)
        return NULL;

    src = (TupleTableSlot *) lfirst(ebs->cursor);
    ebs->cursor = lnext(ebs->result_slots, ebs->cursor);

    /* Copy src -> ss.ss_ScanTupleSlot.  dst is already initialized by
     * PG ExecInitCustomScan with the scan relation's physical tupdesc
     * (all columns).  We always copy ALL columns from src into dst
     * regardless of the query's requested projection because the
     * scan-level projection (ProjInfo) is applied below via
     * ExecProject() to produce the plan node's final output slot. */
    ExecClearTuple(dst);
    natts = dst->tts_tupleDescriptor->natts;
    for (int a = 0; a < natts; a++)
    {
        bool                sisnull;
        Datum               sd;
        Form_pg_attribute   dstatt;
        int16               typlen;
        bool                typbyval;

        if (a >= src->tts_tupleDescriptor->natts)
        {
            dst->tts_isnull[a] = true;
            continue;
        }
        sd = slot_getattr(src, (AttrNumber)(a + 1), &sisnull);
        dst->tts_isnull[a] = sisnull;
        if (!sisnull)
        {
            dstatt = TupleDescAttr(dst->tts_tupleDescriptor, a);
            get_typlenbyval(dstatt->atttypid, &typlen, &typbyval);
            sd = datumCopy(sd, typbyval, typlen);
            dst->tts_values[a] = sd;
        }
        else
            dst->tts_values[a] = (Datum) 0;
    }
    ExecStoreVirtualTuple(dst);
    dst->tts_nvalid = natts;

    /* Reset per-tuple expr context so quals / projection have a clean
     * slate for this tuple. */
    ResetExprContext(econtext);
    econtext->ecxt_scantuple = dst;

    /* Run any scan-level qual (MVP: scan.plan.qual=NIL so quals live
     * in an upper Filter node, but check anyway for future proofing). */
    if (node->ss.ps.qual != NULL &&
        !ExecQual(node->ss.ps.qual, econtext))
        goto next_tuple;

    /* Apply projection (scan.plan.targetlist -> ps_ResultTupleSlot).
     * THIS STEP IS NON-OPTIONAL: the PlanState contract says
     * ExecProcNode() must return the *projected* result slot, not
     * the internal scan slot.  For SELECT * the projection collapses
     * to a trivial no-op (identity mapping), but for partial column
     * SELECT (id, color) / CASE WHEN expressions the projection
     * physically reshuffles columns / evaluates expressions into
     * the correct result tupdesc.  Skipping this step produces
     * "unsupported format code: 32638" or "attribute N has wrong
     * type" errors because printtup / upper nodes see the raw
     * 3-column scan slot where they expect the 2-column projected
     * output. */
    if (node->ss.ps.ps_ProjInfo != NULL)
        return ExecProject(node->ss.ps.ps_ProjInfo);
    return dst;
}

static void
ob_branchscan_end(CustomScanState *node)
{
    ExtendedCustomScanState *ebs = CSS2BS(node);
    ListCell           *lc;

    /* If we never materialized (plain EXPLAIN without ANALYZE:
     * BeginCustomScan ran but ExecCustomScan never did), then
     * result_slots is NIL and there are no slots to free. */
    if (ebs->materialized)
    {
        foreach(lc, ebs->result_slots)
        {
            TupleTableSlot *s = (TupleTableSlot *) lfirst(lc);
            ExecDropSingleTupleTableSlot(s);
        }
        list_free(ebs->result_slots);
    }

    /* IMPORTANT: ebs->rel_desc was obtained via RelationGetDescr()
     * in BeginCustomScan — it is owned by relcache, NOT by us, so we
     * MUST NOT call FreeTupleDesc on it (it would double-free). */
    if (ebs->rel != NULL)
        table_close(ebs->rel, AccessShareLock);
    /* ebs itself lives inside ExtendedCustomScanState allocated by
     * CreateCustomScanState (as part of the CustomScanState); PG's
     * ExecEndNode frees the PlanState subtree, so we must NOT
     * pfree(node) or pfree(ebs) ourselves — that would double-free.
     * Just NULL our per-scan owned pointers above; custom_ps remains
     * NIL as set in Begin. */
    node->custom_ps = NIL;
}

static void
ob_branchscan_rescan(CustomScanState *node)
{
    ExtendedCustomScanState *ebs = CSS2BS(node);

    /* If materialized, reset cursor.  If not yet materialized,
     * first ExecCustomScan call will do it anyway. */
    if (ebs->materialized)
        ebs->cursor = list_head(ebs->result_slots);
}

/* ================================================================
 * Module entrypoint (called from overlay_branch.c::_PG_init())
 * ================================================================ */
void
branch_scan_init(void)
{
    prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
    set_rel_pathlist_hook     = ob_branchscan_planner_hook;
    RegisterCustomScanMethods(&ob_branchscan_scan_methods);
}
