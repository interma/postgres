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

## 目录结构

```
overlay_branch/
├── include/               # C 头文件 (类型声明、函数声明)
│   └── overlay_branch.h
├── src/                   # C 源代码
│   └── overlay_branch.c   # 模块 init / GUC / 钩子 / 6 个 SQL-callable 函数
├── test/                  # 回归测试
│   ├── sql/               #   测试输入 SQL
│   └── expected/          #   期望输出 (待填充)
├── doc/                   # 设计文档 & 目标接口示例
│   ├── design.md
│   └── example_sql.md
├── overlay_branch.control # 扩展控制文件
├── overlay_branch--1.0.sql# CREATE EXTENSION 安装脚本
├── Makefile               # in-tree / PGXS 双模式构建
├── meson.build            # Meson 构建定义
└── README.md              # 本文件
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

安装后在数据库里启用扩展：

```sql
CREATE EXTENSION overlay_branch;
```

验证安装：

```sql
SELECT current_branch();   -- 未进入分支时返回 NULL
```

## 当前进度

- ✅ 扩展骨架（控制文件 / Makefile / meson.build / SQL 安装脚本 / C 头文件 & 源码骨架 / .gitignore）
- ✅ 代码骨架零警告编译通过，C 模块已生成 `overlay_branch.so`
- ✅ GUC 系统：`overlay_branch.current`、`overlay_branch.enabled` 已注册
- ✅ 钩子链：`ExecutorStart/Run/Finish/End_hook`、`ProcessUtility_hook` 已链式安装
- ✅ 6 个 SQL-callable 函数桩：`create_branch` / `use_branch` / `current_branch` / `list_branches` / `apply_branch` / `discard_branch`
- 🔜 下一层：Delta Store（`pg_branch` / `pg_branch_delta` 的真实读写实现）
- 🔜 之后：BranchScan（CustomScan 做 Base + Delta 叠加）、BranchModify（写重定向）、Apply 冲突检测

## 许可证

PostgreSQL License（同 PG contrib 其余扩展）。
