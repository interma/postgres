# overlay_branch — PostgreSQL 数据库级"草稿分支"扩展

> Speculative database state using table overlay and delta store.
> 在 PostgreSQL 里给 AI Agent / 草稿式工作流 提供 Git-Branch 风格的隔离能力。

## 它是什么

`overlay_branch` 是一个 PostgreSQL 扩展，它允许你在**不拷贝数据**的前提下，创建一个「数据库级别的草稿分支 (branch)」：

- **Branch 写操作自动重定向** — 分支内的 INSERT / UPDATE / DELETE 不写主表，只写入独立的 Delta 存储（`pg_branch_delta`）。
- **Branch 读操作自动叠加** — 分支内的查询自动做「主表最新可见版本 ⊕ 本分支 Delta」的合并（BranchScan）。
- **Live Branch 默认语义** — 不是严格的数据库 fork；没被分支改过的行永远跟随 Main 的最新提交，改过的行保留分支自己的版本。
- **事务性 Apply** — `APPLY BRANCH` 把分支增量合入 Main，带乐观冲突检测，冲突就报错交由业务层决定 merge 策略。

典型用途：AI Agent 工作流、交互式草稿编辑、多版本数据对比、A/B 变更预演。

## 5 分钟快速上手

```sql
-- 0) 前提：已 CREATE EXTENSION，且 postgresql.conf 设置了 shared_preload_libraries='overlay_branch'
CREATE EXTENSION IF NOT EXISTS overlay_branch;

-- 1) 建一张有主键的普通表（硬性要求：DML 表必须有 PK）
CREATE TABLE products (id INT PRIMARY KEY, name TEXT NOT NULL, price INT NOT NULL);
INSERT INTO products VALUES (1,'Apple',10), (2,'Banana',5), (3,'Cherry',20);

-- 2) 创建分支（零拷贝，仅写一行 catalog；默认 state=active, mode=live）
SELECT create_branch('my_draft');
-- 返回 branch_id（例如 1）

-- 3) 进入分支（会话级 GUC：overlay_branch.current = 'my_draft'）
SELECT use_branch('my_draft');

-- 4) 在分支里写数据 —— 这些写入只落到 pg_branch_delta，MAIN 表 products 物理上未变
INSERT INTO products VALUES (4,'Date',30);           -- Delta Insert
UPDATE products SET price=100 WHERE id=1;             -- Delta Update (覆盖 Main)
DELETE FROM products WHERE id=2;                      -- Delta Delete (墓碑)

-- 5) 在分支里读：透明 Main⊕Delta 合并（BranchScan CustomScan）
SELECT * FROM products ORDER BY id;
-- 1|Apple |100  (覆盖)
-- 3|Cherry| 20  (Main 原样)
-- 4|Date  | 30  (新增)
-- id=2 被删除 → 看不到

-- 6) 回到 Main，确认 Main 原封不动
SELECT use_branch(NULL);
SELECT * FROM products ORDER BY id;
-- 1|Apple |10     ← 仍是 10
-- 2|Banana| 5     ← 仍在
-- 3|Cherry|20     ← 不变 (id=4 在 Main 上不存在)

-- 7) 有信心了 → Apply 合并到 Main（乐观 old_version 冲突检测；有冲突直接报 ERROR）
SELECT apply_branch('my_draft');
-- NOTICE: APPLY BRANCH 'my_draft' completed (state=applied, delta rows deleted)

SELECT * FROM products ORDER BY id;
-- 1|Apple |100   (已应用)
-- 3|Cherry| 20
-- 4|Date  | 30
-- (id=2 已被 apply 删除)
```

> 另见 [doc/example_sql.md](doc/example_sql.md) 涵盖的完整场景：
> Snapshot vs Live 模式、跨会话 Apply Kickout (ERRCODE 55000)、Pure Delta、RETURNING 子句等。

## 设计文档

### 整体设计 & 实现思路（一句话入门）

把"数据库级分支"做成 PostgreSQL 原生扩展**无需改内核**：
**对普通 SQL 完全透明，利用 CustomScan + Planner Hook + 两张 catalog 表 (pg_branch / pg_branch_delta) 做 MVCC 层的增量表 overlay。**

| 模块（4 层） | 做什么（一句话） | 核心 C 文件 |
|---|---|---|
| **① Branch Context 层** | `create/use/apply/discard` 四个 SQL-callable 函数；写 GUC `overlay_branch.current` 标识当前会话在哪个分支；V3 新增 Mode Cache HTAB（bid-keyed）+ CAS-first state 机（FR2 V1 use=零锁，apply/discard 只有 CAS winner 才取 Exclusive advisory）| [src/branch_lifecycle.c](src/branch_lifecycle.c) / [src/overlay_branch.c](src/overlay_branch.c) |
| **② Write Redirect 层** | 截获 `INSERT / UPDATE / DELETE →` 改写到 `pg_branch_delta` 而不是原表（每 tuple 一行：op=I/U/D + PK JSON + old_version token + 序列化 tuple）；Pure-Delta 单独 ExecQual pass；FAIL#1 修复：**DML-only kickout**（纯 SELECT 是 observer，不做 exit_branch_cleanup，防止 DQL 静默 NOTICE suppress 后下条 DML 漂移 MAIN）| [src/write_redirect.c](src/write_redirect.c) |
| **③ BranchScan 透明读层** | `planner_hook` 把对 MAIN 表的 SeqScan 改造成 CustomScan `BranchScan`；两阶段执行：Pass 1 Main（按 Live/Latest 或 FR5 Snapshot frozen 的 snapshot push）收集 CTIDs；Pass 2 用 hash map 同主键合并 Delta（Delta 永远用 LatestSnapshot 扫，所以协作分支的同事写的 WR 在 snapshot 模式下也可见）；ReScan 清零 `result_slots` + `materialized=false`，跨 session 新 plan 能取到新 delta（FR6）| [src/branch_scan.c](src/branch_scan.c) |
| **④ Delta Store 层** | `pg_branch_delta` CRUD SPI path + PK JSON 序列化（typmod 走 `format_type_with_typmod()`，BPCHAR 两端 rtrim）+ Main 行 version token（Apply 乐观冲突检测用的是 Main 当前 tuple 的 xmin/cmin/tid 混合哈希）；FR3 transient applying-state 入口 Guard（state≠active 直接 ERROR 55000，避免 apply 合并期间 delta torn-write）| [src/delta_store.c](src/delta_store.c) |

并发设计文档（跨会话行为、锁、失效机制）详见 [doc/multi_session_mvcc.md](doc/multi_session_mvcc.md)，上面各设计要点和完整 FAIL#1 复现+修复序列、V3 FR2 V1 零锁证明、FR4 NOTIFY+SPI counter fallback、FR5 MAIN-freeze/Delta-always-latest 契约都在其中。

| 文档 | 说明 |
|------|------|
| [多会话并发设计 (主文档)](doc/multi_session_mvcc.md) | **V3 T1→T8 跨会话并发行为、锁协议、FR 设计决策**：FR2 V1 零锁、FR4 NOTIFY+SPI counter 双层失效、FR5 snapshot 语义契约、FAIL#1 漂移漏洞修复、L1/L2/L3 三层测试规范 |
| [设计草案](doc/design.md) | 完整架构与设计决策：四层模型 (Branch Context / Write Redirect / BranchScan / Delta Store)、核心语义、V1 范围与未来扩展方向 |
| [目标 SQL 与输出示例](doc/example_sql.md) | **用户视角的最终目标接口**：CREATE/USE/DISCARD/APPLY BRANCH 全套 SQL + 期望输出、BranchScan 叠加效果演示、Apply 成功与冲突路径、Live Branch 语义演示 |
| [开发进度跟踪](doc/progress_tracker.md) | V1 (手动 SRF) / V2 (BranchScan) / V3 (并发 9/9 green) 阶段交付物与验收结果 |

## 目录结构

```
overlay_branch/
├── include/                    # C 头文件
│   ├── overlay_branch.h        #   公共类型、宏、对外函数声明
│   └── branch_scan.h           #   BranchScan / Planner hook 相关声明
├── src/                        # C 源代码
│   ├── overlay_branch.c        #   模块入口 (_PG_init)、GUC、全部 SQL-callable 函数
│   ├── delta_store.c           #   Delta 存储 CRUD、主键序列化/反序列化、MAIN heap 访问辅助
│   ├── write_redirect.c        #   写重定向 (INSERT/UPDATE/DELETE → delta)、RETURNING、pure-delta ExecQual 过滤
│   ├── branch_scan.c           #   透明读：Planner hook 注入 BranchScan、CustomScan 两阶段合并执行器
│   └── branch_lifecycle.c      #   Branch 生命周期：create / use / apply / discard
├── test/                       # 三层测试（2026-09-19 重构）
│   ├── regress/                #   L3 pg_regress 门禁 4 套：sql/ + expected/
│   ├── isolation/              #   L1 pg_isolation_regress 精确调度：specs/ + expected/ + isolation_schedule
│   └── bench/                  #   L2 pgbench 压测：apply_contention.sql + README
├── doc/                        # 设计文档与示例（非测试类文档）
├── temp_instance_shared_libs.conf   # 测试 harness 公共 shared_preload_libraries 配置
├── overlay_branch.control      # 扩展控制文件
├── overlay_branch--1.0.sql     # CREATE EXTENSION 安装脚本
├── Makefile                    # in-tree / PGXS 双模式构建（REGRESS_OPTS + ISOLATION_OPTS 指向 test/ 下三层）
├── meson.build                 # Meson 构建定义
└── README.md                   # 本文件
```

## 构建 & 安装

当前采用 PostgreSQL contrib in-tree 方式构建（已在源码树 `contrib/overlay_branch/`）：

```bash
# 1) 先在 pg 源码根目录跑过 configure（示例来自当前环境）
cd /home/ubuntu/work/postgres
./configure --prefix=/home/ubuntu/pg17 \
    --enable-cassert --enable-debug \
    CFLAGS="-ggdb -Og -g3 -fno-omit-frame-pointer"

# 2) 构建扩展
cd contrib/overlay_branch
make -j4

# 3) 安装到 prefix
make install
```

> ⚠️ 启用扩展前必须先在 `postgresql.conf` 中追加：`shared_preload_libraries = 'overlay_branch'`，然后 **重启 postmaster**。

安装后在数据库里启用扩展：

```sql
CREATE EXTENSION overlay_branch;
```

验证安装：

```sql
SELECT current_branch();   -- 未进入分支时返回 NULL
```

## 跑测试

```bash
cd contrib/overlay_branch

# (A) TEMP-INSTANCE 模式（推荐，零依赖已有 postmaster，自动启/停）
# 一次跑 L3 (4个 regress) + L1 (5个 isolation spec) 共 9 套 —— 全部 0 diff
make check

# or (B) USE-EXISTING 模式（复用一台已经把 shared_preload_libraries 配置好并已重启的 postmaster）
make installcheck EXTRA_REGRESS_OPTS="--use-existing --host=/tmp --port=15433"

# or (C) 手动 pg_regress（只在真的需要时用 —— L3 4 套）
pg_regress overlay_branch_basic overlay_branch_user overlay_branch_advanced overlay_branch_mvcc \
  --inputdir=./test/regress --outputdir=./test_output \
  --temp-config=$(pwd)/temp_instance_shared_libs.conf \
  --bindir=$(pg_config --bindir)

# or (D) 手动 pg_isolation_regress（L1 5 套，精确调度并发 permutation）
pg_isolation_regress ob_apply_mutex ob_state_inval ob_applying_freeze ob_snapshot_mode ob_branchscan_rescan \
  --inputdir=./test/isolation --outputdir=./output_iso \
  --temp-config=$(pwd)/temp_instance_shared_libs.conf \
  --bindir=$(pg_config --bindir)

# or (E) L2 pgbench 概率压测（高压混合 apply/use/discard/DML，不进 make check 门禁）
#   详细步骤与校验准则见 test/bench/README.md
pgbench -c 32 -T 60 -f test/bench/apply_contention.sql -h /tmp -p 15433 <testdb>
```

## 当前状态 & 开发进度

- ✅ **V1 MVP (手动 SRF 模式)**：`overlay_main_plus_delta(regclass)` 手动调用 2 套回归 0 diff
- ✅ **V2 MVP (透明 BranchScan 模式)**：`SELECT *` 自动 Main⊕Delta，L3 basic/user/advanced 3 套门禁 0 diff
- ✅ **V3 (跨会话并发) T1→T8 全部交付**：L3 4/4 + L1 5/5 = **9/9 zero-diff verified 2×+，stale clean rebuild green**
  - FR2 V1 零锁协议 → SHARED→EXCLUSIVE 升级死锁彻底消除（L1 ob_apply_mutex 9 permutations 无 360s 死锁）
  - FR4 NOTIFY + SPI counter fallback 双层失效机制（L1 ob_state_inval 2 permutations 55000 ERROR kickout）
  - FR5 SNAPSHOT 模式（MAIN freeze / Delta always-latest 契约，L1 ob_snapshot_mode PASS）
  - FAIL#1 静默 MAIN drift write BUG 已根除（CmdType split invariant + DML-only kickout）
  - FR7 shared-memory Pin 机制 CANCELLED-by-design（见 multi_session_mvcc.md B.8）

详细 Step 交付物、验收标准、归档历史陷阱、见 [doc/progress_tracker.md](doc/progress_tracker.md)；并发设计细节见 [doc/multi_session_mvcc.md](doc/multi_session_mvcc.md)。

## 许可证

PostgreSQL License（同 PG contrib 其余扩展）。
