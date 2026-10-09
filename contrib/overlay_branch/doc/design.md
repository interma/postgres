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

#### Write Redirect 核心实现细节（V2 最终版固化）

**主循环 + Pure-delta 独立 pass 双架构：**
```
 ExecutorRun hook 进入 ModifyTable
    ├─ WR MAIN PASS（不变）：
    │   for each 子计划物理 MAIN 行 (outerPlan):
    │      ctid SPI 取 old_version → 写 delta op=U/D
    │    pk 加入 EMITTED set ✔
    │
    └─ WR PURE-DELTA PASS (8-phase，V2-P3 关键扩展)：
         Phase C: 仅 CMD_DELETE 进入 (MVP UPDATE=安全 NOP)
         Phase D: delta_list WHERE op=I AND NOT EMITTED
                    deep copy 在 es_query_cxt 分配
         Phase E: while (outerPlanState scan_ps):
                    qual_scan = scan_ps.qual
                    qual_extra = switch T_IndexScanState      → indexqualorig
                                       T_BitmapHeapScanState → bitmapqualorig
                                       T_IndexOnlyScanState  → recheckqual
         Phase F: ExecQual (qual_scan AND qual_extra)
         Phase G: SAFETY GUARD 👉 qual 全 NULL 立即 continue (防全表 DML nuke)
                    pass=true → delta(op=D, old_version=NULL, key=pk)  ✔ pure delta origin
         Phase H: cleanup (es_query_cxt 自动回收)
```

**入口硬拦截（V2 强约束，绝不 silent）：**
- `PlannedStmt.hasModifyingCTE = true` → ereport ERROR（Data-Modifying CTE 是唯一能把 DML 结果流入外层 SELECT 的合法方式；不严拦直接导致 MAIN heap pollution）
- `if (commandType != CMD_SELECT) return;` 在 Planner hook 最顶（UPDATE/DELETE baserel scan 不应被 BranchScan 注入，ModifyTable 的 ctid 读不走这个）
- Plsner hook + WR hook 双份 guard（冗余 Belt-and-braces）

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

### Apply 3-pass 执行顺序 & Pure-delta 特殊规则（V2 最终版）

**3-Pass 原子顺序（独立 SPI 事务，任一条冲突整体回滚）：**
```
Pass 1 — DELETE deltas 先执行（外键 child → parent 删除安全）
Pass 2 — UPDATE deltas 再执行（含冲突检测）
Pass 3 — INSERT deltas 最后执行（外键 parent → child 插入安全）
```

**Pure-delta 特殊规则 — 绝不能 false-positive CONFLICT：**

```
delta = {op=D, old_version=NULL, key=k}     ← old_version NULL = pure delta 起源
意思：分支里 INSERT k → 分支里 DELETE k，MAIN 从始至终都没有这行

apply 常规逻辑 (old_version != NULL):
   SELECT * FROM main WHERE pk=k            → 0 row → 抛 CONFLICT
                                      ❌ bug11: 纯 delta 不应报冲突

V2 修法 — apply Pass1 顶部加专属 NOP：
   if (dt->old_version == NULL)
       return;    (NOP skip, 不写 MAIN，不冲突)
```

**Pure-delta INSERT/UPDATE 规则：**
- `op=I, old_version=NULL`：分支自插入，MAIN 不存在 → Pass3 直接 `INSERT INTO main`
- `op=U, old_version=NULL`：pure-delta UPDATE（V2 MVP 不应产生；若出现则按 I 语义插入新版本或升级到 V3 再处理）

### Apply 冲突检测 & 主键类型强约束

`old_version = format("<blocknum>:<offset>-x<xmin>")` 的正确性依赖 MAIN 上该行物理版本没有被 rewrite（HOT 更新除外，HOT 保留 ctid 不变）。

- typmod PK：PK 列为 `BPCHAR(N)` / `NUMERIC(p,s)` 时，全链路必须用 `format_type_with_typmod(atttypid, atttypmod)`（format_type_be 只返回基名，长度信息丢失 → BPCHAR `CAST(... AS char)` 变成 `text` → rtrim 后 key mismatch 100%）
- BPCHAR JSON 字符串数组元素 = 两端均 rtrim：`overlay_serialize_pk` 对 BPCHAR 调 `pq_rtrim(…)`；P0 / apply WHERE 子句 cast 用 `CAST (… AS bpchar(N))::text` 外层 `::text` 保留文本语义 — 否则 WR 写 `["PD001  "]`（带空格），apply 查写 `["PD001"]` → 0 row → 误冲突。

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

### V1/V2 的默认语义：Live Workspace

**Branch = Read Committed Main + Branch Overlay**（等价于 `use_branch(name, mode => 'live')`）

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

这其实更接近 **Workspace**，而不是严格意义上的数据库 fork。更符合 Business-first Agent 场景。

### V3 引入的 Snapshot Mode（严格 fork 语义）

V3 支持 `use_branch(name, mode => 'snapshot')`，在进入分支时通过 `RegisterSnapshot(GetTransactionSnapshot())` 冻结 MAIN 读视图，MAIN 表后续写入不再可见；但 delta 仍为最新已提交（支持多 session 协作写同一分支）。详细协议与风险控制见 [multi_session_mvcc.md](./multi_session_mvcc.md) 的 SNAPSHOT Mode 章节。

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

### 2. Snapshot Branch（V3 T5 实装，MAIN freeze + Delta always-latest）

```sql
SELECT use_branch('b2', 'snapshot');
```

语义：**Frozen Main Snapshot + Branch Delta**

V3 已实装：MAIN 表元组按 snapshot taken 时 xmin horizon 冻结；同一 branch 的 live session 写入 WR delta 仍对 snapshot reader 可见（协作友好）。详细契约见 multi_session_mvcc.md §I8.5。

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
- pure-delta UPDATE（V2 保持安全 NOP；需要改 = DELETE + INSERT 等效语义）

**已在 V2 扩展覆盖（第一版限制已部分解除）：**

- RETURNING 子句（INSERT/UPDATE/DELETE 全路径 + pure-delta DELETE）
- Data-Modifying CTE：入口 `PlannedStmt.hasModifyingCTE` 检测，明确 ereport ERROR（绝不静默 MAIN pollution）
- BPCHAR(6)/NUMERIC(10,2) 等带 typmod 的 PK 列：全链路 `format_type_with_typmod()` 替代 format_type_be；BPCHAR PK JSON 序列化两端均 rtrim

**已在 V3 扩展覆盖（跨会话并发 T1→T8）：**

- 2-param `use_branch(name, mode => 'snapshot' | 'live')` + SNAPSHOT MAIN freeze 语义（FR5）
- state machine `active → applying → applied/discarded` CAS gate（FR3）
- FR2 V1 零锁协议（use=zero lock，SHARED→EXCLUSIVE 升级死锁根除；仅 CAS-winner 取 Exclusive advisory）
- FR4 NOTIFY + SPI counter fallback 双层失效机制（post-apply ERROR 55000 kickout）
- L1 精确调度 5 spec × 9 permutation 全绿 + L2 pgbench fixture（见 `test/` 三层）

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

---

## §G02 S13 权限最小化（ACL Double-Defense Matrix）

> 设计目标：最小暴露面 + 不可绕过硬门禁。PG 没有行级 GRANT；分支 OWNER（pg_branch.owner只是一列 oid，不能动态 GRANT。我们用两层。

### §G02.1 角色与保留名

- 内置 NOLOGIN 管理角色：**overlay_branch_administrators**
  - 改名原因：PG 禁止 `pg_` 前缀的用户角色（保留名），原 pg_manage_overlay_branches 直接 ERROR role name reserved。
  - 注释：Members may create overlay branches and administer any branch. Owners of a specific branch implicitly have use/apply/discard privileges on their own branch without being added to this role.
  - 升级注意：**角色名硬编码，任何扩展脚本（包括 AGENTS / SQL 不得改名，不得删除。
- 有效身份三元组：1) Superuser=全权限；2) overlay_branch_administrators 成员=管理员；3) 分支 OWNER（pg_branch.owner==GetUserId())=自己分支的 use/apply/discard。

### §G02.2 两层防御矩阵

| 层 | 位置 | 能力 | 局限 |
|----|------|------|------|
| SQL Layer 1 | overlay_branch--1.0.sql 的 REVOKE/GRANT + SECURITY INVOKER | 阻止纯 SQL 级绕过（直接 @extschema@ fn 调用） | 不能做行级 owner 对比 |
| C Layer 2 | ob_acl_check_lifecycle() + 每个 wrapper 入口第一行 | 真正 owner/mgmt/superuser gate；SQL 层就算被 hack 也逃不掉 | 不代不了 SQL 层 revoke

SQL Layer 1 细目：
- REVOKE ALL ON @extschema@.pg_branch / pg_branch_delta / seq FROM PUBLIC
- GRANT SELECT ON @extschema@.pg_branch TO PUBLIC（public.pg_branch VIEW 已对 PUBLIC SELECT，对称开放 base table SELECT 给 C 层 SPI 查 owner 时不出 42501
- GRANT SELECT,INSERT,UPDATE,DELETE ON @extschema@.pg_branch / pg_branch_delta TO overlay_branch_administrators
- GRANT ALL ON @extschema@.pg_branch_branch_id_seq TO overlay_branch_administrators
- GRANT USAGE ON SCHEMA @extschema@ TO PUBLIC（public.* synonym 用 SECURITY INVOKER，INVOKER 语义下调用者自己需要 schema 权限
- public.pg_branch_delta VIEW REVOKE ALL FROM PUBLIC（R22：delta 行有业务数据，default 无访问
- public.pg_branch VIEW 保持 GRANT SELECT TO PUBLIC

public synonym 关键权衡（为什么不 SECURITY DEFINER：SECURITY DEFINER 切换 current_user = function owner(超级用户)，导致 C 层 GetUserId() 返回 superuser oid，ob_user_is_superuser_or_mgmt 恒真，ACL 被完全绕过。因此固定为 SECURITY INVOKER + search_path=(@extschema@, pg_catalog) + PUBLIC EXECUTE，配合 SCHEMA USAGE。

### §G02.3 C Layer 2：ob_acl_check_lifecycle

实现文件：src/branch_lifecycle.c §G02 helper：
1. ob_lookup_mgmt_role_oid()：AUTHNAME syscache 拿 overlay_branch_administrators 的 Oid
2. ob_user_is_superuser_or_mgmt()：superuser() 或 is_member_of_role(GetUserId(), mgmt_oid)
3. ob_user_is_branch_owner(bid)：SPI SELECT owner FROM @extschema@.pg_branch，比对 owner == GetUserId()
4. ob_acl_check_lifecycle(ObLifecycleOp, name)：统一 ereport(ERROR, 42501 + hint

授权矩阵：
| 操作 | 允许身份 |
|------|---------|
| CREATE_BRANCH | superuser 或 mgmt（OWNER 不允许自己建防 DOS）
| USE_BRANCH    | superuser 或 mgmt 或 owner；name = NULL/空 → EXIT Main 所有人放行
| APPLY_BRANCH  | superuser 或 mgmt 或 owner
| DISCARD_BRANCH| superuser 或 mgmt 或 owner

统一错误码 ERRCODE_INSUFFICIENT_PRIVILEGE(42501)，统一 hint：Must be branch owner, superuser, or member of role "overlay_branch_administrators"。

### §G02.4 Wrapper 调用点（C 层硬门禁）

| 函数名 | 位置 | 检查时机 |
|-------|------|---------|
| overlay_branch_create | src/overlay_branch.c create branch wrapper 顶部 | PG_ARGISNULL 处理后第一行 |
| overlay_branch_use | 同上 | name NULL/empty 直接 return（EXIT Main，Pub 级），否则 ACL |
| overlay_branch_use_with_mode | 同上 | 同 use |
| overlay_branch_apply | 同上 | - |
| overlay_branch_discard | 同上 | - |

### §G02.5 升级与回滚注意

- 角色名永久：任何 ALTER EXTENSION UPDATE 脚本里都必须保持 overlay_branch_administrators，GRANT 链不
- SECURITY DEFINER 公共同义词禁止加回，除非 ACL 的 user oid 来源同步改成 GetSessionUserId()/GetOuterUserId() 绝对不能用 GetUserId()，否则 superuser() 直通
- discard 后 pg_branch state=discarded，不 DELETE（便于 audit，同名 recreate 时 UNIQUE 报错提示；因此 D2 检查 state = 'discarded'，不是 NOT EXISTS。

---

## §G02 S13 权限最小化（ACL Double-Defense Matrix）

> 设计目标：**「最小暴露面 + 非绕过式硬门禁」**。PostgreSQL 扩展没有行级 GRANT；分支的 OWNER（`pg_branch.owner`）只是 catalog 里的一列 oid，不能直接 `GRANT use_branch(bid) TO xxx`。所以我们把权限分两层建模：

### §G02.1 角色 & 保留名

- 内置 NOLOGIN 管理角色：**`overlay_branch_administrators`**
  - 原因：PG 禁止用户创建 `pg_` 前缀的非模板数据库角色（保留名），原 `pg_manage_overlay_branches` 直接 `CREATE ROLE` 时 `ERROR:  role name "pg_manage_overlay_branches" is reserved`，因此改名。
  - 不可改名 / 不可删除（upgrade 脚本里用此名硬编码，`DO $$ IF NOT EXISTS … COMMENT ON ROLE …$$` 保证幂等）。
  - 注释含义：「Members may create overlay branches and administer any branch. Owners of a specific branch implicitly have use/apply/discard privileges on their own branch without being added to this role.」
- 三大有效调用身份：
  1. **Superuser**（extension 安装者通常是 superuser）= 全部权限
  2. **overlay_branch_administrators 成员** = 管理员
  3. **分支 OWNER**（`pg_branch.owner = GetUserId()`）= 自己分支的 use/apply/discard

### §G02.2 两层防御矩阵

| 层 | 位置 | 能防什么 | 局限 |
|----|------|---------|------|
| SQL Layer 1 | `overlay_branch--1.0.sql` 的 `REVOKE / GRANT` + `SECURITY INVOKER` | 防止纯 SQL 绕过（比如直接调 `@extschema@.<fn>`）；PUBLIC 无权限的元数据表不能读 | **不能按 owner 做行级控制**（PG 没有 row-level GRANT，owner == 当前会话 user 这种条件必须 C 层写死） |
| C Layer 2 | `src/branch_lifecycle.c` 的 `ob_acl_check_lifecycle()` + 每个 `PG_FUNCTION_INFO_V1` wrapper **入口第一行无条件调用** | 真正的 owner / mgmt / superuser 判定；即使 SQL 层被注入（例如直接 hack @extschema@ 的 usage）也逃不过 | 不能代替 SQL 层 revoke；没有 SQL 层的话，调用者还能看见 delta 表内容 |

**SQL Layer 1 细目：**
- `REVOKE ALL ON @extschema@.pg_branch / pg_branch_delta / seq FROM PUBLIC;`
- `GRANT SELECT ON @extschema@.pg_branch TO PUBLIC;`
  - 原因：public.pg_branch VIEW 已经 GRANT SELECT 给所有人（纯 metadata，与 public.pg_branch 同结构），为对称 C 层 `ob_user_is_branch_owner()` 用 SPI 查物理表时不被 42501，显式开放 base table 的 SELECT。
- `GRANT SELECT,INSERT,UPDATE,DELETE ON @extschema@.pg_branch / pg_branch_delta TO overlay_branch_administrators;`
- `GRANT ALL ON @extschema@.pg_branch_branch_id_seq TO overlay_branch_administrators;`
- `GRANT USAGE ON SCHEMA @extschema@ TO PUBLIC;`
  - 原因：public.* synonym wrapper 声明为 `SECURITY INVOKER`（见下），不能用 SECURITY DEFINER 的切换身份绕过 pg_branch.owner 比对——必须让会话 user（也就是调用者本人）能 resolve `@extschema@.<c_fn>`。
- `public.pg_branch_delta`（VIEW）`REVOKE ALL FROM PUBLIC;`（§R22：delta 存整行业务数据，不能被 unprivileged session 看见）
- `public.pg_branch`（VIEW）保持 `GRANT SELECT TO PUBLIC`

**public.* 生命周期 synonym 设计权衡（SECURITY INVOKER vs DEFINER）：**
> 原设计误用 SECURITY DEFINER，会使 C 层 ACL 里的 `GetUserId()` = function owner（即 superuser），导致「任意调用者都被 ob_user_is_superuser_or_mgmt() 通过」的大漏洞。
> 当前实现固定为 **SECURITY INVOKER + 固定 search_path = @extschema@, pg_catalog + PUBLIC EXECUTE**，配合上面的 SCHEMA USAGE grant，使 wrapper 内解析到 `@extschema@.create_branch(...)` 的 C fn，而 C 层拿到的 GetUserId() **严格等于会话当前有效 user（SET ROLE 后的角色）**。

### §G02.3 C Layer 2：`ob_acl_check_lifecycle(op, branch_name)`

> 实现位置：`src/branch_lifecycle.c` §G02 区，4 个 helper：
> 1. `ob_lookup_mgmt_role_oid()` — AUTHNAME syscache 拿 `overlay_branch_administrators` 的 oid
> 2. `ob_user_is_superuser_or_mgmt()` — `superuser() || is_member_of_role(GetUserId(), mgmt_oid)`
> 3. `ob_user_is_branch_owner(bid, out_owner_oid)` — SPI SELECT `owner FROM @extschema@.pg_branch WHERE branch_name = $1`，返回 `owner == GetUserId()`
> 4. `ob_acl_check_lifecycle(ObLifecycleOp op, const char *branch_name)` — 统一 ereport 入口

**操作 → 授权矩阵：**
| 操作 op | 允许身份 |
|---------|---------|
| `OB_OP_CREATE_BRANCH` | superuser **OR** mgmt（普通 OWNER 不能自己建分支，避免资源 DOS；由管理员分配） |
| `OB_OP_USE_BRANCH`    | superuser **OR** mgmt **OR** `pg_branch.owner` == 当前 user；`NULL/''` 参数 → EXIT Main，对所有人开放 |
| `OB_OP_APPLY_BRANCH`  | superuser **OR** mgmt **OR** `pg_branch.owner` == 当前 user |
| `OB_OP_DISCARD_BRANCH`| superuser **OR** mgmt **OR** `pg_branch.owner` == 当前 user |

错误码统一：`ERRCODE_INSUFFICIENT_PRIVILEGE (42501)`，hint：`Must be branch owner, superuser, or member of role "overlay_branch_administrators"`。

### §G02.4 Wrapper 调用点

所有 5 个 SQL-callable C fn 在进入实际业务逻辑 **前** 先过 ACL（`PG_ARGISNULL` 语义保留在最前面）：

| 函数 | 调用行 |
|------|-------|
| `overlay_branch_create`       | `src/overlay_branch.c` create branch 顶部 |
| `overlay_branch_use`          | 同上；PG_ARGISNULL(name) 直接 return void（EXIT Main，Public 权限） |
| `overlay_branch_use_with_mode`| 同上；name NULL/empty 不检查，直接 EXIT |
| `overlay_branch_apply`        | 同上 |
| `overlay_branch_discard`      | 同上 |

### §G02.5 升级 / 回滚注意

- **角色名不可改**：任何 future `ALTER EXTENSION UPDATE` 脚本里都必须仍然叫 `overlay_branch_administrators`，否则旧实例的 GRANT 链会断。
- SECURITY DEFINER 的 public synonyms 已回滚：任何未来加回 SECURITY DEFINER 的 patch 都必须同步改 C 层 ACL 的 user oid 来源为 `GetSessionUserId() / GetOuterUserId()`，否则 GetUserId() = 函数 owner 会导致 ACL 被完全绕过。
- apply / discard 是 **idempotent**：state == applied/discarded 时重复调用 no-op，不做额外 ACL 以外的副作用。
- discard 完成后 catalog 行保留为 `state='discarded'`，**不 DELETE**（便于 audit + future `CREATE BRANCH` 同名时触发 UNIQUE 提示）；这就是 rentry.sql D2 test 用 `state = 'discarded'` 检查而非 NOT EXISTS 的原因。
