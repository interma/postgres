# L3 pg_regress 测试文件拆分 Implementation Plan

## Repository Research（调研结论）

### 当前文件大小（wc -l）

| SQL 文件 | 行数 | 说明 |
|---|---|---|
| overlay_branch_mvcc.sql     | **2042** | 巨文件，含 15+ Section (PART A→F / B→K / L→Q / R-API / R-CTX / R_VERSION) |
| overlay_branch_advanced.sql | **2025** | 巨文件，含 Section A→U + R-ENTRY（共 22 段） |
| overlay_branch_basic.sql    | 754  | ✅ 几百行量级，暂不拆 |
| overlay_branch_user.sql     | 240  | ✅ 小文件，不动 |
| overlay_branch_puredelta.sql | 304 | ✅ 小文件，不动 |

**目标**：mvcc / advanced 两个 >2000 行的文件按用途拆成小文件，控制在 ~几百行。

### pg_regress 运行约束（决定拆分边界的硬规则）
1. **每个 test = 独立 session / 独立数据库**：每个 SQL 文件都在一个全新的数据库跑，`CREATE EXTENSION overlay_branch` 是 session 级的。所以拆分后**每个小文件必须自包含**：
   - 头部都有 `CREATE EXTENSION IF NOT EXISTS overlay_branch` / idempotent 清理（不能依赖"前面的测试建了某表或分支"）。
   - 但 pg_regress 会把之前的 test 跑完的数据库 **drop 后新建**（默认 `d` flag：`--create-role` 默认 clean database per test）。所以两个拆分文件之间完全不能共享对象。
2. **每个 test 需要一个同名 expected/*.out**：跑绿后 `cp test_output/results/<name>.out test/regress/expected/<name>.out`。
3. **Makefile REGRESS = <list>**：拆分后的所有文件名都必须加进去，顺序按依赖从"基础→高级→分类 Bug 回归"排。
4. **Section 之间必须独立可切**：若 Section 用了上一个 Section 的表/分支（例如 advanced 的 S 建了 UPSERT 分支 → T apply → U discard），这 3 个 Section 必须**在同一个文件里**（否则跨 session 无法共享 branch catalog 状态）。

### 现有 Section 边界 + 真实依赖关系图（grep + 行号精查）

#### 1) mvcc 2042 行内部结构
```
文件头 (L1-80): 扩展签名 + use_branch(mode) 介绍
PART A-F  (L81-278):  use_branch 签名/模式/退出/默认值（轻量断言，无依赖）
SETUP (L323-332):    CREATE TABLE t_inv + INSERT (1,2,3) + create_branch b_inv1/2/3
Section B (L334-351): baseline + force_invalidation_check positive → 依赖 SETUP
Section C (L354-411): INVALIDATION kickout DQL path (discard 手动 flip) → 依赖 B
Section D (L412-494): DML kickout after apply state flip → 依赖 B/C
Section E (L510-534): THROTTLE is_active 计数 → 依赖 SETUP
Section F (L539-563): apply/discard same-session 边界 → 依赖 SETUP
NOT-current discard（~L564-682，Section G/H/I 混合）: b_notcurrent_A/B 新建分支 discard 非 current → **独立 SETUP**
Section J (L684-720): GUC defaults + roundtrip + threshold=1 → 无分支依赖
Section K (L745-768): 各种 name/state 错误路径 → 独立
Section L (L859-1024): Invalid snapshot mode (snapshot vs live) + DQL/DML split → 独立 SETUP
Section M (L1025-1116): apply_branch/discard_branch same-session ERROR 捕获 → 独立 SETUP（baseline 100 rows）
Section N (L1117-1251): Cross-branch isolation + FR1 NOTIFY → 独立 SETUP（2 分支 b2/b3）
Section O (L1252-1307): use_branch('') / invalid mode / APPLIED/DISCARDED apply/discard → 独立
Section P (L1308-1434): FR6 rewind/rescan → 独立 SETUP（2 行 bs_fr6_a）
Section Q (L1435-1524): FR7 GUC default unchanged → 轻量无 SETUP
Section R-API (L1525-1687): Review S03 R09(create_branch retval)/R08(use_branch NULL exit)/R10(error template) → 独立 SETUP
Section R-CTX (L1700-1880): Review S09 R07 plan cache + S07 hash merge → 独立 SETUP（18/21 行 MAIN + 分支 b_r_ctx）
Section R_VERSION (L1887-末尾): Review S14/S17 R15/R18 HOT+rewrite → 独立 SETUP（rv_t fillfactor=50）
```

**可切边界**（不共享对象的段）：
- (A) PART A-F + B-K 前半段（L81-768）→ 共享 `t_inv` SETUP → 文件 *1* (~700 行)
- (B) Section L-NOTIFY + O + P + Q（L859-1524）→ 每部分独立 SETUP，可合并 → 文件 *2* (~670 行)
- (C) Section R-API（L1525-1687）→ ~160 行，独立 → 文件 *3*
- (D) Section R-CTX（L1700-1880）→ ~180 行，独立 → 文件 *4*
- (E) Section R_VERSION（L1887-末尾）→ ~155 行，独立 → 文件 *5*

#### 2) advanced 2025 行内部结构
```
0. Prerequisite guard + Idempotent cleanup (L1-43)
Section A (L45-200):  BS_PK_LOOKUP bs_user → 依赖 bs_user SETUP
Section B (L202-366): PK Physics bs_basic numeric PK + tag → 依赖 bs_basic SETUP（独立）
Section C (L368-498): RETURNING clause bs_ret → 独立 bs_ret SETUP
Section E (L500-658): NON-PK qual pushdown bs_nonpk → 独立 bs_nonpk SETUP
Section F (L660-760): PK TYPE DIVERSITY bs_div (7 列 7 种类型 PK) → 独立
Section G (L763-839): PK EDGE CASES bs_edge → 依赖 A 的 bs_user 表？→ 查一下
Section H (L841-974): BUG REGRESSIONS bs_bug → 独立
Section I (L977-1100): Multi-type PK coverage NUMERIC/BPCHAR... bs_typmod → 独立
Section J (L1103-1176): P2 complex non-PK WHERE deparse bs_complex → 独立
Section K (L1179-1239): Mixed DML lifecycle bs_mix + bs_mix2 → 独立 2 表
Section L (L1241-1286): pure delta UPDATE MVP noop bs_pd → 独立
Section M (L1288-1340): BPCHAR/NUMERIC typmod PK pure-delta bs_pd_tm → 独立
Section N (L1342-1405): Multi puredelta rows + non-PK bs_pd_multi → 独立
Section O (L1407-1468): apply/discard boundary bs_pd_app_disc → 独立
Section P (L1470-1504): EMPTY MAIN pure delta 100% bs_pd_empty → 独立
Section Q (L1506-1536): Composite PK pure delta bs_pd_cpk → 独立
Section S (L1538-1621): Pure Delta UPSERT bs_S_tbl + 2-phase → 独立 bs_S_tbl
Section T (L1623-1643): apply bs_S_br (Section S 的分支 → apply) → **依赖 Section S**
Section U (L1645-1665): discard bs_U_disc → 基于 Section T 后状态 → **依赖 S→T**
Section R-ENTRY (L1667-1812): S02 drift-write hard intercept + R19 snapshot TTL → 独立 r_entry_t
```

**关键依赖链**：Section S → T → U 三阶段共享 `bs_S_br` 分支状态，**这 3 段必须在同一文件**。
其他 Section 各自独立 SETUP（每个 Section 都建自己的 bs_* 表 + CREATE branch），可自由组合。

## Files and Modules（变更面）

### 新增文件（拆分后 10 个文件，替代原 2 个）
```
test/regress/sql/
  overlay_branch_mvcc_usage.sql      (原 PART A-F + Section B-K)       ~720 行
  overlay_branch_mvcc_bounds.sql     (原 L/M/N/O/P/Q 各独立 Section)   ~670 行
  overlay_branch_mvcc_rapi.sql       (原 Section R-API R09/R08/R10)    ~160 行
  overlay_branch_mvcc_rctx.sql       (原 Section R-CTX R07+S07)        ~180 行
  overlay_branch_mvcc_rversion.sql   (原 Section R_VERSION R15+R18)    ~155 行

  overlay_branch_adv_scan.sql        (原 Section A/B/C/E/F/G/H/I/J)    ~1130 行 → 再细分
  overlay_branch_adv_scan.sql (A-F)  + overlay_branch_adv_regress.sql (G/H/I/J) 各 ~500-600
  overlay_branch_adv_puredelta.sql   (原 K/L/M/N/O/P/Q K1-K7 段)      ~770 行 → 再细分
  overlay_branch_adv_puredelta.sql (K/P/Q/L/M/N/O) + overlay_branch_adv_upsert.sql (S+T+U)
  overlay_branch_adv_rentry.sql      (原 Section R-ENTRY + R19)        ~150 行
```

### 进一步压缩至"单个 ~几百行"的最终切法（advanced 最终 4 块）
1. `overlay_branch_scan_pk.sql`        — A/B/C/E            (A:156 + B:165 + C:131 + E:159 ≈ 611 行)
2. `overlay_branch_scan_type.sql`      — F/G/H/I/J          (F:100 + G:77 + H:134 + I:124 + J:74 ≈ 509 行)
3. `overlay_branch_puredelta_mix.sql`  — K/L/M/N/O/P/Q      (K:61 + L:46 + M:53 + N:64 + O:62 + P:35 + Q:31 ≈ 352 行)
4. `overlay_branch_upsert.sql`         — S + T + U (依赖链) (84 + 21 + 21 ≈ 126 行)
5. `overlay_branch_rentry.sql`         — R-ENTRY + R19      (150 行)

mvcc 最终 5 块：
1. `overlay_branch_mvcc_usage.sql`     — PART A-F + B-K    ≈ 690 行
2. `overlay_branch_mvcc_bounds.sql`    — L/M/N/O/P/Q        ≈ 665 行
3. `overlay_branch_mvcc_rapi.sql`      — R-API R09/R08/R10  ≈ 162 行
4. `overlay_branch_mvcc_rctx.sql`      — R-CTX R07+S07      ≈ 180 行
5. `overlay_branch_mvcc_rversion.sql`  — R_VERSION R15/R18  ≈ 155 行

### 删除（替换）的源文件
```
test/regress/sql/overlay_branch_advanced.sql       (删除)
test/regress/sql/overlay_branch_mvcc.sql           (删除)
test/regress/expected/overlay_branch_advanced.out  (删除)
test/regress/expected/overlay_branch_mvcc.out      (删除)
```

### 仅修改 1 个 Makefile 变量
- `Makefile` L26 `REGRESS = <old 5>` → `<new 15 files>`（按依赖升序）

## Implementation Steps（依赖顺序执行）

### Step 0 — 备份当前 expected 并记住旧文件行数（预防回归）
- `git add -N test/regress/` 后暂存旧 mvcc/advanced out 为临时 stash（或在 plan 里不做）
- 实际：**不备份**，后续 Step 5 跑 make check 绿了直接 cp actual，当前 2× clean rebuild 通过 → 基线足够可信。

### Step 1 — 切分 overlay_branch_mvcc.sql → 5 个新 sql 文件
1. `sql/overlay_branch_mvcc_usage.sql`：拷贝**原 L1-L768**；末尾加 `RESET overlay_branch.current; DROP TABLE IF EXISTS t_inv;`（清理 t_inv 3 行 baseline + b_inv1/2/3 分支 discard 掉）
2. `sql/overlay_branch_mvcc_bounds.sql`：
   - 头：复制 `L1-L80` 扩展 prereq guard；中间拼接原 **Section L (L859-L1024) / M (L1025-L1116) / N (L1117-L1251) / O (L1252-L1307) / P (L1308-L1434) / Q (L1435-L1524)**；
   - 每 Section 之间加 `DISCARD ALL; RESET overlay_branch.current;`（彻底隔离各段 GUC/branch ctx，虽然新 session 本就干净，但保持原语义）
3. `sql/overlay_branch_mvcc_rapi.sql`：头 prereq guard + 原 **L1525-L1687**（R-API Section）
4. `sql/overlay_branch_mvcc_rctx.sql`：头 prereq guard + 原 **L1700-L1880**（R-CTX Section）
5. `sql/overlay_branch_mvcc_rversion.sql`：头 prereq guard + 原 **L1887-L2042**（R_VERSION Section）

### Step 2 — 切分 overlay_branch_advanced.sql → 5 个新 sql 文件
1. `sql/overlay_branch_scan_pk.sql`：
   - 头 prereq guard：**原 L1-L43** 0. Prerequisite guard 段
   - 顺序拼接：A (L44-200) → B (L201-366) → C (L367-498) → E (L499-658)
   - 每 Section 之间插入 `RESET overlay_branch.current;`（清 ctx）；尾 DROP 各自的 bs_user/bs_basic/bs_ret/bs_nonpk
2. `sql/overlay_branch_scan_type.sql`：头 prereq guard + F (659-761) → G (762-839) → H (840-974) → I (975-1100) → J (1101-1176)
   - 注意 Section G 引用了 A 的 `bs_user`？**查 grep `from bs_user` in G 段**：若真有，在 G 开头补建表（但按 Section H 注释"we reuse bs_user from G"：Section H 可能也引用 G 的 bs_user，因此 F-G-H-I-J 之间不 DISCARD，保持对象存在）
3. `sql/overlay_branch_puredelta_mix.sql`：头 prereq guard + K (1177-1239) + L (1240-1286) + M (1287-1340) + N (1341-1405) + O (1406-1468) + P (1469-1504) + Q (1505-1536)
4. `sql/overlay_branch_upsert.sql`：头 prereq guard + S (1537-1621) + T (1622-1643) + U (1644-1665) **三 section 保持原顺序，中间不插 reset，保留分支状态**
5. `sql/overlay_branch_rentry.sql`：头 prereq guard + R-ENTRY (1666-1812)

### Step 3 — 更新 Makefile REGRESS 列表
旧 L26：
```
REGRESS = overlay_branch_basic overlay_branch_user overlay_branch_advanced overlay_branch_mvcc overlay_branch_puredelta
```
新（按依赖升序：basic→user→扫描核心→扫描类型→纯 delta 混合→UPSERT→puredelta(原 304 行，保留)→mvcc 基础→mvcc 边界→bug 回归 rapi/rctx/rversion→rentry）：
```
REGRESS = \
  overlay_branch_basic \
  overlay_branch_user \
  overlay_branch_scan_pk \
  overlay_branch_scan_type \
  overlay_branch_puredelta_mix \
  overlay_branch_upsert \
  overlay_branch_puredelta \
  overlay_branch_mvcc_usage \
  overlay_branch_mvcc_bounds \
  overlay_branch_mvcc_rapi \
  overlay_branch_mvcc_rctx \
  overlay_branch_mvcc_rversion \
  overlay_branch_rentry
```

### Step 4 — 删除旧 2 个巨 sql + 对应 expected
- 删除 `test/regress/sql/overlay_branch_mvcc.sql`
- 删除 `test/regress/sql/overlay_branch_advanced.sql`
- 删除 `test/regress/expected/overlay_branch_mvcc.out`
- 删除 `test/regress/expected/overlay_branch_advanced.out`

### Step 5 — 跑 make check + cp expected + 2× clean rebuild
1. `rm -rf test_output tmp_check; make check prefix=/home/ubuntu/pg17`
2. 逐个检查 `test_output/regression.diffs`：
   - 理想情况：全部文件 PASS / diffs 为 0（只有 expected 和 actual 的"无 diff"空）
   - 若某新文件出 FAIL 标记：
     * **跨 session 依赖泄漏**：某 Section 引用了上一 session 的 branch/表 → 在对应小文件开头补 CREATE TABLE/BRANCH（最常遇到）
     * **GUC 残留**：某段未 RESET 变量 → 段尾加 `RESET overlay_branch.*;`
     * **FR1 NOTIFY 消息顺序**：Section N 的 NOTIFY 消息跨 session 后 payload 文本不一致 → 查 expected/actual 具体差异
3. 全部 PASS 后：
   ```
   for f in scan_pk scan_type puredelta_mix upsert rentry mvcc_usage mvcc_bounds mvcc_rapi mvcc_rctx mvcc_rversion; do
     cp test_output/results/overlay_branch_$f.out test/regress/expected/overlay_branch_$f.out
   done
   ```
4. AGENTS §3 2× clean rebuild 0 diff（`make clean install; make check` × 2）。

## Dependencies and Considerations
- **跨 session 依赖是 #1 坑**：原 2000 行文件因为在同一会话，Section 之间可以悄悄共享 `bs_user` / `b_notcurrent_A` 等对象；拆分后**不同 session** 必须重建。若 G 段真的引用 A 段的 bs_user，必须在 scan_type.sql 头把 bs_user 建表 SQL 复制一份。
- **pg_regress 每个 test 的 database name**：`contrib_regression`。所有 extension 对象都建在这个 db 里，session 结束时 pg_regress 会 DROP DATABASE。因此**不需要担心小文件之间的对象名冲突**（不同文件用同名表没问题，都是各自 session 内）。
- **Idempotency**：小文件开头必须用 `CREATE TABLE IF NOT EXISTS`、`CREATE EXTENSION IF NOT EXISTS`、`DISCARD BRANCH`/`discard_branch`。避免测试偶发失败。
- **Section S→T→U 依赖链必须原封不动**：S 建 branch bs_S_br，T apply 此 branch，U 在其后验证 discard；**中间绝对不能插 DISCARD ALL**。已在 Step 2.4 明确声明保留原序无 reset。
- **不要改 basic/user/puredelta**：这三个在合理范围（<800 行），用户没提，不动。避免不必要的 diff 面。

## Validation
1. **Step 5 前 make check 全绿**：`make check prefix=/home/ubuntu/pg17` 输出 `1..13`（L3 共 13 个 test），`All 13 tests passed.`
2. **Step 5.3 cp 后 actual ≡ expected**：`diff -r test_output/results/ test/regress/expected/` 无任何差异。
3. **AGENTS §3 ×2 clean rebuild 0 diff**：2 次 `make clean install + make check` 全 13/13 ok，`# All 13 tests passed.` + L1 isolation 8/8 ok。
4. **文件行数检查**：`wc -l test/regress/sql/*.sql` 全部 <= 800 行（目标几百行）。

## Risks
| 风险 | 概率 | 处理 |
|---|---|---|
| 拆分后某 Section 遗漏了前 Section 的 CREATE TABLE/BRANCH → test FAIL | 中 | Step 5.2 时 grep `FROM bs_` / `use_branch('bs_` 在该段，对应 CREATE TABLE SQL 从原文件前半段**原样拷贝进新文件头** |
| GUC `overlay_branch.current` 残留（段间共享 session 状态） → 断言空 | 低 | 每段结束统一 `RESET overlay_branch.current; SET client_min_messages = notice;` |
| FR1 NOTIFY 消息时间戳 / 异步顺序不一致 → diff | 极低 | pg_regress 用 single-session，Section N 在新文件里独立 session，NOTIFY 只会看到自己发的。若出 diff：改 assertion 用 `substring(msg from 'bid=...')` 校验关键字段，忽略时间戳（原 Section O 代码里应该已做） |
| Section H "BUG REGRESSIONS" 引用 A 段 bs_user 表 → scan_type.sql 缺少表 | 中 | 用 `sed -n '840,974p' advanced.sql \| grep -E 'bs_user|FROM bs|INSERT INTO'` 确认是否真有引用；有则把 A 段 bs_user 建表+INSERT 复制到 scan_type.sql 头。 |
| expected 对齐阶段出现 `—` 破折号语法残留 | 极低 | 上批 R_VERSION 里已手动修掉语法破折号；这次是纯文件搬运，不涉及新 SQL 语句编写 |
