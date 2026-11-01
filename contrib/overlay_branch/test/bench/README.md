# T8 L2 pgbench 并发压测 Fixture（非门禁，手动调试用）

> R7 约束重申：正确性第一，性能第二。**本 L2 fixture 不是 `make check` 门禁**（门禁 = L1 12/12 isolation specs + L3 19/19 pg_regress），仅在下列场景才需要手动跑：
> - 想对 FR2 V1 锁协议做高并发压力测试（CAS winner/loser 高频）
> - 想概率性触发 NOTIFY 路径（长连接 LISTEN 稳定）
> - 想复现 edge-case race 但 L1 12 specs 无法覆盖时

---

## 目录内容

| 文件 | 作用 | 运行时机 |
|---|---|---|
| `apply_contention_fixture.sql` | **一次性初始化脚本**：CREATE EXTENSION / pgbench_accounts 10000 行 baseline rows / 8 branches `bs_pgbench_{1..8}` | 首次压测前，**只跑 1 次**（idempotent） |
| `apply_contention.sql` | **pgbench custom script**：每连接按 `CLIENT_ID mod 10` 分配 4 种角色（60% WR writer / 20% branch switcher / 10% applyer / 10% discarder），随机 DML / apply / discard / live↔snapshot rotate | 每次压测持续执行，由 `pgbench -T 60` 控时间 |
| `run_perf_baseline.sh` | **性能基线驱动脚本**：`run_perf_baseline.sh [N] [M]`（默认 200000/20000）→ `LC_ALL=C.utf8 psql -f perf_baseline.sql`，并把全量 transcript `tee` 到 `perf_baseline_<N>_<M>.log` | 想取某组 (N,M) 规模的可比对 wall-clock 数字时手动跑（非门禁） |
| `perf_baseline.sql` | **可复现性能基线**：自建/自清 `public.ob_perf_base`（可控 set-based delta），按 performance.md 成本中心依次量 M1 MAIN seqscan / M2 M=0 叠加 / M3 全量叠加 / M4 PK 等值(P0) / M5 非 PK(P2) / M6 EXPLAIN / M7 delta 体积放大 / **M10a/b/c 写路径计时（UPDATE/DELETE/INSERT 各一段，§8.6 WR-1 基线）** / M8 apply 计时 / M9 apply 后 MAIN；每步夹 `pg_backend_memory_contexts` 泄漏观测 | 性能优化前后取对照、或验证 MF-1/MF-4/MF-5 修复效果时手动跑 |
| `perf_baseline_<N>_<M>.log` | **运行产物**（非手工编辑）：上一条脚本 `tee` 出的完整 psql transcript，留档做前后对照 | 由 `run_perf_baseline.sh` 自动生成；入库存档用于对照（不入库可忽略） |
| `srf_peak_rss.sh` | **内存峰值探针**：`srf_peak_rss.sh [rows] [max_growth_kb]`（默认 300000/16384）。跑两次 `overlay_main_plus_delta()` 全表扫描，只依据**第二次**扫描的 VmHWM 增量判 PASS/FAIL | 验证 MF-1 leak 9（SRF per-row scratch 落在 multi-call ctx）不回归时手动跑（非门禁） |
| `mixed_impact_fixture.sql` | **一次性初始化脚本**（G05/S28）：建两张互不共享行的表 `public.mi_accounts`（MAIN 前台，100000 行）/ `public.mi_agent`（Agent 分支，50000 行），幂等 | G05/S28 混合影响基线首次跑前，**只跑 1 次**（idempotent） |
| `mixed_main_oltp.sql` | **MAIN 前台 OLTP workload**（G05/S28）：每事务 `UPDATE` + `SELECT` 各一条索引点查，**不涉及 overlay_branch** | 被 `run_mixed_impact.sh` 作为 Phase A/B/C 的固定 MAIN 负载 |
| `mixed_agent.sql` | **Agent 分支 workload**（G05/S28）：每事务 `use_branch(:brname,'live')` + 一次叠加读 `count(*)` + 一次 delta 写 | 被 `run_mixed_impact.sh` 作为 Phase B 的 Agent 负载 |
| `mixed_agent_nobranch.sql` | **CONTROL workload**（G05/S28）：查询形状/负载与 `mixed_agent.sql` **完全相同**，但**不进分支**（无 `use_branch`），读写直落 MAIN | 经 `PGAGENTSQL=` 传给 `run_mixed_impact.sh`，作为 Phase C，用于分离「Agent overlay 路径」与「只是多一个并发客户端」 |
| `run_mixed_impact.sh` | **G05/S28 混合前台影响基线驱动**：`run_mixed_impact.sh [N] [T] [M]`（默认 100/30/1）→ 交错重复 + Latin-square 轮换相位，中位数汇总 MAIN tps/p50/p95/p99 + CPU busy + Agent p99，`tee` 到 `mixed_impact_<N>_<T>_<M>[_tag].log` | 需要量化「同机同配置下有/无 Agent 分支会话对 MAIN 前台的影响」时手动跑（**非门禁，无 PASS/FAIL**） |
| `mixed_impact_<N>_<T>_<M>[_tag].log` | **运行产物**（非手工编辑）：上一条脚本 `tee` 出的完整 transcript（含各相位原始样本 + SUMMARY），留档做前后对照 | 由 `run_mixed_impact.sh` 自动生成 |
| `mixed_agent_own.sql` | **独立分支 Agent workload**（多 Agent 扩展性探测）：每客户端 `use_branch(:brpfx \|\| :client_id, 'live')` 进**自己的** branch + 一次叠加读 + 一次 delta 写；查询形状与 `mixed_agent.sql` 完全相同 | 被 `run_agent_scale.sh` 作为 B\<M\> case 的 Agent 负载 |
| `run_agent_scale.sh` | **多 Agent 并发扩展性探测驱动**：`run_agent_scale.sh [N] [T] "M_LIST"`（默认 16 15 "1 2 4 8"）→ 固定 MAIN、在同一交错运行里对 M 列表做 Latin-square 轮换，中位数汇总每 M 的 MAIN tps/Δtps%/p50/p95/p99 + Agent p99/tps + CPU busy% | 需要「MAIN 前台随并发**独立分支** Agent 数 M 增长的退化形状/拐点」时手动跑（**非门禁，无 PASS/FAIL**） |
| `agent_scale_<N>_<T>_<MLIST>[_tag].log` | **运行产物**（非手工编辑）：上一条脚本 `tee` 出的完整 transcript（含各相位原始样本 + SUMMARY），留档做前后对照 | 由 `run_agent_scale.sh` 自动生成 |

> 目录内分五类：`apply_contention*` = **并发正确性** L2 压测；`perf_baseline*` = **性能基线** L2 fixture（对照 [performance.md](../../doc/performance.md) 成本中心，只出数字不做 PASS/FAIL）；`srf_peak_rss.sh` = **内存峰值探针**（唯一带 PASS/FAIL 判定的 L2 脚本）；`mixed_impact*` + `run_mixed_impact.sh` = **G05/S28 混合前台影响基线**；`mixed_agent_own.sql` + `run_agent_scale.sh` + `agent_scale_*.log` = **G05/S28 多 Agent 并发扩展性探测**（后两者见下节，只出数字不做 PASS/FAIL）。

### srf_peak_rss.sh（MF-1 leak 9）

`overlay_main_plus_delta()` 是 FROM 子句 SRF；leak 9 曾把每行 scratch（`values[]`/`isnulls[]`）分配在 SRF 的 multi-call ctx（SRF 结束才删除），使整表扫描随行数线性增长。该增长是**语句内瞬态峰值**，语句结束后即释放，所以 `test/regress` 里的 `TopMemoryContext` 前后断言看不到它——只能看进程峰值 RSS。

**为何跑两次**：即便修复后，**首次**全表扫描仍会把 VmHWM 抬高 O(rows)（executor per-tuple ctx、SRF tuplestore、堆页在 allocator 里 churn，glibc 把已释放页留在进程堆不回吐给 OS——RSS 在语句结束后仍偏高）。这是**一次性 warm-up，有界，不是泄漏**。泄漏的签名不同：**每次**扫描都增长同一份内存。因此本探针以**第二次**扫描的 VmHWM 增量判定，阈值默认 16 MB（实测 second-scan growth ≈ 0.4 MB；带 leak 时每次约 80 MB）。

```bash
PGDATABASE=ob_bench ./test/bench/srf_peak_rss.sh 300000 16384
#   SRF_PEAK_RSS_PASS  (second-scan growth ≈ 0.4 MB)
```

> 目标库需能 `create_branch`（G01 支持矩阵：库内不得有视图等非 `r`/`S`/`i` relkind）。默认 `ubuntu` 库含 `pg_buffercache` 视图会报错，故示例用干净库 `ob_bench`。

### run_mixed_impact.sh（G05/S28 混合前台影响）

**目的**：量化「同一实例、同一配置下，额外挂一个 Agent 分支会话」对 MAIN 前台 OLTP 的影响（G05/S28 指标，见 [review_260926.md §12.6](../../doc/review/review_260926.md)）。**只出数字，无 PASS/FAIL**——G05 阈值仍 OPEN，须由 S04 选定部署/目标 workload 后、最迟 S24 前冻结。

三个 case 交错执行（每 case 重复 R 次，各列取中位数）：

| case | 组成 | 用途 |
|---|---|---|
| A | 仅 N 个 MAIN OLTP 客户端 | 基线（无 Agent） |
| B | N 个 MAIN + M 个 Agent（进分支） | 「有 Agent」 |
| C（可选） | N 个 MAIN + M 个 control（同负载但不进分支） | 分离「overlay 路径」与「多一个并发客户端」 |

```bash
export PGPORT=5432 PGUSER=ubuntu PGDATABASE=ob_bench
psql -f test/bench/mixed_impact_fixture.sql          # 只跑 1 次
./test/bench/run_mixed_impact.sh 100 30 1            # A/B，约 4.5 min
# 加 control（Phase C）：
PGEXTRASQL=test/bench/mixed_agent_nobranch.sql PGTAG=abc \
    ./test/bench/run_mixed_impact.sh 100 30 1
```

**方法论（消除伪影，缺一不可）**：

- **交错重复 + 中位数**：单次 A/B 被运行间方差主导；R 次交错（`PGREPEAT`，默认 4，须为偶数）取中位数。
- **Latin-square 相位轮换**：每轮让每个 case 落在不同位置（首/次/…），否则「后跑的相位更快」的位置效应与真实 Agent 影响无法区分。
- **对称 idle lead-in**：每个相位启动前统一空转 `PGLEADIN` 秒（默认 2），且 **Agent 不得在 lead-in 期间运行**——否则 Agent 只给该相位预热 CPU/页，造成偏置。
- **null test**（`M=0`）：无 Agent 时 A 与 B 应当相等，用于验证 harness 本身无偏。
- **control case**（C）：同负载但不进分支，单独分离「多一个并发客户端」的效应。

**实测（2026-10-30，8 vCPU，`max_connections=200`，原始样本见 `mixed_impact_*.log`）**：

| 运行 | 结论 |
|---|---|
| null test（M=0, R=4） | A=9753.20 B=9748.61 **-0.05%** → harness 无偏 |
| canonical A/B（100/30/1, R=4） | A=10214.25 B=10957.14 **+7.27%**；Agent p99 249.91 ms |
| 3-case（R=3） | A=9699.51 B=10328.93 **+6.49%** C=9439.30 **-2.68%**；B-C=**+9.42%**；Agent p99 branch 222 / control 77 ms |

**判读**：Δ 均为**正**（Agent 在场时 MAIN 更快），不可能是 overlay 读路径对 MAIN 的成本；3-case 显示 B(branch) > C(control)，说明 gap 追踪的是「额外客户端**写到哪里**」（branch → 隔离的 `pg_branch_delta`；control → MAIN 表 → autovacuum/WAL 竞争），而非 Agent 负载本身。**这些数字仅作基线留档，不作为达标依据。**

### run_agent_scale.sh（多 Agent 并发扩展性探测）

**目的**：量化「MAIN 前台固定负载下，并发**独立分支** Agent 数 M 增长时，MAIN 退化的形状与拐点」，即多 Agent 的并发扩展性。**只出数字，无 PASS/FAIL**（G05 阈值仍 OPEN）。

与 `run_mixed_impact.sh` 的区别：后者把**所有** Agent 客户端放进**同一个** branch；本脚本每个 Agent 客户端进**自己的** branch（`mixed_agent_own.sql`），因此测的是**跨分支**干扰（registry 行 / 每分支 advisory lock / GUC mode cache / state-change NOTIFY），而非分支内 delta 写竞争。

**关键方法论**：`A` 基线 + `B<M>`（每个 M in M_LIST）**全部放进同一次运行**做 Latin-square 相位轮换，再取中位数。把 M 扫描拆成「每个 M 单独跑一次」会把运行间漂移错误地归因到 M 轴上——在共享/小机器上尤其致命，会凭空造出一个不存在的拐点。`PGSCALE_CONTROL=1` 追加 `C<M>`（同负载但不进分支）以分离「overlay 路径」与「只是多几个并发客户端」。每个 Agent 相位用**相位独有前缀**新建一批空分支，保证各相位从空 delta 起步、可比。

```bash
export PGPORT=5432 PGUSER=ubuntu PGDATABASE=ob_bench
psql -f test/bench/mixed_impact_fixture.sql          # 只跑 1 次（复用 G05 fixture）
./test/bench/run_agent_scale.sh 16 15 "1 2 4 8"               # A + B1/B2/B4/B8
PGSCALE_CONTROL=1 ./test/bench/run_agent_scale.sh 16 15 "1 2 4 8"   # 追加 C1..C8
```

**实测（2026-11-01，8 vCPU / 9.7 GB VM，`N=16, T=15, R=4`，`max_connections=100`，原始样本见 `agent_scale_16_15_1-2-4-8.log`）**：

| case | MAIN tps | Δtps% | p99 ms | Agent p99 ms | CPU% | Agent tps |
|---|---|---|---|---|---|---|
| A (M=0) | 7647.48 | — | 3.87 | — | 49.50 | — |
| B1 | 7459.38 | −2.46 | 4.44 | 51.21 | 60.05 | 33.70 |
| B2 | 6277.85 | −17.91 | 5.52 | 59.14 | 71.90 | 62.24 |
| B4 | 4973.03 | −34.97 | 8.54 | 81.67 | 84.70 | 101.53 |
| B8 | 3647.48 | −52.30 | 12.28 | 122.14 | 93.00 | 139.02 |
| C1 | 7221.51 | −5.57 | 4.53 | 13.14 | 58.40 | 140.74 |
| C2 | 6864.92 | −10.23 | 5.44 | 15.07 | 67.10 | 266.35 |
| C4 | 5626.75 | −26.42 | 6.78 | 20.37 | 76.45 | 421.99 |
| C8 | 4641.62 | −39.31 | 9.23 | 35.03 | 84.15 | 554.67 |

**判读（诚实边界）**：

- **在这台 8 vCPU VM 上测不到「分支特有的并发上限」**：CPU% 随 M 单调升至 93%（B8）/ 84%（C8），箱子先被 CPU 打满，任何分支特有的串行化点在可见之前就被 CPU 饱和掩盖。绝对数字**无容量意义**，只反映本机规格（日志已记录 nproc/mem）。
- **同 M 下 B 比 C 退化更重**（M=8：−52.3% vs −39.3%）：分支 Agent 每事务 CPU 成本约为 control 的 **3–4×**（Agent p99 51 vs 13 ms；Agent tps 33.7 vs 140.7 @M=1），这是 B 更拖累 MAIN 的主因。M=1 时 B 反而**优于** C（−2.5% vs −5.6%），因为 control Agent 快 4× 反而往 MAIN 表灌了更多写负载——与 G05/S28「写落到哪里」的既有结论一致。
- **B 与 C 的 Agent 吞吐随 M 的扩展性几乎相同**（M=1→8：B 4.1× / C 3.9×，均 ≈50% 效率），皆由 CPU 饱和解释，**在测试范围内没有出现分支特有瓶颈（registry / advisory lock 串行化）的证据**。

---

## 典型运行步骤

假设 PostgreSQL 已编译安装到 `~/pg17`，扩展已 `make install`，PG 实例正常运行、端口 5432、测试数据库 `obtest`：

```bash
# 环境变量
export PGPORT=5432
export PGUSER=ubuntu
export PGDATABASE=obtest
export PG_REGRESS_DB=$PGDATABASE   # 让 apply_contention.sql 头部 shell 注释里的变量匹配

cd ~/postgres/contrib/overlay_branch

# ===== (1) 一次性 fixture 初始化 =====
psql -f test/bench/apply_contention_fixture.sql
# 预期输出里没有 ERROR；最后 8 个 branch 存在
psql -c "SELECT branch_name,state FROM overlay_branch.pg_branch WHERE branch_name LIKE 'bs_pgbench_%' ORDER BY 1;"
# 应看到 8 行 (bs_pgbench_1..8, state='active')

# ===== (2) 60 秒 32 客户端压测 =====
# -n: no vacuum of pgbench default tables（我们没创建 pgbench_history/branches/tellers，不需要）
# -M prepared: 使用 prepared statement，减少 parse 开销，真正打到锁竞争
# -j 2: 2 个 pgbench worker 线程
pgbench -n -c 32 -j 2 -T 60 -M prepared -f test/bench/apply_contention.sql
```

输出示例（实际数字取决于硬件）：
```
transaction type: test/bench/apply_contention.sql
scaling factor: 1
query mode: prepared
number of clients: 32
number of threads: 2
duration: 60 s
number of transactions actually processed: 1234567
latency average = 1.234 ms
tps = 25943.212345 (including connections establishing)
tps = 25978.112345 (excluding connections establishing)
```

---

## 4 种角色分布（每连接固定 = `CLIENT_ID % 10`）

| 角色 | 占比 | 每事务做什么 | 目标覆盖 |
|---|---|---|---|
| **WR writer (0..5)** | 60% | BEGIN; SELECT count WHERE aid=randid; UPSERT (INSERT ON CONFLICT DO NOTHING) id=10000+randid; UPDATE abalance+1; COMMIT | write_redirect DELTA INSERT 高频；snapshot/live 模式 delta 可见性；FR6 ReScan fresh materialization |
| **Branch switcher (6..7)** | 20% | `use_branch(bs_pgbench_{1..8}, live | snapshot)` 交替切换；进入时 cache-set；assign-hook RESTORE 路径 | GUC assign hook cache-hit 路径；ob_mode_cache set/remove；live↔snapshot 切换不触发 FR2 V1 死锁（零锁保证） |
| **Applyer (8)** | 10% | `apply_branch(random active branch)`；**正常会捕获大量 55000**（另一 session 已经 apply 过；FR2 V1 CAS 失败者 fast-fail ERROR，不产生死锁） | FR3 CAS winner/loser；FR2 Exclusive advisory 仅 1 持有者；MAIN merge 写路径；apply→mode_cache_remove；ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE(55000) 高频正确触发（不是挂死/死锁） |
| **Discarder (9)** | 10% | `discard_branch(random applied branch)`；同样 55000 是正常分支 | discarding CAS；delta 表批量 DELETE；symmetric snapshot/mode cache remove |

---

## 压测后验证 SQL（最重要，必须跑）

```sql
-- ===== V1: FR3 状态机无泄露（任何状态异常 state 都算 FAIL） =====
SELECT state, count(*) FROM overlay_branch.pg_branch
 WHERE branch_name LIKE 'bs_pgbench_%' GROUP BY 1 ORDER BY 1;
-- 理想 PASS 分布：active + applied 为主；少量 discarded；
-- FAIL 条件：出现 applying / discarding 行（= 状态机卡中间态 = BUG）

-- ===== V2: 绝无 applying / discarding 残留 =====
SELECT count(*) AS stuck_applying FROM overlay_branch.pg_branch WHERE state = 'applying';
SELECT count(*) AS stuck_discarding FROM overlay_branch.pg_branch WHERE state = 'discarding';
-- ✅ PASS：两行都 = 0

-- ===== V3: MAIN 行数基线精确 10000（= 初始化时的 seed） =====
-- NOTE: WR writer 事务里 UPSERT 的 aid=10000+randid 范围是 [10001,19999]
-- 如果 write_redirect 工作正常，这些行只写入 WR delta，不进 MAIN；
-- 如果有任何写 MAIN 泄露（FAIL#1 那种 CmdType split BUG 的变种），
-- MAIN count 会 >10000。这是我们最敏感的 DRIFT CHECK。
SELECT count(*) AS main_count, count(*) = 10000 AS main_baseline_ok
  FROM public.pgbench_accounts WHERE aid BETWEEN 1 AND 10000;
SELECT count(*) AS main_leak_rows  -- ✅ 理想 = 0；>0 = 写 MAIN 泄露 = CRITICAL BUG
  FROM public.pgbench_accounts WHERE aid > 10000;

-- ===== V4: FR4 NOTIFY 兜底统计（长连接下 NOTIFY 应被实际消费） =====
-- 可以查看 postgresql.conf 是否开启了 log_min_messages=debug1 来统计；
-- 或直接查 pg_stat_activity 中 overlay_branch GUC 非空连接数是否合理
SELECT count(*) AS sessions_in_branch FROM pg_stat_activity
 WHERE backend_type = 'client backend'
   AND (current_setting('overlay_branch.current_branch', true) IS NOT NULL
    AND current_setting('overlay_branch.current_branch', true) <> '');
```

---

## 什么输出 = "压测 PASS"

满足以下 4 条全部算过（**无任何一条是 tps 阈值**，R7 正确性优先）：

1. ✅ `pgbench` 本身无 `connection failed` / `FATAL` / `SIGABRT` 级错误（正常 ERROR 55000 是 expected，pgbench 不把 ERROR 当 FAIL，只是计入 latency）
2. ✅ **V2 stuck = 0**（无 applying/discarding 中间态卡住）
3. ✅ **V3 main_leak_rows = 0**（MAIN 表无任何 WR writer 写入泄露 >10000 的行）
4. ✅ **psql 后端进程无 crash / coredump**（`dmesg` 或 `journalctl -u postgresql` 看 segfault）

> tps 高低、ERROR 55000 数量、平均延迟这些都是**性能指标**，不作为 PASS/FAIL 判定，需要时记录到另一张性能表（V3 再考虑）。

---

## 已知 flakiness & 处理

| 现象 | 原因 | 处理 |
|---|---|---|
| 第 1 次运行 ERROR `branch bs_pgbench_X does not exist` | fixture.sql 初始化没成功执行过 或 另一个测试脚本把 branch discard 了 | 重跑 fixture.sql 1 次 |
| apply/discard 大量 55000 ERROR | **正常**（FR2 V1 CAS loser fast-fail，反而是 L1 ob_apply_mutex 证明的正确性体现）。tps 会低一些但不影响正确性判定 | 忽略。若想看更少 55000，调 `-c 8` 少客户端，或调 apply/discard 占比降低 |
| MAIN leak_rows 跑出 >0 | **CRITICAL BUG**，优先级最高。立即停压测，拿最小复现跑 L1 isolation 测试看能否缩到 permutation 级别 spec | 新开 debug session；记录到 progress_tracker.md 新增 I8.x 条目 |
| stuck_applying > 0 长时间 >30 秒 | CAS winner 在 MAIN merge 期间被挂住（可能是 MAIN 表级锁或 DDL 冲突） | 看 pg_locks; 若真卡了 kill -9 stuck backend 算 FAIL，开 I8.x 条目 |

---

## 与 L1 / L3 门禁的关系图

```
[L1 pg_isolation 12 specs] —— "所有并发关键拓扑精确调度"
         │  (FR2/FR3/FR4/FR5/FR6 逐条语义 verify)
         ▼
[L3 pg_regress 19 suites] —— "L1 外的功能 + FR4 LISTEN 单连接路径"
         │  (本扩展 make check 默认门禁 = L1 + L3 合计 31/31 zero-diff 2×)
         ▼
[L2 pgbench (本目录, 手动)] —— "高并发真实负载下的正确性兜底 sanity check"
         │  (NOT 门禁。用于性能探索 / 触发 L1/L3 没测到的边缘 race)
         ▼
     发现 BUG  →  缩到最小复现 →  写成新 L1 spec  或  新 L3 SQL test  →  回到门禁
```

**规则**：任何在 L2 发现的正确性问题，**必须被沉淀到 L1 或 L3 测试里**（一旦缩成可稳定复现的最小 case，就固化成 pg_regress / isolation 门禁），不能一直停留在 L2 手动看。
