# PostgreSQL Overlay Branch 设计草案

> **核心建议**：不要从"复制数据库"实现 Branch，而是在 PostgreSQL 内核里实现 **Overlay Branch** —— 主表不动，Branch 只保存自己的增量；读时把 Base Snapshot 和 Branch Delta 合并；结束时 `APPLY` 或 `DISCARD`。

---

## 目录

- [最小模型](#最小模型)
- [写操作](#写操作)
- [读操作](#读操作)
- [四层实现架构](#四层实现架构)
  - [第一层：Branch Context](#第一层-branch-context)
  - [第二层：Write Redirect](#第二层-write-redirect)
  - [第三层：BranchScan](#第三层-branchscan)
  - [第四层：Delta Store](#第四层-delta-store)
- [APPLY 语义与冲突检测](#apply-语义与冲突检测)
- [Snapshot 的工程陷阱](#snapshot-的工程陷阱)
- [两种 Branch 语义](#两种-branch-语义)
- [第一版最小功能集](#第一版最小功能集)
- [内部数据结构](#内部数据结构)
- [真正的技术内核](#真正的技术内核)
- [实现顺序建议](#实现顺序建议)

---

## 最小模型

```sql
CREATE BRANCH b1;
```

一个 Branch 的结构：

```
Branch B = {
    base_snapshot,
    delta_tables,
    branch_metadata
}
```

---

## 写操作

正常执行：

```sql
UPDATE t SET x = 10 WHERE id = 1;
```

在 Branch 模式下，**不直接写 `t`**，而是写入 Delta 表：

```
branch_delta_t
---------------
branch_id
op            -- I/U/D
pk
new_tuple
old_version   -- 用于冲突验证
```

---

## 读操作

BranchScan 定义为 Base 与 Delta 的叠加：

```
BranchScan(t)
    =
    BaseScan(t @ base_snapshot)
    ⊕
    DeltaScan(branch_delta_t)
```

**Delta 优先级高于 Base：**

| Delta 操作 | 效果 |
|-----------|------|
| Delta UPDATE | 覆盖 Base tuple |
| Delta DELETE | 屏蔽 Base tuple |
| Delta INSERT | 额外返回 |

### 示例

假设主表：

```
id | value
---+------
1  | A
2  | B
3  | C
```

Branch Delta：

```
id | op | value
---+----+------
1  | U  | X
2  | D  |
4  | I  | Y
```

BranchScan 输出：

```
1  X
3  C
4  Y
```

---

## 四层实现架构

### 第一层：Branch Context

```sql
CREATE BRANCH agent_123;
SET branch = 'agent_123';
```

内部维护：

```c
typedef struct BranchContext
{
    BranchId id;
    SnapshotId base;
    TimestampTz created_at;
    BranchState state;
} BranchContext;
```

> ⚠️ **注意**：不要真的持有一个 PostgreSQL Snapshot 几十分钟，否则 `xmin` 一直不推进，`VACUUM` 会被拖住。第一版可以先规定 Branch 读取"创建时已提交数据"的语义，但物理实现不依赖 long-lived MVCC snapshot。

### 第二层：Write Redirect

**正常路径：**

```
Executor
   ↓
ModifyTable
   ↓
heap_insert/update/delete
```

**Branch 路径：**

```
Executor
   ↓
ModifyTable
   ↓
BranchModify
   ↓
Delta Store
```

> 不建议用 logical decoding。Logical decoding 解决的是"已经写进 PostgreSQL、已经产生 WAL 的变化如何解码出去"，而这里需要的是"这些修改压根不能进入 Main"。正确入口应该在 `Executor / ModifyTable` 之前截住。

> 第一版如果允许改 core，建议直接增加 `BranchModifyTable`，不要为了坚持 extension-only 把自己逼进各种 hook。

### 第三层：BranchScan

最适合做 **CustomScan**。PostgreSQL 允许扩展提供 Custom Scan Path/Plan/Executor，用自己的扫描实现替换普通 relation scan。

Planner 看见 `current_branch != NULL`，把 `SeqScan(t)` 改成 `BranchScan(t)`：

```
           BranchScan
             /    \
            /      \
      Base Scan   Delta Scan
           \        /
            Overlay
               ↓
             Tuple
```

### 第四层：Delta Store

**不要存 Logical WAL**，建议直接存 **Tuple Delta**。

例如连续三次 UPDATE：

```
UPDATE id=1 → A
UPDATE id=1 → B
UPDATE id=1 → C
```

不要存成三条 replay log。Delta 里最后应该是：

```
id=1 → C
```

**Delta Store 是可更新的状态表：**

```
(branch_id, relid, pk) → DeltaTuple
```

类似：

```
Branch Delta Index
        │
        ├── (rel=100, id=1) → UPDATE tuple
        ├── (rel=100, id=2) → DELETE
        └── (rel=100, id=4) → INSERT tuple
```

这样 Branch 活十分钟、执行一万次 UPDATE 同一条记录，查询成本不会变成 replay 一万条日志。

额外保存一个小的验证信息：

```
DeltaTuple {
    operation,
    new_tuple,
    base_tuple_version
}
```

`base_tuple_version` 可以先简单用 `xmin / ctid / row version` 或自定义 `base_version_id`。它不是为了 branch read，而是为了最终 `APPLY`。

---

## APPLY 语义与冲突检测

不实现 `MERGE BRANCH b1;`，而是：

```sql
APPLY BRANCH b1;
```

语义明确：**将 Branch 当前结果，作为一个新的 Main Transaction 重新验证并提交。**

### 示例

Branch B 中：

```
id=1:
  Base: A
  Branch: X
```

Apply 时：

```sql
BEGIN;
SELECT current tuple id=1;
```

**检查：** 当前 Main 是否仍然等于 Branch 创建/读取的那个版本？

- **YES** → `UPDATE ... → X`
- **NO** → **conflict**

### 语义分离

| 操作 | 含义 |
|------|------|
| Branch COMMIT | Branch 内部的一次 transaction commit |
| APPLY BRANCH | **Reality Commit** |

**第一版建议不做"自动冲突解决"**，直接报错：

```
ERROR:
branch conflict on t(id=1)
base version = ...
current version = ...
branch value = ...
```

交给业务层（Agent / Application）自行 resolve，产生新的 transaction。数据库只告诉业务"你的假设已经不成立"，而不是替业务决定 `ours / theirs`。

---

## Snapshot 的工程陷阱

### 严格 fork 语义的问题

```
T0 CREATE BRANCH
    Main: x = 1
T1 Main:
    x = 2
T2 Branch:
    SELECT x
```

如果定义 Branch 是严格 fork，T2 应该看到 `x=1`。但千万不要真的 `RegisterSnapshot()` hold 3 小时，否则 MVCC/VACUUM 会非常难看。

### V1 建议的语义

**Branch = Read Committed Main + Branch Overlay**

```
当前 Main
   +
Branch Delta
```

这样：

- T0 branch sees `x=1`
- T1 main `x=2`
- T2 branch sees `x=2`

除非 Branch 自己修改过（如 `branch x=10`），那么始终看到 `10`。

这其实更接近 **Workspace**，而不是严格意义上的数据库 fork。而且这可能反而更符合 Business-first Agent 场景。

---

## 两种 Branch 语义

### 1. Live Branch（第一版优先）

```sql
CREATE BRANCH b1
WITH (isolation = 'live');
```

语义：**Latest Main + Branch Delta**

AI Agent 通常不是要永久生活在过去，而是：

> 保留自己的修改，同时继续感知现实世界的变化。

这比 Neon 那种 Storage Snapshot Branch 更有意思。

### 2. Snapshot Branch（未来）

```sql
CREATE BRANCH b2
WITH (isolation = 'snapshot');
```

语义：**Frozen Main Snapshot + Branch Delta**

预计 AI Agent **80% 场景**可能更适合 live branch。

---

## 第一版最小功能集

只做 **5 个东西**：

1. `CREATE BRANCH name`
2. `USE BRANCH name`
3. **BranchScan** — Main + Delta Overlay
4. **BranchModify** — INSERT/UPDATE/DELETE → Delta
5. `APPLY BRANCH` / `DISCARD BRANCH`

### 第一版限制范围

只支持：**必须有 PK 的普通 heap 表**

**不碰：**

- partition
- trigger
- FK
- sequence
- generated column
- logical replication
- DDL
- ON CONFLICT
- RETURNING 复杂情况

先证明模型。

---

## 内部数据结构

### pg_branch

```
pg_branch
-----------------
branch_id
branch_name
owner
created_at
mode
state
```

### pg_branch_delta

```
pg_branch_delta
-----------------
branch_id
relid
key
op
old_version
tuple_data
```

> 如果性能起来了，再把 `pg_branch_delta` 从普通 heap 表换成专门的 storage。

---

## 真正的技术内核

最值得做的甚至不是 `CREATE BRANCH` 本身，而是下面这个 primitive：

```
               PostgreSQL Table
                    │
           ┌────────┴─────────┐
           ↓                  ↓
        Base AM          Branch Delta
           │                  │
           └────────┬─────────┘
                    ↓
               Overlay Scan
```

**Overlay Scan + Redirected Modify + Transactional Apply** 才是这个东西的技术内核。`CREATE BRANCH` 只是外面一层 SQL 皮。

这条路线和前面的观点非常一致：

> 数据库没有替业务定义 Branch。数据库只是提供一个廉价的 speculative state primitive，让业务自己决定用它构造 Agent Workspace、审批草稿、规划方案还是别的东西。

---

## 实现顺序建议

先写一个**极小 prototype**：

> **单表 + PK + UPDATE/INSERT/DELETE + Branch SeqScan + APPLY conflict detection**

这个跑通以后，基本就能判断这个想法到底是不是一个真正的新 PostgreSQL primitive。
