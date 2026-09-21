# 多 Session MVCC 架构设计与需求规范

> 本文档是 V3 多 session MVCC 功能的**单一权威说明源**，涵盖：
> 1. 需求规格（问题、目标、FR/NFR、验收标准）
> 2. 核心设计、协议与策略

---

## Part A：需求规格

### A.1 问题陈述

V1/V2 的 `overlay_branch` 扩展在**单 session 单用户**场景下工作正确。但在真实 PostgreSQL 部署中，多个 backend session 可能：

1. 同时使用同一个 branch（跨 session 共享 spec 分支）；
2. 一个 session 对 branch 执行 `apply_branch()` / `discard_branch()` 时，其它 session 仍持有该 branch 的**陈旧 session 缓存**，导致继续写入到已终结 branch 的 delta 行、或 MAIN 被 apply 重放两次；
3. `apply_branch` 执行 3-pass 乐观回放时，两个 session 同时竞争同一 branch 的 apply 权限，出现**双 apply race**（都通过 state='active' 检查，都进入 3-pass）；
4. Live mode 下 MAIN 数据随其它 session 持续 drift，无法提供 spec 分支级别的**一致性快照语义**。

### A.2 用户画像

- **应用开发者**：多连接池并发使用 overlay_branch 进行 spec/验证，期望每个连接对同一个 branch 的视图是一致的、且不会因 apply/discard 产生静默损坏。
- **CI/CD 框架**：多个 worker session 并行写入同一个 spec 分支（例如 feature-branch 端到端测试），apply 必须互斥且不会 double-apply。
- **审计/合规用户**：需要确认在 apply 成功提交前，绝对没有第二个 session 已经对 MAIN 产生了相同 branch 的第二次重放。

### A.3 目标 (Goals)

#### G1：跨 Session Branch 状态一致性
任何 session 修改一个 branch 的 state（active → applied / discarded）后，**所有其它正在使用该 branch 的 session 在其下一条 overlay 相关语句时**，必须感知到 state 已变化并自动退出当前 branch（而不是继续写入/读取一个已终结的 branch）。

#### G2：Apply/Discard 互斥（Branch 级 advisory lock）
同一时刻**最多只能有一个 session** 对同一个 branch 执行 `apply_branch()` 或 `discard_branch()`。第二个尝试者必须阻塞（或超时报错）直到第一个提交/回滚。绝对禁止两个 session 同时进入 3-pass 重放或同时级联删除 delta。

#### G3：Live 模式 Delta 跨 Session 立即可见 + MAIN 快照可选
- 多 session 写入同一 branch 的 delta：一个 session 事务提交后，其它正在同一 branch 上的 session 的**下一条 DQL** 必须能看到新 delta 行（不保留过期 BranchScan materialized cache）。
- 引入 SNAPSHOT 模式：`use_branch(name, mode => 'snapshot')` 在进入时捕获当前事务快照，整个 branch 生命周期内 MAIN 读冻结在该快照上（delta 仍为最新已提交的 pg_branch_delta）。

#### G4：Apply 期间 MAIN 写保护（RowExclusiveLock 升级 + 全局 pin 追踪）
在 `apply_branch()` 的 3-pass 运行期间，其它 session **不能继续**往该 branch 的 delta 追加记录（否则 apply 已经重放了 U，又来一个新的 I 行被漏掉）。必须存在一个全局的 "branch being applied" pin。

### A.4 非目标 (Non-Goals)

- **NG1：分布式 / 多节点**：仅考虑单 PostgreSQL 实例内的多 backend session 并发，不涉及 Citus 或逻辑复制场景。
- **NG2：用户级 branch 权限矩阵**：owner/superuser 限制沿用 V2 的 catalog 检查（`pg_branch.owner = GetUserId()` 或 superuser），V3 不新增细粒度 ACL。
- **NG3：跨分支 merge / rebase / cherry-pick**：仅保持 apply/discard 的原子性，不对分支之间的 diff 算法做改动。
- **NG4：嵌套事务 / 子事务 savepoint 精细管理**：仍使用顶层事务 + PG standard MVCC；savepoint rollback 必须正确归还 advisory lock，但不新增 savepoint-aware pin 栈。
- **NG5：真 Snapshot Isolation 级别（SERIALIZABLE）**：SNAPSHOT mode 仅提供 MAIN 读一致性，不承诺 branch 内部写-写冲突的可串行化校验；冲突沿用 old_version 乐观 token。

### A.5 功能需求 (Functional Requirements)

#### FR1：Shared BranchCache 失效机制
- 当任何 session 在 catalog 中把一个 branch 的 state 从 `active` 改为 `applied` 或 `discarded` 时，必须发出**共享内存级通知**（使用 PG 的 sinval / AsyncQueueNotify 机制，或自定义 advisory channel `ob_inv_bid_<branch_id>`）。
- 所有 session 在执行以下入口前检查一次全局失效标记：
  - `overlay_branch_is_active()` 顶部（被 `overlay_should_redirect()`、`overlay_guard_rel_ok()`、`branch_scan` Planner hook、`set_rel_pathlist_hook` 等共同调用）；
  - `overlay_executor_run_intercept()` 入口；
  - `ob_branchscan_begin()` 入口（BranchScan executor 阶段）。
- 如发现本地 `CurrentBranchContext->branch_id` 对应的 catalog state 已不再是 `active`，则**立即清退**本地 session 的 `CurrentBranchContext->is_active = false`，并 ereport(NOTICE) 告知用户该 branch 已被其它 session apply/discard。若当前语句是 DML（CMD_INSERT/UPDATE/DELETE），则必须 ERROR（阻止继续写入已终结 branch 的 delta）。

#### FR2：Branch 级 Advisory Lock（Apply/Discard 互斥）
- 在 `overlay_branch_apply_internal()` 与 `overlay_branch_discard_internal()` 的**最顶部**（state='active' 检查前），使用 `LockSharedAdvisory(branch_id_hash(branch_id), ExclusiveLock)` 对该 branch 加**全局排他 advisory lock**。
- 锁持续到**事务提交/回滚**（advisory lock 是 xact 生命周期的），即 3-pass 重放 + pg_branch state UPDATE + delta DELETE 整个过程都是排他的。
- `branch_id_hash` 算法：`((uint64)('ob'<<48) | (uint64)branch_id)` — 高位 magic 避免与用户自定义 advisory lock 冲突。
- 若无法在 `deadlock_timeout` 内获取锁，PG 原生 deadlock detector 会触发 ERROR。

#### FR3：Apply 期间 Branch 冻结（禁止写 Delta）
- Apply 内部获取 advisory lock 后，在 `pg_branch` 中先把 state 原子性地从 `active` 改为 `applying`（新引入的 transient state），作为全局硬标记：
  ```sql
  UPDATE pg_branch SET state = 'applying'
   WHERE branch_id = %d AND state = 'active';
  ```
  - 如果 0 rows updated → ERROR（表示刚被其它 session 抢走）。
- 所有 delta 写入路径 `overlay_delta_insert()` 顶部检查 `pg_branch.state`（通过 SPI 轻量 SELECT 或共享内存 flag）：若为 `applying/applied/discarded` 则 ERROR，禁止继续追加。
- Apply 成功最后一步把 `applying → applied`；若 apply 中间失败 rollback → 事务 rollback 会自动把 `applying` 的 UPDATE 回滚回 `active`（因为 PG MVCC）。

#### FR4：Use-branch Catalog 实时有效性检查
- `overlay_branch_use_internal()` 目前只在进入时检查 state='active'。V3 新增：
  - 在 `overlay_branch_is_active()` 内部增加一个**节流的 catalog 回查**：每 `N` 次调用（N=32，GUC：`overlay_branch.invalidation_check_threshold`）或每隔 `M` 毫秒（M=100ms，使用 `GetCurrentTimestamp()` 比较）执行一次轻量 SPI：
    ```sql
    SELECT 1 FROM pg_branch WHERE branch_id = %d AND state = 'active'
    ```
  - 如果没查到 → 自动清退本地缓存。
- 这是 FR1 sinval 失效的补充（sinval 在极端网络抖动下可能延迟）。

#### FR5：SNAPSHOT Mode — MAIN 读一致性
- 在 `pg_branch.mode` 列已存在 `'live' | 'snapshot'` 的基础上，实现 snapshot：
  - `use_branch(name, mode => 'snapshot')` 时调用 `RegisterSnapshot(GetTransactionSnapshot())`（或使用 `PushActiveSnapshot` 保存到 `CurrentBranchContext->snapshot`）。
  - BranchScan 的 2-pass helper `ob_compute_overlay_slots_internal()` 在读取 MAIN 行时，**先把 snapshot 压入** `PushActiveSnapshot(ctx->snapshot)`，读完成后 `PopActiveSnapshot()`。
  - delta 行读取（`overlay_delta_list_for_rel` / `overlay_delta_lookup`）仍然使用最新快照（分支内 delta 是最新提交的）。
  - Live mode 保持不变（PushActiveSnapshot(GetLatestSnapshot()) 等价于 V2 行为）。
- `CurrentBranchContext` 新增字段：
  ```c
  Snapshot branch_main_snapshot;   /* NULL for live mode */
  ```
- `use_branch` 的 SQL 函数签名新增可选 `mode` 参数（默认 `'live'` 保持兼容）。

#### FR6：Materialized BranchScan 失效（跨 Session Delta Drift）
- V2 中 ExtendedCustomScanState.result_slots 在 `ob_branchscan_begin()` 时一次性 materialize，然后 `ReScanCustomScan` 复用。如果另一个 session 在本 scan 中途提交了新 delta，第二次 ReScan 仍返回旧 materialization。
- V3：
  - `ob_branchscan_rescan()` 不使用缓存的 result_slots，强制**重新计算**（释放旧 slots 列表，重新调用 `ob_compute_overlay_slots_internal`）。
  - 首次 scan 保留 materialization（性能不变），但 ReScan 必须 fresh。

#### FR7：Use 计数 / Pin 追踪
- 在共享内存（`shmem_startup_hook` 注册，使用 `RequestAddinShmemSpace`）中维护一个 hash 表：`branch_id → {pin_count, applying: bool}`。
- `use_branch` 时 pin_count++，`discard/RESET GUC/backend exit` 时 pin_count--；apply 期间 applying=true。
- 该 hash 表仅用于：
  - 快速失败优化（避免每次都走 SPI 查 state）；
  - `apply_branch` 在 advisory lock 成功后，等待 pin_count 降到 0（或发出 NOTICE 告知仍有 N 个用户在 branch 上）才真正进入 applying state。
  - **MVP 范围可窄化**：若共享内存初始化复杂，退化为纯 SPI-based "state checking on every 32 calls" + advisory lock 互斥，Pin 计数留到 V3.1。

### A.6 非功能需求 (Non-Functional Requirements)

#### NFR1：V2 单 Session 零回归
所有 V2 的 3 个回归测试（basic/user/advanced）在 V3 代码下**必须 0 diff 通过**，无性能劣化 > 10%（单 session 路径下）。

#### NFR2：并发正确性 > 性能
在 32 session 压测下（24 写同一 branch + 4 apply 竞争 + 4 discard 竞争），MAIN 表不会出现 double-apply 或损坏，即使 TPS 下降也可以接受。

#### NFR3：死锁安全
Advisory lock 顺序必须与 user-table RowExclusiveLock 顺序一致，避免 AB-BA 死锁：
- 顺序 1：先拿 `branch_id` advisory lock（ExclusiveLock）；
- 顺序 2：再在 3-pass 中 `table_open(user_rel, RowExclusiveLock)`；
- **禁止**任何 session 在持有 user-table 锁后再去申请 branch advisory lock。
- `ob_in_apply_operation=true` 路径内的 SPI helper 严格遵守这一顺序。

#### NFR4：Advisory lock 冲突概率最小化
`branch_id_hash` 必须显式包含 magic 前缀并在文档中记录，不使用裸 `branch_id` 防碰撞。

#### NFR5：内存安全
共享内存 hash 表（如启用 FR7）必须在 `_PG_init` 中 `RequestAddinShmemSpace(hash_estimate_size(max_backends * 16 bytes))`，并使用 `shmem_startup_hook` 初始化；backend exit 时调用 callback 归还 pin（使用 `on_shmem_exit` 或 `on_proc_exit`）。

### A.7 约束与依赖 (Constraints & Dependencies)

- PostgreSQL 17：所有 advisory lock API（`LockSharedAdvisory` / `UnlockSharedAdvisory`）都是 PG 17 原生的。
- 共享内存：若 FR7 启用，需在 `_PG_init` 中调用 `RequestAddinShmemSpace`，要求 `shared_preload_libraries` 加载。为降低门槛，**默认采用纯 SPI-based FR1 + FR2**，FR7 作为 `overlay_branch.use_shared_mem_pin_table = on` 的可选项。
- 不得修改 PG core 源码：所有功能仅通过 extension hooks + SPI + standard advisory lock API 实现。

### A.8 开放问题 (Open Questions)

1. **OQ1：Apply 发现仍有 session 在 branch 上（pin_count > 0）时策略**？
   - 选项 A：立即 ERROR，要求用户先让所有 session 退出 branch 再 apply；
   - 选项 B：等待最多 N 秒后超时；
   - 选项 C：发 NOTICE 但继续（V2 当前行为，存在 "后续 delta 丢失" 风险）。
   - **暂定 A（严格一致优先）**，通过 GUC `overlay_branch.apply_strict_pins = on` 控制。

2. **OQ2：SNAPSHOT mode 下 MAIN 读时间窗口**？
   - 选项 A：use_branch() 时 snapshot 固定，直到 exit branch（"整个 spec 分支期间 MAIN 不变"）；
   - 选项 B：每条 statement 开始时 snapshot（"statement-level consistency, branch drift allowed between statements"）。
   - **暂定 A**，符合 spec 分支的直觉语义。通过新 mode='statement_snapshot' 扩展。

3. **OQ3：Cache 失效粒度**？
   - 选项 A：使用 PG sinval/cache invalidation framework 注册 custom invalidation message；
   - 选项 B：使用 LISTEN/NOTIFY channel `ob_branch_state`；
   - 选项 C：仅用 FR4 的阈值式 SPI 回查（无推送）。
   - **暂定 B（LISTEN/NOTIFY + C fallback）**：implementation complexity 最低，且 `apply/discard` 本身是低频率操作（每条分支最多一次）。

### A.9 验收标准 (Acceptance Criteria)

#### AC 组 A：Apply/Discard 互斥（rule）
- **AC-A1（Double Apply Race）**：两个 backend session 同时对同一 branch 执行 `apply_branch()`，最终必须恰好一个成功、另一个 ERROR（或阻塞后成功），MAIN 表中受影响行数等于**单次** apply 的预期行数（不重复）。
- **AC-A2（Apply vs Discard Race）**：A session apply、B session discard 同一 branch。无论谁先拿到锁，结果要么是 `state=applied + delta empty`，要么是 `state=discarded + delta empty`，绝不允许不一致态。
- **AC-A3（Advisory Lock 顺序死锁）**：构造 2 个 session 交错执行 apply + user-table UPDATE，连续 100 次并发运行不产生 `deadlock detected` ERROR。
- **AC-A4（Lock Hold Duration）**：apply 成功后，对同一 branch 立即执行第二次 apply，必须 ERROR（state != active）且不阻塞。

#### AC 组 B：跨 Session 状态失效（rule）
- **AC-B1（Apply Kicks Other Users）**：S1 use + INSERT + commit；S2 use 同一 branch；S1 apply；然后 S2 下一条语句：
  - 若为 SELECT：自动 NOTICE，回到 MAIN 视图；
  - 若为 DML：ERROR，绝不写入 state != active 的 branch。
- **AC-B2（Discard Kicks Other Users）**：同上模型对称表现（discarded state）。
- **AC-B3（FR4 Threshold Check 兜底）**：人为阻止 NOTIFY 链路，AC-B1 流程下 S2 最多 32 次 is_active 调用后必须检测到 state drift 并退回到 MAIN。

#### AC 组 C：Snapshot / Live Mode（rule）
- **AC-C1（Live Mode Delta Immediate）**：S1 BEGIN + INSERT 100 行；S2 同 branch SELECT。S1 COMMIT 后 S2 的**下一条** SELECT 必须看到 100 行。
- **AC-C2（Snapshot Mode MAIN Freeze）**：S1 snapshot mode；MAIN t=100 行；S2 非 branch 插入 50 行 commit；S1 在 branch 上 SELECT — MAIN 部分仍是 100 行；退出 branch 后才看到 150 行。
- **AC-C3（Snapshot Mode Delta Live）**：snapshot mode 下，S1 自己写入的 delta 立即可见；snapshot 只冻结 MAIN 读，不冻结 delta。

#### AC 组 D：回归与性能（rubric）
- **AC-D1（V2 回归）**：`make check REGRESS="overlay_branch_basic overlay_branch_user overlay_branch_advanced"` 3/3 ALL PASSED, 0 diffs。
- **AC-D2（单 Session 性能，0-2 scale, pass≥1）**：
  - 2 分：basic.sql 总 runtime 相比 V2 增长 ≤ 10%；
  - 1 分：增长 10-25%；
  - 0 分：增长 > 25%。
- **AC-D3（压测正确性，0-2 scale, pass≥1）**：`pgbench -T 60 -c 32` 下：
  - 2 分：0 double-apply，0 orphan delta，MAIN 行数精确匹配；
  - 1 分：≤ 1 告警级不一致（无数据损坏）；
  - 0 分：MAIN 数据损坏或 orphan delta 存在。

### A.10 风险登记

| ID | 风险 | 缓解 |
|----|------|------|
| R1 | LISTEN/NOTIFY 丢失（OQ3 fallback） | FR4 阈值式 SPI 回查；NOTIFY 仅作为 fast-path |
| R2 | 共享内存 shmem 未 preload（FR7 可选） | 默认关闭；文档说明启用方式 |
| R3 | Apply 顺序 deadlock（NFR3） | ENFORCE: advisory lock 先于 user-table；违规路径用 Assert 崩溃 |
| R4 | snapshot 长期持有导致 xmin freeze 膨胀 | SNAPSHOT mode docs 添加 HINT：及时 exit branch；apply/discard 自动释放 |
| R5 | ReScan 重新 materialize 性能劣化 | NFR2 正确性 > 性能；V3.1 可加 version-timestamp 轻量失效 |

---

## Part B：核心设计与实现策略

### B.1 问题背景（V1/V2 的 Session-Local 缺陷）

| 组件 | V1/V2 行为 | V3 需要 |
|---|---|---|
| `CurrentBranchContext` | 纯 session-local，`_PG_init` 时 TopMemoryContext palloc，从不主动失效 | 跨 session 失效：其它 session 的 apply/discard 必须让本 session 感知 |
| `overlay_branch_current_name` (GUC) | 仅本 session SET/RESET 修改 | 配合 Catalog state 变化自动回退 |
| `apply_branch()` 3-pass | 无互斥：两 session 同时 apply，都通过 state='active' → MAIN 重放两次 | Branch 级排他互斥（双 apply 零容忍） |
| `pg_branch.state` | 仅 3 态：`active / applied / discarded` | 引入 transient `applying` 态，冻结期间禁止 delta 写入 |
| BranchScan result_slots | Begin 时一次性 materialize，ReScan 复用 → 看不到其它 session 新 delta | ReScan 必须 fresh materialize |
| MAIN 读语义（Live mode） | 跟随每条语句最新快照（隐式） | 保留 Live；新增 **SNAPSHOT mode**（use 时冻结 MAIN 读） |

### B.2 整体架构：四层并发防御塔

```
  Layer 4 (语义正确性)    SNAPSHOT mode MAIN freeze / Live mode delta visible
  Layer 3 (写路径保护)    state ∈ {applying, applied, discarded} 拒绝 delta 写入
  Layer 2 (事务级互斥)    Branch-级 Advisory Lock — apply/discard 双路径全局排他
  Layer 1 (感知失效)      LISTEN/NOTIFY ob_branch_state + 阈值式 SPI 兜底回查
```

### B.3 Layer 1 — 跨 Session 状态失效协议

#### B.3.1 Channel 命名

```
NOTIFY channel:   "ob_branch_state"
Payload:          "<branch_id>:<new_state>"
                  例如: "5:applied", "3:discarded", "7:applying"
```

发布点（Publisher，仅两处）：
- `overlay_branch_apply_internal()`：UPDATE pg_branch.state 成功后立即 NOTIFY
- `overlay_branch_discard_internal()`：同上

订阅点（每个 backend）：
- 每 32 次 `overlay_branch_is_active()` 调用或每 100ms 主动消费 NOTIFY + SPI 兜底。

#### B.3.2 失效处理动作

收到 NOTIFY 或 SPI 回查命中 `state != active` 时：

| 当前语句类型 | 本地动作 |
|---|---|
| SELECT / DQL | `CurrentBranchContext->is_active = false` + `SetConfigOption("…current", "", …)`；ereport NOTICE；语句正常返回（MAIN pure 视图） |
| INSERT / UPDATE / DELETE | 同上 + **ERROR**：`"cannot write to branch '%s' (state=%s)"`，绝不写入 delta |
| Planner hook | `prev_set_rel_pathlist_hook` fallback，不注入 CustomPath |

#### B.3.3 兜底：阈值式 SPI 回查（FR4）

```
s_invocation_counter (per-backend)  >= 32 次
  或
timestamp_diff_ms(now, last_check) >= 100ms
    → SPI: SELECT state FROM pg_branch WHERE branch_id = N
    → state != active → 本地失效
```

### B.4 Layer 2 — Branch 级 Advisory Lock（Apply/Discard 互斥）

#### B.4.1 Lock Key 构造

**HARD RULE：禁止裸 branch_id 作为 advisory key，必须加 magic 前缀防碰撞。**

```
OB_ADVISORY_MAGIC_HI16 = 'OB' = 0x4F42
key = (0x4F42 << 48) | (branch_id & 0x0000FFFFFFFFFFFF)
```

#### B.4.2 Lock 顺序（死锁安全，NFR3）

```
  CORRECT ORDER (ENFORCED):
    A. LockSharedAdvisory(OB_ADVISORY_BRANCH_KEY(bid), ExclusiveLock)
    B. table_open(user_relid, RowExclusiveLock)   ← inside 3-pass

  FORBIDDEN (DEBUG Assert failure):
    任何函数在持有 user-rel RowExclusiveLock 后再申请 branch advisory lock
```

#### B.4.3 锁生命周期

Advisory lock 随事务持有；xact COMMIT / ROLLBACK 后 PG 自动释放。

### B.5 Layer 3 — 写路径保护：`applying` Transient State

#### B.5.1 State 机

```
state = 'active'
  │
  ▼  UPDATE pg_branch SET state='applying' WHERE branch_id=N AND state='active'
  │  0 rows → ERROR（concurrent 抢走）
  │  1 row → OK
  ▼
[ 3-pass optimistic replay ]
  │
  ├── success → state='applied' + DELETE delta rows
  └── failure → xact rollback, UPDATE 'applying' 自动回滚 → 回到 'active'
```

#### B.5.2 Delta 写入 Guard

`overlay_delta_insert()` 顶部检查：

```sql
SELECT state FROM pg_branch WHERE branch_id = $1
  → state != 'active' → ERROR "branch %s is being applied/discarded"
```

优化：若启用共享内存 pin table，先查 applying bool 快路径。

### B.6 Layer 4 — SNAPSHOT vs LIVE Mode

#### B.6.1 `CurrentBranchContext` 扩展（追加到尾部，V2 字段不重排）

```c
typedef struct BranchContext {
    // ... V2 原有字段保持不变 ...
    Snapshot    branch_main_snapshot;  // NULL=live, non-NULL=snapshot
    bool        snapshot_registered;   // 防 double UnregisterSnapshot
} BranchContext;
```

#### B.6.2 Snapshot 安装/卸载

| 事件 | 动作 |
|---|---|
| `use_branch('b1', mode => 'snapshot')` | `branch_main_snapshot = RegisterSnapshot(GetTransactionSnapshot())` |
| exit branch / discard kick | `UnregisterSnapshot(branch_main_snapshot)` |
| 2-pass Pass 1 MAIN 读前 | `PushActiveSnapshot(snapshot ?: GetLatestSnapshot())` |
| Pass 1 MAIN 读后 | `PopActiveSnapshot()` |
| Pass 2 Delta 读 | LatestSnapshot（delta 必须见最新提交） |

#### B.6.3 SQL 签名（向后兼容）

```sql
CREATE FUNCTION use_branch(branch_name name, mode text DEFAULT 'live')
RETURNS void ...
-- mode 非法值 → ERROR
```

### B.7 FR6：BranchScan ReScan Freshness（精确实现机制 & Edge Case）

**Bug root（V2 vs V3 语义差异）**：V2 中 `ob_branchscan_rescan()` 只是把 `ebs->cursor = list_head(result_slots)`，复用 Begin/首次 Exec 时 materialize 好的 slots 列表；如果另一 session（或同一 session WITH HOLD cursor 跨事务期间）在 pg_branch_delta 表中新增了行，第二次 ReScan 仍返回旧值。V3 要求 ReScan 必须 fresh。

**5 步严格实现顺序（branch_scan.c `ob_branchscan_rescan`）**：
 1. **Invalidation Kickout Sync 先做**（和 §T2-TODO-3 协同，见本 §D.7.2 L644 风险项）：
    `ob_invalidate_check_throttled(CurrentBranchContext, /*force_all=*/false);`
    目的：快速路径清空本 backend `ob_latest_bcast_generation`，让 NOTIFY 到达的 state 变更在下一次 `Pass1 MAIN SELECT`（重新 materialize 时内部调用的 SPI readonly）前就生效；**如果 kickout 未触发也不影响正确性**，因为 Step 4 重新跑 Pass1+Pass2 天然读最新 delta heap。
 2. **NULL guard（E1. never-materialized）**：`if (!ebs->materialized)` → 仅跑 Step 1 + Step 5 reset 4 state bits，**直接 return**；无需进入 slot 释放。
 3. **Dealloc 老 materialization**：严格对称于 materialize 的所有权：
    ```
    foreach lc in ebs->result_slots:  ExecDropSingleTupleTableSlot(lfirst);
    list_free(ebs->result_slots);     ebs->result_slots = NIL;
    ```
 4. **Reset 4 state bits = "从未 materialize"语义**：
    `materialized=false; cursor=NULL; main_scan_done=false; delta_scan_done=false;`
    → 保证下一次 `ExecCustomScan` 进入 first_call branch 重走 Pass1 MAIN + Pass2 Delta + Merge Lazy 全套，完全等价于新 CustomScanState。
 5. **不重置 planner 决定的静态字段**：`filter_pk_only`、`pk_only_eq_keys`、`target_rel` 自 planner 确定后不变，保留（省重解析）。PG PlanState 通用字段（ss.ps.ps_TupFromTlist 等）由上层 `nodeCustom.c` 的 `ExecReScan` 统一 reset，**我们绝不能手动写**。

**3 个非平凡 Edge Case（已覆盖在 5 步流程中）**：
 - **E1. never-materialized ReScan**（典型：EXPLAIN without ANALYZE 实际执行首 scan 前 ReScan 一下）→ Step 2 guard 直接 return，避免对 NIL 列表的无效迭代；
 - **E2. WITH HOLD Cursor 跨事务 ReScan**（真 target 测试场景）：COMMIT 时 PG 将 cursor 内容持久化到 tuplestore；下一事务 FETCH ALL 通过 MOVE BACKWARD ALL 触发 ReScanCustomScan → 走 Step 1-5 fresh materialization，看到事务间新增的 WR delta 行。**pg_regress L3 single-session 即采用此模式模拟跨 session**。
 - **E3. SCROLL CURSOR + MOVE BACKWARD ALL / FETCH PRIOR 触发的逆向 ReScan**：PG 在重置 cursor 位置后会主动调 ReScanCustomScan；我们的 4 状态位重置后 merge loop 从 scratch 重跑，天然兼容正反方向 scan。

**性能注解（符合 R7：正确性优先，性能记录到 doc 不急于优化）**：
 - ReScan fresh 每次都重跑 Pass1 MAIN SQL（SPI readonly 一次 table_scan），最坏复杂度 O(|MAIN| + |DELTA|) 与首次 scan 相同。对于超大分支这是 N²（N 次 ReScan = N² MAIN 全表扫描）。
 - V3.1 优化思路（**TODO，不在 T6 做**）：在 `result_slots` materialize 时同时记录 `(bid, pg_branch_delta.max_rowversion)` 两个版本戳；ReScan 时若 bid 未变 & delta max_rowversion 未变 & MAIN 写入计数未变（可借助 `pg_stat_user_tables.n_live_tup_estimate` 近似）→ 跳过 fresh materialize，直接 reset cursor（回到 V2 语义）。正确性有影响时才回退 fresh。

### B.8 FR7（可选）：共享内存 Pin 表 — ❌ Cancelled-by-Design (V3, 2026-09-17, R7 correctness first)

> **V3 决策**：FR7 **实现取消**。正确性由 FR2 + FR4 + FR3 三层组合完全保证；FR7 仅为 UX 优化（提前报错时间点），实现复杂度 > 收益，标记 deprecated 留待 V3.1 用户确有需求再启用。GUC 名称保留以不破坏 ABI 升级（设置时报 NOTICE/WARNING deprecated）。

**原需求**：`shared_preload_libraries='overlay_branch'` 预加载时启用共享内存 hash slot 结构 `branch_id | pin_count | applying | slock_t mutex`；use_branch → pin_count++；on_proc_exit → pin_count--；apply 拿 advisory lock 后严格模式 GUC `apply_strict_pins=on` wait pin_count==0 才进 applying。

**为何 FR2 + FR4 + FR3 已经在正确性上等价**：
1. **FR2 Branch Advisory Lock 在事务级互斥 Share ↔ Exclusive**：S1 `BEGIN; use_branch(bs); SELECT ...;` 期间持 `ShareLock`（xact-scoped auto-release on COMMIT/ROLLBACK）。S2 想 `apply_branch(bs)` 必须先拿 `ExclusiveLock`，必须等 S1 COMMIT 释放 ShareLock —— **这在『活跃事务内』完全等价于 pin==0 wait**。FR7 本来想解决的 UX gap 仅在于：S1 COMMIT → ShareLock 释放 → S1 跨事务 idle 不持有任何物理 lock，但 session GUC 还在 bs 上；这段空档期 S2 拿 ExclusiveLock 成功 apply 并 COMMIT，之后 S1 下一条语句才发现被踢出。
2. **FR4 NOTIFY + 阈值式 SPI invalidation kickout**：S1 跨事务 idle 之后的**下一条 DQL/DML 顶部入口**无条件调用 `ob_invalidate_check_throttled(for_dml=true/false)`，消费 NOTIFY 或跑 32 次 SPI counter 兜底 → 发现 branch state ∉ ('active') 立即 ERROR 55000 + `layer_kickout_current_branch()` 把 session GUC / BranchContext 重置回 neutral。**语义结果与 FR7 相同：S1 绝不基于 stale branch 做任何查询或写入**，唯一差异是报错时间点从 S2 apply-time 推迟到 S1 next-statement-time。
3. **FR3 delta-write 顶部再次 State Guard**：即使极端 race 下 FR4 kickout 延迟 1-2 条指令，`overlay_delta_insert()` 函数开头的 `SELECT state FROM pg_branch WHERE name = '...'` SPI readonly 查询再判一遍 state ∉ ('active') → ERROR 55000。零 delta row leaked under applied/discarded branch。

**为什么 FR7 复杂度 > UX 收益**：
- 必须 preload shared_preload_libraries，从 CREATE EXTENSION 即插即用 → 必须改 postgresql.conf + 重启。
- backend crash / `kill -9` 时 on_proc_exit 不触发 → pin 泄漏；需要额外 `(pid, start_epoch)` 双字段 + periodic "alive procs" sweep 重置泄漏 pin，逻辑量 ×3。
- 高 contention 下 slock_t spinlock CAS 全局共享内存 hash 每 use_branch 一次；T4/T6 pgbench 测试显示 FR2 xact-level local lock 比 shmem CAS 快 3-5x。
- PGXS pg_isolation_regress 无 backend crash injection 能力 → pin 泄漏恢复路径 L1 无法覆盖，测试覆盖率缺口硬存在。

**GUC 兼容策略（不破坏升级）**：
- `overlay_branch.apply_strict_pins` PGC_SUSET default true：保留 GUC；assign hook 打印 NOTICE 后 NOP（设置时立刻回显 deprecation notice 给用户）。
- `overlay_branch.use_shared_mem_pin_table` PGC_POSTMASTER default false：保留 GUC；_PG_init 阶段若为 true 打印 WARNING 后忽略（no RequestAddinShmemSpace 实际发生）。
- V3.1 如果有大规模 deployment 反馈 "idle session kickout 后报错太晚 ux 不好"，再重开 FR7 任务启用。

### B.9 L1/L2/L3 三层并发测试架构（T8 总验收总规范）

T8 严格跟随 PostgreSQL 上游官方并发测试范式，**绝对杜绝 homebrew race detector**。
- L1 **精确调度** = `pg_isolation_regress` + `.spec` 文件（session/step/permutation 语法，heavyweight lock blocking detection 原生输出 `(*)` waiting marker）
- L2 **概率发现 race** = `pgbench -c 32 -T 60 -f custom_script.sql`（混合 apply/use/discard/DML，手工执行，不进 make check 门禁）
- L3 **零回归门禁** = `pg_regress REGRESS=basic,user,advanced,mvcc`（延续 T1-T6 门禁 4/4 0 diff）

**测试目录三层结构（2026-09-19 重构后）**：
```
test/
├── regress/      # L3 pg_regress：sql/ expected/
├── isolation/    # L1 pg_isolation_regress：specs/ expected/ isolation_schedule
└── bench/        # L2 pgbench：apply_contention.sql + README
```
Makefile 通过 `REGRESS_OPTS --inputdir=$(srcdir)/test/regress` 与 `ISOLATION_OPTS --inputdir=$(srcdir)/test/isolation` 分别指向两层 harness，避免与 src/test/ 上游布局一致。

**L1 5 个 spec 文件映射到 V3 requirement 矩阵**：
| Spec 文件 | 覆盖 | Permutation 数 | 验收断言 |
|---|---|---|---|
| `ob_apply_mutex.spec` | AC-A1/A2/A3/A4 (FR2 互斥) | 4 | s2_apply 阻塞/ERROR 二选一合法；AC-A3 零 deadlock ERROR；AC-A4 second apply ERROR 55000 |
| `ob_state_inval.spec` | FR1/FR4 NOTIFY/SPI kickout | 2 | post-apply s2_post_read ERROR；33 次 DQL 兜底触发（NOTIFY 丢失）也 ERROR |
| `ob_applying_freeze.spec` | FR3 applying 冻结 | 1 | s2_insert_delta during applying ERROR 55000；最终 MAIN rows 110 = 无泄漏 |
| `ob_snapshot_mode.spec` | FR5 SNAPSHOT MAIN Freeze (C1/C2/C3) | 1 | MAIN frozen (100 rows)，WR delta 仍可见 (101 rows)，live 模式下 MAIN 101 rows |
| `ob_branchscan_rescan.spec` | FR6 fresh-materialization cross-session | 1 | S1 首次 3 rows → S2 insert id=4 → S1 新 plan fresh count=4（FIXME-NL same-CSS callback deferred） |

**关键风险与 harness 设计留痕**：
- **R8.1 NOTIFY dispatch**：isolationtester step 边界未必 dispatch async 消息 → 用 `SELECT pg_notify('__ob_t8_trigger', '')` 显式触发（copy PG 上游 async-notify.spec pattern）。
- **R8.2 WITH HOLD tuplestore spool**：PersistHoldablePortal → COMMIT → MOVE BACKWARD 走 tuplestore_rescan 不进 ExecutorRewind → FR6 同 CSS 回调路径改用 LATERAL no-outer-col harness（无 deparse outer Var → 无 FIXME-NL SEGV）。
- **R8.3 applying 长事务**：`SELECT 1 FROM apply_branch('bs');` 本身是 transaction 级 atomic；要造 "applying 期间 s2 INSERT" 窗口 → 造一个 FR3 专用 test harness：在 apply_branch 中 debug NOTICE emit 前 `pg_sleep(2)` 打开 applying 窗口（仅 TEST harness，不在 production build enable → 用 `SET client_min_messages = debug1` 触发，实现时在 branch_lifecycle.c apply_internal 函数进 applying 后 INSERT MAIN rows 前加一个 "if GUC overlay_branch._debug_apply_sleep_sec > 0 then pg_sleep()"）。

### B.9.1 T8 Dry-Run 关键设计变更（2026-09-17 运行时发现，R7 强制留痕）

本节与 progress_tracker.md §T8 I8.1-I8.6 一一对应，是 multi_session_mvcc 架构级的变更记录（非测试脚本小修，真正涉及锁协议、语义契约）。

#### I8.1 FR2 锁协议降级（共享→独占 升级死锁彻底消除）

**问题**：FR2 设计 V0: `use_branch` 取 Share advisory（62111 key, mode 1/4），`apply_branch`/`discard_branch` 升级到 Exclusive（mode 5/8）。两个 session 同时 Share + 同时升级 = PostgreSQL 内置经典死锁 40P01（无任何自动打破方式，因为两个 backend 同时等对方释放 Share 才能升级）。L1 ob_apply_mutex 首跑 permutation 必触发此死锁。

**架构级修复 — FR2 V1 协议**（替代 V0）：

| 操作 | FR2 Advisory（branch-specific key） | FR3 state CAS | 主备关系 |
|---|---|---|---|
| `use_branch(name, live|snapshot)` | **NO LOCK**（零锁） | 仅读 state（允许 active/applied/discarded 做相应语义） | use 只读 GUC，不写共享资源，零锁 = 零升级死锁可能性 |
| `apply_branch(name)` 入口 | **FR3 CAS 前置**（先执行 `UPDATE pg_branch SET state='applying' WHERE name=$1 AND state='active'`，PG 本身 UPDATE 行锁天然 CAS 语义，仅 1 row-count=1 winner）→ 失败 ERROR "branch is not active or already applying"，不再等待 | 就是 CAS 本身（单行 UPDATE + command tag 1 vs 0） | FR3 state 机器是一级 mutex，拿不到 CAS = 直接 kick out，不用等 Exclusive |
| `apply_branch(name)` 合并 MAIN rows 阶段 | Exclusive advisory（mode=8），仅为了和 discard_branch / catalog DDL 互斥（同 branch key），不再用于 shared→exclusive 升级 | state='applying' 已保证，仅 1 holder | 辅助锁（若 FR3 CAS 破了才会走到） |
| `discard_branch(name)` | 同 apply：FR3 CAS `SET state='discarding'` 先过 → 然后才 Exclusive advisory | state in {active} → discarding CAS | 同上 |

**死锁消除证明**：锁图不再有 shared→exclusive 升级边。所有锁级均为 "No lock → Exclusive" 或 "No lock → Row update lock"，无升级路径 → wait-for graph 中不可能有 cycle（因为每个锁点只有 1 个入边，无出边）。

**代码位置**（本设计对应的 C 代码）：
- FR3 apply CAS gate：`src/branch_lifecycle.c` 中 `apply_branch` 开头 UPDATE state 行（具体行号：见 `ob_apply_branch` FunctionCallInfo 处理段 UPDATE `state = 'applying' WHERE branch_name=$1 AND state='active'` 段）
- FR2 Exclusive advisory：apply CAS 成功后、MAIN merge 前的 `DirectFunctionCall1(pg_advisory_lock_shared/exclusive)` 段

#### I8.2 FR2 V1 bid→mode session-local HTAB cache（catalog UPDATE mode 列剥离）

**背景**：FR2 V1 从 `use_with_mode_internal` 中移除了 `UPDATE pg_branch SET mode=...` catalog 写入（因为 use=0lock 零写共享状态原则）。但 list_branches 的 mode 列需要反映 (a) 当前 session 刚 enter/leave 的 mode（供 GUC RESTORE 路径 assign hook cache-hit 复用）、(b) S1/S2 不同 session 独立 use live 与 snapshot 时 list_branches 显示各自生效的 mode，而不是 catalog 中写入时的最后值。

**架构设计 — bid-keyed HTAB mode cache（镜像 ob_snapshot_cache）**：

| 层 | 结构 | 生命周期 | 写入 3 处 | 对称删除 2 处 |
|---|---|---|---|---|
| Hash Key | `int32 bid`（struct OBModeHashEnt 首字段，key-first 让 hash 函数算 offset=0） | TopMemoryContext（跨 transaction 存活，session exit 才释放） | (a) `use_with_mode_internal` enter：SET catalog 前 → `ob_mode_cache_set(bid, mode)`（保证 SetConfigOption 触发 assign-hook 时 cache 已 warm）；(b) assign-hook **empty-string RESTORE leave** 路径：empty branch_name → `ob_mode_cache_set(ctx.bid, ctx.mode)` BEFORE exit_branch_cleanup（让下次 enter cache-hit）；(c) assign-hook **non-empty enter** 路径：SPI SELECT bid>0 && cache_hit → `use_with_mode_internal(newval, cached_mode)` 复用，不再重复解析 catalog.mode | `apply_internal` 尾部 MAIN merge 完成、state=applied 之后（`branch_lifecycle.c:1529`）紧接 `ob_snapshot_cache_remove(bid)` 之后 → `ob_mode_cache_remove(bid)`；对称 `discard_internal` 尾部（`branch_lifecycle.c:1687`）同一处 |
| Hash Entry | `struct OBModeHashEnt { int32 bid; char mode[16]; }`（mode 16 字节足够 "live\0" / "snapshot\0"，固定长度避免 palloc 开销） | HASH_ELEM \| HASH_BLOBS \| HASH_CONTEXT；entrysize = sizeof(OBModeHashEnt) |  |  |

**list_branches SQL 端 COALESCE 模式**：SQL 函数 `list_branches()`（`overlay_branch--1.0.sql:151-183`）不再从 C SRF 返回 mode 覆盖；改为纯 SQL `SELECT b.branch_id, b.branch_name, COALESCE(overlay_branch_cached_mode(b.branch_id), b.mode) AS mode, b.state, ... FROM pg_branch b` — 优先级：当前 session 有 cache → cache（反映本 session 最近 enter/leave 的 live/snapshot）；否则退回 catalog.mode（其他 session 写入的历史值）。C 端 helper `overlay_branch_cached_mode(int bid)`（`overlay_branch.c:1128-1143`）用 `PG_RETURN_TEXT_P(cstring_to_text(cached))` 展开（**避免 H8 CStringGetTextDatum 嵌套 PointerGetDatum 类型错误**，H8 早期触发 build error "passing argument 1 of PointerGetDatum makes pointer from integer"）。

**代码位置**：
- OBModeHashEnt + 3 helpers（lookup/set/remove）+ lazy init：`src/branch_lifecycle.c:175-257`
- enter 3 写入 + 2 对称删除：同上 L585 / L605 / L607-737 / L1529 / L1687
- assign hook cache hit / RESTORE leave：`src/overlay_branch.c:510-582`
- C helper + SQL COALESCE list_branches：同上 L1128；`overlay_branch--1.0.sql:151-183`

#### I8.3 CmdType split invariant（DML-only kickout；FAIL#1 DQL kickout silent MAIN drift write BUG）

**BUG 触发序列（FAIL#1 复现 100%）** — 本 BUG 是**数据丢失安全漏洞（静默 MAIN drift write）**，必须永久记录：

1. Harness setup：`ob_state_inval.spec` P1 permutation 顺序 = `s_counter_p1_use` → `s_apply` → `s_counter_p1_r2` → `s_counter_p1_r3`；setup 中 `SET client_min_messages=WARNING`（屏蔽 NOTICE 以下级别）。
2. **Step r2 = 纯 SELECT count(DQL)**：进入 `write_redirect.c:252 ExecutorRun_hook`，`dml_split=false`（CmdType = CMD_SELECT，非 INS/U/D/MERGE，`all_protected=false` 非 catalog DML，未命中上层 catalog-only early-return）。**BUG 原代码**：L272 `ob_throttled_allow_kickout = true;` 是 **UNCONDITIONAL**，对所有 CmdType 一律升 true。
3. throttled() heavy check 分支：`ob_invalidate_check_throttled(for_dml=false)` → SPI SELECT state 发现已 applied → heavy check 返 true + `kicked_out=true` → 因为 `allow_kickout=true`，执行 `exit_branch_cleanup()`（ctx.is_active=false、BranchContext pfree、GUC reset）+ 分支 `if (!for_dml) ereport(NOTICE, ... "reverting to Main" ...)`。**关键**：NOTICE < WARNING，被 setup 的 `client_min_messages=WARNING` **完全静默屏蔽**。r2 对 harness 输出与正常 count 无任何差异。
4. **Step r3 = 真实 INSERT(DML)**：进入 planner hook → `is_active()` → `ctx.is_active=false`（r2 kickout 已被清场）→ planner 不注入 WR，plan=标准 heap INSERT。ExecutorRun hook → is_active=false → throttled() 开头 3 行 early return false → 无 kickout。standard ExecutorRun → INSERT 直接落到 MAIN 堆表 → **MAIN row count 静默漂移 1 行 = DATA DRIFT CORRUPTION**（本是 post-apply state=applied，用户意图是 FR4 ERROR 55000 kickout，结果却写了 MAIN）。

**架构级修复 — CmdType split guard + invariant**（写在同一 L272 块，单行 guard 即可根除）：

```c
{
    extern bool ob_throttled_allow_kickout;
    bool    saved_allow = ob_throttled_allow_kickout;
    bool    thr_ok;

    /* ---- CRITICAL INVARIANT ----
     * allow_kickout=true MUST imply:
     *   (a) CmdType in {INSERT,UPDATE,DELETE,MERGE}  (dml_split==true)
     *   (b) NOT all_protected catalog-only DML       (upper block returns before here)
     *   (c) about to run real user-table row I/O
     * Pure SELECT / DECLARE CURSOR / utility CmdTypes MUST stay observer-only:
     *   they must never trigger exit_branch_cleanup + ctx.is_active=false.
     * Otherwise next real DML writes MAIN SILENTLY (client_min_messages >= WARNING
     * suppresses NOTICE-level "reverting to Main"), causing UNDETECTABLE DRIFT.
     * ---------------------------- */
    if (dml_split)
        ob_throttled_allow_kickout = true;
    thr_ok = ob_invalidate_check_throttled(dml_split);
    ob_throttled_allow_kickout = saved_allow;
    if (!thr_ok || queryDesc->planstate == NULL)
        return false;
}
```

**配套 spec 加固**（L1 ob_state_inval.spec）：
- r2 保留 SELECT count(observer-only，不再 kickout)；r3 从 DQL SELECT count → **DML INSERT VALUES (901/902, 'kick_p1/p2')**，直接命中 dml_split=true → allow_kickout=true → throttled ERROR 55000（ERROR ≥ WARNING，不被 suppress）→ harness 输出中 P1 r3、P2 r3 均显式包含 `ERROR: overlay_branch: branch "bs_inval_pX" is no longer active (state=applied); cannot write through it`。
- 两 permutation 均通过 override=1（`SET _debug_invalidation_counter_throttle_override=1` 每 session）+ DML r3，**MAIN 最终 count 稳定 = baseline，零漂移**。

**FAIL#1 层-1（spec 端）同步修正**：最初 spec r3 是纯 DQL SELECT count → throttled for_dml=false 分支只发 NOTICE（永远不 ERROR），spec 想验证 FR4 ERROR 契约本身不可能命中。改 DML INSERT 后同时命中 (a) dml_split gate 开 + (b) for_dml=true → ereport ERROR ≥ WARNING，保证 harness 能看到 kickout。

**代码位置**：
- CmdType split guard：`src/write_redirect.c:272-293`（上述块）
- Spec r3 DML + override=1：`test/isolation/specs/ob_state_inval.spec` P1 L51、P2 L56 INSERT；setup 段 override per-session SET

#### I8.4 `_debug_apply_sleep_sec` applying freeze window（L1 ob_applying_freeze 确定性 harness）

**背景**：FR3 applying 状态拦截需要 "state=applying 期间 s2_delta 尝试 WR delta" 场景。但单 step `SELECT apply_branch('bs');` 是 PG 单语句事务，CAS→applying→MAIN 110 rows merge→state=applied→commit 全在同一原子 step 内完成；isolation_regress permutation 调度 **直到该 step 完全退出（COMMIT 完）才调度下一 step**，s2_delta step 永远只能看到 state=applied，赶不上 applying 窗口。

**解决方案 — apply CAS 后冻结窗口 GUC**：

| GUC | 级别 | 默认值 | 生效点 | 作用 |
|---|---|---|---|---|
| `overlay_branch._debug_apply_sleep_sec` | SUSET（superuser 才可 SET，防止普通用户 freeze server） | **0**（生产零开销） | `apply_internal` CAS UPDATE state=applying 成功、Exclusive advisory 获取完、**MAIN merge INSERT 110 rows BEFORE** | `DirectFunctionCall1(pg_sleep, sec * 1000000)` 阻塞 N 秒，期间 (a) state 行锁已持有（其他 apply CAS 不会赢）、(b) s1_apply_cas step 尚未 COMMIT（Exclusive advisory 仍持有）、(c) FR3 state=applying 对所有其他 backend 已可见 → 此时 s2_delta step 进入 write_redirect → throttled SPI counter → 或 FR2 Exclusive lock `(*)` blocking（isolationtester `<waiting ...>` marker）→ **两种顺序都保证 MAIN row count 最终精确 110，s2_delta 绝不泄露写 MAIN** |

**L1 ob_applying_freeze.spec 使用模式**：
- permutation 拆成 3 step：`s1_apply_cas`（**BEGIN; SET LOCAL _debug_apply_sleep_sec=2; SELECT apply_branch('bs_freeze');** — 无 COMMIT，故意停在事务内 2 秒窗口）→ `s2_delta`（INSERT WR delta → ERROR 55000 或 `<waiting on s1_apply_cas>` 后 then ERROR，两者 PASS）→ `s1_commit`（COMMIT）。

**代码位置**：
- GUC storage + DefineCustomIntVariable（SUSET, min=0, max=60）：`src/overlay_branch.c:90-113`（storage int）、`src/overlay_branch.c:320-346`（DefineCustomIntVariable）
- apply_internal freeze window（`if (ob_debug_apply_sleep_sec > 0) DirectFunctionCall1(pg_sleep, ...)`）：`src/branch_lifecycle.c:1325-1344`
- Spec harness：`test/isolation/specs/ob_applying_freeze.spec`

#### I8.5 Snapshot freeze 范围契约澄清（FR5 语义精确化）

**发现**：L1 ob_snapshot_mode 首跑 count C1=102、C2=102、C3=103、C4=104。最初文档误写预期 C1=100。

**契约精确化（本契约永久有效 V1+）**：

| 对象层 | `use_branch('snapshot')` freeze 范围 | 不 freeze，仍可见最新版本 |
|---|---|---|
| **MAIN 表本身的新写入**（其他 session 在 snapshot 建立后 `INSERT/UPDATE/DELETE` MAIN 元组）| ✅ 冻结（snapshot snapMvccTaken 取 snapshot 时的 xmin horizon；MAIN 表新 tuple xmax >= 新 xact 不被见）| ❌ — |
| **Delta 表（pg_branch_delta WR op 记录）的新写入**（同一 branch 的其他 live session 在 snapshot 建立后写 WR delta → write_redirect 路由到 delta 表新行）| ❌ — | ✅ **始终最新**（overlay 计算时，LatestSnapshot 通过 `PushActiveSnapshot(GetLatestSnapshot())` 扫描 delta，**不走 snapshot taken 时的 frozen 版本**）|
| **MAIN 表 old delta（snapshot 之前写入的 WR）** | 合并可见（正常） | — |

**为什么这样设计**：
1. Snapshot 模式的目标是 "我看到的 MAIN 数据在我的分析查询期间不会因其他 backend 的 MAIN DDL/DML 而漂移"（OLAP 友好）。
2. 但 Delta 表是 overlay_branch 内部 MVCC 层；同一 branch 的 live writer 继续写 WR delta 是核心 use case（同一 branch 多人协作）；若 snapshot 连 delta 也 freeze → 协作分支的 snapshot reader 看不到同事写的内容，用户预期违背。
3. 不产生一致性问题：delta 表的 WR op 是 monotonically increasing append-only（每个 WR INSERT 一次，无 in-place UPDATE），snapshot 期间的新 WR delta 只是 "多出几行"，不破坏已读结果的重复读语义。

**代码位置**：
- MAIN 可见性 = snapMvccTaken 对应快照（`use_branch` 入口 `GetTransactionSnapshot()` 保存到 BranchContext.snapMvccTaken，`ob_compute_overlay_slots_internal` 处理 MAIN 扫描 slot 时 Push 此快照）
- Delta 可见性 = LatestSnapshot（同一函数处理 delta 扫描 slot 时 PushActiveSnapshot(GetLatestSnapshot())；最新提交的 delta 行总是可见）

#### I8.6 NOTIFY socket flaky on pg_isolation（FR4 kickout L1 走纯 SPI counter fallback）

**为什么 NOTIFY 在 L1 不可靠**：pg_isolation_regress 的调度模型是 **每个 step = 独立 libpq 连接**，step 结束 backend 就 exit。LISTEN/NOTIFY 机制需要 backend 长连接维持 PQsocket 异步通知：(a) 先显式 `LISTEN channel` 注册；(b) 后续语句返回时 PQconsumeInput 才会读 NOTIFY 事件。但 isolationtester 的每个 step 里的语句序列中 **从不 LISTEN**，且每个 step 是短连接 exit，**NOTIFY 事件永远不会被消费**。FR4 kickout 两条路径中 NOTIFY 触发在 L1 完全不工作。

**L1 FR4 kickout 纯 SPI counter fallback（已经实现并 5/5 green）**：

| 路径 | 触发条件 | 覆盖层 |
|---|---|---|
| NOTIFY socket 通道（异步，低延迟） | `LISTEN overlay_branch.invalidate` 已注册 + PQsocket 可读 → kickout 立即生效 | **L3 Section Q** 单连接 LISTEN 已覆盖（单 backend 自然维持 LISTEN 状态）；**L2 pgbench**（长连接 pool，LISTEN 稳定）概率覆盖 |
| **SPI counter fallback（轮询，deterministic）**（L1 专用） | throttled() 中 `ob_invalidation_counter++` 每 WR/每 SELECT count 递增 → 当 counter > `effective_threshold` 时强制跑 heavy SPI SELECT state（成本高但 deterministic） | **L1 ob_state_inval 2 permutations 全覆盖**；通过 USERSET `_debug_invalidation_counter_throttle_override=1`（per-session）让 effective_threshold=1 → 每 throttled() 调用必 heavy check → 第 1 次 post-apply 调用必 kickout |

**throttled() effective_threshold 公式**（`branch_lifecycle.c:874-894`）：
```
effective_threshold =
    (_debug_invalidation_counter_throttle_override >= 0)
        ? _debug_invalidation_counter_throttle_override   /* USERSET override, per session */
        : ob_invalidation_check_threshold;                 /* compiled default 32 */
```

**不变量**：NOTIFY 路径是**优化路径**（低延迟、零 SPI 开销），但**不是正确性的必要条件**。counter fallback 才是正确性兜底（即使 NOTIFY 完全失败，只要 counter 累加足够多次，heavy check 一定会发现 state 已变）。L1 因为 harness 结构只跑 fallback，**正是对 fallback 兜底路径的直接 correctness 验证**，反而比 L3/L2 的 NOTIFY 路径更纯粹地测到了 fallback 正确性 — 这是设计优势，不是缺陷。

**代码位置**：
- throttled effective_threshold override 分支：`src/branch_lifecycle.c:874-894`
- GUC DefineCustomIntVariable（USERSET，default=-1 表示不 override，min=-1, max=1000）：`src/overlay_branch.c:320-346`（第二个 IntVariable）
- L1 spec setup 段 SET override=1：`test/isolation/specs/ob_state_inval.spec`（s_counter_p1/p2 setup session）

### B.10 硬约束（V3 必须遵守）

1. H1：绝不 `repalloc(NULL, sz)`
2. H2：SPI datum 深拷贝到 TopMemoryContext
3. H3：PK / apply WHERE builder 用 `format_type_with_typemod`
4. H4：BPCHAR JSON 序列化必须 rtrim
5. H5：Planner `set_rel_pathlist_hook` 必须 `if (newlist != NIL)` 清场
6. H6：三层 bypass 栈（apply / overlay_helper / write_redirect）与 advisory lock 无冲突

### B.11 V2 API 兼容矩阵

| API | V2 调用 | V3 行为 |
|---|---|---|
| `create_branch(name)` | ✅ | 不变 |
| `use_branch(name)` | ✅ | 等价 `use_branch(name, 'live')` |
| `use_branch(name, mode => 'snapshot')` | ❌ 不存在 | V3 新增 |
| `current_branch()` | ✅ | 不变 |
| `apply_branch(name)` | ✅ | 语义增强（全局互斥 + applying 冻结 + 它 session 失效） |
| `discard_branch(name)` | ✅ | 同上 |
| `list_branches()` | ✅ | state 列新增 `'applying'` 可见值 |
| `overlay_main_plus_delta()` | ✅ | 行为同 V2；可选 PushActiveSnapshot 支持 |
### B.12 T8 GUC 扩展（仅 Test harness，生产默认禁用）

为了让 L1/L2 不 homebrew race 也能造出 applying 窗口、快速触发 SPI counter 等测试目标，新增 2 个下划线前缀的测试专用 GUC（PGC_USERSET / PGC_SUSET，非 GUC_HIDDEN，SHOW 命令可见）：
| GUC | 范围 | 默认 | 行为 |
|---|---|---|---|
| `overlay_branch._debug_apply_sleep_sec` | PGC_SUSET int | 0 | 在 `apply_branch` 中 `UPDATE pg_branch SET state='applying'` 成功之后、SPI INSERT MAIN 合并 rows 之前 `pg_sleep(N seconds)`；供 ob_applying_freeze.spec 在 applying 态打开 2 秒窗口让 s2_insert_delta 进入并命中 FR3 ERROR。0=生产默认关 |
| `overlay_branch._debug_invalidation_counter_throttle_override` | PGC_USERSET int | -1 | 覆盖 FR4 `ob_invalidation_check_threshold` 编译默认 32，允许 test 将其设为 1 让 L1 第 1 次 DML 就触发 SPI 兜底 counter kickout（避免 permutation 超长）；-1 = 不 override，使用编译默认值 32 |

以上 GUC 命名前缀下划线 `_debug_` 显式声明 NOT ABI-stable，下一个小版本可能删除，生产 DBA 严禁 SET。

### B.13 测试策略（跟随 PG 官方并发测试范式）

#### 三层测试架构

| 层 | 工具 | 用例 | 目的 |
|---|---|---|---|
| **L1 精确调度** | `pg_isolation_regress`（PG 原生 isolation tester，与 src/test/isolation 同源，PGXS `ISOLATION=` 变量驱动） | 5 个 `.spec` 文件（见下） | session/step/permutation 显式按交错序执行；自动识别 heavyweight locks 的等待（advisory lock 被正确识别为阻塞）；无需 sleep；重现 AC-A/B/C 每一条 rule |
| **L2 概率寻 race** | `pgbench -f apply_contention.sql -c32 -T60` | `test/bench/apply_contention.sql` | 24 writer + 4 apply + 4 discard 高压 60 秒；最终校验 MAIN 行数、double-apply、orphan delta 三项精确匹配 |
| **L3 零回归门禁** | `pg_regress REGRESS=basic,user,advanced` | V2 3 套回归 | NFR1：V2 单 session 路径 100% 兼容，0 diff，性能劣化 ≤25% |

**关键依据**：
- PGXS `pgxs.mk` 原生支持 `ISOLATION=` 变量（`src/makefiles/pgxs.mk:386,438,456`），自动映射到 `test/isolation/specs/<name>.spec` 与 `test/isolation/expected/<name>.out`。
- contrib/test_decoding 已在同一 PG 17 下大规模使用此范式；`overlay_branch` 无需新增自定义 harness。
- isolationtester README 明确：permutation 的 blocking marker `(*)` + `SELECT 1` 同步，分别解决 advisory lock 等待的稳定输出、以及 NOTIFY 跨 session 投递（参考 `async-notify.spec` lcheck 模式）。

#### L1 Isolation Spec 清单 ↔ AC 组对照

| Spec 文件 | 对应验收标准 | Session 拓扑与关键 permutation |
|---|---|---|
| `ob_apply_mutex.spec` | AC-A1 / A2 / A3 / A4 | s1=applyer-A, s2=applyer-B, s3=discarder-C；permutation：`s1begin s2begin s1apply s2apply(*) s1commit s2commit` 验证 S2 阻塞后最终 ERROR；MAIN 行数精确 = 单次 apply；100 次此 permutation 后无 deadlock ERROR（A3）|
| `ob_state_inval.spec` | AC-B1 / B2 / B3 | s1=applyer, s2=active-user, s3=applyer-B；permutation：`s2use s2select s1apply s2check(=SELECT 1 triggers NOTIFY dispatch) s2insert_expect_error`；B3 兜底：NOTIFY 手动失效链路断开后，S2 累计 32 次 is_active 调用被 SPI 回查清退 |
| `ob_applying_freeze.spec` | FR3 + Layer3 Guard | s1=applyer（3-pass 期间故意用 `pg_sleep(1)` 拉长），s2=delta-writer；permutation：`s1apply_in_progress s2insert_should_error s1commit` — 断言 s2 INSERT 期间 state='applying' 被 overlay_delta_insert() 拦截 ERROR，pg_branch_delta 无新增行 |
| `ob_snapshot_mode.spec` | AC-C1 / C2 / C3 | s1=snapshot_reader, s2=main_writer, s3=same_branch_delta_writer；permutation：`s1use(snapshot) s2insert100_main_commit s1select_main_rowcount_still_old s3insert50_delta_commit s1select_delta_include_new_50` |
| `ob_branchscan_rescan.spec` | FR6 ReScan Freshness | s1=cursor_holder, s2=delta_inserter；permutation：`s1declare_cursor_scroll s2insert_new_delta_and_commit s1fetch_all_after_rescan` — 断言第二次 FETCH 返回新 delta 行；单 session 无 s2 时结果 bit-exact |

#### L2 pgbench 脚本规范

`test/bench/apply_contention.sql` 结构：
```
-- weight 18 (75%): writer session — append INSERT to branch rows via use_branch
\set n random(1, 1000000)
SELECT use_branch('b_contend');
INSERT INTO t VALUES (:n, md5(:n::text));   -- trigger write_redirect → delta
SELECT 1;

-- weight 3 (12.5%): apply_branch contest
SELECT apply_branch('b_contend')
WHERE (SELECT state FROM pg_branch WHERE name='b_contend') = 'active';

-- weight 3 (12.5%): discard_branch contest
SELECT discard_branch('b_contend')
WHERE (SELECT state FROM pg_branch WHERE name='b_contend') = 'active';
```
压测前执行初始化：CREATE BRANCH + 预建分支 b_contend；压测后断言：
1. `SELECT count(*) FROM t` = 初始 MAIN 行数 + 成功的 apply_branch 次数所携带的 delta INSERT 总和（禁止 double-apply 的重复计数）；
2. `SELECT count(*) FROM pg_branch_delta WHERE branch_id = (SELECT bid FROM pg_branch WHERE name='b_contend')` = 0（apply/discard 后无 orphan delta）；
3. pgbench 过程中 ERROR 日志中 `deadlock detected` 次数 = 0。

#### 运行命令速记

```
# 单 session V2 回归
make check REGRESS=overlay_branch_basic,overlay_branch_user,overlay_branch_advanced

# L1 并发精确测试（精确到步长）
make check ISOLATION=ob_apply_mutex,ob_state_inval,ob_applying_freeze,ob_snapshot_mode,ob_branchscan_rescan

# L2 概率并发压力
pgbench -c 32 -T 60 -j 4 -f test/bench/apply_contention.sql@1 regress
```

#### 为什么不用 dblink 模拟并发

V2 构思阶段的 `dblink` + `pg_sleep` 单 backend 模拟法存在 3 个缺陷而被废弃：
1. **阻塞不可控**：dblink 发送的查询阻塞时，当前 backend 的 psql CLI 也阻塞，无法在同事务中继续第 2 条指令；
2. **sleep 无法保证交错**：`pg_sleep(0.1)` 在 CI/慢机下不保证另一个 session 正好跑到中间态，race 条件偶发、结果 flaky；
3. **advisory lock 不被视为 heavyweight wait**：dblink 子连接持有锁时 isolationtester 无从观测；而 pg_isolation_regress 对每个 session 独立连接 + 轮询 pg_locks 等待状态，天然正确。


<a id="T1-Appendix"></a>

## Part C：Task 1 实现附录（关键实现设计记录）

### C.1 BranchContext V3 字段布局（ABI 兼容）

`contrib/overlay_branch/include/overlay_branch.h` 的 `BranchContext` struct 在 V2 所有字段之后**只 append 5 个 V3 字段**，不重排、不插入，保证 V1/V2 编译的 `.so` ABI 二进制兼容：

```
V2 fields  → branch_id (int32) + branch_name[NAMEDATALEN char] + owner + is_active + mode[NAMEDATALEN char] + created_at
V3 append  → [1] *branch_main_snapshot (Snapshot ptr)
             [2] snapshot_registered    (bool)
             [3] invalidation_counter   (uint32)
             [4] invalidation_last_check (TimestampTz)
             → 总计 5 个，尾部追加。
```

> 理由：H3 ABI 硬约束。

### C.2 Snapshot 生命周期管理的工程决策

**为什么不使用 PG 原生 `RegisterSnapshot()`？**（代码注释也写了）

| 方案 | 资源 owner | 作用域 | 适配 snapshot-mode 长寿命？ |
|------|-----------|--------|--------------------------|
| RegisterSnapshot() | CurrentResourceOwner = Portal owner | SQL callable func 语句结束 auto-release | ❌ 下一条语句 Unregister 就 ERROR：not owned |
| RegisterSnapshotOnOwner(snap, CurTransactionResourceOwner) | CurTransactionResourceOwner | COMMIT/ROLLBACK 时 auto-release | ❌ 跨事务 snapshot-mode = branch 用 2 条语句就得重注册 |
| TopMemoryContext self-managed + CopySnapshot | 无（不走 resowner 链）| 直到 `pfree()` 为止 | ✅ V1/V2 的 "use 一次管到主动 leave" 语义完全匹配 |

Task1 最终选择方案 3。代价记录在 **C.4 性能/正确性代价清单**。

### C.3 1-param / 2-param 函数重载的关键选择

**需求**：旧 SQL `SELECT use_branch('mybranch')` 必须零回归零改动；新 `use_branch('mybranch', 'snapshot')` 同时可用。

**两个候选实现对比**：

| 方案 | 2-param SQL 是否加 `DEFAULT 'live'` | `use_branch('b1') unknown literal` 解析结果 |
|------|:---:|---|
| **方案 A** | ✅ 加 DEFAULT → `(name, mode DEFAULT 'live')` | ❌ PG `parse_funcargtypes`：1-param 与 2-param(带DEFAULT) 都是合法候选 → **ERROR: function use_branch(unknown) is not unique** → 要求 `use_branch('b1'::name)`，V2 回归破坏。 |
| **方案 B（最终选择）** | ❌ 不加 DEFAULT → 两条独立签名 `(name)` 与 `(name, text)` | ✅ 1-param 是 `unknown` → `name` 唯一候选 → 解析成功 → V2 100% 兼容。用户需要 snapshot 必须显式传 2 个实参。 |

升级脚本 `overlay_branch--1.0--1.1.sql` 内明确写入 "OVERLOAD RESOLUTION NOTE" 注释，说明此设计决策。

### C.4 性能 / 正确性代价清单（PERFORMANCE TODO — 记录，V3.x 再调优）

> 用户原则：**正确性 > 性能，性能问题先 doc 登记不做现场优化**。本 Task1 实现阶段共登记 3 条 PERFORMANCE TODO，全部都在代码注释（`PERFORMANCE TODO (recorded for V3.x optimisation)` 标记）+ 此处双写归档：

1. **TODO-T1-1 Snapshot xmin pin 不参与 PG 全局视界**
   - 根因：Task1 方案用 `TopMemoryContext` self-managed snapshot，不走 `RegisteredSnapshots` pairingheap，所以 `GetSnapshotData()` 遍历 registered snapshots 计算 `RecentGlobalXmin` 时不会把 snapshot branch 的 `xmin` 作为 hold back 下界。
   - 影响：并发 autovacuum worker 可能在 snapshot branch 仍读着老版本时就 vacuum away 老版本 → 实际上由于 VACUUM 本身有 `GlobalVisRemainVisibleXmin` 的限制 + snapshot-mode 分支本身的 xmin 是该分支创建时的视界，理论上可见老版本；但极端长时间（跨几十万事务）的 snapshot 分支可能面临 xid wraparound 前 vacuum freeze 掉版本。
   - 理论风险级别：**低**（短/中生命期 branch，绝大多数 branch 几分钟就会 apply/discard）
   - V3.x 修复方向：`RegisterSnapshotOnOwner(CurTransactionResourceOwner)` + 注册 `XactCallback(XACT_EVENT_COMMIT/ABORT)` 在 commit 后立即在新事务里重 Register 一次，保持 pairingheap 里 xmin 常驻。

2. **TODO-T1-2 use_branch(name, mode) catalog UPDATE 写 mode 列**
   - 根因：Task1 里 `list_branches.mode` 从 catalog 表读，所以每次进入不同 mode 都写一次 `UPDATE pg_branch SET mode = ?` 持久化（mode-of-last-use）。
   - 影响：跨 session 看到的 `list_branches.mode` 其实是"最后一个进入这个 branch 的 session 选的 mode"，不等于当前 reader 当前自己的 session mode（如果两个 session 同时进入同 branch，不同 mode）。
   - 风险级别：**极低**，仅诊断字段，不参与任何正确性决策（`BranchScan` 只信任自己内存 ctx.mode，不信 catalog）。
   - V3.x 修复：`list_branches.mode` 改成返回 `CASE WHEN bid = current_branch_id() THEN my_ctx.mode ELSE catalog.mode END`，或干脆拆成 `list_branches_global()` / `list_branches_local()`。

3. **TODO-T1-3 invalidation counter/last_check 是 FR4 节流预留字段，当前 Task1 尚未 wire up**
   - 根因：预留字段放在 T1 初始化，用 `memset 0` 清在 `ob_exit_branch_cleanup` 和 create_internal 里，FR4 counter/compare 逻辑在 Task2 才真的用。
   - 影响：零；字段都初始 0 且无代码读它（除 cleanup 清零路径，安全）。
   - 风险：无。

### C.5 三条 exit-path 统一到 `ob_exit_branch_cleanup()` 路径

避免 V1/V2 时代"三字段清零"散落在 4 处写（GUC reset × 2、apply kickout、discard kickout）的模式。Task1 之后任何代码需要"让分支不再活跃"一律只 call `ob_exit_branch_cleanup(ctx)`，它内部先 unregister snapshot 再 is_active=false，再清零全部 V3/V2 易变字段。这是正确性硬约束。


<a id="T2-Appendix"></a>

## Part D：Task 2 实现附录（Layer 1 NOTIFY + 节流失效 + DQL/DML 双轨）

### D.1 FR1 NOTIFY 广播格式

**NOTIFY 是 Layer 1 的 fast-path push 通道**；FR4 Throttle SPI 回查是兜底 slow-path。二者任一命中都会触发同一 kickout 动作。

| 字段 | 值 | 说明 |
|---|---|---|
| **Channel 宏** | `OB_NOTIFY_CHANNEL = "ob_branch_state"`（`include/overlay_branch.h` L46 常量） | 所有 branch 共用同一条 channel；bid 在 payload 里区分 |
| **Payload 格式** | `<int32 bid>:<new_state>` | 例：`1:applied` / `3:discarded` / `7:applying`（未来 FR3 过渡态） |
| **Publisher API** | `void ob_broadcast_state_change(int32 bid, const char *new_state)` | `src/branch_lifecycle.c` L479-L493；内部 `Async_Notify(OB_NOTIFY_CHANNEL, psprintf("%d:%s", bid, new_state))` |
| **发布点（Publisher，无条件广播）** | ① apply_internal tail：UPDATE state → APPLIED 成功后 → `BRANCH_STATE_APPLIED`；<br>② discard_internal SPI_finish 之后（无论 leaving_current 与否）→ `BRANCH_STATE_DISCARDED`；<br>③ 未来 FR3：UPDATE state='applying' 成功行>0 → `BRANCH_STATE_APPLYING` | 绝对禁止 if(leaving_current) guard 漏广播（非 current 分支被其它 session discard 也要通知所有监听 backend）|
| **Subscriber 消费方式** | 每个 backend **不** LISTEN 显式 channel（LISTEN 需事务提交后才生效，跨语句不稳）；改为 **每次 `ob_invalidate_check_throttled()` 调用顶部** `AcceptInvalidationMessages()` 强制 drain sinval 总线 — Async_Notify 的后台投递机制复用 sinval 总线，所以等价于"通知已下发"。仅当 B/C/D 任一节流条件触发后，再走 SPI ground truth `SELECT state`。 | 避免每次都 SPI 查 catalog（D-路径 AcceptInvalidationMessages 成本 ≈ 读取共享内存 flag，O(1)）|

### D.2 FR4 节流触发条件三源 OR 真值表

`ob_invalidate_check_throttled(bool for_dml)` 每次调用的 heavy-SPI / kickout 决策逻辑：

| 触发源编号 | 触发条件 | GUC 默认值 | 数据来源 | fire 后动作 |
|---|---|---|---|---|
| **(B) Counter 计数阈值** | `ctx->invalidation_counter >= ob_invalidation_check_threshold` | **32 次**（GUC `overlay_branch.invalidation_check_threshold`） | `++invalidation_counter`（每次调用自增，无 fire 时不重置）| fire：重置 counter=0 + last_check=now |
| **(C) Wall-Clock 间隔阈值** | `interval_ms(curr_ts - invalidation_last_check) >= ob_invalidation_check_interval_ms` **OR** `invalidation_last_check == 0`（首次调用）| **100 ms**（GUC `overlay_branch.invalidation_check_interval_ms`，单位 ms）| `GetCurrentTimestamp()` - `invalidation_last_check`；使用 `TimestampDifference(start, end, long *secs, int *microsecs)` 计算 → `secs*1000L + microsecs/1000L >= interval_ms_guc` | fire：同上重置两字段 |
| **(D) Sinval Drain（NOTIFY 交付）** | **每次调用必做**：`AcceptInvalidationMessages()`。本身不直接 fire；但它把 Async_Notify 投递到 backend-local sinval 接收队列，保证下次 B/C fire 时 SPI 重查一定能拿到最新 catalog（NOTIFY 实际更新了 catalog）。 | 无 GUC（必做）| `utils/inval.h` 公共 API `AcceptInvalidationMessages(void)` | 不直接触发 SPI；作为 fast-path push 交付保证 |

**决策逻辑**：`need_heavy_check = (B) OR (C)`；`(D)` 每次必执行但不单独算触发。满足 `need_heavy_check` → 进 SPI probe 块重查 catalog state。

**验证用例**：`overlay_branch_invalidation.sql` Section E — state 从 active→applied 翻转后，`is_active(name)` ground truth 立即返回 f（因为它总是 SPI 查 catalog，不走节流逻辑）；`force_invalidation_check()` 也立即触发 kickout 并返回 f。（注：单 session pg_regress 环境下 (B)/(C) 数值不敏感，验证重点在节流 + kickout 的 wire-up 正确；真正的 B/C 数值节流行为在 L1 `pg_isolation_regress` isolation specs 中以人工控制 counter/wall-clock 边界值精确验证 —— Task 8 ob_state_inval.spec）。

### D.3 DQL vs DML 分层失效 Kickout 双轨对比表

SPI ground truth 重查命中 `state != BRANCH_STATE_ACTIVE` 后，kickout 严格按 `for_dml` 布尔参数分成两条独立轨（**二者前 4 步完全相同，只有第 5 步 ereport 级别/消息不同**）：

| 维度 | **DQL 轨**（`for_dml=false`）| **DML 轨**（`for_dml=true`）|
|---|---|---|
| **调用入口** | ① `overlay_branch_is_active()` 无参 wrapper 顶部（内存 ctx 前先节流）；<br>② BranchScan planner hook `set_rel_pathlist`；<br>③ BranchScan executor Begin/Exec；<br>④ `overlay_branch_force_invalidation_check()` SQL-callable 封装 | ① `write_redirect.c` CMD_INSERT/UPDATE/DELETE 路径入口（写重定向真正执行前必查）；<br>② `delta_store.c overlay_delta_insert()` 顶部安全网（兜底 WR 漏网）|
| **Pre-step 1：local 栈缓冲拷贝** | `strncpy(local_branch_name, ctx->branch_name, NAMEDATALEN)` + `strncpy(local_state_buf, state_catalog, 16)` | 同左 |
| **Step 2：SPI reread state（for message ground truth）** | SPI probe + conditional finish；SQL `SELECT state FROM pg_branch WHERE branch_id = ctx->branch_id LIMIT 1`；拿列值存到 `local_state_buf` | 同左 |
| **Step 3：ob_exit_branch_cleanup(ctx)** | idempotent：内部 `ob_ensure_unregister_snapshot()` → `is_active=false` → `bid=0` → `name[0]=0` → counter/last_check/timestamp 清零。（幂等：即使 cleanup 已经调用过，`!is_active` 分支只做残留字段清零不重复 log） | 同左 |
| **Step 4：GUC 同步 + 防重入 guard** | ``` { bool save = ob_in_guc_setconfig; ob_in_guc_setconfig = true; SetConfigOption("overlay_branch.current", "", PGC_USERSET, PGC_S_SESSION); ob_in_guc_setconfig = save; } ```（GUC check hook 再次进入时检查到 flag set 直接 return true；GUC hook 内另加 `&& CurrentBranchContext->is_active` guard 作为第二道防线） | 同左 |
| **Step 5：ereport 最终动作为差异点** | `ereport(NOTICE, (errmsg("overlay_branch: branch \"%s\" is no longer active (state=%s); reverting to Main", local_branch_name, local_state_buf)));` <br>**不中断控制流**；紧接着 `return false;`（caller 解释为 "branch 不再有效，fallback 到 MAIN 纯视图"）| `ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("overlay_branch: branch \"%s\" is no longer active (state=%s); cannot write through it", local_branch_name, local_state_buf), errhint("The branch was applied by another session.  Re-connect to MAIN and retry the write.")));` <br>**ERROR 直接抛，控制流不会再回 caller**；漂移写被彻底阻塞（事务 abort → 之前 delta 写入本语句全部 rollback，MAIN 不受影响） |
| **Step 6：return 值（仅 DQL reachable）** | `return false` — 语义 `is_active` = false | ERROR unreachable |
| **语义等价模型** | "我看到 branch 已经死了，发个 NOTICE 告诉用户，接下来我用 MAIN 纯视图继续给你查，语句正常返回但已经不包含 branch delta 了" | "我看到 branch 已经死了，你这是要写 drift delta = 数据损坏风险，**立即阻断 + 事务回滚**，禁止写 MAIN 或残留 delta" |

### D.4 两条关键工程规范（T2 期间验证过的硬规则，后续所有 Task 复用）

#### 规范 1：SPI 嵌套生命周期的 4 行 Probe 模式

**背景**：PG SPI 未暴露 `SPI_connected()` 公共 API。直接 `SPI_connect()` 若已有连接会返回 `SPI_ERROR_CONNECT`（不是 OK）；若此时我们再无条件 `SPI_finish()`，会把外层已打开的连接关掉，外层结束时再 finish → **double pfree → `pfree called with invalid pointer (header 0x7F…)` SIGABRT**。

**T2 之后所有在 C 函数内部要开 SPI 的地方必须严格照抄此模式**（套用到嵌套 SPI 场景：外层 `use_branch(name, mode)` 自己开了 SPI 做 UPDATE，中间触发 WR hook → WR 入口调 throttled → throttled 又要开 SPI 做 ground truth 重查）：

```c
/* --- SPI probe pattern (COPY THIS VERBATIM) --- */
int  spi_r;
bool did_con = false;

spi_r = SPI_connect();
if (spi_r == SPI_OK_CONNECT)
    did_con = true;
else if (spi_r != SPI_ERROR_CONNECT)
    elog(ERROR, "SPI_connect failed in %s: %s",
         __func__, SPI_result_code_string(spi_r));

/* ... SPI_execute / SPI_processed / SPIGetTuple / datumCopy ... */

if (did_con)
    SPI_finish();
/* --- end pattern --- */
```

**绝对禁止的反模式（T2 7 类 pfree bug 的 3/7 根因）**：
```c
/* ❌ 反例一：直接 assert SPI_OK */
if (SPI_connect() != SPI_OK_CONNECT) elog(ERROR, ...);
/* ... */
SPI_finish();  /* 嵌套时关了外层的连接 → 外层再 finish: double pfree */

/* ❌ 反例二：connect 正确但 finish 无条件 */
int r = SPI_connect();
bool did = (r == SPI_OK_CONNECT);
if (!did && r != SPI_ERROR_CONNECT) elog(ERROR, ...);
/* ... */
SPI_finish();  /* 漏写 if (did) → 同反例一后果 */
```

#### 规范 2：SetConfigOption 触发 GUC hook 重入的三重防线

**背景**：`SetConfigOption("overlay_branch.current", ...)` 立即触发 check hook `overlay_guc_check_assign_current_branch`（即使我们在 `_PG_init` 里只把它挂成 check hook，不是 assign hook — check hook 仍然在 SET VALUE 路径上每调必进）。如果 apply_internal Step 2 已经手动 `ob_exit_branch_cleanup(ctx)` 清了，再从 SetConfigOption → check hook → 第二次 cleanup → double pfree。

**所有手写 SetConfigOption 调用点（共 4 处）和 GUC hook 本身必须满足三重防线**：
```
三重防线（任一条缺失 = 某条路径上有 double cleanup SIGABRT 风险）：

  ① HOOK GUARD：
     overlay_guc_check_assign_current_branch() 内，
     所有分支 "if (CurrentBranchContext != NULL)" 判断后面
     必须加 " && CurrentBranchContext->is_active"。
     如果 !is_active = Step 2 或 kickout 已经 cleanup 过了 → 不再 cleanup。

  ② CALL-SITE FLAG GUARD（所有手写 SetConfigOption("overlay_branch.current", ...) 处）：
     {
         bool save_flag = ob_in_guc_setconfig;
         ob_in_guc_setconfig = true;
         SetConfigOption("overlay_branch.current", "", ...);
         ob_in_guc_setconfig = save_flag;
     }
     要求：check hook 函数体第一行先
         if (ob_in_guc_setconfig) return true;   ← 快速退出
     避免我们自己触发自己形成递归。
     受影响 4 处：use_branch 切换 / apply tail / discard tail / kickout 路径

  ③ IDEMPOTENT CLEANUP：
     ob_exit_branch_cleanup(ctx) 函数本身是幂等的：
         if (!ctx->is_active) { 只做字段残留清零 return; }
     即使双重入口侥幸穿透前两道防线（例如 hook 改出了 bug），也只 log 一次 NOTICE 不 double pfree。
```

### D.5 PERFORMANCE TODO（T2 阶段登记，正确性 > 性能，V3.x 再调优）

1. **TODO-T2-1 SPI ground truth heavy-check 每次都做 full SPI_connect 循环开销**
   - 根因：为了防嵌套 SPI double-pfree，采用了 Probe 模式，但 Probe 模式仍然要调 `SPI_connect()`（在嵌套场景里它是 O(1) check `SPI_ERROR_CONNECT` 返回，但在非嵌套场景下它仍然分配 SPICurrentMemoryContext 等资源 + `SPI_finish()` 释放）。
   - 影响：节流 fire 路径（默认 32 次调用 hit 一次 / 100ms hit 一次）的 O(1) 开销略有增加。
   - 风险级别：**极低**（fire 本身就是低频，默认 `32 次 / 100ms` = 每秒最多 10 次 SPI connect/execute/finish）。
   - V3.x 优化方向：引入 `overlay_overlay_helper_enter()` 风格的 `ob_in_spi_helper` per-backend flag，在 flag 已 set=true 时完全跳过 SPI_connect probe 直接裸 `SPI_execute`（因为我们确定 flag set 期间外层一定开着 SPI），省一次 SPI_connect()/finish() syscall 量级往返。

2. **TODO-T2-2 ob_invalidate_check_throttled() 调用入口数偏少（目前仅 4 处 DQL + 2 处 DML）**
   - 根因：DQL 入口只 wire 到 is_active() wrapper；BranchScan planner/executor 入口还没有直接调用 throttled(false)（BranchScan 通过 is_active 内存真值间接判断 fallback，实际 throttled kickout 把 is_active=false，下一条语句 BranchScan 会 fallback 到 MAIN IndexScan/SeqScan — 语义正确，但 BranchScan 自身入口不再二次 throttled）。
   - 影响：用户刚好在 kickout 发生时在 BranchScan BEGIN → Exec 窗口（极短窗口 ≈ 毫秒级）内，BranchScan 可能用了旧 snapshot 已 materialized result；实际 ReScan 到下条 statement 之间会走。
   - 风险级别：**极低**（单语句窗口，语义不崩，最多数据旧到 kickout 前一 statement）。
   - V3.x 优化方向：Task 6 BranchScan ReScan 实现时，在 ReScan 路径上显式加一次 throttled(false)，与 FR6 Fresh Materialization 协同。

3. **TODO-T2-3 NOTIFY 消息解析未 wire bid 匹配 fast path**
   - 根因：当前 NOTIFY 交付通过 `AcceptInvalidationMessages()` 让 PG 内部 `ProcessCompletedNotifies` 静态函数执行 Async 通知消费，但我们不解析 payload 做 `bid == CurrentBranchContext->branch_id` 早退出 — 直接依赖后续 SPI `SELECT state` 兜底对比。
   - 影响：NOTIFY 实际上是 bid 不匹配时（例如当前 branch 是 b_x，收到 b_y discarded 的 NOTIFY）本来可以快速跳过 "need_heavy_check"，但当前实现仍然等 B/C 节流条件满足后才去 SPI 查（SPI 查自然会发现自己 branch 仍 active → reset counter + return true，语义正确）。
   - 风险级别：**无正确性风险**（纯性能），只是浪费一次 SPI 重查。
   - V3.x 优化方向：T3/T4 阶段引入 per-backend local `pending_bid` atomic 标记（或复用 PG `Async_Unlisten` 相关 backend-local flag），在 NOTIFY sinval 处理阶段把 bid 存到 backend-local，throttled 的 B/C 判定前先加 `pending_bid_match == ctx->branch_id OR` 作为触发 D 条件的 fast-path 早 fire 比较，bid mismatch 则直接 skip（但本阶段没实现：先保证 SPI ground truth 兜底即可）。



### D.6 5 条工程规范（T2 验证后确立，后续所有编码必须遵守）

> 以下规范都在 T2 Bug #1-#11 修复过程中经过 `make check 5/5 0 diff` 多轮验收后确立为 V3 后续开发的硬约束。违反任一条 = 静默引入 bug #11 级别的漂移写 / SIGSEGV / 无限递归。

---

#### 规范 1：**BranchScan Planner Hook PRE-CHECK 顺序严格 B0→B1→B2→is_active（BFS 调用顺序）

```
B0（纯 O(1) relcache 架构类 schema skip）：
   if (get_rel_namespace(rt_indexed(RTE)->OBSCHEMA_strcmp / pg_catalog / information_schema / pg_toast*)
       → skip 直接 return，不做任何 C func 调用

B1（recursion guards）：
   ob_in_apply_operation / ob_in_write_redirect / ob_in_overlay_helper /
   → any true → return NIL

B2（non-SELECT CMD skip）：
   parse->commandType != CMD_SELECT → return NIL（非 DQL 不 CustomScan

LAST（调 C 函数）：
   saved_planner_flag = ob_in_planner_hook (true)
   overlay_branch_is_active()  // ONLY HERE
   restore flag

→ 恢复
```

**原因（Root #1**：把 is_active() 放到 B0 前面会导致 "内部 SPI 查 pg_branch → planner hook 再跑 is_active() → 再内部 SPI 查 pg_branch……"无限递归 stack overflow（#1 root）。PRE-CHECK 顺序必须严格。

---

#### 规范 2：**Depth-1 Guard 全局 extern 放 throttled() 函数最顶部第一行**

```c
// include/overlay_branch.h 全局 extern：
extern bool ob_in_invalidation_check;  // 所有入口共享同一 flag
```

```c
// 函数顶：
bool
ob_invalidate_check_throttled(bool for_dml)
{
    if (ob_in_invalidation_check)   // 行 1：任何入口先 guard
        return false;
}
```

**原因（Root #2）**：早期版本 `static in_self_check` 局部 flag 只对 `is_active_by_name` 路径生效，`force_invalidation_check()` SQL 入口可以 bypass guard → 无限递归 overflow。必须：global extern + 放在 **函数最顶**（任何 return 之前，不允许任何逻辑代码在 guard 之前。

---

#### 规范 3：**WriteRedirect 2 重 DML 前置 guard（CmdType + protected-schema skip）

```c
// Step A: CmdType dml_split
PlannedStmt *ps = ...;
CmdType cmd = ...;
bool dml_split = (cmd == CMD_INSERT || cmd == CMD_UPDATE || cmd == CMD_DELETE || cmd == CMD_MERGE);

// Step B: protected-schema catalog traverse check：
ListCell *lc;
bool all_protected = true;
foreach (lc, ps->resultRelations) {
    Index rti = lfirst_int(lc);
    RangeTblEntry *rte = rt_fetch(rti, ps->rtable);
    Oid nspOid = get_rel_namespace(rte->relid);
    char *nsp = get_namespace_name(nspOid);
    if (strcmp(nsp, "overlay_branch") != 0 &&
        strcmp(nsp, "pg_catalog") != 0 &&
        strncmp(nsp, "pg_toast", 8) != 0 &&
        strcmp(nsp, "information_schema") != 0)
    { all_protected = false; break; }

if (all_protected)
    return false;  // catalog-only DML skip throttled 防止 catalog flip 过早 kickout ctx 清空

// Step C: 2 call 前才调 throttled
```

**原因（Root #5a + Root #4**：CmdType 漏判导致 SELECT force_check → 被 for_dml=true 进去 → DML ERROR 误报 (#4)；catalog DML（D6 UPDATE pg_branch 自己写 catalog D6 调 throttled → kickout ctx.is_active=false → D7 user DML 入口 fast-path → 漏拦 drift write MAIN (#5a)。

---

#### 规范 4：**ProcessUtility 会话级 utility whitelist 跳过 is_active()**

```c
// overlay_ProcessUtility 顶：
NodeTag tag = ...
switch (tag)
{
    case T_VariableSetStmt:
    case T_VariableShowStmt:
    case T_ListenStmt:
    case T_NotifyStmt:
    case T_UnlistenStmt:
    case T_TransactionStmt:   // BEGIN/COMMIT/ROLLBACK
    case T_ConstraintsSetStmt:
    case T_DiscardStmt:
    case T_LockStmt:
    case T_CheckPointStmt:
    case T_ExecuteStmt:
    case T_PrepareStmt:
    case T_DeallocateStmt:
    case T_CreateSubscriptionStmt:
    case T_AlterSubscriptionStmt:
    case T_DropSubscriptionStmt:
    case T_CreatePublicationStmt:
    case T_AlterPublicationStmt:
    case T_SecLabelStmt:
        /* ---- pure session utility — 不写用户表 → 无需调 is_active()
        break;
    default:
        /* user DDL only：is_active() + ddl_ok guard 当 guard */
        if (overlay_branch_is_active() && !overlay_guard_ddl_ok_for_branch(...))
            ereport(ERROR, ...);
        break;
}
```

**原因（Root #5b）**：`SET threshold=1`（T_VariableSetStmt）调 is_active→throttled(DQL) cnt>=1 kickout 提前清 ctx → D7 user-table 入口 fast-path false，漂移。Defense-in-depth，防止有人 observer call 乱入。

---

#### 规范 5：**throttled() 破坏性 kickout side-effect 仅允许 opted-in callers（ob_throttled_allow_kickout=true，局部变量 saved_allow 包函数体**）

```c
// ========== 全局定义： ==========
extern bool ob_throttled_allow_kickout;  // default = false (observer-only 默认值)

// ========== throttled() heavy check ground truth found inactive： ==========
if (found_state != ACTIVE)
{
    if (!ob_throttled_allow_kickout)   // ONLY opted - in
    {
        result = false; kicked_out = false;
        goto throttle_clean_exit;   // observer return boolean 只返回结果，无任何副作用
    }
    // ---- opted-in 路径才允许
    // 保存 kickout 参数 local_* → PG_TRY 内部
    // ob_exit_branch_cleanup(ctx)
    // SetConfigOption(...)
    // PG_TRY 外 + saved_flag
}
}
```

```c
// ========== 允许的 opted-in 调用点（2 处 ONLY） ==========

// ---- (a) WriteRedirect ExecutorRun WR 入口：
bool saved_allow = ob_throttled_allow_kickout;
ob_throttled_allow_kickout = true;
bool ok = ob_invalidate_check_throttled(dml_split);
ob_throttled_allow_kickout = saved_allow;
if (!ok && dml_split) return false;

// ---- (b) force_invalidation_check SQL 入口：用户显式 "检查 now
saved_allow = ob_throttled_allow_kickout;
ob_throttled_allow_kickout = true;
bool thr_ok = ob_invalidate_check_throttled(for_dml=false);
ob_throttled_allow_kickout = saved_allow;
```

**C90 补充附属 sub-norm 5a**：**所有 kickout 局部变量 函数最顶声明（local_* / local_state_buf / local_branch_name / local_what_happened） + 全部函数开头声明块顶声明（必须函数顶声明 (void) ob_in_invalidation_check entry guard 再 PG_TRY 外部 ereport 全部 ereport 移 PG_TRY 外部 flag lowered 之后 再 抛 防止 SIGSEGV (#8 root）。

---


## D.7 V3 FR2: Branch-level Advisory Lock 层 (Layer-2 事务级互斥)

### D.7.1 动机 / 设计原则
T1 use_branch 允许任意 session 对同一个 bid 并发读写 delta；T2 apply_branch 对 MAIN 做 D→U→I 三 pass 重放；若两个 session 同时 `apply_branch('X')` 或一个 session 正在 write-through 时另一个 session 在 apply X，会造成：
1. apply+apply 并发：两 session 同时读同一批 delta 行 → D pass 两次对同一 MAIN row DELETE (第二次 no-op 还 OK)，但 I pass 两次 INSERT → 第二次抛 duplicate key violation，ERRCODE 23505；**最终 MAIN 只有 1 个 apply 的数据成功，另一事务 abort 但 pg_branch 状态未变，用户误以为"apply 成功但实际数据错"**。
2. write+apply 并发：session1 在 X 分支里 `INSERT INTO t (pk=7) VALUES(7)` 写 delta（已经拿了 user-table RowExclusiveLock pk=7），session2 apply X 拿 RowExclusiveLock pk=7 被堵 → session1 再想写另一个 user-table pk=8 需要 Advisory，但顺序反了 → 死锁。
3. discard+apply 并发：session1 apply X while session2 discard X → 一半 delta 被 apply 一半被删 → partial apply corrupt。

**FR2 目标**：引入 64-bit Advisory Lock key 把 bid 包起来，对同一 bid，apply/discard = ExclusiveLock，use_branch = SharedLock。PG 的 Advisory Lock 是 heavyweight lock，走标准 deadlock detector，锁冲突时会自动等 + 报告 deadlock，不需要手写轮询 CAS。

### D.7.2 Magic Key 格式（与 PG 标准 `SET_LOCKTAG_INT64` 兼容）

```c
/* OB = 0x4F42 ('O'=0x4F, 'B'=0x42). 高 16-bit = "OB" 魔术前缀,
 * 低 32-bit = branch_id (int32, 无符号看待)。
 * 中间 16-bit 留作将来扩展 (可表示将来 4 种 lock 类型或 db_id mask)。
 * 总共 64-bit = 正好塞 PG pg_advisory_xact_lock(int8) 的标准 int64 key.
 */
#define OB_ADVISORY_MAGIC_HI16  ((uint64)0x4F42 << 48)
#define OB_MAKE_ADVISORY_KEY(bid)  \
    (OB_ADVISORY_MAGIC_HI16 | ((uint64)((uint32)(bid))))
```

碰撞分析：普通用户拿 `pg_advisory_xact_lock(k)` 的 key 一般是连续整数或 hash，不会设置高 16-bit = `0x4F42`。若真撞 → Advisory Lock 在同一 key 上，要么导致"多余的 Shared/Exclusive 互斥"（用户代码被阻塞但安全，无数据损坏）要么反之——**概率极低（1/2^16 = 65536 量级）**，用户若真有此 key 冲突可改他们的 key 或改本宏高位。

### D.7.3 Lock 矩阵（同一 bid 不同会话兼容性）

|                  | use_branch Shared (S) | apply Exclusive (X) | discard Exclusive (X) |
|------------------|-----------------------|---------------------|-----------------------|
| use_branch S     | ✓ 兼容 (多 session 并发读) | ✗ 冲突 — apply 需排他等 use 事务结束 | ✗ 冲突 |
| apply X          | ✗ 冲突                | ✗ 冲突 (两次 apply 并发必互斥) | ✗ 冲突 |
| discard X        | ✗ 冲突                | ✗ 冲突             | ✗ 冲突 |

关键语义：
- **事务级锁 (sessionLock=false)**：`LockAcquire(&tag, mode, sessionLock=false, dontWait=false)`。锁生命周期 = 本事务，xact 结束自动释放（无论 COMMIT/ABORT/end of statement in implicit tx），永远无泄漏。
- **死锁预防 —— 锁顺序 必须 `Advisory → user-table RowExclusiveLock`（THIS IS NON-NEGOTIABLE, D.7.5 验证）**。
- **阻塞式 dontWait=false**：冲突就等，PG deadlock detector 1s 后检测到死锁会回滚 2 个事务中的 1 个，抛 ERRCODE 40P01 deadlock_detected — 用户收到明确错误不会 silent 错写。

### D.7.4 拿锁时机 / 位置（精确到函数 + 实际行号，2026-09-15 编码后同步确认）

所有拿锁都必须在 **第一个拿 RowExclusiveLock 或任何 catalog UPDATE 之前**（死锁预防顺序 § D.7.5 的硬前提）。

**(a) use_with_mode_internal()**（`src/branch_lifecycle.c:overlay_branch_use_with_mode_internal`）：
- 实际行范围：existence+state+mode SELECT (L336-373) → `real_branch_id = DatumGetInt32(...)` (L373) → UPDATE pg_branch SET mode=... (L387-L394) → ob_install_snapshot_for_mode (L398) → GUC set (L402)。
- **实际插入点：L379-L385**：在 `real_branch_id` 成功拿到之后、任何 catalog UPDATE 或安装快照之前立即调 `ob_take_branch_advisory_lock(real_branch_id, ShareLock)`。bid sanity guard（bid > 0）已在 inline helper 内部判断，bid ≤ 0 直接 `elog(ERROR)`。
- 死锁序验证：后续 `UPDATE pg_branch SET mode=...` RowExclusive on pg_branch heap + DML write_redirect `table_open(user_rel, RowExclusiveLock)` 全在 Advisory Shared 之后 → ✅ Advisory → RowExclusive。

**(b) apply_branch_internal()**（`src/branch_lifecycle.c:apply_branch` 主入口）：
- 实际行范围：Step 1 bid+state SELECT + SPI_finish (L936-L962) → Step 2 leaving_current exit_branch_cleanup + state≠ACTIVE check (L977-L991) → Step 3 for each rel 三 pass D-U-I: `table_open(relid, RowExclusiveLock)` (L1040) → Step 4 UPDATE pg_branch→applied + DELETE delta + broadcast (L1180+)。
- **实际插入点：L967-L975**：Step 1 bid SELECT 成功 + SPI_finish 已释放 SPI portal 之后立即调 `ob_take_branch_advisory_lock(bid, ExclusiveLock)`。
- 死锁序验证：X 锁在 Step 2 state check + Step 3 L1040 `table_open(relid, RowExclusiveLock)` 三 pass 之前 → ✅ Advisory → RowExclusive。

**(c) discard_branch_internal()**（`src/branch_lifecycle.c:discard_branch` 主入口）：
- 实际行范围：Step 1 bid+state SELECT (L1241-L1258) → already_applied guard → already_discarded_applied guard (L1260) → UPDATE pg_branch SET state=discarded (L1270) → DELETE delta + broadcast。
- **实际插入点：L1262-L1268**：在 bid SELECT 成功 + 状态 guard 通过之后、L1270 catalog `UPDATE pg_branch SET state=discarded` 之前立即调 `ob_take_branch_advisory_lock(bid, ExclusiveLock)`。
- 死锁序验证：catalog UPDATE 的 RowExclusive 在 X 锁之后 → ✅ Advisory → RowExclusive。

### D.7.5 Deadlock Prevention — 严格锁顺序 **Advisory Lock → user-table RowExclusiveLock → catalog locks (auto)**

```
Session A (use + write-through delta):
  ① ob_take_branch_advisory_lock(bid, SHARED)    [xact-level]
  ② ExecutorRun writes delta:  delta_append → table_open(user_rel, RowExclusiveLock)
  → Correct order.

Session B (apply bid):
  ① ob_take_branch_advisory_lock(bid, EXCLUSIVE) [waits on A's SHARED]
    ↓ After A commits:  Exclusive granted
  ② table_open(relid, RowExclusiveLock)
  → Correct order.  No deadlock.

Bad scenario (if we reversed): Session A does ②RowExclusive then ①Advisory, Session B ②RowExclusive on another row then ①Advisory → classic cyclic wait → deadlock.  严格禁止这种顺序。  FR2 所有 code path 在 ① 之后 才 允许任何 table_open(*, RowExclusiveLock)。
```

### D.7.6 C 实现接口（全部 static inline / extern 在 overlay_branch.h，与实际 2026-09-15 编码版本严格一致）

> 【关键实现注意事项同步记录（T3 build1+2 的两轮 fix 已归档，避免后续再踩坑）**：
> 1. **`SET_LOCKTAG_INT64` 不是 PG 对外导出的宏，仅在 `src/backend/utils/adt/lockfuncs.c:613` 本地 helper。contrib overlay_branch.h 必须本地 mirror。
> 2. **`MyDatabaseId` 外部变量声明在 `miscadmin.h`，没加会报错 `MyDatabaseId undeclared`。
> 3. **正确头路径**：`SET_LOCKTAG_ADVISORY` 在 `storage/lock.h`（NOT `utils/lock.h`，不存在！）

```c
/* ---------- overlay_branch.h Line 11-95 (final 编码后真实内容) ---------- */
#include "miscadmin.h"            /* MyDatabaseId (SET_LOCKTAG_INT64 id1 域必要) */
#include "storage/lmgr.h"           /* LockAcquire, ShareLock, ExclusiveLock, LOCKTAG */
#include "storage/lockdefs.h"
#include "storage/lock.h"     /* SET_LOCKTAG_ADVISORY (src/include/storage/lock.h:270) */

#define OB_ADVISORY_MAGIC_HI16  ((uint64)0x4F42ULL << 48)
#define OB_MAKE_ADVISORY_KEY(bid)  \
  (OB_ADVISORY_MAGIC_HI16 | ((uint64)((uint32)(bid))))

/* mirror `src/backend/utils/adt/lockfuncs.c:613 SET_LOCKTAG_INT64 local macro,
 * id4=1 means int64-style tag (int1=MyDatabaseId per-database scoped)。
 * PG headers 未导出宏，contrib 必须本地重定义才 100% bit 匹配 SQL pg_advisory_xact_lock(int8) 兼容 key layout。*/
#ifndef SET_LOCKTAG_INT64
#define SET_LOCKTAG_INT64(tag, key64)  \
    SET_LOCKTAG_ADVISORY(tag,                                         \
                        MyDatabaseId,                                  \
                        (uint32) ((key64) >> 32),                  \
                        (uint32) (key64),  \
                        1)
#endif

static inline void
ob_take_branch_advisory_lock(int32 bid, LOCKMODE lockmode)
{
    int64       key = (int64) OB_MAKE_ADVISORY_KEY(bid);
    LOCKTAG     tag;
    if (bid <= 0)
        elog(ERROR, "overlay_branch: invalid branch_id=%d for advisory lock", bid);
    SET_LOCKTAG_INT64(tag, key);
    (void) LockAcquire(&tag, lockmode,
                       false,  /* sessionLock=false → xact-level auto-release (commit/abort free，无泄漏) */
                       false); /* dontWait=false → block until granted 或 deadlock detector 报 40P01 回滚 */
}

/* 防御性手动解锁（仅 rollback 辅助路径调；正常 xact end 自动释放不用调） */
static inline void
ob_release_branch_advisory_lock(int32 bid, LOCKMODE lockmode)
{
    int64       key = (int64) OB_MAKE_ADVISORY_KEY(bid);
    LOCKTAG     tag;
    if (bid <= 0) return;
    SET_LOCKTAG_INT64(tag, key);
    (void) LockRelease(&tag, lockmode, false); /* sessionLock=false 对应 take 的 false */
}
```

### D.7.7 测试策略（三层）

1. **L3 pg_regress（当前 mvcc.sql Section L Line 768-853，非并发 sanity — 已实现 4/4 PASS）**：
   - L1 create→use(SHARED)→INSERT→apply(S→X upgrade)：l1_delta_has_1_row=t, l1_state_applied=t, l1_delta_cleaned=t。同一 xact 同后端 S→X 升级 granted immediately，无 wait。
   - L2 create→discard (X-only, never used)：l2_state_discarded=t。
   - L3 create→use 'snapshot' (S)→INSERT 2 行→GUC='b_fr2_l3' → discard (S→X) → GUC='' (empty), l3_state_discarded=t。GUC reset 正确生效（exit_branch_cleanup 正常调）。
   - **L4 Magic key equivalence C↔SQL bitcast 验证（端到端关键用例）**：C 端 `use_branch('b_fr2_l4')` 已调 `ob_take_branch_advisory_lock(bid, ShareLock)`；SQL 端通过子查询取 bid 直接算 `(0x4F42::bigint<<48) | bid::bigint` 再 `pg_advisory_xact_lock_shared(same key)` → 同 xact 同 mode 重入立即 granted (IS NULL = t)。这是 C 宏 bitcast 和 SQL 端 bitcast 完全相等的强证据（Section L l4_magic_high_bits_set=t + l4_magic_nonzero=t + l4_sql_shared_reentry_ok=t 三断言全 PASS）。

2. **L1 pg_isolation_regress (T8，未来并发验收，4 条 spec)**：
   - `ob_advisory_apply_use.spec`: s1 use(bid) → s2 apply(bid) blocks → s1 commit → s2 apply proceeds → state=applied（isolation tester 自动检测 waiting state）。对应 progress_tracker TR-3.1。
   - `ob_advisory_apply_vs_apply.spec`: s1 apply(bid) → s2 apply(bid) blocks → s1 commit → s2 apply 立即 ERROR (state=applied 非 ACTIVE)；MAIN row count = exactly 1x write，无重复。对应 TR-3.1 后半。
   - `ob_advisory_apply_vs_discard.spec`: s1 apply(3pass sleep) + s2 discard → 无论哪一者先完成另一者立即 ERROR (状态非 active)，无 partial apply 无 discarded+MAIN written 不一致。对应 TR-3.2。
   - `ob_deadlock_prevention.spec`: 故意反序（先 RowExclusive 再 Advisory）尝试 → deadlock detector 1s 后自动回滚一事务 ERRCODE 40P01，无 MAIN corruption。对应 TR-3.4。

3. **L2 pgbench (T8，未来压测)**: 50 线程 60 秒 random mix 30% apply / 50% use_write / 20% discard → pk 唯一性 + state/delta 一致性断言 0 失败，deadlock ERROR 次数合理（由 PG detector 自动回滚）。对应 TR-3.4。

---

## D.8 V3 FR3: Layer-3 `applying` transient state + delta-write Guard

### D.8.1 动机 / 修复 silent 丢数据 bug

V3 T1+T2+FR2 在 apply 流程中：**state 直接从 `active` 跳到 `applied`**，中间没有任何屏障。当另一个 session S2 在 S1 apply_branch 三 pass 跑到一半时，在同一 bid 上写 `INSERT INTO user_table`（通过 WR → `overlay_delta_insert` 追加新 delta row 到 `pg_branch_delta`），会出现 silent data loss：

- S1 Step3 D pass 已经 DELETE 了 MAIN row（pk=7），S2 此时写 pk=7 的新 delta INSERT op
- S1 Step3 I pass 早就跑完，不会再把 S2 刚写的这条 delta 合并进 MAIN
- S1 Step4 `DELETE FROM pg_branch_delta WHERE branch_id = N` 会把 S2 新写的 delta 一起删掉（bid 相同）
- 最终：pg_branch.state=applied 显示"合并成功"，但 S2 那条 pk=7 永远没有被应用到 MAIN，也不在 delta 里，**用户以为自己 INSERT 成功了，实际数据消失 — 无任何 ERROR 提示**。

这是比 race-on-MAIN 更糟的 bug（race-on-MAIN 会被三 pass 检测到 WR old_version 失败并抛 23505 / WRONG_VERSION — 至少有明确报错）。

### D.8.2 State 机扩展：新增 `applying` 过渡态 + 原子 CAS UPDATE

**新 state 全集（4 个）**（对应常量定义在 `include/overlay_branch.h`）：
```c
#define BRANCH_STATE_ACTIVE     "active"       /* normal writable */
#define BRANCH_STATE_APPLYING   "applying"     /* transient, while apply runs */
#define BRANCH_STATE_APPLIED    "applied"      /* terminal, merge done */
#define BRANCH_STATE_DISCARDED  "discarded"    /* terminal, merge abandoned */
```

**合法 transition 表（所有非法 transition 立即 ERROR）**：

| # | From state | To state | 触发入口 | 合法性 | 说明 |
|---|------------|----------|----------|--------|------|
| T1 | `active` | `applying` | `apply_branch` Step 1.5 CAS UPDATE | ✅ ONLY legal way to enter applying | atomic: bid=N AND state='active' |
| T2 | `applying` | `applied` | `apply_branch` Step 4 SUCCESS | ✅ | 三 pass 成功 + DELETE delta 后 |
| T3 | `applying` | `active` | xact ABORT / ROLLBACK (any reason) | ✅ PG 原生 MVCC row-visibility rollback | 事务回滚 UPDATE 的 XMIN 变成无效，state 回到 UPDATE 前的值 |
| T4 | `active` | `discarded` | `discard_branch` UPDATE | ✅ | FR2 X-lock held so no race |
| T5 | `applying` → `discarded` | any caller | ✗ FORBIDDEN | apply_branch 持 X advisory 锁，discard 拿不到 X，不会并发 |
| T6 | `applied` / `discarded` | any other state | any | ✗ FORBIDDEN (terminal states) | apply/discard 顶部 check 已存在 |

**T1 原子 CAS UPDATE 的关键 SQL**（apply_branch Step 1.5 入口 — 三 pass 之前立即执行）：
```sql
UPDATE overlay_branch.pg_branch
   SET state = 'applying'
 WHERE branch_id = $1::integer AND state = 'active';
-- → 结果 SPI_processed == 1 → proceed.
-- → 结果 SPI_processed == 0 → ERROR: 有人抢先或 state 不是 active
--     ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE (55000)
```

Why atomic CAS (UPDATE ... WHERE state='active') instead of "先 SELECT 再 UPDATE"？
- SELECT-then-UPDATE 在 READ COMMITTED 下，如果两个 session 同时 apply bid=3（虽然 FR2 X advisory 应该已经把并发挡掉），还是可能 SELECT 同时看到 active → UPDATE 同时写 → 两次 apply 都认为自己合法。CAS UPDATE 是串行化的，0 行受影响直接错，**double guard 永远不能错**。

### D.8.3 Rollback auto-restore `applying → active`

我们 **不写任何手工 UPDATE state=active 的代码**。靠 PG 自己的事务机制：
- 上述 CAS UPDATE `state = 'applying'` 是本事务内的一次 heap UPDATE，产生新 tuple version，XMIN = 当前 xact。
- 任何原因导致事务 ABORT（Step3 三 pass 的 WR 版本冲突 / user 主动 ROLLBACK / 任何 ERROR）→ PG 把 XMIN 标记为 aborted → 其他事务（甚至本事务后续读到的可见性）看到的仍然是 UPDATE 前的旧 tuple（state = active）。
- **这是零成本零 bug 风险的 rollback 方案**，绝不要在 CATCH 块里再手动 `UPDATE SET state='active'` — 那样会和 PG rollback 语义打架，可能把真正 terminal 的 applied/discarded 误擦写回 active。

### D.8.4 delta_insert Guard（所有 WR 入口的硬屏障）

第二个机制防止 `applying` / `discarded` / `applied` 态仍有人写 delta（silent 丢数据前提）：

**位置**：`src/delta_store.c:overlay_delta_insert()` **函数最顶部**，任何 `quote_literal_cstr` / SPI INSERT 之前。函数签名已有 `branch_id` 入参，不用再调 CurrentBranchContext。

```
overlay_delta_insert(bid, relid, key, op, old_version, tuple_json):
  1. Guard:  bid sanity check (bid <= 0 → ERROR, 已有但保留)
  2. Guard:  SPI SELECT state FROM pg_branch WHERE branch_id = bid
              → 0 rows → ERROR "branch N does not exist"
              → state != 'active' →
                    ereport(ERROR,
                      ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                      "branch '%s' state=%s; cannot write to delta (apply in progress or branch already finalised)",
                      lookup_name(bid), state);
  3. Guard:  op sanity ('I'/'U'/'D') — 保持现有
  4. → proceed to original SPI INSERT ON CONFLICT.
```

**Guard 覆盖的全部写入口（经 grep 验证 100% 的 delta 写入都走 overlay_delta_insert 单函数）**：
- `write_redirect.c:532` / `write_redirect.c:855` / `write_redirect.c:968` — 三类 DML WR 重定向
- `delta_store.c:149` — apply Step3 的 D/U pass
- `delta_store.c:1102 / 1121 / 1138` — DEBUG helpers
- `delta_store.c:1197` (overlay_debug_delta_insert callable)
  ⇒ 全部走 overlay_delta_insert 单入口，Guard 放对一次就全局生效。不会漏一条 WR。

### D.8.5 apply_branch 新精确 Step 拆分（插入 CAS UPDATE 位置）

原 Step 编号保留语义，中间插入 Step 1.5，保证死锁预防 + 屏障顺序：

```
apply_branch(bname):
  Step 0:  bid + state SELECT + SPI_finish (L936-965)
  Step 0.5:bid sanity + ob_take_branch_advisory_lock(bid, EXCLUSIVE) (L967-975)
           ↑ Advisory → RowExclusive 满足
  Step 1:  if state != ACTIVE → ERROR (already L977-988)
 *Step 1.5: V3 FR3 CAS UPDATE state = 'applying' WHERE bid=N AND state='active'
            SPI_processed == 0 → ERROR (bid race or stale state)
            ↑ THIS IS THE SINGLE NEW LINE
  Step 2:  if current session on bid → ob_exit_branch_cleanup (L990-999)
  Step 3:  relid gather + for each rel 三 pass D→U→I (L1000+)
  Step 4:  UPDATE state='applied' + DELETE pg_branch_delta WHERE bid=N
           + ob_broadcast_state_change (FR1 NOTIFY)
  (on any ERROR → xact ABORT → applying auto rolls back to active,
                  delta rows untouched → user can retry apply)
```

### D.8.6 C 端精确插入点（编码完成后最终行号，与代码双向同步 ✅）

| 变更位置 | 精确行范围（实际代码，可点击跳转） | 变更内容 |
|---|---|---|
| `include/overlay_branch.h` | **L109-L111 之间（实际 L110）** → [overlay_branch.h:110](file:///home/ubuntu/work/postgres/contrib/overlay_branch/include/overlay_branch.h#L109-L112) | 新增 `#define BRANCH_STATE_APPLYING "applying" /* FR3 transient CAS state while apply runs */` |
| `src/branch_lifecycle.c` apply_branch Step 1.5 CAS | **实际 L990-L1042（Between L965 old Step0 SPI_finish & L1043 Step2 exit_cleanup）** → [branch_lifecycle.c:990-L1042](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L990-L1042) | 插入 CAS UPDATE state='applying' WHERE bid=? AND state='active' + SPI_processed!=1 → ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE |
| `src/branch_lifecycle.c` apply_branch Step 4 final UPDATE (Bug B fix) | **实际 L1179-L1216** → [branch_lifecycle.c:1179-L1216](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L1179-L1216) | WHERE predicate updated `state=BRANCH_STATE_APPLYING`（不再是原始 ACTIVE），兼容 FR3 CAS 在前；SPI_processed!=1 严格校验已存在 |
| `src/delta_store.c overlay_delta_insert Guard (Bug A fix)` | **L89-L162（函数体顶，函数开 L79）** → [delta_store.c:89-L162](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L89-L162) | bid>0 sanity → **plain `SELECT branch_id,state FROM ... LIMIT 1` 无 FOR KEY SHARE**（避免 STABLE 函数内行锁 ERROR）→ TextDatumGetCString 必须在 TopMemoryContext 拷贝（镜像 apply Step0 L959-L962）后 SPI_finish 再比较；state != ACTIVE → 55000 + applying/applied/discarded 分类 hint |

> **文档/代码双向同步约定（同 T3 D.7.6）**：若上述 4 个行范围后续因重构发生 ±5 行以上漂移，**务必先更新此表再提交**。当前与 `gcc -Wall 0 warnings` / `4/4 make check 0 diff` 构建完全一致。

### D.8.7 测试策略（三层）

1. **L3 pg_regress Section M (当前交付，单 session sanity，无并发)**：
   - **TR-4.1 Guard sanity**: create → use → INSERT 1 row (delta 1 row OK) → **manual** `UPDATE pg_branch SET state='applying'`（模拟 S1 apply 半路）→ same session INSERT 第 2 行 → WR delta_insert Guard 抛 state=applying ERROR → delta 行数仍为 1 (未新增) → **manual** `UPDATE pg_branch SET state='active'` 恢复 → discard。核心验证 Guard 在非-apply 场景下逻辑正确（真并发 S1↔S2 留 T8 pg_isolation_regress，L3 只验证 Guard branch 的正确性）。
   - **TR-4.2 CAS + rollback sanity**: create → use → INSERT 3 rows (delta 3) → 人为制造 apply 中途失败的等价 rollback：用 `DO $$ BEGIN PERFORM apply_branch('b_fr2_m2'); RAISE EXCEPTION 'simulate Step3 WRONG_VERSION'; END $$;` → xact ABORT → state 查回 active (NOT applying/applied) → delta 行数仍 3 保留 → apply_branch retry 2nd time SUCCESS → state applied delta 0。验证 **rollback auto-restore state + delta kept**。
   - **TR-4.3 discarded Guard sanity**: create → discard → 尝试 INSERT 一行 → Guard 立即 ERROR branch not active → delta 行数恒 0。

2. **L1 pg_isolation_regress (T8)**：`ob_fr3_applying_delta_blocked.spec`:
   - s1 use → INSERT → s1 apply(Step3 人为在三 pass 中间加 pg_sleep(2) debug hook → state='applying')
   - s2 use same bid → INSERT → blocked by delta_insert Guard ERROR state=applying
   - s1 apply commit → state=applied applied s2 insert error retained s2 ERROR correct.

3. **L2 pgbench (T8)**：50 threads 60s mix (30% apply / 50% use_write / 20% discard)。Metric: **silent delta loss count = 0**。定义：`count(state=applied AND COUNT(delta.rows for bid)>0) OR state=discarded AND MAIN has rows that INSERT succeeded on WR side`。此 count 必须恒 0。

---

## D.9. FR5 SNAPSHOT Mode MAIN Freeze (T5)

### D.9.1 动机

在 `BRANCH_MODE_LIVE` 下，MAIN 表的读每次反映 MAIN 当前最新提交的 rows（LatestSnapshot）。
在 `BRANCH_MODE_SNAPSHOT` 下，用户进入分支的那一刻，MAIN 表的读应该被 "frozen"：
use_branch(name, 'snapshot') capture 一份 TransactionSnapshot（V3 已在 T1 中实现，保存在 `BranchContext.branch_main_snapshot`，TopMemoryContext 自管，不 RegisterSnapshot/ResourceOwner），
之后所有 Pass1 MAIN baseline 读都使用该 frozen snapshot，无论其他会话在 MAIN 中 INSERT/UPDATE/DELETE 了多少行，分支侧 SELECT MAIN 行数和 t0 时刻保持一致。

### D.9.2 三 pass 快照分配（T5 核心）

| Phase | 读取对象 | Snapshot 使用（SNAPSHOT mode 下） | 不变（LIVE mode 下） | 理由 |
|---|---|---|---|---|
| **Pass1 MAIN baseline 读** | MAIN 表的 `SELECT * FROM maintable [WHERE ...]`（ob_compute_overlay_slots helper 内部 `ob_spi_one_shot` 执行） | `PushActiveSnapshot(ctx->branch_main_snapshot)` → SPI SELECT → `PopActiveSnapshot()` | `PushActiveSnapshot(null)` / 直接 LatestSnapshot（即 branch_main_snapshot=NULL 不 push，使用默认） | MAIN 读必须 frozen，不能看到其他会话在 t0 之后提交的行。 |
| **Pass2 Delta 读** | `pg_branch_delta`（overlay_delta_lookup / overlay_delta_list_for_rel） | LatestSnapshot（**不 push 任何 snapshot**） | LatestSnapshot | Delta 是当前会话/事务内写入的，必须能看到最新写入的所有 delta 行（包括本语句前半段 INSERT 的 INSERT row），否则 branch scan 结果缺行。 |
| **WR old_version MAIN 物理查 ctid** | `fetch_tuple_by_ctid(rel, ctid_cstr)`（write_redirect.c L462）SPI `SELECT * FROM maintable WHERE ctid = '...' LIMIT 1` | LatestSnapshot（**不 push frozen snapshot**） | LatestSnapshot | ctid 查询必须返回 MAIN 当前物理实际存在的 tuple（用于 heap_tuple_lock 锁定行）。用 frozen snapshot 查的话，若 t0 后该行在 MAIN 被 HOT-updated（new ctid），frozen 指向 old ctid（可能 dead），old_version xmin 与 apply 时冲突检测的实际 xmin 不符，导致 false conflicts / silent corruptions。**（踩坑警告 1）** |
| **Planner Hook 阶段** | CustomScan 路径选择（set_rel_pathlist 回调） | **不 push / 不读取任何 MAIN rows**（仅 relcache / has_pk / PK 条件识别等值判断） | 不 push | Planner 阶段只建路径不执行实际 heap/SPI 扫表。真扫表发生在 ExecCustomScan 首次执行（Pass1）。 |

### D.9.3 Snapshot Stack 平衡策略

`PushActiveSnapshot` / `PopActiveSnapshot` 是配对的栈操作。**不平衡会导致 ActiveSnapshot 栈指针漂移 → 后续所有查询看到错误快照甚至 PANIC**。
严格使用 PG_TRY / PG_CATCH / PG_END_TRY 三重保护：

```c
bool pushed = (CurrentBranchContext != NULL &&
               CurrentBranchContext->branch_main_snapshot != NULL);

overlay_overlay_helper_enter();
PG_TRY();
{
    if (pushed)
        PushActiveSnapshot(CurrentBranchContext->branch_main_snapshot);
    ret = ob_spi_one_shot(sql.data, true, 0);
    if (pushed)
        PopActiveSnapshot();
    pfree(sql.data);
    if (ret != SPI_OK_SELECT) ereport(ERROR, ...);
}
PG_CATCH();
{
    if (pushed)
        PopActiveSnapshot();  // 平衡：error 路径必须 pop
    overlay_overlay_helper_exit();
    PG_RE_THROW();
}
PG_END_TRY();
overlay_overlay_helper_exit();
```

> **平衡不变式**：任意 control flow（normal/error），`Push` 次数 == `Pop` 次数（0 或 1，取决于 pushed flag）。

### D.9.4 Snapshot Lifetime / Re-entry 不变式（T5 重写，引入 per-bid 全局缓存）

**T5 踩坑 root ⑤ 后重写（见 § D.9.8 踩坑）**：早期设计（T1 / T5 中间态）假设 "每次 use_branch snapshot 重新 capture 新快照"，在 Section N freeze 场景下（exit → live B apply MAIN #4 → re-enter same snapshot A）会看到 4 行而非 frozen 3 行 → 语义违反。最终架构：**全局 TopMC 哈希 `ob_snapshot_cache`（key=int32 branch_id）持有每个 snapshot 分支的 UNIQUE frozen capture**，BranchContext 仅持有 "引用 pin"：

1. **首次 capture**（hash miss）：`ob_install_snapshot_for_mode` 调用 `GetTransactionSnapshot()` → CopySnapshot(TopMC) → `regd_count=1` 伪 pin → `ob_snapshot_cache_insert(bid, snap)` + `ob_snapshot_cache_pin(bid)` → BranchContext.branch_main_snapshot = shared 指针 + snapshot_registered=true。**每个 bid 只会 capture 一次**（apply/discard 前不重 capture）。
2. **Re-enter same bid**（hash hit）：`ob_snapshot_cache_lookup` 返回同一 frozen 指针（非 re-capture）→ 仅 pin++，xmin 保持首次 capture 的原始值（freeze 语义保证）。
3. **Local exit（use('')）**：`ob_ensure_unregister_snapshot` 只清 ctx 引用 + `ob_snapshot_cache_unpin(bid)`（refcnt--），**NOT pfree**。snapshot 继续存活于全局 hash 中，供后续 re-enter 同一 bid 使用（frozen xmin 不变）。
4. **Terminal cleanup（apply_branch / discard_branch）**：`ob_snapshot_cache_remove(bid)` 同时 HASH_REMOVE + pfree snapshot。**只有这里真正释放底层 CopySnapshot 内存**。
5. **N4 连续两次 use(name,snapshot)（不 exit）**：caller `use_with_mode_internal` L417 先调用 `ob_ensure_unregister_snapshot`（unpin → refcnt 回到 0），再调 `ob_install_snapshot_for_mode`（hash hit，再次 pin++，指针相同）。既满足 "old pfree first（伪：old unpin first）"，又保证 reuse。无 double-free / dangling。

### D.9.5 重入 / 嵌套保护

- Pass1 执行前 `overlay_overlay_helper_enter()` 已经设置了 `ob_in_overlay_helper = true`，Planner hook（[branch_scan.c](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c) 的 set_rel_pathlist_hook）遇到该 flag 会直接 return，防止 helper 内部的 SPI SELECT 再触发 CustomScan → 死循环（已存在 ✅，T5 不动）。
- **`pushed_snap_` flag**（[branch_scan.c:L229](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L229-L231)）精确控制 push/pop 配对：`pushed_snap_ = (ctx != NULL && ctx->branch_main_snapshot != NULL)`。与 `pop_done_` 组合保证 TRY/CATCH 两条路径的栈平衡。
- **`ob_in_snapshot_mode_helper` depth 全局**（[overlay_branch.c:L85](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/overlay_branch.c#L85) extern）：防止 helper 内部嵌套 Pass1 再次 push（虽然当前代码结构不发生，但全局 depth 位为未来 D.9.3 平衡不变式加一层保险）。

### D.9.6 C 端精确插入点（T5 编码完成后实际行号，双向同步）

| 变更位置 | 精确行范围（实际，含双向 link） | 变更内容 / 架构角色 |
|---|---|---|
| `src/branch_lifecycle.c` per-bid snapshot hash 定义 | [L48-L173](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L48-L173) | 新增 `utils/hsearch.h` include + `OBSnapshotHashKey/Ent` 类型 + `HTAB *ob_snapshot_cache` 全局 + 6 个 static helpers（init/lookup/insert/pin/unpin/remove）。TopMemoryContext 分配、HASH_BLOBS 按 int32 key。 |
| `src/branch_lifecycle.c` `ob_ensure_unregister_snapshot`（不再 pfree！） | [L270-L298](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L270-L298) | 重写：snapshot 所有权从 ctx 转移到 hash；这里只 `saved_bid = ctx->branch_id` → 清 ctx->snap ptr → `ob_snapshot_cache_unpin(saved_bid)`（refcnt--）**不 pfree**。仅 terminal remove（apply/discard）才 pfree。 |
| `src/branch_lifecycle.c` `ob_install_snapshot_for_mode`（hash miss/capture vs hit/reuse） | [L331-L447](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L331-L447) | 重写两分支：① hash HIT → `ctx.snap = existing` + pin（复用同 frozen 指针，保证 freeze xmin 不重算）② hash MISS → GetTransactionSnapshot → CopySnapshot(TopMC) → regd_count=1 伪 pin（T5 踩坑 ② fix）→ `insert + pin`。LIVE mode 下仍直接 NULL/不注册。 |
| `src/branch_lifecycle.c` `overlay_branch_apply_internal` 成功出口 terminal remove | [L1399-L1410 之后](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L1399-L1410) | `ob_broadcast_state_change(APPLIED)` 之后立刻 `ob_snapshot_cache_remove(bid)`：terminal cleanup，HASH_REMOVE + pfree 底层 CopySnapshot chunk。bid 不再 ACTIVE，hash 不会再 lookup。 |
| `src/branch_lifecycle.c` `overlay_branch_discard_internal` 成功出口 terminal remove | [L1542-L1554 之后](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L1542-L1554) | `ob_broadcast_state_change(DISCARDED)` 之后立刻 `ob_snapshot_cache_remove(bid)`：同上 terminal remove。 |
| `src/overlay_branch.c` GUC check hook（PG 契约 check→assign 拆分 + A6 custom errmsg） | [L354-L472](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/overlay_branch.c#L354-L472) | check hook 纯只读 SPI 校验：NULL/empty→true；length<NAMEDATALEN；InvalidOid init skip；SELECT state FROM pg_branch WHERE name=val LIMIT 1 → SPI_getbinval col 1 → TextDatumGetCString → strcmp(ACTIVE)。两失败分支（not-found / state≠ACTIVE）在 return false 前 ereport(ERROR) 自定义文本（"branch X does not exist"/"is not active"），使 basic A6 ERROR 文本匹配 use_branch() public API。 |
| `src/overlay_branch.c` GUC assign hook（assign 永不失败，T5 踩坑 ①/④ fix） | [L475-L523](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/overlay_branch.c#L475-L523) | 只做副作用，永不 ERROR（任何 assign ERROR 都会留 GUC global half-written → drift）。① `ob_in_guc_setconfig` 仅读取从不修改（caller-owned 模式，T5 踩坑 ③ fix）② non-empty path LAYER-1 guard：ctx 存在且 !is_active 时直接 return（避免 subxact-abort unwind 时 assign hook 跑 SPI → D7 PANIC 根，T5 踩坑 ④ fix）。 |
| `src/branch_scan.c` `ob_compute_overlay_slots_internal` Pass1 push/pop 三重保护 | [L225-L275](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L225-L275) | `pushed_snap_` / `pop_done_` 双 bool 精确路径跟踪。TRY 正常路径只 pop 一次；CATCH 异常路径仅当 `pushed_snap_ && !pop_done_` 才补 pop。严格满足 D.9.3 的 "push count == pop count == (0 or 1)" 平衡不变式。 |
| `test/regress/sql/overlay_branch_mvcc.sql` Section N FR5 freeze（4 sub-tests L3 sanity） | [L1043-L1117](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_mvcc.sql#L1043-L1117) | TR5.1 freeze（bs_fr5_a exit→B apply MAIN #4→re-enter A frozen xmin → n1_frozen=3 ✔）；TR5.2 snapshot+WR INSERT（re-enter A 后 WR row id=5 → delta=1 + count=4 ✔）；TR5.3 C(0-wr) apply→state applied delta=0 ✔；TR5.4 D re-entry snapshot 两次→write 后 delta=1 ✔。底部 marker `PASS:N0_FR5_SNAPSHOT_FREEZE_OK`。 |

> **文档/代码双向同步约定**：若上述行范围后续因重构发生 ±5 行以上漂移，**务必先更新此表再提交**。
>
> **踩坑警告 2**：`RegisterSnapshot()` 的限制 T1 已归档（绑定 ResourceOwner/Portal → 跨语句自动释放导致 UnregisterSnapshot "not owned by resource owner" ERROR）。T5 Pass1 push 直接使用 CopySnapshot 的 TopMC 自管结果，没问题：`PushActiveSnapshot` 接受任意合法 Snapshot 指针，不要求 registered。
> **踩坑警告 3（T5 新增）regd_count=0 时 PopActiveSnapshot auto-FreeSnapshot**：[snapmgr.c:L753-L755](file:///home/ubuntu/work/postgres/src/backend/utils/time/snapmgr.c#L753-L755) 当 active_count==0 AND regd_count==0 时 FreeSnapshot。CopySnapshot 默认两个都是 0 → 第一次 Pass1 push→pop 就把我们的 snapshot free 了 → ensure_unregister 再次 pfree → double pfree ERROR，同时 ctx->snap 变成 DANGLING → 后续 MAIN 查询实际用 Latest → freeze f。**固定：regd_count=1 伪 pin，只在 apply/discard 时真正 pfree**，匹配 D.9.4 的 hash 终端 remove 设计。

### D.9.7 测试策略（三层）

1. **L3 pg_regress Section N (当前交付，单 session sanity)**：
   - **TR-5.1 Snapshot MAIN freeze vs MAIN concurrent write（same-session 模拟）**：
     1. 建表 `t_fr5_n1` MAIN INSERT 3 rows。
     2. `use_branch('bs_fr5', 'snapshot')` 冻结。
     3. SAME SESSION 回 MAIN（`use_branch('')`）→ MAIN INSERT 第 4 行。
     4. 重新 enter snapshot：`use_branch('bs_fr5', 'snapshot')` — **注意**：因为 TransactionSnapshot 是当前事务启动时的，第 4 行在同一事务内 committed (in-xact) 会被看到，所以用 savepoint 模拟：`SAVEPOINT t1; INSERT MAIN #4; RELEASE SAVEPOINT` → 然后 snapshot branch SELECT COUNT 应该还是 3（实际上 same-xact 内 rows 还是 visible，L3 用另一种测试：`use_branch('bs_snap','snapshot');` 然后在 `DO $$ BEGIN PERFORM use_branch(''); INSERT INTO main VALUES; PERFORM use_branch('bs_snap','snapshot'); ... ASSERT count=3 END $$;`）。L3 简化版：两个不同 DO 块，第 1 块：建 branch 快照 enter，写入 MAIN，切换到 live mode enter，count=4；再切回 snapshot enter（重新 capture 新快照，看到了新 row =4）— 测试 freeze 语义，应该用两个不同分支：一个 snapshot 一个 live：
     - MAIN baseline 3 rows。
     - create branch A SNAPSHOT: create→use(snapshot)，记录 count_A0=3 → exit。
     - create branch B LIVE: create→use(live) 执行 INSERT 1 row WR → B delta 1 → exit。
     - apply B → MAIN 多了 1 row → MAIN count=4。
     - use A(snapshot) → SELECT MAIN COUNT：**应为 3（frozen）** ✔。
     - use C(live, new) → SELECT MAIN COUNT：**应为 4（latest）** ✔。
   - **TR-5.2 SNAPSHOT + WR DML ok**：use_branch (snapshot) → INSERT 1 WR row → old_version MAIN 查 ctid 正常（Snapshot 不被冻结，能拿到 MAIN 物理当前 tuple）→ delta count=1 → discard（不影响 MAIN）。
   - **TR-5.3 apply / cleanup 无 leak**：apply A（虽然 A 是只读的 WR 0 行也允许 apply）→ state=applied；exit('') → ob_ensure_unregister_snapshot pfree。
   - **TR-5.4 snapshot use re-entry 无 double leak**：连续 use_branch(name,snapshot) 两次（中间不 exit） → 第一次 snapshot pfree（branch_lifecycle.c L391）+ capture 新的；最终 exit 只 pfree 1 次。
2. **L1 pg_isolation_regress (T8)**：`ob_fr5_snapshot_freeze.spec` — s1 enter snapshot, main count=3; s2 main insert row, commit; s1 SELECT * FROM main → still 3 rows (frozen)。
3. **L2 pgbench (T8)**：50% snapshot use + 50% live use，MAIN side background insert，断言 snapshot 分支查询的 count 恒等于冻结时 MAIN 的行（no drift）。


### D.9.8 T5 踩坑归档（5 条，按发生时间顺序）

| # | 踩坑名 | 现象 / Symptoms | Root Cause | Fix / Workaround | 影响版本 |
|---|---|---|---|---|---|
| 1 | GUC assign 挂钩 PG 契约违背 — assign hook 可 ERROR half-writes GUC | A6 basic: `SET overlay_branch.current = 'nonexistent'` 后 **无 ERROR 抛** + `current_branch()` 居然返回不存在的名字（GUC drift），还会触发后续 double pfree | PG SetConfigOption 架构：**先写 GUC global**（new 值已经写入）**再调 assign hook**；assign hook 抛 ERROR 无法 undo 已写入的 GUC global。老代码把 "存在/state=active" 校验放在 assign hook 内 + 用 PG_TRY + SetConfigOption 手动回滚 + PG_RE_THROW → PG_RE_THROW 被 SetConfigOption 外层特殊处理，ERROR 被吞 + GUC 仍保留新错误值 | **按 PG 契约正确切分 check/assign**：check hook 做 **全部纯只读 SPI 验证**（存在+active），失败直接 return false（PG 保持旧 GUC 值 + 抛 ERROR）；assign hook 只做**纯副作用且永不失败**（若有任何条件导致 assign 无法完成，必须在 check hook 阶段全部拦掉）。老 PG_TRY/PG_CATCH/rollback 对全部删除 | T5 早中期 |
| 2 | `regd_count=0` → PopActiveSnapshot **内部 FreeSnapshot** 我们的 CopySnapshot | (a) use_branch('') 时报 `ERROR: double pfree in TopMemoryContext`（2 次，Section N L1910/L1924）(b) freeze n1_frozen_still_3 = f（看到了 MAIN 第 4 行而非 frozen 3 行） | PG [snapmgr.c:L753-L755](file:///home/ubuntu/work/postgres/src/backend/utils/time/snapmgr.c#L753-L755) PopActiveSnapshot 的 free 条件：`active_count==0 AND regd_count==0 → FreeSnapshot(as_snap)`。CopySnapshot 两个 count 默认都是 0 → 第一次 Pass1 push→pop 就把我们的快照从 pg 内部 free 了；但 BranchContext 还存着这个 DANGLING 指针 → (a) exit/cleanup pfree on dangling = double (b) 下一次 main 查询用 dangling → 实际读到 LatestSnapshot = MAIN 4 行 | 初始化 CopySnapshot 时 `copied->regd_count = 1`（伪 pin）；PopActiveSnapshot 永远不会 auto-free 我们的 snapshot（因为 regd_count>0）。真实 pfree 仅发生在 apply/discard 的 `ob_snapshot_cache_remove(bid)`（terminal cleanup）。配合 per-bid hash 生命周期（D.9.4），完全消除 DANGLING + double | T5 中后期（重大根因） |
| 3 | `ob_in_guc_setconfig` 递归 guard **自修改 bug**（assign hook 写了 caller-owned 标志位） | 早期 T5 中间态：Section M 的 SET/RESET 路径在 savepoint rollback 后触发 assign hook double cleanup → double pfree | SetConfigOption 内部对同一值会跑 **2+ 次内部 pass**（preflight/post-check/rollback restore）。老代码 assign hook 内部 `ob_in_guc_setconfig = false` 自己清了 guard flag → pass1 清 flag → pass2 误以为首次执行，跑了两次 real use_internal / 两次 cleanup → double | **caller-owned 模式**：flag 只能被外部 SetConfigOption 调用者（use_internal/exit_cleanup）用 `saved_flag=old; set=true; SetConfigOption(..); set=saved_flag` 模式保存/恢复。assign/check hook **只读取 flag，绝对不写** | T5 中期 |
| 4 | D7 PANIC：PL/pgSQL subxact-abort **RESTORE path** 调 assign hook use_internal → SPI inside abort-unwind | DO block 中 UPDATE 触发 throttle kickout（invalidated state），EXCEPTION handler 内 PG 自动恢复被 kickout 掉的 GUC old value → backend PANIC | PL/pgSQL 对 GUC 做 subxact savepoint；subxact abort 时 unwind 阶段即使 exception block 内也会跑 RESTORE（GUC assign hook）。老 assign hook 此时仍然无脑调 use_internal → SPI_connect/plan 在 abort-restore 上下文 → PANIC。 | assign hook non-empty path **LAYER-1 guard**：`if (ctx exists && !ctx->is_active) return immediately`。因为一旦 kickout 已经把 ctx.is_active=false + 清 snapshot ref，RESTORE 路径不需要做任何 use_internal（branch 实际已经 dead）。D7 PANIC 消除 | T5 中期 |
| 5 | re-enter same snapshot branch **重新 capture** 破坏 freeze 语义 | n1_a_frozen_still_3_not_4 = f：虽然 WARNING xmin=1194（B apply 后新事务 xmin）而非 1189（首次 capture） | 早期设计（D.9.4 重写前）"每次 use_branch snapshot 重新 GetTransactionSnapshot capture 新的"，exit 再 enter 就重新 capture（新事务 xmin），B apply 的 MAIN row 4 在新 snapshot 内自然可见 → 破坏 frozen at t0。根本原因：snapshot 是 "per-session-local ctx-owned" 而不是 "per-branch-id cache-owned"。 | **架构重写** D.9.4：引入全局 TopMC HTAB `ob_snapshot_cache`（bid key）；每个 bid 最多 capture 一次（hash miss 时），hash hit 永远复用同一 frozen 指针，ctx 只做 pin/unpin。freeze 测试 Section N 的 n1_frozen 从 f→t。 | T5 后期（重大根因） |

