# Overlay Branch — 用户 SQL 接口与示例

> 本文档从**用户视角**描述 Overlay Branch 扩展的 SQL 接口和行为。
> 文中所有示例均可直接在 psql 里复现。
>
> **范围**：仅支持普通 heap 表；默认须有主键，无 PK 表见 §6.8 实验特性。用函数调用代替自定义 SQL 语法。

---

## 目录

- [0. 前置：安装扩展与建表](#0-前置安装扩展与建表)
- [1. 分支管理](#1-分支管理)
  - [1.1 create_branch() — 创建分支](#11-create_branch--创建分支)
  - [1.2 use_branch() — 切换当前分支 / 离开分支](#12-use_branch--切换当前分支--离开分支)
  - [1.3 current_branch() — 查询当前分支](#13-current_branch--查询当前分支)
  - [1.4 list_branches() — 列举分支](#14-list_branches--列举分支)
  - [1.5 discard_branch() — 丢弃分支](#15-discard_branch--丢弃分支)
- [2. 在分支内读写数据](#2-在分支内读写数据)
  - [2.1 INSERT](#21-insert)
  - [2.2 UPDATE](#22-update)
  - [2.3 DELETE](#23-delete)
  - [2.4 叠加语义总览](#24-叠加语义总览)
  - [2.5 完整叠加示例](#25-完整叠加示例)
  - [2.6 分支读不到其它分支的改动](#26-分支读不到其它分支的改动)
- [3. Snapshot vs Live 两种分支语义对比](#3-snapshot-vs-live-两种分支语义对比)
  - [3.1 典型场景对比](#31-典型场景对比)
  - [3.2 Live 语义：感知 Main 变化](#32-live-语义感知-main-变化)
  - [3.3 同一 MAIN 变化下的感知差异](#33-同一-main-变化下的感知差异)
  - [3.4 分支内自写 delta 均可见（两种语义一致）](#34-分支内自写-delta-均可见两种语义一致)
- [4. apply_branch() — 把分支增量合入 Main](#4-apply_branch--把分支增量合入-main)
  - [4.1 APPLY 成功路径](#41-apply-成功路径)
  - [4.2 APPLY 冲突路径](#42-apply-冲突路径)
- [5. 暂不支持的场景（明确报错）+ 已实装 UPSERT](#5-暂不支持的场景明确报错--已实装-upsert)
  - [5.1 UPSERT 实装示例](#51-upsert-实装示例)
  - [5.2 Data-Modifying CTE：明确 ERROR](#52-data-modifying-cte明确-error)
  - [5.3 支持矩阵](#53-支持矩阵)
- [6. 特殊情况](#6-特殊情况)
  - [6.1 Pure Delta 场景：分支里先 INSERT 再 UPDATE/DELETE](#61-pure-delta-场景分支里先-insert-再-updatedelete)
  - [6.2 RETURNING 子句](#62-returning-子句)
  - [6.3 Applying 防护 + Kickout 失效 & 新 helper](#63-applying-防护--kickout-失效--新-helper)
  - [6.4 list_branches() 身份与 drift guard 字段](#64-list_branches-身份与-drift-guard-字段)
  - [6.5 get_branch_identity() — 身份与恢复绑定](#65-get_branch_identity--身份与恢复绑定)
  - [6.6 空分支 Apply/Discard 快速返回](#66-空分支-applydiscard-快速返回)
  - [6.7 活动分支内 DDL RESTRICTED](#67-活动分支内-ddl-restricted)
  - [6.8 无 PK 表（实验特性）](#68-无-pk-表实验特性)

---

## 0. 前置：安装扩展与建表

### 0.1 安装（PG17）

```bash
# ① 确认目标实例 PG_CONFIG（版本 17）
pg_config --version; pg_config --pkglibdir; pg_config --sharedir

# ② 编译
cd /home/ubuntu/work/postgres/contrib/overlay_branch
make USE_PGXS=1 PG_CONFIG=$(which pg_config) -j4

# ③ 安装
make USE_PGXS=1 PG_CONFIG=$(which pg_config) install

# ④ 启用 shared_preload_libraries，重启
grep -qE '^shared_preload_libraries\s*=\s*.*overlay_branch' $PGDATA/postgresql.conf \
  || echo "shared_preload_libraries = 'overlay_branch'" >> $PGDATA/postgresql.conf
pg_ctl -D $PGDATA restart -m fast

# ⑤ 创建扩展
psql -d your_db <<'SQL'
CREATE EXTENSION overlay_branch;
SQL
```

### 0.2 安装验证

```sql
SHOW shared_preload_libraries;
-- 必含 overlay_branch

SELECT proname FROM pg_proc WHERE proname IN ('create_branch','apply_branch','list_branches') ORDER BY 1;
SELECT count(*) FROM public.list_branches();

-- E2E 冒烟
SELECT create_branch('smoke_test_001');
BEGIN;
  SELECT use_branch('smoke_test_001');
  EXPLAIN (COSTS OFF) SELECT 1;
ROLLBACK;
SELECT discard_branch('smoke_test_001');
```

### 0.3 准备示例表

```sql
CREATE TABLE products (
    id    integer PRIMARY KEY,
    name  text    NOT NULL,
    price integer NOT NULL
);

INSERT INTO products VALUES
  (1, 'Apple',  10),
  (2, 'Banana',  5),
  (3, 'Cherry', 20);
```

---

## 1. 分支管理

> 所有操作都通过 `public.*` 函数调用，可省略 schema 限定。

### 1.1 create_branch() — 创建分支

```sql
SELECT create_branch('agent_workspace');
 create_branch
---------------
             1
(1 row)
```

> 返回内部递增的 `branch_id`。创建分支不会拷贝任何数据。

失败示例：

```sql
SELECT create_branch('agent_workspace');
ERROR:  branch "agent_workspace" already exists
```

### 1.2 use_branch() — 切换当前分支 / 离开分支

```sql
-- 进入一个分支（会话级）
SELECT use_branch('agent_workspace');
 use_branch
------------

(1 row)
```

```sql
-- 进入分支时指定语义模式：
--   mode => 'live'     （默认：Latest Main + Branch Delta）
--   mode => 'snapshot' （MAIN 表读按进入时快照冻结）
SELECT use_branch('agent_workspace', mode => 'snapshot');

-- 切换回 live
SELECT use_branch('agent_workspace', mode => 'live');
```

```sql
-- 离开当前分支，回到 Main
SELECT use_branch(NULL);
 use_branch
------------

(1 row)
```

失败示例：

```sql
SELECT use_branch('does_not_exist');
ERROR:  branch "does_not_exist" does not exist
```

### 1.3 current_branch() — 查询当前分支

```sql
-- 在 Main 里：返回空
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

表函数，可 JOIN、可过滤，返回 11 列：

```sql
\x
SELECT * FROM list_branches() WHERE branch_name = 'agent_workspace';
-[ RECORD 1 ]-------------------------+----------------------------------
branch_id                             | 1
branch_name                           | agent_workspace
owner                                 | 10
created_at                            | 2026-09-10 10:00:00+08
mode                                  | live
state                                 | active
delta_count                           | 0
schema_hash                           | \x2d6f...
creation_snapshot_xmin                | 789
tablespace_list                       | 'pg_default:1663,my_tblspc:16789'
col_signature                         | \x8a3b...
registry_user_tables                  | 12
registry_schema_current_match         | t
\x
```

11 列字段一览：

| 列名 | 类型 | 含义 |
|------|------|------|
| `branch_id` | integer | 内部递增 id |
| `branch_name` | name | 分支名（唯一）|
| `owner` | oid | 创建者 |
| `created_at` | timestamptz | 创建时间 |
| `mode` | text | `live` 或 `snapshot`（当前 backend 正在使用的模式）|
| `state` | text | `active` / `applied` / `discarded` / `applying`（瞬态）|
| `delta_count` | bigint | 该分支的增量行数 |
| `schema_hash` | bytea | 创建时 MAIN 所有受管表 schema 指纹 MD5 |
| `creation_snapshot_xmin` | xid | 创建时 MAIN 事务 snapshot xmin |
| `tablespace_list` | text | 受管表 tablespace `name:oid` CSV |
| `col_signature` | bytea | 列签名，drift detection 权威依据 |
| `registry_user_tables` | bigint | 创建时 MAIN 受管表数量 |
| `registry_schema_current_match` | bool | MAIN 当前 schema 是否与创建时一致（未漂移）|

### 1.5 discard_branch() — 丢弃分支

```sql
SELECT discard_branch('agent_workspace');
 discard_branch
----------------

(1 row)

-- 丢弃后：state = discarded，delta_count = 0
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

## 2. 在分支内读写数据

**进入分支后，所有普通的 INSERT / UPDATE / DELETE 不会写主表，只会写入分支增量。**
用户不需要改 DML 语法。

```sql
SELECT use_branch('agent_workspace');
```

### 2.1 INSERT

```sql
INSERT INTO products VALUES (4, 'Date', 30);
INSERT 0 1
```

### 2.2 UPDATE

```sql
UPDATE products SET price = 100 WHERE id = 1;
UPDATE 1
```

### 2.3 DELETE

```sql
DELETE FROM products WHERE id = 2;
DELETE 1
```

---

### 2.4 叠加语义总览

| 分支操作 | Main 有 | Main 无 | 输出 |
|-----------|---------|---------|------|
| INSERT | — | ✅ | 输出分支新行 |
| UPDATE | ✅ | — | 输出分支覆盖后的行 |
| DELETE | ✅ | — | 屏蔽 Main 的行 |
| 无操作 | ✅ | — | 原样输出 Main 行 |
| 无操作 | — | — | 看不到 |

两种读路径：

```sql
-- 透明读（推荐：零语法改动，需 shared_preload_libraries='overlay_branch'）
SELECT * FROM products ORDER BY id;

-- 手动 SRF（兜底入口；必须显式给列类型列表）
SELECT * FROM overlay_branch.overlay_main_plus_delta('products')
       AS x(id integer, name text, price integer)
 ORDER BY id;
```

### 2.5 完整叠加示例

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

**在分支里看**：

```sql
SELECT use_branch('agent_workspace');
SELECT * FROM products ORDER BY id;
 id |  name  | price
----+--------+-------
  1 | Apple  |   100
  3 | Cherry |    20
  4 | Date   |    30
(3 rows)
```

### 2.6 分支读不到其它分支的改动

```sql
SELECT create_branch('branch_B');
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

## 3. Snapshot vs Live 两种分支语义对比

同一 branch 可以在不同 session 里分别以 snapshot 或 live 方式进入。

### 3.1 典型场景对比

| 维度 | mode='live'（默认） | mode='snapshot' |
|------|---|---|
| **MAIN 表的旧行**（没被自己写过） | 始终跟随 Main 最新提交 | **冻结**在进入那一刻的快照 |
| **分支自己写的行** | 始终可见 | 始终可见 |
| **同一 branch 其他 session 新写的行** | 始终可见 | 始终可见 |
| **典型场景** | Agent 协作工作区；保留自己改的同时看别人的新提交 | 报表 / 离线分析；MAIN 数据在查询期间不漂移 |
| **怎么进入** | `use_branch('b')` 或 `use_branch('b','live')` | `use_branch('b','snapshot')` |

### 3.2 Live 语义：感知 Main 变化

默认的 `mode='live'` 语义是 **Latest Main + Branch Delta 的叠加**：
没被分支改过的行，永远跟随 Main 最新提交的版本；分支改过的行，看到分支自己覆盖后的版本。

```sql
SELECT use_branch(NULL);
UPDATE products SET price = 9999 WHERE id = 3;
UPDATE 1

SELECT use_branch('branch_B');
SELECT * FROM products WHERE id = 3;
 id |  name  | price
----+--------+------
  3 | Cherry | 9999
(1 row)
```

### 3.3 同一 MAIN 变化下的感知差异

```sql
-- 准备
SELECT use_branch(NULL);
TRUNCATE products;
INSERT INTO products VALUES (1,'A',10),(2,'B',5),(3,'C',20);
SELECT create_branch('b_snap');

-- s1: snapshot 会话
SELECT use_branch('b_snap', 'snapshot');
SELECT * FROM products ORDER BY id;
-- 看到 MAIN baseline: A(10), B(5), C(20)

-- s2: live 会话在 MAIN 写入（另一个 psql 连接）
SELECT use_branch(NULL);
INSERT INTO products VALUES (4,'D',30);
UPDATE products SET price=9999 WHERE id=3;
COMMIT;

-- 回到 s1: snapshot 会话
SELECT * FROM products ORDER BY id;
-- id | name | price      （MAIN 冻结！）
--  1 |  A   | 10
--  2 |  B   |  5         看不到 MAIN id=4/D；id=3 仍为 20
--  3 |  C   | 20
-- (3 rows)

-- s3: 同一 branch live 方式进入
SELECT use_branch(NULL);
SELECT use_branch('b_snap', 'live');
SELECT * FROM products ORDER BY id;
-- id | name | price
--  1 |  A   |   10
--  2 |  B   |    5         LIVE：id=4 看到了！id=3 涨价到 9999 了！
--  3 |  C   | 9999
--  4 |  D   |   30
-- (4 rows)
```

### 3.4 分支内自写 delta 均可见（两种语义一致）

```sql
SELECT use_branch('b_snap', 'snapshot');
UPDATE products SET price=7 WHERE id=2;
INSERT INTO products VALUES (99, 'Mine', 1);
SELECT * FROM products ORDER BY id;
-- 1|A|10       (MAIN frozen)
-- 2|B|7        (自己的 delta)
-- 3|C|20       (MAIN frozen)
-- 99|Mine|1    (自己的 delta)
```

---

## 4. apply_branch() — 把分支增量合入 Main

把分支当前的增量合入 Main，逐条做乐观冲突验证。冲突直接报 ERROR，不做自动 merge。

### 4.1 APPLY 成功路径

```sql
SELECT use_branch(NULL);
SELECT apply_branch('agent_workspace');
NOTICE:  overlay_branch: APPLY BRANCH 'agent_workspace' completed
             (state=applied, delta rows deleted)
 apply_branch
--------------

(1 row)
```

Apply 后，Main 被真实改写，分支的增量被清空，`state` 变为 `applied`：

```sql
SELECT * FROM products ORDER BY id;
 id |  name  | price
----+--------+-------
  1 | Apple  |   100
  3 | Cherry |    20
  4 | Date   |    30
(3 rows)

SELECT branch_name, state, delta_count FROM list_branches()
 WHERE branch_name = 'agent_workspace';
  branch_name   |  state  | delta_count
----------------+---------+-------------
 agent_workspace | applied |           0
(1 row)
```

**自 apply**：如果当前 session 正在 `use_branch(X)`，自己执行 `apply_branch(X)` 后，本 session 会自动回到 Main，无需再 `use_branch(NULL)`。

**跨会话 apply/discard**：如果 session A 正在 `use_branch(X)`，session B 执行了 `apply_branch(X)` 或 `discard_branch(X)`，则 session A 下一次 DML 会报错 `ERRCODE 55000` 并被踢出分支（见 §6.3）。

### 4.2 APPLY 冲突路径

场景：创建分支后，Main 自己也改了同一行，然后分支 apply —— 检测到冲突。

```sql
TRUNCATE products;
INSERT INTO products VALUES (1,'Apple',10),(2,'Banana',5),(3,'Cherry',20);

SELECT create_branch('alice');
SELECT use_branch('alice');
UPDATE products SET price = 999 WHERE id = 1;
UPDATE 1

SELECT use_branch(NULL);
UPDATE products SET price = 42 WHERE id = 1;
UPDATE 1

SELECT apply_branch('alice');
ERROR:  overlay_branch: apply conflict on relation products(primary key=["1"])
DETAIL:  branch's base old_version = '...'
         main's current old_version = '...'
         branch intends to write: (1,'Apple',999)
HINT:  Row was modified in Main after branch wrote it.
       Re-read current state and retry in a fresh branch.
```

---

## 5. 暂不支持的场景（明确报错）+ 已实装 UPSERT

以下场景会**明确拒绝**（不会静默穿透到 Main）：

| 场景 | 行为 | 典型错误 |
|------|------|----------|
| 对**无主键**表写 DML | 默认拒绝（见 §6.8）| `table must have a PRIMARY KEY for branch-mode DML ...` |
| 分支内 DDL（CREATE/ALTER/DROP TABLE 等）| 拒绝，提示退回 Main | `§A7 DDL RESTRICTED 0A000`（见 §6.7）|
| `INSERT ... ON CONFLICT` (UPSERT) | **已支持**（见 §5.1）| N/A |
| **分区表** | 拒绝 | `overlay_branch does not support partitioned tables` |
| **FK 级联写** | 拒绝 | `cannot modify via FK-triggered write` |
| 直接写 `pg_branch` / `pg_branch_delta` | 拒绝 | PG 原生视图错误 |
| 对 VIEW / MATVIEW / FOREIGN TABLE 写 DML | 拒绝 | guard 拦截 |
| **COPY FROM / MERGE / Data-Modifying CTE** | 拒绝 | `0A000 feature_not_supported` |
| **UPDATE SET pk_col = ...**（改主键）| 拒绝 | `Updating the PRIMARY KEY column ... is not supported` |
| 权限不足调用 use/apply/discard/create | 拒绝 | `42501 insufficient_privilege` |

### 5.1 UPSERT 实装示例

`INSERT ... ON CONFLICT DO NOTHING / DO UPDATE`（针对 PK 冲突）在分支内正常工作：

```sql
SELECT create_branch('br_upsert_demo');
SELECT use_branch('br_upsert_demo');

-- ON CONFLICT DO NOTHING：撞 PK → NOP
INSERT INTO products VALUES (1, 'NewApple', 999) ON CONFLICT (id) DO NOTHING;
INSERT 0 0
SELECT * FROM products WHERE id = 1;
 id | name  | price
----+-------+-------
  1 | Apple |    10

-- ON CONFLICT DO UPDATE：撞 PK → 以 EXCLUDED 新值更新
INSERT INTO products VALUES (3, 'Cherry_v2', 222)
  ON CONFLICT (id) DO UPDATE SET name  = EXCLUDED.name,
                                  price = EXCLUDED.price;
INSERT 0 1
SELECT * FROM products WHERE id = 3;
 id |   name    | price
----+-----------+-------
  3 | Cherry_v2 |   222

-- 撞分支自己的 pure-origin INSERT
INSERT INTO products VALUES (100, 'First', 1) ON CONFLICT (id) DO NOTHING;
INSERT INTO products VALUES (100, 'Dupe',  2) ON CONFLICT (id) DO NOTHING;
INSERT 0 0
SELECT * FROM products WHERE id = 100;
 id  | name  | price
-----+-------+-------
 100 | First |     1
```

### 5.2 Data-Modifying CTE：明确 ERROR

CTE 里嵌入 DML 再外层 SELECT 不允许：

```sql
WITH u AS (UPDATE products SET price=price+1 RETURNING id, price)
SELECT * FROM u;
ERROR:  overlay_branch: Data-Modifying CTE (WITH ... UPDATE/INSERT/DELETE ... RETURNING)
        is not supported inside an active branch. Use top-level DML with RETURNING instead.
```

替代方案：直接 top-level DML 加 `RETURNING`（见 §6.2）。

### 5.3 支持矩阵

| 能力项 | 状态 |
| --- | --- |
| plain INSERT/UPDATE/DELETE | ✅ 支持 |
| INSERT … ON CONFLICT（PK） | ✅ 支持 |
| UPDATE/DELETE … RETURNING | ✅ 支持 |
| WHERE pk = CONST | ✅ 推荐路径 |
| Seq scan / range pred | ✅ 支持 |
| 分支内 TEMP 表操作 | ✅ 支持（白名单）|
| CREATE TABLE / ALTER / DROP / CREATE INDEX / DROP TRIGGER | ❌ 0A000（退回 Main 执行）|
| 分区表 | ❌ 拒绝 |
| FK 级联写 | ❌ 拒绝 |
| COPY FROM / MERGE / Data-Modifying CTE | ❌ 拒绝 |
| UPDATE SET pk_col | ❌ 拒绝（用 DELETE + INSERT 替代）|
| 无 PK 表写 DML | ❌ 默认拒绝；`overlay_branch.keyless_tables=on` 时 ✅（实验，见 §6.8）|

RESTRICTED 项处理建议：

| 受限项 | 用户替代方案 |
| --- | --- |
| DDL | 先在 Main 执行 DDL → 重新 create_branch |
| COPY FROM | 改写为多行 `INSERT INTO t VALUES (...), (...)` |
| MERGE | 拆成独立 INSERT ON CONFLICT / UPDATE / DELETE |
| Data-Modifying CTE | 改 top-level DML + RETURNING |
| UPDATE pk | 改 `DELETE WHERE pk=old` + `INSERT VALUES (new_pk, ...)` |
| FK 级联写 | 应用层显式先写子表再写父表 |

---

## 6. 特殊情况

### 6.1 Pure Delta 场景：分支里先 INSERT 再 UPDATE/DELETE

分支里 INSERT 出来的、Main 上没有的行，后续 UPDATE/DELETE 同样正常工作。

#### 6.1.1 Pure Delta INSERT 再 DELETE

```sql
SELECT use_branch('agent_workspace');

INSERT INTO products VALUES (99, 'PureInserted', 777);
INSERT 0 1
SELECT count(*) FROM products WHERE id = 99;
 count
-------
     1

DELETE FROM products WHERE id = 99;
DELETE 1
SELECT count(*) FROM products WHERE id = 99;
 count
-------
     0
```

复合 WHERE 条件：

```sql
CREATE TABLE bs_pd_multi (
    grp   text NOT NULL,
    score int4 NOT NULL,
    id    int4 PRIMARY KEY
);
INSERT INTO bs_pd_multi VALUES ('main', 0, 1);

SELECT use_branch('agent_workspace');
INSERT INTO bs_pd_multi VALUES
  ('C', 333, 30), ('C', 333, 31), ('C', 999, 32);

DELETE FROM bs_pd_multi WHERE grp = 'C' AND score = 333;
DELETE 2
SELECT id, grp, score FROM bs_pd_multi WHERE grp = 'C' ORDER BY id;
 id | grp | score
----+-----+-------
 32 | C   |   999
(1 row)
```

#### 6.1.2 Pure Delta UPDATE

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
 999 | UpdatedOK   | 999999
```

#### 6.1.3 Pure Delta × typmod PK：NUMERIC / BPCHAR

```sql
CREATE TABLE bs_pd_numeric(price NUMERIC(10,2) PRIMARY KEY, tag TEXT);
SELECT use_branch('agent_workspace');
INSERT INTO bs_pd_numeric VALUES (30.33, 'pd-n1');
INSERT INTO bs_pd_numeric VALUES (40.44, 'pd-n2');
DELETE FROM bs_pd_numeric WHERE price = 30.33;
SELECT string_agg(tag, ',' ORDER BY tag) FROM bs_pd_numeric;
 string_agg
------------
 pd-n2

CREATE TABLE bs_pd_bpchar(code BPCHAR(6) PRIMARY KEY, tag TEXT);
SELECT use_branch('agent_workspace');
INSERT INTO bs_pd_bpchar VALUES ('PD001', 'A'), ('PD002', 'B'), ('PD003', 'C');
DELETE FROM bs_pd_bpchar WHERE code = 'PD002';
SELECT code::text, tag FROM bs_pd_bpchar ORDER BY code;
 code  | tag
-------+-----
 PD001 | A
 PD003 | C
```

#### 6.1.4 Pure Delta × 空 MAIN heap 全链路

```sql
CREATE TABLE bs_empty_pk(id INT4 PRIMARY KEY, tag TEXT);
SELECT use_branch('agent_workspace');
INSERT INTO bs_empty_pk VALUES (1,'one'),(2,'two'),(3,'three'),(4,'four');

DELETE FROM bs_empty_pk WHERE id IN (2,4);
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

SELECT apply_branch('agent_workspace');
 apply_branch
--------------

SELECT * FROM bs_empty_pk ORDER BY id;
 id | tag
----+-----
  1 | one
```

### 6.2 RETURNING 子句

`INSERT/UPDATE/DELETE RETURNING` 全支持：

```sql
SELECT use_branch('agent_workspace');

INSERT INTO products VALUES (10, 'ten', 10) RETURNING id, name, price;
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

Pure-delta DELETE 同样支持 RETURNING：

```sql
DELETE FROM bs_pd_multi WHERE id = 32 RETURNING id, grp, score;
 id | grp | score
----+-----+-------
 32 | C   |   999
(1 row)
```

### 6.3 Applying 防护 + Kickout 失效 & 新 helper

#### 6.3.1 两个失效检查 SQL helper

| 函数 | 返回值 | 用途 |
|---|---|---|
| `is_active(name)` | `bool` | 该 branch 是否仍为 `active` 且当前 session 未被踢出 |
| `force_invalidation_check()` | `bool` | 立即检查分支状态；发现 state 变了则踢出 branch，返回 false |

```sql
SELECT use_branch('agent_workspace');

SELECT is_active('agent_workspace');
 is_active
-----------
 t

-- 如果这时另一会话 apply 了 agent_workspace...
SELECT force_invalidation_check();
NOTICE:  overlay_branch: force_invalidation_check: branch "agent_workspace" state is applied,
             kicking out (was active)
 force_invalidation_check
--------------------------
 f

SELECT current_branch();
 current_branch
----------------
                (空 — 已回 Main)
```

#### 6.3.2 跨会话 Apply/Discard 后 DML Kickout

分支一旦被别的会话 apply/discard，当前 session 下一条 **DML** 会被 `ERRCODE 55000` 拦截并踢出分支。
DQL（纯 SELECT）则只给 NOTICE + fallback，不会报错。

```sql
-- sA: 在分支 b_kick 里工作
SELECT create_branch('b_kick');
SELECT use_branch('b_kick');
INSERT INTO products VALUES (5, 'SA_work', 777);

-- sB: 另一个连接 apply 了 b_kick
SELECT apply_branch('b_kick');

-- sA 继续写
INSERT INTO products VALUES (6, 'SA_late', 0);
ERROR:  overlay_branch: state of branch "b_kick" is not active (state=applied);
             drift writes to MAIN are blocked. DML cannot proceed inside a non-active
             branch context. Re-enter a fresh branch or return to MAIN.
HINT:  ERRCODE 55000 (object_not_in_prerequisite_state).

-- sA 已被踢出 branch
SELECT current_branch() = '';
 ?column?
----------
 t
```

#### 6.3.3 Applying 状态期间 DML 阻塞

apply_branch() 执行期间，状态先变为 `applying`，直到合并成功后变为 `applied`。
此窗口内其他 session 在该分支里的 DML 会被阻塞：

```sql
INSERT INTO products VALUES (7, 'rush', 1);
ERROR:  overlay_branch: state of branch "b_large" is 'applying'; write to delta
             is forbidden while an apply is in progress. Please retry.
HINT:  ERRCODE 55000.
```

#### 6.3.4 DO block 包裹重试模板

```sql
DO $$
DECLARE
    retries int := 0;
BEGIN
    <<retry_loop>>
    LOOP
        BEGIN
            INSERT INTO products VALUES (9, 'retryable', 99);
            EXIT retry_loop;
        EXCEPTION
            WHEN SQLSTATE '55000' THEN
                retries := retries + 1;
                IF retries > 3 THEN RAISE; END IF;
                PERFORM pg_sleep(0.1 * retries);
                PERFORM use_branch('fresh_' || retries::text, 'live');
        END;
    END LOOP retry_loop;
END $$;
```

### 6.4 list_branches() 身份与 drift guard 字段

`list_branches()` 一次调用返回生命周期 + 恢复身份 + MAIN schema 漂移自检：

```sql
SELECT branch_id, branch_name, state,
       schema_hash, creation_snapshot_xmin, tablespace_list, col_signature,
       registry_user_tables, registry_schema_current_match
  FROM list_branches() WHERE branch_name = 'agent_workspace';
```

若 `registry_schema_current_match = false`，说明 MAIN schema 已漂移（有人在 Main 上改了受管表结构），apply 时可能产生错误结果。**推荐流程**：先 discard 该分支，重新 create，再重做修改；不要硬 apply。

### 6.5 get_branch_identity() — 身份与恢复绑定

纯读 helper，用于 apply 前自检 drift：

| 列 | 说明 |
|---|---|
| `branch_id` | 分支 id |
| `schema_hash` | 创建时 MAIN schema 指纹（16 bytes MD5）|
| `creation_snapshot_xmin` | 创建时 MAIN snapshot |
| `tablespace_list` | 受管表表空间 CSV |
| `col_signature` | 列签名（drift 权威依据）|
| `registry_user_tables` | 创建时 MAIN 受管表数 |
| `registry_schema_current_match` | 现在 MAIN 的 col_signature 是否等于创建时 |

```sql
SELECT * FROM public.get_branch_identity('agent_workspace');
```

若 `registry_schema_current_match = false`，说明存在 drift，不能 apply，必须人工处理。

### 6.6 空分支 Apply/Discard 快速返回

对于**非当前激活**的空分支（delta 为 0），apply/discard 会快速返回，不产生 `NOTICE: overlay_branch: APPLY BRANCH ...` 提示：

```sql
DO $$
DECLARE
    _s timestamp; _ms int;
BEGIN
    FOR i IN 1..5 LOOP
        PERFORM public.create_branch('b_empty_'||i);
        _s := clock_timestamp();
        PERFORM public.apply_branch('b_empty_'||i);
        _ms := round(1000 * EXTRACT(EPOCH FROM (clock_timestamp()-_s)))::int;
        RAISE NOTICE 'empty apply #% elapsed=% ms', i, _ms;
    END LOOP;
END $$;
```

非空分支或当前激活分支的 apply/discard 仍走完整路径，所有 NOTICE 不变。

### 6.7 活动分支内 DDL RESTRICTED

进入 active branch 后，对 Main 用户对象的 DDL 会被拒绝，报错 `0A000`：

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

受保护的 DDL 动作（全部 0A000）：

| 类型 | 行为 |
|------|------|
| CREATE TABLE / UNLOGGED TABLE | 拦截（**TEMP 表除外**）|
| ALTER TABLE / ADD/DROP COLUMN / ALTER TYPE / SET SCHEMA | 全部拦截 |
| DROP TABLE / DROP INDEX / TRUNCATE | 全部拦截 |
| CREATE INDEX / DROP INDEX / REINDEX / CLUSTER | 全部拦截 |
| ALTER TYPE / CREATE TYPE / DROP TYPE | 全部拦截 |
| ALTER TABLE ADD/DROP/ENABLE/DISABLE TRIGGER | 全部拦截 |
| CREATE/ALTER/DROP SEQUENCE | 全部拦截 |

> **TEMP 表白名单**：`CREATE TEMP TABLE` / `CREATE TEMPORARY TABLE` 不受限（session 私有，不写 Main）。

推荐 workflow：

```sql
-- step 1: 在 Main 上建表 / 改表
CREATE TABLE orders (id int PRIMARY KEY, amount numeric(12,2));
ALTER TABLE products ADD COLUMN stock int DEFAULT 0;

-- step 2: 开 branch 改数据
SELECT create_branch('b_order_work');
SELECT use_branch('b_order_work');
INSERT INTO orders VALUES (1, 99.0);
UPDATE products SET stock = 10 WHERE id = 1;
```

### 6.8 无 PK 表（实验特性）

无 PK 表的 DML **默认被拒绝**（见 §5.3）。设置 `overlay_branch.keyless_tables = on` 后，扩展会给出一行 WARNING，并改用行的物理位置作为身份标识，使无 PK 表也能像 PK 表一样进入分支。

```sql
-- 默认（off）：无 PK 表写 DML 被拒绝
CREATE TABLE logs (msg text);
INSERT INTO logs VALUES ('boot');
SELECT create_branch('b_logs');
SELECT use_branch('b_logs');
INSERT INTO logs VALUES ('x');
ERROR:  overlay_branch: cannot INSERT relation  inside active branch "b_logs":
        table must have a PRIMARY KEY for branch-mode DML (V1 constraint;
        enable overlay_branch.keyless_tables for the experimental ctid fallback)
DETAIL:  Target relation: logs.
```

```sql
-- 开启实验开关后：一行 WARNING + 正常读写叠加
SET overlay_branch.keyless_tables = on;
INSERT INTO logs VALUES ('x');
WARNING:  overlay_branch: table "public.logs" has no PRIMARY KEY; using
          experimental ctid-based row identity (overlay_branch.keyless_tables=on)
INSERT 0 1

SELECT * FROM logs;   -- Main('boot') ⊕ 分支('x')
  msg
-------
 boot
 x

-- apply / discard 行为与 PK 表一致
SELECT apply_branch('b_logs');
```

行为要点：

| 项 | 行为 |
|----|------|
| 开关 | `overlay_branch.keyless_tables`（bool，默认 `off`，session 级 SET，需超级用户）|
| WARNING | 每会话首次命中无 PK 表时输出一次 |
| 读写叠加 | INSERT/UPDATE/DELETE/SELECT/apply/discard 与 PK 表一致 |
| 行身份 | 基于行的**物理位置**（best-effort）：分支存活期间若其它会话改动了分支锚定的 Main 行，则 apply 会报冲突 |
| UPSERT | `INSERT ... ON CONFLICT` 仍不支持，报 `0A000` |

> 该特性不追求与 PK 表完全对等，仅建议用于实验 / 短期草稿场景；长期使用请为表补主键。设计与限制详见 [keyless_ctid_design.md](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/keyless_ctid_design.md)。
