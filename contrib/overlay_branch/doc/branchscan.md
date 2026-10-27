# BranchScan 透明扫描 — 设计、优化策略与硬约束

> 本文档统一收录 BranchScan CustomScan（方案A）的**设计评审 / 实施切片 / 优化策略 / 踩坑硬约束**。
> 原 `branchscan_plan.md`（实施计划）和 `branchscan_strategy.md`（P0/P2 策略+回退矩阵）已合并至此，避免在两个文件间跳读。
>
> - **Part A**：V2 骨架——设计调研、实施步骤切片、风险矩阵（原 plan）
> - **Part B**：V2 优化——P0 PK O(1) 快路径 + P2 非 PK 条件下推 + 协议 + 验证断言（原 strategy §1-7）
> - **Part C**：硬约束踩坑——L3 Delta O(1) lookup MemoryContext 生命周期（原 strategy §8）
> - **Part D**：阶段 2 读路径架构——去 SPI / 流式早停（RD-8 / RD-7 / RD-6）
> - **Part E**：P0 谓词下推扩展——从「单列等值」到 `IN` / 范围 / `OR`

---

# Part A — BranchScan 设计与实施计划

## A.1 Repository Research

### 现状（V1 入口）
用户在分支模式下读分支叠加视图**必须手动**调用 SRF：
```sql
SELECT * FROM overlay_branch.overlay_main_plus_delta('public.t')
  AS x(id int, v text);
```
内部 2-pass 合并逻辑已经在 `overlay_main_plus_delta()` 中实现（Step4b，`src/overlay_branch.c#L1487-L1710`），核心阶段可直接复用：
1. `overlay_delta_list_for_rel(branch_id, relid)` 加载 `List<DeltaTuple*>`（L1543）
2. SPI seqscan 主表（L1549-L1553）
3. Pass1 主表 merge：逐行算 key（typed PK 优先；`M_total==0` 或 delta 全 typed 时跳过昂贵的 JSON 文本键 `serialize_pk`，见 performance.md §5 阶段 1b RD-8）→ delta 匹配 → 无匹配输出主表 / 'D' tombstone 跳过 / 'U'/'I' 输出 delta 覆盖行，matched entry 标记 `->emitted=true`（L1584-L1695）
4. Pass2 未出现的 delta INSERT 行补漏：遍历 delta_list 找 `!emitted && op='I'` → `reconstruct_slot_from_delta` 输出（L1673-L1695）
5. 辅助：`reconstruct_slot_from_delta(Relation, bytea)` 把 tuple_data bytea 还原成 `TTSOpsVirtual` slot

### PG CustomScan API 关键入口调研结论（PG17 verified）
| 层 | 回调 / 注册 API | 头文件 | 我们用它做什么 |
|---|---|---|---|
| Planner Hook | `set_rel_pathlist_hook_type` | `optimizer/paths.h:L30-L34` | 每条 RTE 构造完 pathlist 后回调 → 在分支模式下往 rel->pathlist 追加一条 CustomPath |
| Plan CustomPath | `CustomPathMethods.PlanCustomPath` | `nodes/extensible.h:L91-L105` | 把 CustomPath 转成 makeNode(CustomScan)；custom_private 塞 `relid` (OID) |
| Register Executor Methods | `RegisterCustomScanMethods` | `nodes/extensible.h:L112-L160` | 注册一张 CustomScanMethods 表：CreateCustomScanState |
| Executor runtime | `CustomExecMethods`（Begin/Exec/End/ReScan 必填；其他 skip）| `nodes/extensible.h:L124-L158` | Begin 一次性跑 2-pass 合并好，Exec 一行一行吐；End 释放；ReScan 重置游标 |

V1 MVP 不做 Parallel、MarkPos/RestorePos、EXPLAIN 详细输出（ExplainCustomScan 不提供，走默认显示）。

### 不可行简化方案（已放弃，仅记录 Why）
- ❌ "View / Rule rewrite 方式把 `SELECT * FROM t` 改成 SRF 调用"：会让 PG 把真实表 RTE 替换掉，和系统目录的 relkind、relid 视图语义冲突；DML UPDATE/DELETE 子计划也找不到 target relation。Plan 阶段 CustomScan 直接替换读节点才是干净的方式，RTE 不变，target 关系也保留。

### Step7 Hard Guard 兼容性（已考虑）
1. **BranchScan 不改变 DML 重定向链路**：原 Step3/4 的 INSERT/UPDATE/DELETE 重定向（在 overlay_ExecutorRun 顶部 per-result-rel guard + redirect 拦截）照常走。ModifyTable 的读子计划（原先是 SeqScan on target rel）被我们换成 CustomScan → 读子计划吐出的是合并结果，而 ModifyTable 自身在 ExecutorRun 入口仍然被 overlay 的钩子按原规则重定向，不冲突。
2. **非支持表必须走原路径**：rte→rtekind 非 RTE_RELATION（VIEW / SUBQUERY / FUNCTION / CTE）、relkind 非 RELKIND_RELATION（matview / foreign table）、无 PK（overlay_relation_has_pk == false）→ 全跳过，planner 走原来的 SeqScan / IndexScan。

3. **成本策略（D-3 A8 MVP 实装，不再是 force-win 极小值）**：
   - **精确公式 cost_branchscan()**（严格按 R11/R12 算法复杂度证明）：
     * Phase A sort+dedup of delta-unique-keys: O(M log M)，对应 startup_cost = cpu_tuple_cost × (M log M + M)
     * Pass1 MAIN tuple × delta 二分查找：O(N log M)，对应 run cost 的 MAIN-hit 部分
     * Pass2 delta-hit latest-wins linear chain resolve + PK pred fastpath 短路
   - **PK equality pred fast-path 分支**：如果 baserestrictinfo 中存在 PK=const 等值下推（single-tuple lookup），
     直接 cost = 4.45..5.15（≈ 2× index scan cost，保证 planner 不回退到 seqscan+filter 全局扫）
   - **add_path dominance silent reject → lappend bypass**（A8 关键修复）：planner add_path 在 BranchScan cost
     ≈ 原路径 4× 时会静默用 dominance 规则把我们的 CustomPath 丢掉（实际我们强制 delta merge 语义必须走
     BranchScan 不能走原 SeqScan）→ 我们在 Planner hook 用 lappend(rel->pathlist, cpath) 直接 append（绕过 dominance），
     然后在 add_path 完成后 prune pathlist：删除所有 non-CustomPath 的 path 条目（保证 100% 选 BranchScan）
   - **Pathlist-prune contract**（A8 MVP 硬保证）：BranchScan 注入后 prune 掉 pathlist 中 SeqScan/SampleScan/IndexScan 等
     所有非 CustomPath 路径 → planner 只剩我们的 CustomPath + 等价 CustomPath 可选 → 100% 叠加语义正确（不会静默回 MAIN heap 读）
   - 验证：L3 planner_cost spec A8.1 SEQ scan cost != 0.00 / A8.2 PK scan << seq / A8.3 SELF-JOIN 2× BranchScan
     nodes 210 对 correctness 全部 ✅。


| 文件 | 改动（增量最小） |
|---|---|
| `contrib/overlay_branch/include/overlay_branch.h` | 在函数签名区（~L129-L155）追加 3 个 public 前向声明：`ob_compute_overlay_slots`（公共 helper，给 SRF 和 CustomScan 共用）、`ob_branchscan_planner_hook`（planner hook entry，_PG_init 里取地址注册） |
| `contrib/overlay_branch/src/overlay_branch.c` | 唯一 C 源：(a) 全局 saved-hook chain + 4 张 static 方法表；(b) Planner hook + PlanCustomPath；(c) 2-pass 公共 helper 抽出；(d) Begin/Exec/End/ReScan 4 个 executor 实现；(e) _PG_init 链入 saved hook chain + RegisterCustomScanMethods |
| `contrib/overlay_branch/test/regress/sql/overlay_branch_user.sql` | 新增 Part G（Transparent SELECT 等价性）+ Part H（DML ModifyTable 读子计划触发 BranchScan + apply 主表精确变化） |
| `contrib/overlay_branch/test/regress/sql/overlay_branch_basic.sql` | 末尾新增 Part G（EXPLAIN 验证、MAIN baseline 无回归、where pk=1 不下推仍正确） |
| `contrib/overlay_branch/test/regress/expected/*.out` | 两条 pg_regress baseline 重生成 0 diff |

## A.3 Implementation Steps（3 切片 MVP，严格顺序依赖）

### Slice 1 — Planner 层：hook + CustomPath 注入 + PlanCustomPath
**目标**：能 `EXPLAIN SELECT * FROM t` 在分支模式下看到 `CustomScan on overlay_branch_branchscan`，不分支时完全看不到。

**步骤**：
1. **header**：`include/overlay_branch.h` 在现有函数签名末尾加：
   ```c
   extern List *ob_compute_overlay_slots(Oid relid, int32 branch_id);
   extern void ob_branchscan_planner_hook(PlannerInfo *root, RelOptInfo *rel,
                                          Index rti, RangeTblEntry *rte);
   ```
2. **src 静态全局 & saved hook**：L73-75（两个 flag 下）新增：
   ```c
   static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook = NULL;
   static const CustomPathMethods  ob_branchscan_path_methods;
   static const CustomScanMethods  ob_branchscan_scan_methods;
   static const CustomExecMethods  ob_branchscan_exec_methods;
   ```
3. **ob_branchscan_planner_hook 实现**（严格顺序，每一步 return 都先调 prev chain）：
   - (a) 先跑 prev hook chain：`if (prev_set_rel_pathlist_hook) prev_set_rel_pathlist_hook(root,rel,rti,rte);`
   - (b) apply / discard 内部 bypass：`if (ob_in_apply_operation) return;`
   - (c) `if (!overlay_branch_is_active()) return;`（MAIN 模式完全不介入）
   - (d) `if (rte->rtekind != RTE_RELATION) return;`（只处理 TABLE RTE）
   - (e) `if (rte->relkind != RELKIND_RELATION) return;`（过滤 matview / foreign table / partition）
   - (f) RelationGetCatalog 获取 rel → `if (!overlay_relation_has_pk(rel)) { table_close; return; }`
   - (g) 构造 CustomPath：
     - `CustomPath *cpath = makeNode(CustomPath);`
     - cpath→path 的 6 个必填：`pathtype=T_CustomPath; parent=rel; pathtarget=rel->reltarget; rows=rel->rows; startup_cost=0; total_cost=(现有最小路径 total_cost * 1e-5)`；`pathkeys=NIL;`
     - cpath→flags = 0；custom_paths=NIL；custom_restrictinfo=NIL；
     - cpath→custom_private = list_make1_oid(rte->relid)；
     - cpath→methods = &ob_branchscan_path_methods；
     - `add_path(rel, (Path *) cpath);`
     - `table_close(rel, NoLock);`
4. **PlanCustomPath 回调**：拿 CustomPath→custom_private 的 relid → `makeNode(CustomScan)`；scan.scanrelid = rti；custom_plans=NIL；custom_exprs=NIL；custom_private = copyObject(path→custom_private)；custom_scan_tlist=NIL；custom_relids=bms_make_singleton(rti)；methods=&ob_branchscan_scan_methods；flags=0；return (Plan*) cscan。
5. **_PG_init hook 链入 + Register**：末尾（现有 GUC 注册后）：
   ```c
   prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
   set_rel_pathlist_hook = ob_branchscan_planner_hook;
   RegisterCustomScanMethods(&ob_branchscan_scan_methods);
   ```
6. **验收（Slice1 单独验收）**：编译 0 warning → pg_ctl restart 15433 → psql：`SET client_min_messages=log; CREATE TABLE bs1(id int pk, v text); SELECT overlay_branch.create_branch('b1'); SELECT overlay_branch.use_branch('b1'); EXPLAIN (COSTS OFF) SELECT * FROM bs1;` → 输出必须包含 `Custom Scan: overlay_branch_branchscan`；RESET overlay_branch.current 后同样 EXPLAIN → 输出应为普通 `Seq Scan on bs1`（CustomScan 消失）。

### Slice 2 — Executor 层：CustomScanState + 拆公共 2-pass helper
**目标**：SELECT * FROM t 真返回和 overlay_main_plus_delta 逐行一致的合并结果。

**步骤**：
1. **拆公共 helper `ob_compute_overlay_slots`**：
   - 把 overlay_main_plus_delta L1542-L1702 整个（从 `branch_id=...` 到 `SPI_finish(); table_close();`）抽出到独立函数，签名：`static List *ob_compute_overlay_slots_internal(Oid relid, int32 branch_id, TupleDesc *out_reldesc)`；返回 `List<TupleTableSlot*>`（TopMCxt 分配，每个 slot 是 TTSOpsVirtual，tuple desc 已经 CreateTupleDescCopy 过独立副本），*out_reldesc 返回该关系的 Relation TupleDesc（用来给 CustomScan 建 ScanTupleSlot）。
   - `overlay_main_plus_delta` SRF 函数重写：内部只调 `ob_compute_overlay_slots_internal`，拿到 result_slots + reldesc → 后面 per-call 的 return 逻辑不变（列名匹配 / SRF_PERCALL_DONE）。好处：两套入口保证结果 bit-exact identical，regression 不出意外。
   - public wrapper `ob_compute_overlay_slots(Oid relid, int32 bid)`：内部 active branch bid 校验失败 ereport。
2. **BranchScanState 结构（static，只在 C 源）**：
   ```c
   typedef struct BranchScanState {
     List         *result_slots;       /* List<TupleTableSlot*> */
     ListCell     *cursor;             /* next slot to return from ExecCustomScan */
     Relation      rel;                /* opened ref, closed in End */
   } BranchScanState;
   ```
3. **CreateCustomScanState**：`makeNode(CustomScanState)` → return；CustomName = `"overlay_branch_branchscan"`。
4. **BeginCustomScan**：
   - cscan = (CustomScan *) node→ss.ps.plan；`Oid relid = linitial_oid(cscan->custom_private);`
   - `int32 bid = overlay_branch_get_current_id();`（非 active → ereport，按 Step1 错误语义）
   - 打开 rel = table_open(relid, AccessShareLock)；reldesc = RelationGetDescr(rel)；
   - `List *result_slots = ob_compute_overlay_slots_internal(relid, bid, NULL);`（复用公共 helper）
   - 存 BranchScanState：`BranchScanState *bss = palloc0(sizeof(BranchScanState)); bss->result_slots=result_slots; bss->cursor=list_head(result_slots); bss->rel=rel;` → `node->custom_ps = bss;`
   - 给 ss.ss_ScanTupleSlot 创建 slot：`TupleDesc sd = CreateTupleDescCopy(reldesc); node->ss.ss_ScanTupleSlot = MakeSingleTupleTableSlot(sd, &TTSOpsVirtual);`
5. **ExecCustomScan**：
   - 取 bss = (BranchScanState*) node→custom_ps；取 slot = node→ss.ss_ScanTupleSlot；
   - `if (bss->cursor == NULL) { ExecClearTuple(slot); return NULL; }`（EOF）
   - `TupleTableSlot *src = (TupleTableSlot*) lfirst(bss->cursor); bss->cursor = lnext(bss->result_slots, bss->cursor);`
   - ExecClearTuple(slot); 逐列 `for a 0..natts-1: slot->tts_values[a] = src->tts_values[a]? datumCopy(...) : 0; isnull 拷贝;` → `ExecStoreVirtualTuple(slot); return slot;`
6. **EndCustomScan**：
   - bss = node→custom_ps；foreach bss→result_slots → `ExecDropSingleTupleTableSlot(item)`；`list_free(bss->result_slots)`；`table_close(bss->rel, AccessShareLock)`；`pfree(bss)`；
7. **ReScanCustomScan**：`bss->cursor = list_head(bss->result_slots);`
8. **验收（Slice2 单独验收）**：重启 postmaster → 打开 t，写入 baseline 3 行 → create_branch / use_branch → 分支 INSERT 1 行 / UPDATE 1 行 / DELETE 1 行 → `psql -c 'SELECT * FROM t ORDER BY id'` vs SRF 调用结果 → `string_agg(concat(id,':',v),' | ' ORDER BY id)` 结果完全相等；RESET 后 SELECT * FROM t 看到 baseline 3 行不变。

### Slice 3 — 回归测试 SQL 追加（2 个 regress 各加）
**目标**：两条 pg_regress baseline 0 diff，BranchScan 的透明读 + DML 驱动 都进入 CI。

**内容**：
1. **overlay_branch_user.sql（用户 happy-path 故事）**：
   - Part G Transparent read：`PASS:USER_BRANCHSCAN_TRANSPARENT_EQ_SRF`（I/U/D 三阶段各比对一次 string_agg 相等）+ `PASS:USER_BRANCHSCAN_RESET_TO_MAIN_BASELINE`（RESET 后主表 3 行 baseline 不变）。
   - Part H DML 驱动：UPDATE t SET v = v || '-driven' WHERE id=1 → overlay_debug_delta_count=4（原 3 行 + UPDATE 新增一条 U 覆盖） → apply → MAIN 精确匹配期望 4 值（id=1→'-driven' 叠加）。
2. **overlay_branch_basic.sql（工程断言）**：
   - Part G BranchScan plan 可见性：
     - `PASS:BS_EXPLAIN_SEQSCAN_IN_MAIN`（MAIN 模式 EXPLAIN 无 CustomScan 关键字）
     - `PASS:BS_EXPLAIN_CUSTOMSCAN_IN_BRANCH`（active branch 下 EXPLAIN 出现 overlay_branch_branchscan）
     - `PASS:BS_WHERE_PK1_RESULT_CORRECT`（WHERE id=1 不做下推，全表扫后上层 Filter，结果仍与 SRF 调 WHERE id=1 一致）
     - `PASS:BS_MAIN_NOT_POLLUTED_AFTER_APPLY_CONFLICT`（Step5 冲突场景回归：branch 写后 MAIN 独立 race write → apply conflict rollback → MAIN 含 race 值）
3. 重生成两个 expected baseline → `pg_regress --use-existing 15433` → All 2 tests passed, 0 diff。

## A.4 Dependencies and Considerations

### 绝对依赖
- PG17 CustomScan / CustomPath / set_rel_pathlist_hook API（headers 已验证存在）
- `shared_preload_libraries='overlay_branch'`：已有硬 prerequisite guard（RAISE EXCEPTION）

### 关键注意项
1. **MemoryContext 安全**：
   - `ob_compute_overlay_slots_internal` 的 List 节点 / slot / datum 全在 TopMemoryContext（沿用 Step4b 的做法）；EndCustomScan 自己显式释放（绝不靠 PG 的 query cxt 隐式回收，避免 0x7F poison SIGABRT）。
   - BeginCustomScan 里 palloc 的 BranchScanState 在 estate→es_query_cxt（默认）→ EndCustomScan 自己 pfree。
2. **apply / discard 内部 bypass（Risk1 mitigation）**：
   - ob_branchscan_planner_hook 顶部第一行 `if (ob_in_apply_operation) return;`，确保 apply 3-pass 内部 SPI 执行的 `SELECT * FROM <main>` 读真实 MAIN（不带 delta 叠加），否则 old_version 冲突检测取错 row 漏判冲突。
   - 已有 `ob_in_apply_operation` flag 用 PG_TRY/PG_CATCH 包住，哪怕 conflict ERROR longjmp 也会恢复，不会让后续用户 branch DML 永久 bypass。
3. **MAIN 模式零侵入**：overlay_branch_is_active()==false 时，planner hook 直接 return。让 PG 完整用自己的 SeqScan / IndexScan 原计划；绝不改变 MAIN 模式下任何行为。
4. **Step7 guard 先于 BranchScan**：DML（UPDATE/DELETE/TRUNCATE/VIEW/CREATE FK 等）guard 在 overlay_ExecutorRun 和 overlay_ProcessUtility 顶部仍先执行。BranchScan 只在"允许的表 + PK 正常"的读路径生效。
5. **DML ModifyTable 读子计划 替换一致性**：
   - ModifyTable 的读子计划原是 SeqScan，现在变成 CustomScan。但重定向 redirect 在 overlay_ExecutorRun 顶部判 ModifyTable 节点后立即拦截，根本不会真进入 ModifyTable → 所以"读子计划是什么扫描"不影响 redirect；redirect 直接把 target tuple 序列化写 delta。
   - 子计划被 CustomScan 替换的真实影响边界**只在 SELECT / RETURNING 子句 / WHERE 子查询读**：这些场景输出合并后的"最新分支视图"是正确行为（用户期望）。
6. **不碰 set_rel_pathlist 其他 hooks**：严格 saved hook chain 模式。下游如果还有别的插件挂了 set_rel_pathlist_hook，先跑 prev 再跑我们。

### 新增：A.4.1 — Planner/Executor 4 个 Bypass Flag（Belt-and-braces）

BranchScan planner hook 和内部 SPI SELECT 通过 4 个互斥的 `bool` flag 保证**不递归、不把 delta 叠加在内部 SELECT 上**：

| flag | 置位位置 | 置位期间的 BranchScan 行为 | 真实风险（无 flag 后果）|
|------|---------|------------------------|-----------------------|
| `ob_in_apply_operation` | apply_branch 3-pass (DEL/UPD/INS) MAIN replay 前 | Planner hook 直接 return，不走 CustomScan | apply 自己内部 SELECT main 时拿到 delta 叠加后的行 → 冲突检测漏判 或 apply 写重复数据 |
| `ob_in_write_redirect` | Write Redirect 主循环 wrapper 前 | Planner hook 直接 return | WR 内部 `SELECT * FROM rel WHERE ctid='...'` 本来要拿 MAIN 原始物理行，被 BranchScan 包了直接 0 行 → old_version 永远 NULL，apply 冲突检测崩溃 |
| `ob_in_overlay_helper` | `ob_compute_overlay_slots_internal` 跑 MAIN SPI SELECT 前（2-pass helper） | Planner hook 直接 return | 2-pass helper 内部的 MAIN seqscan 再被 BranchScan 包一层 = 无限递归 StackOverflow + 结果永远多一倍 |
| `ob_in_guc_setconfig` | GUC `overlay_branch.current` check hook 内同步 GUC 前 | `use_branch(NULL/name)` 不会被误触发二次 guard | GUC check → use_branch → SetConfigOption → check hook 重入死循环 |

### 新增：A.4.2 — Dummy baserel 空 pathlist 保护 (newlist != NIL)

Planner 看到 `WHERE id = NULL`（Const NULL = never true）或 `WHERE id IS NULL` 且 PK is NOT NULL → baserel 只产生 rows=0 的 dummy `Result` path，seqscan/indexscan 都不加，rel->pathlist **非空但只有 dummy**。早期 MVP 直接 `rel->pathlist = newlist`（newlist=NIL if PK no columns matched） → pathlist 全空 → Planner 直接 fatal `could not devise a query plan` 崩整个 postmaster。

**最终固化实现：**
```c
/* 只在有实际注入的 CustomPath 时 override */
if (newlist != NIL)
{
    /* MVP:  purge non-CustomPath entries (IndexScan/Sort 成本会赢导致 delta 漏读) */
    rel->pathlist = newlist;
}
/* 否则完全保留原 pathlist（只有 dummy Result 的时候也保留，不崩）*/
```

典型触发（已在 Section L.3 `UPDATE WHERE id=NULL` 断言覆盖）：
```sql
UPDATE bs_pu SET v = 'x' WHERE id = NULL;   -- 0 row (dummy baserel + newlist=NIL)
-- 不保护 → Planner fatal; 保护后 → 0 rows updated, 静默 OK
```

## A.5 Validation（每片必过）
| 片 | 验证 |
|---|---|
| 通用 | `make install 2>&1 \| tail -3` → 0 warning、0 error |
| Slice1 | 见 Slice1 末尾 EXPLAIN 验收；MAIN 下 Seq Scan；branch 下 CustomScan |
| Slice2 | 见 Slice2 末尾 string_agg 完全相等；RESET 后 baseline 3 行 |
| Slice3 | `pg_regress --use-existing --host=/tmp --port=15433 overlay_branch_user overlay_branch_basic` → All 2 passed；cat regression.diffs → 空 |
| MAIN 不变量回归 | apply no-conflict 精确匹配 / conflict rollback 保留 MAIN race 值 / discard cascade MAIN baseline 3 行（Step5 F1/F2、Step8 Part E 断言保留在 basic.sql 末） |

## A.6 Risks

| # | 风险 | 发生后果 | Mitigation |
|---|---|---|---|
| R1 | apply 内部读 MAIN 被 BranchScan 叠加 delta → 冲突检测 false-negative → MAIN 静默污染 | 最严重的正确性 bug | Slice1 ob_branchscan_planner_hook 顶部 `if (ob_in_apply_operation) return;` 第一行硬判 |
| R2 | result_slots / List 节点分配在 SPIProc cxt → SPI_finish 释放 → use-after-free → SIGABRT 0x7F | Step4 曾 hit 过同根因 | `ob_compute_overlay_slots_internal` 严格沿用 Step4b 做法：list / slot 分配前 MemoryContextSwitchTo(TopMemoryContext)；EndCustomScan 自己释放 |
| R3 | add_path 的成本设太低？其他插件自定义路径没机会被选 | 非 bug；分支模式下叠加读就是 desired | 是 desired；1e-5 * 现有最小路径 总成本，确保不管 planner 怎么 tie-break 都选我们 |
| R4 | 无 PK / VIEW / matview / foreign table 误拦截 | 用户正常 SQL 被我们的 plan 吃了 → 读不到正确数据 | Slice1 5 条 filter：(c)(d)(e) rtekind / relkind / has_pk，每条不满足立即 return |
| R5 | ss_ScanTupleSlot TupleDesc 不匹配 rel → 上游 projection 错位 | client 看到字段串位 / cassert tdtype | BeginCustomScan 严格 `CreateTupleDescCopy(reldesc)`；Scan 的 ScanTupleSlot tupdesc=这个独立副本；和 ExecCustomScan 填充的 natts 完全一致 |
| R6 | ReScanCustomScan 忘记重置游标 → 第二次取返回空 → 嵌套循环 join/NLParam 结果不全 | plan 节点 重跑结果错 | ReScan 明确 `bss->cursor = list_head(bss->result_slots)` |

## A.7 非目标 Out-of-scope（V1 绝对不做）
- ❌ WHERE qual 下推到 CustomScan 内部（V1 全表扫，上层通用 Filter 过滤即可）
- ❌ Join 下推 / parameterized nested-loop key 下推
- ❌ Parallel worker CustomScan
- ❌ MarkPos / RestorePos cursor 可滚动语义（V1 ReScan 支持就够）
- ❌ EXPLAIN ANALYZE 实际行数 / custom 打印
- ❌ Partition / FK 级联子表的 BranchScan 透明叠加（Step7 guard 已经 block 建这些）

---

# Part B — P0/P2 优化策略

> 对应 Planner hook 里 baserestrictinfo 的 `OpExpr(Var(PK col), Const)` 识别 & 非 PK 条件 deparse。
> 修复前版本存在"手动按 PK type 调 output function"导致的 corruption。

## B.1 功能定位

当 BranchScan 透明视图的顶层 WHERE 出现形如 `pkcol = 2` / `2 = pkcol` 的**单列等值谓词**时，启用两层 O(1) 快路径：

| 层 | 位置 | 作用 |
|----|------|------|
| L1 MAIN | `ob_compute_overlay_slots_internal` 拼 MAIN SPI SELECT | 把 `WHERE pkcol = <val>` **直接下推到 MAIN heap 的 SPI**，MAIN 返回最多 1 行（PK 唯一性），避免全量 seqscan |
| L2 DELTA | helper 内部 Pass1 lookup | 不调 `overlay_delta_list_for_rel`（O(N) 全量扫），直接调 `overlay_delta_lookup(branch_id, relid, serialized_key)` O(1) 精确命中 |

两层配合，典型场景 10x~100x 读延迟下降（取决于 MAIN 大小）。

> 正确性兜底：L1 下推只做"筛 MAIN 行数"，**CustomScan 顶层 plan.qual 对应的 ExecQual 永远再跑一次**（`ExecBranchScan` L962）。所以即使下推 SQL 写坏、漏写、写多、写少，最终行图景仍和 SRF bit-exact 等价（最多 MAIN 多拉几行，结果不会错）。

## B.2 修复前 Bug 实锤（V1 deparse 的 4 个致命问题）

原 V1 手动拼 SQL 的代码位于 `branch_scan.c:L519-L549`（修复前版本，已归档），流程：

```c
typid = pkatt->atttypid;                                 // 取 PK 列自身的 type，通常是 int8
getTypeOutputInfo(typid, &outfunc, &typisvarlena);       // outfunc = int8out
valstr = OidOutputFunctionCall(outfunc, con->constvalue); // 错！！
// con->consttype == INT4OID（字面量 2 是 int4 Const），con->constvalue 是 4-byte Datum
// 把 4-byte 值传给 int8out → 按 8-byte 解读 → high 4 bytes 是栈上 garbage
// 结果：valstr = "140671778252802" 之类乱码或直接 segfault
```

### 四个独立 Bug：

| # | Bug | 触发条件 | 症状 |
|---|-----|----------|------|
| 1 | **类型错位：按 PK type 输出 Const Datum** | PK 是 int8/bpchar(8)/uuid 等任何 ≠ 字面量 Const 的类型（非常常见，`CREATE TABLE t(id bigint PRIMARY KEY); SELECT * FROM t WHERE id=2;` 字面量 2 是 int4 Const） | pk_where_sql 里 `'乱码'::bigint` → `invalid input syntax for type bigint` → helper SPI `SPI_execute` ERROR → 整个事务 abort（pg_regress exit code 2 就是它） |
| 2 | **format_type_be 返回 static buffer** | 未来只要 deparse 链里在同一个 Planning cycle 第二次调 format_type_be，就会把刚才拼进 pk_where_sql 的类型名**直接覆写**掉（static char[N] 在 `src/backend/utils/cache/lsyscache.c` 里） | WHERE `id = '2'::bigint` → 下次 format_type_be('text') 覆写 → WHERE `id = '2'::text??` → 类型 cast 错位，随机 ERROR 或 silent wrong result |
| 3 | **pass-by-ref 非 varlena 类型生命周期** | PK 类型是定长非 byval（如 uuid = 16 byte pass-by-ref、numeric(4,0) = BINARY 8 字节、date = int32 byval 安全但 timestamp = int64 byval 安全 64-bit …）→ `con->constvalue` 是指向 parser context 的指针；Planner 释放该 context 后 deref 就是 UAF（use-after-free） | 随机 garbage 或 SIGSEGV，仅当 PK 非 int4 时命中 |
| 4 | **自定义类型 output function 抛 ERROR 无保护** | 某自定义类型的 outfunc 里 throw elog(ERROR) → 外层没 PG_TRY → 整个 Planner hook 直接 propagate 到用户面报错 "unexpected deparse error in PK pushdown" | 无法回退到 seqscan（回退才是 MVP 期望语义） |

## B.3 修复后 V2 deparse 设计（最终方案）

**核心原则：用 ruleutils `deparse_expression` 反编译**整个 `OpExpr(Var(PK col), Const)` 节点，**绝不按 PK type 单独手动调 output function**。P2 非 PK 下推已经用这条路径（L616-L637）跑通 Section E 全量断言，证明 robust。

### 流程：

```
1. baserestrictinfo 遍历识别出 OpExpr(Var(PK)=Const/Const=Var(PK)) → opexpr（和原来 P0 一模一样）
2. 只要类型匹配判定通过，不手动拼 pk_where_sql
3. 而是直接：
     dpctx = deparse_context_for(RelationGetRelationName(reln), rte->relid);
     PG_TRY(); {
       deparsed = deparse_expression((Node*) opexpr, dpctx, false, false);
     } PG_CATCH(); {
       FlushErrorState();   // ← 关键，自定义类型 error 时清栈
       deparsed = NULL;
     } PG_END_TRY();
4. 输出校验：deparsed != NULL && *deparsed && strlen<65536
     ✓ → has_pk_pred = true; pk_where_sql = pstrdup(deparsed);
     ✗ → has_pk_pred = false; 回退 seqscan 全量，绝不让 error propagate
5. pk_serialized_key 仍然走 overlay_serialize_pk_from_single_datum()
     （该函数内部用 SPI to_jsonb(ARRAY[val::text]) → 不依赖 output function
       Datum 的类型和长度完全由 val 自身决定，它是和 Write Redirect 序列化 key 同一条已验证路径）
```

### 为什么 V2 deparse 能解决 4 个 Bug：

- **Bug 1 类型错位**：`deparse_expression` 看到 Const 节点时直接读 `Const->consttype` 调对应的 outfunc（字面量 `2` 看到 consttype=INT4OID → int4out → "2"），和 PK 类型无关；然后 PG parser 再隐式 coerce 到 PK 列类型，和用户手写 `WHERE id=2` 完全一致。
- **Bug 2 format_type_be static**：`deparse_expression` 内部 cast 类型名用的是 `deparse_context_for` + `generate_relation_name`（TopMCxt palloc），或直接 `quote_qualified_identifier`（palloc），无 static buffer 依赖。
- **Bug 3 UAF**：Const 节点本身在 Planner 的 `PlannerGlobal->planner_cxt`（活过整个 plan 生成周期，直到 plan 交给 executor）里；Planner hook 里做 deparse + pstrdup 时还在同一 context，pstrdup 拷贝到 CurMCxt 后交给 custom_private（makeString 也深拷贝），不会 UAF。
- **Bug 4 outfunc ERROR**：PG_TRY/PG_CATCH 包住，任何 ERROR 都 FlushErrorState 后 `has_pk_pred=false` 安全回退 seqscan。

## B.4 与 P2 非 PK 下推的协同策略

Planner hook 执行顺序（不可改变，否则 Section E "id=12 AND color='red'" 类组合谓词重复下推）：

```
[1] 先扫 baserestrictinfo 识别 P0 PK=Const
       → 命中则 has_pk_pred=true + 记录 is_pk_clause
[2] 再扫 baserestrictinfo（第二遍）做 P2
       → foreach 时重新跑一遍 P0 的完全一致识别算法找 is_pk_clause
       → is_pk_clause 的那个 RestrictInfo 被排除出 remain list
       → 保证 P0 已消费的条件不会再进 general_where_sql 做 AND 组合
```

这样 `WHERE pk=12 AND color='red'` 最终：
```sql
SPI MAIN SELECT: SELECT * FROM rel WHERE (pk=12) AND (color='red')
pk_where_sql   = "pk = 12"          → 反编译 OPEXPR 结果
general_where  = "color = 'red'"    → P2 deparse 剩下的 AND 条件
```
**没有 `pk=12 AND pk=12 AND color='red'` 的重复。**

## B.5 Executor 解包协议（custom_private 6-element，v3 最终版）

| index | node type | 内容 | malformed 回退 |
|-------|-----------|------|----------------|
| 0 | String | relid 十进制字符串（兼容性占位，实际不用，取 relid 从 scan.scanrelid→rte） | 忽略 |
| 1 | Integer | pkflag (0/1) | `IsA(nde,Integer)?intVal(nde):0`；任何类型错 → 0 → `has_pk_pred=false` |
| 2 | String | pk_where_sql（deparse_expression 结果；flag=0 时为空串） | `!*gwstr` → has_pk_pred=false，回退 seqscan（保留 pk_serialized_key 也没用） |
| 3 | String | pk_serialized_key（和 Write Redirect `overlay_serialize_pk` 格式完全兼容） | 空串 → has_pk_pred=false |
| 4 | Integer | gwflag (0/1) P2 非 PK | 类型错 → 0 → has_general_where=false |
| 5 | String | general_where_sql P2 反编译结果 | 空串 → has_general_where=false |

**向后兼容硬约束**：list_length(cpriv) == 4 时（P2 代码加入前的旧版本），index 4/5 不存在 → BeginCustomScan 直接 fallthrough `has_general_where=false`，永不崩。

### Executor 解包关键修复：
- **不得使用 `lsecond_int / list_nth_int`**：这些宏（或函数）的语义是"把 ListCell 的 `ptr_value` 当作 int 读"，而 List 里存的是 `Integer *` 节点指针。**必须用 `intVal(lsecond(...))` 和 `intVal(list_nth(cpriv, 4))`** 先解指针再取 ival。lsecond_int 在 PG17 里直接宏展开到 `(int)(intptr_t) lsecond(l)` → 截断 Integer* 成 int（64-bit 下高位丢）。之前 exit code 2 连续 3 次崩的真根因就是这个（不是 deparse，是 Executor 越界读 Assert）。

## B.6 错误回退矩阵（保证永不崩、永不静默错）

| 阶段 | 错误 | 回退动作 | 结果 |
|------|------|----------|------|
| Planner P0 识别 | 非 Var(Const) / 非 PK attno / 非 btree eq_opr | has_pk_pred=false，跳过快路径，P2 照常扫 | MAIN seqscan + L2 delta list；正确性 OK，性能回退到 V2 基础 |
| Planner P0 deparse_expression | outfunc ERROR（自定义类型） / 结果 NULL / 长度>64KB / 空串 | has_pk_pred=false，pk_where_sql=NULL，pk_serialized_key=NULL | 同上 |
| Planner P2 deparse_expression | outfunc ERROR（any 节点） | has_general_where=false，**P0 快路径继续**（两条路径独立） | 只 MAIN seqscan 没筛 non-PK 行；但 MAIN 行数被 P0 已经限到 1 行，实际无性能损失 |
| BeginCustomScan 解包 | custom_private 任一项类型错 | has_pk_pred/has_general_where 对应 flag 置 false | 自动回退，永不 SIGSEGV |
| Executor SPI MAIN SELECT | pk_where_sql 语法错 / 类型错（理论不发生，但兜底） | SPI 正常 error propagate 到 SQL 事务回滚（和普通 `WHERE id='not_a_num'::int8` 行为完全一致） | 不是静默 bug，是明确 ERROR，安全 |
| Delta O(1) lookup | pk_serialized_key 和 delta 表 key 格式不兼容 | overlay_delta_lookup 找不到 → 等价于"该 PK 没 delta 修改" | 仍正确（只是 L2 O(1) 命中退化成"no delta found"），MAIN 行照常回；最多漏 UPDATE/INSERT/DELETE 但不会，因为序列化格式已和 WR 对齐 |

## B.7 验证断言列表（对应 regression Section A+B PK 部分 & Section C RETURNING 不回归）

| Test | 内容 | 通过标准 |
|------|------|----------|
| ADV_A1 | EXPLAIN SELECT * FROM bs_a WHERE id=3 | 只输出 "Custom Scan on bs_a"，无任何 IndexScan/SeqScan 泄露 |
| ADV_A2 | id=3 纯 PK SRF vs BS 行图景相等 | 'PASS:SRF_EQ_BS' （id=3 UPDATE 后 BS 看到 WR 写的 delta new 版本） |
| ADV_A3 | id=9999 不存在的 PK，SRF=BS 都 0 rows | PASS:EMPTY_AGREE |
| ADV_A4 | id=5 DELETE tombstone，SRF=BS 都没行 | PASS:TOMB_AGREE |
| ADV_B1 | 多条件 id=7 AND v='XXX'（PK + non-PK） | SRF_EQ_BS；P0 命中 + P2 剩余 v=XXX AND 组合 |
| ADV_B2 | 仅 PK，ORDER BY id ASC 下 SRF_EQ_BS | 同上（即使 CustomScan 本身不排序，ORDER 由上层 Sort 节点处理） |
| ADV_C2-C8 | RETURNING 子句 + id=N WHERE 快路径启用 | 16 条 PASS 断言全绿（P0 启用后 WR delta 的 pk=id=X 必须能被 O(1) 查到，不能和 WR 序列化 key 错位） |
| Pristine baseline | RESET MAIN → 原始 3/4/8 rows 正确 | A-D-E 尾段 "RESET 后零污染" 断言全 PASS |

---

# Part C — Delta O(1) lookup MemoryContext 生命周期硬约束

> 本节记录 P0.2.3 调试整整 10h+ 的真实 Bug，防止未来重写 lookup / list_for_rel 时再次踩坑。

## C.1 现象

- `overlay_delta_lookup()` 的 WHERE `key='["3"]'` **明确能找到 1 行**（`SPI_processed=1`，`SPI_getvalue(col3)` 打印 `["3"]` 完全正确）
- 但 lookup 返回给调用者后，`dt->key` 在 `branch_scan.c:290` 打印出来却是 **`‹ ›`（只有空格 / 垃圾字符）**
- 结果 `strcmp(dt->key, pk_key)` 永远不匹配 → `match==NULL` → DELETE tombstone 跳过、UPDATE green 不 override → `bs67 FAIL:ID3_STILL_PRESENT`、`ADV_A id=10 FAIL:10:red`
- 更诡异：**完全相同的 `SPI_getbinval + TupleDescAttr + datumCopy + TextDatumGetCString` 代码**，写在 `overlay_delta_list_for_rel()` 里就 100% 正确，写在 `overlay_delta_lookup()` 里就 100% 错 → 排除索引、typbyval、序列化格式等所有其他怀疑对象。

## C.2 根因（实锤）

`overlay_delta_list_for_rel()` 的 294 行有一句**被作者忽略了三个月的关键切换**：

```c
oldmc = MemoryContextSwitchTo(TopMemoryContext);   // ← 生命线！
for (i = 0; i < SPI_processed; i++) {
    dt->key         = TextDatumGetCString(datumCopy(d, typbyval, typlen));
    dt->old_version = TextDatumGetCString(datumCopy(d, typbyval, typlen));
    dt->tuple_data  = (bytea *) datumCopy(d, typbyval, typlen);
}
MemoryContextSwitchTo(oldmc);
```

而 `overlay_delta_lookup()` 在修复前**完全没有这对切换**，于是：

1. `ob_spi_one_shot()` → 内部 `SPI_connect()` 把 CurrentMemoryContext 切到 **SPI proc 上下文**（一个短生命周期的内存池）
2. 所有 `datumCopy()`、`TextDatumGetCString()`、`pstrdup()` 的 palloc 都落在 **SPI proc** 里
3. lookup 函数末尾调用 `SPI_finish()` → **整个 SPI proc 上下文被 `MemoryContextDelete()` 暴力释放**
4. 返回给调用者的 `out_tuple->key / old_version / tuple_data` 三个 char* 指针 **全部悬垂**，指向的内存要么已清零、要么被后续 palloc 覆盖（典型表现是只剩空格、乱码）
5. 但 `SPI_getvalue()` 打印是在 `SPI_finish()` 之前，所以看到的结果永远"看起来正确" → 误导调试方向

## C.3 修复（写此文档强制固化规则）

**任何**在 SPI proc 内 palloc、又需要在 `SPI_finish()` 之后继续使用的内存，必须：

```c
oldmc = MemoryContextSwitchTo(TopMemoryContext);   // ← 任何版本都不许漏
/* datumCopy / TextDatumGetCString / pstrdup / palloc0 全部放这里 */
MemoryContextSwitchTo(oldmc);                       // ← SPI_finish 前切回
SPI_finish();
```

且此规则适用于：
- `DeltaTuple.key`（TEXT → C 字符串）
- `DeltaTuple.old_version`（TEXT → C 字符串）
- `DeltaTuple.tuple_data`（BYTEA → varlena 拷贝）
- 任何 SPI proc 分配后需要"活过 SPI_finish"的对象

## C.4 对比检查清单（写代码时逐项对齐）

| 检查项 | list_for_rel | lookup（修复前） | lookup（修复后） |
|--------|:---:|:---:|:---:|
| MemoryContextSwitchTo(TopMemoryContext) | ✅ | ❌ 缺失 | ✅ |
| datumCopy / pstrdup 发生在 Top | ✅ | ❌ SPI proc 内 | ✅ |
| SPI_finish 后 dt->key 仍有效 | ✅ | ❌ 悬垂 | ✅ |
| bs67 DELETE tombstone skip | — | ❌ FAIL | ✅ PASS |
| ADV_A id=10 UPDATE green override | — | ❌ FAIL | ✅ PASS |

---

# Part D — 阶段 2 读路径架构（去 SPI / 流式早停）

> 消除 MAIN 基线读取的 SPI 中间物化与结果集全量物化，使大表分支读接近原生扫描，
> 并让 `LIMIT` / 游标能早期终止。**不改** planner hook 的 P0/P2 谓词识别逻辑
> （custom_private 6 元素协议保持不变）。

## D.1 现状与瓶颈

| 环节 | 阶段 2 前的现状 | 代价 |
|---|---|---|
| MAIN 基线读取 | `ob_spi_one_shot("SELECT * FROM rel [WHERE ...]")` | SPI 把整张结果集物化进 `SPI_tuptable`，再逐列 `SPI_getbinval` + `datumCopy` 拷入 virtual slot = **两次全量拷贝** |
| 结果集 | 首次 `ExecCustomScan` 即全量叠加进扫描上下文（`ebs->materialized` 游标） | `LIMIT` / 游标 / `EXPLAIN ANALYZE` 无法截断 |
| 每行 PK 文本键 | `overlay_serialize_pk()` 一条 `to_jsonb(ARRAY[...])` SPI | ≈8.4 µs/行；读 Pass1 strcmp 回退、写路径、apply 共用 |

**三处均已在阶段 2 消除**：RD-8（PK 键纯 C 化，§D.2）、RD-7（MAIN 去 SPI，§D.3）、RD-6（流式迭代器，§D.4）。

## D.2 RD-8 收尾 — `overlay_serialize_pk` 纯 C 化（**已完成**）

**输出契约（必须字节不变）**：`to_jsonb(ARRAY[e1::text, ...])::text`
→ `["v1", "v2", ...]`；NULL 元素 → `null`；元素 = PK 列 output function 结果
（BPCHAR 先 r-trim，见现行注释）经 JSON 字符串转义。多列 PK 依序拼接。

**实现**：`[` + 逐列（`", "` 分隔——jsonb 文本数组元素以「逗号+空格」连接；
isnull → `null`；否则 `escape_json(buf, valstr)`）+ `]`。
`escape_json()`（`utils/json.h`）即 jsonb 的字符串转义实现
（`jsonb_put_escaped_value`），故字节一致。去掉 SPI 后该函数不再依赖
ActiveSnapshot / SPI proc 上下文；写路径
（[L1623](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L1623) /
[L1729](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L1729) /
[L1754](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L1754)）与 apply 同步受益，
且顺带消除 Part C 那类“SPI proc 悬垂指针”风险面。

**验证**：L3 / L1 全绿。读侧 Section F 覆盖空 delta / all-typed delta 快捷路径，
Section G 新增 JSON 转义用例（PK 或列值含 `"` 与 `\`）——该用例同时暴露并修复了一个
**既有写侧缺陷**（见下）。

**连带修复｜`pg_branch_delta.tuple_data` 写入转义**：为覆盖转义路径而新增的用例立即
触发 `invalid input syntax for type bytea`。根因：`overlay_delta_insert`
（[delta_store.c](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L206-L238)）
用 `quote_literal_cstr(json)` + `(%s)::bytea` 存行 JSON，而 `::bytea` 按 **legacy escape 格式**
解析，`\"` 直接报错、`\\` 折叠为单字节——凡列值含 `"` 或 `\` 的分支写都会失败或静默损坏。
改为直接拼 `'\x<hex>'::bytea`（与读侧 `convert_from('\x..'::bytea,'UTF8')` 对称），
逐字节保真，且兼容 `overlay_debug_delta_insert` 存储任意二进制（如 `DECODE('feedface','hex')`）。

## D.3 RD-7 — MAIN 基线读取去 SPI（**已完成**）

把 [branch_scan.c L278-L330](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L278-L330)
的 SPI SELECT + 结果读取，替换为直接堆扫描：

- **快照**：沿用现有语义——SNAPSHOT 模式下用 `CurrentBranchContext->branch_main_snapshot`，
  否则 `GetActiveSnapshot()`；直接传给 `table_beginscan`（不再需要 push/pop，因为不再借道 SPI）。
- **扫描**：`table_beginscan(rel, snap, 0, NULL)` + 逐行 `table_scan_getnextslot()`；
  探针 slot 用 `TTSOpsBufferHeapTuple`（`MakeSingleTupleTableSlot(rel_desc, &TTSOpsBufferHeapTuple)`）。
- **预过滤**：用 `node->ss.ps.qual` 对应的 `ExprState`（= baserestrictinfo 全套）对 MAIN 行**预筛**，
  与现行 P0+P2 下推语义等价：MAIN 行不满足则不进 Pass1；被分支改过的行由 Pass2 orphan 规则补回；
  最终 `scan.plan.qual` 仍照常对叠加结果复筛
  （[L1862](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L1862)）→ 正确性兜底不变。
- **P0 点查例外（保留 SPI）**：`has_pk_pred` 时仍走原 SPI SELECT（≤1 行，无物化成本），
  以复用 planner 的 PK 索引能力。**理由**：手搓 btree `ScanKey` 需自行完成
  字面量 → PK 列类型的严格 coercion（BPCHAR / NUMERIC / collation 跨类型），
  一旦 key 不匹配会**静默返回 0 行**（Pass2 只补 delta 行），风险远高于收益；
  而该路径只命中 1 行，去 SPI 的收益近乎为零。
- **辅助列**：不再需要 `pk_where_sql` / `general_where_sql` 的 SQL 文本（改由 qual 预筛），
  仅保留 `pk_serialized_key` 供 delta O(1) lookup；custom_private 协议不新增/不删元素（兼容）。

### D.3.1 实现要点（踩坑记录）

1. **virtual slot 上编译的 qual 不能直接跑在 buffer-heap 探针 slot 上**。
   CustomScan 的 scan slot 是 `TTSOpsVirtual`（nodeCustom.c），于是 `ExecComputeSlotInfo()`
   认定「slot 恒为 virtual → 永不需 de-form」，**整条 `EEOP_SCAN_FETCHSOME` 被优化掉**；
   `EEOP_SCAN_VAR` 遂按 `scanslot->tts_values[attnum]` 直取，并 `Assert(attnum < tts_nvalid)`。
   把未物化的 `TTSOpsBufferHeapTuple` 探针 slot 塞进 `econtext->ecxt_scantuple` 会触发该断言 /
   越界（`execExprInterp.c` EEOP_SCAN_VAR）。**修法**：`ExecQual` 之前先
   `slot_getsomeattrs(probe, natts)` 手工物化（等价于 `cesscan` 自己的 de-form）。
2. **P0 分支的 SPI 上下文会吞掉刚建好的行**。`ob_spi_one_shot()` 会 `SPI_connect()`，
   而 `SPI_connect()` 把 `CurrentMemoryContext` 重挂到 SPI proc 上下文——`SPI_finish()`
   一调用就整块销毁。RD-7 后 P0 分支把 `SPI_finish()` 前移到了「读完 tuptable」处，
   因此**逐行物化的 mslot / `lappend` 的 List 必须先切回 `result_cxt`**（否则 Pass1 读到
   `0x7f7f…` clobber 崩溃）。旧代码之所以没炸，只是因为它的 `SPI_finish()` 恰好在最末尾。
   非 P0 直扫分支无 `SPI_connect`，天然在 `result_cxt`，无需切换。

## D.4 RD-6 — 按需吐行（流式 + 早停）（**已完成**）

叠加读路径由「先全量物化 `List<TupleTableSlot*>`」改为**迭代器**：

- `OverlayScanState` + `overlay_scan_open()` / `overlay_scan_next()` / `overlay_scan_close()`
  （[branch_scan.c L371 / L642 / L885](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L371-L910)）：
  持有 `slot_desc`、排序后的 delta 指针数组、MAIN 侧读取源（P0 SPI 单元子行 / 非 P0 `TableScanDesc`）、
  Pass1/Pass2 进度。Pass1 按需取一行 MAIN 行 → 匹配 / 重建 → 返回；堆扫尽后进入 Pass2
  逐条补发未命中的 delta I/U 孤儿 → 返回；再 `NULL`。**迭代顺序与旧的「全量物化」逐字节一致**。
- `ob_branchscan_exec()`（[L1974](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L1974)）
  懒打开迭代器，每次只拉一行 → 上层 `Limit` 停止调用即可早停；游标跨多次 Exec 复用同一迭代器。
- SRF `overlay_main_plus_delta` 改为流式句柄 `ob_overlay_srf_open` / `ob_overlay_srf_next` /
  `ob_overlay_srf_close`（[L1791](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L1791-L1835)），
  对外结果集不变；`LIMIT` 截断时由 `MemoryContextRegisterResetCallback` 保证 handle 被释放。
- **MarkPos / RestorePos 不实现**：`CustomPath.flags` 不含 `CUSTOMPATH_SUPPORT_MARK_RESTORE`，
  exec methods 表对应槽位留 NULL（前向堆扫无法回退）。planner 会在需要时自动插入 `Materialize`
  （`ExecSupportsMarkRestore()` / MergeJoin inner path）。
- **正确性**：Pass1 未结束前不进入 Pass2；`LIMIT` 截断只影响「无 ORDER BY 时的任意子集」语义
  （SQL 标准允许），不破坏任何有序 / 聚合查询（它们会拉干）。
- **关联子计划不参与静态下推**：`baserestrictinfo` 中含 PARAM_EXEC / 外层 Var 的 clause 由
  [`ob_expr_has_outer_ref()`](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L927-L946)
  在 deparse 前剔除。这类 clause 既无法反编译成独立 SQL 片段，又会让 `ruleutils.get_parameter()`
  以 NULL plan 解引用 `dpns->plan`（`deparse_context_for()` 留 `plan == NULL`）而**崩溃**；
  剔掉后由 `scan.plan.qual` 逐行复筛。相关子计划的 rewind 靠 ReScan 重建迭代器。
- **内存生命周期**：迭代器与所有对外 slot 归属 `scan_cxt`（BranchScan）/ `multi_call_memory_ctx`（SRF）；
  `End` / `ReScan` 释放（[`overlay_scan_close`](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L885-L910) + `MemoryContextDelete` / `MemoryContextReset`）。

## D.5 实施切片（每片独立 Green-Bar + 本地 commit）

1. **2a**：RD-8 收尾（纯 C `overlay_serialize_pk`）——**已完成**（含 Section G 转义用例
   及其暴露的 `tuple_data` bytea 写入修复）。
2. **2b**：RD-7（去 SPI 直扫 + qual 预筛；P0 保留 SPI）——**已完成**
   （新增 `scan_pk` Section H：全扫 / 预筛 orphan 补回 / 复筛兜底 / P0 点查 / SRF 等价 / LIMIT）。
3. **2c**：RD-6（迭代器 + 早停）——**已完成**（新增 `scan_pk` Section I，见 §D.6）。

## D.6 验证与风险

- 每片：2× clean rebuild 0 warning；L3 17/17；L1 12/12 `diff -r` 0 行；`LC_ALL=C.utf8`。
- **RD-7 已完成**，`overlay_branch_scan_pk` 新增 **Section H**（8 用例，全 PASS）覆盖：
  无谓词全扫（`RD7_FULLSCAN`）、非 PK 预筛 orphan 补回（分支改动落在谓词列，
  `RD7_PREFILTER_ORPHAN`）、`scan.plan.qual` 复筛兜底（`RD7_REQUAL_NET`）、
  P0 点查结果 + tombstone 消失（`RD7_P0_BRANCH` / `RD7_P0_TOMBSTONE`）、
  SRF 与直读结果一致（`RD7_SRF_EQUIV`）、`LIMIT`（`RD7_LIMIT`）、MAIN 零污染（`RD7_MAIN_CLEAN`）。
- **RD-6 已完成**，`overlay_branch_scan_pk` 新增 **Section I**（9 用例，全 PASS）覆盖：
  拉干全扫（`RD6_STREAM_FULL`）、Pass1 内早停（`RD6_LIMIT_PASS1`）、
  跨 Pass1→Pass2 边界早停（`RD6_LIMIT_PASS2`）、游标跨多次 Exec 复用（`c_rd6` 四次 FETCH）、
  SRF 早停释放（`rd6_srf_limit_rows`）、SRF 与直读等价（`RD6_SRF_EQUIV`）、
  **相关子计划 rewind**（`rd6_rescan_ok`）、MAIN 零污染（`RD6_MAIN_CLEAN`）。
- 风险 ① 预筛语义等价：已由 Section H 的“预筛 orphan 补回 + 复筛兜底”组合用例实测通过
  （被预筛剔除但被分支改写的行由 Pass2 orphan 规则补回）。
- 风险 ② 堆扫描 slot 生命周期：已收敛为两条硬规则——探针 slot 用 `TTSOpsBufferHeapTuple`
  且**只**追 `tts_tid` / raw xmin，输出 slot 一律 `slot_desc` 的 `TTSOpsVirtual`；
  预筛前必须 `slot_getsomeattrs`（见 D.3.1）。
- 风险 ③ 早停下的结果集顺序：RD-6 严格保持「Pass1 走 MAIN 基线序，Pass2 走 delta append 序」，
  故任何拉干查询（聚合 / ORDER BY / 全扫）结果与阶段 2 前**逐字节相同**；只有无 ORDER BY 的
  `LIMIT` 取的是任意前缀（SQL 允许）。
- 风险 ④ 关联谓词下推：见 §D.4 最后一条——PARAM_EXEC / 外层 Var 一律不下推，交由执行器复筛。

---

# Part E — P0 谓词下推扩展（`IN` / 范围 / `OR`）

> P0 的 L1（MAIN SPI 谓词）与 L2（DELTA O(1) key）在 Part B 里是**同一个 `has_pk_pred` 开关**，
> 且只在「单列 PK = 常量」时才启用。本 Part 把 L1 的识别面放宽到任意**只由 PK 列构成的
> btree 比较**（等值 / 范围 / `IN` / 其 `AND`/`OR` 组合），同时把 L2 明确降级为「仅单点等值才
> 走 O(1)」，其余走全量 delta 列表。**SPI 机制不变**。

## E.1 目标与边界

- **要做的**：把「MAIN 侧能被 PK 谓词收窄」的判定，从 `pk = Const` 推广到
  `pk = c` / `pk <> …` 之外的 btree 五算子（`= < <= > >=`）、`pk IN (…)` / `= ANY(array)`、
  以及这些子句的 `AND` / `OR` 组合。命中即把整段谓词 deparse 成 SQL 文本，透传给 MAIN SPI SELECT。
- **不变的**：MAIN 一律走 SPI（复用 planner 选出的 PK 索引扫描，规避手工 btree ScanKey 的
  字面量→PK 类型强制转换陷阱，见 §B.3）；`scan.plan.qual` 仍对每一行复筛（§B.1 正确性兜底）。
- **不做**（明确排除，回退到 P2 / qual）：
  - 多列 PK、keyless 表 —— `overlay_get_pk_single_attno()` 本就不命中，P0 不启用；
  - `IS NULL` / `IS NOT NULL`（`NullTest`）、`NOT`（`NOT_EXPR`）；
  - `<> ALL(…)` / `= ALL(…)`（`ScalarArrayOpExpr.useOr == false`）；
  - `OR` 中混入非 PK 分支（`OR` 不可拆分，整条放弃）；
  - 数组元素或右操作数含非 `Const` 节点（子查询 / Param / Var）——无法静态化为 SQL 片段。

## E.2 识别范围（node 形态）

新增单一判定函数 `ob_is_pk_pushable_clause(clause, rti, pk_attno)`，递归判定一条顶层
`RestrictInfo.clause` 是否「纯 PK 谓词」：

| node 形态 | 接受条件 |
|---|---|
| `OpExpr(op, [Var(pk), Const])`（`Const` 可在左） | `op` 属于该 Var 类型的 btree 五算子（typcache `eq/lt/le/gt/ge_opr`） |
| `ScalarArrayOpExpr(op, [Var(pk), Const-or-ArrayExpr])` | `useOr == true` 且 `op == eq_opr`；`ArrayExpr` 的每个元素都必须是 `Const` |
| `BoolExpr(AND_EXPR / OR_EXPR, args)` | 所有 `args` 递归满足本判定 |
| 其它 | 一律不接受 |

`Var` 校验沿用 P0 原有三条：`varno == rti`、`varattno == pk_attno`、`varlevelsup == 0`。
比较算子不再要求「等值」，因此不再需要 `get_commutator` —— 对 `OpExpr` 而言
`id < 5` 与 `5 > id` 的 `opno` 各自都是类型自己的 btree 算子，直接判 `opno ∈ {eq,lt,le,gt,ge}`
即可；deparse 后原样交给 SPI，语义由 `ruleutils` 保证。

**单点等值判定**（决定 L2 是否可 O(1)）单独用一个 `ob_pk_point_const(clause, rti, pk_attno, &con)`：
只认 `OpExpr(=, [Var(pk), Const])` / `OpExpr(=, [Const, Var(pk)])`。

## E.3 双层策略：L1 放宽、L2 退化

Planner hook 的 P0 采集改为「**收集全部** 纯 PK 子句 → 用 `AND` 串起 → 一次性 deparse」：

```
pk_clauses = [ r for r in baserestrictinfo if ob_is_pk_pushable_clause(r.clause) ]
has_pk_pred       ← deparse(AND(pk_clauses)) 成功
pk_where_sql      ← 该 SQL 文本（IN / BETWEEN 区间 / OR 都落在这一串文本里）
pk_serialized_key ← 仅当 len(pk_clauses)==1 且该条是单点等值时才序列化；否则为空
```

- **L1 MAIN**：`pk_where_sql` 作为 SPI `SELECT * FROM rel WHERE (pk_where_sql) [AND (general_where)]`
  的过滤条件。`IN` / 范围 / `OR` 都能被 `deparse_expression` 忠实还原，故 L1 **自动获益**，
  无需新增任何执行侧代码路径。
- **L2 DELTA**：沿用 [`overlay_scan_open`](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L419-L433) 已有的二分支——
  `pk_serialized_key` 非空 → `overlay_delta_lookup` 单点 O(1)；**为空 → `overlay_delta_list_for_rel` 全量加载 + qsort + bsearch**。
  于是 `IN` / 范围 / `OR` 的 delta 侧自动落到「全量列表」这条已被 P2 验证过的路径，
  **零新增代码**，正确性由同一套 Pass1 bsearch / Pass2 orphan 规则兜底。

> 取舍：L2 不再对多值谓词做「逐 key lookup」优化（可做但有界收益 + 需处理 key 个数上界）；
> 本扩展以「L1 拿到索引扫描」为主要收益来源，delta 侧复用全量路径换取最小改动面。

## E.4 协议与执行

**custom_private 6 元素协议不变**（§B.5）。唯一变更是**语义放宽**：

| index | 变更 |
|---|---|
| 1 `pkflag` | 维持 0/1；1 表示「有可用 pk_where_sql」——**不再要求 pk_serialized_key 非空** |
| 2 `pk_where_sql` | 文本内容可为 `IN` / 范围 / `OR` 片段，长度上限 64KB 不变 |
| 3 `pk_serialized_key` | **允许空串**：空 → Executor 侧 delta 走全量列表 |

- Planner 打包端（[L1646-L1674](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L1646-L1674)）本就不要求 key 非空，无需改动。
- Executor 解包端（[L1997-L2022](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L1997-L2022)）原把「where 或 key 任一为空」整体判为 `has_pk_pred=false`，已拆开：
  `has_pk_pred ← (pk_where_sql 非空)`；`pk_serialized_key ← (非空则取，空则 NULL)`。
- `≠ 1.0` 之外无协议版本号；旧计划（4 元素）继续 fallthrough，不回归。

**P0 SPI 执行改为多行物化**：原实现只取 `SPI_tuptable->vals[0]`（点查 ≤1 行的假设），
现改为遍历 `SPI_processed` 全部行，逐行建 `ObMainRow` 追加到 `st->p0_rows`。
迭代器（`overlay_scan_next` Pass1 的 `p0_cur` 游标）本就按 `List` 逐条取，无需改动。

> 已知取舍：P0 路径把 MAIN 结果一次性物化，故 P0 命中的查询失去了 RD-6 的 MAIN 侧早停；
> 但谓词已被收窄、且走的是 planner 的 PK 索引扫描，物化行数有界。非 P0 路径仍保持流式早停。

## E.5 正确性论证

三层不变式共同保证「结果集与 SRF 全量叠加一致」：

1. **`scan.plan.qual` 永远复筛**：P0 只减少喂给 Pass1 的 MAIN 行，不改行内容；
   最终每行仍过 `ExecQual`（§B.1）。谓词下推写多/写少都不会让错误行进入结果。
2. **被过滤掉的 MAIN 行不会丢结果**：P0 的 `pk_where_sql` 与顶层 qual 同源，
   被它排除的 MAIN 行本就不满足顶层 qual；唯一例外是「分支 UPDATE 把行改进了谓词范围」——
   该 delta 行在 Pass1 无 MAIN 配对，落入 Pass2 orphan 规则补发（I/U 重建），由 qual 决定去留。
3. **delta 全量列表不会多发错误行**：Pass2 对未配对的 I/U 一律重建后交给 qual；
   范围/IN 之外的 orphan 会被 qual 丢掉，语义安全（与 §D.3 的 qual 预筛策略同构）。

## E.6 回退矩阵（在 §B.6 基础上增补）

| 阶段 | 新增错误 | 回退动作 | 结果 |
|---|---|---|---|
| Planner P0 识别 | 子句含 `useOr=false` 的 SASE / `OR` 混入非 PK 分支 / 数组元素非 `Const` / 类型无 btree 算子 | 该子句不入选 `pk_clauses` | 该子句落到 P2 / qual；其余 PK 子句照常下推 |
| Planner P0 识别 | 有 PK 子句但合并 deparse 失败 | `has_pk_pred=false`，`pk_where_sql/key` 全清 | **全部** PK 子句回到 P2（因为 P2 的排除以 `has_pk_pred` 为前提） |
| Executor 解包 | `pk_where_sql` 空但 key 非空 | `has_pk_pred=false` | 回退全扫（不会用空谓词去 SPI） |
| Delta 侧 | `pk_serialized_key` 为空（IN/范围/OR） | 走 `overlay_delta_list_for_rel` 全量列表 | 正确，代价 O(M log M) |

> P2 的排除条件必须是 `has_pk_pred && ob_is_pk_pushable_clause(clause)`——两个条件缺一不可；
> 若 P0 deparse 失败（`has_pk_pred=false`），PK 子句必须能被 P2 重新收编，否则会漏筛。

## E.7 验证断言（新增 `scan_pk` Section J）

| Test | 内容 | 通过标准 |
|---|---|---|
| `PKIN_BASIC` | `WHERE id IN (1,3,5)`（含分支内改/删/增） | BS 结果 = SRF 全量叠加，行图景一致 |
| `PKRANGE_BASIC` | `WHERE id BETWEEN 2 AND 4` | 同上 |
| `PKRANGE_OPEN` | `WHERE id > 3` / `WHERE id < 3` | 同上 |
| `PKOR_BASIC` | `WHERE id = 1 OR id = 4` | 同上 |
| `PKIN_PLUS_NONPK` | `WHERE id IN (…) AND tag = '…'`（P0 + P2 协同） | 无重复下推，结果一致 |
| `PKIN_ORPHAN` | 分支把谓词外的行 UPDATE 进谓词范围（Pass2 orphan 补回） | 结果一致 |
| `PKIN_EXPLAIN` | `EXPLAIN` 只出现 `Custom Scan`，无 SeqScan 泄露 | 单节点 |
| `PKIN_MAIN_CLEAN` | `RESET` 后 MAIN 零污染 | 逐行比对 |
