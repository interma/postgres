# Overlay Branch 内部实现文档

> 本文档记录代码层面的实现细节、关键算法和 corner case 处理。
> 高层设计见 [design.md](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/design.md)，
> BranchScan 细节见 [branchscan.md](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/branchscan.md)，
> 并发与 MVCC 见 [multi_session_mvcc.md](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/multi_session_mvcc.md)。

---

## 0. 模块结构

| 文件 | 行数 | 职责 |
|------|------|------|
| [overlay_branch.c](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/overlay_branch.c) | ~1850 | 主模块：`_PG_init`、GUC 注册与 check/assign hook、4 个 Executor hook + ProcessUtility hook、Step7 HARD GUARD、递归 guard 标志、SQL-callable 薄分发、`overlay_main_plus_delta` SRF |
| [branch_lifecycle.c](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c) | ~3700 | create/use/apply/discard 全流程、advisory lock + CAS、snapshot 注册与 cache、mode cache、NOTIFY 广播、throttled 失效检查与 kickout、ACL 双层校验、registry 指纹 |
| [write_redirect.c](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c) | ~2470 | ExecutorRun DML 拦截：INSERT/UPDATE/DELETE 重定向到 delta、pure-delta 后处理、UPSERT 4-way promotion、RETURNING 采集回放 |
| [delta_store.c](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c) | ~2140 | PK 序列化、tuple 序列化、`old_version` token、SPI 读写 `pg_branch_delta`、typed PK 构建与比较、slot/ctid 辅助 |
| [branch_scan.c](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c) | ~1930 | planner hook（set_rel_pathlist_hook）、CustomScan 2-pass 叠加、成本模型、lappend bypass + pathlist 剪枝 |
| [overlay_branch.h](file:///home/ubuntu/work/postgres/contrib/overlay_branch/include/overlay_branch.h) | — | 所有共享类型与函数声明 |
| [branch_scan.h](file:///home/ubuntu/work/postgres/contrib/overlay_branch/include/branch_scan.h) | — | BranchScan 对外接口 |

**无独立 .h 内联文件**：所有逻辑在 5 个 .c 中实现，头文件只放声明。

---

## 1. 如何实现 branch

### 1.1 PK 的作用

PK 是分支内**行的唯一身份标识**，贯穿整个实现：

| 用途 | 说明 |
|------|------|
| delta 存储键 | `pg_branch_delta` 的 `key` 列是 PK 值的 JSON 序列化（`["v1","v2"]`），与 `(branch_id, relid)` 组成唯一键 |
| 写时定位 | UPDATE/DELETE 时从 junk slot 的 ctid 反查 MAIN 行，再序列化其 PK 作为 delta key |
| 读时匹配 | BranchScan pass 1 对每个 MAIN 行序列化 PK，与 delta 列表做 bsearch 匹配 |
| UPSERT 冲突判定 | INSERT 时用 PK 分别查 delta 和 MAIN，决定 4-way promotion |
| apply 冲突检测 | `old_version = ctid:xmin` token，apply 时比对 MAIN 当前行的 ctid+xmin |

**无 PK 表直接拒绝**（Step7 G5 guard），不做任何 fallback。

### 1.2 增量写的实现方式

**入口**：`overlay_ExecutorRun` hook → `overlay_executor_run_intercept`（[write_redirect.c:391](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c#L391)）

**核心不变量**：MAIN heap 在分支内永不被写入，所有变更通过 `overlay_delta_insert` 追加/覆盖到 `pg_branch_delta`，按 `(branch_id, relid, key)` 幂等（`ON CONFLICT DO UPDATE`）。

#### UPDATE/DELETE 路径（[write_redirect.c:839-1051](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c#L839-L1051)）

```
对 subplan 的每一行 junk_slot:
  1. ctid_cstr = slot_get_ctid_cstr(junk_slot)        // 从 junk slot 的 ctid 系统列取物理地址
  2. old_clean_slot = fetch_tuple_by_ctid(rel, ctid)   // 按 ctid 从 MAIN heap 重新取干净行
  3. oldver = overlay_tuple_version(rel, old_clean_slot)  // 生成 ctid:xmin 版本令牌
  4. [UPDATE] SET-merge: 按列名匹配 junk_slot 与 reldesc, 用 datumCopy 覆盖 old_clean_slot
     (R03: 禁止修改 PK 列; R20: 正确处理 SET col=NULL)
  5. pk = overlay_serialize_pk(rel, old_clean_slot)
  6. tdata = overlay_serialize_tuple(rel, old_clean_slot)  // DELETE 时为 NULL
  7. overlay_delta_insert(bid, relid, pk, op, oldver, tdata)
```

`fetch_tuple_by_ctid`（[delta_store.c:617](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L617)）走 SPI `SELECT * FROM rel WHERE ctid = '...'::tid`，必须在 `ob_in_overlay_helper=true` 下执行，否则 planner hook 会对该 SELECT 注入 BranchScan → 无限递归。

#### INSERT 路径与 UPSERT 4-way promotion（[write_redirect.c:1727-2376](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c#L1727-L2376)）

对每一行 subplan slot，做 **2-phase 冲突检测**：

- **Phase I**：SPI 查 delta 中是否有同 PK 的 op=I/U（pure-delta 命中）
- **Phase II**：SPI 查 MAIN heap 是否有同 PK（MAIN 命中）

然后根据 `(main_hit, pure_hit, oc_action)` 做 4-way 分发：

| main_hit | pure_hit | oc_action | promotion | 含义 |
|----------|----------|-----------|-----------|------|
| - | - | NONE | 1 | 普通 INSERT |
| false | false | DO_UPD/NOTHING | 1 | 无冲突 → INSERT |
| true/false | true/false | DO_NOTHING | 0 | 跳过 |
| true | false | DO_UPD + WHERE 满足 | 2 | UPDATE MAIN 行 |
| false | true | DO_UPD + WHERE 满足 | 3 | UPDATE pure-delta 行 |
| true | true | DO_UPD + WHERE 满足 | 2 | MAIN wins tie-break |

promo=2 时用 `fetch_tuple_by_ctid` 取 MAIN 旧行作为 scantuple，`ExecProject(oc_proj)` 计算 SET 后的值；promo=3 时用 Phase I 重建的 pure-delta 行作 scantuple。

#### pure-delta 后处理（[write_redirect.c:1053-1708](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c#L1053-L1708)）

"pure-delta" 指行只存在于 delta（op=I/U），MAIN heap 中没有。PG 标准 planner 对非 SELECT 的 commandType 不注入 BranchScan，所以 UPDATE/DELETE 的 subplan 只是对 MAIN 的 SeqScan，pure-delta 行**不会进入主循环**。

实现为 **ExecQual 后处理 pass**：

1. **采集**：SPI 查 delta 中 op IN ('I','U') 的行，重建为 slot 数组
2. **去重**：同 key 只取最新 delta
3. **过滤**：提取 subplan 的 `qual`（含 indexqualorig/bitmapqualorig/recheckqual），对每个候选 slot 执行 `ExecQual`
4. **写入**：通过的行执行 DELETE（op='D'）或 UPDATE（ExecProject 计算 SET 后的值）

**安全设计**：必须有显式 WHERE（qual 非空）才处理 pure-delta；全表 DML 对 pure-delta 行静默跳过，避免误删所有新插入行。

#### RETURNING 子句（[write_redirect.c:85-134](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c#L85-L134)）

策略：**采集 + 回放**。

- `ob_project_returning` 镜像 `ExecProcessReturning`（该函数是 nodeModifyTable.c 内 MODULE-local，无法直接调用），复用 `rri->ri_projectReturning` 这个已构建好的 ProjectionInfo
- 在 subplan 循环中为每行执行 RETURNING 投影，materialize 到 `retslots[]` 数组（分配在 `es_query_cxt`）
- 循环结束后统一 `dest->receiveSlot()` 推送给客户端

各操作的 scantuple/outertuple 选择：

| 操作 | scantuple | outertuple |
|------|-----------|------------|
| INSERT | 新行 slot | 新行 slot |
| UPDATE (MAIN) | SET 合并后行 | junk_slot |
| UPDATE (pure) | post_image | set_plan_slot |
| DELETE | 删除前行 | junk_slot |

### 1.3 增量读的实现方式（2-pass 叠加）

详见 [branchscan.md](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/branchscan.md)。核心算法在 `ob_compute_overlay_slots_internal`（[branch_scan.c:139-795](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L139-L795)）：

- **Pass 1**：扫 MAIN 行，对每行序列化 PK → bsearch delta → 无匹配输出 MAIN，匹配 DELETE 则丢弃，匹配 I/U 则用 delta 覆盖
- **Pass 2**：追加未被 pass 1 消费的 delta（pure-INSERT）

**性能优化**：从 O(N×M) 朴素双扫描改为 O(M log M + N log M)——先对 delta 按 PK 排序 + latest-wins 去重，再用 bsearch 匹配 MAIN 行。

### 1.4 算法性能考虑

| 优化点 | 说明 |
|--------|------|
| sort + bsearch | delta 排序后 MAIN 行用二分查找，避免 N×M 全表扫描 |
| latest-wins 去重 | qsort 后同 key 连续，只保留最后追加的 delta，修复 I→U 链首匹配 bug |
| P0 PK 等值快速路径 | `WHERE pk = const` 时只查一条 delta，MAIN 侧也只查一行 |
| typed PK 比较 | 用 PG 原生 btree 比较函数替代 strcmp，避免 BPCHAR/NUMERIC 语义错误 |
| 递归 guard | 内部 SPI 查询不被 BranchScan 包裹，避免栈溢出 |
| lappend bypass | 绕过 `add_path` 成本比较，强制 BranchScan 为唯一候选 |

---

## 2. BranchScan 实现

简述：通过 `set_rel_pathlist_hook` 将对用户表的扫描替换为 `CustomScan`（`overlay_branch_branchscan`），执行期把 MAIN 基线与分支 delta 合并。

**planner hook 预检查顺序（生命线，违反即递归崩溃）**：

1. **B0** schema 跳过（pg_catalog/information_schema/pg_toast*/overlay_branch）
2. **B1** 递归 guard（apply / write_redirect / overlay_helper 任一为真则 return）
3. **R16** ModifyingCTE 拒绝（0A000）
4. **B2** 仅 CMD_SELECT 注入
5. **is_active()**（必须在 B0 之后，因为 is_active 内部跑 SPI 会重入 hook）

**成本模型**：如实报告（不人为压低），但通过 `lappend` bypass + pathlist 剪枝（只保留 CustomPath）强制选中。

详见 [branchscan.md](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/branchscan.md)。

---

## 3. 并发控制

### 3.1 死锁自由设计（I8.1）

- `use_branch` **不加锁**
- `apply/discard` 用 **CAS**（`UPDATE pg_branch SET state='applying' WHERE branch_id=X AND state='active'`）做 1-winner 竞态，只有赢家才升级到 `ExclusiveLock` advisory
- 锁图无 shared→exclusive 升级边，证明无死锁
- 加锁顺序：Advisory → catalog 锁 → 用户表 RowExclusiveLock

### 3.2 apply 乐观冲突检测

- `old_version = ctid:xmin` token，apply 时比对 MAIN 当前行
- **HOT-update 二次校验**（R15/R18）：HOT update 下 ctid/xmin 不变但数据列被改写，比较 delta 中 `_base` 镜像与 MAIN 当前行（`jsonb_object_agg(... ORDER BY key)` 规范化）
- 3-pass 回放顺序：D → U → I（先删冲突键再更新再插入，避免唯一约束冲突）

### 3.3 跨会话失效（NOTIFY + throttled SPI + kickout）

- **广播**：apply/discard 成功后 `Async_Notify("ob_branch_state", "bid:state")`
- **检测**：`ob_invalidate_check_throttled` 用计数（32 次）+ 墙钟（100ms）双 throttle 控制 SPI 重查频率
- **观察者抑制**：仅当 `ob_throttled_allow_kickout=true`（只有 write_redirect ExecutorRun 和 `force_invalidation_check` 会置）才执行破坏性清理；planner/rel_ok 等观察者只返回 false 不改状态
- **DQL 软回 MAIN**：`for_dml=false` 时 NOTICE + fallback 读 MAIN
- **DML 硬 ERROR**：`for_dml=true` 时 55000 + kickout

### 3.4 snapshot 模式

- bid-keyed `TopMemoryContext` HTAB 缓存 `CopySnapshot`，设 `regd_count=1` 伪注册防 PG 自动释放
- re-entry 复用同一 frozen Snapshot 指针
- **TTL 闸门**（R19）：`ob_snapshot_max_hold_minutes`（默认 10 分钟），超时本地侧 kickout

详见 [multi_session_mvcc.md](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/multi_session_mvcc.md)。

---

## 4. Corner Cases

### 4.1 递归 guard 体系（5 层 bypass）

| Guard | 标志 | 设置者 | 防止的场景 |
|-------|------|--------|-----------|
| Layer 1 | `ob_in_apply_operation` | apply_internal | apply 阶段内部 SPI 查 MAIN 不被 BranchScan 包裹 |
| Layer 2 | `ob_in_overlay_helper` | 2-pass helper / fetch_tuple_by_ctid | 内部 SPI SELECT 不被注入 BranchScan |
| Layer 3 | `ob_in_write_redirect` | write_redirect | 写重定向内部的 MAIN 查询不经过 delta 合并 |
| Layer 4 | `ob_in_invalidation_check` | throttled 失效检查 | 防止 SPI→planner→is_active→throttled 重入 |
| Layer 5 | `ob_in_planner_hook` | planner hook | 让 throttled 在 planner 阶段只返回布尔值、不做副作用 |

**嵌套模式**（写重定向中调用内部 SPI）：
```
overlay_write_redirect_enter()       // ob_in_write_redirect=true
...
overlay_write_redirect_exit()        // 临时退出
overlay_overlay_helper_enter()       // ob_in_overlay_helper=true
SPI SELECT ... (planner hook B1 看到 overlay_helper=true → return)
overlay_overlay_helper_exit()
overlay_write_redirect_enter()       // 恢复
```

`PG_TRY/PG_CATCH` 保证异常路径也会清 guard，否则单次失败会让 guard 永久 stuck。

### 4.2 GUC check/assign hook 分离

`overlay_branch.current` 的 GUC hook 严格遵循 PG 契约：

- **check hook** = **无副作用**校验（SPI 只读，可被 PG 多次调用）。绝不 touch `ob_in_guc_setconfig`
- **assign hook** = 真实副作用（enter/leave branch），由 `ob_in_guc_setconfig` 防递归

**踩坑教训**（T5）：check hook 中若设置递归标志，PG 的 savepoint rollback / SET LOCAL 多阶段调用会导致 assign hook 第二次看到 flag=false → 重复清理 → double-pfree → PANIC。

### 4.3 Step7 HARD GUARD

`overlay_guard_rel_ok`（[overlay_branch.c:705-811](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/overlay_branch.c#L705-L811)）按从便宜到贵的顺序检查，任一命中即 ERROR：

| Guard | 检查 |
|-------|------|
| G1 | relkind != RELKIND_RELATION（视图/matview/外部/分区/序列/TOAST）|
| G2 | `relispartition`（分区子表）|
| G3 | 用户触发器（非 internal）|
| G4 | FK（出向 + 被引用，SPI 查 pg_constraint）|
| G5 | 无 PK |

DDL guard（`overlay_guard_ddl_ok_for_branch`）拦截 CREATE/ALTER/DROP/TRUNCATE/INDEX/REINDEX/CLUSTER/VACUUM 等，**TEMP 表白名单**（session 私有，不写 Main）。

### 4.4 typed PK 比较

**问题**：delta 持久化的 `key` 是 JSON text，`strcmp` 对 BPCHAR（尾部空格）、NUMERIC（`1.0` vs `1.00`）、TIMESTAMPTZ（时区表示）会产生语义错误。

**解决**：运行时用 `TypedKey[]` 替代 text 比较：

1. `overlay_build_typed_pk_from_slot` / `_from_key_text` 从 slot 或 JSON key 构建带类型信息的 PK 数组
2. `overlay_typed_pk_cmp` 用 `get_sort_group_operators` 取该类型 btree 的 `<`/`=` 操作符，`OidFunctionCall2Coll` 调用原生比较函数
3. collation 透传，按列真实 collation 比较

**回退策略**：typed 构建失败（如表达式索引）时 `typed_pk_n=0`，回退 strcmp，保证比较器始终是全序。

### 4.5 PK 序列化的 typmod 处理

- WRITE 侧 `overlay_serialize_pk` 只取 `atttypid` 调 output 函数，**BPCHAR 手动 r-trim**（对齐 `::text` 隐式 r-trim 行为）
- planner 侧 `overlay_serialize_pk_from_single_datum` 用 `format_type_with_typemod` 做 `CAST(... AS <type with typmod>)::text`
- 两侧最终都走 `::text` 归一化，保证字节兼容

### 4.6 old_version token 生成

`overlay_tuple_version`（[delta_store.c:1094](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L1094)）：

1. 从 junk slot 取 ctid
2. 走 `ob_in_overlay_helper` 下的 SPI `SELECT ctid::text, xmin::text FROM rel WHERE ctid = '...'`
3. 组装 `blk:off-x<xmin>` 格式 token

### 4.7 junk attr 映射

UPDATE/DELETE 子计划的 slot 包含 junk attrs（ctid、tableoid 等），列位置与 relation 描述不一致。`rel_attno_to_slot_idx`（[delta_store.c](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c)）把真实 attno 映射到 slot 内位置，避免错列。

### 4.8 memory context 纪律

所有跨 SPI 边界返回的字符串/bytea/TypedKey 都 `datumCopy`/`pstrdup` 到 `TopMemoryContext`，因为 `SPI_finish` 会销毁 SPI proc 上下文。BranchScan 的 result_slots 也分配在 TopMemoryContext，由调用方释放。

### 4.9 快照纪律

`overlay_delta_lookup` / `list_for_rel` 用 `PushActiveSnapshot(GetLatestSnapshot())` 强制刷新 `curcid`，确保同一事务内后写入命令的 delta 行对当前游标可见（解决 DECLARE CURSOR 时冻结的 curcid 问题）。

### 4.10 自 apply/discard 自动回 Main

- 若当前 session 正在 `use_branch(X)`，自己执行 `apply_branch(X)` 后，自动 `ob_exit_branch_cleanup` + GUC 置空 + PlanCache 失效
- discard 同理，并发 NOTICE 提示

### 4.11 mode cache（I8.2）

session-local bid→mode HTAB 替代旧的 catalog `UPDATE pg_branch.mode`，消除并发 use 的 RowExclusiveLock 互锁（修复 360s 假死锁）。GUC 1-arg SET 时复用缓存的 mode，使 snapshot 模式在 leave→re-enter 后保持。

### 4.12 registry 指纹延迟写入

`pg_branch_registry` 的 schema 指纹**故意延迟**到首次 `use_branch`/`apply_branch` 时才写入（而非 create 时），以支持 `create_branch → CREATE TABLE on MAIN → use_branch` 的合法时序，使快照取在 entry-time 而非 create-time。

### 4.13 PlanCache 失效

每次 enter/leave branch 后调用 `CacheInvalidateRelcacheAll()` + `CommandEndInvalidationMessages()`，强制 PREPARE 语句下次 EXECUTE 重新规划，注入或移除 BranchScan。

### 4.14 ACL 双层防御

- **Layer 1（SQL）**：lifecycle 函数 `REVOKE EXECUTE TO PUBLIC`，仅 owner + `overlay_branch_administrators` 角色
- **Layer 2（C）**：`ob_acl_check_lifecycle` 在每个 wrapper 内再次校验，防止直接 `@extschema@.qualification` 绕过

| 操作 | 允许条件 |
|------|----------|
| CREATE | superuser 或 mgmt 角色 |
| USE（退出）| 恒允许 |
| USE（进入）/ APPLY / DISCARD | superuser 或 mgmt 角色 或 branch owner |

### 4.15 apply/discard 幂等拒绝

已 `discarded` 或 `applied` 的分支再次 discard 报 55000，不可重复操作。

### 4.16 publication 泄漏检查（G06）

create_branch 时 throttled 检测是否有 FOR ALL TABLES / 含 overlay_branch schema 的 publication，命中则 WARNING（逻辑复制会把 delta 表的变更复制到订阅端，破坏隔离）。

---

## 5. 关键不变量汇总

1. **MAIN heap 在分支内永不被写入** — 所有变更进 `pg_branch_delta`
2. **无 PK 表直接拒绝** — 不做 fallback
3. **planner hook 预检查顺序不可颠倒** — B0 → B1 → R16 → B2 → is_active
4. **GUC check hook 无副作用** — assign hook 才做真实操作
5. **递归 guard 必须 PG_TRY 保护** — 异常路径也要清
6. **跨 SPI 内存必须进 TopMemoryContext** — 防止 SPI_finish 后悬垂
7. **use 不加锁，apply/discard 用 CAS** — 死锁自由
8. **观察者不做破坏性清理** — kickout 只在 DML 路径执行
