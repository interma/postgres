# Overlay Branch — 目标 SQL 接口与输出示例

> 本文档从**用户视角**描述 Overlay Branch 扩展未来要达到的完整 SQL 接口和典型输出。
> 文中的示例就是我们最终要跑通的回归测试用例基线。
>
> V1 范围：仅支持**有主键的普通 heap 表**，Live Branch（Latest Main + Branch Delta）语义。

---

## 目录

- [0. 前置：安装扩展与建表](#0-前置安装扩展与建表)
- [1. 分支管理](#1-分支管理)
  - [1.1 CREATE BRANCH — 创建分支](#11-create-branch--创建分支)
  - [1.2 USE BRANCH — 切换当前分支 / 离开分支](#12-use-branch--切换当前分支--离开分支)
  - [1.3 current_branch() — 查询当前分支](#13-current_branch--查询当前分支)
  - [1.4 SHOW BRANCHES / list_branches() — 列举分支](#14-show-branches--list_branches--列举分支)
  - [1.5 DISCARD BRANCH — 丢弃分支](#15-discard-branch--丢弃分支)
- [2. 在分支里写数据（Write Redirect 自动生效）](#2-在分支里写数据write-redirect-自动生效)
  - [2.1 INSERT](#21-insert)
  - [2.2 UPDATE](#22-update)
  - [2.3 DELETE](#23-delete)
- [3. 在分支里读数据（BranchScan 自动叠加）](#3-在分支里读数据branchscan-自动叠加)
  - [3.1 叠加语义总览](#31-叠加语义总览)
  - [3.2 完整叠加示例](#32-完整叠加示例)
  - [3.3 分支读不到其它分支的改动](#33-分支读不到其它分支的改动)
- [4. APPLY BRANCH — 把分支增量合入 Main（带冲突检测）](#4-apply-branch--把分支增量合入-main带冲突检测)
  - [4.1 APPLY 成功路径](#41-apply-成功路径)
  - [4.2 APPLY 冲突路径](#42-apply-冲突路径)
- [5. Live Branch 语义：感知 Main 变化](#5-live-branch-语义感知-main-变化)
- [6. V1 不支持的场景](#6-v1-不支持的场景)

---

## 0. 前置：安装扩展与建表

```sql
-- 安装扩展（会创建 pg_branch、pg_branch_delta 两张系统表和 6 个函数）
CREATE EXTENSION overlay_branch;
CREATE EXTENSION
```

```sql
-- 准备一张有主键的普通表（V1 硬性要求：必须有 PK）
CREATE TABLE products (
    id    integer PRIMARY KEY,
    name  text    NOT NULL,
    price integer NOT NULL
);
CREATE TABLE

-- 写入主表基线数据
INSERT INTO products VALUES
  (1, 'Apple',  10),
  (2, 'Banana',  5),
  (3, 'Cherry', 20);
INSERT 0 3
```

---

## 1. 分支管理

### 1.1 CREATE BRANCH — 创建分支

```sql
-- 最简语法（V1 默认就是 isolation = 'live'）
CREATE BRANCH agent_workspace;
CREATE BRANCH

-- 或者显式指定 isolation
CREATE BRANCH frozen_snapshot WITH (isolation = 'snapshot');
CREATE BRANCH
```

失败示例：

```sql
-- 重名会报错
CREATE BRANCH agent_workspace;
ERROR:  branch "agent_workspace" already exists
```

> 说明：CREATE BRANCH 只会在 `pg_branch` 里插入一行元数据，
> **不会拷贝任何数据**，成本 ≈ 一次单行 INSERT。

---

### 1.2 USE BRANCH — 切换当前分支 / 离开分支

```sql
-- 进入一个分支（会话级）
USE BRANCH agent_workspace;
SET

-- 等价的 GUC 写法（效果完全相同）
SET overlay_branch.current = 'agent_workspace';
SET
```

```sql
-- 离开当前分支，回到 Main（直接读写主表）
USE BRANCH NONE;
SET

-- 等价写法
RESET overlay_branch.current;
SET
```

失败示例：

```sql
USE BRANCH does_not_exist;
ERROR:  branch "does_not_exist" does not exist
```

---

### 1.3 current_branch() — 查询当前分支

```sql
-- 在 Main 里
SELECT current_branch();
 current_branch
----------------

(1 row)

-- 进入分支后
USE BRANCH agent_workspace;
SET
SELECT current_branch();
 current_branch
----------------
 agent_workspace
(1 row)
```

---

### 1.4 SHOW BRANCHES / list_branches() — 列举分支

```sql
-- 表函数形式，可 JOIN、可过滤
SELECT branch_id, branch_name, state, mode, delta_count
FROM list_branches();

 branch_id |   branch_name    | state  |  mode  | delta_count
-----------+------------------+--------+--------+-------------
         1 | agent_workspace  | active | live   |           0
         2 | frozen_snapshot  | active | snapshot|          0
(2 rows)
```

字段说明：

| 字段 | 含义 |
|------|------|
| `branch_id` | 内部递增 ID |
| `branch_name` | 分支名（唯一） |
| `owner` | 创建者 oid |
| `created_at` | 创建时间 |
| `mode` | `live` 或 `snapshot` |
| `state` | `active` / `applied` / `discarded` |
| `delta_count` | 该分支目前 `pg_branch_delta` 中的行数 |

---

### 1.5 DISCARD BRANCH — 丢弃分支

```sql
-- 丢弃一个不再需要的分支（会删除它所有 delta，**不会**动 Main 的数据）
DISCARD BRANCH frozen_snapshot;
DISCARD BRANCH

-- 丢弃后 state 变成 discarded，delta 被清空，list_branches 还能看到（除非后续 VACUUM）
SELECT branch_name, state, delta_count FROM list_branches() WHERE branch_name = 'frozen_snapshot';
  branch_name   |   state   | delta_count
----------------+-----------+-------------
 frozen_snapshot | discarded |           0
(1 row)
```

如果丢弃的是**当前正在使用**的分支：

```sql
USE BRANCH agent_workspace;
SET
DISCARD BRANCH agent_workspace;
NOTICE:  discarding current branch "agent_workspace", reverting to Main
DISCARD BRANCH

SELECT current_branch();
 current_branch
----------------

(1 row)
```

---

## 2. 在分支里写数据（Write Redirect 自动生效）

**进入分支后，所有普通的 INSERT / UPDATE / DELETE 都不会写主表，只会写 `pg_branch_delta`。**
用户不需要改 DML 语法。

```sql
USE BRANCH agent_workspace;
SET
```

### 2.1 INSERT

```sql
INSERT INTO products VALUES (4, 'Date', 30);
INSERT 0 1
```

> 上面这行实际是写入 `pg_branch_delta`：
> `(branch_id=1, relid=<products OID>, key='4', op='I', tuple_data=<序列化后的 (4,Date,30)>)`

### 2.2 UPDATE

```sql
UPDATE products SET price = 100 WHERE id = 1;
UPDATE 1
```

> 实际写入 `pg_branch_delta`（如果同一个 (branch,rel,key) 已经有行就做 UPSERT，保证一条 tuple 只有一条最新 delta）：
> `op='U', old_version=<主表 id=1 那一行的 xmin/ctid>, tuple_data=<(1,Apple,100)>`

### 2.3 DELETE

```sql
DELETE FROM products WHERE id = 2;
DELETE 1
```

> 实际写入 `pg_branch_delta`：
> `op='D', old_version=<主表 id=2 的版本>, tuple_data=NULL`

---

## 3. 在分支里读数据（BranchScan 自动叠加）

### 3.1 叠加语义总览

BranchScan 自动对 `主表最新可见版本` ⊕ `分支 Delta` 做合并：

| Delta 操作 | Base 有 | Base 无 | 输出 |
|-----------|---------|---------|------|
| `I`(nsert) | — | ✅ | 输出 Delta 新 tuple |
| `U`(pdate) | ✅ | — | **输出 Delta 的 tuple**（覆盖 Base） |
| `D`(elete) | ✅ | — | **屏蔽** Base 的 tuple（输出中看不到） |
| 无 Delta  | ✅ | — | 原样输出 Base tuple |
| 无 Delta  | — | — | 看不到 |

### 3.2 完整叠加示例

沿用 [§2](#2-在分支里写数据write-redirect-自动生效) 写过的 3 条 DML，此时：

**主表 products（Main）**：

```sql
-- 回到 Main 看一下，主表没被改动（这是 Overlay Branch 的核心承诺）
USE BRANCH NONE;
SET
SELECT * FROM products ORDER BY id;
 id |  name  | price
----+--------+-------
  1 | Apple  |    10
  2 | Banana |     5
  3 | Cherry |    20
(3 rows)
```

**在分支里看（BranchScan 叠加）**：

```sql
USE BRANCH agent_workspace;
SET
SELECT * FROM products ORDER BY id;
 id |  name  | price
----+--------+-------
  1 | Apple  |  100     ← Delta UPDATE 覆盖了 Base 的 10
                          id=2 Banana 被 Delta DELETE 屏蔽了，看不到
  3 | Cherry |   20     ← Base 原样，因为无 Delta
  4 | Date   |   30     ← Delta INSERT 新增行
(3 rows)
```

叠加过程的可视化：

```
Main(products)         Delta(agent_workspace)      BranchScan 输出
===============        ======================      ================
id=1 Apple  10   ──U── id=1 → (Apple,100)   ────►  id=1 Apple 100
id=2 Banana  5   ──D── id=2 → DELETE         ──┐
                                               ├─►  (被屏蔽)
id=3 Cherry 20        (无 Delta for id=3)   ────►  id=3 Cherry 20
                      id=4 → I(Date,30)     ────►  id=4 Date 30
```

### 3.3 分支读不到其它分支的改动

```sql
CREATE BRANCH branch_B;
CREATE BRANCH

USE BRANCH branch_B;
SET
-- branch_B 没改过任何东西，看到的就是 Main 原样
SELECT * FROM products ORDER BY id;
 id |  name  | price
----+--------+-------
  1 | Apple  |    10
  2 | Banana |     5
  3 | Cherry |    20
(3 rows)

-- agent_workspace 的改动，branch_B 完全不可见（分支隔离）
USE BRANCH agent_workspace;
SET
SELECT * FROM products WHERE id = 1;
 id | name  | price
----+-------+-------
  1 | Apple |  100
(1 row)
```

---

## 4. APPLY BRANCH — 把分支增量合入 Main（带冲突检测）

`APPLY BRANCH` 语义：**把分支当前的 delta 当作一次新的 Main 事务，逐条做乐观冲突验证再提交**。
不做 `ours/theirs`，冲突就报错，交由业务层（Agent / 应用）自行解决。

### 4.1 APPLY 成功路径

准备：agent_workspace 里的改动还是 §2/§3 的内容（id=1 U→100，id=2 D，id=4 I→Date/30）。
**Main 这边没有动过这些行，所以版本没变，不会冲突。**

```sql
USE BRANCH NONE;           -- Apply 必须在 Main 执行（或者哪个会话都能执行，只要它在 Main 上操作）
SET

-- 先看一下 Main 现状
SELECT * FROM products ORDER BY id;
 id |  name  | price
----+--------+-------
  1 | Apple  |    10
  2 | Banana |     5
  3 | Cherry |    20
(3 rows)

-- 执行 APPLY
APPLY BRANCH agent_workspace;
APPLY BRANCH
```

Apply 后，Main 已经真的改了，branch 的 state 变成 `applied`，delta 保留（可审计）：

```sql
SELECT * FROM products ORDER BY id;
 id |  name  | price
----+--------+-------
  1 | Apple  |  100      ← 更新了
                           id=2 被删除了
  3 | Cherry |   20
  4 | Date   |   30      ← 新增了
(3 rows)

SELECT branch_name, state, delta_count FROM list_branches() WHERE branch_name = 'agent_workspace';
  branch_name   |  state  | delta_count
----------------+---------+-------------
 agent_workspace | applied |           3
(1 row)
```

### 4.2 APPLY 冲突路径

场景：branch 创建后，**Main 自己也改了 id=1**，然后 branch 再 APPLY ——
因为 `base_version != current_version`，检测到冲突。

```sql
-- 初始化：先在 Main 把基线恢复（为了演示，实际不会回滚 applied）
-- 这里假设新建了一个干净的 branch 叫 'alice'，做了 UPDATE id=1 price = 999
USE BRANCH alice;
SET
UPDATE products SET price = 999 WHERE id = 1;
UPDATE 1
```

```sql
-- 与此同时（并发场景），Main 也改了 id=1
USE BRANCH NONE;
SET
UPDATE products SET price = 42 WHERE id = 1;     -- Main 先提交了这个改动
UPDATE 1
```

现在 alice 分支的 delta 里记录的 `old_version` 是当初写 delta 时 Main 上 id=1 的版本（xmin=X1），
而当前 Main 上 id=1 的 xmin 已经变成 X2。Apply 时：

```sql
APPLY BRANCH alice;
ERROR:  branch conflict on products(id=1)
DETAIL:  base_version    = <xmin=..., ctid=(0,3)>
         current_version = <xmin=..., ctid=(0,12)>
         branch value    = (1, 'Apple', 999)
HINT:  The row was modified in Main after the branch touched it.
       Re-read current state and retry in a new branch.
```

**Overlay Branch 不会自作主张做 ours/theirs merge。** 错误信息里给了业务层足够的
信息（行定位、两个版本、分支拟写入的值），让 Agent / 应用自己决定怎么办。

---

## 5. Live Branch 语义：感知 Main 变化

Live Branch（V1 默认模式）不是严格的数据库 fork，它的语义是：

> **Latest Main + Branch Delta 的叠加**

也就是说：**没被分支改过的行，永远跟着 Main 最新提交的版本走**。
分支改过的行，则一直看到分支自己的版本。

```sql
-- Main 里 id=3 没被任何分支动过
USE BRANCH NONE;
SET
UPDATE products SET price = 9999 WHERE id = 3;   -- Main 涨价了
UPDATE 1

-- 进入一个仍然 active 的分支（比如 branch_B，它从没动过 id=3）
USE BRANCH branch_B;
SET
SELECT * FROM products WHERE id = 3;
 id |  name  | price
----+--------+------
  3 | Cherry | 9999                   ← 看到了 Main 最新的涨价！
(1 row)

-- agent_workspace 改过 id=1，所以即使 Main 动了 id=1，它也看到自己的版本
USE BRANCH agent_workspace;
SET
SELECT id, price FROM products WHERE id = 1;   -- 它曾把 id=1 改成 100
 id | price
----+-------
  1 |   100                          ← 仍然看到分支自己覆盖过的值，不跟随 Main
(1 row)
```

这更贴近 AI Agent / Workspace 场景：Agent 需要保留自己做的草稿修改，
同时又要**感知世界其它部分的变化**（库存、别人的提交、价格更新…）。

---

## 6. V1 不支持的场景

V1 目标是先证明 Overlay Branch 模型成立，所以下列场景会**明确报错**：

| 场景 | 错误信息（示例） |
|------|-----------------|
| 对**没有主键**的表写 DML | `ERROR:  table "no_pk_table" has no primary key; Overlay Branch V1 requires a primary key on all modified tables` |
| 在分支里执行 DDL（`ALTER TABLE` / `CREATE TABLE`） | `ERROR:  DDL inside a branch is not supported in V1` |
| `ON CONFLICT` / UPSERT | `ERROR:  ON CONFLICT inside a branch is not implemented in V1` |
| 分区表（partitioned table） | `ERROR:  overlay branch does not support partitioned tables in V1` |
| 外键级联写 / trigger 写 | `ERROR:  overlay branch does not support FK-triggered writes in V1` |
| 对 `pg_branch` / `pg_branch_delta` 本身做用户写 | `ERROR:  cannot modify catalog table "pg_branch" directly` |

这些限制都会在后续版本逐步解除。V1 能跑通「有 PK 的普通 heap 表 + I/U/D + BranchScan SeqScan + APPLY/DISCARD + conflict detection」就已经证明了模型。
