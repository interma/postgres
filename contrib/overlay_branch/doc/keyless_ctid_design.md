# 无 PK 表的 ctid 实验特性设计（keyless_ctid）

> **状态：已实现（MVP，方案 A）。** 代码见 `src/delta_store.c`（身份助手）、`src/branch_scan.c`（读路径）、
> `src/write_redirect.c`（写路径）、`src/branch_lifecycle.c`（apply 定位）；L3 回归
> `test/regress/sql/overlay_branch_keyless.sql`。
> 目标读者：实现者与评审者。
> 关联文档：[internals.md](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/internals.md)（§1.1 PK 作用、§4.3 Step7 HARD GUARD G5、§5 不变量 2）、
> [design.md](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/design.md)、
> [branchscan.md](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/branchscan.md)。
> 参考原型：gammadb `prototype/identity.c` + `prototype/retention.c`（E02–E04，见 §4）。

---

## 1. 目标与非目标

### 1.1 目标

当前扩展的硬前提是**用户表必须有 PRIMARY KEY**（[internals.md §1.1](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/internals.md)）。这一限制对"开箱体验"过重——很多真实表没有 PK，用户遇到 `G5` 报错后直接放弃。

本特性提供一个**受 GUC 控制的实验通道**：允许对**无 PK 表**进入分支，行身份改用**物理锚点（ctid + xmin）**而非 PK 值。

### 1.2 非目标

- 不改变 PK 表的现有行为（有 PK 时仍走 PK 路径，逐字节兼容）。
- 不追求与 PK 路径同等的鲁棒性。ctid 身份在**并发 MAIN 写**下天然脆弱（§3.6），仅作实验用途。
- 不默认开启。默认行为与现状完全一致。

---

## 2. 用户体验（GUC 语义）

### 2.1 GUC 定义

| 项 | 值 |
|----|----|
| 名称 | `overlay_branch.keyless_tables` |
| 类型 | `bool` |
| 默认 | `false` |
| 上下文 | `PGC_SUSET`（与 `overlay_branch.enabled` / `apply_strict_pins` 一致） |
| 注册点 | `_PG_init()` 内，紧邻 [overlay_branch.c](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/overlay_branch.c#L274-L283) 的 `overlay_branch.enabled` |

新增全局变量 `bool overlay_keyless_tables = false;`（[overlay_branch.c](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/overlay_branch.c#L76) 变量区）并在 [overlay_branch.h](file:///home/ubuntu/work/postgres/contrib/overlay_branch/include/overlay_branch.h#L197-L209) 加 `extern`。

### 2.2 行为矩阵

| GUC | 用户表 | 行为 |
|-----|--------|------|
| `off`（默认） | 有 PK | 现状：PK 路径（不变） |
| `off`（默认） | 无 PK | 现状：`G5` guard 报错 `0A000`（不变） |
| `on` | 有 PK | PK 路径（不变，PK 优先） |
| `on` | 无 PK | **输出一行 WARNING**，随后走 ctid 路径 |

### 2.3 WARNING 文案（每会话首次进入分支时输出一次）

```
WARNING: overlay_branch: table "<schema>.<table>" has no PRIMARY KEY; using
         experimental ctid-based row identity (overlay_branch.keyless_tables=on).
DETAIL:  Row identity is the physical tuple location (ctid, xmin).  Concurrent
         UPDATE/DELETE on this table by other sessions may change a row's ctid and
         cause the branch to silently miss that row's delta (detected only at
         apply time as a conflict error).
HINT:    Add a PRIMARY KEY (or ALTER TABLE ... ADD COLUMN ... GENERATED ALWAYS AS
         IDENTITY PRIMARY KEY) to get durable row identity.
```

WARNING 只在"检测到无 PK 表且 GUC=on"时触发一次（按 `(session, relid)` 去重，避免刷屏）；不得在 planner hook 里发出（planner 会被多次调用），在**写重定向入口**（`overlay_should_redirect` 命中后、首次真实 redirect）或 `use_branch` 首次扫描时发出。

---

## 3. 核心设计：anchor 身份

### 3.1 身份表示（复用 `pg_branch_delta.key`，不改表结构）

`pg_branch_delta.key text NOT NULL` 保持不变。keyless 表用**带 tag 前缀**的编码，与现有 PK key（JSON 数组，以 `[` 开头）天然不冲突：

| 行的来源 | key 编码 | 说明 |
|----------|----------|------|
| MAIN 中存在（anchored） | `A:<blk>:<off>` | 由 junk slot 的 `ctid` 系统列（[slot_get_ctid_cstr](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L581)）派生 |
| MAIN 中不存在（synthetic，纯 INSERT） | `S:<seq>` | per-`(branch_id, relid)` 单调序号，`seq = 1 + MAX(现有 S: 行的 seq)`（[overlay_synthetic_key](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L854) 从 delta 表自身推导；delta 行在 discard 前不物理删除，故单调） |

`old_version` 列**保持不变**，仍为 `blk:off-x<xmin>` 令牌（[overlay_tuple_version](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L1094) 已经生成该格式，直接复用）。

> 设计取舍：不改 SQL 表结构 → 不触碰 `overlay_branch--1.0.sql`、ACL（G02/R22）与 registry 指纹（R23）。keyless 的 `key` 只是换了一种字符串编码。

### 3.2 写路径映射

`overlay_modify_insert/update/delete`（[delta_store.c:1219](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L1219)）当前统一调用 `overlay_serialize_pk`。改造为统一的身份解析入口：

```
overlay_serialize_identity(rel, slot, op):
    if relation_has_pk(rel)         → return overlay_serialize_pk(rel, slot)          // 现状
    else (keyless):
        if op == INSERT and no MAIN origin → return "S:<nextseq>"
        else                               → return "A:" + ctid_of(slot)               // 从 ctid 系统列/tts_tid
```

- **UPDATE/DELETE**：`rel` 的 junk slot 携带 `ctid`（现有 [slot_get_ctid_cstr](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L581) 已能取到）→ anchored key。`old_version` 仍由 `overlay_tuple_version` 生成（§4 备注其物理含义）。
- **INSERT（MAIN 无同键）**：synthetic key `S:<seq>`。
- **INSERT（MAIN 已有同行，UPSERT promo=2）**：此时按 MAIN 行的 ctid 生成 anchored key。

### 3.3 读路径映射（BranchScan 2-pass）

合并算法 [ob_compute_overlay_slots_internal](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L139-L795) 结构不变，仅**身份提取与比较**两处替换：

| 阶段 | PK 现状 | keyless 改造 |
|------|---------|--------------|
| Pass 1 取 MAIN 行身份 | `overlay_serialize_pk(rel, main_slot)`（[branch_scan.c:560](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L560)）→ `overlay_build_typed_pk_from_slot` | 取 `main_slot->tts_tid` → `A:<blk>:<off>`；typed_pk 置空 |
| delta 排序 + bsearch 比较器 | `overlay_typed_pk_cmp`（[branch_scan.c:339-394](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L339-L394)） | keyless 时 typed_pk 为 NULL → 自动回退 `strcmp(key)`（[cmp_dtp_by_cstr / cmp_searchkey_vs_dtp](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L348-L396)）。两侧 key 均由同一格式生成，等值匹配正确 |
| Pass 2 追加 pure-INSERT | 遍历 leftover `op='I'` | 不变，`S:*` 与未被锚定的 `A:*`（孤儿）都会被追加，需处理孤儿（§3.6） |

**成本模型 / lappend bypass / pathlist 剪枝**（[branchscan.md Part A.3](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/branchscan.md)）不变。唯一变化：`ob_branchscan_planner_hook` 的注入前置条件 [branch_scan.c:1022](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L1022) 由 `if (!overlay_relation_has_pk(reln)) return;` 改为 `if (!has_pk && !overlay_keyless_tables) return;`；且 keyless 时**跳过** P0 PK 等值 fast-path（[branch_scan.c:1028](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L1028)）。

### 3.4 apply 路径映射

`apply` 现在按 `old_version`（`ctid:xmin`）做乐观校验，但**定位 MAIN 行**用的是 PK WHERE 子句 [ob_build_pk_where_clause](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L2852)。keyless 无 PK → 定位改为 ctid：

```
ob_build_identity_where_clause(rel, key):
    if key starts with "A:"   → "ctid = '(<blk>,<off>)'::tid"
    if key starts with "S:"   → NULL  (纯 INSERT，MAIN 无对应行，跳过定位)
    else (PK JSON)            → 现状 ob_build_pk_where_clause
```

冲突校验复用现有链路：`old_version` 比对（[branch_lifecycle.c:3409](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L3409) / [:3504](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L3504)）+ `apply_check_base_image_match`（[branch_lifecycle.c:3216](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L3216)，R18 HOT 二次校验）。**该链路已基于 ctid/xmin/`_base` 镜像，与 PK 解耦，可直接复用。**

> 注：R18 描述的 `_base_ctid` + `_base_xmin` `heap_fetch(SnapshotAny)` 方案，与 gammadb 原型的 `probe_resolve_scan`/`heap_get_latest_tid`（版本链解析）思路一致；若需要对陈旧 ctid 沿版本链找回最新行，可在此处引入 `heap_get_latest_tid`。**MVP 先不引入版本链解析**（§6）。

### 3.5 pure-delta 身份传播（关键实现点）

pure-delta 后处理（[write_redirect.c:1053-1708](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c#L1053-L1708)）在 PK 模式下靠"重建行 → 读回 PK 值 → 重新生成 key"来保持幂等。keyless 重建出的 slot **没有可用的 ctid**（`tts_tid` 为无效），因此：

> **要求**：pure-delta 循环在处理一条 delta 行（原始 key 已知）时，写回的新 delta 必须**沿用原始 `key`**，不得重新派生。即身份随 `DeltaTuple` 一起在循环内传递。

这是 keyless 与 PK 路径最主要的实现差异。若实现时不注意，同一逻辑行的多次 UPSERT 会各生成新 key → 读时出现重复行。

> **实现澄清**：pure-delta 后处理循环仅在 `oc_action != ONCONFLICT_NONE`（即 UPSERT）时进入（[write_redirect.c:1851](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c#L1851)），而 keyless + UPSERT 已在更早处被 `0A000` 拒绝（[write_redirect.c:1791](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c#L1791)）。因此在**本 MVP 中该循环对 keyless 表不可达**；`cand_inserts` 携带原始 key 的改动属于保持一致性/防御性实现，不构成 keyless 的必要路径。

### 3.6 锚点失效的触发条件（必须显式声明）

锚点有效 ⟺ "锚点 ctid 仍指向 MAIN 上**同一逻辑行**的**同一物理版本**"。分支生命周期内 MAIN 表**不被分支自身写入**（不变量 1），但**其它会话**仍可对其做 live UPDATE/DELETE，从而打破该等价关系。触发条件与两侧表现：

| 触发 | 机制 | 读侧（分支内 SELECT） | apply 侧 |
|------|------|------------------------|----------|
| 并发 UPDATE 锚定行 | 新版本落在**新 ctid**，旧 ctid 变死元组 | 旧 ctid 不再被 seqscan 看见 → delta 不匹配 → **静默丢 delta**（Pass 2 丢弃孤儿 `A:*`） | 定位 ctid 无可见行 → `0A000`（`MAIN row missing`） |
| 并发 DELETE 锚定行 | ctid 不可见 | 同上 | 同上 |
| **ctid 复用**（VACUUM 回收旧槽位后被新 INSERT 占用） | 旧槽位置 `LP_UNUSED` → 新行落在**同一 ctid** | 新行与旧 delta **误匹配** → 把旧行改动套在新行上 = **静默错读** | `old_version` 的 `xmin` 分量不匹配 → `0A000`（`conflict ... old_version expected=...`） |
| 表整体 rewrite（`VACUUM FULL` / `CLUSTER` / 重建型 `REINDEX` / `TRUNCATE` 后重填） | 全表元组获得新 ctid | 全表 anchor 一并失配 → **静默丢 delta** | 逐行 `0A000` |

> **读侧没有 `xmin` 校验**：[branch_scan.c](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L568-L596) 仅由 `tts_tid` 生成 `A:<blk>:<off>` 并做**等值匹配**，不解 `old_version`。因此第 3 行（ctid 复用）在读侧表现为**静默错读**，只有 apply 的 `old_version` 比对能发现它。前两行与第 4 行是"静默丢 delta"，第 3 行是"静默错读"。

活动分支期间，rewrite 类操作被 §A7 DDL 门禁拦截（[internals.md §4.3](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/internals.md#L235)）；但分支**非活跃窗口**（`use_branch(NULL)` 之后、`apply` 之前）无法拦截，只能靠 apply 的 `old_version` 兜底。

因此本特性必须：
1. 进入分支时输出 §2.3 的 WARNING；
2. apply 时能**确定性报错**（锚点行定位失败或 `old_version` 不匹配 → 走 [overlay_guard_ereport_fail](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/overlay_branch.c#L708)，SQLSTATE `0A000`）；
3. 读侧检测"孤儿 anchored delta"：**已实现**——Pass 2 丢弃未匹配的 `A:*` 锚点（`op='U'/'D'` 孤儿不复活），见 [branch_scan.c Pass 2](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L745)。**不额外发 WARNING**（避免读放大）。

### 3.7 能否降低"偶然"锚点失效的概率？（以 autovacuum 为例）

**普通 VACUUM / autovacuum 本身不会让锚点失效。** 它不移动活元组——只要锚定的 MAIN 行未被其它会话改动，其 ctid 恒定，anchor 继续有效。autovacuum 的唯一作用是**回收死元组的 line pointer（置 `LP_UNUSED`）**，从而使后续 INSERT 能复用该 ctid（§3.6 第 3 行）。故：

> 失效的**根因是"其它会话的并发 DML"**；autovacuum 只是把"并发 DML 已造成的静默丢失"进一步升级为"可能的静默错读"。没有并发 DML 时，VACUUM 刷多少遍都不影响已锚定且未改动的行。

（实现补充：`INSERT` 复用某个 ctid 的前提是该 line pointer 已是 `LP_UNUSED`，而 `LP_UNUSED` **只由 VACUUM 设置**；HOT 剪枝只把根 line pointer 标记为 `LP_DEAD`，不释放偏移量。所以"ctid 复用"必然经过一次 VACUUM。）

**可行手段（按性价比排序）**：

| 手段 | 效果 | 代价 |
|------|------|------|
| 给表加 PRIMARY KEY | 彻底消除（回到 PK 路径） | 需有可唯一标识列 |
| 缩短分支生命周期（尽早 apply/discard） | 线性降低命中并发写的概率（blast radius ∝ 存活时间 × 表写入速率） | 使用方式约束 |
| 分支存续期间避免其它会话写该表（业务串行化，或手动 `LOCK TABLE ... IN SHARE MODE` 阻塞写者） | 消除"并发写"这一根因 | 牺牲写并发 |
| 该表 `ALTER TABLE ... SET (autovacuum_enabled = off)` | **只**掐断 §3.6 第 3 行的 ctid 复用路径 → 消除"静默错读" | 该表死元组持续累积（膨胀）；挡不住手动 `VACUUM`；**不改善**静默丢 delta |
| （可选代码增强）读侧 Pass 1 补 `old_version` 的 `xmin` 校验 | 把"ctid 复用 → 静默错读"变为"丢弃/报错" | 需改 `branch_scan.c`（当前无 xmin 校验，§8 未决） |

**不可行/已否决**：用 persistent replication slot 钉住 `OldestXmin` 可**物理阻止**死元组被回收，从根上消除 ctid 复用（方案 B），但代价是全集群 VACUUM 抑制 / HOT 失效 / 写放大（§4.3、§4.4），本版本不实现。

> 结论：autovacuum 不是"偶然失效"的根因，因此**不存在"关掉 autovacuum 就能保护锚点"的通用解法**——只能靠缩短窗口、串行化写入或加 PK 来降低概率；若要读侧也杜绝"静默错读"，唯一干净的做法是给读路径补 `xmin` 校验（可选增强）。

---

## 4. 持久化与保留策略（本设计最大的决策点）

### 4.1 为什么需要"保留"

ctid 锚点要跨越分支生命周期有效，需要保证两点：
1. **旧版本可解析**：MAIN 行被并发 UPDATE 后，锚点指向的旧元组仍能沿版本链找回逻辑行（`heap_get_latest_tid`，gammadb `probe_resolve_scan`）。
2. **旧元组不被回收**：否则第 1 点无对象可解析。

第 2 点需要 VACUUM 不回收锚点元组，即**全局 `OldestXmin` 被钉住**。

### 4.2 方案 A：best-effort，无 slot（**推荐 MVP**）

不引入任何保留机制，接受 §3.6 的脆弱性：并发 MAIN 写可能使锚点失效，**在 apply 时以冲突错误（`0A000`）干净报错**。分支生命周期通常很短，blast radius 可控，且**不引入集群级副作用**。

- 优点：改动面小、无集群级代价、默认关闭时对现网零影响。
- 缺点：并发 MAIN 写下可能静默丢 delta（读侧），仅 apply 报错兜底。

**apply 报错示例**（无 PK 表；锚点行被并发改写）：

```sql
-- s1：分支内对 MAIN 的 r1 建锚点，delta key = "A:<blk>:<off>"
SET overlay_branch.keyless_tables = on;
SELECT overlay_branch.create_branch('b1');
SELECT overlay_branch.use_branch('b1');
UPDATE kl SET v = 'r1-branch' WHERE v = 'r1';   -- 锚点 A:0:1
SELECT overlay_branch.use_branch(NULL);

-- s2（另一会话）：并发改写同一 MAIN 行 → 新元组落在新 ctid
--    UPDATE kl SET v = 'RACE-MAIN' WHERE v = 'r1';

-- s1：apply → 锚点 ctid 在 MAIN 上已无对应行
SELECT overlay_branch.apply_branch('b1');
```

```
ERROR:  overlay_branch: cannot apply_branch (conflict) relation  inside active branch "<unknown>": UPDATE pk=A:0:1 MAIN row missing at apply time (concurrent DELETE)
DETAIL:  Target relation: kl.
HINT:  Switch back to the MAIN database before running this statement:
    SELECT overlay_branch.discard_branch('');
  or
    RESET overlay_branch.current;
```

`SQLSTATE = 0A000`（`ERRCODE_FEATURE_NOT_SUPPORTED`）。apply 失败后分支仍为 `state='active'`，s2 已提交的 MAIN 值（`RACE-MAIN`）原样保留——**报错而非静默写错**。

> 注：`relation ` 后为空、分支名为 `<unknown>` 是当前 `overlay_guard_ereport_fail` 的措辞——目标关系名由 `DETAIL` 行给出，且 apply 在分支模式外执行，故 `overlay_branch_get_current_name()` 返回空；`pk=A:0:1` 即失效的 ctid 锚点。`(concurrent DELETE)` 是固定文案，覆盖 UPDATE 移位 / DELETE 两类失效。

### 4.3 方案 B：persistent replication slot 保留（gammadb 原型）

创建 `RS_PERSISTENT` slot 并将 `MyReplicationSlot->data.xmin` 置为分支创建时的 horizon，从而把该 xmin 纳入集群 `OldestXmin`，**阻止**锚点旧元组被回收。代价：

| 现象 | 机制 |
|------|------|
| **集群级 VACUUM 抑制** | slot 的 data xmin 参与全局 `OldestXmin` → 所有库、所有表中 `xmin > H` 的死元组无法回收 → 表膨胀。**是"回收不掉"，不是禁止用户 VACUUM** |
| **HOT 失效** | 页内无空闲空间 → UPDATE 无法同页放新元组 → HOT 退化为普通 UPDATE（新建索引条目） |

**E03 实测（gammadb，规模化数据）**：

| 指标 | 无 slot | 有 slot | 倍数 |
|------|---------|---------|------|
| MAIN UPDATE 耗时 | 1607 ms | 7755 ms | 4.83× |
| VACUUM 耗时 | 132 ms | 621 ms | — |
| heap 大小 | 52 MB | 90.6 MB | 1.74× |
| HOT 比例 | 98.59% | 41.26% | — |
| WAL 生成 | 26.7 MB | 88.1 MB | 3.30× |

> 结论：方案 B 影响**全集群**，不宜作为默认路径。若采用，必须 (a) 独立 GUC 二次开启、(b) 纳入 `max_slot_hold` TTL 自动释放（参照 snapshot 模式的 `overlay_branch.snapshot_max_hold_minutes`）。

### 4.4 结论

**本版本只做方案 A**：GUC 关闭 → 现状报错；GUC 开启 → WARNING + best-effort ctid。

方案 B（persistent slot）会带来**全集群级副作用**（VACUUM 抑制 / HOT 失效 / 写放大），**本版本明确不实现**：不提供任何开关，故无需在代码中额外报错（无触发入口）。若未来引入，须作为**独立 GUC** 二次开启，并纳入 TTL 自动释放（参照 `overlay_branch.snapshot_max_hold_minutes`）。

---

## 5. 代码挂载点清单

| # | 位置（文件 : 函数） | 现状 | keyless 改造 |
|---|---------------------|------|--------------|
| 1 | [overlay_branch.c : `_PG_init`](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/overlay_branch.c#L274-L283) | 注册 `overlay_branch.enabled` 等 | 新增 `overlay_branch.keyless_tables` GUC + 全局变量 |
| 2 | [overlay_branch.h](file:///home/ubuntu/work/postgres/contrib/overlay_branch/include/overlay_branch.h#L197-L209) | GUC extern 声明 | 加 `extern bool overlay_keyless_tables;` |
| 3 | [overlay_branch.c : `overlay_guard_rel_ok`](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/overlay_branch.c#L804-L808) | G5：无 PK → 报错 | `if (!has_pk && !overlay_keyless_tables) → 报错`；否则放行 |
| 4 | [delta_store.c : `overlay_should_redirect`](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L1209-L1211) | 无 PK → return false | `if (!has_pk && !overlay_keyless_tables) return false;` |
| 5 | [delta_store.c : `overlay_serialize_pk`](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L826) 调用点（[write_redirect.c](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c#L995) 等 4 处） | 统一 PK 序列化 | 新增 `overlay_serialize_identity(rel, slot, op)`，按 has_pk 分派（§3.2） |
| 6 | [delta_store.c : `overlay_tuple_version`](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L1094) | `blk:off-x<xmin>` | 复用，无需改 |
| 7 | [branch_scan.c : `ob_compute_overlay_slots_internal`](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L139-L795) | PK 提取 + typed_pk_cmp | main identity 改 ctid；新增 anchor 比较器；Pass 2 处理孤儿 |
| 8 | [branch_scan.c : planner hook](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L1022-L1028) | 无 PK → return；PK fast-path | keyless 时继续注入并跳过 P0 fast-path |
| 9 | [branch_lifecycle.c : `ob_build_pk_where_clause`](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L2852) | apply 用 PK 定位 | 新增 `ob_build_identity_where_clause`（`A:`→ctid，`S:`→跳过，PK→现状） |
| 10 | [branch_lifecycle.c : apply old_version/base-image 校验](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L3409) | ctid/xmin + `_base` | 复用，无需改 |
| 11 | [write_redirect.c : pure-delta 循环](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c#L1053-L1708) | 重建行后重新派 PK | **改为沿用原始 key**（§3.5） |
| 12 | [write_redirect.c : UPSERT 4-way](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c#L1727-L2376) | PK Phase I/II 冲突检测 | keyless 需改 anchor 检测；**MVP 不支持 keyless+UPSERT（`0A000`，§6/§8）** |
| 13 | WARNING 埋点 | — | 写重定向首次命中 / `use_branch` 首次扫描，按 `(session, relid)` 去重 |

> 实现落地说明：以上 13 项均已落地。其中 #11（pure-delta 沿用原始 key）通过 `cand_inserts` 携带原始 key 实现；
> #12 keyless + UPSERT 在 [write_redirect.c](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c#L1791) 直接 `0A000` 拒绝；
> #13 WARNING 在 [overlay_should_redirect](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L1365) 命中后发出（写路径，非 planner hook）。

无改动的部分：递归 guard（[internals.md §4.1](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/internals.md)）、GUC check/assign 分离（§4.2）、advisory lock / CAS（§3.1）、registry 指纹（§4.12）、DDL guard（§4.3）。

---

## 6. MVP 范围与不支持项

**支持**：无 PK 表的 SELECT / INSERT / UPDATE / DELETE（best-effort 身份，§4.2 方案 A）。

**不支持（复用现有 `0A000` 报错风格）**：
- keyless + `INSERT ... ON CONFLICT`（UPSERT 4-way 依赖 PK）→ `0A000`。
- keyless + 分区表 / 外键 / MERGE / COPY FROM / DML-CTE（沿用现有 MVP 限制）。
- keyless + 并发 MAIN 写的一致性保证（仅 apply 报错兜底，§3.6）。
- 读侧 `xmin` 校验（ctid 复用导致的"静默错读"不被读路径发现，仅 apply 兜底，§3.6/§3.7）。
- 陈旧 ctid 的版本链解析（`heap_get_latest_tid`）→ 列为后续增强。

---

## 7. 测试计划

- **L3 回归**：新增 `test/regress/sql/overlay_branch_keyless.sql`（~独立模块，文件头写用途/覆盖/依赖），覆盖：
  - GUC off → 无 PK 表报错（现状回归）
  - GUC on → WARNING + INSERT/UPDATE/DELETE + BranchScan 读写一致
  - GUC on → pure-delta 多次 UPDATE 幂等（验证 §3.5 key 传播）
  - GUC on → apply 成功 / 并发改 MAIN 后 apply `0A000`（冲突）
  - 有 PK 表在 GUC on 下仍走 PK 路径（回归）
- **L1 隔离**：`keyless_basic.spec`、`keyless_pure_delta_repeat.spec`、`keyless_apply_conflict.spec`（**已实现**，覆盖 GUC on WARNING + 读写叠加 + pure-delta 合成键幂等 + 跨会话 apply 冲突路径；与 L3 Part D 同会话时序互补）。
- **不变式断言**：PK 表行为逐字节不变（diff 空）。

---

## 8. 已决策项（原开放问题）

1. **保留策略**：采用**方案 A**（无 slot，best-effort）。方案 B（persistent slot）因集群级 VACUUM 抑制 / HOT 失效代价**本版本不实现**（§4.4）。
2. **keyless + UPSERT**：**已拒绝**（`0A000`，`write_redirect.c`）。
3. **读侧孤儿检测**：**已实现**——BranchScan Pass 2 丢弃未匹配的 `A:*` 锚点（静默丢弃，不报 WARNING）。
4. **GUC 名称**：采用 `overlay_branch.keyless_tables`。
5. **key 前缀**：采用 `A:` / `S:` tag（与 PK JSON 数组不冲突）。

> **未决（后续）**：
> 1. 是否引入 `heap_get_latest_tid`（gammadb 做法）解析陈旧 ctid 版本链，以缓解 §3.6 的"静默丢 delta"。
> 2. 是否给读侧 Pass 1 补 `old_version` 的 `xmin` 校验，以消除 ctid 复用导致的"静默错读"（§3.6/§3.7）。
>
> 两者本版本均不做。

---

## 9. 文档同步清单（编码完成后）

- [x] [internals.md](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/internals.md)：§1.1「无 PK 表直接拒绝」、§4.3 G5、§5 不变量 2 已补 keyless 分支说明。
- [x] [README.md](file:///home/ubuntu/work/postgres/contrib/overlay_branch/README.md)：MVP 限制补 GUC 例外 + 实验特性条目。
- [x] [example_sql.md](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/example_sql.md)：§5 报错表/§5.3 矩阵更新，§6.8 新增 keyless 行为（仅 SQL + 行为描述）。
- [x] [AGENTS.md](file:///home/ubuntu/work/postgres/contrib/overlay_branch/AGENTS.md)：§1 Managed tables（keyless 例外 + GUC）+ §7 文档索引（keyless_ctid_design）。
- [x] [doc/test_coverage_index.md](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/test_coverage_index.md)：新增 3 条 L1 keyless 条目（26 entries: 14 L3 + 12 L1）。
- [x] L1 基线：`keyless_basic.spec` / `keyless_pure_delta_repeat.spec` / `keyless_apply_conflict.spec` 已注册 Makefile `ISOLATION=` + `isolation_schedule`，expected 已生成，L1 12/12 PASS 0 diff。
