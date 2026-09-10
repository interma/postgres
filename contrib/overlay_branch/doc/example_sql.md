# Overlay Branch — 用户 SQL 接口与示例

> 本文档从**用户视角**描述 Overlay Branch 扩展**当前已实现**的 SQL 接口和典型输出。
> 文中所有示例均可直接在 psql 里复现，内容就是回归测试用例所验证的真实行为。
>
> **范围（MVP）**：仅支持**有主键的普通 heap 表**；Live Branch（Latest Main + Branch Delta）语义；
> 没有 snapshot 隔离；没有自定义 SQL 语法（用函数调用代替）。
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
  - [6.2 Pure Delta UPDATE（MVP = NOP）](#62-pure-delta-updatemvp--nop)
  - [6.3 Pure Delta × typmod PK：NUMERIC / BPCHAR](#63-pure-delta--typmod-pknumeric--bpchar)
  - [6.4 Pure Delta × 空 MAIN heap 全链路](#64-pure-delta--空-main-heap-全链路)
- [7. RETURNING 子句](#7-returning-子句)
- [8. Data-Modifying CTE：明确 ERROR](#8-data-modifying-cte明确-error)
- [9. MVP 暂不支持的场景（明确报错）](#9-mvp-暂不支持的场景明确报错)

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

失败示例：

```sql
-- 重名会报错
SELECT create_branch('agent_workspace');
ERROR:  branch "agent_workspace" already exists
```

> MVP **不支持 snapshot 模式**（`WITH (isolation='snapshot')`）。
> 所有 branch mode 恒为 `'live'`，即「未改的行跟随 Main 最新版本」。

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

纯 SQL 实现的表函数，可 JOIN、可过滤。**返回 7 列，顺序固定**：

```sql
\x
Expanded display is on.
SELECT * FROM list_branches() WHERE branch_name = 'agent_workspace';
-[ RECORD 1 ]+-------------------------------
branch_id    | 1
branch_name  | agent_workspace
owner        | 10                    -- 实际为当前用户 oid
created_at   | 2026-09-10 10:00:00+08  -- TIMESTAMPTZ
mode         | live                  -- MVP 恒为 'live'
state        | active                -- active | applied | discarded
delta_count  | 0                     -- pg_branch_delta 中该行 branch_id 的行数

\x
Expanded display is off.
```

字段一览：

| 列名 | 类型 | 含义 |
|------|------|------|
| `branch_id` | integer | 内部递增 id |
| `branch_name` | name | 分支名（唯一）|
| `owner` | oid | 创建者 |
| `created_at` | timestamptz | 创建时间 |
| `mode` | text | **MVP 恒为 `'live'`**；snapshot 模式未实现 |
| `state` | text | `active` / `applied` / `discarded` |
| `delta_count` | bigint | 当前 `pg_branch_delta` 中该分支的增量行数 |

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

> ⚠️ 容易被忽略的架构边界：**Write Redirect 的 CMD_UPDATE/DELETE 主循环只扫 MAIN heap（物理行 ctid）**，
> 所以"分支里自己 INSERT 出来、MAIN 上从来没有过"的纯 delta 行不会进入主循环。
> V2 已经实现 **pure-delta 单独 ExecQual pass**（8-phase），对纯 INSERT 候选重跑一次 WHERE
> 条件（兼容 IndexScan / BitmapHeapScan / IndexOnlyScan 3 种下推 qual 存储位置），命中的写 tombstone。
> 详见 `doc/progress_tracker.md` V2-P3 8-phase 图。

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

### 6.2 Pure Delta UPDATE（MVP = NOP）

分支里自己 INSERT 出来的行再 UPDATE — **V2 MVP 明确保持安全 NOP**
（不静默脏改数据；需要改先 DELETE 再 INSERT 等效语义）。断言不会 silent 变更：

```sql
INSERT INTO products VALUES (999, 'WillNotChange', 1);
SELECT id, name, price FROM products WHERE id = 999;
 id  |     name      | price
-----+---------------+-------
 999 | WillNotChange |     1

UPDATE products SET price = 999999 WHERE id = 999;
SELECT id, name, price FROM products WHERE id = 999;
 id  |     name      | price
-----+---------------+-------
 999 | WillNotChange |     1     ← 仍是原值（MVP 安全 NOP；V3 再实装 pure-delta UPDATE）
```

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
| 在分支里执行大部分 DDL（ALTER / CREATE TABLE / DROP）| ProcessUtility 钩子拦截 | `overlay_branch: cannot execute <DDL stmt> inside active branch ...` |
| `INSERT ... ON CONFLICT` (UPSERT) | 走标准 ModifyTable → 被 guard 拦截或按普通 INSERT 处理，MVP 建议先查询再写入 + 应用层重试 |
| **分区表**（根/叶）| 6 层 guard 的 G3 级提前拒绝（relkind / partitioned） | `overlay_branch does not support partitioned tables`（guard 通用提示）|
| **FK 级联写**（trigger 触发的子表级联 UPDATE/DELETE）| G4 级非 internal trigger 拦截或 G5 `pg_constraint` FK 检测 | `cannot modify via FK-triggered write`（guard 通用提示）|
| 直接对 `pg_branch` / `pg_branch_delta` 用户写 | `security_barrier` view 无 INSERT/UPDATE/DELETE rule → 普通报错 | `cannot insert into view "pg_branch"`（PG 原生视图错误）|
| snapshot 模式 / `isolation = 'snapshot'` | mode 列目前只接受 `'live'`（create_branch() 内部写死默认值） | MVP 直接用 live 不需要传参；传入 snapshot 不生效（保留字段为未来兼容）|
| 对 VIEW / MATVIEW / FOREIGN TABLE 写 DML | 在 guard 的 G2/G3 级就拦截，不支持透明叠加读 | V2 BranchScan planner hook 直接 skip，走原路径；DML 则在 WR guard 报错 |

这些限制在后续版本逐步解除。MVP 能跑通「有 PK 的普通 heap 表 + I/U/D + V2 透明 BranchScan
（含 P0 PK O(1) 快路径 + P2 WHERE 非 PK 下推 + P1 RETURNING）+ apply/discard + 冲突检测」
就已证明 Overlay Branch 模型的正确性。
