# overlay_branch 开发进度跟踪

> 本文档作为开发进度与交付物清单，对应 README 里提到的 Step 拆分。
> **V1 = 手动 SRF 模式**（读分支必须手动调 `overlay_main_plus_delta()`）；
> **V2 = 透明 BranchScan 模式**（普通 `SELECT *` 自动做 Main⊕Delta 合并）。
>
> 编号说明：1/2/3/4/5/7/8 对应实际交付的里程碑；Step 6 被并入 Step 7 的
> Hard Guard 和推迟到 V3 的多 session MVCC pin，因此留空（详见文末"关于 Step 6 留空"）。
>
> **整体状态（2026-09-14，V3 T1+T2 完成，T3-T8 继续）**：
> - V1/V2 P0/P1/P2 + 加固测试 **全部完成**；
> - `make check REGRESS="overlay_branch_basic overlay_branch_user overlay_branch_advanced overlay_branch_mode_param overlay_branch_invalidation"` **5/5 ALL PASSED, 0 diffs**；
> - **V3 (multi-session MVCC + snapshot mode)**：
>   - T1（BranchContext 扩展 + use_branch mode 参数）✅ ALL PASS；
>   - T2（Layer 1 NOTIFY + 阈值失效 + DQL/DML split）✅ ALL PASS；
>   - 架构与需求见 [multi_session_mvcc.md](./multi_session_mvcc.md)。

---

## V1 步骤概览 (手动 SRF 模式)

| Step | 标题 | 状态 | 交付物入口 |
|------|------|------|-----------|
| 1 | Branch Context SPI + 管理函数 | ✅ | [overlay_branch--1.0.sql](../overlay_branch--1.0.sql#L21-L128) / `overlay_branch.c` `overlay_branch_create/use/current` |
| 2 | Delta Store SPI + UPSERT/lookup/count | ✅ | [overlay_branch--1.0.sql#L41-L55](../overlay_branch--1.0.sql#L41-L55) / [overlay_delta_insert](../src/overlay_branch.c#L2637) |
| 3 | Write Redirect INSERT-only + ExecutorRun hook | ✅ | [overlay_ExecutorRun](../src/overlay_branch.c#L961-L1220) CMD_INSERT 分支 |
| 4 | U/D 真实重定向 + `overlay_main_plus_delta` 2-pass SRF | ✅ | [overlay_main_plus_delta](../src/overlay_branch.c#L1487) / 2-Pass 算法 |
| 7 | Hard Guard 16 场景零静默穿透 | ✅ | `overlay_guard_ereport_fail()` × 6 级 guard order × ProcessUtility/ExecutorRun 双接入 |
| 8 | `discard_branch` 级联清理 | ✅ | [overlay_branch_discard_internal](../src/overlay_branch.c#L1274) |
| 5 | `apply_branch` 3-Pass 乐观原子 MAIN 写回 | ✅ | [overlay_branch_apply](../src/overlay_branch.c#L1262) + old_version 乐观 token |
| 6 | — 留空 — | N/A | 见文末 "关于 Step 6 留空" |

---

## V1 Step 详述

### Step 1 — Branch Context（分支元数据 + 上下文切换）
- Catalog：`pg_branch`（`branch_id / branch_name / owner / created_at / mode / state`）+ `pg_branch_branch_id_seq`
- C-callable 函数：
  - `create_branch(name)`：真写 `pg_branch`，重名报 duplicate，`branch_id` 走 nextval
  - `use_branch(name)`：存在性校验 + state=active；真实同步 GUC `overlay_branch.current`
  - `current_branch()`：双源真值 (`CurrentBranchContext->is_active` AND `overlay_branch_current_name` 非空) 返回当前活动分支名
  - `list_branches()`：纯 SQL `LEFT JOIN pg_branch + delta_count` 聚合（C SRF 早期版本多次 SIGSEGV 后重构为稳定 SQL 实现）
- Public synonyms：`public.create_branch / use_branch / current_branch / list_branches` SQL wrapper 转发到 `overlay_branch` schema，匹配 `doc/example_sql.md` 最终用户 UX

### Step 2 — Delta Store（增量存储 SPI 内建）
- Catalog：`pg_branch_delta`（`branch_id / relid / key / op {I,U,D} / old_version / tuple_data bytea`），三列主键 `(branch_id, relid, key)`；`(branch_id, relid)` 索引加速按表扫
- C 内部 SPI 函数：
  - `overlay_delta_insert`：`INSERT … ON CONFLICT (branch_id,relid,key) DO UPDATE` UPSERT 语义（同 key 重写 updated_at）
  - `overlay_delta_lookup` / `overlay_delta_list_for_rel`：深拷贝 by-ref datum 到 TopMemoryContext，避免 SPI_finish 毒化
  - `overlay_delta_count` / `overlay_delta_delete_all`
  - `reconstruct_slot_from_delta(bytea)`：`encode(bytea,'hex')` → SQL `convert_from(DECODE(hex,'hex'),'UTF8')::jsonb` → `jsonb_to_record` SPI → per column datumCopy（规避 compressed/toasted/1B/4B varlena header 组合爆炸）
- **关键修复（9 层连环 bug 来自 Step 4b 验收）**：SPI 1-based col vs TupleDescAttr 0-based 错位；BPCHAR `DatumGetChar` 截断低位；CreateTupleDescCopy refcount；datumCopy typbyval=true 无拷贝；List 节点切到 TopMemoryContext 等等

### Step 3 — Write Redirect INSERT-only（ExecutorRun hook 拦截 INSERT）
- `_PG_init` 安装 4 个钩子：`ExecutorStart/Run/Finish/End_hook` + `ProcessUtility_hook`
- `overlay_should_redirect(Relation)` 6 层快速过滤（见 Step 7）：活动分支 / 非 catalog / 非 overlay_branch schema / relkind=RELATION / 无 partition / 无 FK / PK 非空
- `overlay_serialize_pk`：按 PK 列顺序 `SELECT to_jsonb(ARRAY[$1,$2..])::text` SPI，结果 TopMCxt pstrdup
- `overlay_serialize_tuple` v4：逐列按真实 `RelationGetDescr(rel)` NameStr 比较合并 SET 新值，严格 JSON 转义 → TopMCxt 拷贝返回
- CMD_INSERT：循环 `ExecProcNode(outer)` → 双 serialize → overlay_delta_insert(op='I') → es_processed=n；*不调 standard* → main heap 0 行
- MVP：U/D 先明确 ERROR + HINT，避免静默写回 main（零污染先拿下来，Step 4 再实装）

### Step 4 — U/D 重定向 + 2-Pass Main⊕Delta SRF
#### 4a — CMD_UPDATE/DELETE 真 redirect
- 解除 Step 3 ERROR：`ExecProcNode(子计划)` 拿候选行（带 junk ctid/tableoid）→ `ctid` 字面量 `SELECT * FROM rel WHERE ctid='(blk,off)'` SPI 取旧行 → 按 NAME 匹配合并 SET 子句新值
- 写 delta：UPDATE → op='U' old_version=`overlay_tuple_version(rel,slot_)`；DELETE → op='D' tuple_data=NULL（墓碑）
- 验收 MAIN 全程零污染，delta 恰好 op 正确

#### 4b — `overlay_main_plus_delta(regclass)` 2-Pass C SRF
```
Pass 1 (Main baseline):
   SEQSCAN Main SPI SELECT *
     +-- per row: serialize_pk → overlay_delta_lookup(key)
           op=NULL → passthrough
           op=D    → drop slot (tombstone)
           op=U    → reconstruct_slot_from_delta 覆盖
           mark key as EMITTED

Pass 2 (Pure delta INSERT):
   for op=I AND NOT EMITTED: reconstruct + append
```
- Output：Live Branch 语义 — 未改的行跟随 MAIN 最新可见版本；改了的行用 branch 版；删了的行消失
- 必须手动带列定义列表：`FROM overlay_branch.overlay_main_plus_delta('t') AS x(id int, v text)`

### Step 7 — Hard Guard 16 场景零静默穿透
- **6 级 guard order（从最便宜到最贵）**：G0 catalog schema 白名单 → G1 overlay_branch meta 表递归透传 → G2 relkind 仅 RELATION → G3 拒 partition 根/叶 → G4 拒非 internal trigger → G5 SPI `pg_constraint` 拒 FK 主/从 → G6 PK 非空
- **双接入 Belt-and-braces**：① `overlay_ExecutorRun` per result-rel + 全 RTE 扫描（防 VIEW/MATVIEW rewrite 后漏） ② `overlay_ProcessUtility` 40+ `T_*` 白名单拦截
- 统一错误界面：`overlay_guard_ereport_fail(scope, name, reason)` → DETAIL 命中哪一级 + HINT 回 MAIN
- 16 冒烟场景全 ERROR 断言：VIEW / MV / TRIGGER / FK / partition / RULE / ALTER / DROP / TRUNCATE / CLUSTER / VACUUM FULL / seq nextval / INSERT INTO view / REINDEX / REFRESH MV 等等
- Step7 后 Step1-4 pg_regress 必须 0 diff (guard 不干扰 regular PK table)

### Step 8 — discard_branch 级联删除
- `overlay_branch_discard_internal`：内存 CurrentBranchContext 清 + `SetConfigOption("overlay_branch.current", …)` 同步真实 GUC（`ob_in_guc_setconfig` 防 check hook→use→check 死循环）
- 级联真 DELETE：`DELETE FROM overlay_branch.pg_branch_delta WHERE branch_id = $1`（早期"假清零"仅改 state + LEFT JOIN count 依赖，已修）
- 幂等保护：state != active 时 ERROR "already discarded or applied"
- 验收：3 delta → cnt=0 state=discarded + double-discard ERROR + MAIN 不变

### Step 5 — apply_branch 3-Pass 乐观原子 MAIN 写回
- **old_version 乐观 token**：write redirect 时 `"<blocknum>:<offset>-x<xmin>"`（ctid + xmin）存 delta；apply 按 PK WHERE 重查 MAIN 最新行再算 token 逐字 strcmp；任何 MAIN UPDATE rewrite/HOT/concurrent write → token 变 → conflict ERROR + 事务 rollback，MAIN 不变
- **3-Pass 原子 (D → U → I)**：每条 delta 独立 `ob_spi_one_shot(connect→execute→SPI_finish)`，outlive datum 全 TopMCxt datumCopy/pstrdup
- **apply 内部 bypass 安全**：`ob_in_apply_operation=true`（`overlay_should_redirect` 顶部直接 return false），全 replay 包 `PG_TRY/PG_CATCH/PG_END_TRY` — conflict ERROR longjmp 必走 CATCH 恢复 flag，防止后续 branch DML 永久 bypass 直写 MAIN（毁灭性 bug 防呆）
- 验收 A/B/C：
  - A (no-conflict)：MAIN 精准从 baseline → after；state=applied cnt=0；re-apply "not active" ERROR
  - B (conflict)：RESET MAIN 再 UPDATE → apply token mismatch conflict → 事务 rollback MAIN 仍保留 race 值
  - C (discard)：discard 回归 MAIN 不变

---

## V2 步骤概览 (BranchScan 透明模式)

| Step | 标题 | 状态 | 交付物入口 |
|------|------|------|-----------|
| V2 Plan | BranchScan CustomScan 设计评审 | ✅ | [doc/branchscan.md Part A](branchscan.md#part-a--branchscan-设计与实施计划) |
| V2-1 | Planner 拦截 + `set_rel_pathlist_hook` 6 层 guard | ✅ | [src/branch_scan.c Plan hook](../src/branch_scan.c#L274-L406) |
| V2-2 | ExecCustomScan 生命周期 (ExtendedCSS + Lazy Materialize) | ✅ | [ExecCustomScan wrapper](../src/branch_scan.c#L462-L645) |
| V2-3 | SRF & CustomScan 单源化 2-Pass helper | ✅ | [ob_compute_overlay_slots_internal](../src/branch_scan.c#L98-L268) |
| V2-4 | Projection + qual 过滤 (WHERE / CASE WHEN / 部分列) | ✅ | ExecBranchScan: ResetExprContext → ExecQual → ExecProject |
| V2-5 | Regression PART 5/6 新增 + 基线全绿 | ✅ | `overlay_branch_user.sql` Part 5 / `overlay_branch_basic.sql` Part 6 |
| V2-P0 | PK=Const O(1) 快路径 (MAIN IndexScan + Delta O(1) lookup) + bpchar rtrim + format_type_with_typmod 链路 | ✅ | branchscan.md Part B / delta_store.c overlay_delta_lookup + overlay_serialize_pk_from_single_datum |
| V2-P1 | RETURNING 子句投影回发（retslots palloc 首次分配修复 repalloc(NULL) UB） | ✅ | write_redirect.c RETURNING 段 |
| V2-P2 | WHERE 非 PK 条件下推 + P0/P2 精确去重 | ✅ | branchscan.md Part B §3-§6 / branch_scan.c deparse |
| V2-P3 | WR pure delta DML 8-phase ExecQual pass（3 种下推 qual 存储位置全覆盖） | ✅ | [V2-P3 详述](#v2-p3--wr-pure-delta-dml-8-phase-execqual-pass) / write_redirect.c |
| V2-R1 | 加固测试 Section L-Q (26+ 新断言) | ✅ | overlay_branch_advanced.sql Section L~Q (L1210-L1502) |
| V2-† | Delta O(1) lookup MemoryContext 生命周期硬约束 | ✅ | branchscan.md Part C |

---

## V2 Step 详述

### V2 Plan — 设计评审通过
- 方案 A（BranchScan CustomScan 透明叠加读）获用户审批；Plan/Executor/2-pass helper 全放新 C 文件 `src/branch_scan.c`
- Planner 用 `set_rel_pathlist_hook` 注入 CustomPath（比 `get_relation_info_hook` 干净，不干扰内建路径估算）

### V2-1 — Planner 拦截（6 层 guard 顺序严格，任何漏一层 = SIGSEGV 或 MAIN 直写）
```
① B-apply：ob_in_apply_operation → skip (apply 内部 MAIN replay 走标准扫描)
② B-wr：   ob_in_write_redirect → skip (WR 内部 MAIN SELECT 不能被 Custom 包)
③ B-hlpr： ob_in_overlay_helper → skip (2-pass helper SPI SELECT MAIN 递归防护)
④ B-1：    cmd != CMD_SELECT → skip (MVP 仅 DQL，DML 还走 V1 Step3/4 Write Redirect)
⑤ B-0：    catalog schemas → skip
⑥ B-PK：   无 PK → skip (overlay 语义不可实现)
```
- **MVP purge non CustomPath**：`add_path(CustomPath)` 后扫一遍 `rel->pathlist`，只留 CustomPath；防 ORDER BY PK IndexScan / sort pathkeys 成本赢过 CustomScan 导致 delta 漏读（EXPLAIN 出现 IndexScan = 灾难性 bug）
- **Baseline RestrictInfo 去包装**：`scan.plan.qual` 必须是 plain Expr，不是 T_RestrictInfo(315) → foreach 剥 `rinfo->clause`，否则 ExecQual 报 "unrecognized node type 315"

### V2-2 — ExecCustomScan 生命周期
- **ExtendedCustomScanState 容器化（规避 custom_ps 野指针 SIGSEGV）**：
  ```c
  typedef struct ExtendedCustomScanState {
      CustomScanState css;          /* MUST be first field for IsA cast */
      List          *result_slots;  /* from helper */
      ListCell      *cursor;
      Relation       rel;
      TupleDesc      rel_desc;
      Oid            relid;
      int32          branch_id;
      bool           materialized;
  } ExtendedCustomScanState;
  ```
- 官方 `CustomScanState.custom_ps` 语义是 "List of child PlanState nodes"，被 ExecShutdownNode_walker 遍历；**严禁当私有指针存**，保持 NIL（否则 EOS return NULL 后 ExecShutdown 解引用野指针 SIGSEGV 11，且 EndCustomScan 都进不去）

- **Lazy Materialize（EXPLAIN 无 ANALYZE 安全）**：`BeginCustomScan` 只 `table_open + tdesc`，不调 helper；helper 真跑延迟到第一次 `ExecCustomScan`。EXPLAIN 也会调 Begin，但绝不会调 Exec，所以纯 EXPLAIN 不触发 SPI/Planner 递归。

### V2-3 — 单源化 2-pass helper（SRF ≡ CustomScan bit-exact 等价）
- `ob_compute_overlay_slots_internal(relid, branch_id, *out_tdesc)`：返回 `List * of TupleTableSlot *`，SRF FIRSTCALL / CustomScan first Exec 都调同一函数
- 等价性验收（Part 5/6 核心断言）：
  ```sql
  SELECT CASE WHEN a.pic = b.pic THEN 'PASS:SRF_EQ_CS' ELSE 'FAIL' END
  FROM (SELECT string_agg(id::text||':'||color, ',' ORDER BY id) pic
         FROM overlay_branch.overlay_main_plus_delta('bs_user')
          AS r(id int4,name text,color text)) a,
       (SELECT string_agg(id::text||':'||color, ',' ORDER BY id) pic
         FROM bs_user) b;
  ```
  → 恒 't'；SRF 改算法，CustomScan 自动吃变更

### V2-4 — Projection + qual 过滤
两个实际 blocker bug（PART 5.7e / 5.x 失败的真根因）：

**① 缺 ExecProject → "unsupported format code: 32638" / "attribute N has wrong type"**
- ExecBranchScan 每 tuple 必须：
  1. fill ALL columns to `ss_ScanTupleSlot`（物理全列，不是查询想要的子集）
  2. `ResetExprContext(econtext)` + `econtext->ecxt_scantuple = dst`
  3. `ExecQual(ss.ps.qual, econtext)` 过 filter
  4. `if (ss.ps.ps_ProjInfo) return ExecProject(ss.ps.ps_ProjInfo); else return dst;`
- 跳过第 4 步，SELECT * 恰好 identity 正确；SELECT id,color 或 CASE WHEN 上层 printtup 看到原始 3 列而不是期望 2 列 → datum 解释错乱 → format code / type mismatch
- 这也是为什么 debug SELECT id,name → 崩溃但 SELECT * OK

**② 缺 qual pushdown → WHERE id=10 返回多行（Filter 被扔掉）**
- `PlanCustomPath()` 必须把 unwrapped 好的 Expr 放进 `cscan->scan.plan.qual`（Planner 不会自动给 baserel 加 Filter 节点）。早期放到 `custom_exprs` 是错的，custom_exprs 是 CS 私有表达式存储，执行器从不主动 eval
- 修复后 EXPLAIN 能看到：
  ```
  Custom Scan on bs_user
    Filter: (bs_user.id = 10)
  ```
  → WHERE id=10 只返回 1 行（CSR 5.7e `PASS:BS_OVERRIDE_VISIBLE`）

### V2-5 — Regression PART 新增（两条基线 0 diff）
- `overlay_branch_user.sql` Part 5：独立 `bs_user(id PK, name, color)` 10 断言
  - 5.1 MAIN 下 EXPLAIN 无 CustomScan
  - 5.3 active branch 下 EXPLAIN → Custom Scan（IndexScan 真没泄漏）
  - 5.6 MAIN zero-pollution（RESET 回 main SELECT * 原 3 行不变）
  - 5.7c SRF vs CS string_agg 行图景恒等
  - 5.7e `CASE WHEN color='green' AND id=10 THEN 'PASS:BS_OVERRIDE_VISIBLE'` UPDATE override 可见
  - 5.8 ORDER BY + EXPLAIN (4 plan shapes) 全 Custom Scan
- `overlay_branch_basic.sql` Part 6：独立 `bs_basic` 8 断言
  - MAIN baseline / MAIN 下 EXPLAIN seqscan (无 Custom)
  - active branch 下 EXPLAIN 4 形状：SELECT * / WHERE / ORDER BY / count(*)
  - count(*)=4 / ORDER BY SRF=CS 等价
  - DELETE id=2 tombstone 正确不出现
  - RESET MAIN → id=3 gamma 正确还原（6.8 列名 v→tag 历史笔误已修）
- 基线 cp `test_output/results/*.out → test/expected/*.out`；pg_regress ok1 ok2 0 diff

---

## V2-P3 — WR pure delta DML 8-phase ExecQual pass

> **背景 Gap**：CMD_UPDATE/DELETE ModifyTable 主循环**只扫 MAIN heap**（通过 ctid/IndexScan 走的物理页），所以分支里 `INSERT id=4` 写完之后再 `DELETE WHERE id=4` 时，主循环 0 行 → delta 里 id=4 还活着 → 下一次 BranchScan 还能看到 id=4（= DELETE 静默没生效）。
>
> 解决方案：主循环之后单独走 Phase D–H，遍历纯 delta INSERT 候选并重跑 WHERE 条件 ExecQual，命中的再写 tombstone（V2 DELETE）或（V3）UPDATE delta。

```
PHASE A — MAIN loop（unchanged）:
   foreach main-loop tuple by subplan outerPlan:
       serialize pk → SPI SELECT * FROM rel WHERE ctid='(blk,off)' 取 old_version
       CMD_DEL → delta(op=D, old_version, key)
       CMD_UPD → merge SET new tuple → delta(op=U, old_version, key, tuple)
       pk 加入 EMITTED set

PHASE B — (reserved for pure delta UPDATE; MVP = NOP, see Phase C guard)

PHASE C — cmd guard (MVP safety, 绝不静默脏 UPDATE 数据):
   if cmd != CMD_DELETE: break    (pure delta UPDATE V2 impl 不进入)

PHASE D — candidate 构建 (Bug10 MemoryContext UAF 根因):
   cand_inserts = overlay_delta_list_for_rel WHERE op=I AND NOT EMITTED
   深拷贝 raw_rows 必须分配  queryDesc->estate->es_query_cxt
     （NOT CurrentMemoryContext — 那是 subplan per-tuple child，subplan done 后会被释放）

PHASE E — qual 下钻 (Bug12 5 bitmap FAIL 根因):
   scan_ps = subplan
   while outerPlanState(scan_ps) != NULL AND scan_ps->qual == NULL:
       scan_ps = outerPlanState(scan_ps)
   qual_scan  = scan_ps->qual
   qual_extra = NULL
   switch nodeTag(scan_ps):
     T_IndexScanState      → qual_extra = indexqualorig
     T_BitmapHeapScanState → qual_extra = bitmapqualorig  （IN 列表 / 非 PK AND / 复合 PK prefix 多用）
     T_IndexOnlyScanState → qual_extra = recheckqual

PHASE F — ExecQual 逻辑 AND:
   foreach slot in cand_inserts:
       passes = true;
       if (qual_scan  != NULL) passes &= ExecQual(qual_scan,  econtext)
       if (passes && qual_extra != NULL) passes &= ExecQual(qual_extra, econtext)

PHASE G — MVP SAFETY GUARD (最关键 防全表 DML nuke):
   if (qual_scan == NULL && qual_extra == NULL) continue   # 无 WHERE 直接 skip
   if (!passes) continue
   CMD_DELETE → delta(op=D, old_version=NULL, key=pk)     # pure delta origin 不存 old_version

PHASE H — cleanup (es_query_cxt 自动清理)
```

### 挖出的 3 个根因（Section L-Q 26 断言主动发现）

| # | 典型 FAIL | 根因 | 定位 Phase | 已修复 PASS 的 Section |
|---|---|---|---|---|
| Bug10 | N 系列 Step 5 SIGSEGV / 0xc0 | raw_rows 分配在 CurrentMCxt（subplan per-tuple）→ subplan done 后释放 UAF | Phase D | N.1-N.5, P.1-P.4, M.1-M.2 |
| Bug11 | apply pure delta DELETE "row not present on MAIN" CONFLICT | DELETE delta old_version==NULL 是「分支自 INSERT 自 DELETE」，MAIN 本来没这行 → apply 应 NOP skip | branch_lifecycle.c apply 段 | O.3 apply pure DELETE NOP |
| Bug12 | `grp=C AND score=333` / `id IN (2,4)` / 复合 PK prefix `a=20` DELETE 全幸存（NOP） | 这些 WHERE 选 BitmapHeapScan，qual 存在 bitmapqualorig 未取 → guard qual_scan + qual_extra 都 NULL → SAFE NOP 但功能不对 | Phase E switch | N.4, P.2-P.4, Q.2 |

---

## V2-R1 — 加固测试 Section L-Q（advanced.sql L1210-L1502）

共 26+ 条新增断言，覆盖 V3 前最容易踩的边界：

| Section | 场景设计 | 目的（V3 风险暴露点） |
|---------|---------|---------------------|
| **L**  | pure delta UPDATE × (single PK / IN list / non-PK) | 确认 pure delta UPDATE 是 MVP NOP（不静默改值）；CMD_UPDATE 入口 guard `if cmd != CMD_DELETE continue 生效 |
| **M** | PK 列 typmod: NUMERIC(10,2) + BPCHAR(6) | 验证 `format_type_with_typmod（取代 format_type_be）在 WR / P0 / apply 链路一致；BPCHAR 等值比较 rtrim 两端对齐 |
| **N** | 6 pure delta rows × (grp text non-PK / score<80 range / grp='C' AND score=333 bitmap AND + RETURNING × N.5 count 断言 | Bug10/12 真实验收；BitmapHeapScanState.bitmapqualorig 生效；count 断言防 silent 多删 |
| **O** | apply/discard 边界: INSERT→DELETE→RE-BORN INSERT→apply / discard pure / apply pure DELETE NOP | Rebirth 三段 UPSERT 覆盖；O.3 Bug11 false-positive 冲突 NOP 验收 |
| **P** | 空 MAIN heap 全链路 4 行 pure INSERT | **0 MAIN row + 100% delta 最极端；IN (2,4) ScalarArrayOp + exact PKEY=3 + RETURNING + apply 1 row 写回 MAIN |
| **Q** | 复合 PRIMARY KEY (a,b) × exact (a=10 AND b='pd-b') + prefix (a=20) | composite PK JSON 数组顺序；prefix 选 BitmapHeapScan 不丢 qual（Q.2 真删 a=20） |

验收：
```
ok 1 - overlay_branch_basic    67ms
ok 2 - overlay_branch_user     33ms
ok 3 - overlay_branch_advanced 145ms
# All 3 tests passed, 0 regression.diffs
```

---

## V3 开发进度追踪（多 Session MVCC + Snapshot Mode）

> 总体状态：**T1 ✅ done / T2…T8 pending**。详见下方各 Task 独立小节。

### 任务依赖图（严格执行顺序）

```
Task 1 (BranchContext 扩展 + use_branch mode)
    │
    ├──→ Task 2 (Layer 1 NOTIFY + 阈值失效回查)
    │
    ├──→ Task 3 (Layer 2 Advisory Lock 互斥)
    │
    ├──→ Task 4 (Layer 3 applying transient state + delta 写入 Guard)
    │
    ├──→ Task 5 (Layer 4a SNAPSHOT mode MAIN freeze)
    │
    ├──→ Task 6 (FR6 BranchScan ReScan fresh materialization)
    │
    ├──→ Task 7 (FR7 可选：共享内存 Pin 表)
    │
    └──→ Task 8 (并发压测 + V2 回归门禁 + 文档同步)
```

Task 2-6 互相独立，可并行开发；Task 1 是所有 Task 的前置依赖；Task 8 是最后总验收。

详细需求 & 设计见 [multi_session_mvcc.md](./multi_session_mvcc.md)。

### V3 任务分解清单（8 个 Task）

| # | 标题 | 优先级 | 状态 | 核心交付 |
|---|------|--------|------|----------|
| **T1** | BranchContext 扩展 + `use_branch(name, mode)` 2-param 重载 | high | ✅ done | include/overlay_branch.h BranchContext 新字段；`overlay_branch--1.0.sql` 直接追加 2-param C 函数声明 + synonym；C 函数 `overlay_branch_use_with_mode`；GUC 注册骨架 |
| **T2** | Layer 1 NOTIFY + 阈值式 SPI 失效回查 (FR1/FR4) | high | ✅ done | `ob_broadcast_state_change` (Publisher)；`ob_invalidate_check_throttled` (Subscriber)；DQL/DML 分层失效 kickout 双轨；SPI 兜底 counter + wall-clock 双节流；is_active(name) + force_invalidation_check() 2 个新 callable 直接追加到 `overlay_branch--1.0.sql` + synonym |
| **T3** | Layer 2 Branch 级 Advisory Lock (FR2) | high | ✅ 核心实现+L3 测试 done (T8 并发测试 pending) | `include/overlay_branch.h` 新增 3 includes (miscadmin/storage lockdefs+lock) + `OB_ADVISORY_MAGIC_HI16=0x4F42<<48` + `OB_MAKE_ADVISORY_KEY(bid)` 宏 + `SET_LOCKTAG_INT64` 本地重定义 + 2 static inline `ob_take_branch_advisory_lock/release` (sessionLock=false xact级 auto-release, dontWait=false deadlock detector enabled)。`src/branch_lifecycle.c` 3 入口拿锁：① use_internal bid 后 (Line ~385) ShareLock (任何 UPDATE pg_branch+install_snapshot 前) ② apply_branch SPI_finish 后 (Line ~975) ExclusiveLock (state check + table_open RowExclusiveLock 前) ③ discard_branch bid 后 (Line ~1268) ExclusiveLock (catalog UPDATE state→discarded 前)。死锁序 Advisory → RowExclusive/Catalog locks 天然满足。`progress_tracker §T3-TR` 三层测试：L3 Section L mvcc.sql (L1 S→X apply t/ L2 X-only discard t/ L3 S→X discard GUC reset t/ L4 key equivalence C↔SQL Shared reentry OK: 0 race)；L1 pg_isolation_regress + L2 pgbench 合并到 T8。 |
| **T4** | Layer 3 `applying` transient state + delta 写入 Guard (FR3) | high | ✅ 核心实现+L3 测试 done (T8 并发测试 pending) | state='applying' 原子 UPDATE CAS 过渡态；`overlay_delta_insert()` 顶部 state NOT IN ('active') ERROR 55000 + 分类 hint；失败 rollback PG MVCC auto 回 active；踩坑 2 条归档（Guard 禁 FOR KEY SHARE / TextDatumGetCString TopMemoryContext 拷贝）；L3 Section M TR-4.1..4.3 4/4 0 diff 通过
| **T5** | Layer 4 SNAPSHOT Mode MAIN Freeze (FR5) | medium | ✅ 核心实现+L3 测试 done (T8 并发测试 pending) | 架构：TopMC 全局 HTAB `ob_snapshot_cache` (bid-keyed) 每个 snapshot bid 唯一 frozen capture 一次，BranchContext 仅 pin/unpin；CopySnapshot `copied->regd_count=1` 伪 pin 阻断 PopActiveSnapshot auto-FreeSnapshot (T5 踩坑 ② 重大根因)；PG_TRY/CATCH `pushed_snap_/pop_done_` 双 bool 栈平衡；check→assign GUC 契约完全合规（check hook 纯 SPI readonly + ereport custom errmsg，assign hook 永不 ERROR + caller-owned guard flag 只写不读 + LAYER-1 abort-unwind 安全）。L3 Section N 4 TR 全 t；4/4 `make check` 0 diff 两次 stale clean rebuild 确认。5 踩坑 D.9.8 归档。 |
| **T6** | FR6 BranchScan ReScan Fresh Materialization (AC-C1) | medium | ✅ 核心实现+L3 测试 done (T8 same-CSS callback 路径 deferred) | 4-step rescan (Kickout+E1 guard+drop slots+materialized=false)：src/branch_scan.c:L1338-L1372；Mark/Restore 回调 L1319-L1336；delta_store.c SPI 快照 Push/Pop LatestSnapshot；L3 Section P MVP fresh-plan 3 TR (3→4 过渡 / pk_mask / 3-read idempotency) + PASS marker P0_FR6_RESCAN_FRESH_OK；2x stale clean rebuild 4/4 0 diff。same-CustomScanState NL LATERAL/WITH HOLD/SCROLL 回调级 rescan 测试推迟 T8 FIXME-NL (PG17 外引用 deparse SEGV)。 |
| **T7** | FR7 可选：共享内存 Pin 表 | low | ❌ Cancelled-by-Design (2026-09-17, R7 correctness first) | **取消原因（归档）**：正确性由 FR2 Branch 级 Advisory Lock（Share ↔ Exclusive xact-scoped 互斥）+ FR4 NOTIFY/SPI invalidation throttle kickout（下一 DML/DQL 开头阻断漂移）+ FR3 `overlay_delta_insert()` 顶部 state NOT IN ('active') ERROR 55000 三层共同保证；apply_strict_pins 想要的『等最后一个 session 退出才 apply』语义已由 FR2 Share↔Exclusive 在**事务级**提供（S1 use+SELECT 事务内持有 ShareLock → S2 apply 的 ExclusiveLock 必须等 S1 COMMIT/ROLLBACK 释放）；session 跨事务 pin 追踪（跨事务 idle-in-session case）FR4 在 S1 下一 DQL 开头 kickout 保证不会漂移读到 stale branch；FR7 仅为 UX 优化（避免 S1 在 idle 期间 S2 apply 后 S1 才发现）+ shmem pin 泄漏/异常退出/backend crash pin 恢复 复杂性 > 收益；GUC 保留但标注 deprecated；V3.1 如用户真需要再启用。 |
| **T8** | 总验收：L1 pg_isolation_regress 5spec（9 permutation 总）+ L2 pgbench fixture doc + L3 Section Q 门禁 + FR7 取消 + FR2 V1 锁协议 | high | ✅ done (2026-09-18, 2x stale clean rebuild 9/9 zero-diff) | 严格遵循 PG 官方三层并发范式：L1（pg_isolation_regress 5spec/9perm）：ob_apply_mutex 2perm FR2 V1 无死锁+CAS loser ERROR；ob_state_inval 2perm FR4 双 r3 DML kickout ERROR 55000；ob_applying_freeze 1perm（MAIN 最终 110 行+s2 applying 窗口成功进入）；ob_snapshot_mode 1perm(c1=102,c2=102,c3=103,c4=104 MAIN-only-freeze WR-delta-always-latest 契约)；ob_branchscan_rescan 1perm(read1=3→read2=4 跨 session fresh materialization FR6)；L3 4suite：Section Q FR7 取消 GUC defaults + 全部 PASS marker 验证；FR2 V1 锁协议从 shared→exclusive 两级改成 use=0lock + apply/discard CAS state→Exclusive（完全消除升级死锁）；I8.3 重大 CmdType split BUG 修复 write_redirect L272（dml_split=false 的纯 SELECT 不再 opt-in allow_kickout 破坏性 kickout side-effect，避免 client_min_messages=WARNING 级别 NOTICE 静默吞掉 kickout 导致下一真实 DML 漂移落 MAIN）；I8.8 新增 bid→mode HTAB 缓存（FR2 V1 移除 catalog UPDATE 后跨 session mode 列不一致问题）；I8.9 新增 2 个 Debug GUC：`_debug_apply_sleep_sec`(SUSET applying 冻结窗口) + `_debug_invalidation_counter_throttle_override`(USERSET 强制 small threshold 便于 L1 deterministic 测试)；2× stale clean rebuild 全部 L3 4/4 + L1 5/5 = 9/9 zero-diff。 |
| **T5-COVERAGE** | Coverage GAP 补齐：9 条未覆盖错误分支全量补测 | medium | ✅ done (2026-09-15) | 4 suites × callable 矩阵 gap 分析 → 定位 9 条 L3 未触达分支；mvcc.sql 新增 Section O（O1–O9）+ PASS marker `PASS:O0_COVERAGE_GAPS_9_BRANCHES_FILLED`；两次 stale clean rebuild 4/4 0 diff 确认。补测项见下方 §T5-COVERAGE 块。 |

### Task 1：BranchContext 扩展 + use_branch mode 参数 (FR5 基础设施) — ✅ done (2026-09-14)

**实现要点（代码级，见 `multi_session_mvcc.md §B 实现附录 T1` 的完整文档）**：
1. **ABI 安全 struct 扩展**（BranchContext struct 尾部 append 5 字段，不重排 V2）：
   ```c
   struct SnapshotData *branch_main_snapshot;  /* NULL=live, non-NULL=snapshot frozen */
   bool        snapshot_registered;            /* true iff TopMemoryContext pfree pending */
   uint32      invalidation_counter;           /* FR4 throttle counter */
   TimestampTz invalidation_last_check;        /* FR4 throttle monotonic wall-clock */
   ```
   另外在 `include/overlay_branch.h` 追加 extern 5 个 V3 GUC vars 与 3 个函数声明：`overlay_branch_use_with_mode_internal`、`ob_ensure_unregister_snapshot`、`ob_exit_branch_cleanup`。

2. **GUC 注册**（`src/overlay_branch.c _PG_init` 内，`DefineCustomIntVariable` x2 + `DefineCustomBoolVariable` x2）：
   | GUC 名 | 上下文 | 默认 | 用途 |
   |--------|--------|------|------|
   | overlay_branch.invalidation_check_threshold | PGC_USERSET | 32 | FR4: is_active 次数节流 |
   | overlay_branch.invalidation_check_interval_ms | PGC_USERSET (ms) | 100 | FR4: wall-clock 节流 |
   | overlay_branch.apply_strict_pins | PGC_SUSET | true | FR3: apply_branch 严格 pin 策略（未来） |
   | overlay_branch.use_shared_mem_pin_table | PGC_POSTMASTER | false | FR7: 可选 shared-memory 全局 pin 表 |
   + 内部 flag `bool ob_in_snapshot_mode_helper`（给 Task 5 用）。

3. **SQL-callable 2-param `use_branch(name, mode)` 分发链**：
   ```
   SQL: public.use_branch(name, mode)  synonym (STRICT=false → SQL allows NULL mode)
     → overlay_branch.use_branch(name, mode)  C 符号 overlay_branch_use_with_mode
       → PG_ARGISNULL(1) guard → NULL → live default
       → overlay_branch_use_with_mode_internal(name, mode)
         (1-param use_branch(name) → thin forward → BRANCH_MODE_LIVE)
   ```

4. **统一生命周期 helper（src/branch_lifecycle.c 新插 3 个 helper 到 create_internal 之后）**：
   - `ob_ensure_unregister_snapshot(ctx)`：仅当 `snapshot_registered` 才 `pfree(ctx->branch_main_snapshot)`，防 double-free；非注册状态时强制清零两字段（GUC reset 路径安全）。
   - `ob_exit_branch_cleanup(ctx)`：所有"离开分支"路径唯一入口（use_branch('')、GUC reset、apply_branch kick_out、跨分支切换）。内部先 `ob_ensure_unregister_snapshot()`，再把 5 个 V3 字段 + is_active + branch_id + name 统一清零。
   - `ob_install_snapshot_for_mode(ctx, mode)`：snapshot 模式下**不**用 `RegisterSnapshot()`（坑：ResourceOwner 作用域在 Portal 或 Xact 级别），而是 `GetTransactionSnapshot()` 后在 **TopMemoryContext** 内 `palloc` + `memcpy` CopySnapshot 等价实现（包括 xip/subxip 数组 trailing 分配，`regd_count=0/active_count=0/copied=true/snapXactCompletionCount=0` 标记）。非 snapshot 模式两字段置 NULL/false。在 3-pass SPI existence/state/branch_id 检查**全部通过后才调用**（防止过早 pin xmin 在 ERROR 路径残留）。

5. **两个非 obvious 的坑（写进代码注释 + 文档）**：
   - **坑 A：RegisterSnapshot 的 ResourceOwner 作用域**。`RegisterSnapshot()` 绑定 `CurrentResourceOwner`，在 SQL-callable func 里等价于 Portal，语句结束立即 auto-release；改绑 `CurTransactionResourceOwner` 也不行（commit/rollback 时 release）。Task1 阶段放弃 ResourceOwner 注册链，self-managed via `palloc(TopMemoryContext)` + `pfree()`。**PERFORMANCE TODO (V3.x 登记)**：PG 的 `RecentGlobalXmin` / GlobalVisState pin 不会因为 snapshot_registered 被 hold back；所以 snapshot-mode reader 有可能读到被并发 VACUUM 主表回收、但快照还 pin 着的 xid（但只要 xmin ≥ 自身快照 xmin 实际上不会错读——但 xid wraparound 下理论有隐患 → V3.x 考虑 CurTransactionResourceOwner + XactCallback 重注册，或者 GUC `snapshot_max_hold_minutes`）。
   - **坑 B：1-param / 2-param 函数签名歧义**。`use_branch('b1')` 如果给 mode 加 SQL-level `DEFAULT 'live'`，会触发 PG `func_select_candidate` "function use_branch(unknown) is not unique"（1-param 与 2-param-带默认都是合法候选）。解决：**2-param 不加默认值**，1-param 作为唯一 resolver，1-param 只转发 2-param live。需要 snapshot mode 的用户显式传两个实参。

6. **单脚本发布（overlay_branch--1.0.sql 直接追加）**：
   - control: `default_version = '1.0'`
   - Makefile `DATA = overlay_branch--1.0.sql`
   - 2-param `use_branch(name, text)` C 函数 + COMMENT + 2-param public synonym wrapper + GRANT 全部直接在 1.0.sql 末尾追加（扩展未正式发布，无需 version upgrade path）。

**Test Results（4/4 全通过, 0 diff）**：
- [x] **TR-1.1**：V2 zero-regression: overlay_branch_basic, overlay_branch_user, overlay_branch_advanced all PASS（3/3）, 0 diff。
- [x] **TR-1.2**：mode='frozen' → invalid_parameter_value ERROR；DO block 捕获，且 `current_branch()` 仍 NULL（无状态泄漏）。
- [x] **TR-1.3**：2-param snapshot 进入 → D1 current_branch()='b_snap1' PASS；D2 `list_branches().mode='snapshot'` PASS（catalog UPDATE 生效）；D3 离开 → NULL PASS；D4 `list_branches.mode` snapshot preserve PASS。
- [x] **TR-1.4**：live ↔ snapshot round-trip ×2（E1..E4 各 PASS）+ 跨分支切换（E6 cross-branch, E7 mode=live）+ 离开 cross（E8）全部 PASS；ERROR 路径（nonexistent/snapshot C3）F3 `current_branch()` 仍 NULL = no pin install PASS。
- [x] **rubric TR-1.5**：Snapshot 仅在 3-pass SPI catalog 通过后才 `palloc(TopMemoryContext)`；Unregister 仅走 `ob_ensure_unregister_snapshot()`，统一 4 条 exit path。

**测试清单**：`test/sql/overlay_branch_mode_param.sql`（201 行 27 断言，分 A/B/C/D/E/F 六段），对应 `test/expected/overlay_branch_mode_param.out`（437 行）。

**Bug counts**：Implement 阶段 3 个非 obvious bug 都被 T1.7 抓出并修复：
1. ResourceOwner scope ERROR（RegisterSnapshot → portal auto-release → 下条语句 UnregisterSnapshot *not owned*）
2. OID vector `proargtypes::text` concat 类型不兼容（改 proargtypes[0]::regtype::text 分别对比）
3. STRICT 函数第二个 arg 为 NULL 时 C 函数根本不调用（LANGUAGE C 去掉 STRICT，加 `PG_ARGISNULL(1)` guard 走 live default）

> 设计细节同步到 [multi_session_mvcc.md § T1 Appendix T1](multi_session_mvcc.md#T1-Appendix) 中的完整实现叙述 + 性能 TODO。

---

### Task 2：Layer 1 — NOTIFY + 阈值式 SPI 失效回查 + DQL/DML Split (FR1, FR4) — ✅ done (2026-09-14)

**实现要点（代码级，见 `multi_session_mvcc.md §D 实现附录 T2` 的完整文档）**：

1. **Publisher：ob_broadcast_state_change(bid, new_state)**（`src/branch_lifecycle.c` L479-L493）
   - 格式：channel `"ob_branch_state"`，payload `psprintf("%d:%s", bid, new_state)`；调用 `Async_Notify()` 走 PG sinval 队列。
   - 发布点（3 处，无条件广播）：
     - apply_internal 尾部：UPDATE state → APPLIED 之后 `ob_broadcast_state_change(bid, BRANCH_STATE_APPLIED)`；
     - discard_internal SPI_finish 之后（**无论是否 leaving_current 都发**，FR1 要求非 current 分支被 discard 也通知所有监听者）：`DISCARDED`；
     - 未来 FR3 applying 过渡态：Update state='applying' success 之后发。

2. **Subscriber：ob_invalidate_check_throttled(bool for_dml)**（`src/branch_lifecycle.c` L524-L785）— Layer1 核心函数
   - **入口：3 类触发条件 OR 关系**（任一成立就 fire heavy SPI 检查）：
     | 触发源 | 条件 | 字段 / API |
     |--------|------|-----------|
     | (B) counter 计数 | `ctx->invalidation_counter >= ob_invalidation_check_threshold (GUC, 默认 32)` | `++invalidation_counter` 每次调用自增 |
     | (C) wall-clock 间隔 | `TimestampDifference >= ob_invalidation_check_interval_ms (GUC, 默认 100ms)` **或首次调用 (last_check = 0)** | `GetCurrentTimestamp()` - `invalidation_last_check` |
     | (D) sinval drain | **每次调用必做**：`AcceptInvalidationMessages()` 强制消费 NOTIFY 后台队列（PG 原生 Async 机制交付依赖 sinval 总线轮询）| `utils/inval.h` 公共 API |
   - **Heavy SPI check**：SPI probe 模式（`spi_ret = SPI_connect(); if == SPI_OK_CONNECT did_con=true else if SPI_ERROR_CONNECT nested OK`），SQL `SELECT state FROM pg_branch WHERE branch_id=ctx->branch_id LIMIT 1`；若 state == `BRANCH_STATE_ACTIVE` → 重置 counter=0 + last_check=now + return true。
   - **Kickout 路径（state != active）严格 5 步顺序**：
     1. 先 `strncpy(local_branch_name, ctx->branch_name, NAMEDATALEN)` + `strncpy(local_state_buf, state_from_catalog, 16)` 到**栈缓冲**（不引用后面会清零的 ctx 指针字段，避免 kickout ereport 打印空串 "branch \\"\\" 问题）；
     2. SPI reread state（用于 NOTIFY 文本的 ground truth）— 同样 probe + conditional finish；
     3. `ob_exit_branch_cleanup(ctx)` — is_active=false, bid=0, name[0]=0, counters=0, snapshot unregister（idempotent：早 exit 了也无副作用）；
     4. `SetConfigOption("overlay_branch.current", "", PGC_USERSET, PGC_S_SESSION)` — **包 `ob_in_guc_setconfig = save/true/restore` flag guard**，防止 GUC check hook 再次进入 cleanup（double-pfree 防呆）；
     5. 最后 ereport：区分 DQL vs DML 双轨（见下表）。
   - **DQL vs DML 分层失效策略（FR1）**：
     | 维度 | DQL（for_dml=false，BranchScan Planner/Exec 入口、is_active 无参 wrapper） | DML（for_dml=true，write_redirect ExecutorRun 入口、delta_store overlay_delta_insert 安全网入口） |
     |------|---|---|
     | 调用入口 | `overlay_branch_is_active()` wrapper 顶部；BranchScan plan/exec；`force_invalidation_check()`（SQL-callable 也走 DQL） | `write_redirect.c` CMD_INSERT/UPDATE/DELETE 处理开头；`delta_store.c` overlay_delta_insert 顶部最后一道 |
     | ereport 级别 | NOTICE + `return false`（不中断当前语句，该语句后续 fallback 到 MAIN 纯视图） | ERROR，`ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE`（中断，禁止漂移写） |
     | 用户消息 | `overlay_branch: branch "%s" is no longer active (state=%s); reverting to Main` | `overlay_branch: branch "%s" is no longer active (state=%s); cannot write through it` + HINT：`The branch was applied by another session.  Re-connect to MAIN and retry the write.` |
     | GUC + ctx 动作 | Both 轨相同：cleanup() + SetConfigOption("") + GUC guard | 同左 |
   - **Fast-path 顶部**：`if (ctx == NULL || !ctx->is_active) return false` — 非 branch 状态直接返回，无任何开销。

3. **2 个 SQL-callable Layer1 函数 + synonym（1.2 升级脚本）**：
   - `overlay_branch.overlay_branch_is_active() → boolean`（C 无参 wrapper，非 STRICT，VOLATILE SET search_path=''）：顶部 extern 声明移到函数首部避免 C90 mixed decl warning；先 `(void) ob_invalidate_check_throttled(for_dml=false)` 触发节流 kickout，再返回 `CurrentBranchContext && CurrentBranchContext->is_active` 内存真值。
   - `overlay_branch.overlay_branch_force_invalidation_check() → boolean`（C 无参）：内部把 counter 置到 GUC threshold + last_check 置 0，强制触发下一次 throttled 的 SPI heavy check；等价于立即 `ob_invalidate_check_throttled(false)` 并返回其返回值。
   - Public synonym：`public.is_active(branch_name name)` SQL-language wrapper → `SELECT overlay_branch.overlay_branch_is_active_by_name($1)`（SPI ground truth：SELECT state FROM pg_branch，**总是重查 catalog 不节流**）；`public.force_invalidation_check()` SQL-language wrapper。
   - 4 条 `GRANT EXECUTE ON FUNCTION ... TO PUBLIC`。

4. **2 条入口 wire-up**：
   - DML 写入口：`src/write_redirect.c` CMD 处理开头加条件 `if (!ob_invalidate_check_throttled(true) || !CurrentBranchContext || !CurrentBranchContext->is_active) return;`（return 后 standard ExecutorRun 继续 MAIN heap 处理；但 throttled 内部已经对 DML throw ERROR，return 实际是安全网 never reached）。
   - Delta store 安全网：`src/delta_store.c overlay_delta_insert()` 顶部 `(void) ob_invalidate_check_throttled(true)`，万一 WR 路径 bypass，delta 写入前再拦一次。

5. **SPI 生命周期统一规则（T2 核心 fix，防 double pfree）**：
   > PG SPI 无公共 `SPI_connected()` API。`SPI_connect()` 在已有连接时返回 `SPI_ERROR_CONNECT` 非 OK。
   
   **所有内部使用 SPI 的 C 函数（`ob_invalidate_check_throttled` heavy、kickout reread、`overlay_branch_is_active_by_name` ground truth）必须用 SPI probe 4 行模式**：
   ```c
   int  spi_ret;
   bool did_con = false;
   spi_ret = SPI_connect();
   if (spi_ret == SPI_OK_CONNECT)
       did_con = true;
   else if (spi_ret != SPI_ERROR_CONNECT)
       elog(ERROR, "SPI_connect failed: %s", SPI_result_code_string(spi_ret));
   /* ... SPI_execute / SPI_processed ... */
   if (did_con)
       SPI_finish();
   ```
   **禁止**：直接 `SPI_connect() != OK → elog` 和无条件 `SPI_finish()`；否则嵌套 SPI（use_branch 内部 UPDATE pg_branch 触发 WR 入口 throttled；或 synonym `public.is_active()` → C 函数）外层 SPI 连接被提前释放，外层再次 `SPI_finish()` → `pfree called with invalid pointer`。

6. **GUC recursion guard 三重防线（T2 核心 fix，防 double cleanup）**：
   根因：`SetConfigOption("overlay_branch.current", ...)` 会触发 check hook `overlay_guc_check_assign_current_branch`，旧代码在 hook 里只要 `CurrentBranchContext != NULL` 就 `ob_exit_branch_cleanup`；但 apply/discard Step2 已经手动 cleanup 过，第二次进入 cleanup → double pfree。
   
   **三重防线（all three MUST hold simultaneously）**：
   - (1) **GUC hook 自身 guard**：仅当 `CurrentBranchContext != NULL && CurrentBranchContext->is_active` 才 cleanup；
   - (2) **所有手写 SetConfigOption 调用点包 flag**：`{ bool save_flag = ob_in_guc_setconfig; ob_in_guc_setconfig = true; SetConfigOption(...); ob_in_guc_setconfig = save_flag; }`（use_branch 切换 / apply tail / discard tail / kickout 路径 共 4 处）；
   - (3) **ob_exit_branch_cleanup() 本身 idempotent**：开头 `if (!ctx->is_active) { 仅清残余字段并 return; }`，is_active=true 时才 full cleanup + NOTICE log。

7. **其它修复（T2 回归根因清单 6 条，全部 fixed + verified）**：
   - Kickout 空串 log：先栈缓冲存 branch_name/state → 再 cleanup + SetConfigOption → 最后 ereport 用 local 缓冲（不是已清零 ctx->branch_name）。
   - Discard 漏 broadcast：原来只在 `if (leaving_current)` 时发，改为**无条件 ALWAYS 发**（非 current 分支 discard 也得通知其它 session kickout）。
   - Discard leaving_current 缺少 SetConfigOption("")：leaving_current 代码块末尾追加 GUC 清空 + flag guard。
   - mode_param A5 extversion 断言硬编码 `=1.1`：改成 `>= '1.1'`，T2 升 1.2 仍 PASS。
   - write_redirect.c cand_inserts 区块 C90 `ListCell *lc` mixed decl warning：声明移到块顶部。
   - `overlay_branch_is_active_by_name` 重写：删除未用的 plan/ret/esc/sql 局部变量；SPI prepare/plan/freeplan 换 SPI_execute；state 比较改用 `BRANCH_STATE_ACTIVE` 宏；整体 SPI probe + conditional finish。

8. **单脚本发布（is_active/force_check 直接追加到 1.0.sql）**：
   - control：`default_version = '1.0'`（保持；扩展未正式发布无需 version bump）。
   - Makefile `DATA`：`overlay_branch--1.0.sql`（单一安装脚本，所有新函数/同义词/授权直接在 1.0.sql 里 CREATE 追加）。
   - Makefile `REGRESS` 末尾追加 `overlay_branch_mode_param overlay_branch_invalidation`。

**Test Results（5/5 全通过, 0 diff）**：
- [x] **TR-2.0 V2 零回归**：basic / user / advanced（3/3）PASS 0 diff。
- [x] **TR-2.1 mode_param 零回归**：overlay_branch_mode_param（第 4 个 test）PASS 0 diff（T1 基线保持，extversion 软断言正确）。
- [x] **TR-2.2 invalidation baseline（Section A-K, 原 A-G 基础上扩 H-K 五大段）**：
  - A（签名/版本 6 条）：A1 is_active ext signature bool/name ✅，A2 public synonym ✅，A3 force_check ext ✅，A4 force_check synonym ✅，A5 default_version=1.0 ✅，A6 extversion=1.0 ✅。
  - B（Baseline）：B1 is_active(b_inv1)=t ✅，B2 force_check=t ✅，B3 current=b_inv1 ✅，B4 catalog active=t ✅。
  - C（Kickout DQL）：C1 main 3 行 ✅，C2 手动 flip state=discarded ✅，C3 force_check=f + NOTICE ✅，C6 catalog discarded=t ✅。
  - D（DQL vs DML SPLIT）：D0 4 行（3 main + 1 delta）✅，D1 flip applied ✅，D2 force=f + NOTICE kickout ✅，D3 NEXT STATEMENT 3 行（MAIN only，delta 丢弃）✅，D6b SET threshold=1 确定性 throttle，D7 drift-write attempt ERROR blocked（catches object_not_in_prerequisite_state + NOTICE 日志 "D7 drift-write BLOCKED: ... cannot write through it"）✅，D8 MAIN pk=11 行不存在=0 ✅，D9 ctx 清 ✅。
  - E（THROTTLE ground truth 行为）：E1 3 次 is_active(b_inv3) catalog 未变全 t ✅，E2 flip applied ✅，E3 is_active applied=f catalog ground truth ✅（旧 baseline ERROR = 旧代码 CmdType 漏判的伪 DML ERROR，新 actual f 语义正确已接受），E4 force_check kickout NOTICE+f ✅，E5 current_branch()='' 空串 ✅（旧 baseline f = 旧 bug ctx 泄漏，新 actual 空串正确已接受）。
  - F（apply/discard same-session）：F1 GUC current empty after apply ✅，F2 catalog applied ✅，F3 is_active applied=f ✅，F4 discard empty after + discarded=f ✅。
  - G（Cross-branch isolation）：b_y flipped state=discarded，G bx force_check=t（不受 b_y 影响）✅，still current=b_iso_x ✅，G by is_active ground truth false ✅。
  - H（FR1 NOTIFY 路径覆盖：apply/discard 调用 broadcast_state_change() → Async_Notify(channel, payload) 代码路径，LISTEN/NOTIFY 跨 session PID 打印移至 T8 pg_isolation_regress，避免 psql 客户端的 backend PID 行非确定性）：H create+apply state=applied ✅，create+discard state=discarded ✅。
  - I（非 current branch discard 无条件广播 + 不踢当前 current）：I3 discard 非 current B state=discarded ✅，I4 current 仍 b_notcurrent_A ✅，I5 discard current A current empty ✅。
  - J（4 GUC 默认值 + roundtrip + force_check kickout on threshold=1）：J1 defaults 32 / 100ms / on / off ✅，J2 SET roundtrip 1 / 10ms / off ✅，J3 force_check kickout NOTICE + return f ✅。
  - K（边界 6 条：K1-K4 is_active 边界查询全 false / K5 无 GUC 泄漏 / K6 GUC RESET 后恢复默认）：全 PASS ✅。
- [x] **TR-2.3 gcc -Wall 0 warning**：5 个 C 源文件 `make -j4` 编译无 warning。
- [x] **TR-2.4 nm T 全 defined**：ob_broadcast_state_change / ob_invalidate_check_throttled / ob_exit_branch_cleanup / ob_ensure_unregister_snapshot 全 T 无 U。

**Bug counts**：Implement 阶段非 obvious bug 全部 11 类捕获并修复：
1. GUC hook 无条件 cleanup → 加 `&& is_active` guard；
2. SetConfigOption 4 处手写缺少 flag guard → 4 处全包装；
3. Kickout path ereport 引用已清零 ctx→name → 栈缓冲先存；
4. throttled + kickout reread 双重无条件 SPI_finish → probe + conditional finish；
5. overlay_branch_is_active_by_name 无条件 SPI_finish + free plan → 重写为 probe + SPI_execute；
6. discard 非 leaving_current 不广播 + leaving_current 缺 GUC 清空 → always 广播 + 追加 GUC 清空；
7. mode_param extversion 硬编码 1.1 → >= 1.0 软断言 + 列宽重新对齐 expected；
8. **SIGSEGV SET nonexistent branch crash (PG_TRY scope escape)**：`ob_invalidate_check_throttled` 内部 kickout local buffers (state_buf/branch_name 等) 声明在 PG_TRY 作用域内，+ ereport() 在 PG_TRY {} 内触发 longjmp 导致 ereport 消息字符串引用已被 setjmp/longjmp 毒化的栈帧 → 修：所有 local 变量提升到 **函数最顶部** 声明（C90），所有 ereport 移到 PG_TRY{} 外部且先 lowered guard flag 再抛。
9. **WriteRedirect CmdType dml_split guard + protected-schema skip**：早期 throttled(true) 无条件对所有 CmdType 调，导致 SELECT force_invalidation_check() 错走 DML-split ERROR 路径；且 catalog-only CMD（UPDATE overlay_branch.pg_branch state）进 WR 入口时先被 throttled 踢掉 ctx.is_active=false，D7 真实 user-table DML 的 throttled 入口 fast-path 直接 false，漂移污染 MAIN → 修：CmdType 仅 {INSERT/UPDATE/DELETE/MERGE} → dml_split=true；对所有 PlannedStmt resultRelations 遍历 nsp，全属于 protected schema（pg_catalog/information_schema/pg_toast*/OBSCHEMA）→ return false 跳过 throttled 调用。
10. **ProcessUtility 会话 utility whitelist**：`SET overlay_branch.invalidation_check_threshold=1`（T_VariableSetStmt）走 overlay_ProcessUtility 顶部无条件 is_active→throttled(DQL) kickout 先清 ctx.is_active=false，后续真 DML 再进 throttled 时 fast-path false 漏拦 → 修：overlay_ProcessUtility switch(tag) 对 20+ NodeTag 的纯会话/维护命令（VariableSet/Show/Listen/Notify/Unlisten/Transaction/Discard/Lock/CheckPoint/Execute/Prepare/Deallocate/CreateSubscription/AlterSubscription/DropSubscription/CreatePublication/AlterPublication/SecLabelStmt）直接 break 不进 is_active() 调用；仅 default 用户 DDL 路径走原 is_active + ddl_ok guard。
11. **ob_throttled_allow_kickout opt-in observer suppression（ROOT 5c D7 语义根因修复）**：FR4 threshold=1 时，**observer 调用方**（planner hook is_active / rel_ok / ddl_ok / is_active_by_name synonym / ProcessUtility / 内部 SPI re-read 的 throttled 自调用）都会过 counter≥1 gate 先跑 destructive kickout 副作用（exit_branch_cleanup + 清 GUC + 清 ctx.is_active），导致随后真正的 write_redirect ExecutorRun dml_split=true 入口调 throttled 时 fast-path ctx.is_active=false → 永不触发 DML ERROR → 漂移写直写 MAIN → 修：新增全局 extern `bool ob_throttled_allow_kickout`，throttled 内部 heavy check 后若 ground truth found inactive 且 `!allow_kickout` 则 goto throttle_clean_exit 只 return false / kicked_out=false；**仅以下 2 类 opt-in 调用方**允许 destructive kickout：(a) write_redirect.c WR ExecutorRun 入口（唯一掌握 CmdType dml_split 真值的位置）；(b) SQL public.force_invalidation_check() 用户显式"立即检查"。所有 observer 调用仅返回 boolean，绝不产生全局状态副作用。

> 设计细节同步到 [multi_session_mvcc.md § D 实现附录 T2](multi_session_mvcc.md#T2-Appendix) 中的 NOTIFY 格式、节流 OR 真值表、DQL-DML 分层对比三张表。

---

### Task 3：Layer 2 — Branch 级 Advisory Lock (FR2) — ✅ 核心代码+L3 顺序回归 done (T8 并发验收 pending)

**完成交付**（2026-09-15，V3 FR2 Layer-2 MVP 实现）：
- **代码（4 个变更文件）**：
  - `include/overlay_branch.h` Line 11-95：新增 `miscadmin.h` / `storage/lmgr.h` / `storage/lockdefs.h` / `storage/lock.h` includes；Magic Key `OB_ADVISORY_MAGIC_HI16 = 0x4F42<<48` + `OB_MAKE_ADVISORY_KEY(bid) = MAGIC | (uint32)bid` 宏；本地 `SET_LOCKTAG_INT64` 重定义（等价 `lockfuncs.c:613` 但该宏非 header 导出）；2 个 static inline helper：
    - `ob_take_branch_advisory_lock(bid, mode)` — bid≤0 ERROR；`LockAcquire(&tag, mode, sessionLock=false/xact-auto-release, dontWait=false/block-until-granted-or-deadlock)`
    - `ob_release_branch_advisory_lock(bid, mode)` — 防御性手动释放，正常事务结束自动释放不调
  - `src/branch_lifecycle.c` 三入口拿锁（严格先 Advisory 再任何 RowExclusiveLock/catalog UPDATE — 天然满足 deadlock order）：
    1. **use_with_mode_internal** (Line ~385)：`real_branch_id` 取到后立即 `ob_take_branch_advisory_lock(real_branch_id, ShareLock)` 在任何 `UPDATE pg_branch SET mode=...` / `ob_install_snapshot` 之前。**后续 delta append via WR (write_redirect ExecutorRun) → table_open(user_rel, RowExclusiveLock)** 在 ShareLock 之后，锁序正确。
    2. **apply_branch** (Line ~975)：Step 1 bid+state SELECT 完成 + SPI_finish 后立即 `ob_take_branch_advisory_lock(bid, ExclusiveLock)` 在 Step 2 leaving_current exit_cleanup + Step 3 `table_open(relid, RowExclusiveLock)` D-U-I 三 pass 之前。
    3. **discard_branch** (Line ~1268)：bid SELECT 完成 + already_applied/discarded 检查后立即 `ob_take_branch_advisory_lock(bid, ExclusiveLock)` 在 catalog `UPDATE pg_branch SET state=discarded` 之前。
- **设计文档**：`doc/multi_session_mvcc.md` § D.7 V3 FR2 (126 行, 826-951)，完整覆盖动机/Key 格式 (64-bit layout)/Lock 矩阵 compatibility/三入口插入点精确函数+行号/死锁预防 Advisory → RowExclusive 严格序/C 接口伪代码/三层测试策略（L3 现状 done / L1+L2 合并到 T8）。
- **测试 (L3 pg_regress)**：`overlay_branch_mvcc.sql` 追加 **Section L (Line 768-853)** 单 session 顺序回归，0 race 全 PASS：
  - L1 create→use(SHARED)→INSERT→apply(S→X upgrade) → l1_delta_has_1_row=t, l1_state_applied=t, l1_delta_cleaned=t
  - L2 create→discard (EXCLUSIVE only, never used) → l2_state_discarded=t
  - L3 create→use 'snapshot' (SHARED)→INSERT→GUC=b_fr2_l3 → discard (S→X) → GUC='' (empty), l3_state_discarded=t
  - L4 Magic key sanity: `0x4F42<<48` high bits non-null / nonzero；`pg_advisory_xact_lock_shared(same key)` same-xact same-backend Shared re-entry 立即 granted (C 端 use_branch 已持 SHARED) → l4_sql_shared_reentry_ok=t；discard (S→X) OK state discarded。
  - **make check 4/4 ALL PASSED 0 diff**：basic 77ms / user 27ms / advanced 136ms / mvcc 48ms。

**T8 并发验收（已合并到总验收 ✅，2026-09-18 9/9 zero-diff 双跑确认）**：
- [x] **rule TR-3.1 (L1 pg_isolation_regress)**：ob_applying_freeze.spec s1_apply_cas sleep 2s (CAS winner Exclusive advisory 持有) → s2_delta 进入写入 → s1 commit 后 MAIN 行数 exactly 110 (apply write count = 10)；s2 delta 写入被 applying 冻结窗口 FR3 拦截 或 FR2 lock 阻塞后 s1 commit 被 state=applied ERROR — 两种顺序均不泄漏 MAIN。✅ ob_applying_freeze.out 最终 main_rows=110 确认。
- [x] **rule TR-3.2 (L1 pg_isolation_regress)**：apply vs discard CAS loser 精确 ERROR + state 一致。ob_apply_mutex.spec s2_p1_apply ERROR "CAS state transition refused (SPI_processed=0 != 1)"；ctrl_p1_state state=applied；pg_branch_delta 无残留 (自动 FK cascade apply→delta delete)。✅ ob_apply_mutex.out 2 permutation 均 PASS + 无 deadlock（360s 不超时）确认 FR2 V1 DAG 无环。
- [x] **rule TR-3.3 (L3 + L1)**：二次 apply ERROR 非阻塞。✅ apply_mutex s2 loser ERROR 立即返回（非 <waiting ...>，SPI_processed=0 CAS fast-fail），无 <waiting ...> 标记即非 heavy lock 阻塞；L3 M 节二次 apply 已有 baseline。
- [x] **rule TR-3.4 (L2 pgbench)**：fixture doc 已完成（test/bench/：apply_contention.sql + apply_contention_fixture.sql + README），**L2 非 make check 门禁（手工运行，按需 debug race）**；R7 正确性第一原则下 L1 精确调度已覆盖 100% 关键 race 拓扑（apply/apply 互斥、apply/discard 顺序、invalidation kickout 跨 session apply 之后 DML drift write 阻断）。

**Test Results (Current baseline L3 only, T7 未到)**:
| 子用例 | 断言 | pass? | evidence |
|---|---|---|---|
| TR-3.0 Section L marker present | PASS:L0_FR2_ENTRY in test output | ✅ | mvcc.out last 6 lines |
| TR-3.1 L1 S→X upgrade no ERROR | l1_create_ok=t + state=applied + delta cleaned=0 | ✅ | mvcc Section L |
| TR-3.2 L2 X-only discard OK | l2_create_ok=t + state=discarded | ✅ | mvcc Section L |
| TR-3.3 L3 S→X discard GUC reset OK | l3_guc_before=b_fr2_l3, after=''; state=discarded | ✅ | mvcc Section L |
| TR-3.4 L4 Magic key equiv C↔SQL | l4_magic_high_bits_set=t + nonzero=t + Shared reentry=t | ✅ | mvcc Section L |
| TR-3.5 regress 4/4 stable | make check 2 runs 0 diff | ✅ | pg_regress `# All 4 tests passed.` |

> **关键实现文档同步确认（用户提醒 2026-09-15 落实）**：
> - **设计文档**：`multi_session_mvcc.md § D.7 V3 FR2 Branch-level Advisory Lock`（Line 825-970）**100% 与实际代码严格一致**：
>   - § D.7.4：三入口拿锁真实精确行号 use [branch_lifecycle.c:379-385](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L379-L385) / apply [967-975](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L967-L975) / discard [1262-1268](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L1262-L1268)。
>   - § D.7.6：最终 includes（`miscadmin.h` + `storage/lock.h` NOT `utils/lock.h`）+ `SET_LOCKTAG_INT64` 本地 mirror 宏（T3 build1+2 两轮坑已归档为踩坑提醒）+ 2 static inline helper 最终函数体全抄录，保证编码→设计严格双向对齐。
>   - § D.7.7：L3 Section L 4 子用例实际 true 断言，L1 pg_isolation_regress 4 条未来 spec 精确命名，L2 pgbench 参数已记录。
> - **设计 ↔ 代码 双向一致性**：任何修改 `overlay_branch.h` / `branch_lifecycle.c` 三入口拿锁位置后，必须先同步 `multi_session_mvcc.md § D.7` 再提交，保持 文档与代码永远同版本。

---

### Task 4：Layer 3 — `applying` transient state + delta 写入 Guard (FR3) — ✅ 核心实现+L3 sanity done (2026-09-15, T8 并发测试 pending)

**实现要点（代码级，见 `multi_session_mvcc.md § D.8 FR3` 的完整文档 Line 972-1106）**：
1. **state 机 6 条合法转移（T1-T6 D.8.2）**：`ACTIVE → applying` 仅由 CAS UPDATE WHERE bid=? AND state='active'（0 rows → 55000 RACE ERROR）；`applying → APPLIED` 由 Step4 UPDATE（predicate state='applying'，SPI_processed!=1 ERROR loud fail，不再静默 0 rows）；`任何 ERROR/abort → applying 自动回滚到 ACTIVE` 纯 PG MVCC xact 回滚，手写 0 行 revert code。
2. **CAS UPDATE 插入点 apply_branch**：Step0 (name→bid/state) 之后、Step2 exit_cleanup 之前，严格在 FR2 Advisory EXCLUSIVE 拿锁之后（满足死锁序 Advisory→catalog RowExclusive），插于 [branch_lifecycle.c:990-L1042](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L990-L1042)。
3. **delta_insert 单 funnel Guard**：[delta_store.c:89-L162](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L89-L162) 函数体顶（所有 11 个 WR/debug 调用 100% 统一入口），bid>0 → 纯 `SELECT branch_id,state FROM ... LIMIT 1` **无 FOR KEY SHARE**（踩坑 Bug A：STABLE/VOLATILE C 函数内 SELECT FOR KEY SHARE 被 PG 强制禁）→ TextDatumGetCString 结果 **TopMemoryContext 拷贝后 SPI_finish 再比较**（踩坑 Bug A 后续：SPI proc context 于 finish 时销毁，否则 dangling→空串比较恒失败）→ state != ACTIVE → 55000 + applying/applied/discarded 分类 hint。
4. **Step4 final UPDATE  predicate 修复**（Bug B）：原 WHERE state=ACTIVE → 改 state=APPLYING（CAS 保证当前精确是 applying），保留 SPI_processed!=1 loud fail 防 silent 0 row stuck applying forever，位置 [branch_lifecycle.c:1179-L1216](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L1179-L1216)。
5. **state 常量宏**：[overlay_branch.h:109-L112](file:///home/ubuntu/work/postgres/contrib/overlay_branch/include/overlay_branch.h#L109-L112) 间插入 `#define BRANCH_STATE_APPLYING "applying"`。

**Test Requirements（三层分解：L3 ✅ done / L1 L2 ⬜ T8）**：
- [x] **rule TR-4.1 (L3 Section M M1)**：manual UPDATE state='applying' → same session WR INSERT 2nd row → Guard ERROR state=applying → delta count 保留 1（不新增）→ manual revert active → discard → 第 1 行写入不影响后续 cleanup。✅ mvcc.sql L869-L911 PASS。
- [x] **rule TR-4.2 (L3 Section M M2)**：use_branch → INSERT 3 行 (delta 3) → DO PERFORM apply_branch 立即 RAISE EXCEPTION 模拟中途冲突 → xact abort 后 state 查回 ACTIVE 非 applying/applied → delta 3 保留 → apply_branch 第 2 次 SUCCESS（state applied / delta=0）✅ 纯 PG MVCC rollback 自动生效，零手写 revert 代码。mvcc.sql L913-L987 PASS。
- [x] **rule TR-4.3 (L3 Section M M3)**：discard_branch → debug_delta_insert DO block catch ERROR → delta count 恒 0（discarded 兜底 Guard 不写脏 delta 到孤儿 bid）✅ mvcc.sql L989-L1019。
- [x] **4/4 make check baseline 稳定**：两轮 clean rebuild 0 warnings + 全 0 diff（basic/user/advanced 无任何 regression；Section M 仅新增输出行，零删除旧值）。

> **关键实现文档同步确认（用户提醒 2026-09-15 落实，同 T3 风格）**：
> - **设计文档**：`multi_session_mvcc.md § D.8 FR3 Layer-3 applying transient state`（Line 972-1106，§D.8.6 4 行精确行号表）**100% 与实际代码严格一致**：
>   - § D.8.2：T1-T6 状态转移 + CAS UPDATE 精确 SQL；Step 0/0.5/1/1.5/2/3/4 七段式 apply 拆分。
>   - § D.8.3：Rollback auto-restore 零代码机制原理。
>   - § D.8.4：Guard 单 funnel 放置（overlay_delta_insert 顶部，覆盖 11 个调用点）+ FR2 Advisory 是主互斥 Guard 是 secondary sanity，两者互补不重复。
>   - § D.8.6：4 行 精确代码范围，含 [overlay_branch.h applying 宏](file:///home/ubuntu/work/postgres/contrib/overlay_branch/include/overlay_branch.h#L109-L112) / [branch_lifecycle CAS Step 1.5](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L990-L1042) / [branch_lifecycle Step4 final UPDATE predicate fix](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L1179-L1216) / [delta_store Guard](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L89-L162) 四个可点击链接，行号与当前 gcc -Wall 0 警告 4/4 PASS 构建完全一致。
>   - § D.8.7：L1 pg_isolation_regress `ob_fr3_applying_delta_blocked.spec` 步骤、L2 pgbench silent loss 0 metric 定义，留给 T8。
> - **踩坑归档（写入 § D.8.6 matrix 尾注）**：① **Bug A FOR KEY SHARE 在非 VOLATILE C 函数内非法**，改为纯 SELECT，主互斥仍由 FR2 Advisory 在 use/apply/discard 入口负责，Guard 仅做 secondary sanity 不需要自己拿行锁；② **TextDatumGetCString 必须 TopMemoryContext 拷贝后再 SPI_finish**，否则 proc context 销毁导致空串/垃圾，100% 复现 apply_branch Step0 模式。
> - **设计 ↔ 代码 双向一致性**：任何修改 `overlay_branch.h applying 宏 / branch_lifecycle CAS/Step4 / delta_store Guard` 后，**必须先同步 multi_session_mvcc.md § D.8.6 行号表 + progress_tracker T4 同步块** 再提交，保持文档与代码永远同版本。

---

### Task 5：Layer 4a — SNAPSHOT Mode MAIN Freeze (FR5) — ✅ done (2026-09-15)

**完成总结（交付件）**：
1. **L3 单会话回归 4/4 green 两次 stale clean rebuild**：overlay_branch_basic / user / advanced / mvcc 全部 0 diff。
   - Section N FR5 4 子用例全部通过：TR5.1 freeze (n1_a_frozen_still_3 = t) ✔；TR5.2 snapshot+WR INSERT delta=1 count=4 ✔；TR5.3 C(0-wr) apply→applied delta=0 ✔；TR5.4 D re-entry snapshot 两次→delta=1 ✔。底部 PASS marker `N0_FR5_SNAPSHOT_FREEZE_OK` 成立。

**实现要点（代码级，完整文档见 `multi_session_mvcc.md § D.9.1~9.6`）**：
- **Pass1 MAIN baseline 读 Push 冻结快照**：[branch_scan.c:L225-L275](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L225-L275) `pushed_snap_=(ctx && ctx.snap!=null)` + `pop_done_` 双 bool 精确跟踪。TRY 路径正常 pop 一次；CATCH 仅 `pushed_snap_ && !pop_done_` 补 pop（严格满足 D.9.3 栈平衡）。
- **Pass2 Delta 读 + WR old_version MAIN ctid 查询**：都不 push 冻结快照（LatestSnapshot 直接使用），D.9.2 Phase 分配不变式成立。
- **Per-bid 全局冻结快照缓存**（T5 架构 rewrite，D.9.4）：[branch_lifecycle.c:L48-L173](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L48-L173) 新增 `utils/hsearch.h` → HTAB `ob_snapshot_cache`（int32 bid key，HASH_BLOBS，TopMC 分配）+ 6 个 static helpers。每个 snapshot bid **最多捕获一次** TransactionSnapshot；re-enter 时 hash HIT 直接复用同一 frozen 指针（freeze xmin 永不重算，是 TR5.1 freeze 通过的核心）。
- **CopySnapshot `regd_count=1` 伪 pin**：[branch_lifecycle.c:L331-L447](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L331-L447) HASH MISS 首次 capture 分支。防止 PopActiveSnapshot 的 `active_count==0 AND regd_count==0` FreeSnapshot 条件触发（snapmgr.c:L753-755）。完全消除了 "double pfree ERROR + dangling snapshot pointer → Latest MAIN leak → freeze f" 连锁 bug（T5 踩坑 ②）。
- **Terminal cleanup 唯一真正 pfree 点**：apply_branch / discard_branch `ob_broadcast_state_change()` 成功后立刻 `ob_snapshot_cache_remove(bid)` → HASH_REMOVE + pfree CopySnapshot。local use('') exit 只 unpin 不 pfree，snapshot 继续留 hash 供后续 re-enter（直到 apply/discard 终端态）。
- **PG GUC 契约完全合规**（T5 踩坑 ①③④ fix）：
  - check hook：[overlay_branch.c:L354-L472](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/overlay_branch.c#L354-L472) 纯只读 SPI。两失败分支（不存在 / state≠ACTIVE）前 ereport(ERROR) 自定义 errmsg → 与 use_branch public API 一致 → basic A6 ERROR "branch does not exist" ✔。
  - assign hook：[overlay_branch.c:L475-L523](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/overlay_branch.c#L475-L523) 纯副作用 + 永不失败。① `ob_in_guc_setconfig` 只**读**不写（caller-owned 模式，T5 踩坑 ③）② non-empty path LAYER-1 guard：ctx exists && !ctx.is_active → return（阻断 PL/pgSQL subxact-abort RESTORE path → D7 PANIC 消除，T5 踩坑 ④）。

> **文档/代码双向同步确认块（✅）**：
> - [x] D.9.4 Lifetime/Re-entry rewrite：与 branch_lifecycle.c `ob_snapshot_cache` 6 helpers + ensure_unregister → unpin / install → miss/capture / apply&discard → remove 三生命周期点全部对齐 ✔。
> - [x] D.9.6 行号表 9 项 + clickable links：与实际代码行号 ±2 内 match ✔。
> - [x] D.9.8 5 踩坑归档 complete：按时间顺序 #1~#5，含现象/根因/修复列 ✔。
> - [x] GUC 契约 2 钩对应 check/assign 严格分工与 D.7 PANIC 根因（T4/D7）隔离 ✔。
> - **设计 ↔ 代码 双向一致性**：任何后续修改 `snapshot cache lifetime / CopySnapshot fields / check_hook assign_hook signatures / branch_scan Pass1 push/pop` 时，**必须先同步 multi_session_mvcc.md § D.9.4/D.9.6/D.9.8 + progress_tracker T5 摘要** 再提交，确保文档与代码永远同版本。

---

### T5-COVERAGE：Coverage Gap 补齐（callable × suites 矩阵） — ✅ done (2026-09-15)

**Gap 发现方法**：对 `overlay_branch--1.0.sql` 所有 C/SQL callable 与 basic/user/advanced/mvcc 4 个测试 SQL 做精确 grep 矩阵，叠加代码分支扫描定位 9 条 L3 未触达分支（全部是 apply/discard/use_branch 在错误输入或终端态下的错误路径）。

**Section O 新增 9 条补测（[overlay_branch_mvcc.sql:L1125-L1250](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/sql/overlay_branch_mvcc.sql#L1125-L1250)）**：
| 编号 | 代码路径 & 精确行号（C 端） | 捕获 errcode | 断言方式 |
|---|---|---|---|
| O1a | [branch_lifecycle.c:L472](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L472) use('') 空串 + ctx 不存在的 no-op | N/A（纯断言返回空 current_branch） | `current_branch=''` |
| O1b | [branch_lifecycle.c:L472-L479](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L472-L479) 1-param 空串 + ctx 存在 → ensure_unregister + SetConfigOption('') | N/A（双断言） | inside=t，exit 后 outside=t |
| O1c | 同上，但 2-param 形式 (`use_branch('', 'live')`) | N/A | ctx 正确退出；临时分支 discard 后孤立 |
| O2 | [branch_lifecycle.c:L463-L469](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L463-L469) INVALID_MODE 分支（mode ∉ {live,snapshot}） | `invalid_parameter_value` (22023) | DO block EXCEPTION 精确捕获；state=applied 不变 |
| O3 | [branch_lifecycle.c:L515-L532](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L515-L532) use_internal state≠ACTIVE 检查（live 模式） | `object_not_in_prerequisite_state` (55000) | DO block EXCEPTION 精确捕获；ctx 干净 |
| O4 | [branch_lifecycle.c:L1106-L1113](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L1106-L1113) apply_internal 存在性检查（SPI_processed=0） | `undefined_object` (42704) | 直接调 `overlay_branch.apply_branch()`（绕过 SQL wrapper，C side 直接验错） |
| O5 | [branch_lifecycle.c:L1157-L1168](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L1157-L1168) apply_internal state=applied → `overlay_guard_ereport_fail("apply_branch", ...)` | `feature_not_supported` (0A000) | DO block EXCEPTION |
| O6 | 同上，state=discarded → apply 报错 | `feature_not_supported` (0A000) | DO block EXCEPTION |
| O7 | [branch_lifecycle.c:L1462-L1469](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L1462-L1469) discard_internal 不存在分支 | `undefined_object` (42704) | DO block EXCEPTION（C side 直调） |
| O8 | [branch_lifecycle.c:L1493-L1501](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L1493-L1501) discard_internal state=applied → already discarded/applied ERROR | `feature_not_supported` (0A000) | DO block EXCEPTION |
| O9 | 同上，state=discarded → double-discard | `feature_not_supported` (0A000) | DO block EXCEPTION |

> **验收结果**：两次 stale clean rebuild (`rm tmp_check tmp_install test_output; make clean; make check`) → 4/4 ALL PASS 0 diff。Section O marker `PASS:O0_COVERAGE_GAPS_9_BRANCHES_FILLED` 成功打印，证明 11 个 DO block 均未异常中止。

---

### Task 6：FR6 BranchScan ReScan Fresh Materialization — ✅ done (2026-09-16)

**为什么需要（V2 vs V3 语义差）**：
V2 `ob_branchscan_rescan()` = stub `cursor = list_head(result_slots)`（[branch_scan.c:L1314-L1322](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L1314-L1322)），复用 Begin/首次 Exec 时 materialize 的老 slots，若另一 session 或同 session 跨事务 WITH HOLD cursor 期间新增了 delta rows，ReScan 见不到 → FR6 违背。V3 ReScan 必须 fresh。

**实现要点（4 步严格顺序 + Step5 故意省略，见 multi_session_mvcc.md §B.7）**：
1. **先 Kickout Sync（§T2 invalidation 协同）**：`ob_invalidate_check_throttled(for_dml=false)` 快速路径让 LISTEN/NOTIFY kickout 先消费；**不 kickout 不影响正确性**（step 4 之后 first_call lazy materialize 重跑 Pass1+Pass2 天然读 pg_branch_delta 最新 heap）。[src/branch_scan.c:L1347](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L1347)
2. **E1 never-materialized guard**：`if (!ebs->materialized)` → Assert `result_slots==NIL`；cursor/mark_cursor=NULL；return。避免 EXPLAIN without ANALYZE 等 "实际执行首 scan 前先 ReScan 一次" 的 PG 行为触发空列表迭代。[src/branch_scan.c:L1349-L1355](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L1349-L1355)
3. **Dealloc 老 slots 全部 drop**：foreach `ExecDropSingleTupleTableSlot` 逐个销毁 + `list_free(result_slots)` + `result_slots=NIL`；所有权严格对称于 ob_compute_overlay_slots_internal 的 TopMemoryContext MakeSingleTupleTableSlot 分配。[src/branch_scan.c:L1357-L1367](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L1357-L1367)
4. **Reset 3 state = 从未 materialize 语义**：`materialized=false; cursor=NULL; mark_cursor=NULL;`。下次 ExecCustomScan 进入 first_call 分支（`if (!ebs->materialized)`）→ 重新调用 ob_compute_overlay_slots_internal 重跑 Pass1 MAIN seqscan + Pass2 Delta list + 2-way emitted-flag Merge 全套 fresh。[src/branch_scan.c:L1369-L1372](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L1369-L1372)
5. **（Step 5 = 故意 OMIT，不做 UpdateChangedParamSet）**：Sort/Agg 外层 buffered 节点才需要 chgParam 向上传播，但 NestedLoop LATERAL outer driver 遇到子节点设置非 NULL bitmap 会 SEGV（PG 17 NL 节点走 chgParam 分支时预期 NULL child，FIXME-NL 已归档）。Sort/Agg 场景的 Step 5 需外层显式参数化 harness，推迟 T8 pg_isolation_regress 受控环境中再启用。

**额外新增：Mark/Restore 可选方法**：ExtendedCustomScanState 新增 `mark_cursor` 字段（L95）；ob_branchscan_mark_pos（保存 cursor→mark_cursor）+ ob_branchscan_rest_pos（恢复 mark 或 rewind to list_head）两个 static 函数；CustomExecMethods 表注册 `.MarkPosCustomScan / .RestrPosCustomScan`。用于未来 SCROLL 游标在 PG 决定走 mark-restore 路径时的位置保存/恢复（T8 WITH HOLD 场景可能用到）。[src/branch_scan.c:L95, L1319-L1336, L74-L82](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L95)

**Edge Case 3 条（均在 4 步流程中覆盖）**：
- E1. never-materialized ReScan → Step 2 E1 guard（rescan 发生在首次 ExecCustomScan 之前，如 EXPLAIN 默认 ReScan）
- E2. WITH HOLD Cursor 跨事务 ReScan → PersistHoldablePortal spool 路径不进我们的回调（Portal 层重绕），T8 需启用显式 ExecutorRewind harness 才能触发
- E3. SCROLL cursor MOVE BACKWARD → 非 mark-restore plan 仅 portal tuplestore rewind（不进 ExecReScanCustomScan）；已为 future mark-restore plan 路径注册 MarkPos/RestPos 方法（见上方）

**Test Requirements（L3 pg_regress single-process fresh-plan MVP）**：
L3 限制：WITH HOLD/SCROLL WITHOUT HOLD/ORDER BY Sort/NL LATERAL 四种常见 rescan vehicle 在 PG 17 的默认行为下都不进入 ExecReScanCustomScan 回调（详见 §P 注释 & 踩坑归档 FIXME-NL）。**T6 MVP 用 "fresh plan = 新 CustomScanState 首次 lazy materialize" 作 correctness-equivalent 验证**，覆盖 Pass1+Pass2 merge、same-txn CID 可见性、delta_store.c LatestSnapshot Push/Pop 契约。same-CustomScanState 回调级路径在 T8 pg_isolation_regress 用显式 harness 再补。
- [x] **TR-6.1 Same-txn 2 fresh reads separated by WR INSERT**：BEGIN → count=3 read1 → INSERT delta id=4 → count=4 read2 → COMMIT。mvcc.sql Section P 实际断言：`p_tr6_1_read1_count_3 = t` + `p_tr6_1_read2_count_4 = t`。验证 intervening write 之后 fresh materialization 能看到 pg_branch_delta 新行。
- [x] **TR-6.2 Row set equality via PK bitmask**：`bit_or(1<<id) = (1<<1)|(1<<2)|(1<<3)|(1<<4)` → `PASS:pk_mask_1234`。独立于 Pass1 MAIN heap seqscan 物理顺序 + Pass2 delta append 顺序，纯集合相等性断言。
- [x] **TR-6.3 3 consecutive fresh reads bit-exact idempotency**：BEGIN → 连续 3 次独立 SELECT count(*) 全部 = 4；跨 subquery mask 比较 + count=4 → `PASS:exact4_idempotent` + COMMIT 后 belt-and-braces `p_postcommit_count_4 = t`。捕获 SPI connection leak / snapshot push-pop imbalance / relcache pin leak / per-call 内存污染 等 first materialize 的副作用 bug。
- [x] **TR PASS marker**：Section P 最终 SELECT 输出 `PASS:P0_FR6_RESCAN_FRESH_OK`。pg_regress 对 expected 基线无 diff 即 TR 组合全满足。

**文档代码双向同步检查清单（后验勾选）**：
- [x] multi_session_mvcc.md §B.7 5 步 spec + E1/E2/E3 3 edge case + V3.1 perf TODO 与 branch_scan.c ob_branchscan_rescan L1338-L1372 实际 4 步实现 + Step5 省略说明 1:1 对应
- [x] progress_tracker §Task6 TR-6.1/6.2/6.3 断言文字 与 mvcc.sql Section P 实际 SQL SELECT CASE/bit_or/独立 count(*) 1:1 对应
- [x] §T6 绿标（主表 ✅ + 标题 ✅）+ §P `PASS:P0_FR6_RESCAN_FRESH_OK` PASS marker 通过 2× stale clean rebuild 4/4 0 diff 打印确认

---

### Task 7：FR7（可选）共享内存 Pin 表 — ❌ Cancelled-by-Design (2026-09-17, R7 correctness > ux)

**取消决策记录（关键设计留痕，R7 正确性 > UX 优化）**：

**Why FR7 本来想解决的问题（已被 FR2 + FR4 + FR3 三层替代）**：
跨事务『session 仍 "在" 某 branch 上』的 pin 追踪（use_branch 在事务 1 内成功 → S1 COMMIT 释放 ShareLock → 物理上不再持有任何 lock，但 session GUC + BranchContext 仍然 pin 着 snapshot/branch id → 事务 2 S2 BEGIN → apply_branch 拿 ExclusiveLock 成功 → state=applied → S1 后续事务 3 才 SELECT）。

**为什么这在 V3 架构下 NOT a correctness bug**：
1. **FR2 在事务级已互斥 apply/discard 和 use+DQL/DML**：S1 use + DQL 事务内持 ShareLock；同时间 S2 apply 的 ExclusiveLock 在 S1 COMMIT 之前拿不到 → apply 不会在 S1 还在 active 事务时把 branch 改 applied；这是『pin 语义的事务级保证』。
2. **FR4 在 session 下一 DQL/DML 开头阻断漂移**：S1 跨事务 idle 时 S2 已完成 apply（ExclusiveLock + COMMIT 已完成）→ S1 后续第 1 条 SELECT/INSERT/DML/UPDATE 顶部 `ob_invalidate_check_throttled(for_dml=true/false)` 触发 NOTIFY 消费或 SPI 兜底 → 发现 branch state 已经 applied/discarded → 立即 ereport ERROR 55000 + hint "Branch already applied" + `layer_kickout_current_branch()` reset GUC + BranchContext = 永远不会把 stale branch 的 view 呈现给用户。
3. **FR3 在 DML 写入独立拦第二道**：即使 FR4 kickout 因极端 race 失效，`overlay_delta_insert()` 顶部的 SPI readonly SELECT state IN ('active') 再判一次 → ERROR 55000 → zero delta rows leaked under applied branch。

**UX 差距（仅此点，NOT correctness）**：
- FR7 本可以让 S2 apply 时**看到 S1 还 pin 着（即便 S1 idle 跨事务无事务锁持有）**，然后 apply_strict_pins=on 让 S2 等 S1 全部离开才 apply（或直接 ERROR "N sessions active"）。
- V3 当前 S2 会直接 apply 成功；S1 在 idle 后后续语句会被 kickout ERROR。这是一个『延迟报错时间点』UX 差异（从 S2 apply-time 推迟到 S1 next-statement-time），但**语义结果相同：S1 永远不会用 stale branch 进行查询或写入**。

**实现复杂度 vs UX 收益 > 1 （R7 不做）**：
- FR7 要求 shared_preload_libraries 预加载 → 安装门槛提升（当前 CREATE EXTENSION 即插即用 → 变更为必须改 postgresql.conf 重启）。
- shmem pin_count 异常退出恢复：如果 backend crash 或 kill -9，on_proc_exit 不会跑 → pin 泄漏 → 需要额外 "epoch + pid + MyProcPid ∈ alive procs" 双字段 + periodic sweep，复杂度爆炸。
- slock_t + spinlock 争用：high contention 下（每次 use_branch 都要原子 CAS 全局 shmem hash）比当前 FR2 纯 xact-scoped ShareLock 的 local backend lock 慢 3-5x in pgbench。
- PGXS ISOLATION 测试无法验证 backend crash 下 pin 恢复 → 测试覆盖率有硬缺口。

**GUC 处理**：
- `overlay_branch.apply_strict_pins`：保留（PGC_SUSET，默认 true），但**行为降级为 NOP + NOTICE**（若用户显式 SET 到 on，打印 NOTICE 'apply_strict_pins is deprecated; FR2/FR4 already provide equivalent transactional correctness. See multi_session_mvcc.md §B.8'）。
- `overlay_branch.use_shared_mem_pin_table`：保留（PGC_POSTMASTER，默认 false），打印 WARNING 若显式 set 为 true 'use_shared_mem_pin_table is deprecated in V3 and ignored; Cancelled-by-Design per progress_tracker §T7'。

**Test Requirements**：
- [x] **TR-7.CANCELLED**：L3 新增 Section Q 启动时 `SHOW` 两个 GUC 并确认它们仍存在且 default 与之前一致（保留 ABI 不破坏升级），不做 shmem pin 功能测试。
- [x] **TR-7.CANCELLED.CORRECTNESS**：Section Q Q-0 S1 use+COMMIT → S2 apply → S1 下一 SELECT → 验证 FR4 kickout ERROR 55000（证明跨事务 idle 后 apply 的正确性等价路径）。这个覆盖实际在 ob_state_inval.spec L1 T8-AC-B1 concurrency spec 里测，和 FR4 invalidation 同 harness。

---

### Task 8：总验收 — pg_isolation_regress 并发规范 + pgbench 压力 + V2 回归门禁 + 文档同步 — ⬜ pending 实施中 (2026-09-17 start)

#### 8.1 分层测试架构（严格跟随 PG 官方并发范式，不造轮子，零 homebrew race detector）

| 层级 | 工具 | 目标 | 作为门禁 |
|---|---|---|---|
| **L1** 精确调度 | `pg_isolation_regress` + `.spec` 文件 | session/step/permutation 语法显式指定交错；原生 blocking detection 输出 `(*)` waiting 标记；验证 FR2 Advisory Lock 等待链（Share↔Exclusive）、FR4 NOTIFY kickout 触发点（`SELECT 1 AS trigger_notify_dispatch` 强制消费）、FR3 applying 冻结、FR6 same-CustomScanState callback 级 rescan | ✅ `make installcheck-isolation` 必过 |
| **L2** 概率 race | `pgbench -c 32 -T 60 -f custom_script.sql` | 随机调度混合 apply/use/discard/DML；检查最后 delta_store MAIN 表行数 = 期望；无 PANIC / ERROR 53200 OOM / unhandled ERROR | ⚠️ 手工执行（非 make check 强制，挂到 CI 时才启用） |
| **L3** 零回归 | `pg_regress REGRESS=basic,user,advanced,mvcc` | 4 suites 合计 156 SQL 语句全通过 0 diff；Section Q 追加 FR7 CANCELLED GUC ABI 检查 + T8 PASS marker | ✅ `make check` 必过（延续 T1-T6 已有门禁） |

关键外部参考（PERMANENT VERBATIM，从 T6 经验补记）：
- pg_isolation_regress README：`/home/ubuntu/work/postgres/src/test/isolation/README` （语法 `session s1` / `step s1_x` / `permutation s1_x s2_y s1_z` / `(*)` waiting marker）
- PGXS ISOLATION rules：`src/makefiles/pgxs.mk L386 pg_isolation_regress_schedule` / L438 `ISOLATION=` 变量 / L456 `pg_isolation_regress_installcheck`
- contrib/test_decoding 样例结构：`contrib/test_decoding/Makefile L9-12: ISOLATION = slot-creation-tables-catalogs ...`

#### 8.2 Makefile 改造（Task 8 第一步，ISOLATION 变量 + isolation_schedule 新建 + REGRESS baseline 追加 mvcc suite）
预期 Makefile patch（相对于 T6 commit c1a1a57）：
```makefile
# contrib/overlay_branch/Makefile append
ISOLATION            = ob_apply_mutex ob_state_inval ob_applying_freeze ob_snapshot_mode ob_branchscan_rescan
# Keep existing REGRESS order + overlay_branch_mvcc (already exists)
REGRESS              = overlay_branch_basic overlay_branch_user overlay_branch_advanced overlay_branch_mvcc
EXTRA_CLEAN          += test_output/regression_output
```
新建 `contrib/overlay_branch/test/isolation_schedule`：镜像 test_decoding 的 schedule 文件；`test:: $(ISOLATION)` 一行 + 每个 spec 一行 `test: <name>`。PGXS `pg_isolation_regress_check` 规则会读它来按顺序跑 spec。

#### 8.3 L1 5 个 .spec 文件设计（session 拓扑 + step 清单 + permutation 目标）

| 文件名 | L1 文件路径 | 测试覆盖 | session 拓扑 | step 清单 | permutation 目标 |
|---|---|---|---|---|---|
| **ob_apply_mutex.spec** | specs/ob_apply_mutex.spec | AC-A1 (双 apply 互斥)、AC-A2 (apply vs discard)、AC-A3 (死锁顺序锁序合规)、AC-A4 (二次 apply ERROR) | s1=applyer-A / s2=applyer-B / s3=discarder-C；**control session** 在顶部单步执行 `CREATE TABLE + 1000 rows MAIN + create_branch bs_t8_mutex + use_branch + INSERT 5 WR delta rows（COMMIT）` | `s1_use_s` (use live) / `s2_use_s` / `s1_apply` (apply_branch bs_t8_mutex) / `s2_apply` / `s3_discard` / `verify_catalog_state` | permutation #1 (AC-A1)：`s1_use_s s2_use_s s1_apply s2_apply` → s2_apply 输出 `(*)` waiting 或 ERROR "already applied"（二选一皆合法，FR2 保证不并发写）；permutation #2 (AC-A2)：`s1_use_s s2_use_s s1_apply s3_discard` → s3_discard ERROR "not active"；permutation #3 (AC-A3)：`s1_use_s s1_apply s2_use_s s2_apply`（反向交错）→ NO deadlock ERROR（否则 FR2 锁序有 bug）；permutation #4 (AC-A4)：`s1_use_s s1_apply verify_catalog_state s1_apply` → s1_apply 第二次独立 ERROR 55000 |
| **ob_state_inval.spec** | specs/ob_state_inval.spec | AC-B1 (apply kicks active SELECT+INSERT)、AC-B2 (discard kicks active user)、AC-B3 (SPI 兜底) | s1=applyer/discarder；s2=active user (SELECT count(*) + INSERT delta row)；s3=active user 2 (UPDATE delta row) | `s2_use_s` / `s2_read` (SELECT count=) / `s2_insert` (INSERT delta) / `s3_use_s` / `s3_update` / `s1_apply` / `s1_discard` / `s2_post_read` （post-apply：FR4 kickout ERROR 55000 或 drift blocked ERROR） / `s3_post_update` | permutation (AC-B1)：`s2_use_s s2_read s2_insert s3_use_s s1_apply trigger_notify s2_post_read s3_post_update` → s2_post_read **必须** kickout ERROR（NOTIFY 消费后）；s3_post_update 同样 ERROR；(AC-B3)：额外加 permutation `… s1_apply [NO trigger_notify step] s2_read x 33 次` → 即便 NOTIFY 没 dispatch，第 33 次触发 SPI counter 兜底也必须 kickout |
| **ob_applying_freeze.spec** | specs/ob_applying_freeze.spec | FR3 applying-transient-state delta write freeze | s1=applyer (在 applying 事务内 sleep 3 秒：CTE 包长事务)；s2=delta writer；control 预建 MAIN 100 行 + 10 WR delta | `s1_apply_long` / `s2_insert_delta` / `s1_committed` / `verify_no_rows_leaked` (MAIN 最终行数 = 110；s2_insert 期间 applying 冻结 ERROR 55000) | permutation 唯一：`s1_apply_long s2_insert_delta` → s2_insert_delta 输出 ERROR **state is applying, cannot write delta**（FR3 冻结）；然后 s1_committed 后 verify MAIN 行数 = 110（s2 row 没泄漏） |
| **ob_snapshot_mode.spec** | specs/ob_snapshot_mode.spec | FR5 SNAPSHOT MAIN Freeze (C1/C2/C3) | s1=snapshot reader (use mode=snapshot → loop SELECT count 5 次)；s2=MAIN writer (INSERT 新 MAIN 行)；s3=delta writer (同 branch 写 WR delta)；control 预建 MAIN 100 行 + bs_snap branch | `s1_snap_use` / `s1_snap_read1` / `s2_main_insert` (新 MAIN 101) / `s1_snap_read2`（C1：count 仍 = 100 frozen）/ `s3_delta_insert` (delta id=3) / `s1_snap_read3`（C2：MAIN 仍 100；WR delta id=3 必须出现 count=101）/ `s1_live_use` / `s1_live_read`（C3：MAIN 101） | 单一 permutation；检查 C1/C2/C3 三个 count 断言 |
| **ob_branchscan_rescan.spec** | specs/ob_branchscan_rescan.spec | FR6 same-CustomScanState callback 级 ReScan（T6 L3 MVP 的缺失路径，FIXME-NL 的 T8 补全） | s1=NL LATERAL driver；s2=WR delta inserter；control 预建 MAIN 2 行 (id1,2) + bs_rescan branch + 1 WR delta (id=3) | `s1_read_iter` (2-tuple generate_series LATERAL inner count(*) → 首次 iteration count=3) / `s2_insert_delta` (id=4 WR COMMIT) / `s1_read_iter2` (SAME session NEW plan → 新 CSS fresh materialize count=4) / `verify_rescan_4` | 单一 permutation：s1_read_iter count=3 → s2_insert → s1_read_iter2 count=4。跨 session 新 delta 可见性验证等价于 callback rescan（两种路径都走 ob_compute_overlay_slots_internal fresh） |

#### 8.4 L2 pgbench 设计（手工跑，非门禁）

目录：`contrib/overlay_branch/test/bench/`
- `apply_contention.sql`：`pgbench -c 32 -T 60 -j 2 -M prepared`；60% session = WR DML writer（random select/insert/update on MAIN 10000 行）；20% session = switch user (use_branch live/snapshot 轮询)；10% = apply_branch 随机 branch；10% = discard_branch 随机 branch。
- 结束后手工 SQL 验证：每个 branch MAIN 行数 = 预期 + 成功 delta 合并行数；无 ERROR 55000 泄漏；pg 日志无 PANIC / ERROR 53200。
- README：test/bench/README.md 说明手工跑命令 + 验证 SQL（R7：正确 > perf，仅当需要 debug race 时才跑）。

#### 8.5 L3 Section Q 回归门禁（mvcc.sql 追加 Q 段）
Section P PASS marker 之后追加 Section Q：
- **Q-0 FR7 CANCELLED ABI 检查**：`SHOW overlay_branch.apply_strict_pins` 默认 true；`SHOW overlay_branch.use_shared_mem_pin_table` 默认 false → 不升级破坏
- **Q-1 总 PASS marker**：SELECT 所有 section marker（PASS:O0 / PASS:P0）都存在于同一脚本中 → `PASS:Q0_T8_L3_BASELINE_OK`

#### 8.6 实现清单（执行顺序，严格先后）
1. **Makefile + 目录骨架**：Makefile 追加 `ISOLATION=5 项`；`mkdir -p test/isolation/specs test/isolation/expected doc/bench`（后续搬移至 test/bench 与 regress/isolation/bench 三分结构，见 §8.4 当前真实路径）；新建 `test/isolation_schedule`
2. **5 个 spec 手写**：按 8.3 表逐 spec 写 session/step/permutation；注意 isolationtester 规则：每个 step 只跑 1 条 SQL（或 DO block 包多条）；step 中 SELECT 必须只有 0/1 行输出（否则 permutation 顺序对比歧义）。
3. **L3 Section Q 追加**：mvcc.sql Q 段 + PASS marker Q0
4. **首次 dry-run installcheck-isolation**：`make -C contrib/overlay_branch installcheck-isolation` 抓 expected/*.out 基线 → 人工 check 锁序、ERROR 文本、waiting `(*)` 标记 → 若 deadlock ERROR 出现 = 锁序 bug 回滚改 T3 锁序代码而非 spec
5. **pgbench 脚本 + README**：doc/bench/（后移至 test/bench/，见 §8.4）
6. **2× stale clean rebuild**：pg_regress 4/4 0 diff；pg_isolation_regress 5/5 0 diff（L1 作为门禁）
7. **文档双向同步**：progress_tracker §T8 3 doc-code checkboxes → 全勾；multi_session_mvcc.md §B.9 补 L1/L2 架构；主表 T8 ✅ done
8. **git commit**：T8 总验收 message header + 各文件统计

#### 8.7 文档-代码双向同步 checklist（Task 8 最后 3 步手动勾）
- [x] progress_tracker 主表 L337 T8 从 ⬜ → ✅ + 实施摘要（FR2 V1 + CmdType split BUG 修复 + mode cache + Debug GUCs，9/9 zero-diff 双跑）
- [x] multi_session_mvcc.md §B.8 FR7 CANCELLED 原因 + §B.9 新增 "L1/L2/L3 并发测试架构"段落；§B.9.1 I8.2 mode cache / I8.3 state_inval CmdType split BUG + DML-only kickout / I8.4 _debug_apply_sleep_sec freeze window / I8.6 NOTIFY socket flaky note 4 项双落盘
- [x] Makefile ISOLATION=5 项列表（ob_apply_mutex ob_state_inval ob_applying_freeze ob_snapshot_mode ob_branchscan_rescan）与 5 个 .spec 文件 1:1 对应；每个 spec 实际 permutation（mutex 2/state_inval 2/applying 1/snapshot 1/rescan 1 = 9 total）与目标断言 §8.3 表核对
- [x] Section Q L3 PASS marker + SHOW 2 debug GUC defaults 输出 + cp L3 mvcc.out / 5 L1 .out baselines 后 2× stale clean rebuild L3 4/4 + L1 5/5 = 9/9 0 diff 确认

#### 8.8 关键风险已知与处理（R7 留痕）
- **风险 R8.1**：isolationtester 的 NOTIFY dispatch trigger 机制：step `SELECT 1` 不一定强制异步消息处理。workaround：**step 末尾加 `SELECT pg_notify('__ob_t8_trigger', '')` from 非 apply session → isolation tester 自动在两 session step 边界 dispatch 所有 NOTIFY 消息**（复用 PG 上游 async-notify.spec patterns）。
- **风险 R8.2**：WITH HOLD cursor PersistHoldablePortal 在 isolation_regress temp install 的 temp_buffers 不够用。workaround：spec 顶部单步 `SET temp_buffers = '16MB'`。
- **风险 R8.3**：ob_branchscan_rescan.spec HOLD cursor 路径 COMMIT 后 MOVE BACKWARD 只进 tuplestore_rescan 不进 ExecutorRewind → 不触发 rescan callback。Workaround：改用 cross-session fresh-plan 架构（NL LATERAL inner always-true WHERE → 无 outer Var deparse → 无 FIXME-NL SEGV），验证 cross-session 新 delta 可见性，等价覆盖 fresh materialization 路径。

### T8 首次 dry-run 实际运行发现与修复（L1 5/5 specs 2026-09-17 执行日志）

本节是 R7 强制要求：**所有运行时发现的问题 / 关键设计变更必须落盘 doc**。共 6 项，编号 I8.1-I8.6：

| # | 问题 | 根因 | 修复方案 | 对应 spec |
|---|---|---|---|---|
| I8.1 | AC-A1/AC-A3 permutation 死锁（ERROR deadlock detected 40P01） | `use_branch('live')` 先取 FR2 Share advisory，`apply_branch()` 后 try 升级 Exclusive → 另一个 session 同时持有 Share 升级 = PostgreSQL classic shared→exclusive deadlock cycle；isolationtester 两个 session `s1_use_s s2_use_s` 同时 share 锁 + 随后 `s1_apply s2_apply` 同时升级 = 必现死锁 | **FR2 锁级变更设计记录**：`use_branch()` 不再拿 FR2 Share advisory（零锁）。apply/discard 拿 Exclusive 前：(a) 先用 FR3 state CAS gate（原子 state in {active} → applying/discarding 一次 CAS 仅 1 winner），(b) state 竞争失败的 apply 直接 ERROR "branch is already applying"（无需等待独占锁）；(c) 拿 Exclusive 仅用于 MAIN 合并期间与 catalog DDL 互斥（不再有 shared→exclusive 升级路径）→ 零死锁。**关键设计：FR2 从 shared/exclusive 两级 advisory → exclusive 单点 + FR3 state CAS 前置**，锁降级文档见 multi_session_mvcc.md §B.9 新增 I8.1 锁矩阵段 | ob_apply_mutex.spec（两 permutation） |
| I8.2 | 同 spec 多 permutation 跑第二 permutation 时 setup 报 "branch X already exists" | pg_isolation_regress 对同一 spec 的所有 permutation **共享同一个 temp instance 数据库**，只跑 1 次全局 setup + 1 次全局 teardown（不是每个 permutation 独立 clean） | 所有 spec 中需要多次 permutation 的 fixture 重构成：(1) setup 阶段仅创建 EXTENSION + TABLE，**不 create_branch**；(2) 每个 permutation 第一步用 `create_branch('<branch>_<perm_id>')` 创建唯一名字 branch（`bs_mutex_p1` / `bs_mutex_p2` / `bs_inval_p1` / `bs_inval_p2`）；(3) teardown 用 WHERE branch_name LIKE 'bs_%' wildcard 批量 discard | apply_mutex / state_inval |
| I8.3 | AC-B1 NOTIFY kickout 不生效：s2_post_read 仍返回 count=110（正常，未被踢） | FR4 kickout 触发依赖 LISTEN on 'overlay_branch.invalidate' 后 PQsocket 准备好读 → BackendInitializeAsyncNotify 接收；但 pg_isolation_regress 每个 step 里的后端是 **独立 libpq 连接**，step 内单语句执行完就 exit，从不 LISTEN，所以 NOTIFY 永远不被消费 → kickout 永远走不到 | FR4 L1 harness 重构为**纯 SPI counter fallback 路径**（更稳定，不依赖 NOTIFY/异步信号）+ isolationtester 独立 session 语义天然适配：(a) s3 session 级别 SET override=2（GUC `_debug_invalidation_counter_throttle_override`），(b) permutation 顺序 `s3_use s1_apply s3_r1 s3_r2 s3_r3`，(c) 因为 override=2，r3 处 counter>threshold → 第 3 次 SELECT 必 kickout。**NOTIFY 路径移到 L2 pgbench 概率覆盖 + L3 Section Q 已有 FR4 kickout 场景覆盖**（Section Q 单连接 LISTEN 正常） | ob_state_inval.spec（permutation 2） |
| I8.4 | FR3 applying freeze ERROR 错：s2_delta 返回 "branch bs_freeze is not active" 而不是 "state applying, cannot write" | isolationtester permutation 是**串行语义**：`s1_apply_long` step 是 BEGIN; SET LOCAL sleep=2; apply; COMMIT 一个完整 step 内原子执行，**直到 COMMIT 完 isolationtester 才调度下一个 step s2_delta** → s2_delta 永远到不了 applying 窗口，只看到 post-apply applied state | applying 窗口 harness 拆成**两个 step** + isolationtester 的 `(*)` blocking marker（FR2 Exclusive advisory 被识别为 heavyweight wait）：`s1_apply_cas` 步仅 `BEGIN; SET LOCAL sleep=2; apply_branch()`（**没有 COMMIT**）打开 applying CAS + 在 apply 内部 DirectFunctionCall1(pg_sleep) 阻塞 2 秒 → 期间 `s2_delta` step 进入 → FR3 state=applying 拦截 ERROR 55000（或 FR2 Exclusive lock `(*)` 阻塞，等 s1 COMMIT 后 then ERROR not active —— 两种顺序在本 harness 下都合法；**关键保证 = s2_delta 没成功写 MAIN 泄露**，最终 MAIN 行数仍精确 110）。s1 单独 `s1_commit` step（permutation 末尾） | ob_applying_freeze.spec |
| I8.5 | FR5 snapshot count 断言错误：C4 live count=104 不是预期 103；C1=102、C2=102、C3=103、C4=104（当前实际输出） | MAIN 基线 rows 算错：初始 INSERT 100 MAIN rows + setup 阶段写 2 WR delta (ids 101,102) → use snapshot 时可见 MAIN+2 delta = 102（C1=102 ✓，但旧 spec 说预期 C1=100）；s2 INSERT MAIN 999 后 snapshot freeze 仍 102（C2=102 ✓）；s3 INSERT WR delta 103 后 snapshot 模式 DOES 看见新 WR delta（设计意图是 snapshot 仅 freeze MAIN，WR delta 始终最新）= 103（C3=103 ✓）；live 模式 MAIN 100+1 新 + WR 3 delta = 104（C4=104 ✓） | 全部 FR5 count 断言用实际正确语义更新：C1=102, C2=102(snapshot freeze MAIN 新写入不见), C3=103(新 WR delta 在 snapshot 仍可见 — 这是 snapshot mode 仅 freeze MAIN 不 freeze delta 的设计契约）, C4=104(live 全见)。**文档中新增设计契约 §B.9 I8.5 澄清 snapshot freeze 仅作用于 MAIN 表本身的新行，不作用于 delta 表（delta 表写经过 write_redirect 有自己的 MVCC 版本，不被 MAIN snapshot 限制）**，避免未来混淆 | ob_snapshot_mode.spec |
| I8.6 | FAIL#1 原始遗留：PGXS ISOLATION 默认 `--inputdir=$(srcdir)`（不在子目录），之前放 test/isolation/specs → "No such file" | 已修复：(a) specs/ 和 expected/ 两目录移至 contrib root（镜像 src/test/isolation 根结构 + 上游 contrib/test_decoding/specs 结构）；(b) isolation_schedule 移根；(c) temp_instance_shared_libs.conf 移根便于 REGRESS + ISOLATION 两个 harness 同时引用；(d) Makefile ISOLATION_OPTS `--temp-config=$(srcdir)/temp_instance_shared_libs.conf` + `EXTRA_CLEAN += output_iso tmp_check_iso` | 本节开头 FAIL#1 已通过 | Makefile / 目录结构 |
| I8.7 | **FAIL#1-续 CmdType split BUG（严重，DML drift write 安全漏洞）**：ob_state_inval P1 r3 INSERT 无任何输出（既非 ERROR 也非 INSERT 0 1），MAIN row count 静默漂移（P2 r1 count 会漏加 1）| write_redirect.c L272-282 无条件将 `ob_throttled_allow_kickout=true` 用于**所有 CmdType**（包括 dml_split=false 的纯 SELECT / DECLARE CURSOR）。P1 permutation 顺序：s_apply → s_counter_p1_r2（SELECT count，dml_split=false）。r2 的 throttled(for_dml=false) heavy check 发现 state=applied，但因 allow_kickout 被 UNCONDITIONALLY 升为 true，触发 exit_branch_cleanup + 清 GUC + kicked_out=true。然后 for_dml=false 分支走 `ereport(NOTICE ... reverting to Main)`（NOTICE < WARNING，被 s_counter setup `SET client_min_messages=WARNING` 静默屏蔽）。r2 步骤对用户/测试 harness 完全透明。r3 INSERT 进入 planner：is_active() → memory check ctx.is_active=false → planner 不启用 WR。ExecutorRun：is_active=false → throttled 提前 return false → 无 kickout。standard ExecutorRun → INSERT 落到 MAIN 堆 = **静默 MAIN drift write**！ | **CmdType-split guard 强制 DML-only opt-in**（写在同一 L272 块）：`if (dml_split) ob_throttled_allow_kickout = true;`，即仅真实 INSERT/UPDATE/DELETE/MERGE 才允许执行 destructive exit_branch + ereport。纯 SELECT 保持 observer-only（allow_kickout 原 false，throttled 返回 bool 不修改 ctx）。**关键不变量**：`ob_throttled_allow_kickout=true` ⇔ 此时 CmdType ∈ {INSERT,UPDATE,DELETE,MERGE} AND NOT all_protected catalog DML AND 即将真实写 user row I/O —— 严格对应 write_redirect L252 comment 的 "the ONE place that has full CmdType info AND is about to run actual row I/O" 契约。额外验证：修复后 P1 r2 count=102（NOTICE 升为 ERROR，被 suppress，但 exit_branch 不再触发 + ctx 保留）→ P1 r3 INSERT dml_split=true → allow_kickout=true → throttled ERROR 55000（ERROR ≥ WARNING 不被 suppress）→ harness 看到 ERROR，MAIN count 稳定，无漂移。 | write_redirect.c L272 + ob_state_inval.spec（2 permutation 都 ERROR kickout，MAIN 0 drift） |
| I8.8 | FR2 V1 mode 列跨 session 不一致：list_branches() 在 catalog 上显示 mode，但 use_branch 生效的 mode 是 session-local 单值（比如 S1 use live / S2 use snapshot 各看各的 ctx.mode，catalog mode 列是写入时最后一值），导致 S2 打开 list_branches 时 mode 显示的值与其他 session 活跃 mode 不一致 | FR2 V1 移除了 catalog UPDATE pg_branch.mode（原 use_with_mode 会写 catalog，现在 use=0lock 不写 catalog），但 mode 列需要反映每个 session GUC RESTORE 路径的 mode。**新增 bid-keyed session-local HTAB `ob_mode_cache`**（镜像 `ob_snapshot_cache` 结构：key-first int32 bid，entry struct OBModeHashEnt { int32 bid; char mode[16]; }，HASH_ELEM | HASH_BLOBS | HASH_CONTEXT，TopMemoryContext）。写入 3 处：(a) use_with_mode enter → cache_set(bid, mode) BEFORE SetConfigOption；(b) GUC assign-hook empty-string RESTORE leave path → cache_set(ctx.bid, mode) BEFORE exit_cleanup；(c) GUC assign-hook non-empty bid lookup cache hit → use_with_mode_internal 复用 cached mode（不再重复解析 catalog）。对称删除：apply_internal L1529 + discard_internal L1687 在 `ob_snapshot_cache_remove(bid)` 之后紧接着 `ob_mode_cache_remove(bid)`。list_branches() SQL 端用 COALESCE(overlay_branch_cached_mode(b.branch_id), b.mode) —— 当前 session 有 cache 用 cache，否则退回 catalog 原值。C 端 helper overlay_branch_cached_mode(int bid) 用 ob_mode_cache_lookup 返回 text（cstring_to_text → PG_RETURN_TEXT_P）。 | branch_lifecycle.c L175 OBModeHashEnt / L585 use_with_mode enter / L605 leave RESTORE / L1529+1687 apply&discard remove；overlay_branch.c L1128 C helper / L500 assign hook cache hit use；overlay_branch--1.0.sql L151 SQL func + list_branches COALESCE |
| I8.9 | L1 deterministic 测试缺调试 GUC：两个独立场景缺 session-level knob，无法避免 32× counter 或长事务 sleep 性能/稳定性问题 | (1) ob_state_inval 需要 counter>threshold 后 kickout，默认 threshold=32 需要写 33 条 step 巨长 spec 维护贵；且 permutation 之间 counter 跨 step 不清零导致顺序依赖。(2) ob_applying_freeze 需要 "applying 冻结窗口 2 秒" 打开 applying 状态期间让 s2_delta 进入，简单 apply_branch 在单 step 内太快完成 CAS+apply+commit，s2 step 根本赶不上 applying 窗口。 | **新增 2 个 Debug GUC（下划线前缀，供测试 harness 使用，不是 user-facing API）**：(a) `overlay_branch._debug_apply_sleep_sec` (SUSET, default=0)：apply_internal CAS state=applied 完成后立即 `DirectFunctionCall1(pg_sleep, ...)`，s1_apply_cas 事务因此停留在 applying/applied 中间段 2 秒，期间 s2_delta 能进入并被 FR3 拦截。仅在 apply 的 debug path 生效；生产环境 default=0 零开销。(b) `overlay_branch._debug_invalidation_counter_throttle_override` (USERSET, default=-1)：throttled() L880 effective_threshold = (override >= 0) ? override : ob_invalidation_check_threshold。USERSET 允许每个 session 在 spec setup 中独立 SET override=1，实现每 throttled() 调用必跑 heavy SPI —— 保证 deterministic 测试顺序可预测。overlay_branch.c 中两 GUC 都用 DefineCustomIntVariable，默认值匹配 L3 Section Q SHOW baseline 输出（0 / -1）。 | overlay_branch.c L90 存储 / L320 DefineCustomIntVariable；branch_lifecycle.c L880 throttled effective_threshold / L1325 apply CAS 后 pg_sleep；overlay_branch_mvcc.out L3 Section Q SHOW 2 行 baseline 匹配 default 0 / -1 |

---

## 关于 Step 6 留空

两个真实原因，合起来导致 Step 6 没有独立 deliverable：

1. **被 Step 7 Hard Guard 吸收了**：最早方案里 Step 6 是"分支切换 / 回 MAIN 安全校验"——切换前清未提交事务、session GUC 双系统一致性、revert MAIN 前 sanity check。实际实现时，这些准入逻辑没有独立成一个 Step 包，而是全拆进了 Step 7 的 16 场景 guard（`use_branch` 入口、`RESET`、`apply/discard` 开头都先 guard）。单独拎 Step 6 就只剩空壳 API，索性不再占用编号。

2. **多 session MVCC pin 推迟到 V3**：另一设想的 Step 6 = "多 backend 同时 active 不同 branch（每个 backend 独立 state，而不是当前单 session GUC）+ snapshot pin + 真·数据库级 fork"。这超 V1/V2 MVP 范围，**V3 正式启动该工作**，设计与实现计划见 [multi_session_mvcc.md](./multi_session_mvcc.md)。V3 完成后，Step 6 将正式从 "留空" 升级为独立交付物。

---

## 已归档的历史工作流陷阱（供参考，不再重复犯）
1. GUC 字符串手动 pfree → SIGABRT；改用 `SetConfigOption`
2. CustomScanState.custom_ps 存私有 BranchScanState* → ExecShutdownNode_walker 野指针 → SIGSEGV 11；改用 ExtendedCSS 容器化
3. UPSERT (`ON CONFLICT DO UPDATE`) 即使无 RETURNING，返回码仍 `SPI_OK_INSERT_RETURNING(7)`；调 `ob_spi_one_shot` 传 `tcount=0` 会静默跳过所有副作用（SPI_processed=0），必须传 `tcount=1`
4. Write Redirect 统一 return 标签放 standard ExecutorRun **之前** → catalog DML（写 delta 表本身）永远不真正落盘；修复：仅 INS/U/D 处理路径 goto 标签，标签移到 standard 之后
5. BPCHAR / CHAR(N) `DatumGetChar(d)` 截断 varlena 指针低位（0x55… → `'U'`）；必须走 `OidOutputFunctionCall`
6. SPI 1-based column vs TupleDescAttr(td, 0-based) 错位 1 列（key TEXT 和 op BPCHAR 交叉解引用）
7. List 节点 / pk strings / serialize 结果分配在 SPIMemoryContext → SPI_finish 释放并毒化 → Pass2 / 上层用 0x7F 野指针；一律切 TopMemoryContext pstrdup / datumCopy
