# D-2 A2 读写视图统一性（精简 3 高优项）Implementation Plan

> Scope：review_260926 A2 + progress_tracker OPEN D-2（原缺口：self-join / rescan / cursor / parameterized NL 独立 case 组缺失）。
> 砍低 ROI：不单独追 MarkPos/RestorePos 覆盖率（planner 大概率透明 spool 不触发）、不做 EXPLAIN ANALYZE JSON 细节断言（mvcc_usage/mvcc_review 已经有 EXPLAIN ANALYZE 零散跑过）。只留 3 P0 正确性硬项。

---

## Repository Research（现状 + 缺口 = 结论）

### 已实现回调（4 个均 OK，不重写）
| 回调 | 位置 | 行为 | 验证状态 |
|------|-----|------|---------|
| Exec | branch_scan.c:L1608-L1720 | lazy first-tuple materialize + list iter + ExecProject | ✅ 有覆盖，但 self-join 两实例独立 = 0 断言 |
| **ReScan** | branch_scan.c:L1786-L1816 | throttled invalidate + 4-step drop slots + reset materialized | ⚡ ZERO **same-CSS functional 覆盖**（L1 T8 spec 是 cross-session 不同 CSS，不是 ExecReScan callback 路径；mvcc_bounds L703 明确 `FIXME-NL` 延期）|
| MarkPos / RestrPos | branch_scan.c:L1767-L1784 | save/restore ListCell ptr | ⚡ ZERO 覆盖 → **本轮跳过（ROI 低：PG 对 SCROLL 会透明 spool 到 portal tuplestore，根本走不到这里；除非整棵计划 ALL-markable + enable_material=off + 叶子 CustomScan markable，否则不触发。MVP 不追这条线）**|

### 其他研究结论
- Self-join 两个 RTE 分别 inject CustomScan（MVP override 删非 CustomPath，确保 2 个都走 BranchScan）→ 结果应正确，ZERO 断言。
- NL LATERAL inner：Planner 每一个 outer tuple 调 `ExecReScan(NL inner child)` → 这是 **100% 触发 SAME-CSS ReScan** 的方法（比 cursor rewind 靠谱），用来硬证 A2.3。
- Parameterized NL：`param_info = NULL` 硬编码，MVCC bounds FIXME-NL 警告 "inner WHERE 引用 outer 列时 PG17 BEFORE BeginCustomScan → SEGV" → 需要冒烟：不崩就跳过，崩就加 `contain_vars_of_level` skip 注入。

---

## 3 P0 正确性子任务（按顺序）

### 1️⃣ A2.1 self-join（2 个 BranchScan 实例独立 & 不互串）L3
- Fixture：t(id PK, grp int, amt numeric)，MAIN 10 rows。Branch：INSERT 5（id 11-15）+ UPDATE 2（id=1/2 amt +10）+ DELETE 1（id=3）。Overlay 总 = 14 rows（10 - 1 + 5）。
- SJ1：`SELECT count(*) FROM t a JOIN t b ON a.id = b.id + 1`；正确 = 13（b.id 存在且 a=b+1，id=3 删后 b=2→a=3 无 a，b=3→a=4 无 b）。CASE WHEN count=13 则 PASS，否则 FAIL。
- SJ2：LEFT JOIN `t a LEFT JOIN t b USING (grp)`；与 MAIN baseline copy + 手动重放 delta 的 `EXCEPT ALL = 0 rows` 比对。
- SJ3 EXPLAIN 断言：`EXPLAIN (FORMAT TEXT) SELECT ...` 输出中 `overlay_branch_branchscan` 出现 **exactly 2 次**（证明 self-join 两 RTE 各 inject 1 CustomScan，不是共享 state = correctness 保证）。

### 2️⃣ A2.3 SAME-CSS ReScan（4-step rescan 契约 functional 硬证据）L1 Isolation
**用 NL LATERAL 驱动（cursor 不触发，NL LATERAL 100% 触发 inner ExecReScan）：**
- Setup：outer driver = 3 行小表；inner = 分支表。
- s_ctrl：create_branch → INSERT 3 delta rows → commit。
- s1（driver）：use_branch → 跑 LATERAL 驱动 3 outer rows，inner 每次 rescan 返回 count = (3 main + 3 delta) = 6。结果 = 3 × 6 = PASS。
- s2：use_branch → INSERT id=10（第 4 个 delta）→ commit。
- s1：SAME SESSION，re-run SAME SQL（BEGIN; ... 同语句 → 其实每跑一次 SQL 是新 planstate CSS。**怎么触发 SAME-CSS？换结构：驱动 3 outer rows 是单语句内，每一个外 tuple 会 rewind inner child，**这就是 exact same CustomScanState re-entering ReScan！所以 Step 2 的正确做法是：在 LATERAL WHERE 里加 **动态条件**（比如让外 row 1 找 grp=1，外 row 2 找 grp=2）且 inner 有 2 种不同结果；然后证明 inner 确实每次 rescan 重物化了（给同一 outer 表加个 s2 写 delta 后，下一次 s1 跑新语句 LATERAL，inner 见新 delta —— 这是 cross-statement fresh CSS，不是 same-CSS。要 hard-evidence same-CSS，就做 **outer 3 rows + 断言每一次 inner materialize 都触发**（在 C 代码里加个 DEBUG log 计数）或者更简单：MVCC bounds FR6 same-txn 连续两读之间插 write → Read1 count=3，Read2 count=4（fresh CSS）已过；**same-CSS 用 Nested Loop 3 outer rows 触发 3 次 inner ReScan = 3 × ExecReScanCustomScan**，检查输出结果行稳定，证明 rescan 不崩且返回正确。）

**简化版 Step 6 L1 spec（可直接落地）**：
- Setup：CREATE TABLE outer_drv AS SELECT g AS g FROM generate_series(1,3) g；CREATE TABLE inner_tbl(id PK, grp int, v text); INSERT 3 rows (grp=1,2,3 each)；create_branch；INSERT 2 delta rows 每 grp 各 1，所以每个 grp 现在 = 2。
- s1：use_branch → `SELECT o.g, (SELECT count(*) FROM inner_tbl i WHERE i.grp = o.g) FROM outer_drv o ORDER BY o.g`；结果 = 3 行每行 count=2。这条 SQL 里 SubPlan 或 NL 内层每一行都会重新 scan inner_tbl（如果是 NL inner child 就调 our ReScan callback；如果是 SubPlan 是新 CSS，但 NL inner 方式是 same CSS）。**强制 enable_hash/merge=off 让 Planner 选 NL + LATERAL，保证 inner 是同一个 CustomScanState 被 rescan 3 次**。hard-evidence 就是：如果我们的 ReScan 没正确 drop+reset，第 2/3 次 inner 会返回空（cursor 走到 list_end 不会重绕），导致 count=0 失败。PASS 就是 3 次 count=2 全部正确。这是 same-CSS ReScan 硬证据！

### 3️⃣ A2.4 Parameterized NL LATERAL outer Var（FIXME-NL 冒烟 + 条件修复）L3
**分两档：**
#### A档（必跑，safe）
```sql
SET client_min_messages = warning;
SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_nestloop = on;
-- 不引用外列（安全：不会触发 deparse outer Var SEGV）
SELECT o.id, x.cnt
  FROM t_px o,
       LATERAL (SELECT count(*) AS cnt FROM t_px i WHERE i.grp = o.grp) x
 ORDER BY o.id;
```
结果 = 每 id 的 cnt = (同 grp 在 overlay 后行数)；CASE WHEN 全部 = 预期 → PASS:A2_NL_SAFE。

#### B档（条件：独立 psql 跑，不嵌到 installcheck 里防 crash 污染 expected）
```sql
-- 引用外列 id（i.id = o.id）→ 踩不踩 FIXME-NL SEGV？
SELECT count(*) FROM
 (SELECT o.id, x.amt FROM t_px o,
         LATERAL (SELECT i.amt FROM t_px i WHERE i.id = o.id) x
  ORDER BY o.id) s;
```
- 如果 **不崩**（大概率，我们 deparse 已经走 baserestrictinfo 逐 clause，可能已隐式处理）→ 不加任何 C 代码。
- 如果 **崩**（signal 11 或 garbled）→ 加 C guard 代码：
  - `ob_branchscan_planner_hook` baserestrictinfo deparse 外层包 `PG_TRY`；且检查 `contain_vars_of_level((Node*)clause, 1)`（outer-level Var 存在）。
  - 命中或 CATCH → `elog(DEBUG1, ...)` + `return;`（不 inject CustomScan，planner fallback 到 SRF wrapper view 等价路径，结果正确但性能略降）。
  - L3 同步加负向 case：`DO $$ BEGIN ... EXCEPTION WHEN OTHERS THEN RAISE NOTICE 'crashed'; END $$;` 结果 NOT crashed → PASS:A2_NL_NOCRASH。

---

## 改动文件清单（精简后最小）

| 文件 | 类型 | 原因 |
|-----|-----|-----|
| `test/regress/sql/overlay_branch_a2.sql` | NEW | A2.1 + A2.4 两组 L3 |
| `test/regress/expected/overlay_branch_a2.out` | NEW | expected |
| `test/isolation/specs/ob_nl_rescan_inner.spec` | NEW | A2.3 SAME-CSS L1 Isolation |
| `test/isolation/expected/ob_nl_rescan_inner.out` | NEW | expected |
| `Makefile` | MOD 2 处 | REGRESS 加 overlay_branch_a2；ISOLATION 加 ob_nl_rescan_inner |
| `src/branch_scan.c` | **条件 MOD** | 仅 A2.4-B 测出 SEGV 时加 PG_TRY + outer Var guard（不崩不碰） |
| `doc/progress_tracker.md` | MOD | A2 ✅ + Batch 8 row + 提交链 6 + OPEN D-2→✅ |
| `doc/review/poc_fix_tracker.md` | MOD | R25/R26/R27 台账 + Batch 8 S20 3 子条目 |
| `doc/test_coverage_index.md` | MOD | 新增 a2 (L3) + ob_nl_rescan_inner (L1) 2 行 |

---

## 9 Steps Implementation

0. Triple-check baseline → L3 11/11 + L1 8/8 0 diff；
1. 写 L3 A2.1 self-join（SJ1/SJ2/SJ3）；
2. 写 L3 A2.4（A档 SQL → 文件；B档 独立 psql 测）；
3. （条件）若 B 档崩 → 加 C guard；
4. 写 L1 A2.3 NL LATERAL SAME-CSS spec（3 outer rows → 3 inner rescans，inner result not empty 第 2/3 次）；
5. Makefile REGRESS/ISOLATION 追加；
6. make build → cp 3 files → restart → `make installcheck`；
7. actual → expected cp；2× consecutive rebuild → L3 12/12 + L1 9/9 0 diff；
8. 3 docs sync；
9. git commit short English（NOT amend，普通 push 兼容）。

---

## 验收 = 5 Bucket
1. L3 12/12 × 2 0 diff；
2. L1 9/9 × 2 0 diff；
3. A2.1 SJ3 EXPLAIN × 2 CustomScan；
4. A2.3 3 outer rows → 3 inner count=2（若 ReScan 错误 reset cursor → 第 2/3 次 0，FAIL 保证能 catch）；
5. A2.4-B 不崩；若崩后加 guard → PASS:A2_NL_NOCRASH；
6. 原 11+8 不回归。

Exit：全部满足 + commit 完成。
