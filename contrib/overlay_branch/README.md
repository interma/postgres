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

## 设计文档

| 文档 | 说明 |
|------|------|
| [设计草案](doc/design.md) | 完整架构与设计决策：四层模型 (Branch Context / Write Redirect / BranchScan / Delta Store)、核心语义、V1 范围与未来扩展方向 |
| [目标 SQL 与输出示例](doc/example_sql.md) | **用户视角的最终目标接口**：CREATE/USE/DISCARD/APPLY BRANCH 全套 SQL + 期望输出、BranchScan 叠加效果演示、Apply 成功与冲突路径、Live Branch 语义演示 |
| [开发进度跟踪](doc/progress_tracker.md) | V1 (手动 SRF) 与 V2 (BranchScan 透明 CustomScan)  |

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
├── test/                       # regress回归测试
├── doc/                        # 设计文档与示例
├── overlay_branch.control      # 扩展控制文件
├── overlay_branch--1.0.sql     # CREATE EXTENSION 安装脚本
├── Makefile                    # in-tree / PGXS 双模式构建
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
make check

# or (B) USE-EXISTING 模式（复用一台已经把 shared_preload_libraries 配置好并已重启的 postmaster）
make installcheck EXTRA_REGRESS_OPTS="--use-existing --host=/tmp --port=15433"

# or (C) 手动 pg_regress（只在真的需要时用 — 下面 4 个参数一个都不能漏！）
pg_regress overlay_branch_basic \
  --inputdir=./test --outputdir=./test_output \
  --temp-config=$(pwd)/test/temp_instance_shared_libs.conf \
  --bindir=$(pg_config --bindir)
```

## 当前状态 & 开发进度

- ✅ **V1 MVP (手动 SRF 模式)**：Step 1/2/3/4/5/7/8 全部完成，`overlay_main_plus_delta(regclass)` 手动调用
- ✅ **V2 MVP (透明 BranchScan 模式)**：Step 全部完成，`SELECT *` 在活动分支下自动做 Main⊕Delta；两条 pg_regress 基线 0 diff

详细 Step 交付物、验收标准、归档历史陷阱、以及 "Step 6 为什么留空" 的完整说明见：
[doc/progress_tracker.md](doc/progress_tracker.md)

## 许可证

PostgreSQL License（同 PG contrib 其余扩展）。
