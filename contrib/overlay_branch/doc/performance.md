# Overlay Branch 性能设计

> 本文档只谈性能：性能模型、已识别瓶颈、可复现基线方法、优化路线图与度量陷阱。
> 功能语义 / 正确性见 [design.md](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/design.md)、
> [internals.md](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/internals.md)、
> [branchscan.md](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/branchscan.md)、
> [multi_session_mvcc.md](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/multi_session_mvcc.md)。

---

## 0. 范围与原则

覆盖四个面：**读路径（BranchScan 2-pass 叠加）**、**写路径（DML 重定向到 delta）**、
**apply（3-pass 回灌 MAIN）**、**delta 体积**。

原则：
1. **正确性优先于性能**。任何优化不得改变 L1/L3 语义，必须先有基线数字再动手。
2. **先度量后优化**。没有可复现基线就不改热点代码。
3. 性能不是门禁。L2 基线输出只用于对照与回归比较，不做 PASS/FAIL 判定。

---

## 1. 性能模型

### 1.1 记号

| 符号 | 含义 |
|---|---|
| `N` | MAIN 表行数 |
| `M` | 分支 delta 行数 |
| `A` | 表的用户列数 |
| `S` | 单行平均字节数 |

### 1.2 结构性事实（决定一切）

- delta 的物理键是 `PRIMARY KEY (branch_id, relid, key)`
  （[overlay_branch--1.0.sql](file:///home/ubuntu/work/postgres/contrib/overlay_branch/overlay_branch--1.0.sql#L41-L55)），
  且所有写路径最终都落到
  [overlay_delta_insert](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L196-L219)
  的 `ON CONFLICT ... DO UPDATE`。
  → **`M` = 分支内被触碰过的「不同行数」，与操作次数无关**。同一行改 1000 次仍只占 1 行。
- 由此推论：`branch_scan.c` 里整套 “latest-wins 多 op 折叠（I→U→D 链）” 逻辑
  在当前 schema 下**不可达**，是纯防御性代码；`M_unique == M` 恒成立。
- 单 delta 行净重远大于一行 heap tuple：
  `key` 是 PK 的 JSON 文本；`tuple_data` 是整行 JSON；且 UPDATE/DELETE 会再把旧值作为
  `_base` 塞进同一 blob（[overlay_modify_update](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L1491-L1498)、
  [overlay_modify_delete](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L1516-L1530)）。
  → **U/D 行 ≈ 2× 整行 JSON**，外加 bytea/hex 文字化开销。

### 1.3 路径成本

| 路径 | 当前形态 | 复杂度（当前） | 目标 |
|---|---|---|---|
| 读：delta 加载 | `list_for_rel` 走 `pg_branch_delta_relid_idx`；P0 命中时 `lookup` 走 PK 索引 | O(M) / O(1) | 不变 |
| 读：delta 排序 | `qsort` + 逐比较重建 btree 算子 | O(M log M) × syscache | O(M log M)，算子缓存 |
| 读：去重 | 逐 run 线性回查原始下标 | **O(M²)** | O(M)，或直接删除（结构性不可达） |
| 读：Pass1 匹配 | `bsearch` + 每 MAIN 行重建 TypedKey | O(N log M) × relcache/syscache | 同阶但常数大幅下降 |
| 读：Pass2 | 逐未命中 delta 全表线性扫 | **O(U × M)**，`U` = pure-delta 行数 | O(M) |
| 读：物化 | 首次取行即整结果集物化进 per-scan 内存上下文 | O(N + M) 内存，无早期终止 | 按需/流式（视需要） |
| 写：每 DML 行 | state 查询 + `fetch_tuple_by_ctid` + `tuple_version` + UPSERT | 3~4 次 SPI | 减少 SPI 次数 |
| apply：每 delta 行 | fetch + version + base_image 探测 + base_image 比对 + DML | **4~5 次 SPI** | 集合式/批量化 |
| apply：遍历次数 | D→U→I 各完整遍历一遍 delta list | 3 × O(M) | 单次分桶或批处理 |

---

## 2. 已识别瓶颈清单

编号仅用于本文档内引用。`RD-*/WR-*/AP-*/DL-*` 来自静态代码分析；
`MF-*` 来自首轮 harness 实测（见 §2.1），其严重性由数字而非直觉排序。

### 2.1 首轮实测结论（优先处理）

环境：PG 17.11，`max_parallel_workers_per_gather=0`（**必须**，原因见 MF-2），
`public.ob_perf_base(id PK, grp, payload text)`，`N=200000, M=20000`，串行。

| 实测项 | 耗时 | 对照 |
|---|---|---|
| M1 MAIN 原生 `count(*)` | 8.6 ms | — |
| M2 分支 `count(*)`，**M=0** | 3829 ms | **比 M1 慢 ~440×；此时完全没有 delta** |
| M3 分支 `count(*)`，M=20000 | 6737 ms | 相对 M2 增量仅 +76% |
| M4 分支 PK 等值读 | 50 ms（受泄漏影响，冷态 0.2–0.5 ms） | MAIN 模式同查询 0.5 ms |
| M5 分支非 PK（`grp=7`）| 14292 ms | **比 M3 还慢 ~2×** |
| M8 `apply_branch()` | 2544 ms | ~127 µs/delta 行 |
| M7 delta 体积 | 20000 行 / 2097 kB | MAIN 28 MB，`amplification=0.072` |

**结论：当前读路径的瓶颈不是 delta 大小，而是与 `M` 无关的每行恒定开销 + 会话级内存泄漏。**
`M=0` 的空分支已比原生扫描慢两个数量级，说明 2-pass 合并本身的常数因子才是主要矛盾。

（**后注**：此句最初把“恒定开销”归因于**常数项**，阶段 1 又一度归因于**架构**
——SPI 全量物化 + 结果集全量物化。阶段 1b 的隔离实验纠正了这两个说法：真正的量级主因是
Pass1 **每 MAIN 行一次 `overlay_serialize_pk()`**（一次 `to_jsonb` SPI，≈8.4 µs/行），见 §5 阶段 1b。）

> **阶段 0 状态**：`MF-2` / `MF-3` / `MF-1` / `MF-5` 已修复并有回归覆盖（见 §5）。
> 上表 M2/M3/M4/M5 是**修复前**的数字，保留作为基线对照；修复后需重跑 §4 基线更新。
> `MF-4`（每行恒定开销）**已由 `RD-8` 关闭**：根因是每 MAIN 行的 `overlay_serialize_pk()`
> SPI 调用，`RD-8` 在无 delta / delta 全 typed 时跳过它，M=0 全扫 2067 → ~123 ms（≈15×）。

- **MF-1｜`TopMemoryContext` 无界泄漏（最高优先）— 已修复。**
  实测单次全量叠加使后端内存 `1.8 MB → 394 MB`，连续 3 次 `→ 746 MB → 1098 MB`
  （≈ **352 MB / 次**），`discard_branch()` 也**不**回收。
  含义：长会话必然 OOM；且泄漏后所有查询变慢（PK 点查 0.2 ms → 50 ms）。
  用 `pg_backend_memory_contexts` 逐层二分量出**两个相互独立**的泄漏，二者叠加才解释全部增量：
  1. **物化上下文（slot + tupdesc）。** 整段物化运行在 `TopMemoryContext`，且每 MAIN 行
     `CreateTupleDescCopy(reldesc)` 的 tupdesc 无人释放 —— `ExecDropSingleTupleTableSlot`
     只 pfree slot，`ReleaseTupleDesc` 对 refcount=-1 的拷贝是 no-op；
     R13 清理（[L828-L872](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L828-L872)）
     只覆盖 `delta_list`。仅修此项**数值不变**，说明它不是量级主因。
  2. **delta 侧 `TextDatumGetCString(datumCopy(...))` 中间副本（真正的量级主因）。**
     `overlay_delta_lookup` 与 `overlay_delta_list_for_rel` 共 4 处先 `datumCopy()` 造临时
     varlena 再交给 `TextDatumGetCString()`；后者本身就在当前 context 返回新 palloc 的 cstring，
     于是那个 `datumCopy` 副本**每 delta 行泄漏一次**。缩放实验证明泄漏量与 `M` 成正比、与 `N` 无关：
     `S0_200rows_0delta: grew=0`、`S2_200rows_200delta: grew=131072`、
     `B0_2000rows_0delta: grew=0`、`B2_2000rows_200delta: grew=524288`。
  修复：slot/tupdesc 迁到 per-scan 内存上下文（`begin` 建、`Rescan` reset、`EndCustomScan` 删），
  delta 侧去掉 4 处中间 `datumCopy`，并在 R13 清理补 `pfree(dt->old_version)`。
  验证：全部场景 `grew=0`（[overlay_branch_memgrow.sql](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_memgrow.sql)）。
- **MF-2｜并行路径绕过 overlay（潜在静默错读）— 已修复。**
  分支模式下只要 `max_parallel_workers_per_gather>0` 且表足够大，计划就变成
  `Parallel Seq Scan on ob_perf_base` —— **完全不含 BranchScan，delta 被忽略**。
  根因：[L1538-L1555](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L1538-L1555)
  只从 `rel->pathlist` 剔除其它路径，**从未处理 `rel->partial_pathlist`**
  （core PG 在 `set_rel_pathlist()` 返回后 `generate_useful_gather_paths()` 会把
  `partial_pathlist` 转成 Gather 重新加回 `pathlist`）。
  回归测试因表小未触发并行，故一直未暴露。
  修复：hook 内同时 `rel->partial_pathlist = NIL; rel->consider_parallel = false;`，
  并补 [overlay_branch_parallel.sql](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_parallel.sql)
  用例（控制组出 Gather、分支内只有 branchscan 且无 Gather）。
- **MF-3｜分支模式并行查询硬报错（掩盖 MF-2）— 随 MF-2 修复消失。**
  上述 `Parallel Seq Scan` 之后，worker 恢复 GUC `overlay_branch.current` 时 on-assign
  hook 需要 SPI 快照，直接失败：
  `cannot execute SQL without an outer snapshot or portal ... while setting parameter "overlay_branch.current"`。
  即“并行 + 分支”曾是**必失败**。好消息：它挡住了 MF-2 的静默错读。
  修复 MF-2 后分支 rel 不再产生并行路径，该报错路径自然不可达。
- **MF-4｜每行恒定开销 ~10 µs（M=0 也成立）— 已修复（根因：每行一次 `overlay_serialize_pk` SPI）。**
  `RD-3/4/5` 只消掉 ~3%（M2 2123 → 2067 ms）；真正主因是 Pass1 对**每个** MAIN 行调用
  `overlay_serialize_pk()`，其内部是一条 `to_jsonb(ARRAY[...])` 的 SPI parse+plan+exec（≈8.4 µs/行）。
  决定性隔离实验：同规模 20 万行、M=0 全表扫描，
  **有 PK 表 1800 ms vs 无 PK 表（走 ctid，根本不调 `overlay_serialize_pk`）121 ms**；
  且 `work_mem=512MB` 对 PK 表无改善（1811 vs 1765 ms）→ 排除 tuplestore 落盘/物化。
  `RD-8` 在 `M_total == 0` 或全部 delta 键都有 typed PK（`delta_all_typed`）时跳过该序列化，
  仅用纯 C 的 typed PK 比较键；修复后 M=0 2067 → ~123 ms（≈15×），M=20000 全量叠加 2474 → ~545 ms（≈4.5×）。
  （**勘误**：§5 阶段 1 曾据一次“临时短路 `serialize_pk` 无改善”的隔离实验判其为架构性问题；
  该实验结论**不成立**——极可能因运行中的 postmaster 仍映射旧 `.so`：`overlay_branch` 在
  `shared_preload_libraries` 中，postmaster 启动时 `dlopen`，替换磁盘上的 `.so` **不影响**已运行后端。
  详见 §5 阶段 1b。）
- **MF-5｜非 PK 下推路径比“不下推”更慢 2–4× — 已修复（根因即 RD-1/RD-2）。**
  `grp=7` 下推后（一般 WHERE 走 `general_where_sql`）耗时 14292 ms，
  反而高于不加限定的 M3（6737 ms）。专项实测（`mf5_base`，`grp = id % 100`，`N=200000, M=20000`，
  同一分支同一 delta，只变 P2 限定）：

  | P2 限定 | MAIN 命中行 | 修复前 | 修复后 |
  |---|---|---|---|
  | 无 | 200000 | 3780 ms | 2763 ms |
  | `grp < 100`（命中全部） | 200000 | 3600 ms | 2533 ms |
  | `grp < 10` | 20000 | 12697 ms | 675 ms |
  | `grp = 7` | 2000 | 14303 ms | 479 ms |

  即：偏差不在 MAIN 侧扫描（`grp=7` 的 MAIN 原生 seqscan 反而更快，6.6 ms vs 8.6 ms），
  也不是“带了一般 WHERE”本身（`grp < 100` 与不下推几乎同速）。
  根因是 **P2 下推缩小了 Pass1 的命中集**：被推掉的 MAIN 行对应的 delta
  （UPDATE/DELETE/INSERT 覆盖行）在 Pass1 不再被标记 `emitted`，全部落入 **Pass2 孤儿路径**，
  而 Pass2 Phase 2a 是 **O(ndelta × U_unemitted)**、每次比较还走 `overlay_typed_pk_cmp` 的 syscache
  （即 RD-2）。下推越“有效”（MAIN 命中越少），`U` 越大、Pass2 越爆。
  实测 `U` 从 ~5000（不下推，仅剩 pure-INSERT）涨到 ~19850（`grp=7`），
  估算 Pass2 比较量 1e8 → 4e8，与耗时 3.8× 吻合。
  结论：**MF-5 不是独立缺陷，而是 RD-1/RD-2 在“非 PK 下推”下的放大表现**；
  删除 RD-1/RD-2 后即同时消掉 MF-5（`grp=7` 14303 → 479 ms，约 30×），下推逻辑本身未改。
  回归用例见 [overlay_branch_pushdown.sql](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_pushdown.sql)：
  覆盖 Pass1 命中集从“满命中”到“零命中”（每个 delta 都成孤儿）的正确性，以及 key 结构性唯一这一前提。

### 读路径

- **RD-1｜去重 O(M²)，恒定触发 — 已删除。**
  排序后按 run 折叠，但为了取 “最大原始下标” 对**每个**元素做了一次
  `foreach(delta_list)` 线性扫描。因 key 结构上唯一，run 恒为 1，于是每个元素都付 O(M)，
  总计 O(M²)。这是注释里 “O(M log M)” 说法不成立的原因。已删除 post-sort 折叠
  （`qsort` 保留，bsearch 仍需有序数组）。
- **RD-2｜Pass2 去重同为 O(M²) — 已删除。**
  Phase 2a 对每个未命中 delta 做一次全量 `foreach`，内层无 early-break。
  代价 O(U × M)；纯 delta 写入多的分支 `U ≈ M` → O(M²)。
  已退化为 O(M) 单遍标记：未标记 `emitted` 的 INSERT/UPDATE 必为该 key 最终后像，直接 emit。
- **RD-3｜行循环内重建 slot 与 TupleDesc — 已修复（见 §5）。**
  [branch_scan.c L579-L580](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L579-L580)：
  `CreateTupleDescCopy` + `MakeSingleTupleTableSlot` 位于 per-MAIN-row 循环内 → N 份 tupdesc 拷贝。
- **RD-4｜每 MAIN 行重建 TypedKey，伴随 relcache/index 操作 — 已修复（见 §5）。**
  [overlay_build_typed_pk_from_slot](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L1900-L1999)：
  每行 `RelationGetIndexList` + `index_open`/`index_close` 找 PK 索引，再逐列 `get_typlen/byval`。
  delta 侧 pre-flight 走同源的 `_from_key_text`。
- **RD-5｜比较器逐次比较重查 syscache — 已修复（见 §5）。**
  [overlay_typed_pk_cmp](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L2220-L2265)：
  每次比较都 `get_sort_group_operators` + 2×`SearchSysCache(OPEROID)`。
  `TypedKey` 结构（[overlay_branch.h L164-L173](file:///home/ubuntu/work/postgres/contrib/overlay_branch/include/overlay_branch.h#L164-L173)）
  **没有缓存 `FmgrInfo`**，与 AGENTS.md §Identity 描述不一致。
- **RD-6｜全量物化、无早期终止 — 已修复（见 §5 阶段 2）。**
  [ob_branchscan_begin 注释](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L1759-L1773)
  + [ob_branchscan_exec](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L1797)：
  首次取行即把整表结果物化进 `TopMemoryContext` 链；`LIMIT` / 游标 / `EXPLAIN ANALYZE` 均无法截断。
- **RD-7｜MAIN 侧未下推时是 SPI seqscan — 已修复（见 §5 阶段 2）。**
  MAIN 基线读取已去 SPI（`table_beginscan` 直扫 + `baserestrictinfo` prefilter）；
  P0 命中时仍走 SPI，且谓词识别面已从**单列等值** `pk = const` 放宽到
  PK 列的 btree 五算子 + `IN` / `= ANY(...)` + `AND`/`OR` 组合（Part E）。
  注：P0 的 SPI 查询由真实 planner 规划（helper guard 抑制 hook），故可用 PK 索引。
- **RD-8｜Pass1 每 MAIN 行一次 `overlay_serialize_pk()` SPI — 已修复（见 §5 阶段 1b）。**
  [overlay_serialize_pk](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L1236)
  为每行构造 PK 的 JSON 文本键，内部是一次 `to_jsonb(ARRAY[...])` SPI（parse+plan+exec，≈8.4 µs/行）。
  该文本键仅在 typed PK 不可用（strcmp 回退）时才需要；`M=0` 时完全不需要。
  修复：`M_total == 0` 时不做任何键工作；`delta_all_typed` 时只建纯 C 的 typed PK、跳过文本键。
  这是 `MF-4` 的量级主因。

### 写路径

- **WR-1｜每 DML 行多次 SPI，无批量。**
  [overlay_delta_insert](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L95-L224)
  每次先查一次 branch state，再 UPSERT；UPDATE/DELETE 还要额外
  [fetch_tuple_by_ctid](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L617)
  与 [overlay_tuple_version](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L1249)。

### apply

- **AP-1｜每 delta 行 4~5 次 SPI。**
  [apply_internal](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L2507-L2551)
  每行：`ob_fetch_main_current_slot` + `overlay_tuple_version`
  + [apply_check_base_image_match 的两次探测](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L3288-L3349)
  （先 `? '_base'`，再 jsonb 规范化 + `FOR UPDATE`）+ 1 次实际 DML。全部为独立 parse+plan+exec。
- **AP-2｜持锁窗口 = 全量 apply 时长。**
  [L2350-L2358](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L2350-L2358)
  起持 `EXCLUSIVE` advisory lock，直到 Step 4 结束，单事务。apply 越慢，阻塞同分支操作、
  抬 vacuum horizon 的窗口越长。

### delta 体积

- **DL-1｜无体积可观测、无准入约束。** 目前既没有 delta 行数/字节数查询接口，
  也没有任何上限或 WARNING。最坏情形（分支内全表 `UPDATE`）delta ≈ 整表 ×2 的 JSON 体积。

---

## 3. Delta 体积：现状与可观测

### 3.1 结论

`M` 与操作次数无关（§1.2），所以 “delta 无限膨胀” 不成立；真正的风险是
**单分支触碰行数逼近整表** 与 **单行净重被 `_base` 翻倍**。

### 3.2 本轮范围：只做可观测

本轮**不**加任何硬/软上限，不改变写路径语义。只提供可复现的体积报告，
使 “这个分支有多重” 可被量化：

| 指标 | 口径 |
|---|---|
| `delta_rows` | `count(*)` on `(branch_id, relid)` |
| `delta_payload_bytes` | `sum(octet_length(key) + octet_length(coalesce(old_version,'')) + coalesce(octet_length(tuple_data),0))` |
| `delta_relation_bytes` | `pg_total_relation_size('pg_branch_delta')` |
| `main_bytes` | `pg_total_relation_size(relid)` |
| `amplification` | `delta_payload_bytes / main_bytes`（含 `_base` 翻倍效应） |

参考查询见 [test/bench/perf_baseline.sql](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/bench/perf_baseline.sql)。

### 3.3 后续（不在本轮）

产品化路径：把 §3.2 的口径固化为扩展视图 `overlay_branch.pg_branch_delta_stats`
（或 SRF），并在 `create_branch` / 写路径做 throttled WARNING。
任何 catalog 变更必须同步 L3 用例（Conv-1），因此单独作为一次增量交付。

---

## 4. 基线方法

### 4.1 harness

`test/bench/perf_baseline.sql` + `test/bench/run_perf_baseline.sh`：

```bash
cd contrib/overlay_branch
PGPORT=5432 ./test/bench/run_perf_baseline.sh 200000 20000      # N=20万, M=2万
```

harness 为 psql 驱动（非 pgbench），使用 `\timing` 与
`EXPLAIN (ANALYZE, BUFFERS)` 采集：

1. MAIN 模式全表 `count(*)`（对照基线）；
2. 进入分支后全表 `count(*)`（触发 BranchScan 2-pass 合并，核心读指标）；
3. 分支内 PK 等值读（P0 快路径）；
4. 分支内非 PK 条件读（P2 下推）；
5. `EXPLAIN (ANALYZE)` 确认 `overlay_branch_branchscan` 被选中及实际节点耗时；
6. `apply_branch()` 计时；
7. §3.2 的 delta 体积报告。

delta 由**集合式** DML 构造（`UPDATE ... WHERE id <= M`、`DELETE`、`INSERT ... generate_series`），
保证 `M` 可控且构造开销不污染测量。

### 4.2 基线记录

harness 输出重定向到 `test/bench/perf_baseline_<N>_<M>.log`。
优化前后用**同一** `N/M` 与同一机器对比；文档只保留一组参考数字（见 §2.1）。

---

## 5. 优化路线图

按 “收益 / 风险” 排序，每一项独立成增量。

### 阶段 0 —— 先止血（实测 MF-*，可与性能无关地独立交付）

- **`MF-2` + `MF-3`（已完成）**：在 planner hook 中同时清空 `rel->partial_pathlist`
  并置 `consider_parallel = false`，使分支模式**永不**生成 `Parallel Seq Scan`；
  MF-3 的硬报错随之消失，且堵住 MF-2 的静默错读。
  已补 **大表 + 默认并行参数** 的回归用例
  [overlay_branch_parallel.sql](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_parallel.sql)。
- **`MF-1`（已完成）**：物化分配离开 `TopMemoryContext`（迁到 per-scan 上下文，
  `begin` 建 / `Rescan` reset / `EndCustomScan` 删），并去掉 delta 侧 4 处
  `TextDatumGetCString(datumCopy(...))` 的中间副本（真正的量级主因，见 §2.1）。
  已补会话级内存用例
  [overlay_branch_memgrow.sql](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_memgrow.sql)。
- **`MF-5`（已完成）**：非 PK 下推反而更慢，不是独立缺陷，而是 **RD-2** 在
  “下推缩小 Pass1 命中集、逼大 Pass2 孤儿集 `U`”下的放大表现（见 §2.1 MF-5 实测表）。
  随 `RD-1`/`RD-2` 的删除一并消掉（`grp=7` 14303 → 479 ms，约 30×），下推逻辑未改。
  已补非 PK 下推 + Pass2 孤儿路径用例
  [overlay_branch_pushdown.sql](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_pushdown.sql)。

### 阶段 1 —— 读路径算法与常数（低风险，高收益）

- `RD-1` / `RD-2`（**已完成**）：消除两个 O(M²)。既然 key 结构性唯一，
  直接删掉去重与多 op 折叠，退化为 O(M) 单遍标记。
- `RD-3` / `RD-4` / `RD-5`（**已完成**）：见下。

**RD-3/4/5 实现要点**

- `RD-3`：Pass1 的 `CreateTupleDescCopy()` 由“每行一次”改为“整趟共享一份”。
  安全前提：`CreateTupleDescCopy` 产出的描述符 `tdrefcount == -1`（非引用计数），
  `MakeSingleTupleTableSlot`/`ExecDropSingleTupleTableSlot` 的
  `PinTupleDesc`/`ReleaseTupleDesc` 对它是 no-op，因此共享不会导致提前 free。
  同时把行循环内的 `get_typlenbyval()`（syscache 往返）换成属性自带的
  `att->attlen` / `att->attbyval`（`ALTER COLUMN TYPE` 会同步二者，见
  `ATExecAlterColumnType`）；吐行路径与 `reconstruct_slot_from_delta()` 同样处理。
- `RD-4`：新增 `ob_pk_rel_meta()`，按 `relid` 记忆化 PK 元数据
  （PK 列 attno / typid / typmod / collid / typlen / typbyval），
  由 `CacheRegisterRelcacheCallback` 在 relcache 失效时驱逐（`InvalidOid` = 全清）；
  所有 PK helper（含 `overlay_serialize_pk`）不再每行
  `RelationGetIndexList()` + `index_open()`。无 PK 表也缓存（`has_pk=false`），
  keyless 路径同样免走。
- `RD-5`：`TypedKey` 增加 `eq_finfo` / `lt_finfo`，由 `ob_pk_type_cmp_fmgrs()`
  按类型记忆化（`FmgrInfo` 常驻 `TopMemoryContext`，与 `TypedKey` 生命周期解耦，
  条目永不删除）；`overlay_typed_pk_cmp()` 的每次比较退化为两次
  `FunctionCall2Coll()`，删除每比较一次 `get_sort_group_operators()` +
  两次 `SearchSysCache(OPEROID)`。类型无 btree 排序算子时保留旧回退路径。

**实测（`N=200000, M=20000`，同机同 harness，对照 §4.2）**

| 段 | 场景 | RD-3/4/5 前 | 后 |
|---|---|---|---|
| M2 | 分支 `count(*)`，M=0 | 2123 ms | 2067 ms |
| M3 | 分支全量叠加 | 2499 ms | 2474 ms |
| M5 | 分支非 PK（`grp=7`） | 478 ms | 475 ms |

**结论：RD-3/4/5 消掉了真实的每行 catalog 开销，但对 M2 只有 ~3% 的改善。**
它们的价值在于消除 catalog 往返，为后续 typed 路径铺路，但**没有**触及 `MF-4` 的真正主因。

（**勘误**）本节曾据一次“临时短路 `overlay_serialize_pk()` + TypedKey 构造也只省 ~60 ms”的
隔离实验，判定 M2 的 ~10 µs/行 恒定开销**不是**常数项而是**架构性**（`RD-7` 全量物化 +
`RD-6` 结果集二次拷贝）。该结论**不成立**，见阶段 1b：主因恰恰是被短路的每行 `serialize_pk`。

验证：L3 17/17 + L1 12/12 零 diff；两次独立 clean rebuild 0 warning。

### 阶段 1b —— 读路径每行键开销（低风险，高收益）

- `RD-8`（**已完成**）：Pass1 对每个 MAIN 行调用 `overlay_serialize_pk()`
  （一条 `to_jsonb` SPI，≈8.4 µs/行）来构造 PK 的 JSON 文本键。
  该文本键只在 typed PK 不可用（strcmp 回退）时才需要：
  `M_total == 0`（delta 为空）时 Pass1 根本不做键工作；
  `delta_all_typed`（全部 delta 条目都有可用 typed PK）时只建纯 C 的 typed PK、跳过文本键。
  兜底：若某 MAIN 行的 typed PK 构建失败（`main_typed_pk == NULL`），仍回退构造文本键，
  以免该行 delta 副作用因无匹配键被静默丢弃。

**实测（`N=200000`，同机）**

| 场景 | `RD-8` 前 | 后 | 提升 |
|---|---|---|---|
| PK 表 M=0 全表 `count(*)`（`ob_pk2`）| ~1800–2067 ms | ~123 ms | ≈15× |
| PK 表 M=20000 全量叠加（`ob_perf_base`）| 2474 ms | ~545 ms | ≈4.5× |

隔离证据：无 PK 表（走 ctid，**不**调 `overlay_serialize_pk`）同规模 M=0 全扫仅 121 ms；
`work_mem=512MB` 对 PK 表无改善（1811 vs 1765 ms）→ 排除物化落盘因素。

**教训（排障）**：`overlay_branch` 位于 `shared_preload_libraries`，postmaster 启动时
`dlopen` 该 `.so`；**替换磁盘上的 `.so` 不影响已运行后端**。任何“改了代码却测不出差异”
的现象，先 `pg_ctl restart` 再复测，否则极易得出错误结论（阶段 1 的误判即疑似此因）。

验证：L3 17/17 + L1 12/12 零 diff；两次独立 clean rebuild 0 warning。
回归覆盖：[overlay_branch_scan_pk.sql](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_scan_pk.sql)
Section F（空 delta / all-typed delta 两种叠加结果与 MAIN、SRF 一致）、
Section G（PK / 列值含 `"` 与 `\` 时的 JSON 转义 + 叠加正确性）。

### 阶段 2 —— 读路径架构（中风险）

详见 [branchscan.md](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/branchscan.md) Part D。

- `RD-8` 收尾（**已完成**）：`overlay_serialize_pk()` 纯 C 化，去掉每行
  `to_jsonb(ARRAY[...])` SPI 往返（读 Pass1 strcmp 回退、写路径、apply 共用）。
- `RD-7`（**已完成**）：MAIN 基线读取去 SPI —— `table_beginscan` 直扫 +
  executor `baserestrictinfo` qual 预筛；`has_pk_pred`（P0 PK 谓词）保留
  SPI 以复用 planner 索引能力。见 branchscan.md §D.3。
- `RD-6`（**已完成**）：按需吐行 / 迭代器化（`OverlayScanState` + `overlay_scan_open/next/close`），
  使 `LIMIT` 与游标能早期终止；SRF 同步改为流式句柄。见 branchscan.md §D.4。
- delta 侧：结合 `LIMIT` 做早停预估。
- P0 谓词下推扩展（**已完成**）：L1 识别面从「单列等值」放宽到 PK 列的
  btree 五算子（`= < <= > >=`）+ `IN` / `= ANY(...)` + 其 `AND`/`OR` 组合，
  合并 deparse 成单段 SQL 透传同一个 MAIN SPI SELECT（自动获得索引扫描）；
  L2 DELTA 明确降级为「仅单点等值走 O(1)」，其余走全量列表（零新增代码）。
  见 branchscan.md Part E；断言见 `scan_pk` Section J。非 PK 条件继续走 P2。

### 阶段 3 —— apply（中高风险）

- `AP-1`：集合式 apply —— 按 op 分桶，用一条批处理 SQL 处理多行
  （如 `INSERT ... SELECT ... FROM jsonb_populate_record(NULL::t, ...)` 的数组展开），
  base_image 校验合并进同一次 fetch，减少每行 SPI 次数。
- `AP-2`：评估分块 apply 与持锁窗口的关系；需保证仍满足
  [multi_session_mvcc.md](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/multi_session_mvcc.md)
  的 CAS / 状态机不变量。

### 阶段 4 —— delta 体积

- 落地 §3.3 的 `pg_branch_delta_stats` 视图 + throttled WARNING。
- 视需要在 `apply` 前给出规模提示（仍是可观测，不是硬限制）。

---

## 6. 度量陷阱

- **必须用 `LC_ALL=C.utf8`** 起临时实例（见 AGENTS.md §3），否则 `make check` 基线会因
  多字节列宽产生假 diff。
- **必须关并行**：harness 里 `max_parallel_workers_per_gather=0` 曾不只是为了公平对照，
  也是**规避 MF-2/MF-3** 的必要条件（开并行时分支模式读到的根本不是 overlay）。
  MF-2 修复后分支 rel 已不产生并行路径，但跑基线仍建议关并行以获得稳定的串行对照。
- **必须测内存**：任何长基线都要在每步之间记录 `pg_backend_memory_contexts` 总量。
  MF-1 已修复，但同类“跨 SPI 存活对象误放 `TopMemoryContext`”的回归很容易再犯；
  不看内存就可能把泄漏误判成算法劣化（修复前的 PK 点查 0.2 ms → 50 ms 正是此例）。
- **首次 ExecCustomScan 会全量物化**：`count(*)` 与 `SELECT *` 的耗时几乎相同，
  这是 `RD-6` 的表现，不是测量误差。
- **计划缓存**：进入 / 离开分支后扩展会做 PlanCache 失效；跨分支切换测量时不要复用同一 PREPARE。
- **autovacuum 噪声**：delta 表在 apply/discard 后只做 `DELETE`，空间靠 autovacuum 回收；
  连续多轮基线之间应留出 vacuum 窗口或显式 `VACUUM`，否则 `pg_total_relation_size` 不可比。
- **不要用 pgbench 的 tps 直接比较**：现有 L2 fixture 面向正确性竞态，语句构成不同，不可与本文基线混用。
