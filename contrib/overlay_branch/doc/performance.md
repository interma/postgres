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
| 读：delta 加载 | `list_for_rel` 走 PK 索引 `(branch_id, relid)` 前缀；P0 命中时 `lookup` 走 PK 点查 | O(M) / O(1) | 不变 |
| 读：delta 排序 | `qsort` + 逐比较重建 btree 算子 | O(M log M) × syscache | O(M log M)，算子缓存 |
| 读：去重 | 逐 run 线性回查原始下标 | **O(M²)** | O(M)，或直接删除（结构性不可达） |
| 读：Pass1 匹配 | `bsearch` + 每 MAIN 行重建 TypedKey | O(N log M) × relcache/syscache | 同阶但常数大幅下降 |
| 读：Pass2 | 逐未命中 delta 全表线性扫 | **O(U × M)**，`U` = pure-delta 行数 | O(M) |
| 读：物化 | 首次取行即整结果集物化进 per-scan 内存上下文 | O(N + M) 内存，无早期终止 | 按需/流式（视需要） |
| 写：每 DML 行 | state 查询 + `fetch_tuple_by_ctid` + `tuple_version` + UPSERT | 3~4 次 SPI | 减少 SPI 次数 |
| apply：每 delta 行（3B 前） | fetch + version + base_image 探测 + base_image 比对 + DML | **4~5 次 SPI** | 集合式/批量化 ✅（3B） |
| apply：遍历次数 | D→U→I 各一趟；3B 起每 `(relid, op)` 一条验证 SQL + 一条批 DML | 3 × O(M) → O(1)/op | 已达（3B） |

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

  **写路径同类泄漏（后续补修，WR-1 前置）。** 上面的修复只覆盖读路径；**写重定向**里同一
  “跨 `SPI_finish` 存活对象误放 `TopMemoryContext`”又现**三处独立**对象（根因详见 §8.6.1）：
  3. `fetch_tuple_by_ctid()` 每行重建的 pre-image slot、其 `CreateTupleDescCopy()` tupdesc 与
     全部 `datumCopy()` 列值都落在 `TopMemoryContext` —— 每重定向 UPDATE/DELETE 行泄漏一份；
  4. UPDATE SET-merge 的 post-image 列副本也写 `TopMemoryContext`（每行每 varchar SET 列一份）；
  5. pure-delta UPDATE 步取 `overlay_delta_lookup()` 结果（`key`/`old_version`/`tuple_data` 按设计
     深拷贝进 `TopMemoryContext`）后从不释放 —— 每重写行泄漏整段序列化 payload。
  修复：`fetch_tuple_by_ctid()` 增 `MemoryContext result_cxt` 形参（镜像 `reconstruct_slot_from_delta()`），
  3 个调用点传 `es_query_cxt`；SET-merge 与 pure-delta post-image 分配改 `es_query_cxt`；
  pure-delta 步补齐 `dtu` 释放（对照读路径 `ob_overlay_free_delta_list()`）。
  验证：写路径探针（`MF1_WRITE_MEM_STABLE_PASS`）全部 `grew=0`。

  **错误路径的同类问题（本轮全库统一审计，已修复）。** 上述 5 处都在**成功/正向路径**；全库审计
  又定位到 5 处**只在错误分支触发**的同类缺陷，分两类：**UAF**（`SPI_finish()` 之后仍在读 SPI
  上下文里的指针）与**每次失败泄漏**（只服务 `ereport` 消息的副本被写进 `TopMemoryContext`，
  而错误路径永不返回，永远没有机会释放）。cassert 构建下 UAF 实测打印 `0x7F` 垃圾：
  6. `ob_check_branch_scope()`：`detail = SPI_getvalue(...)` → `SPI_finish()` → `errdetail("%s", detail)`
     —— 悬垂；`create_branch` 每次因 G01 scope 预检失败都会触发（回归 Section G01 即覆盖）。
  7. `ob_registry_check_current_schema()`：两处 `saved = pstrdup(sql.data / ins.data)`（两个
     StringInfo 都建在 SPI proc 上下文）→ `SPI_finish()` → 拼 `errmsg` —— 悬垂；同一函数的
     schema-drift 报错分支还把 `detail_text` 双重分配进 `TopMemoryContext`（`text_to_cstring` +
     `pstrdup`）从不释放。
  8. `ob_build_pk_where_clause()`：`SPI_getvalue()` 在 `TopMemoryContext` switch **内**调用 →
     返回值本身即落在 TopMC，随后又 `pstrdup` 一份，只释放了后者 —— 每 PK 列每次调用泄漏一份，
     在 per-row apply（`apply_batch=off`）下按行累积。
  9. `ob_batch_report_conflict()`：`c->key/old_version/main_ver` 是 `ob_batch_validate()` 按设计
     放进 TopMC 的，冲突 abort 路径消费后从不释放 —— 每次冲突泄漏 3 个串。
  10. `overlay_delta_insert()`：`state != ACTIVE` 错误分支的 `st`（TopMC）从不释放 —— 每次被拒写泄漏。
  修复原则：错误路径一律**在 `SPI_finish()` 之前**把要进消息的字符串拷进**调用方内存上下文**
  （`CurrentMemoryContext`，在 `SPI_connect()` 前捕获）；`ereport` 会立即展开 `%s`，因此副本只需在
  finish 前有效，**不需要**更长的生命周期，更**不该**用 `TopMemoryContext`。
  第 8 处改为直接接管 `SPI_getvalue()` 的返回值（它本就属于当前上下文）。验证：§8.7 错误路径探针。

  **成功路径补充（本轮全库审计新增一处）。** 另定位到 1 处**正向路径**的逐行泄漏：
  11. `write_redirect.c` pure-delta Phase D 的 `raw_rows` 收集循环在
      `MemoryContextSwitchTo(es_query_cxt)` 内调用 `SPI_getvalue()`，`pkstr`/`ov` 本就分配在
      `es_query_cxt`，旧代码却又各 `pstrdup` 一份存进 `WR_PDR1` —— 原串无人释放，纯 delta 多行
      UPDATE 每行泄漏 2 个串；`continue` 分支同样泄漏。修复：直接接管 `SPI_getvalue()` 返回串
      （与它们替掉的 `pstrdup` 同属 `es_query_cxt`，生命周期一致），并在 `continue` 前 `pfree`。

  **SRF 读路径的语句内瞬态峰值（本轮审计新增，非 `TopMemoryContext` 泄漏）。** 上面 11 处都是
  **跨语句存活**的泄漏，`TopMemoryContext` 前后断言即可观测。另有两处**语句内瞬态**增长，
  语句结束即释放、TopMC 断言看不见，只能靠**进程峰值 RSS**（`test/bench/srf_peak_rss.sh`）判定：
  - `overlay_branch.c : overlay_main_plus_delta()` 把每行 scratch（`values[]`/`isnulls[]` 与结果
    tuple）分配在 `funcctx->multi_call_memory_ctx`（SRF 结束才删除）——整表扫描随行数线性增长。
    修复：per-call **不再**切 multi-call ctx，scratch 落 executor 的 `ecxt_per_tuple_memory`
    （value-per-call 循环每次 `ResetExprContext`），返回 tuple 在下次 reset 前已被拷进 tuplestore。
  - `branch_scan.c : overlay_scan_next()` 每行重建的 result slot 及其 `datumCopy` 落在
    `result_cxt`（SRF 路径即 multi-call ctx，扫描期不 reset）——同样随行数线性增长。
    修复：新增 child `row_cxt`，每次调用入口 `MemoryContextReset`，slot/`datumCopy` 落 `row_cxt`。
  验证：`srf_peak_rss.sh` 300k 行 → **首次**扫描 VmHWM +32 MB，**第二次**仅 +0.4 MB
  （带缺陷时每次约 +80 MB）。重复扫描**平台化**即证明无泄漏：首次增长是 allocator（glibc）
  保留已释放页造成的**高水位**，语句结束后 `VmRSS` 仍偏高但**不再**随扫描次数增长。
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
  后续补修**写路径**同类的三处泄漏（pre-image slot/tupdesc、UPDATE SET-merge post-image、
  pure-delta `overlay_delta_lookup()` 结果），见 §2.1 与 §8.6.1。
  再补修 **SRF 读路径**两处**语句内瞬态峰值**（`overlay_main_plus_delta()` 的 per-row scratch
  误落 multi-call ctx；`overlay_scan_next()` 的 per-row slot 落 `result_cxt`），见 §2.1。
  已补会话级内存用例
  [overlay_branch_memgrow.sql](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_memgrow.sql)
  （读路径 + 写路径两组探针；SRF 瞬态峰值由
  [srf_peak_rss.sh](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/bench/srf_peak_rss.sh) 兜底）。
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

> 详细设计见 §6。**3A**（每行合并，语义不变）已落地；**3B**（集合式批处理）设计见 §6.4，待评审。

- `AP-1`：集合式 apply —— 按 op 分桶，用一条批处理 SQL 处理多行
  （如 `INSERT ... SELECT ... FROM jsonb_populate_record(NULL::t, ...)` 的数组展开），
  base_image 校验合并进同一次 fetch，减少每行 SPI 次数。
- `AP-2`：评估分块 apply 与持锁窗口的关系；需保证仍满足
  [multi_session_mvcc.md](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/multi_session_mvcc.md)
  的 CAS / 状态机不变量。

### 阶段 4 —— delta 体积

- 落地 §3.3 的 `pg_branch_delta_stats` 视图 + throttled WARNING。
- 视需要在 `apply` 前给出规模提示（仍是可观测，不是硬限制）。

### 阶段 5 —— 写路径 WR-1（中风险）

> 详细设计见 §8。目标：降低 DML 重定向到 delta 时的**每行** SPI 与 planning 成本
> （当前 UPDATE/DELETE 4 次、INSERT 2 次 SPI，且每行重建 SQL 文本）。

- `WR-1a`：把每行 SQL 改为**参数化 prepared plan**（消除每行 parse/plan）。
- `WR-1b`：减少 SPI 次数（合并 ctid fetch + version；缓存 branch state）。
- `WR-1c`：集合式批写（架构级，MVP 暂不做）。
- 前置：先把写路径纳入基线（M10），无数字不动手（§0 原则 2）。

---

## 6. 阶段 3 设计：apply 批量化（AP-1）

> Conv-2 设计先行。目标：把 `apply_branch()` 的**每 delta 行** SPI 次数从 8 降到 4（3A），
> 再进一步降到「每 `(relation, op)` 一条集合式 SQL」（3B）。
> **状态：3A 与 3B 均已落地**（含 §6.3.1 的两处修正）；3B 设计见 §6.4。

### 6.1 现状：每 delta 行的 SPI 预算（3A 前）

`overlay_branch_apply_internal()` 先按 `relid` 分桶，再对每个 relation 做 D→U→I 三趟。
单列 PK 表、DELETE/UPDATE 行的实测链路：

| 环节 | 调用点 | SPI/行 |
|---|---|---|
| 键 → WHERE | `ob_build_identity_where_clause` → `ob_build_pk_where_clause`：`jsonb_array_length` + 逐 PK 列 `#>> '{}'` + `string_agg` | 3 |
| 取 MAIN 行 | `ob_fetch_main_current_slot`（整行逐列 `datumCopy`） | 1 |
| 版本 token | `overlay_tuple_version`（**又发一次** `SELECT ctid::text, xmin::text`） | 1 |
| `_base` 探测 | `apply_check_base_image_match` 的 `? '_base'` | 1 |
| `_base` 比对 | 同上的 jsonb 规范化 + `FOR UPDATE` | 1 |
| 实际 DML | DELETE / UPDATE / INSERT | 1 |
| **DELETE / UPDATE 合计** | | **8** |
| **INSERT 合计**（无 base 比对） | | **4** |

两处**纯浪费**：`ob_fetch_main_current_slot` 按列物化整行，调用方只取 ctid + xmin；
`overlay_tuple_version` 为拿这两个值再查一次堆。

### 6.2 必须保持的不变量

1. **3 趟顺序**：同 relation 内 D→U→I；每趟只处理对应 `op` 的 delta。
2. **逐行乐观冲突语义**：MAIN 行缺失 / `old_version` 不匹配 / R15·R18 base_image 分歧，
   一律 `55000` 中止整个 apply（`overlay_guard_ereport_fail`）。
3. **纯 delta 特例**：`old_version IS NULL` 的 DELETE = NOP；UPDATE 无 MAIN 锚点时
   回退 `apply_relation_insert_pass`。
4. **keyless 身份**：`A:<blk>:<off>` → `ctid` 定位；`S:<seq>` → 无 MAIN 锚点。
5. **锁序**：`FOR UPDATE` 先于 DML，同事务持锁到语句结束。
6. **CAS / 状态机**：`active→applying→applied` 与 xact-scoped EXCLUSIVE advisory 锁不变（AP-2）。

### 6.3 阶段 3A：每行合并（低风险，语义不变）✅ 已落地

- **3A.1 键 → WHERE 去 SPI**：`ob_build_pk_where_clause` 由 3 次 SPI 压成 1 次。
  该唯一 SPI 只做「取长度 + 逐 PK 列取文本」的裸 targetlist
  （`SELECT jsonb_array_length($1::jsonb), ($1::jsonb)->>0, ...`），
  WHERE 字符串在 C 层用 `quote_identifier` + `quote_literal_cstr` + 缓存的
  `format_type_with_typemod()` 拼装。列名/类型清单按 rel 记忆（3A.4）。
  PK 键是 JSON **字符串数组**（`["10"]`，NULL → JSON `null`），与 `overlay_serialize_pk`
  的产出格式一致；`->>` 取其文本内容。keyless 的 `A:`/`S:` 分支本就是纯 C，不变。
- **3A.2 取行 + 取版本合并**：D/U 不再物化整行，改为单条轻量 SELECT
  `SELECT ctid::text, xmin::text FROM <rel> WHERE <where> FOR UPDATE LIMIT 1`
  → 一次同时得到「存在性 + 版本 token」；`overlay_tuple_version()` 不再参与 apply 路径。
- **3A.3 `_base` 探测**：见 §6.3.1 —— **不在 SQL 里探测**，改在 C 层扫描
  `tuple_data` 原始字节，无 `"_base"` 时直接返回，永不发起该次 SPI。
- **3A.4 per-rel 元数据记忆**：带引号 relname、列名清单、PK 列名/类型、bytea 输出函数句柄，
  在 rel 循环内算一次并传给三趟 pass（现每行重算）。
- INSERT：存在性检查改 `SELECT 1`，4 → 3。

落地后 D/U 每行 **3** 次 SPI（键→WHERE 1 + 取行/版本 1 + DML 1），I 每行 **2** 次。

### 6.3.1 实现期修正：规划成本 ≫ SPI 次数

3A 初版把「`_base` 探测」与「比对」合并成**一条**复杂 SQL（`? '_base'` + 两侧
`jsonb_object_agg` 规范化 + `LEFT JOIN LATERAL`）。实测发现这是净负优化：

- 该 SQL 每行的字面量都不同 → **计划无法复用**，每条语句都要全量 parse+plan；
  `pg_stat_statements(track_planning=on)` 显示其 plan 约 **55 µs/行**，
  而 HEAD 那条廉价的 `? '_base'` 探测仅约 4 µs/行。同理 3A 初版的 `VALUES`+`string_agg`
  键→WHERE 查询 plan 约 17 µs/行（HEAD 的等价三条只约 3 µs/行）。
- 结论：**在 apply 逐行路径上，SPI 调用次数不是主要成本，计划时间才是。**

据此修正两处：

1. **`apply_check_base_image_match`**：先在 C 层扫描 `tuple_data` 原始字节是否含
   `"_base"` 键；不含（当前写重定向路径的**全部** delta 都不含）即直接返回，
   该次 SPI 变为 0。只有真带 base_image 的 delta 才走原来的规范化比对 SQL。
2. **键 → WHERE**：见 3A.1，把 SQL 退化为裸 targetlist，拼装搬到 C 层。

修正后（同机、同 harness、`N=200000 M=20000`，M8 = `apply_branch()`）：

| 版本 | M8 |
|---|---|
| HEAD（3A 前） | ~1301–1319 ms |
| 3A 初版 | **3293.7 ms**（回归 +150%） |
| 3A 修正版 | **1310.5 ms**（回归消除，与 HEAD 持平） |
| 3B（集合式批处理，默认 on） | **~180 ms**（192.4 / 165.9 ms 两次采样） |

3A 修正版的 SPI 次数已低于 HEAD（D/U 6→3、I 5→2），墙钟持平；3B 把 planning 与 exec
都压到「每 `(relid, op)` O(1)」，M8 再降 **~7×**（同机 A/B：`apply_batch=off` 1260.8 ms
vs `on` ~180 ms），见 §6.4.13。

### 6.4 阶段 3B：集合式批处理 ✅ 已落地

> **状态：已落地**（设计先行稿 → 评审通过 → 实现 → 门禁全绿；决策记录见 §6.4.14）。
> 3B 为目标命名（AP-1），中高风险。

#### 6.4.1 目标与非目标

- **目标**：把 apply 的 SPI 次数从「每 delta 行 O(1)」降到「每 `(relation, op)` O(1)」。
  这同时消除 §6.3.1 暴露的**逐行 planning 主成本**（3 次/行 `SPI_execute` 的 parse+plan），
  并收窄持锁窗口（AP-2 受益）。
- **非目标**：不做并行 apply / 分块 apply；不改 `pg_branch_delta` schema；**不改** CAS /
  状态机 / advisory 锁语义（§6.2 全部不变量保持）；不追求与 PK 方案的功能对等。

#### 6.4.2 动因（来自 3A 修正版的度量）

M8(`M=20000`) ≈ **1310 ms** ⇒ ~65 µs/行，而每行只有 3 次 SPI。SPI 次数已不是瓶颈
（§6.3.1 已证明**规划时间**才是），因此 3B 的收益全部来自「把 planning 从 O(M) 降到 O(1)」。

#### 6.4.3 两种候选（评审决策点）

| 方案 | 形态 | 风险 | 预期收益 |
|---|---|---|---|
| **3B-1 参数化 prepared plan** | 保留逐行循环，改用 `SPI_prepare` + `SPI_execute_plan($n)`，计划跨行复用 | 极低（语义逐字不变） | planning → ~1×；仍 O(M) 次 SPI exec |
| **3B-2 集合式批处理（AP-1，推荐）** | 每 `(relid, op)` 一条**验证** SQL + 一条**批 DML** | 中高 | planning 与 exec 都 → O(1)/op |

推荐 **3B-2**；3B-1 可作为低风险前置或 3B-2 的回退路径（见 6.4.10）。

#### 6.4.4 结构原则：验证与变更分离、谓词同源

每个 `(relid, op)` 执行 **2 条 SQL**（happy path）：

1. **验证 SELECT**：用批 DML **完全相同**的合法性谓词，只返回**首个冲突行**
   （`ORDER BY key LIMIT 1`，附冲突种类与 C 端格式化所需字段）；无冲突返回 0 行。
2. **批 DML**：一次处理该 op 的全部合法行。

- **谓词同源**：批 DML 的 `WHERE`/`NOT EXISTS` 复刻验证谓词，故「验证通过 ⇒ DML 恰好命中全部
  预期行」。以 `SPI_processed` 与 C 端预期值（C 已加载 delta 列表，可直接计数）比对做**兜底断言**；
  失配（仅在异常并发下理论可达）报内部错误。
- **原子性**：整个 apply 在同一事务内，任一冲突 `ereport(ERROR)` 即回滚全部已做的批 DML——
  与现行「任一冲突中止整个 apply」等价（净效果相同，不需要子事务）。
- **锁**：批 DML 语句自身的行锁替代逐行 `FOR UPDATE`；因为它把
  「token 相等」写进同一语句的 `WHERE`，**加锁与校验在同一语句内原子完成**，等价于现行
  `SELECT ... FOR UPDATE` → 比对 → DML（见 6.4.9）。

#### 6.4.5 三条批 DML 的形态

记号：`@delta` = `@extschema@.pg_branch_delta`；`j` = `jsonb_populate_record(NULL::<rel>, convert_from(d.tuple_data,'UTF8')::jsonb)`；
`TOKEN(m)` = `replace(translate(m.ctid::text,'()',''),',',':') || '-x' || m.xmin::text`
（逐字匹配 `ob_fetch_main_version` 的 `"%u:%u-x%s"` 产出，可 `strcmp` 级别对齐）；
`PKJOIN(d,m)` = 逐 PK 列 `m."c" = ((d.key::jsonb)->>(i-1))::<pktype_i>` 用 `AND` 串起
（n=1 时即 `(d.key::jsonb->>0)::<pktype>`）。

- **D**（`old_version IS NOT NULL`；NULL 者 = 纯 delta NOP，跳过）：
  ```sql
  DELETE FROM <rel> m USING @delta d
   WHERE d.branch_id=$1 AND d.relid=$2 AND d.op='D' AND d.old_version IS NOT NULL
     AND PKJOIN(d,m) AND d.old_version = TOKEN(m);
  ```
- **U**（`old_version IS NOT NULL`）：
  ```sql
  UPDATE <rel> m
     SET (<cols>) = (SELECT <cols_parens> FROM jsonb_populate_record(NULL::<rel>, s.tj) AS j)
    FROM (SELECT convert_from(d.tuple_data,'UTF8')::jsonb AS tj, d.key, d.old_version
            FROM @delta d
           WHERE d.branch_id=$1 AND d.relid=$2 AND d.op='U' AND d.old_version IS NOT NULL) s
   WHERE PKJOIN(s,m) AND s.old_version = TOKEN(m);
  ```
- **I**（`op='I'` ∪ **orphan-U**`= op='U' AND old_version IS NULL`；均要求 MAIN 无锚点）：
  ```sql
  INSERT INTO <rel> (<cols>)
  SELECT <cols_parens> FROM @delta d
    CROSS JOIN LATERAL jsonb_populate_record(NULL::<rel>, convert_from(d.tuple_data,'UTF8')::jsonb) AS j
   WHERE d.branch_id=$1 AND d.relid=$2
     AND ( d.op='I' OR (d.op='U' AND d.old_version IS NULL) )
     AND NOT EXISTS (SELECT 1 FROM <rel> m WHERE PKJOIN(d,m));
  ```
  orphan-U 的 INSERT 归属 **U 步**（等价于现行 `apply_relation_update_pass` 内联调用
  `apply_relation_insert_pass`），以保持 D→U→I 的错误优先级顺序。

> 直读 `d.tuple_data` 后 `convert_from(...,'UTF8')` 即可，**不再**走现行逐行
> `substring(%s from 3)` hex 往返（省一次 `decode`）。

#### 6.4.6 冲突检测与错误消息保持

验证 SELECT 只取**首个冲突行**（`ORDER BY key`，与现行 delta 遍历顺序一致），
把 `(key, old_version, main_missing, main_ver)` 回传 C，由 C **沿用现有措辞**调用
`overlay_guard_ereport_fail()`（保持 `mvcc_review` / `rentry` 基线逐字不变）：

| op | 触发条件 | 消息分支（现状一致） |
|---|---|---|
| D | `m.ctid IS NULL` | `DELETE delta pk=%s: MAIN row no longer present ...`（含 "Resolve manually"） |
| D | token 不等 | `conflict pk=%s: old_version expected=%s but MAIN latest=%s`（含 "but"） |
| U | `old_version IS NULL AND m.ctid IS NOT NULL` | `conflict pk=%s: old_version expected=(null) MAIN latest=(null)` |
| U | `old_version IS NOT NULL AND m.ctid IS NULL` | `UPDATE pk=%s MAIN row missing at apply time ...` |
| U | token 不等 | `conflict pk=%s: old_version expected=%s MAIN latest=%s`（**无** "but"） |
| I | MAIN 已有同 PK | `INSERT pk=%s MAIN row already present ...` |

保留 D→U→I 的**错误优先级**：先 D 后 U 再 I 各跑一遍验证，故「同时存在 D 冲突与 I 冲突」
时仍先报 D（与现状一致）。

#### 6.4.7 纯 delta / keyless 分区

- **纯 delta**：`old_version IS NULL` 的 D = NOP（验证与 DML 都天然排除）；`old_version IS NULL`
  的 U = 有 MAIN 锚点则冲突、无锚点则并入 I 批。
- **keyless**（`A:<blk>:<off>` / `S:<seq>`）：`A:` 的 `PKJOIN` 换成
  `m.ctid = ('(' || replace(substring(d.key from 3),':',',') || ')')::tid`；`S:` 无 MAIN 锚点。
  **3B 范围决策：keyless relation 一律回退 3A 逐行路径**（降风险；如评审要求再纳入批处理）。

#### 6.4.8 `_base` 比对（R15/R18）

`_base` 规范化比对依赖 `to_jsonb(main_row) - 系统列` + `jsonb_object_agg`，集合式下 SQL 复杂且
**当前写重定向路径从不写 `_base`**（3A 已确认为死路径）。
**3B 范围决策：任一 delta 行含 `_base` 的 `(relid, op)` 一律回退 3A 逐行路径**——
用已有的 C 层字节扫描 `ob_delta_has_base_key()` 对已加载的 delta 列表在内存判定，**零新增 SPI**。

#### 6.4.9 锁与不变量（AP-2）

- advisory 锁顺序不变：`Advisory → (catalog + user RowExclusiveLock)（§6.2.6）`。
- 逐行 `FOR UPDATE` → 批 DML 自带行锁 + 同语句 token 谓词；**加锁即校验**，
  与现行「先锁后改」在语义上等价（并发写要么在 DML 前已提交 → token 失配被检出，
  要么被 DML 行锁阻塞）。
- 不变量 §6.2.1–6.2.6 **全部保持**；3 趟顺序因「同一 key 在 delta 中结构性唯一 ⇒ 各 op 的 key 集合互斥」
  而与集合式天然兼容，保留顺序仅为错误优先级与可观测一致性。

#### 6.4.10 GUC 与回退

- GUC `overlay_branch.apply_batch`（`bool`，`PGC_SUSET`，**默认 `on`**）控制启用；
  便于逐会话关闭以灰度/回退与 A/B 对照。
- 逐 `(relid, op)` 自动回退 3A 逐行路径的判据：GUC off / keyless rel（`n_pk=0`）/ 含 `_base`
  （见 6.4.14 #4）。
- 回退路径 = 现行 3A 代码，**不改动**，保证随时可用。

#### 6.4.11 等价性论证（评审要点）

1. **key 结构性唯一**（§1.2）：同一 `(branch, relid, key)` 只会有一个 op ⇒ 三趟作用于互斥 key 集。
2. **纯 delta 特例**：见 6.4.7，验证与 DML 谓词一致地排除/并入。
3. **冲突即回滚**：单事务内批 DML 先行提交的部分由 ERROR 回滚，净效果与逐行一致。
4. **消息与优先级**：见 6.4.6 表，逐字对齐现有基线。
5. **锁**：见 6.4.9。

#### 6.4.12 测试计划

- **差分等价（最强证据）**：同一 branch 在 MAIN 的两个克隆上各 apply 一次，
  一次 `apply_batch=off`、一次 `on`，比对结果表逐字节一致（含冲突场景的错误码/文案一致）。
- **L3**：`overlay_branch_mvcc_review.sql` 新增 `SECTION AP2 (milestone 3B)`：
  D+U+I 正路径；6 类冲突负向（见 6.4.6 表）；orphan-U→INSERT；keyless 回退；`off`/`on` 双跑一致。
- **L1**：现有 12 specs 在默认 `on` 下全绿（即 3A 逐行语义被集合式路径保持），
  无需新增 spec；`ob_apply_mutex` / `ob_applying_freeze` / `pure_delta_*` /
  `keyless_apply_conflict` 均照常通过。
- **门禁**：沿用 AGENTS.md §3 Green-Bar（两次 clean rebuild + L3 17/17 + L1 12/12 + 双 `diff -r` 0）。

#### 6.4.13 度量

复用 M8（`perf_baseline.sql`），对比 3A 修正版（`off`）/ 3B-2 集合式（`on`）；记录 transcript。
诊断同 §7：`pg_stat_statements.track_planning=on`，同会话交替 A/B。

**结果（N=200000 / M=20000，同机 A/B）**：`on` **192.4 / 165.9 ms**，`off` **1260.8 ms**，
⇒ **~7×** 提速。数字见 §6.3.1 表。

#### 6.4.14 决策记录（评审 2026-10-28）

1. **方案取舍**：**直接实施 3B-2 集合式**（不做 3B-1 前置）。
2. **GUC 默认值**：`on`（开箱即用）。
3. **keyless**：3B **回退** 3A 逐行。
4. **`_base`**：3B **回退** 3A 逐行。
5. **多列 PK**：3B 验收**仅单列 PK**；多列仅在 6.4.5 生成器层面支持。
6. **分块（AP-2）**：本轮不做；仅在 6.4.13 取「批 DML 持锁窗口 vs M」观测数据。

### 6.5 验证

- **不变量回归**：L3 `upsert` / `mvcc_review` / `puredelta` / `puredelta_mix` / `user`；
  L1 `ob_apply_mutex` / `ob_applying_freeze` / `pure_delta_upsert|update|delete` /
  `keyless_apply_conflict`。
- **3A 断言**：`test/regress/sql/overlay_branch_mvcc_review.sql` 的 `SECTION AP1`
  覆盖 D+U+I 单次回放正路径与三条 `0A000` 冲突分支（DELETE/UPDATE 锚点消失、
  INSERT 主键已存在），以及冲突后的恢复 apply。
- **3B 断言**：见 §6.4.12（`SECTION AP2` + 差分等价 + L1 on/off 双跑）。
- **性能**：复用 M8，`perf_baseline.sql` 前后对照（见 §6.3.1 表）。

### 6.6 本里程碑范围

3A、3B 均已落地（语义不变）；**3B 集合式批处理**见 §6.4（含决策记录 §6.4.14）。

---

## 7. 度量陷阱

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
- **逐行 SPI 路径看计划时间，不看 SPI 次数**：`SPI_execute` 每次都要全量 parse+plan；
  字面量内联使计划无法复用，因此「合并 SQL」可能反而更慢（§6.3.1）。诊断时开
  `pg_stat_statements.track_planning=on`，并**在同一会话内**交替测 A/B —— 跨会话、
  跨 `shared_preload_libraries` 配置的历史数字不可直接对照（同一份 HEAD 曾出现
  1301 ms 与 2185 ms 两个值）。

---

## 8. 阶段 5 设计：写路径 WR-1

> Conv-2 设计先行稿 → **已评审（2026-10-28）**。目标：降低一条 DML 语句在分支内**每行**的
> SPI 次数与 planning 成本。语义（delta 内容、冲突语义、状态 guard）必须不变。

### 8.1 目标与非目标

- **目标**：把写路径从「每行 2~4 次 `SPI_execute`（每次 parse+plan）」降到
  「每行（或每语句）复用计划」；在不改变 delta 行内容的前提下降低 CPU 与延迟。
- **非目标**：不改 `pg_branch_delta` schema；不引入批量/异步提交；不改 keyless 与
  `_base` 语义；不动 apply 与读路径。

### 8.2 现状：每 DML 行的 SPI 预算

`overlay_executor_run_intercept()`（[write_redirect.c](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c#L392)）
对单目标 DML 逐行拦截，每行按需发起以下 `SPI_connect/execute/finish`：

| op | SPI 调用（每行） | 次数 |
|---|---|---|
| INSERT | `overlay_delta_insert` = state 查询 + UPSERT（[delta_store.c:98-103](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L98-L103) / [L250](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L250)） | **2** |
| UPDATE | `fetch_tuple_by_ctid`（[L655](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L655)）+ `overlay_tuple_version`（[L1530](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L1530)）+ delta_insert 2 次 | **4** |
| DELETE | 同 UPDATE | **4** |
| INSERT…ON CONFLICT→UPDATE（推广） | 上述 + delta 探测 + MAIN 探测 | **6~7** |

共同点：**每条 SQL 都在行循环内用字面量内联重建**，`SPI_execute` 每次全量
parse+plan。已缓存者仅 per-relation 的 PK 元数据（`ob_pk_rel_meta`）与比较算子。

### 8.3 动因

§6.3.1 已证：**planning 成本 ≫ SPI 调用次数**（apply 3A 初版把复杂 SQL 内联后，
SPI 次数下降但墙钟翻倍）。写路径是同一模式：4 次调用 × 每行全量 parse+plan。
故首要杠杆是**消除每行 planning**，其次才是减少调用次数。

### 8.4 候选方案

- **WR-1a 参数化 prepared plan（推荐主线）**：把 4 条每行 SQL 由字面量内联改为
  `$1..$n` 参数，`SPI_prepare` + `SPI_keepplan` 缓存、`SPI_execute_plan` 复用；
  计划随 relcache/plan invalidation 失效重建。**只改计划复用，不改 SQL 语义**。
- **WR-1b 减少调用次数（可与 1a 叠加）**：
  - 合并 `fetch_tuple_by_ctid` + `overlay_tuple_version` 为一次
    `SELECT *, xmin::text FROM t WHERE ctid = $1::tid`（UPDATE/DELETE 4→3）。
  - 缓存 branch state：`overlay_delta_insert` 的状态查询按「会话内已确认 ACTIVE、
    且分支失效计数未变」短路（与 FR1/FR4 的失效节流同源），需保证 apply/discard
    并发时仍能及时察觉（正确性硬约束，见 8.5）。
- **WR-1c 集合式批写**：像 3B 那样把整语句的行先物化、再以集合式 DML 回放。
  拦截点内嵌在 executor 行循环里，改动面大、风险高，**MVP 暂不做**，仅存档。

### 8.5 必须保持的不变量

1. **delta 行内容逐字节不变**：`(branch_id, relid, key, op, old_version, tuple_data)`
   与现行路径一致（含 `tuple_data` 的 `'\x..'::bytea` 十六进制编码）。
2. **state guard 语义不变**：非 ACTIVE（applying/applied/discarded）时写入必须报
   与现行完全相同的错误（`overlay_delta_insert` 的 55000 / 0A000 文案）。
   任何 state 缓存都不得放宽「apply 进行中不得写 delta」。
3. **keyless / `_base` / 纯 delta 语义不变**；R03 PK-mutation guard、R20 SET NULL
   处理、R21 INSERT 前置检查等负向行为逐字不变。
4. **SPI 结果遍历**：沿用 `SPI_processed` 判定，不引入「只取第一行」的假设。
5. **内存所有权**：跨 `SPI_finish()` 存活的字符串必须在 **finish 之前**拷出 SPI 上下文
   （MF-1 教训），缓存本身也要有明确生命周期与失效回调。两条反向纪律同样不可破：
   - **finish 之后不得再读** SPI 返回的任何指针（tuptable / `SPI_getvalue` / `SPI_getbinval`
     派生值），否则 use-after-free（cassert 下 `0x7F`）；
   - 只服务 `ereport` 消息的临时副本落**调用方上下文**，不得落 `TopMemoryContext`
     —— 错误路径永不返回，TopMC 分配等于每次失败泄漏；
   - `SPI_getvalue()` 的结果归属于**调用时的当前上下文**，因此禁止在
     `MemoryContextSwitchTo(TopMemoryContext)` 内调用。

### 8.6 前置基线（M10）

写路径当前**无度量**（harness 构 delta 时 `\timing off`）。先加 **M10**：
`perf_baseline.sql` 的 build-delta 步骤拆为三段分别计时（`UPDATE m/2` / `DELETE m/4` /
`INSERT m/4`），产出可复现的「每行 µs」基线，作为 WR-1a/b 的优化前对照。

M10 首跑即暴露一个**接受范围外的异常**：INSERT 每行 ~1686 µs，比 UPDATE/DELETE 慢
**约 20×**（见 8.6.1）。该异常不是 planning 也不是内存问题，而是 `pg_branch_delta`
上一次退化扫描；它必须在 WR-1a 之前先修掉，否则 M10c 的「优化前」基线被污染、
WR-1a 的收益无法归因。

### 8.6.1 前置修复：删除冗余索引 `pg_branch_delta_relid_idx`

**现象（M10c）**：plain INSERT 的每行耗时随 delta 行数**线性增长**——正是 §8.4 要消除
的模式，但成因不在 planning。

**证据**：

- 三段计时显示 INSERT 单行成本是 UPDATE/DELETE 的 ~20×。
- freshness 探针（fresh 分支、`max_parallel_workers_per_gather=0`）：
  C1 INSERT 5000 行 = 1344 ms → C3（叠加两次 BranchScan 后）= 3762 ms →
  C4 = 6133 ms；**`pg_backend_memory_contexts` 的 TopMemoryContext 恒为 2112 kB**。
  ⇒ 排除「每行 parse+plan 复杂 SQL」与「内存上下文膨胀」两个假说；真因是扫描退化。
- 对写路径 delta 存在性探测 SQL 直接 EXPLAIN（真实 `pg_branch_delta`，delta=15000）：

  | 统计状态 | 计划 | key 谓词 | 耗时 |
  |---|---|---|---|
  | `reltuples = -1`（统计缺失） | `Index Scan using pg_branch_delta_relid_idx` | **Filter**，`Rows Removed by Filter: 5000` | 0.722 ms |
  | `ANALYZE` 后 | `Index Scan using pg_branch_delta_pkey` | Index Cond | 0.024 ms |

**根因**：`pg_branch_delta_relid_idx (branch_id, relid)` 是 PK
`(branch_id, relid, key)` 的**严格前缀**，本身完全冗余；`pg_branch_delta` 是内部表，
autovacuum 几乎不 ANALYZE 它，`reltuples = -1` 长期存在。统计缺失时 planner 会优先
选更窄的冗余索引，把 `key = $n` 降级为 **Filter**。于是 plain INSERT 每行的 delta
存在性探测退化成 **O(delta)**，构成 M10c 的 20× 慢。

**修复**：删除该冗余索引（[overlay_branch--1.0.sql](file:///home/ubuntu/work/postgres/contrib/overlay_branch/overlay_branch--1.0.sql#L41-L64)）。
`delta_store.c` 的 4 处 delta 访问（点查 `overlay_delta_lookup`、前缀读 `list_for_rel`、
`overlay_delta_count`、`overlay_delta_delete_all`）全部只用到 `(branch_id, relid)`
前缀，PK 索引恒可覆盖，与统计无关。已在 pkey-only 的临时表上复现验证：`reltuples = -1`
时前缀读与点查**均走 pkey**。副作用是顺带降低每行 delta 的写放大（少维护一棵索引）。

**修复后（M10 重跑，N=200000 / M=20000，全新库、无冗余索引）**：
M10c INSERT 5000 行 **8429.8 ms → 422.6 ms**（每行 1686 µs → 84.5 µs，**约 20×**），
与 M10a UPDATE（825.3 ms / 10000 行 = 82.5 µs/行）、M10b DELETE（399.5 ms / 5000 行 =
80.0 µs/行）回到同一量级——写入不再随 delta 规模线性恶化。

> 注：上述写路径 TopMemoryContext 泄漏（UPDATE/DELETE 每行 ~0.8–1.6 KB）是**真实但独立**于
> INSERT 退化扫描的问题（MF-1 家族），已随 WR-1 前置一并修复——`fetch_tuple_by_ctid()` 参数化
> `result_cxt`、SET-merge / pure-delta post-image 改 `es_query_cxt`、pure-delta 步补 `dtu` 释放
> （见 §2.1）。写路径探针 `MF1_WRITE_MEM_STABLE_PASS` 全 `grew=0`。

### 8.7 测试计划

- **L3**：新增写路径用例（`overlay_branch_scan_pk.sql` 或新文件，视体量）——
  覆盖 INSERT/UPDATE/DELETE 多行 set-based DML 的正确 delta 内容、PK 不可变、
  SET NULL、含 `"`/`\` 的值（十六进制编码回归）；prepared-plan 复用下**跨语句**
  结果一致（含计划失效后重建：ALTER TABLE / `DISCARD PLANS` 后重跑）。
- **L1**：`ob_apply_mutex` / `ob_applying_freeze` 必须继续通过，验证 state guard
  在缓存路径下未被放宽。
- **错误路径 / 二次审计探针**（[overlay_branch_memgrow.sql](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_memgrow.sql)
  Section 2–4）：纯 delta 多行 UPDATE（`MF1_PD_MULTIROW_MEM_STABLE_PASS`）、
  反复失败 `create_branch()` 的错误路径（`MF1_ERRPATH_MEM_STABLE_PASS`）、
  `apply_batch=off` per-row apply 回放（`MF1_APPLYPR_MEM_STABLE_PASS` +
  `MF1_APPLYPR_RESULT_CORRECT_PASS`），三者 `grew=0`。
- **门禁**：AGENTS.md §3 Green-Bar（两次 clean rebuild + L3 18/18 + L1 12/12 + 双 `diff -r` 0）。

### 8.8 度量

复用 M10 + M8（apply），对比 WR-1a / WR-1a+1b；`pg_stat_statements.track_planning=on`
看 planning 时间占比；同会话交替 A/B。

### 8.9 决策记录（评审 2026-10-28）

1. 主线：**WR-1a（参数化 prepared plan）**——本轮只做计划复用，不做 1b 减调用。
2. branch state 缓存：**不纳入本轮**（正确性敏感，state guard 保持每行校验）。
3. WR-1c（集合式批写）：确认 MVP 不做。
4. 验收范围：仅单列 PK 表（同 3B）。
5. 基线：**先落 M10**（build-delta 三段计时）再动手优化（§0 原则 2）。

### 8.10 实现与验收（2026-10-28）✅ 已落地

**新机制**：新增 [src/plan_cache.c](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/plan_cache.c)
——`TopMemoryContext` 子 context + `HASH_STRINGS` 缓存（键 = SQL 文本，含内联关系名）；
`ob_spi_exec_params()` 契约与 `ob_spi_one_shot()` 相同（`SPI_connect()` + 执行，**留 SPI
连接给调用方 `SPI_finish()`**）。`SPI_prepare_cursor(CURSOR_OPT_GENERIC_PLAN)` +
`SPI_keepplan()` 只 prepare 一次，`SPI_execute_plan()` 复用。失效**不自建回调**：完全交给
常规 plancache 机制（`ResetPlanCache()` 遍历 `saved_plan_list`，由 relcache/syscache 回调
与 `DISCARD PLANS` 驱动），branch 切换的 `CacheInvalidateRelcacheAll()` 同样命中。

**改造面**：`delta_store.c` 4 处（`overlay_delta_insert` / `overlay_delta_lookup` /
`fetch_tuple_by_ctid` / `overlay_tuple_version`）+ `write_redirect.c` 4 处（R21 delta 探测 /
R21 MAIN 冲突 / UPSERT Phase I 纯 delta / Phase II MAIN ctid）。行数据一律 `$n` 绑定；
`tuple_data` 由 `'\x..'` 文本字面量改为 `BYTEAOID` datum。每条 SQL 之后显式 `pfree` 参 datum，
保持写路径在语句生命周期 context 内 allocation-neutral（§8.5 第 5 条，MF-1 写探针不回归）。

**连带修复（沿用 use-after-free 根因）**：`r21_check_insert_preconditions()` 曾在 SPI 上下文内
`pstrdup(pkval_txt)`，`SPI_finish()` 后 `ereport` 再读——cassert 构建 (`CLOBBER_FREED_MEMORY`)
下打印 `0x7F` 垃圾，`overlay_branch_upsert` 的 DETAIL 行为此被 bless 成脏数据。删除该
`pstrdup` / `pfree`，`ereport` 直接引用调用方上下文的 `pkval_txt`（delta / MAIN 两处）。

**验收**：Green-Bar 全绿——两次独立 clean rebuild 均 0 warning/error；重建 #2 后 L3 **18/18**、
L1 **12/12**；`diff -r test_output/results test/regress/expected` 与
`diff -r output_iso/results test/isolation/expected` 均 0 行。新增 L3
`overlay_branch_plancache.sql`：跨语句计划复用结果一致 / 含 `"` `\` `'` 换行与非 UTF8 bytea
的 delta durability / `DISCARD PLANS` 与 `ALTER TABLE` 后重跑正确。
