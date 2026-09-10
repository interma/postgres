# BranchScan 透明扫描 — 设计、优化策略与硬约束

> 本文档统一收录 BranchScan CustomScan（方案A）的**设计评审 / 实施切片 / 优化策略 / 踩坑硬约束**。
> 原 `branchscan_plan.md`（实施计划）和 `branchscan_strategy.md`（P0/P2 策略+回退矩阵）已合并至此，避免在两个文件间跳读。
>
> - **Part A**：V2 骨架——设计调研、实施步骤切片、风险矩阵（原 plan）
> - **Part B**：V2 优化——P0 PK O(1) 快路径 + P2 非 PK 条件下推 + 协议 + 验证断言（原 strategy §1-7）
> - **Part C**：硬约束踩坑——L3 Delta O(1) lookup MemoryContext 生命周期（原 strategy §8）

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
3. Pass1 主表 merge：逐行 serialize_pk → delta_list linear scan → 无匹配输出主表 / 'D' tombstone 跳过 / 'U'/'I' 输出 delta 覆盖行，matched entry 标记 `->emitted=true`（L1584-L1695）
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
3. **成本策略**：CustomPath 的 `path.total_cost` 设为当前 rel 最小路径总成本的 0.00001（极小值），强制 planner 选我们的路径；分支模式下就是要 100% 读叠加视图，没有理由让原路径赢。
4. **分支 inactive 时不介入**：`overlay_branch_is_active()==false` → planner hook 直接 return，完全不碰 pathlist。

## A.2 Files and Modules

| 文件 | 改动（增量最小） |
|---|---|
| `contrib/overlay_branch/include/overlay_branch.h` | 在函数签名区（~L129-L155）追加 3 个 public 前向声明：`ob_compute_overlay_slots`（公共 helper，给 SRF 和 CustomScan 共用）、`ob_branchscan_planner_hook`（planner hook entry，_PG_init 里取地址注册） |
| `contrib/overlay_branch/src/overlay_branch.c` | 唯一 C 源：(a) 全局 saved-hook chain + 4 张 static 方法表；(b) Planner hook + PlanCustomPath；(c) 2-pass 公共 helper 抽出；(d) Begin/Exec/End/ReScan 4 个 executor 实现；(e) _PG_init 链入 saved hook chain + RegisterCustomScanMethods |
| `contrib/overlay_branch/test/sql/overlay_branch_user.sql` | 新增 Part G（Transparent SELECT 等价性）+ Part H（DML ModifyTable 读子计划触发 BranchScan + apply 主表精确变化） |
| `contrib/overlay_branch/test/sql/overlay_branch_basic.sql` | 末尾新增 Part G（EXPLAIN 验证、MAIN baseline 无回归、where pk=1 不下推仍正确） |
| `contrib/overlay_branch/test/expected/*.out` | 两条 pg_regress baseline 重生成 0 diff |

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
| 通用 | `make install prefix=/tmp/pg17-writable 2>&1 \| tail -3` → 0 warning、0 error |
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
