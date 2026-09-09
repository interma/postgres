# overlay_branch 开发进度跟踪 (V1 & V2 MVP Steps)

> 本文档作为开发进度与交付物清单，对应 README 里提到的 Step 拆分。
> **V1 = 手动 SRF 模式**（读分支必须手动调 `overlay_main_plus_delta()`）；
> **V2 = 透明 BranchScan 模式**（普通 `SELECT *` 自动做 Main⊕Delta 合并）。
>
> 编号说明：1/2/3/4/5/7/8 对应实际交付的里程碑；Step 6 被并入 Step 7 的
> Hard Guard 和推迟到 V3 的多 session MVCC pin，因此留空（详见文末"关于 Step 6 留空"）。

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
| V2 Plan | BranchScan CustomScan 设计评审 | ✅ | [doc/branchscan_plan.md](branchscan_plan.md) |
| V2-1 | Planner 拦截 + `set_rel_pathlist_hook` 6 层 guard | ✅ | [src/branch_scan.c Plan hook](../src/branch_scan.c#L274-L406) |
| V2-2 | ExecCustomScan 生命周期 (ExtendedCSS + Lazy Materialize) | ✅ | [ExecCustomScan wrapper](../src/branch_scan.c#L462-L645) |
| V2-3 | SRF & CustomScan 单源化 2-Pass helper | ✅ | [ob_compute_overlay_slots_internal](../src/branch_scan.c#L98-L268) |
| V2-4 | Projection + qual 过滤 (WHERE / CASE WHEN / 部分列) | ✅ | ExecBranchScan: ResetExprContext → ExecQual → ExecProject |
| V2-5 | Regression PART 5/6 新增 + 基线全绿 | ✅ | `overlay_branch_user.sql` Part 5 / `overlay_branch_basic.sql` Part 6 |

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

## 关于 Step 6 留空

两个真实原因，合起来导致 Step 6 没有独立 deliverable：

1. **被 Step 7 Hard Guard 吸收了**：最早方案里 Step 6 是"分支切换 / 回 MAIN 安全校验"——切换前清未提交事务、session GUC 双系统一致性、revert MAIN 前 sanity check。实际实现时，这些准入逻辑没有独立成一个 Step 包，而是全拆进了 Step 7 的 16 场景 guard（`use_branch` 入口、`RESET`、`apply/discard` 开头都先 guard）。单独拎 Step 6 就只剩空壳 API，索性不再占用编号。

2. **多 session MVCC pin 被推迟到 V3+**：另一设想的 Step 6 = "多 backend 同时 active 不同 branch（每个 backend 独立 state，而不是当前单 session GUC）+ snapshot pin + 真·数据库级 fork"。这超 V1/V2 MVP 范围，等 V3 引入 pg_proc catalog 传 branch_id 替代 GUC 时再启用 Step 6 编号，现在故意留空提醒未来方向。

---

## 已归档的历史工作流陷阱（供参考，不再重复犯）
1. GUC 字符串手动 pfree → SIGABRT；改用 `SetConfigOption`
2. CustomScanState.custom_ps 存私有 BranchScanState* → ExecShutdownNode_walker 野指针 → SIGSEGV 11；改用 ExtendedCSS 容器化
3. UPSERT (`ON CONFLICT DO UPDATE`) 即使无 RETURNING，返回码仍 `SPI_OK_INSERT_RETURNING(7)`；调 `ob_spi_one_shot` 传 `tcount=0` 会静默跳过所有副作用（SPI_processed=0），必须传 `tcount=1`
4. Write Redirect 统一 return 标签放 standard ExecutorRun **之前** → catalog DML（写 delta 表本身）永远不真正落盘；修复：仅 INS/U/D 处理路径 goto 标签，标签移到 standard 之后
5. BPCHAR / CHAR(N) `DatumGetChar(d)` 截断 varlena 指针低位（0x55… → `'U'`）；必须走 `OidOutputFunctionCall`
6. SPI 1-based column vs TupleDescAttr(td, 0-based) 错位 1 列（key TEXT 和 op BPCHAR 交叉解引用）
7. List 节点 / pk strings / serialize 结果分配在 SPIMemoryContext → SPI_finish 释放并毒化 → Pass2 / 上层用 0x7F 野指针；一律切 TopMemoryContext pstrdup / datumCopy
