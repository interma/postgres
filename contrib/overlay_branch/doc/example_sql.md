# Overlay Branch — 用户 SQL 接口与示例

> 本文档从**用户视角**描述 Overlay Branch 扩展**当前已实现**的 SQL 接口和典型输出。
> 文中所有示例均可直接在 psql 里复现，内容就是回归测试用例所验证的真实行为。
>
> **范围**：仅支持**有主键的普通 heap 表**；默认为 Live Branch（Latest Main + Branch Delta）语义；
> 用函数调用代替自定义 SQL 语法。
>
> **V3 新增**：SNAPSHOT mode 已实装 `use_branch(name, mode => 'snapshot')`，MAIN 冻结 / Delta 始终最新，详细契约见 [multi_session_mvcc.md](./multi_session_mvcc.md) §I8.5。

> **V4 新增 (2026-10 M3)**: list_branches() 扩展到 11 列（恢复元 + drift guard）；新增 get_branch_identity() 身份与 schema drift 自检；apply_branch/discard_branch 对空分支（非当前进入）启用 empty fast-return SQL wrapper；活动分支内 DDL CREATE/ALTER/DROP 表严格 0A000 RESTRICTED（A7 guard，schema-drift guard 硬门禁）。
>
> **读路径双入口**：
> - **V1 手动 SRF**：必须显式 `FROM overlay_branch.overlay_main_plus_delta('t') AS x(...)`；
> - **V2 透明 BranchScan**：普通 `SELECT * FROM t` 自动叠加，无需改语法（需要 `shared_preload_libraries='overlay_branch'`）。

---

## 目录

- [0. 前置：安装扩展与建表](#0-前置安装扩展与建表)
- [1. 分支管理（函数式接口）](#1-分支管理函数式接口)
  - [1.1 create_branch() — 创建分支](#11-create_branch--创建分支)
  - [1.2 use_branch() — 切换当前分支 / 离开分支](#12-use_branch--切换当前分支--离开分支)
  - [1.3 current_branch() — 查询当前分支](#13-current_branch--查询当前分支)
  - [1.4 list_branches() — 列举分支](#14-list_branches--列举分支)
  - [1.5 discard_branch() — 丢弃分支](#15-discard_branch--丢弃分支)
- [2. 在分支里写数据（Write Redirect 自动生效）](#2-在分支里写数据write-redirect-自动生效)
  - [2.1 INSERT](#21-insert)
  - [2.2 UPDATE](#22-update)
  - [2.3 DELETE](#23-delete)
- [3. 在分支里读数据（V1 SRF 与 V2 BranchScan 两种方式）](#3-在分支里读数据v1-srf-与-v2-branchscan-两种方式)
  - [3.1 叠加语义总览](#31-叠加语义总览)
  - [3.2 完整叠加示例](#32-完整叠加示例)
  - [3.3 分支读不到其它分支的改动](#33-分支读不到其它分支的改动)
- [4. apply_branch() — 把分支增量合入 Main（带冲突检测）](#4-apply_branch--把分支增量合入-main带冲突检测)
  - [4.1 APPLY 成功路径](#41-apply-成功路径)
  - [4.2 APPLY 冲突路径](#42-apply-冲突路径)
- [5. Live Branch 语义：感知 Main 变化](#5-live-branch-语义感知-main-变化)
- [6. Pure Delta 场景：分支里先 INSERT 再 UPDATE/DELETE](#6-pure-delta-场景分支里先-insert-再-updatedelete)
  - [6.1 Pure Delta INSERT 再 DELETE](#61-pure-delta-insert-再-delete)
  - [6.2 Pure Delta UPDATE](#62-pure-delta-update)
  - [6.3 Pure Delta × typmod PK：NUMERIC / BPCHAR](#63-pure-delta--typmod-pknumeric--bpchar)
  - [6.4 Pure Delta × 空 MAIN heap 全链路](#64-pure-delta--空-main-heap-全链路)
- [7. RETURNING 子句](#7-returning-子句)
- [8. Data-Modifying CTE：明确 ERROR](#8-data-modifying-cte明确-error)
- [9. MVP 暂不支持的场景（明确报错） + 已实装 UPSERT](#9-mvp-暂不支持的场景明确报错--已实装-upsert)
  - [9.1 新增：UPSERT 实装示例（B2）](#91-新增upsert-实装示例b2)
- [10. V3 FR5：Snapshot vs Live 两种分支语义对比](#10-v3-fr5snapshot-vs-live-两种分支语义对比)
  - [10.1 典型场景对比](#101-典型场景对比)
  - [10.2 同一 MAIN 变化下的感知差异](#102-同一-main-变化下的感知差异)
  - [10.3 分支内自写 delta 均可见（两种语义一致）](#103-分支内自写-delta-均可见两种语义一致)
- [11. V3 FR3 / FR4：Applying 防护 + Kickout 失效 & 新 helper](#11-v3-fr3--fr4applying-防护--kickout-失效--新-helper)
  - [11.1 两个失效检查 SQL helper](#111-两个失效检查-sql-helper)
  - [11.2 跨会话 Apply/Discard 后 DML Kickout (ERRCODE 55000)](#112-跨会话-applydiscard-后-dml-kickout-errcode-55000)

- [12. V4：list_branches() 扩展到 11 列（A9 identity + drift guard）](#12-v4list_branches-扩展到-11-列a9-identity--drift-guard)
- [13. V4：get_branch_identity() — 身份与恢复绑定（A9）](#13-v4get_branch_identity--身份与恢复绑定a9)
- [14. V4：Empty-Branch Apply/Discard Fast-Return（A4 performance）](#14-v4empty-branch-applydiscard-fast-returna4-performance)
- [15. V4：活动分支内 DDL RESTRICTED（A7 schema-drift guard）](#15-v4活动分支内-ddl-restricteda7-schema-drift-guard)
  - [11.3 FR3 Applying 状态期间 DML 阻塞](#113-fr3-applying-状态期间-dml-阻塞)
  - [11.4 DO block 包裹重试模板](#114-do-block-包裹重试模板)

---

## 0. 前置：安装扩展与建表

```sql
-- 1. 必须把 overlay_branch 放进 shared_preload_libraries（V2 BranchScan 钩子用）
--    在 postgresql.conf 里写：
--      shared_preload_libraries = 'overlay_branch'
--    然后重启 postmaster。没放也能装扩展，但 V2 透明 SELECT 不会生效。

-- 2. 安装扩展（会创建 pg_branch、pg_branch_delta 两张系统表 + 一组 public synonym）
CREATE EXTENSION overlay_branch;
CREATE EXTENSION
```

```sql
-- 准备一张有主键的普通表（MVP 硬性要求：所有分支里 DML 的表必须有 PK）
CREATE TABLE products (
    id    integer PRIMARY KEY,
    name  text    NOT NULL,
    price integer NOT NULL
);
CREATE TABLE

INSERT INTO products VALUES
  (1, 'Apple',  10),
  (2, 'Banana',  5),
  (3, 'Cherry', 20);
INSERT 0 3
```

---

## 1. 分支管理（函数式接口）

> **注意**：MVP 阶段没有自定义 SQL 语法，所有操作都通过 `public.*` synonym 函数调用。
> V2 / V3 可能补 `CREATE BRANCH` / `USE BRANCH` 语法糖。
>
> 以下所有 `SELECT create_branch('x')`、`SELECT use_branch('x')` 均可省略 schema，
> 因为 `overlay_branch--1.0.sql` 末尾已经挂了 `public.*` synonym。

### 1.1 create_branch() — 创建分支

```sql
-- 最简语法（默认 mode='live'，也是 MVP 唯一支持的模式）
SELECT create_branch('agent_workspace');
 create_branch
---------------
             1
(1 row)
```

> 说明：`create_branch()` 返回内部递增的 `branch_id`。
> 只会在 `pg_branch` 里插入一行元数据，**不会拷贝任何数据**，成本 ≈ 一次单行 INSERT。
> `create_branch()` 时不区分 mode（catalog 中 pg_branch.mode 恒为 `'live'`）；
> **分支隔离语义在 `use_branch(name, mode => …)` 进入时按 session 决定**（见 §1.2）。

失败示例：

```sql
-- 重名会报错
SELECT create_branch('agent_workspace');
ERROR:  branch "agent_workspace" already exists
```

### 1.2 use_branch() — 切换当前分支 / 离开分支

```sql
-- 进入一个分支（会话级，写 GUC overlay_branch.current）
SELECT use_branch('agent_workspace');
 use_branch
------------

(1 row)

-- 等价的 GUC 写法（效果完全相同；两者二选一，推荐用函数）
SET overlay_branch.current = 'agent_workspace';
SET
```

```sql
-- === V3 新增 FR5：2-param use_branch(name, mode) ===
--   mode 可选值：'live' （默认，等价于不传）
--               'snapshot' （V3 实装；MAIN 表读按进入时快照冻结）
-- 两个入口均挂了 public synonym，可不带 schema。
SELECT use_branch('agent_workspace', mode => 'snapshot');
 use_branch
------------

(1 row)

-- 退出 snapshot 模式：重新 use live 或直接离开 branch
SELECT use_branch('agent_workspace', mode => 'live');   -- 切换回 live
SELECT use_branch(NULL);                                 -- 彻底回 Main
```

```sql
-- 离开当前分支，回到 Main（直接读写主表）
--   NULL = 让 overlay_branch.current = ''，后续 current_branch() 返回空
SELECT use_branch(NULL);
 use_branch
------------

(1 row)

-- 等价写法
RESET overlay_branch.current;
RESET
```

失败示例：

```sql
SELECT use_branch('does_not_exist');
ERROR:  branch "does_not_exist" does not exist
```

### 1.3 current_branch() — 查询当前分支

```sql
-- 在 Main 里：返回空 name
SELECT current_branch();
 current_branch
----------------

(1 row)

-- 进入分支后
SELECT use_branch('agent_workspace');
SELECT current_branch();
 current_branch
----------------
 agent_workspace
(1 row)
```


### 1.4 list_branches() — 列举分支

纯 SQL 实现的表函数，可 JOIN、可过滤。**V4 返回 11 列（原 7 列 + A9 identity 4 恢复元列 + drift 2 自检列），顺序固定**：

```sql
\x
Expanded display is on.
SELECT * FROM list_branches() WHERE branch_name = 'agent_workspace';
-[ RECORD 1 ]-------------------------+----------------------------------
branch_id                             | 1
branch_name                           | agent_workspace
owner                                 | 10
created_at                            | 2026-09-10 10:00:00+08
mode                                  | live
state                                 | active
delta_count                           | 0
/* -- V4 (A9 identity) 新增 6 columns below -- */
schema_hash                           | \x2d6f...16bytea MD5 hex           -- MAIN 受管表 set-of(relid+schema+table+attnum+attname+typid+typmod+notnull+generated+pk_ord) 的 MD5
creation_snapshot_xmin                | 789                                   -- create_branch 时 MAIN 事务 snapshot xmin，用于 restore 时校验
tablespace_list                       | 'pg_default:1663,my_tblspc:16789'     -- 受管表 tablespace OID:name CSV（restore / 打包恢复时重建）
col_signature                         | \x8a3b...16bytea                     -- 与 C UPDATE block 同算法的 8-tuple 列签名 (PK attnum first)；比 schema_hash 更鲁棒 drift detection
registry_user_tables                  | 12                                    -- pg_branch_registry 中登记的受管用户表数量（与实际 MAIN 受管表数量一致性检查）
registry_schema_current_match         | t                                     -- 纯 SQL drift guard：col_signature(current MAIN) = pg_branch.col_signature？t=未漂移；f=在其它分支 apply 过 schema 变更 / MAIN 用户手动改表
\x
Expanded display is off.
```

> **V4 兼容说明**：原 7 列（branch_id/branch_name/owner/created_at/mode/state/delta_count）**位置未变**，旧代码 `SELECT branch_id, state, delta_count FROM list_branches()` 无需任何改动。
> 新增 4 列 + drift 2 列在右侧，老 `SELECT *` 会看到更多字段；若需 strict 7 columns，请显式列名。

11 列字段一览：

| 列名 | 类型 | 含义 |
|------|------|------|
| `branch_id` | integer | 内部递增 id |
| `branch_name` | name | 分支名（唯一）|
| `owner` | oid | 创建者 |
| `created_at` | timestamptz | 创建时间 |
| `mode` | text | catalog 默认 `'live'`；若本 backend 正在 snapshot 模式使用，则显示 `'snapshot'`（COALESCE cached_mode）|
| `state` | text | `active` / `applied` / `discarded` / `applying`（瞬态，apply 期间）|
| `delta_count` | bigint | 当前 `pg_branch_delta` 中该分支的增量行数 |
| `schema_hash` | bytea | A9：create_branch 时 MAIN 所有受管表 schema 指纹 MD5（16 bytes hex decode）；用于 restore 时 "这个 branch 备份是在哪个 MAIN schema 版本下产生的" |
| `creation_snapshot_xmin` | xid | A9：create_branch 事务 snapshot xmin；restore 前可与当前 MAIN 对照是否在安全范围 |
| `tablespace_list` | text | A9：受管表 tablespace `name:oid` CSV（含 PRIMARY / DEFAULT / UNIQUE 索引 tablespace），打包恢复时按此清单重建 |
| `col_signature` | bytea | A9：与 C apply/discard 前置 UPDATE block **同算法** 的 8-tuple 列签名（pk_ord 非零列优先排序）；是 drift detection 权威依据（见 §13 get_branch_identity） |
| `registry_user_tables` | bigint | A9：pg_branch_registry 中受管表登记数量（≠ 0 说明创建时 MAIN 有受管表） |
| `registry_schema_current_match` | bool | A9：**纯 SQL drift guard**（planner 阶段不进入 C UPDATE/WR block）→ 若 false 说明 MAIN 当前 schema 已漂移，apply 会触发 FR4 kickout，建议先 restore / re-baseline |

### 1.5 discard_branch() — 丢弃分支

```sql
-- 丢弃一个不再需要的分支：
--   (a) pg_branch.state 改为 'discarded'；(b) 级联删除 pg_branch_delta 中该 branch 的所有行
--   Main 表数据完全不动
SELECT discard_branch('agent_workspace');
 discard_branch
----------------

(1 row)

-- 丢弃后：state = discarded，delta_count = 0（delta 已被清理）
SELECT branch_name, state, delta_count FROM list_branches()
 WHERE branch_name = 'agent_workspace';
  branch_name   |   state   | delta_count
----------------+-----------+-------------
 agent_workspace | discarded |           0
(1 row)
```

如果丢弃的是**当前正在使用**的分支，会自动回到 Main：

```sql
SELECT use_branch('agent_workspace');
SELECT discard_branch('agent_workspace');
NOTICE:  discarding current branch "agent_workspace", reverting to Main
 discard_branch
----------------

(1 row)

SELECT current_branch();
 current_branch
----------------

(1 row)
```

对已 `discarded` 或 `applied` 的分支再次 discard 会报错：

```sql
SELECT discard_branch('agent_workspace');
ERROR:  overlay_branch: discard_branch: branch "agent_workspace" is not active
             (already discarded or applied); discard cannot be called twice
             on the same branch
```

---

## 2. 在分支里写数据（Write Redirect 自动生效）

**进入分支后，所有普通的 INSERT / UPDATE / DELETE 不会写主表，只会写 `pg_branch_delta`。**
用户不需要改 DML 语法。

```sql
SELECT use_branch('agent_workspace');
```

### 2.1 INSERT

```sql
INSERT INTO products VALUES (4, 'Date', 30);
INSERT 0 1
```

> 实际写入 `pg_branch_delta`：
> `(branch_id=1, relid=<products OID>, key='["4"]', op='I', old_version=NULL, tuple_data=<序列化后的 (4,Date,30)>)`

### 2.2 UPDATE

```sql
UPDATE products SET price = 100 WHERE id = 1;
UPDATE 1
```

> 实际写入 `pg_branch_delta`（同一 `(branch, rel, key)` 冲突自动 UPSERT，保证每 tuple 只有一条最新 delta）：
> `op='U', old_version=<主表 id=1 行的 version token>, tuple_data=<(1,Apple,100)>`

### 2.3 DELETE

```sql
DELETE FROM products WHERE id = 2;
DELETE 1
```

> 实际写入 `pg_branch_delta`：
> `op='D', old_version=<主表 id=2 行的 version token>, tuple_data=NULL`（墓碑）

---

## 3. 在分支里读数据（V1 SRF 与 V2 BranchScan 两种方式）

### 3.1 叠加语义总览

两种读路径的语义**位精确等价**（回归测试 `BS_EQUIV_SRF_BRANCHSCAN` 即断言此等价性）：

| Delta 操作 | Main 有 | Main 无 | BranchScan / SRF 输出 |
|-----------|---------|---------|----------------------|
| `I`(nsert) | — | ✅ | 输出 Delta 新 tuple |
| `U`(pdate) | ✅ | — | **输出 Delta 的 tuple**（覆盖 Main） |
| `D`(elete) | ✅ | — | **屏蔽** Main 的 tuple（输出中看不到） |
| 无 Delta  | ✅ | — | 原样输出 Main tuple |
| 无 Delta  | — | — | 看不到 |

两条入口：

```sql
-- === V2 透明 BranchScan（推荐：零语法改动，需 shared_preload_libraries='overlay_branch'）===
SELECT * FROM products ORDER BY id;

-- === V1 手动 SRF（兜底入口；必须显式给列类型列表，因为 RETURNS SETOF record）===
SELECT * FROM overlay_branch.overlay_main_plus_delta('products')
       AS x(id integer, name text, price integer)
 ORDER BY id;
```

### 3.2 完整叠加示例

沿用 §2 写过的 3 条 DML：id=1 UPDATE、id=2 DELETE、id=4 INSERT。

**Main 未改动**：

```sql
SELECT use_branch(NULL);
SELECT * FROM products ORDER BY id;
 id |  name  | price
----+--------+-------
  1 | Apple  |    10
  2 | Banana |     5
  3 | Cherry |    20
(3 rows)
```

**在分支里看（两种方式结果相同）**：

```sql
SELECT use_branch('agent_workspace');
SELECT * FROM products ORDER BY id;   -- V2 透明（推荐）
 id |  name  | price
----+--------+-------
  1 | Apple  |   100
  3 | Cherry |    20
  4 | Date   |    30
(3 rows)
```

叠加过程：

```
Main(products)       Delta(agent_workspace)       输出
=============        ====================       ========
id=1 Apple  10 ──U── id=1 → U(Apple,100)    ──►  id=1 Apple 100
id=2 Banana  5 ──D── id=2 → D(tombstone)    ──┐
                                                ├─►  被屏蔽
id=3 Cherry 20      (无 Delta for id=3)     ──►  id=3 Cherry 20
                    id=4 → I(Date,30)       ──►  id=4 Date 30
```

### 3.3 分支读不到其它分支的改动

```sql
SELECT create_branch('branch_B');       -- 新分支，无任何改动
SELECT use_branch('branch_B');

-- branch_B 没改过任何东西，看到的就是 Main 原样
SELECT * FROM products ORDER BY id;
 id |  name  | price
----+--------+-------
  1 | Apple  |    10
  2 | Banana |     5
  3 | Cherry |    20
(3 rows)

-- agent_workspace 改的东西，branch_B 完全不可见（分支隔离）
SELECT use_branch('agent_workspace');
SELECT * FROM products WHERE id = 1;
 id | name  | price
----+-------+-------
  1 | Apple |   100
(1 row)
```

---

## 4. apply_branch() — 把分支增量合入 Main（带冲突检测）

语义：**把分支当前的 delta 当作一次新的 Main 事务，逐条做乐观 old_version 冲突验证再写入 Main**。
冲突直接报 ERROR，不做 ours/theirs 自动 merge。

### 4.1 APPLY 成功路径

准备：agent_workspace 的改动仍为 §2（id=1 U→100，id=2 D，id=4 I→Date/30）。
**Main 没有动过这些行**，所以版本号没变，无冲突。

```sql
SELECT use_branch(NULL);   -- 可以在任何会话执行 apply，都会写 Main
SELECT * FROM products ORDER BY id;   -- apply 前 Main 基线
 id |  name  | price
----+--------+-------
  1 | Apple  |    10
  2 | Banana |     5
  3 | Cherry |    20
(3 rows)

SELECT apply_branch('agent_workspace');
NOTICE:  overlay_branch: APPLY BRANCH 'agent_workspace' completed
             (state=applied, delta rows deleted)
 apply_branch
--------------

(1 row)
```

> **V3 FR4 行为变更（自 apply / 跨会话 apply kickout）**：
> - **自 apply**：如果当前 session 正在 use_branch(X)，然后自己执行 `apply_branch(X)` → apply 完成后本 session 会自动回到 Main（`current_branch()` 返回空），无需再 `use_branch(NULL)`。
> - **跨会话 apply**：如果 session A use_branch(X) 进行中，session B 执行了 `apply_branch(X)` 或 `discard_branch(X)` → 下一次 session A 在 X 里运行 DML / 或 DQL 达到 throttling 阈值时，会**立即报 `ERRCODE 55000` 并踢出 branch**，防止在 state=applied/discarded 下继续产生漂移写入。跨会话会首先通过 NOTIFY 通道；短连接（如 L1 isolation harness）下走纯 SPI counter fallback 兜底。详见 `doc/multi_session_mvcc.md` §I8.3 / I8.6。
>
> - **自 discard**：类似自 apply；在 use_branch(X) 中 discard 后立即回 Main，已有 NOTICE。
>
> 建议在业务长循环中（如 Agent 多次 WR）每条语句外 `DO block BEGIN ... EXCEPTION WHEN 55000 THEN ... END` 包裹以检测分支失效并 retry（见 §11.2 示例）。

Apply 后，Main 被真实改写，**branch 的 delta 被清空**（不保留审计副本，因为 MVP），
`pg_branch.state` 变为 `applied`：

```sql
SELECT * FROM products ORDER BY id;
 id |  name  | price
----+--------+-------
  1 | Apple  |   100     -- 改了
  3 | Cherry |    20
  4 | Date   |    30     -- 新增（id=2 被删了）
(3 rows)

SELECT branch_name, state, delta_count FROM list_branches()
 WHERE branch_name = 'agent_workspace';
  branch_name   |  state  | delta_count
----------------+---------+-------------
 agent_workspace | applied |           0   -- delta 行已被清理，count=0
(1 row)
```

### 4.2 APPLY 冲突路径

场景：创建分支后，**Main 自己也改了 id=1**，然后分支 apply ——
old_version token 和当前 Main 行不匹配 → 检测到冲突。

```sql
-- ============== 准备演示环境（重新建表 + 建分支 alice）
TRUNCATE products;
INSERT INTO products VALUES (1,'Apple',10),(2,'Banana',5),(3,'Cherry',20);

SELECT create_branch('alice');
SELECT use_branch('alice');
UPDATE products SET price = 999 WHERE id = 1;   -- alice 分支把 id=1 → 999
UPDATE 1

-- ========= 与此同时（Main 并发改同一行）
SELECT use_branch(NULL);
UPDATE products SET price = 42 WHERE id = 1;    -- Main 先提交了 42
UPDATE 1

-- ========= 现在让 alice apply：冲突！
SELECT apply_branch('alice');
ERROR:  overlay_branch: apply conflict on relation products(primary key=["1"])
DETAIL:  branch's base old_version = '...'   -- 写 delta 时 Main 上 id=1 的 version token
         main's current old_version = '...'  -- 现在 Main 上 id=1 的 version token
         branch intends to write: (1,'Apple',999)
HINT:  Row was modified in Main after branch wrote it.
       Re-read current state and retry in a fresh branch.
```

Overlay Branch 不会自作主张做 ours/theirs merge。错误信息里给了业务层（Agent / 应用）
足够的信息：行定位、两个版本 token、分支拟写入的值，让调用方自己决定怎么办。

---

## 5. Live Branch 语义：感知 Main 变化

MVP 默认的 `mode='live'` 不是严格的数据库 fork，语义是：

> **Latest Main + Branch Delta 的叠加**

即：**没被分支改过的行，永远跟随 Main 最新提交的版本走**；
分支改过的行，一直看到分支自己覆盖后的版本。

```sql
-- Main 里 id=3 没被任何分支动过
SELECT use_branch(NULL);
UPDATE products SET price = 9999 WHERE id = 3;   -- Main 涨价
UPDATE 1

-- 进入一个没动过 id=3 的分支（例如 branch_B）
SELECT use_branch('branch_B');
SELECT * FROM products WHERE id = 3;
 id |  name  | price
----+--------+------
  3 | Cherry | 9999   -- 看到了 Main 最新的涨价
(1 row)
```

这更贴近 AI Agent / Workspace 场景：Agent 保留自己的草稿修改，
同时**感知世界其它部分的变化**（库存、别人的提交、价格更新…）。

---

## 6. Pure Delta 场景：分支里先 INSERT 再 UPDATE/DELETE

> **架构说明（用户可见后果）**：Write Redirect 的 CMD_UPDATE/DELETE 主循环**先扫 MAIN heap（物理行 ctid）**，然后再跑一次
> **pure-delta 8-phase fallback 循环**（针对 "分支里自己 INSERT 出来、MAIN 上从来没有过" 的纯 delta 行）：
> 重跑一次 WHERE 条件（兼容 IndexScan / BitmapHeapScan / IndexOnlyScan 3 种下推 qual 存储位置），命中后以 latest-wins UPSERT 写回 delta 表。
> Pure-delta 路径详情见 [multi_session_mvcc.md §D.11-D.12](./multi_session_mvcc.md) 及 AGENTS.md §4.R19。

### 6.1 Pure Delta INSERT 再 DELETE

**单 PK 等值**：分支里先 INSERT 一行 MAIN 没有的 id，再 WHERE PK 等值 DELETE — 真的消失：

```sql
SELECT use_branch('agent_workspace');

INSERT INTO products VALUES (99, 'PureInserted', 777);
INSERT 0 1
SELECT count(*) FROM products WHERE id = 99;   -- 1 (可见)
 count
-------
     1

DELETE FROM products WHERE id = 99;            -- pure delta DELETE
DELETE 1
SELECT count(*) FROM products WHERE id = 99;   -- 0 (真的没了)
 count
-------
     0
```

**复合 WHERE（BitmapHeapScan 典型：AND + IN 列表）**：

```sql
CREATE TABLE bs_pd_multi (
    grp   text NOT NULL,
    score int4 NOT NULL,
    id    int4 PRIMARY KEY
);
INSERT INTO bs_pd_multi VALUES ('main', 0, 1);    -- 唯一 MAIN 行

SELECT use_branch('agent_workspace');
INSERT INTO bs_pd_multi VALUES
  ('C', 333, 30), ('C', 333, 31), ('C', 999, 32);  -- 3 条 pure delta

DELETE FROM bs_pd_multi WHERE grp = 'C' AND score = 333;
DELETE 2
SELECT id, grp, score FROM bs_pd_multi WHERE grp = 'C' ORDER BY id;
 id | grp | score
----+-----+-------
 32 | C   |   999           -- 30/31 满足条件被删，32 分数不同幸存
(1 row)
```

### 6.2 Pure Delta UPDATE

分支里自己 INSERT 出来的纯 delta 行（MAIN 上没有对应物理行）再 UPDATE，走 **pure-delta 单独 8-phase 主循环**：
重跑一次 WHERE 条件（兼容 IndexScan / BitmapHeapScan / IndexOnlyScan 三种下推 qual 存储位置），对命中行做 SET 列合并后，以 UPSERT 最新-wins 的方式写回 `pg_branch_delta`（`op='U'`，原 `tuple_data` 的列被 SET 覆盖）：

```sql
INSERT INTO products VALUES (999, 'WillBeUpdated', 1);
SELECT id, name, price FROM products WHERE id = 999;
 id  |      name       | price
-----+-----------------+-------
 999 | WillBeUpdated   |     1

UPDATE products SET price = 999999, name = 'UpdatedOK' WHERE id = 999;
UPDATE 1
SELECT id, name, price FROM products WHERE id = 999;
 id  |    name     |  price
-----+-------------+---------
 999 | UpdatedOK   | 999999     ← 实装：pure-origin UPDATE 生效（SET 两列都写回 delta op='U'）
```

后续 `apply_branch()` 时 MAIN 上 id=999 不存在，会走 "UPDATE pure-origin MAIN-miss → `insert_pass` fallback"
（因为 delta 行 `_base_ctid` / `_base_xmin` 为 NULL），最终以 `op='U'` 的语义 INSERT 进 MAIN 表。

### 6.3 Pure Delta × typmod PK：NUMERIC / BPCHAR

有 typmod 的列当 PK 时，**主键序列化必须走 `format_type_with_typmod()` + WR/both-side rtrim**
（BPCHAR 尾部空格语义）。否则 lookup 100% miss：

```sql
-- NUMERIC(10,2) PK
CREATE TABLE bs_pd_numeric(price NUMERIC(10,2) PRIMARY KEY, tag TEXT);
SELECT use_branch('agent_workspace');
INSERT INTO bs_pd_numeric VALUES (30.33, 'pd-n1');
INSERT INTO bs_pd_numeric VALUES (40.44, 'pd-n2');
DELETE FROM bs_pd_numeric WHERE price = 30.33;
SELECT string_agg(tag, ',' ORDER BY tag) FROM bs_pd_numeric;
 string_agg
------------
 pd-n2

-- BPCHAR(6) PK（等值比较忽略尾部空格；序列化两端均 rtrim）
CREATE TABLE bs_pd_bpchar(code BPCHAR(6) PRIMARY KEY, tag TEXT);
SELECT use_branch('agent_workspace');
INSERT INTO bs_pd_bpchar VALUES ('PD001', 'A'), ('PD002', 'B'), ('PD003', 'C');
DELETE FROM bs_pd_bpchar WHERE code = 'PD002';    -- ='PD002' 和 ='PD002 ' 都能命中
SELECT code::text, tag FROM bs_pd_bpchar ORDER BY code;
 code  | tag
-------+-----
 PD001 | A
 PD003 | C
```

### 6.4 Pure Delta × 空 MAIN heap 全链路

MAIN 一行都没有（CREATE TABLE 后未 INSERT），100% 数据都来自 pure delta INSERT 的极端：

```sql
CREATE TABLE bs_empty_pk(id INT4 PRIMARY KEY, tag TEXT);   -- MAIN 0 行
SELECT use_branch('agent_workspace');
INSERT INTO bs_empty_pk VALUES (1,'one'),(2,'two'),(3,'three'),(4,'four');  -- 4 pure

DELETE FROM bs_empty_pk WHERE id IN (2,4);   -- IN 列表走 BitmapHeapScan
DELETE 2
SELECT count(*) FROM bs_empty_pk;
 count
-------
     2

DELETE FROM bs_empty_pk WHERE id = 3 RETURNING id, tag;
 id |  tag
----+-------
  3 | three
(1 row)

SELECT apply_branch('agent_workspace');   -- MAIN 本 0 行 → apply 后剩 id=1 one
 apply_branch
--------------

SELECT * FROM bs_empty_pk ORDER BY id;   -- RESET 回 MAIN 也能看到 id=1
 id | tag
----+-----
  1 | one
```

---

## 7. RETURNING 子句

`INSERT/UPDATE/DELETE RETURNING` 在 WR 路径全支持，投影直接从 delta 写出后的 slot 回发：

```sql
SELECT use_branch('agent_workspace');

INSERT INTO products VALUES (10, 'ten', 10.00::INT4) RETURNING id, name, price;
 id | name | price
----+------+-------
 10 | ten  |    10
(1 row)

UPDATE products SET price = 100 WHERE id = 1 RETURNING id, price;
 id | price
----+-------
  1 |   100
(1 row)

DELETE FROM products WHERE id = 2 RETURNING *;
 id |  name  | price
----+--------+-------
  2 | Banana |     5
(1 row)
```

Pure-delta DELETE（§6.1 的 id=99）同样支持 RETURNING：
```sql
DELETE FROM bs_pd_multi WHERE id = 32 RETURNING id, grp, score;
 id | grp | score
----+-----+-------
 32 | C   |   999
(1 row)
```

## 8. Data-Modifying CTE：明确 ERROR

MVP 下 **CTE 里嵌入 DML 再外层 SELECT**（`WITH x AS (UPDATE ... RETURNING) SELECT * FROM x`）是不允许的。
因为这种写法会让 WR 和外层 Planner 产生子计划依赖，若不严加拦截会导致
**静默 MAIN heap 污染**。入口检测 `PlannedStmt.hasModifyingCTE`，命中直接 ereport ERROR：

```sql
WITH u AS (UPDATE products SET price=price+1 RETURNING id, price)
SELECT * FROM u;
ERROR:  overlay_branch: Data-Modifying CTE (WITH ... UPDATE/INSERT/DELETE ... RETURNING)
        is not supported inside an active branch. Use top-level DML with RETURNING instead.
```

替代方案（MVP 推荐）：直接 top-level DML 加 `RETURNING`（见 §7）。

---

## 9. MVP 暂不支持的场景（明确报错）

以下场景在 MVP 会**明确拒绝**（不会静默穿透到 Main，也不给出错误结果）。
具体错误前缀统一为 `overlay_branch: cannot <action> inside active branch "..."` + 触发哪一级 guard：

| 场景 | 行为 | 典型错误（节选）|
|------|------|----------------|
| 对**无主键**表写 DML | 立即拒绝，不写任何东西 | `overlay_serialize_pk requires a primary key on ...`（写 delta 阶段）或 Write Redirect guard 拦截 |
| 在分支里执行大部分 DDL（CREATE TABLE / ALTER TABLE / DROP TABLE / DROP INDEX / CREATE TRIGGER / ALTER TYPE 等）| **A7 DDL RESTRICTED MVP 0A000**：create_branch 时登记 pg_branch_registry schema epoch；后续进入 active branch 时 ProcessUtility hook 直接拦截 + 给 HINT：如何退回 MAIN 再 DDL；再进入原分支需要重新 create（避免 MAIN schema 变更后 delta join 列类型错位静默错）| `overlay_branch: cannot CREATE TABLE inside active branch "...": CREATE TABLE on MAIN user objects during an active branch would modify underlying schema/tables in place and break MAIN/delta join semantics after apply. (§A7 DDL RESTRICTED 0A000 — schema-drift guard)`（真实 L3 view_unify & rentry 回归） |
| `INSERT ... ON CONFLICT` (UPSERT) | **已实装（B2）**：Plain INSERT ON CONFLICT DO NOTHING / DO UPDATE 都走 pure-delta 4-way promo 调度；ON CONFLICT 23505 冲突检测在 MAIN/delta 双侧 2-phase 进行；唯一限制：R21 写前 NOT NULL + PK UNIQUE 双侧预检仅对 plain INSERT（ONCONFLICT_NONE）生效（UPSERT 走 Phase I/II 更丰富的 2-phase 检测）；见下面 §9.1 | N/A（实装，正常支持）|
| **分区表**（根/叶）| 6 层 guard 的 G3 级提前拒绝（relkind / partitioned） | `overlay_branch does not support partitioned tables`（guard 通用提示）|
| **FK 级联写**（trigger 触发的子表级联 UPDATE/DELETE）| G4 级非 internal trigger 拦截或 G5 `pg_constraint` FK 检测 | `cannot modify via FK-triggered write`（guard 通用提示）|
| 直接对 `pg_branch` / `pg_branch_delta` 用户写 | `security_barrier` view 无 INSERT/UPDATE/DELETE rule → 普通报错 | `cannot insert into view "pg_branch"`（PG 原生视图错误）|
| **在 `create_branch()` 时指定 `isolation = 'snapshot'`** | `create_branch()` 不接受 isolation 参数（catalog 中 pg_branch.mode 恒为 live）；要开启 snapshot 语义，请在进入分支时显式 `use_branch(name, mode => 'snapshot')`（见 §1.2）| 若硬传 snapshot 到 mode 字段不生效（建议走 use_branch 的 FR5 入口）|
| 对 VIEW / MATVIEW / FOREIGN TABLE 写 DML | 在 guard 的 G2/G3 级就拦截，不支持透明叠加读 | V2 BranchScan planner hook 直接 skip，走原路径；DML 则在 WR guard 报错 |
| **COPY FROM / MERGE / Data-Modifying CTE** | R05/R06/R16 入口拦截（MVP 范围硬性禁止）| 0A000 feature_not_supported："COPY FROM on overlay-managed tables is not supported in MVP"（防止写穿 MAIN）|
| **UPDATE t SET pk_col = ...**（修改主键列）| R03 拒绝：MAIN path + pure-delta path 两路都检查 targetList 是否命中 PK attnum → 0A000 | "Updating the PRIMARY KEY column of an overlay-managed table is not supported" |
| **非分支 OWNER / 非 overlay_branch_administrators 成员 / 非 Superuser 调用 use / apply / discard / create**（§G02 ACL 硬门禁）| 所有 5 个 wrapper 在 C 层入口第一行调用 `ob_acl_check_lifecycle`，调用身份必须是 (branch owner) ∪ (mgmt role 成员) ∪ (superuser)，否则 42501 | `42501 insufficient_privilege: Must be branch owner, superuser, or member of role "overlay_branch_administrators"`（CREATE_BRANCH 不接受 OWNER 身份，必须 mgmt 或 superuser）|

这些限制在后续版本逐步解除（分类二 S13 G02 ACL / 分区支持 / FK 级联等见 review_260926 §Cat2）。

### 9.1 新增：UPSERT 实装示例（B2）

`INSERT ... ON CONFLICT DO NOTHING / DO UPDATE`（针对 PK 的冲突）现已在分支内正常工作：

```sql
SELECT create_branch('br_upsert_demo');
SELECT use_branch('br_upsert_demo');

-- MAIN 已有 id=1 (Apple,10)、id=2 (Banana,5)、id=3 (Cherry,20)

-- 1. ON CONFLICT DO NOTHING：撞 PK → NOP，不报错
INSERT INTO products VALUES (1, 'NewApple', 999) ON CONFLICT (id) DO NOTHING;
INSERT 0 0
SELECT * FROM products WHERE id = 1;
 id | name  | price
----+-------+-------
  1 | Apple |    10   ← 仍为主表 / 分支 delta 的最新（live）覆盖值，不被 DO NOTHING 覆盖

-- 2. ON CONFLICT DO UPDATE：撞 PK → 以 EXCLUDED 新值 SET 更新
INSERT INTO products VALUES (3, 'Cherry_v2', 222)
  ON CONFLICT (id) DO UPDATE SET name  = EXCLUDED.name,
                                  price = EXCLUDED.price;
INSERT 0 1
SELECT * FROM products WHERE id = 3;
 id |   name    | price
----+-----------+-------
  3 | Cherry_v2 |   222   ← 分支 delta op='U' 覆盖 MAIN 原 Cherry/20

-- 3. 撞分支自己的 pure-origin INSERT（MAIN 上没有 id=100）
INSERT INTO products VALUES (100, 'First', 1) ON CONFLICT (id) DO NOTHING;
INSERT INTO products VALUES (100, 'Dupe',  2) ON CONFLICT (id) DO NOTHING;  -- 撞 1st delta I
INSERT 0 0
SELECT * FROM products WHERE id = 100;
 id  | name  | price
-----+-------+-------
 100 | First |     1  -- latest-wins：First 幸存
```

Apply 后 MAIN 上的最终值与分支 LIVE 视图严格对齐（UPSERT 产生的 delta op 类型按 MAIN-hit × pure-hit × oc_action × WHERE-cond 4 维表归一化成 I/U/D，然后走 D-U-I 3-pass merge）。

---

## 10. V3 FR5：Snapshot vs Live 两种分支语义对比

> 从 V3 T5 开始，`use_branch()` 支持两种 session-local 隔离模式；
> 同一 branch 可以在不同 session 里分别以 snapshot 或 live 方式进入（互不干扰）。

### 10.1 典型场景对比

| 维度 | mode='live' （进入默认） | mode='snapshot' （V3 实装 FR5） |
|------|---|---|
| **MAIN 表的旧行**（没被自己写过） | 始终跟随 Main 的最新提交 → 看到别人 commit 的修改 | **冻结**在 `use_branch(..., 'snapshot')` 那一刻的事务快照 → 别人后续 commit 看不到 |
| **分支自己写的 WR delta 行** | 始终可见（与 MAIN 叠加） | 始终可见（与 MAIN 叠加） |
| **同一 branch 的其他 live session 新写的 WR delta** | 始终可见（LatestSnapshot 读 delta） | **始终可见**（delta 表 append-only；因为 delta 表是 overlay_branch 内部 MVCC，不因 snapshot 冻结） |
| **典型场景** | AI Agent 协作工作区；保留自己改的同时看库存/别人的新提交；写冲突留在 apply 时一次性解决 | 报表 / 离线分析 / 长查询；MAIN 数据在查询期间不漂移；同一批分析结果可重复读 |
| **怎么进入** | `use_branch('b')` 或 `use_branch('b','live')` | `use_branch('b','snapshot')` |
| **怎么退出** | `use_branch(NULL)` / `RESET overlay_branch.current` / 切回 live：`use_branch('b','live')` | 相同；也可以切换成 live 再继续工作 |

### 10.2 同一 MAIN 变化下的感知差异

```sql
-- ========== 准备 ==========
SELECT use_branch(NULL);
TRUNCATE products;
INSERT INTO products VALUES (1,'A',10),(2,'B',5),(3,'C',20);
SELECT create_branch('b_snap');

-- ========== s1: snapshot 会话 ==========   (相当于 psql session 1)
SELECT use_branch('b_snap', 'snapshot');
SELECT * FROM products ORDER BY id;
-- 看到 MAIN baseline: A(10), B(5), C(20)   （id=4 还未插入）

-- ========== s2: live 会话在 MAIN 写入 ==========   (psql session 2)
SELECT use_branch(NULL);
INSERT INTO products VALUES (4,'D',30);     -- MAIN 新增 id=4
UPDATE products SET price=9999 WHERE id=3;  -- MAIN id=3 涨价
COMMIT;

-- ========== 回到 s1: snapshot 会话 ==========
SELECT * FROM products ORDER BY id;
-- id | name | price      （MAIN 冻结！）
--  1 |  A   | 10
--  2 |  B   |  5         看不到 MAIN id=4/D；id=3 仍为 20
--  3 |  C   | 20         （snapshot xmin 取在 use_branch 时刻）
--  (3 rows)

-- ========== s3: 同一 branch live 方式进入 ==========
SELECT use_branch(NULL);      -- 先回 Main 换个"视角"
SELECT use_branch('b_snap', 'live');
SELECT * FROM products ORDER BY id;
-- id | name | price
--  1 |  A   |   10
--  2 |  B   |    5         LIVE 模式：id=4 看到了！id=3 涨价到 9999 了！
--  3 |  C   | 9999
--  4 |  D   |   30
--  (4 rows)
```

### 10.3 分支内自写 delta 均可见（两种语义一致）

不管 snapshot 还是 live，自己在分支里写的 delta 永远会叠加显示：

```sql
SELECT use_branch('b_snap', 'snapshot');
UPDATE products SET price=7 WHERE id=2;     -- delta U(id=2)
INSERT INTO products VALUES (99, 'Mine', 1);-- delta I(id=99)
SELECT * FROM products ORDER BY id;
-- 1|A|10       (MAIN frozen)
-- 2|B|7        (自己的 delta U)
-- 3|C|20       (MAIN frozen)
-- 99|Mine|1    (自己的 delta I)
```

---

## 11. V3 FR3 / FR4：Applying 防护 + Kickout 失效 & 新 helper

V3 引入两组跨会话正确性保护，对用户行为**可见**（会产生新的 NOTICE / ERROR），
但不改变正常路径语义。

### 11.1 两个失效检查 SQL helper

V3 Task 2 新挂了 2 个 `public.*` synonym，可在应用代码里主动检查分支状态：

| 函数 | 返回值 | 典型用途 |
|---|---|---|
| `is_active(name)` | `bool` | true = ground truth：该 branch 仍为 `active`（且当前 session 未被踢出）；false = applied/discarded/nonexistent |
| `force_invalidation_check()` | `bool` | 立刻忽略 throttling、跑一次重量级 SPI select 从 pg_branch 拿 ground truth；发现 state 变了则踢出 branch，返回 false 并附带 NOTICE |

典型用法：

```sql
-- 进入分支后，长事务 loop 的心跳检查：每次迭代先确认 branch 还活着
SELECT use_branch('agent_workspace');

-- ... 中间若干 DML ...
SELECT is_active('agent_workspace');
 is_active
-----------
 t

-- 如果这时另一会话 apply 了 agent_workspace...
SELECT apply_branch('agent_workspace') FROM (SELECT 1) x WHERE (SELECT current_setting('overlay_branch.current', true)) <> 'agent_workspace';
-- (上面的 WHERE 用来模拟"另一 session 执行 apply"，真实场景是另一个 psql 连接)

SELECT force_invalidation_check();
NOTICE:  overlay_branch: force_invalidation_check: branch "agent_workspace" state is applied,
             kicking out (was active)
 force_invalidation_check
--------------------------
 f

SELECT current_branch();
 current_branch
----------------
                (empty — 已回 Main)
```

### 11.2 跨会话 Apply/Discard 后 DML Kickout (ERRCODE 55000)

> 这是 FR4 与 FAIL#1 CmdType split 修复的用户可见后果：
> 分支一旦被别人 apply/discard，下一条 **DML** 会被 **ERROR 55000 拦截**（不是静默 NOTICE 继续），
> 从根本上防止漂移写入 MAIN。DQL（纯 SELECT）则只给 NOTICE + fallback，
> 通过后踢出（但 DQL 不会污染 MAIN）。

```sql
-- ========== 场景：两个 psql 连接，sA 仍在分支 b_kick 里工作，sB 将其 apply ==========

-- (sA)
SELECT create_branch('b_kick');
SELECT use_branch('b_kick');
INSERT INTO products VALUES (5, 'SA_work', 777);
INSERT 0 1

-- (sB)
SELECT apply_branch('b_kick');
NOTICE:  overlay_branch: APPLY BRANCH 'b_kick' completed ...
 apply_branch
--------------

-- (sA 继续写，以为 b_kick 仍 active)
INSERT INTO products VALUES (6, 'SA_late', 0);
ERROR:  overlay_branch: state of branch "b_kick" is not active (state=applied);
             drift writes to MAIN are blocked. DML cannot proceed inside a non-active
             branch context. Re-enter a fresh branch or return to MAIN.
HINT:  ERRCODE 55000 (object_not_in_prerequisite_state).
DETAIL:  Apply/Discard on this branch was committed by another session; a NOTIFY was
         broadcast. This backend's next DML statement detected the state change via
         throttled SPI counter fallback + force recheck.

-- 此时 sA 自动踢出 branch，current_branch() 为空：
SELECT current_branch() = '';
 ?column?
----------
 t
```

### 11.3 FR3 Applying 状态期间 DML 阻塞

apply_branch() 开始执行时，状态会先原子切换到 `applying`（CAS gate），
直到 MAIN 合并全部成功 commit 后才变成 `applied`。在这短暂窗口内：

```sql
-- 假设 sA 正在 apply b_large（用 _debug_apply_sleep_sec 模拟长窗口）
--   sB 在同一 b_large 里发一条 INSERT：
INSERT INTO products VALUES (7, 'rush', 1);
ERROR:  overlay_branch: state of branch "b_large" is 'applying'; write to delta
             is forbidden while an apply is in progress. Please retry.
HINT:  ERRCODE 55000.
DETAIL:  FR3 transient state guard: apply winner CAS set state='applying' while
         performing MAIN merge; after commit state transitions to 'applied' via
         pg_branch catalog row update; delta writes are blocked so there is no
         "torn write" where a new WR delta was written during apply and silently
         lost.
```

### 11.4 DO block 包裹重试模板

推荐在 Agent 应用代码里用此模板包住每条 WR 语句：

```sql
DO $$
DECLARE
    retries int := 0;
BEGIN
    <<retry_loop>>
    LOOP
        BEGIN
            INSERT INTO products VALUES (9, 'retryable', 99);
            EXIT retry_loop;                          -- 成功就退出
        EXCEPTION
            WHEN SQLSTATE '55000' THEN                -- 分支失效 / applying 期间
                retries := retries + 1;
                IF retries > 3 THEN RAISE; END IF;     -- 最多 3 次
                PERFORM pg_sleep(0.1 * retries);
                -- 开新分支重试（实际逻辑按业务写）
                PERFORM use_branch('fresh_' || retries::text, 'live');
        END;
    END LOOP retry_loop;
END $$;
```

生产级封装：对 55000 显式分支失效场景（apply/discard kickout、applying 窗口）做有限重试；
超过阈值则上抛让调用方人工介入，不做静默 ours/theirs merge。

---

## 12. V4：list_branches() 扩展到 11 列（A9 identity + drift guard）

V4 起 `list_branches()` 不再只是 catalog 上 7 列的视图投影——它额外返回 A9 恢复用的 4 个身份元字段（`schema_hash` / `creation_snapshot_xmin` / `tablespace_list` / `col_signature`）以及 2 个 drift guard 字段（`registry_user_tables` / `registry_schema_current_match`），
一次调用同时拿到"生命周期 + 恢复身份 + MAIN schema 漂移自检"，无需额外 roundtrip。

典型用法（Agent 保存 work-unit checkpoint 时，与 branch name 一起持久化 identity）：

```sql
/* 备份包 checkpoint：记录 branch identity，下次 restore 前先做 drift 检查 */
SELECT branch_id, branch_name, state,
       schema_hash, creation_snapshot_xmin, tablespace_list, col_signature,
       registry_user_tables, registry_schema_current_match
  FROM list_branches() WHERE branch_name = 'agent_workspace';
```

若 `registry_schema_current_match = false`，代表在 branch 创建后到现在之间，"有人在 MAIN 上改了受管表结构"（ALTER TABLE add/drop col、改列类型），即使 branch 自己没做任何 schema 变更，apply 时 MAIN/delta JOIN 也会因为列错位静默产生 wrong results。**此时推荐流程**：先 discard 该 branch（或通过 get_branch_identity 打印 diff），重新 create，再重做修改；不要硬 apply。

---

## 13. V4：get_branch_identity() — 身份与恢复绑定（A9）

A9 新增的 pure SQL helper（planner 不进入 C UPDATE block，即使有 drift 也不会写任何东西，纯读安全）。

| 列 | 说明 |
|---|---|
| `branch_id` | pg_branch.branch_id |
| `schema_hash` | 创建时 MAIN schema 指纹（16 bytes MD5） |
| `creation_snapshot_xmin` | 创建时 MAIN snapshot |
| `tablespace_list` | 受管表表空间 CSV |
| `col_signature` | C UPDATE block 同算法 8-tuple 列签名（drift 权威依据） |
| `registry_user_tables` | 创建时 MAIN 受管表数 |
| `registry_schema_current_match` | **现在 MAIN 的 col_signature == 创建时的 col_signature？** |

```sql
SELECT * FROM public.get_branch_identity('agent_workspace');
```

> 典型用法（apply 前自检）：如果 `registry_schema_current_match = false`，说明存在 drift，不能 apply，必须人工处理。

---

## 14. V4：Empty-Branch Apply/Discard Fast-Return（A4 performance）

A4 MVP 的纯 SQL wrapper 优化（不改动 C 层任何东西，正确性通过"3-condition guard 不满足就 fallback 到 C 层"绝对保证）。

### 14.1 3 条件同时满足 → 空分支 fast return（跳过 C 3-pass replay）

```
(1) branch.state = 'active'（apply 时）OR 'active'/'applying'（discard 时）
(2) count(pg_branch_delta WHERE branch_id = x) = 0
(3) current_setting('overlay_branch.current') IS DISTINCT FROM _name
    即：当前 session 没有正在 use_branch(_name)（current branch）
```

### 14.2 为什么 condition (3)

current branch 上的 apply/discard 必须走 C 层 handler，因为要做：

1. `overlay_branch.current` GUC reset 为空（SET ConfigOption 内存结构，SQL function 没法保证所有 backend 都按 SET search_path 和 GUC 的交互顺序正确）
2. session-level 内存 hash `ob_snapshot_cache` / `ob_mode_cache` 清掉（snapshot 模式冻结的快照 / live mode cache）
3. **NOTICE** 用户可见"discarding current branch, reverting to Main"
4. 广播 invalidation + NOTIFY 其它 backend（C 层 `CacheInvalidateRelcacheAll` + `CommandEndInvalidation`）

SQL wrapper 做不到 1/2/4，所以 current branch 一律 fallback C 层（语义 100% 保留）。

### 14.3 用户可见效果

```sql
-- 1000 次空分支 create+apply，快 < 1ms（非 current）：
DO $$
DECLARE
    _s timestamp; _ms int;
BEGIN
    FOR i IN 1..5 LOOP
        PERFORM public.create_branch('b_empty_'||i);
        _s := clock_timestamp();
        PERFORM public.apply_branch('b_empty_'||i);   -- fast return 生效
        _ms := round(1000 * EXTRACT(EPOCH FROM (clock_timestamp()-_s)))::int;
        RAISE NOTICE 'empty apply #% elapsed=% ms', i, _ms;
    END LOOP;
END $$;
```

不会产生 `NOTICE: overlay_branch: APPLY BRANCH ...` / `NOTICE: overlay_branch: DISCARD BRANCH ...` 这两条 C 层 elog（因为没进入 C 3-pass）。
fallback 场景（非空分支 / current 分支空）仍然完整走 C 层 handler，所有 NOTICE 不变。

---

## 15. V4：活动分支内 DDL RESTRICTED（A7 schema-drift guard）

A7 MVP 的硬门禁（之前只是"MVP 暂不支持"级别的 generic ProcessUtility 拦截），现在升级为**精确的 schema-drift guard**。

典型报错（已进入 active branch，再 CREATE TABLE）：

```
ERROR:  overlay_branch: cannot CREATE TABLE inside active branch "b_live":
        CREATE TABLE on MAIN user objects during an active branch would modify
        underlying schema/tables in place and break MAIN/delta join semantics
        after apply.  (§A7 DDL RESTRICTED 0A000 — schema-drift guard)
HINT:  Switch back to the MAIN database before running this statement:
           SELECT overlay_branch.discard_branch('b_live');
         or
           RESET overlay_branch.current;
```

### 15.1 受保护的 DDL 动作（全部 0A000 feature_not_supported）

| 类型 | 行为 |
|------|------|
| CREATE TABLE / AS SELECT / UNLOGGED / TEMP* | **TEMP 表除外**（TEMP 属于 session-local，不写 MAIN）；普通表 / UNLOGGED 拦截 |
| ALTER TABLE / ADD COLUMN / DROP COLUMN / ALTER TYPE / SET SCHEMA | 全部拦截（会直接改 MAIN 物理列 → delta join 错位） |
| DROP TABLE / DROP INDEX / TRUNCATE | 全部拦截；TRUNCATE MAIN 会导致 base_image old_version 全部失效 |
| CREATE INDEX / DROP INDEX / REINDEX / CLUSTER | 全部拦截（会影响 MAIN 执行计划缓存 / 表空间） |
| ALTER TYPE / CREATE TYPE / DROP TYPE | 全部拦截（列类型变了 → col_signature 失效 → drift） |
| ALTER TABLE ADD / DROP / ENABLE / DISABLE TRIGGER | 全部拦截（FK trigger cascade write 等后续子问题） |
| CREATE SEQUENCE / ALTER SEQUENCE / DROP SEQUENCE | 拦截（sequence 是 shared object，nextval 会直接改 MAIN） |

> **TEMP 表白名单**：`CREATE TEMP TABLE` / `CREATE TEMPORARY TABLE` 不受限（属于当前 session 私有，commit 自动 drop，不写 MAIN catalog、不会导致 drift）。

### 15.2 正确 workflow 推荐

```sql
/* 推荐模式：MAIN 上把所有 CREATE TABLE / ALTER 先做完 → 再开 branch 写数据 */
-- step 1: 在 MAIN 上建表 / 改表
CREATE TABLE orders (id int PRIMARY KEY, amount numeric(12,2));   -- OK（MAIN）
ALTER TABLE products ADD COLUMN stock int DEFAULT 0;             -- OK（MAIN）

-- step 2: 开 branch 改数据
SELECT create_branch('b_order_work');
SELECT use_branch('b_order_work');
INSERT INTO orders VALUES (1, 99.0);            -- OK（WR delta）
UPDATE products SET stock = 10 WHERE id = 1;    -- OK
-- CREATE TABLE foo(id int);                     -- ERROR 0A000 §A7 schema-drift guard（必须先回 MAIN）
```
