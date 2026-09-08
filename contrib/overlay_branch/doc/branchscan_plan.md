# BranchScan CustomScan (方案A) Implementation Plan

## Repository Research

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

## Files and Modules

| 文件 | 改动（增量最小） |
|---|---|
| `contrib/overlay_branch/include/overlay_branch.h` | 在函数签名区（~L129-L155）追加 3 个 public 前向声明：`ob_compute_overlay_slots`（公共 helper，给 SRF 和 CustomScan 共用）、`ob_branchscan_planner_hook`（planner hook entry，_PG_init 里取地址注册） |
| `contrib/overlay_branch/src/overlay_branch.c` | 唯一 C 源：(a) 全局 saved-hook chain + 4 张 static 方法表；(b) Planner hook + PlanCustomPath；(c) 2-pass 公共 helper 抽出；(d) Begin/Exec/End/ReScan 4 个 executor 实现；(e) _PG_init 链入 saved hook chain + RegisterCustomScanMethods |
| `contrib/overlay_branch/test/sql/overlay_branch_user.sql` | 新增 Part G（Transparent SELECT 等价性）+ Part H（DML ModifyTable 读子计划触发 BranchScan + apply 主表精确变化） |
| `contrib/overlay_branch/test/sql/overlay_branch_basic.sql` | 末尾新增 Part G（EXPLAIN 验证、MAIN baseline 无回归、where pk=1 不下推仍正确） |
| `contrib/overlay_branch/test/expected/*.out` | 两条 pg_regress baseline 重生成 0 diff |

## Implementation Steps（3 切片 MVP，严格顺序依赖）

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

## Dependencies and Considerations

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

## Validation（每片必过）
| 片 | 验证 |
|---|---|
| 通用 | `make install prefix=/tmp/pg17-writable 2>&1 \| tail -3` → 0 warning、0 error |
| Slice1 | 见 Slice1 末尾 EXPLAIN 验收；MAIN 下 Seq Scan；branch 下 CustomScan |
| Slice2 | 见 Slice2 末尾 string_agg 完全相等；RESET 后 baseline 3 行 |
| Slice3 | `pg_regress --use-existing --host=/tmp --port=15433 overlay_branch_user overlay_branch_basic` → All 2 passed；cat regression.diffs → 空 |
| MAIN 不变量回归 | apply no-conflict 精确匹配 / conflict rollback 保留 MAIN race 值 / discard cascade MAIN baseline 3 行（Step5 F1/F2、Step8 Part E 断言保留在 basic.sql 末） |

## Risks

| # | 风险 | 发生后果 | Mitigation |
|---|---|---|---|
| R1 | apply 内部读 MAIN 被 BranchScan 叠加 delta → 冲突检测 false-negative → MAIN 静默污染 | 最严重的正确性 bug | Slice1 ob_branchscan_planner_hook 顶部 `if (ob_in_apply_operation) return;` 第一行硬判 |
| R2 | result_slots / List 节点分配在 SPIProc cxt → SPI_finish 释放 → use-after-free → SIGABRT 0x7F | Step4 曾 hit 过同根因 | `ob_compute_overlay_slots_internal` 严格沿用 Step4b 做法：list / slot 分配前 MemoryContextSwitchTo(TopMemoryContext)；EndCustomScan 自己释放 |
| R3 | add_path 的成本设太低？其他插件自定义路径没机会被选 | 非 bug；分支模式下叠加读就是 desired | 是 desired；1e-5 * 现有最小路径 总成本，确保不管 planner 怎么 tie-break 都选我们 |
| R4 | 无 PK / VIEW / matview / foreign table 误拦截 | 用户正常 SQL 被我们的 plan 吃了 → 读不到正确数据 | Slice1 5 条 filter：(c)(d)(e) rtekind / relkind / has_pk，每条不满足立即 return |
| R5 | ss_ScanTupleSlot TupleDesc 不匹配 rel → 上游 projection 错位 | client 看到字段串位 / cassert tdtype | BeginCustomScan 严格 `CreateTupleDescCopy(reldesc)`；Scan 的 ScanTupleSlot tupdesc=这个独立副本；和 ExecCustomScan 填充的 natts 完全一致 |
| R6 | ReScanCustomScan 忘记重置游标 → 第二次取返回空 → 嵌套循环 join/NLParam 结果不全 | plan 节点 重跑结果错 | ReScan 明确 `bss->cursor = list_head(bss->result_slots)` |

## 非目标 Out-of-scope（V1 绝对不做）
- ❌ WHERE qual 下推到 CustomScan 内部（V1 全表扫，上层通用 Filter 过滤即可）
- ❌ Join 下推 / parameterized nested-loop key 下推
- ❌ Parallel worker CustomScan
- ❌ MarkPos / RestorePos cursor 可滚动语义（V1 ReScan 支持就够）
- ❌ EXPLAIN ANALYZE 实际行数 / custom 打印
- ❌ Partition / FK 级联子表的 BranchScan 透明叠加（Step7 guard 已经 block 建这些）
