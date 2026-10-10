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
#include "access/sysattr.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_operator.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "executor/nodeCustom.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "nodes/primnodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/typcache.h"
#include "utils/syscache.h"
#include "utils/snapmgr.h"

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
static void  ob_branchscan_mark_pos(CustomScanState *node);
static void  ob_branchscan_rest_pos(CustomScanState *node);

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
    .CustomName         = "overlay_branch_branchscan",
    .BeginCustomScan    = ob_branchscan_begin,
    .ExecCustomScan     = ob_branchscan_exec,
    .EndCustomScan      = ob_branchscan_end,
    .ReScanCustomScan   = ob_branchscan_rescan,
    .MarkPosCustomScan  = ob_branchscan_mark_pos,
    .RestrPosCustomScan = ob_branchscan_rest_pos,
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
    ListCell     *mark_cursor;    /* saved position for Mark/Restore */
    Relation      rel;
    TupleDesc     rel_desc;
    Oid           relid;
    int32         branch_id;
    bool          materialized;
    /* P0 PK-IndexScan MVP: 从 Planner hook 透传的快速路径信息 */
    bool          has_pk_pred;
    char         *pk_where_sql;
    char         *pk_serialized_key;
    /* P2 general non-PK qual pushdown: 已 deparse 的 SQL WHERE 片段 */
    bool          has_general_where;
    char         *general_where_sql;
} ExtendedCustomScanState;

/* css MUST be a valid CustomScanState* whose embedding is ExtendedCustomScanState.
 * Since css is field offset 0 we could simply cast, but explicit arithmetic
 * makes the container_of pattern visible. */
#define CSS2BS(css)  ((ExtendedCustomScanState *)(css))


/* ================================================================
 * SHARED 2-pass helper (called by BOTH SRF and CustomScan Executor)
 *
 *   PK-predicate fast-path (P0 MVP):
 *     has_pk_pred = true
 *     pk_where_sql = non-empty SQL fragment like "id = 10::integer"
 *     pk_serialized_key = non-empty JSON array string like '["10"]'
 *   → SPI MAIN 查询用 WHERE 子句（不是 SELECT * FROM table 全表）
 *   → Delta 用 overlay_delta_lookup 精确匹配（不是 list_for_rel 全量）
 *
 *   General non-PK qual pushdown (P2 MVP):
 *     has_general_where = true
 *     general_where_sql = deparse_expression() 产出的一个或多个 AND 子句，
 *                        例如 "color = 'green'::text AND amt < 100::numeric"
 *   → SPI MAIN 查询把这段追加 (PK 和 general 均存在时用 "AND (general)")
 *   → Delta 侧仍走 list_for_rel 全量（非 PK 条件无法按主键 key 索引），
 *     但 MAIN 侧行数已大幅减少，通常 10x~100x 收益。
 * ================================================================ */
static List *
ob_compute_overlay_slots_internal(Oid relid, int32 branch_id, TupleDesc *out_tupdesc,
                                  bool has_pk_pred,
                                  const char *pk_where_sql,
                                  const char *pk_serialized_key,
                                  bool has_general_where,
                                  const char *general_where_sql)
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
    bool        any_where;
    int         ret;

    rel = table_open(relid, AccessShareLock);
    reldesc = RelationGetDescr(rel);
    natts = reldesc->natts;

    /* P0 PK-IndexScan MVP: 选 delta 加载策略 */
    if (has_pk_pred && pk_serialized_key != NULL && pk_serialized_key[0] != '\0')
    {
        DeltaTuple *singleton;

        singleton = (DeltaTuple *) palloc0(sizeof(DeltaTuple));
        if (overlay_delta_lookup(branch_id, relid, pk_serialized_key, singleton))
            delta_list = list_make1(singleton);
        else
        {
            pfree(singleton);
            delta_list = NIL;
        }
    }
    else
        delta_list = overlay_delta_list_for_rel(branch_id, relid);

    q_nspname = get_namespace_name(RelationGetNamespace(rel));
    q_relname = RelationGetRelationName(rel);
    q_qualified = quote_qualified_identifier(q_nspname, q_relname);
    initStringInfo(&sql);
    any_where = false;

    /* Build the SPI SELECT with any combination of pushdown conditions. */
    appendStringInfo(&sql, "SELECT * FROM %s", q_qualified);
    pfree(q_qualified);

    if (has_pk_pred && pk_where_sql != NULL && pk_where_sql[0] != '\0')
    {
        appendStringInfo(&sql, " WHERE (%s)", pk_where_sql);
        any_where = true;
    }
    if (has_general_where && general_where_sql != NULL && general_where_sql[0] != '\0')
    {
        if (any_where)
            appendStringInfo(&sql, " AND (%s)", general_where_sql);
        else
        {
            appendStringInfo(&sql, " WHERE (%s)", general_where_sql);
            any_where = true;
        }
    }

    /* ----- Guard: run the internal MAIN seqscan *outside* the Planner
     * hook overlay (otherwise infinite recursion: helper → SPI SELECT →
     * Planner hook → CustomScan → helper …).  Use PG_TRY to restore the flag
     * on ANY error path — otherwise a single failed helper call leaves the
     * flag stuck true for the rest of the session and *disables*
     * transparent overlay reads permanently.
     *
     * V3 FR5 (T5): Push/Pop Snapshot Balance Rules
     * --------------------------------------------
     * When the branch uses SNAPSHOT mode, the MAIN baseline read must use
     * the frozen TransactionSnapshot captured at use_branch() entry time and
     * stored in ctx->branch_main_snapshot.  We wrap ONLY push it AROUND the
     * single ob_spi_one_shot() call (not longer — no helper-wide push without pop
     * early-return ereport(ERROR) double-pops or imbalance).
     *
     * BALANCE INVARIANT (strict):
     *   • pushed_snap_ = (ctx != NULL && ctx->snap != NULL)
     *   • TRY body: push → SPI → pfree sql → check ret; ONLY ONE pop BEFORE any
     *     control-flow leaves the TRY block.
     *   • CATCH body: pop ONLY if the push was STILL ACTIVE (not yet popped)
     *     by the normal path).  We track this with a separate
     *     `pop_done` flag that both TRY and CATCH can see (declared at
     *     outer block scope so CATCH can read write it).
     *   • helper enter/exit are balanced independently (already handled outermost.  Live mode: zero
     *     overhead (no push pop skipped entirely).                          */
    {
        bool        pushed_snap_;
        bool        pop_done_;

        pushed_snap_ = (CurrentBranchContext != NULL &&
                      CurrentBranchContext->branch_main_snapshot != NULL);
        pop_done_ = false;

        overlay_overlay_helper_enter();
        PG_TRY();
        {
            if (pushed_snap_)
                PushActiveSnapshot(CurrentBranchContext->branch_main_snapshot);
            ret = ob_spi_one_shot(sql.data, true, 0);
            if (pushed_snap_)
            {
                PopActiveSnapshot();
                pop_done_ = true;
            }
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
            if (pushed_snap_ && !pop_done_)
                PopActiveSnapshot();
            overlay_overlay_helper_exit();
            PG_RE_THROW();
        }
        PG_END_TRY();
        overlay_overlay_helper_exit();
    }

    {
        HeapTuple  *htups = NULL;
        TupleDesc   td_spi = NULL;
        uint64      n = 0;
        MemoryContext oldcxt;

        if (SPI_processed > 0 && SPI_tuptable != NULL && SPI_tuptable->vals != NULL)
        {
            htups = SPI_tuptable->vals;
            td_spi = SPI_tuptable->tupdesc;
            n = SPI_processed;
        }

        /* All result slots (from MAIN or delta INSERT) go in
         * TopMemoryContext so they outlive this helper call.  Switch
         * here regardless of whether MAIN has rows (SPI_processed==0
         * case: delta INSERT-only result set still needs correct cxt). */
        oldcxt = MemoryContextSwitchTo(TopMemoryContext);

        /* ================================================================
         * REVIEW-260926 / R11 R12 —  O(N × M) → O(M log M + N log M) merge
         * ================================================================
         * Hot loop in Pass1 used to be:
         *     foreach MAIN i  { serialize pk;  foreach(dt in delta_list) strcmp; }
         * which is exactly O(N × M) comparisons (N = #MAIN rows, M = #delta rows).
         * Under realistic workloads N≈1e5 rows × M≈50k deltas = 5e9 string
         * comparisons which stalls the executor for minutes.
         *
         * Fix: sort delta_list pointers by dt->key (C string strcmp order),
         * collapse to unique keys keeping the LATEST entry (fixes the
         * latent I→U first-match semantic bug from the O(N×M) scan), then
         * use bsearch per MAIN row:
         *   Phase A (copy+sort):          O(M log M)
         *   Phase B (unique latest-wins): O(M + K²)  K = max same-key run.
         *                                  For real-world K ∈ 1..3 this is
         *                                  effectively O(M).  We intentionally
         *                                  do NOT rely on qsort stability.
         *   Pass1 (MAIN merge):           O(N log M_unique)
         *
         * Pass2 also used to use an HTAB keyed by `const char *` +
         * HASH_BLOBS.  That produced WRONG fold behaviour because
         * HASH_BLOBS keysize=sizeof(pointer) hashes the ADDRESS of the
         * key string, not its CONTENTS.  Two different allocations for
         * the same logical key would land in different buckets and the
         * dedup silently failed (multi-op pure-I/U chains emitted
         * duplicate rows).  Pass2 is therefore also rewritten as two
         * pure linear walks (no hash table) over delta_list, with
         * explicit per-key latest-wins bookkeeping; complexity O(M²)
         * worst-case for pathological all-keys-equal input, in practice
         * 1-to-few per-key → O(M).
         *
         * Latest-wins tie-break for same-key multi-op (Pass1 + Pass2):
         *   I→U  → keep U   (Pass1 MAIN merge uses U.tuple_data; Pass2
         *                    pure path reconstructs U, fixes latent I bug).
         *   U→U  → keep last U.
         *   U→D  → keep D   (Pass1 MAIN tombstone; Pass2 drops key).
         *   I→U→D→I (reborn) → keep the final I.
         * The "original delta_list order" (= append chronological by
         * construction inside delta_store.c list_for_rel) is the ground
         * truth for "latest": higher list-index = later. */
        {
            /* ---- SORT / BSEARCH PASS1 HELPERS ---- */
            int            M_total = list_length(delta_list);
            DeltaTuple   **arr = NULL;
            int            i_arr;
            ListCell      *lc;
            /* qsort comparator: DeltaTuple** vs DeltaTuple**.
             * Prefer overlay_typed_pk_cmp when BOTH sides have non-NULL
             * typed_pk (R14: preserves PG native type semantics for
             * BPCHAR / NUMERIC / TIMESTAMPTZ etc.); fall back to legacy
             * strcmp(dt->key) whenever either side lacks a typed key so
             * the comparator is a total order regardless of populate
             * state.  NULL keys (shouldn't happen; defensive) sort to
             * the end. */
            int cmp_dtp_by_cstr(const void *a, const void *b)
            {
                DeltaTuple *const *da = (DeltaTuple *const *) a;
                DeltaTuple *const *db = (DeltaTuple *const *) b;
                DeltaTuple *dta = (da) ? *da : NULL;
                DeltaTuple *dtb = (db) ? *db : NULL;
                const char *ka = (dta) ? dta->key : NULL;
                const char *kb = (dtb) ? dtb->key : NULL;
                if (ka == NULL && kb == NULL) return 0;
                if (ka == NULL) return +1;
                if (kb == NULL) return -1;
                if (dta != NULL && dtb != NULL &&
                    dta->typed_pk_n > 0 && dtb->typed_pk_n > 0 &&
                    dta->typed_pk != NULL && dtb->typed_pk != NULL)
                    return overlay_typed_pk_cmp(dta->typed_pk, dta->typed_pk_n,
                                                dtb->typed_pk, dtb->typed_pk_n);
                return strcmp(ka, kb);
            }
            /* Search key bundle: carries BOTH the typed pk (preferred)
             * and legacy text pk (fallback) for a single MAIN row.  Used
             * as the bsearch "key" pointer so the fixed-signature
             * comparator receives everything it needs without any
             * per-call global state. */
            typedef struct SearchKeyBundle
            {
                TypedKey   *typed_pk;      /* NULL if MAIN-side build failed */
                int         typed_n;       /* columns in typed_pk, 0 if invalid */
                const char *text_key;      /* serialised JSON text (non-NULL) */
            } SearchKeyBundle;
            /* bsearch comparator: const SearchKeyBundle* (search key) vs
             * DeltaTuple** (array element).  Typed comparison is used
             * whenever BOTH sides carry a populated typed pk; otherwise
             * falls back to strcmp on the legacy text key.  NULL safety
             * everywhere because comparator is used with qsort/bsearch
             * from libc (we cannot guarantee non-NULL pointers on all
             * paths for all libc implementations). */
            int cmp_searchkey_vs_dtp(const void *vkey, const void *velem)
            {
                const SearchKeyBundle *skb = (const SearchKeyBundle *) vkey;
                DeltaTuple *const *pdt = (DeltaTuple *const *) velem;
                DeltaTuple       *dt  = (pdt) ? *pdt : NULL;
                const char       *ka;
                const char       *kd;
                if (skb == NULL && dt == NULL) return 0;
                if (skb == NULL) return +1;
                if (dt == NULL) return -1;
                if (skb->typed_pk != NULL && skb->typed_n > 0 &&
                    dt->typed_pk != NULL && dt->typed_pk_n > 0)
                    return overlay_typed_pk_cmp(skb->typed_pk, skb->typed_n,
                                                dt->typed_pk, dt->typed_pk_n);
                ka = skb->text_key;
                kd = dt->key;
                if (ka == NULL && kd == NULL) return 0;
                if (ka == NULL) return +1;
                if (kd == NULL) return -1;
                return strcmp(ka, kd);
            }

            if (M_total > 0)
            {
                arr = (DeltaTuple **) palloc(sizeof(DeltaTuple*) * (M_total + 1));
                i_arr = 0;
                foreach(lc, delta_list)
                {
                    arr[i_arr++] = (DeltaTuple *) lfirst(lc);
                }
                /* ---- PRE-FLIGHT: lazy-populate typed_pk for every dt
                 * in the working array whose typed_pk_n is still -1.
                 * We do this ONCE here (before qsort) instead of
                 * repeatedly inside the comparators / post-sort /
                 * Pass2, because the helper needs Relation `rel`
                 * (available in this helper scope) and may internally
                 * invoke SPI (cheap amortised once per dt → O(M) vs
                 * O(M log M) × N bsearch lookups if done per compare).
                 * Build failures (no PK / relation changed) are left
                 * as typed_pk_n==-1 → every comparator above falls
                 * back to legacy strcmp gracefully. */
                {
                    int pfi;
                    for (pfi = 0; pfi < M_total; pfi++)
                    {
                        DeltaTuple *dtp = arr[pfi];
                        if (dtp != NULL && dtp->typed_pk_n < 0 && dtp->key != NULL)
                        {
                            int       nout = 0;
                            TypedKey *out  = NULL;
                            out = overlay_build_typed_pk_from_key_text(rel, dtp->key, &nout);
                            if (out != NULL && nout > 0)
                            {
                                dtp->typed_pk   = out;
                                dtp->typed_pk_n = nout;
                            }
                            else
                            {
                                dtp->typed_pk   = NULL;
                                dtp->typed_pk_n = 0;
                            }
                        }
                    }
                }
                qsort(arr, M_total, sizeof(DeltaTuple*), cmp_dtp_by_cstr);

                /* ---- POST-SORT LATEST-WINS DEDUP (per key) ----
                 * Runs of identical keys are contiguous after qsort.  For
                 * each run we pick the entry with the HIGHEST original
                 * position in delta_list (highest pos = latest by
                 * chronological append → correct semantic regardless of
                 * whether qsort was stable).
                 *
                 * R14: key-equality test uses overlay_typed_pk_cmp when
                 * BOTH sides carry a typed pk; otherwise falls back to
                 * strcmp on the legacy text key.  This fixes BPCHAR
                 * r-trim / NUMERIC precision / TIMESTAMPTZ runs where
                 * qsort had already clustered the semantically equal
                 * entries but strcmp would slice a run in two. */
                {
                    int w = 0;
                    int r = 0;
                    while (r < M_total)
                    {
                        int         rr = r + 1;
                        DeltaTuple *dtr = arr[r];
                        while (rr < M_total)
                        {
                            DeltaTuple *dtrr = arr[rr];
                            bool        eq;
                            if (dtr == NULL || dtrr == NULL)
                            {
                                if (dtr == NULL && dtrr == NULL) eq = true;
                                else                              eq = false;
                            }
                            else if (dtr->typed_pk_n > 0 && dtrr->typed_pk_n > 0 &&
                                     dtr->typed_pk != NULL && dtrr->typed_pk != NULL)
                            {
                                eq = (0 == overlay_typed_pk_cmp(dtr->typed_pk, dtr->typed_pk_n,
                                                                dtrr->typed_pk, dtrr->typed_pk_n));
                            }
                            else if (dtr->key == NULL || dtrr->key == NULL)
                            {
                                eq = (dtr->key == dtrr->key);
                            }
                            else
                            {
                                eq = (0 == strcmp(dtr->key, dtrr->key));
                            }
                            if (!eq) break;
                            rr++;
                        }
                        /* Scan run arr[r..rr-1] → find one with max original pos. */
                        {
                            int     best_in_run = r;
                            int64   best_pos    = -1;
                            int     j;
                            for (j = r; j < rr; j++)
                            {
                                ListCell   *lc2;
                                int         idx = 0;
                                foreach(lc2, delta_list)
                                {
                                    if (((DeltaTuple*) lfirst(lc2)) == arr[j])
                                        break;
                                    idx++;
                                }
                                /* idx==M_total if not found (impossible;
                                 * we built arr from delta_list). */
                                if (idx > best_pos) { best_pos = idx; best_in_run = j; }
                            }
                            arr[w++] = arr[best_in_run];
                        }
                        r = rr;
                    }
                    M_total = w;   /* shrunk: M_unique ≤ M_total */
                }
            }

        /* Pass 1: MAIN baseline rows, merged with delta side-effects. */
        for (uint64 i = 0; i < n; i++)
        {
            HeapTuple        htup = htups[i];
            TupleTableSlot  *main_slot;
            TupleTableSlot  *output_slot;
            char           *pk_key;
            DeltaTuple     *match = NULL;
            TupleDesc       slot_desc;
            TypedKey       *main_typed_pk;
            int             main_typed_n;
            SearchKeyBundle skb;

            slot_desc = CreateTupleDescCopy(reldesc);
            main_slot = MakeSingleTupleTableSlot(slot_desc, &TTSOpsVirtual);
            ExecClearTuple(main_slot);
            main_typed_pk = NULL;
            main_typed_n  = 0;

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
            main_typed_pk = overlay_build_typed_pk_from_slot(rel, main_slot,
                                                              &main_typed_n);

            match = NULL;
            if (pk_key != NULL && arr != NULL && M_total > 0)
            {
                DeltaTuple  **found;
                skb.typed_pk = main_typed_pk;
                skb.typed_n  = main_typed_n;
                skb.text_key = pk_key;
                found = (DeltaTuple **) bsearch(&skb, arr, M_total,
                                                sizeof(DeltaTuple*),
                                                cmp_searchkey_vs_dtp);
                if (found != NULL) match = *found;
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
            if (main_typed_pk != NULL && main_typed_n > 0)
            {
                int            tpk_i;
                for (tpk_i = 0; tpk_i < main_typed_n; tpk_i++)
                {
                    TypedKey *mtk = &main_typed_pk[tpk_i];
                    if (!mtk->isnull && !mtk->typbyval && mtk->value != (Datum) 0)
                        pfree(DatumGetPointer(mtk->value));
                }
                pfree(main_typed_pk);
            }
            pfree(pk_key);

            if (output_slot != NULL)
                result_slots = lappend(result_slots, output_slot);
        }
            if (arr != NULL) pfree(arr);
        }   /* end sort+bsearch + latest-wins Pass1 merge block */

        /* Pass 2: pure delta INSERTs (rows created inside the branch
         * that have no MAIN baseline counterpart).  MUST run even when
         * n == 0: otherwise queries like "WHERE pk = <newkey>" fail
         * because MAIN has 0 rows but Pass2 used to be nested inside
         * the (n>0) block.
         *
         * Pure-delta multi-op fold: a single PK may have multiple
         * pending delta entries (e.g. INSERT→UPDATE, or INSERT→UPDATE
         * →DELETE, or UPDATE→UPDATE).  Without folding, Pass2 would
         * emit duplicate rows because each op's DeltaTuple satisfies
         * the `!emitted && (I||U)` guard independently.
         *
         * Collapse rules (same PK, preserve relative order):
         *   - Any chain that ends in DELETE   → skip (row dies).
         *   - Otherwise                        → use the LAST delta that
         *     is not DELETE (either INSERT or UPDATE — reconstruct its
         *     tuple_data as the final post-image).
         *
         * Implementation (pure linear walks, NO hash tables — avoids
         * the earlier "hash on pointer-address" semantic bug):
         *   Phase 2a (forward scan): build parallel int[] final_idx of
         *     length ndelta.  For each un-emitted key find the LATEST
         *     (highest index ≥ i) delta entry that shares its key.
         *     Mark all lower duplicates final_idx[i] = -1.
         *   Phase 2b (forward scan): for i with final_idx[i] == i:
         *     op == I||U → reconstruct; op == D → total skip. */
        {
            bool        *emit_flags;
            int          i, ndelta;
            ListCell    *lc;

            ndelta = list_length(delta_list);
            if (ndelta == 0) goto pass2_done;
            emit_flags = (bool*) palloc0(sizeof(bool) * ndelta);

            /* Phase 2a: for each delta entry, decide whether we are the
             * latest for our key. */
            {
                int    *final_idx_latest;
                int     cur;
                final_idx_latest = (int *) palloc0(sizeof(int) * ndelta);
                for (i = 0; i < ndelta; i++) final_idx_latest[i] = -1;

                cur = 0;
                foreach(lc, delta_list)
                {
                    DeltaTuple *dt_i = (DeltaTuple *) lfirst(lc);
                    int         latest_j = cur;   /* assume self=latest */
                    ListCell   *lc2;
                    int         j;
                    if (dt_i->emitted)            /* matched Pass1; skip */
                    {
                        cur++;
                        continue;
                    }
                    /* Linear scan to find the rightmost same-key unemitted. */
                    j = 0;
                    foreach(lc2, delta_list)
                    {
                        DeltaTuple *dt_j = (DeltaTuple *) lfirst(lc2);
                        bool        same_key;
                        same_key = false;
                        if (j > cur
                            && !dt_j->emitted
                            && dt_i->key != NULL
                            && dt_j->key != NULL)
                        {
                            if (dt_i->typed_pk_n > 0 && dt_j->typed_pk_n > 0 &&
                                dt_i->typed_pk != NULL && dt_j->typed_pk != NULL)
                                same_key = (0 == overlay_typed_pk_cmp(
                                                dt_i->typed_pk, dt_i->typed_pk_n,
                                                dt_j->typed_pk, dt_j->typed_pk_n));
                            else
                                same_key = (0 == strcmp(dt_i->key, dt_j->key));
                        }
                        if (same_key)
                        {
                            latest_j = j;
                        }
                        j++;
                    }
                    final_idx_latest[cur] = (cur == latest_j) ? cur : -1;
                    cur++;
                }
                /* Translate final_idx_latest[] → emit_flags (only for surviving) */
                for (i = 0; i < ndelta; i++)
                {
                    ListCell   *lc3 = list_nth_cell(delta_list, i);
                    DeltaTuple *dt = (DeltaTuple *) lfirst(lc3);
                    if (final_idx_latest[i] == i
                        && !dt->emitted
                        && (dt->op == DELTA_OP_INSERT || dt->op == DELTA_OP_UPDATE))
                    {
                        emit_flags[i] = true;
                    }
                }
                pfree(final_idx_latest);
            }

            /* Phase 2b: reconstruct only the emit_flags[i]==true rows. */
            i = 0;
            foreach(lc, delta_list)
            {
                DeltaTuple *dt = (DeltaTuple *) lfirst(lc);
                if (emit_flags[i])
                {
                    TupleTableSlot  *new_slot;
                    if (dt->tuple_data == NULL)
                    {
                        i++;
                        continue;   /* defensive: already guarded */
                    }
                    new_slot = reconstruct_slot_from_delta(rel, dt->tuple_data);
                    result_slots = lappend(result_slots, new_slot);
                    dt->emitted = true;
                }
                i++;
            }
            pfree(emit_flags);
pass2_done:
            ;
        }

        MemoryContextSwitchTo(oldcxt);
    }
    SPI_finish();

    /* ================================================================
     * REVIEW-260926 / R13 S07 — TopMemoryContext RSS leak cleanup.
     * ================================================================
     * overlay_delta_list_for_rel allocates every DeltaTuple* (and
     * dt->key, dt->tuple_data payload) in TopMemoryContext (see
     * delta_store.c L413).  Previously those were leaked because the
     * helper returns result_slots (also TopMCxt) and forgets the
     * delta_list; repeated BranchScan calls would grow the session's
     * RSS without bound until the backend exited (= OOM for long
     * sessions).  Free every non-`emitted`? No: we allocated them
     * all via palloc so we must pfree regardless; `emitted` is only
     * a bookkeeping bool for Pass2 duplicate suppression and does
     * NOT imply external ownership.
     *
     * Two allocation paths exist and BOTH must be cleaned up:
     *   (1) PK-singleton fast-path: palloc0(sizeof(DeltaTuple)) at L167
     *       → delta_list == list_make1(singleton) → singleton_free below
     *       also works via lappend deconstruction list_free_deep alternative?
     *       No, we iterate explicit because key/tuple_data are separate
     *       palloc chunks.
     *   (2) General full-list path: overlay_delta_list_for_rel returns
     *       N list nodes; iterate and pfree each field.
     *
     * After this block delta_list is freed to empty list; any hash tables
     * (Pass1 delta_hash, Pass2 final_map) were already destroyed via
     * hash_destroy inside their blocks.  */
    {
        ListCell *lc_r13;
        foreach(lc_r13, delta_list)
        {
            DeltaTuple *dt = (DeltaTuple *) lfirst(lc_r13);
            if (dt == NULL) continue;
            /* key always exists (list_for_rel always pstrdup("") for isnull).
             * tuple_data is NULL for op=DELETE entries; non-NULL for I/U.
             * typed_pk is R14 add-on: allocated lazily in pre-flight, contains
             * typbyval=false datum deep copies so MUST be freed via helper
             * before the DeltaTuple itself is pfree'd (otherwise RSS leak). */
            if (dt->key != NULL) pfree(dt->key);
            if (dt->tuple_data != NULL) pfree(dt->tuple_data);
            if (dt->typed_pk != NULL && dt->typed_pk_n > 0)
                overlay_typed_pk_free(dt->typed_pk, dt->typed_pk_n);
            pfree(dt);
        }
        list_free(delta_list);
    }

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

    /* ================================================================
     * PRE-CHECK LAYER (fast-path exits BEFORE any C function call
     * into branch_lifecycle.c that may itself invoke SPI / planner,
     * which would re-enter this hook → stack overflow.
     *
     * ORDERING RULE (CRITICAL, violation ⇒ infinite recursion SIGABRT):
     *   B0 (schema skip, O(1) relcache)
     *   → B1 (recursion guards: apply / write_redirect / helper)
     *   → B2 (command-type bypass: non-SELECT CMD)
     *   → ONLY THEN call C wrappers is_active/CurrentBranchContext etc.
     *
     * Rationale:
     *   overlay_branch_is_active() (L?) ≝
     *       ob_invalidate_check_throttled(false)  [SPI-execute: SELECT state
     *                                                FROM overlay_branch.pg_branch]
     *       + return ctx->is_active;
     *   If we call is_active() BEFORE filtering RTE's whose namespace ==
     *   OBSCHEMA ("overlay_branch"), the inner SPI query against
     *   pg_branch (which IS in OBSCHEMA but wasn't yet skipped because
     *   B0 was AFTER is_active()) will re-plan → re-enter this hook →
     *   is_active() again → infinite recursion →
     *   "stack depth limit exceeded" in 50-100 calls (2MB stack).
     *   See Section J3 of test/sql/overlay_branch_invalidation.sql
     *   (J3 Bug) for the regression guard that exercises this exact path.
     * ================================================================ */

    /* B0 PRE-CHECK: Skip system / overlay_branch catalog schemas.
     * MUST be the very FIRST per-RTE filter (before ANY C function into
     * the overlay module that could SPI-plan anything). */
    if (rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION)
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

    /* B1 PRE-CHECK: recursion guards for bypass layers */
    if (overlay_in_apply_operation())
        return;
    if (overlay_in_write_redirect())
        return;
    if (overlay_in_overlay_helper())
        return;

    /* ================================================================
     * REVIEW-260926 / R16: MVP guard for ModifyingCTE (Data-Modifying
     *   statements inside WITH clauses).
     *
     * Why THREE separate choke points (belt-and-braces, redundancy by
     * design — silent MAIN pollution from ModCTE is a P0 data
     * corruption bug):
     *   1. [THIS BLOCK — Planner hook, NEW] — catches ALL entry points
     *      including EXPLAIN ANALYZE / prepared / SPI / DO / function
     *      body SQL.  The outer query is CMD_SELECT so the B2 check
     *      below would LIE and return early.  hasModifyingCTE is set
     *      by the rewriter on the Query node exactly when WITH-list
     *      contains I/U/D/MERGE.  We refuse to plan this.
     *   2. [ExecutorRun intercept in write_redirect.c — EXISTING] —
     *      catches at runtime plannedstmt->hasModifyingCTE (the
     *      PlannedStmt copy).  Acts as last line of defense.
     *   3. [PlannedStmt dispatch — not needed; #1/#2 sufficient]
     *
     * Placement:  between B1 and B2, BEFORE the CmdType check.  If we
     * placed this after B2 (`ct != CMD_SELECT -> return`) then a
     * ModCTE whose outer query is SELECT would pass B2 (ct==SELECT
     * ok) and proceed to custom planning → the planned
     * ModifyTable nodes inside CTE subplans run with zero WR
     * protection = write MAIN directly.  NOT acceptable. */
    if (root != NULL && root->parse != NULL &&
        nodeTag(root->parse) == T_Query)
    {
        Query *pq = (Query *) root->parse;
        if (pq->hasModifyingCTE)
        {
            /* Raise 0A000 here in the planner context.  This is
             * intentionally NOT a no-op / skip / elog(DEBUG); if we
             * silently returned without injecting CustomScan the
             * planner would continue, build a plan with ModifyTable
             * inside CTE subplans, and ExecutorRun's outer CmdType is
             * CMD_SELECT → dml_split=false → WR checks do NOT opt-in
             * kickout → the ModifyTable nodes execute against MAIN
             * heap.  Catastrophic drift-write.  So we ERROR HERE,
             * BEFORE any plan shape can be constructed.
             *
             * NOTE: ereport inside a planner hook IS safe (PG core
             * does this all the time for constraint errors during
             * planning; PG_TRY/CATCH in caller handles it). */
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("overlay_branch MVP does not support data-modifying statements inside WITH (CTE) clauses (R16 planner-stage blocked)"),
                     errhint("Rewrite WITH (UPDATE/DELETE/INSERT ... RETURNING) SELECT ... "
                             "using a TEMP TABLE to collect RETURNING rows:\n"
                             "  CREATE TEMP TABLE _r AS UPDATE t SET ... RETURNING ...;\n"
                             "  SELECT * FROM _r; DROP TABLE _r;")));
        }
    }

    /* B2 PRE-CHECK: MVP only inject CustomScan for pure SELECT.
     * (Same logic as before, but now it's here, above the is_active() call
     * to avoid unnecessary helper C entry for non-SELECT statements.) */
    if (root->parse != NULL && root->parse->type == T_Query)
    {
        CmdType ct = ((Query *) root->parse)->commandType;
        if (ct != CMD_SELECT)
            return;
    }

    /* ---- End of pre-check layer; it is now SAFE to call into the
     * overlay module's C helpers because:
     *   (1) any SPI inside the helper would target *another* user table,
     *       not OBSCHEMA — we've already filtered those out,
     *   (2) helper recursion guards are set on their respective internal
     *       helper-enter/exit functions.
     * ---- */

    /* OLD BLOCK L367-431's order (now PROHIBITED, see J3 bug):
     *   if (in_apply) return;            // B1 → ok
     *   if (in_write_redirect) return;   // B1 → ok
     *   if (in_overlay_helper) return;   // B1 → ok
     *   if (ct != SELECT) return;        // B2 → ok
     *   if (!overlay_branch_is_active()) // ← ⚠ CALLED BEFORE B0!
     *       return;                          stack overflow ⇐ B0 was at ~L418
     *   ... B0 ...                        ← too late, recursion already on
     * DO NOT revert to that order without re-running J3 + J threshold=1. */

    /* --- Planner-hook throttle-suppression wrap ---
     * We call the C wrapper overlay_branch_is_active() here, which in turn
     * calls ob_invalidate_check_throttled(for_dml=false).  The throttled()
     * helper, when its heavy SPI check fires, has SIDE-EFFECTS: it runs
     * ob_exit_branch_cleanup() → ctx->is_active=false +
     * SetConfigOption("overlay_branch.current","") + ereport DQL NOTICE.
     *
     * That is the CORRECT behaviour during *executor* processing (a plain
     * SELECT that discovers its branch is stale MUST revert and announce
     * it).  It is the WRONG behaviour during planner processing, for a
     * non-SELECT CmdType whose plan we will not even generate a
     * CustomScan for anyway (B2 returns immediately after).  The ordering
     * race that kills D7 if we don't wrap this:
     *
     *   D6: manual catalog flip state=applied
     *   D6b: SET threshold=1 (protected by ProcessUtility whitelist; safe)
     *   D7: UPDATE t_inv SET v = ... WHERE pk=11
     *     → planner hook runs for t_inv RTE (UPDATE target)
     *       B0 (schema public: NOT protected) → B1 ok → B2 non-SELECT →
     *       (B2 already returns; fine.  BUT: if the CmdType were SELECT,
     *        or if B2 were somehow after the is_active() call, the wrap
     *        below is still safe: a stale-branch SELECT plan should NOT
     *        mutate user state in the *planner*, only during *executor*.)
     *     → executor_run_intercept runs with dml_split=true, is_active()
     *       was NOT prematurely cleared by the planner, throttled(true)
     *       runs heavy SPI → DML split ERROR → D7 PASS.
     *
     * Concretely: set ob_in_planner_hook around the C call.  Inside
     * throttled(), when this flag is set and the heavy SPI check would
     * kick out, we jump straight to returning `false` / `true` with NO
     * side-effects (no ctx cleanup, no GUC reset, no NOTICE) and let the
     * subsequent executor-stage call do the real enforcement.
     *
     * Scope of the flag: it wraps ONLY the is_active() call, because:
     *   - everything before (B0/B1/B2) is pure O(1) relcache + guard bools
     *     and cannot SPI-plan anything;
     *   - everything AFTER the is_active() call (CustomPath construction)
     *     does NOT call back into throttled/is_active helpers, so keeping
     *     the flag live there would be pointless and add unnecessary
     *     recursion-guard depth.
     */
    {
        bool active_snapshot;

        ob_in_planner_hook = true;
        active_snapshot = overlay_branch_is_active();
        ob_in_planner_hook = false;
        if (!active_snapshot)
            return;
    }
    if (rte->rtekind != RTE_RELATION)
        return;
    if (rte->relkind != RELKIND_RELATION)
        return;
    /* OLD B0 block WAS HERE (L418-431, after is_active).
     * Removed to avoid double-schema-check; B0 has been moved above. */
    {
        /* 把 reln 的生命周期扩大到整个 CustomPath 构建块：先前置的
         * has_pk 检查 + 新增的 PK 等值条件识别都要访问 relcache。 */
        Relation    reln = table_open(rte->relid, NoLock);
        CustomPath *cpath;
        Cost        min_cost = 1.0e-6;
        ListCell   *lc;
        /* P0: PK-IndexScan MVP — 尝试从 baserestrictinfo 识别
         * 单列 PK = Const 等值条件。命中则快速路径：SPI MAIN 查用
         * WHERE pk=val，delta 用 overlay_delta_lookup（O(1)）。
         * custom_private 传递协议（List 长度 6）：
         *   [0] String  → rte->relid 十进制字符串（不再解析，仅留兼容性占位）
         *   [1] Integer → has_pk_pred_flag (0 或 1)
         *   [2] String  → pk_where_sql_cstr（空串当 flag=0）
         *   [3] String  → serialized_pk_key（空串当 flag=0）
         *   [4] Integer → has_general_where_flag (0 或 1, P2 non-PK pushdown)
         *   [5] String  → general_where_sql（deparse_expression 的输出，空串当 flag=0）
         * 读取端：先拿 RT-index→rte→relid 的标准路径，再按 index 解包。 */
        AttrNumber  pk_attno = 0;
        const char *pk_colname = NULL;
        bool        has_pk_pred = false;
        char       *pk_where_sql = NULL;
        char       *pk_serialized_key = NULL;
        bool        has_general_where = false;
        char       *general_where_sql = NULL;

        if (!overlay_relation_has_pk(reln))
        {
            table_close(reln, NoLock);
            return;
        }

        if (overlay_get_pk_single_attno(reln, &pk_attno, &pk_colname))
        {
            ListCell   *rc;
            foreach(rc, rel->baserestrictinfo)
            {
                RestrictInfo *rinfo = (RestrictInfo *) lfirst(rc);
                OpExpr      *opexpr;
                Node        *left, *right;
                Var         *var;
                Const       *con;
                Oid         eqop;
                bool        var_on_left;

                if (!IsA(rinfo, RestrictInfo))
                    continue;
                if (!IsA(rinfo->clause, OpExpr))
                    continue;
                opexpr = (OpExpr *) rinfo->clause;
                if (list_length(opexpr->args) != 2)
                    continue;
                left  = (Node *) linitial(opexpr->args);
                right = (Node *) lsecond(opexpr->args);

                /* 识别 Var-Const 或 Const-Var 形式 */
                if (IsA(left, Var) && IsA(right, Const))
                {
                    var = (Var *) left;
                    con = (Const *) right;
                    var_on_left = true;
                }
                else if (IsA(left, Const) && IsA(right, Var))
                {
                    con = (Const *) left;
                    var = (Var *) right;
                    var_on_left = false;
                }
                else
                    continue;

                /* Var 必须指向本 rel 的 PK 列 */
                if (var->varno != rti)
                    continue;
                if (var->varattno != pk_attno)
                    continue;
                if (var->varlevelsup != 0)
                    continue;

                /* 操作符必须是该 PK 类型的 btree 等值操作符（=）。
                 * 用 typcache 拿该类型的默认 btree eq op OID。 */
                {
                    TypeCacheEntry *tcache;

                    tcache = lookup_type_cache(var->vartype,
                                               TYPECACHE_EQ_OPR);
                    eqop = tcache ? tcache->eq_opr : InvalidOid;
                }
                if (!OidIsValid(eqop))
                    continue;
                if (var_on_left)
                {
                    if (opexpr->opno != eqop)
                        continue;
                }
                else
                {
                    /* commuted: 检查 commutator op */
                    Oid commut = get_commutator(opexpr->opno);
                    if (commut != eqop)
                        continue;
                }

                /* 命中：构造 pk_where_sql 片段 + 序列化 key。
                 * V2 策略（见 doc/p0_pk_oidx_deparse_strategy.md §3）：
                 *   直接把整个 OpExpr(Var(PK)=Const) 节点交给
                 *   ruleutils 的 deparse_expression()，而不是手动按 PK
                 *   列的 type 调 output function。这样：
                 *   1) 字面量的 consttype 和实际输出函数严格对应（避免
                 *      int4 Datum 当 int8 解读 → garbage SQL）；
                 *   2) format_type_be static buffer 覆写问题不存在
                 *      （deparse_expression 内部用 palloc 生成 cast）；
                 *   3) 自定义类型的 output function ERROR 用 PG_TRY
                 *      捕获后安全回退（has_pk_pred=false）。 */
                {
                    List       *dpctx;
                    char       *deparsed;

                    dpctx = deparse_context_for(
                                RelationGetRelationName(reln),
                                rte->relid);
                    deparsed = NULL;
                    PG_TRY();
                    {
                        deparsed = deparse_expression(
                                       (Node *) opexpr,
                                       dpctx,
                                       false,
                                       false);
                    }
                    PG_CATCH();
                    {
                        FlushErrorState();
                        deparsed = NULL;
                    }
                    PG_END_TRY();

                    if (deparsed != NULL && *deparsed != '\0' &&
                        strlen(deparsed) < 65536)
                    {
                        pk_where_sql = pstrdup(deparsed);
                    }
                    else
                    {
                        /* deparse 失败 → P0 回退，不消费这条谓词，
                         * P2 会把它当作普通 qual 纳入 general_where。 */
                        has_pk_pred = false;
                        pk_where_sql = NULL;
                        pk_serialized_key = NULL;
                        continue;
                    }

                    pk_serialized_key = overlay_serialize_pk_from_single_datum(
                        reln, pk_attno, con->constvalue,
                        con->constisnull, con->consttype);
                }
                has_pk_pred = true;
                break; /* 第一个命中的 PK 等值条件即可 */
            }
        }

        /* P2 general (non-PK) qual pushdown: 收集 baserestrictinfo 中
         * 不是 PK=Const 自身的剩余 clauses，用 AND 组合后 deparse 成
         * SQL 文本透传到 Executor，MAIN 的 SPI SELECT 直接用它
         * 过滤（减少 MAIN 侧从磁盘拉回的行数）。
         * 回退策略：deparse 报错、包含不可反编译节点（Param 等）或
         * 结果为空串 → has_general_where=false，Main 保持原查询。 */
        if (rel->baserestrictinfo != NIL)
        {
            List       *remain = NIL;
            ListCell   *rc;

            foreach(rc, rel->baserestrictinfo)
            {
                RestrictInfo *rinfo = (RestrictInfo *) lfirst(rc);
                Node        *clause;
                bool        is_pk_clause = false;

                if (!IsA(rinfo, RestrictInfo)) continue;
                clause = (Node *) rinfo->clause;

                if (has_pk_pred && IsA(clause, OpExpr))
                {
                    OpExpr *ope = (OpExpr *) clause;
                    if (list_length(ope->args) == 2 &&
                        pk_attno != 0 && pk_colname != NULL)
                    {
                        Node       *ln = (Node *) linitial(ope->args);
                        Node       *rn = (Node *) lsecond(ope->args);
                        Var        *v = NULL;

                        if (IsA(ln, Var) && IsA(rn, Const))
                            v = (Var *) ln;
                        else if (IsA(ln, Const) && IsA(rn, Var))
                            v = (Var *) rn;
                        if (v != NULL &&
                            v->varno == rti &&
                            v->varattno == pk_attno &&
                            v->varlevelsup == 0)
                        {
                            TypeCacheEntry *tc;
                            Oid            eqop;
                            tc = lookup_type_cache(v->vartype, TYPECACHE_EQ_OPR);
                            eqop = tc ? tc->eq_opr : InvalidOid;
                            if (OidIsValid(eqop) &&
                                (ope->opno == eqop ||
                                 get_commutator(ope->opno) == eqop))
                            {
                                is_pk_clause = true;
                            }
                        }
                    }
                }
                if (!is_pk_clause)
                    remain = lappend(remain, clause);
            }
            if (remain != NIL)
            {
                Node       *top;
                List       *dpctx;
                char       *deparsed;

                top = (Node *) ((list_length(remain) == 1)
                                    ? (Node *) linitial(remain)
                                    : (Node *) makeBoolExpr(AND_EXPR,
                                                            remain,
                                                            -1));
                dpctx = deparse_context_for(RelationGetRelationName(reln), rte->relid);
                PG_TRY();
                {
                    deparsed = deparse_expression(top, dpctx, false, false);
                }
                PG_CATCH();
                {
                    FlushErrorState();
                    deparsed = NULL;
                }
                PG_END_TRY();
                if (deparsed != NULL && *deparsed != '\0' &&
                    strlen(deparsed) < 65536)
                {
                    has_general_where = true;
                    general_where_sql = pstrdup(deparsed);
                }
            }
        }

        cpath = makeNode(CustomPath);
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

        /* =====================================================================
         * REVIEW-260926 / A8 — cost_branchscan() MVP-level cost model
         * ---------------------------------------------------------------
         * Background (R11/R12 review):
         *   Pass1 MAIN merge loop used to be O(N×M) naive double-scan.
         *   After R11 fix: Phase A = copy + qsort → O(M log M)
         *                   Phase B = unique latest-wins → O(M) in practice
         *                   Pass1 = MAIN merge × bsearch → O(N log M_unique)
         *                   Pass2 = delta_list 2 linear walks → O(M) in practice
         *   Total complexity = O(M log M + N log M).
         *
         * Planner stage constraint (MVP):
         *   M (#delta rows for current_bid + relid) is NOT trivially known
         *   here (planner has no direct SPI access to pg_branch_delta w/o
         *   a heavy call).  We therefore estimate M using a realistic
         *   "editing branch" ratio: typical in-branch rewrite ratio for a
         *   100% acceptance MVP ∈ [1%, 10%] of MAIN rows.  We use a middle
         *   estimate of 5% (= 0.05), clamped to a sane interval to avoid
         *   pathological estimates for very small/large tables.
         *
         *   M = clamp( rel->tuples × 0.05 , 1, 1000000 )
         *
         * Cost decomposition (follows PG cost.c conventions):
         *   startup_cost = Phase A + Phase B
         *                = cpu_tuple_cost × (M × log2(M+1))        [sort]
         *                + cpu_tuple_cost × M                       [dedup]
         *   run_cost     = Pass1 + Pass2
         *                = cpu_tuple_cost × (N × log2(M+1))        [bsearch]
         *                + cpu_tuple_cost × N                       [MAIN emit]
         *                + cpu_tuple_cost × M                       [Pass2 linear]
         *   total_cost   = startup_cost + run_cost
         *
         *   PK-pred fast path (override):
         *     If has_pk_pred=true, Pass1 bsearch lookup is 1× not N×, so
         *     we squash N→1 in run_cost.  Rows = 1.
         *
         * Safety / fallback:
         *   If the resulting total_cost would be LARGER than the cheapest
         *   native path (unlikely, given our 5% M ratio is conservative),
         *   we still keep our computed cost (truthful, EXPLAIN output is
         *   correct for DBA inspection) — MVP correctness does NOT depend
         *   on planner choosing us, because the L1350-1360 block below
         *   forcibly deletes all non-CustomPath paths from rel->pathlist
         *   in active-branch mode (BranchScan becomes the ONLY candidate).
         *
         * Historical note (was):
         *   cpath->path.startup_cost = 0.0;
         *   cpath->path.total_cost   = min_cost * 1.0e-5; (artificially zero)
         * =================================================================== */
        {
            double      N = (rel->tuples > 0) ? rel->tuples : 100.0;
            double      M_est_raw = N * 0.05;
            double      M = (M_est_raw < 1.0) ? 1.0 :
                            (M_est_raw > 1000000.0) ? 1000000.0 : M_est_raw;
            double      log_M = 1.0;
            double      startup, run, total;

            if (M > 1.0)
            {
                double  ln_M = 0.0;
                double  m_tmp = M;
                while (m_tmp > 1.0) { ln_M += 1.0; m_tmp /= 2.0; }
                log_M = ln_M;
            }

            startup = cpu_tuple_cost * (M * log_M + M);

            if (has_pk_pred)
            {
                run = cpu_tuple_cost * (1.0 * log_M + 1.0)       /* Pass1: 1 bsearch */
                    + cpu_tuple_cost * M;                        /* Pass2 linear */
            }
            else
            {
                run = cpu_tuple_cost * (N * log_M + N)           /* Pass1: N bsearch */
                    + cpu_tuple_cost * M;                        /* Pass2 linear */
            }
            total = startup + run;

            cpath->path.startup_cost = startup;
            cpath->path.total_cost   = total;

            if (has_pk_pred)
                cpath->path.rows = 1.0;
            else
                cpath->path.rows = rel->rows;
        }
        cpath->path.pathkeys   = NIL;
        cpath->flags           = 0;
        cpath->custom_paths    = NIL;
        cpath->custom_restrictinfo = NIL;

        /* 打包 custom_private（统一 Integer/String 包装）。
         * OID 用十进制字符串传递（PG List 的 Integer cell 是 int32，
         * 大 OID >INT32_MAX 会截断成负数，Executor 端 open rel 失败）。
         * custom_private 扩展到 6 元素保持向后兼容：任何元素缺失
         * Executor 端都安全回退到相应的全量路径。 */
        {
            List       *cpriv = NIL;
            int         pkflag;
            int         gwflag;
            char       *oid_cstr;
            pkflag = has_pk_pred ? 1 : 0;
            gwflag = has_general_where ? 1 : 0;
            oid_cstr = psprintf("%u", (unsigned) rte->relid);
            /* CRITICAL: makeString() stores the passed char* pointer
             * BY REFERENCE — it does NOT copy.  We MUST NOT pfree()
             * the strings we hand to makeString(), otherwise the
             * String* nodes inside custom_private end up pointing to
             * freed/reused memory (→ Executor reads garbage bytes
             * like 0x0111 or random pointers).
             * Strategy: always pstrdup() on the way in, then free our
             * locals as usual.  copyObject() (which happens when
             * CustomScan plan is deep-copied) then takes its own
             * deep copies of String nodes via pstrdup in
             * _copyString. */
            cpriv = lappend(cpriv, makeString(pstrdup(oid_cstr)));
            pfree(oid_cstr);
            cpriv = lappend(cpriv, makeInteger(pkflag));
            if (pkflag && (pk_where_sql == NULL || *pk_where_sql == '\0'))
            {
                pkflag = 0;
                has_pk_pred = false;
                if (pk_serialized_key) { pfree(pk_serialized_key); pk_serialized_key = NULL; }
                /* Replace the Integer cell we just appended. */
                list_nth_cell(cpriv, 1)->ptr_value = (void *)(intptr_t) makeInteger(0);
            }
            cpriv = lappend(cpriv,
                            makeString(pstrdup(
                                (has_pk_pred && pk_where_sql) ? pk_where_sql : "")));
            cpriv = lappend(cpriv,
                            makeString(pstrdup(
                                (has_pk_pred && pk_serialized_key) ? pk_serialized_key : "")));
            elog(DEBUG2, "P0 DEBUG: pkflag=%d where=%s key_prefix=%.*s; gwflag=%d general=%s",
                 pkflag,
                 (has_pk_pred && pk_where_sql) ? pk_where_sql : "(empty)",
                 (has_pk_pred && pk_serialized_key) ? 16 : 0,
                 (has_pk_pred && pk_serialized_key) ? pk_serialized_key : "(null)",
                 gwflag,
                 (has_general_where && general_where_sql) ? general_where_sql : "(empty)");
            cpriv = lappend(cpriv, makeInteger(gwflag));
            cpriv = lappend(cpriv,
                            makeString(pstrdup(
                                (has_general_where && general_where_sql) ? general_where_sql : "")));
            cpath->custom_private = cpriv;
        }
        if (pk_where_sql)
            pfree(pk_where_sql);
        if (pk_serialized_key)
            pfree(pk_serialized_key);
        if (general_where_sql)
            pfree(general_where_sql);

        cpath->methods         = &ob_branchscan_path_methods;
        /* =================================================================
         * CRITICAL FIX (A8 regression): do NOT use add_path() here!
         * ---------------------------------------------------------------
         * Standard add_path() performs dominance/cheaper checks: if our
         * CustomPath's total_cost is HIGHER than an existing SeqScan/
         * IndexScan (which becomes true the moment we report a realistic
         * non-zero cost model instead of the old 0.00 1e-5 artificial
         * floor), add_path() SILENTLY REJECTS our cpath and returns NULL
         * without appending it to rel->pathlist.
         *
         * Then the MVP pathlist-prune block below finds newlist=NIL
         * (no CustomPath exists) and falls back to keeping the
         * original SeqScan/IndexScan-only pathlist → the entire overlay
         * delta merge is BYPASSED (silent correctness bug: UPDATEs/
         * DELETEs/INSERTs of the branch silently don't show up in reads).
         *
         * MVP contract = in active branch, BranchScan is the ONLY
         * correct scan (MAIN heap's bare rows are just the baseline).
         * We must therefore FORCE our cpath into the pathlist regardless
         * of Planner cost comparisons, then delete all native competitors.
         * Direct lappend bypasses add_path() entirely; the prune step
         * ensures only ours survives.
         * ================================================================= */
        rel->pathlist = lappend(rel->pathlist, cpath);

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
            /* If our CustomPath is the only candidate (normal active-branch
             * case, at least one SeqScan/IndexScan existed to seed pathlist),
             * strip everything else.  If newlist is EMPTY (e.g. PG Planner
             * already reduced baserel to a single dummy Result path with
             * rows=0, as happens for `WHERE id = NULL` or
             * `WHERE id IS NULL` on a NOT-NULL PK), KEEP the original
             * pathlist.  A zero-row dummy plan is correct (no rows can
             * possibly match, so no need for BranchScan injection), and
             * otherwise `pathlist = NIL` leads to a fatal
             * "could not devise a query plan for the given query" error. */
            if (newlist != NIL)
                rel->pathlist = newlist;
        }
        table_close(reln, NoLock);
    }
}

static Plan *
ob_branchscan_plan_custom_path(PlannerInfo *root, RelOptInfo *rel,
                               CustomPath *cpath, List *tlist,
                               List *clauses, List *custom_plans)
{
    CustomScan *cscan = makeNode(CustomScan);
    /* P0: custom_private 4 元素协议：
     *   [0] String  → OID 十进制字符串（strtoul 解析，避免大 OID int32 截断）
     *   [1] Integer → has_pk_pred_flag (0/1)
     *   [2] String  → pk_where_sql_cstr（可能空串）
     *   [3] String  → serialized_pk_key（可能空串）
     * 直接透传到 cscan，不做结构转换。 */
    const char *relid_cstr = strVal(linitial(cpath->custom_private));
    Oid         relid = (Oid) strtoul(relid_cstr, NULL, 10);
    List       *custom_exprs_list;

    (void) root;
    (void) relid;    /* relid OID 已经在 custom_private[0] 中透传，此处仅解包检查 */
    (void) clauses;  /* qual is NOT pushed down in MVP: we materialize the
                      * full overlay result set in ExecCustomScan and let PG
                      * wrap our CustomScan node in an upper Filter node to
                      * evaluate WHERE quals.  For PK-predicate fast-path:
                      * Planner 已经识别 PK=Const 并传递了 pk_where_sql；
                      * 非 PK 条件仍在 scan.plan.qual 中运行。
                      * 注意：clauses 中可能包含已被 PK 条件吸收的
                      * RestrictInfo，仍需解包挂载到 scan.plan.qual ——
                      * ExecQual 对 AND-条件评估是幂等的，重复评估
                      * 一次 PK=Const 不影响正确性（只是多一次比较）。*/

    cscan->scan.scanrelid  = rel->relid;
    cscan->flags           = cpath->flags;
    cscan->custom_plans    = custom_plans;

    /* Strip RestrictInfo wrappers: scan.plan.qual expects plain Exprs,
     * not RestrictInfo nodes (T_RestrictInfo=315 would be reported as
     * "unrecognized node type" by ExecQual). */
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
    cscan->custom_private  = copyObject(cpath->custom_private);
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
    return ob_compute_overlay_slots_internal(relid, branch_id, NULL,
                                             false, NULL, NULL,
                                             false, NULL);
}

static void
ob_branchscan_begin(CustomScanState *node, EState *estate, int eflags)
{
    CustomScan      *cscan = castNode(CustomScan, node->ss.ps.plan);
    ExtendedCustomScanState *ebs = CSS2BS(node);
    Oid              relid;
    int32            branch_id;
    Relation         rel;
    List            *cpriv;
    int              flag;

    (void) eflags;
    (void) estate;

    /* P0: custom_private 4 元素协议解包。
     * OID 不再从 custom_private[0] 解析（字符串易出错），
     * 直接从 estate 继承 PG 结构拿：RT index → RTE → relid。*/
    cpriv = cscan->custom_private;
    {
        Index           rti = cscan->scan.scanrelid;
        RangeTblEntry  *rte;
        /* scanrelid 是 baserel 的 RT index（从 1 开始编号）。
         * 对 CustomScan 来说 Plan 阶段我们已设置 cscan->scan.scanrelid
         * = rel->relid (RT index)，所以这里一定能拿到。*/
        rte = rt_fetch(rti, estate->es_range_table);
        relid = rte->relid;
    }
    /* P0 PK=Const 快路径解包（见 doc/p0_pk_oidx_deparse_strategy.md §5）。
     * 协议：cpriv[1]=Integer pkflag；cpriv[2]=String pk_where_sql；
     *       cpriv[3]=String pk_serialized_key。
     * 任何 malformed / 空串 → has_pk_pred=false 安全回退到 seqscan
     * （此时 P2 general_where 仍可独立启用，两条路径不耦合）。
     * 注意：**必须用 intVal() 从 Integer 节点取 ival，不能用
     * lsecond_int / list_nth_int** —— 后者直接把 ListCell 存的
     * Integer* 指针截断成 int（64-bit下高位丢失），会导致整个
     * 进程 exit code 2 崩溃。 */
    flag = 0;
    if (list_length(cpriv) >= 2)
    {
        Node *nde = lsecond(cpriv);
        if (nde != NULL && IsA(nde, Integer))
            flag = intVal(nde);
    }
    ebs->has_pk_pred = (flag != 0);
    ebs->pk_where_sql = NULL;
    ebs->pk_serialized_key = NULL;
    if (ebs->has_pk_pred && list_length(cpriv) >= 4)
    {
        Node       *n_w = lthird(cpriv);
        Node       *n_k = lfourth(cpriv);
        const char *wstr = (n_w != NULL && IsA(n_w, String))
                               ? strVal(n_w) : NULL;
        const char *kstr = (n_k != NULL && IsA(n_k, String))
                               ? strVal(n_k) : NULL;

        if (wstr != NULL && *wstr != '\0' && kstr != NULL && *kstr != '\0')
        {
            ebs->pk_where_sql = pstrdup(wstr);
            ebs->pk_serialized_key = pstrdup(kstr);
        }
        else
        {
            /* 任一缺失 → 任一已分配都要 pfree 后回退（不过这里上面
             * 还没分配，直接清零 flag 就行） */
            ebs->pk_where_sql = NULL;
            ebs->pk_serialized_key = NULL;
            ebs->has_pk_pred = false;
        }
    }

    /* P2 general (non-PK) qual pushdown.  Any malformed element = fallback.
     * custom_private[5] is a String node; "" or NULL → skip pushdown. */
    ebs->has_general_where = false;
    ebs->general_where_sql = NULL;
    if (list_length(cpriv) >= 6)
    {
        int             gwflag;
        Node           *nde;
        const char     *gwstr;

        nde = list_nth(cpriv, 4);
        if (nde != NULL && IsA(nde, Integer))
            gwflag = intVal(nde);
        else
            gwflag = 0;
        ebs->has_general_where = (gwflag != 0);
        nde = list_nth(cpriv, 5);
        gwstr = (nde != NULL && IsA(nde, String)) ? strVal(nde) : NULL;
        if (ebs->has_general_where && gwstr != NULL && *gwstr != '\0')
            ebs->general_where_sql = pstrdup(gwstr);
        else
        {
            ebs->general_where_sql = NULL;
            ebs->has_general_where = false;
        }
        elog(DEBUG2, "P2 EXEC UNPACK: gwflag=%d general_where=%s",
             gwflag,
             ebs->general_where_sql ? ebs->general_where_sql : "(null)");
    }

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
                                                   &helper_tdesc,
                                                   ebs->has_pk_pred,
                                                   ebs->pk_where_sql,
                                                   ebs->pk_serialized_key,
                                                   ebs->has_general_where,
                                                   ebs->general_where_sql);
        ebs->result_slots = slots;
    ebs->cursor       = list_head(slots);
    ebs->mark_cursor  = NULL;
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

    /* P0 PK-IndexScan MVP: 释放 Begin 阶段 pstrdup 的字符串 */
    if (ebs->pk_where_sql != NULL)
    {
        pfree(ebs->pk_where_sql);
        ebs->pk_where_sql = NULL;
    }
    if (ebs->pk_serialized_key != NULL)
    {
        pfree(ebs->pk_serialized_key);
        ebs->pk_serialized_key = NULL;
    }
    /* P2 general (non-PK) qual pushdown cleanup */
    if (ebs->general_where_sql != NULL)
    {
        pfree(ebs->general_where_sql);
        ebs->general_where_sql = NULL;
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
ob_branchscan_mark_pos(CustomScanState *node)
{
    ExtendedCustomScanState *ebs = CSS2BS(node);

    ebs->mark_cursor = ebs->cursor;
}

static void
ob_branchscan_rest_pos(CustomScanState *node)
{
    ExtendedCustomScanState *ebs = CSS2BS(node);

    if (ebs->mark_cursor != NULL)
        ebs->cursor = ebs->mark_cursor;
    else
        ebs->cursor = list_head(ebs->result_slots);
}

static void
ob_branchscan_rescan(CustomScanState *node)
{
    ExtendedCustomScanState *ebs = CSS2BS(node);

    (void) ob_invalidate_check_throttled(/*for_dml=*/ false);

    if (!ebs->materialized)
    {
        Assert(ebs->result_slots == NIL);
        ebs->cursor = NULL;
        ebs->mark_cursor = NULL;
        return;
    }

    {
        ListCell *lc;

        foreach(lc, ebs->result_slots)
        {
            TupleTableSlot *slot = (TupleTableSlot *) lfirst(lc);
            ExecDropSingleTupleTableSlot(slot);
        }
        list_free(ebs->result_slots);
        ebs->result_slots = NIL;
    }

    ebs->materialized  = false;
    ebs->cursor        = NULL;
    ebs->mark_cursor   = NULL;
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
