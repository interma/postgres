# overlay_branch — PostgreSQL 数据库级"草稿分支"扩展

> 在 PostgreSQL 里给 AI Agent / 草稿式工作流提供 Git-Branch 风格的隔离能力，无需修改内核。

## 它是什么

`overlay_branch` 允许你在**不拷贝数据**的前提下创建数据库级草稿分支：

- **写自动重定向** — 分支内 INSERT/UPDATE/DELETE 写入独立 Delta 表，不碰主表
- **读自动叠加** — 查询自动做「主表最新版本 ⊕ 本分支 Delta」合并
- **Live 默认语义** — 没改过的行跟随 Main 最新提交，改过的行保留分支版本
- **事务性 Apply** — 合入 Main 时带乐观冲突检测，冲突报错交由业务层处理

典型用途：AI Agent 工作流、交互式草稿编辑、多版本数据对比、A/B 变更预演。

## 5 分钟快速上手

```sql
-- 0) 前提：已 CREATE EXTENSION，且 postgresql.conf 设置了 shared_preload_libraries='overlay_branch'
CREATE EXTENSION IF NOT EXISTS overlay_branch;

-- 1) 建一张有主键的普通表（硬性要求：DML 表必须有 PK）
CREATE TABLE products (id INT PRIMARY KEY, name TEXT NOT NULL, price INT NOT NULL);
INSERT INTO products VALUES (1,'Apple',10), (2,'Banana',5), (3,'Cherry',20);

-- 2) 创建分支（零拷贝，仅写一行 catalog）
SELECT create_branch('my_draft');

-- 3) 进入分支（会话级）
SELECT use_branch('my_draft');

-- 4) 在分支里写数据 —— 只落到 pg_branch_delta，MAIN 表物理不变
INSERT INTO products VALUES (4,'Date',30);
UPDATE products SET price=100 WHERE id=1;
DELETE FROM products WHERE id=2;

-- 5) 在分支里读：透明 Main⊕Delta 合并
SELECT * FROM products ORDER BY id;
-- 1|Apple |100  (覆盖)
-- 3|Cherry| 20  (Main 原样)
-- 4|Date  | 30  (新增)

-- 6) 回到 Main，确认原封不动
SELECT use_branch(NULL);
SELECT * FROM products ORDER BY id;
-- 1|Apple |10
-- 2|Banana| 5
-- 3|Cherry|20

-- 7) 有信心了 → Apply 合并到 Main
SELECT apply_branch('my_draft');
```

> 完整场景（Snapshot vs Live、跨会话 Kickout、Pure Delta、RETURNING、UPSERT、DDL 限制等）见 [doc/example_sql.md](doc/example_sql.md)。

## 设计概览

无需改内核，利用 CustomScan + Planner Hook + 两张 catalog 表（`pg_branch` / `pg_branch_delta`）做 MVCC 层增量 overlay。

| 模块 | 职责 | 核心文件 |
|---|---|---|
| Branch Context | create/use/apply/discard 生命周期、GUC 状态、advisory lock + CAS 并发控制 | [src/branch_lifecycle.c](src/branch_lifecycle.c) |
| Write Redirect | 拦截 DML 改写到 delta、UPSERT 4-way 分发、pure-delta 后处理、RETURNING | [src/write_redirect.c](src/write_redirect.c) |
| BranchScan | Planner hook 注入 CustomScan，2-pass 叠加 MAIN+delta，typed PK 比较 | [src/branch_scan.c](src/branch_scan.c) |
| Delta Store | PK/行序列化、old_version token、SPI 读写 delta | [src/delta_store.c](src/delta_store.c) |

## 文档

| 文档 | 说明 |
|------|------|
| [doc/example_sql.md](doc/example_sql.md) | **用户视角**：全套 SQL 接口 + 期望输出 |
| [doc/design.md](doc/design.md) | 整体架构与设计决策 |
| [doc/internals.md](doc/internals.md) | 内部实现细节、corner case、关键不变量 |
| [doc/branchscan.md](doc/branchscan.md) | BranchScan 节点内部实现 |
| [doc/multi_session_mvcc.md](doc/multi_session_mvcc.md) | 跨会话并发：锁协议、失效机制、snapshot 语义 |
| [doc/ops_runbook.md](doc/ops_runbook.md) | 运维手册：安装、故障排查、监控 |
| [doc/test_coverage_index.md](doc/test_coverage_index.md) | 测试覆盖索引 |
| [doc/progress_tracker.md](doc/progress_tracker.md) | 开发进度跟踪 |

## 目录结构

```
overlay_branch/
├── include/          # C 头文件
├── src/              # C 源代码（5 个文件）
├── test/
│   ├── regress/      # L3 pg_regress（11 套）
│   ├── isolation/    # L1 pg_isolation_regress（8 套）
│   └── bench/        # L2 pgbench 压测
├── doc/              # 设计与示例文档
├── overlay_branch--1.0.sql
├── overlay_branch.control
├── Makefile
└── README.md
```

## 构建 & 安装

```bash
cd /home/ubuntu/work/postgres
./configure --prefix=/home/ubuntu/pg17 --enable-cassert --enable-debug
cd contrib/overlay_branch
make -j4 && make install
```

> ⚠️ 启用前必须在 `postgresql.conf` 中设置 `shared_preload_libraries = 'overlay_branch'` 并重启 postmaster。

```sql
CREATE EXTENSION overlay_branch;
```

## 跑测试

```bash
cd contrib/overlay_branch

# L3 + L1 一键全跑（TEMP-INSTANCE 模式）
make check

```

## 当前状态

- ✅ 核心功能全部交付：分支生命周期、透明读写叠加、apply/discard、snapshot/live 双模式
- ✅ 测试绿标：L3 11/11 + L1 8/8 全 PASS（2× clean rebuild）
- ⚠️ MVP 限制：仅支持有 PK 的普通 heap 表；不支持分区表、外键、MERGE、COPY FROM、Data-Modifying CTE 等（见 example_sql.md §5）

## 许可证

PostgreSQL License。
