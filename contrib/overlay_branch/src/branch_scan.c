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

        /* Pass 1: MAIN baseline rows, merged with delta side-effects. */
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

            match = NULL;
            foreach(lc, delta_list)
            {
                DeltaTuple *dt = (DeltaTuple *) lfirst(lc);
                if (dt->key != NULL && pk_key != NULL && strcmp(dt->key, pk_key) == 0)
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

        /* Pass 2: pure delta INSERTs (rows created inside the branch
         * that have no MAIN baseline counterpart).  MUST run even when
         * n == 0: otherwise queries like "WHERE pk = <newkey>" fail
         * because MAIN has 0 rows but Pass2 used to be nested inside
         * the (n>0) block. */
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
    /* B1.24 REDUNDANT SAFETY GUARD: always skip BranchScan injection
     * for WRITE-side scan subplans.  The *primary* guard that keeps
     * UPDATE / DELETE / INSERT subplans on raw SeqScan (not CustomScan)
     * is B-1 further down: `if (ct != CMD_SELECT) return;`.  However we
     * additionally check overlay_in_write_redirect() here, so that even if
     * a future refactor accidentally changes B-1, WR subplans are still
     * driven by physical ctid.  Pure-delta DML (UPDATE/DELETE of rows
     * that only exist in the delta table, i.e. INSERTed in-branch) is
     * documented as MVP-out-of-scope until WRITE subplans can run with
     * ExecQual evaluation against reconstructed delta slots — see
     * write_redirect.c "NOTE: pure-delta INSERT rows" comment. */
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
        /* PK-predicate 快速路径：进一步压低 cost 显式鼓励 Planner（即使
         * 我们后面强删 pathlist，成本标记也能保留给 EXPLAIN 看）。 */
        if (has_pk_pred)
            cpath->path.rows = 1.0;
        else
            cpath->path.rows       = rel->rows;
        cpath->path.startup_cost = 0.0;
        cpath->path.total_cost = min_cost * 1.0e-5;
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
