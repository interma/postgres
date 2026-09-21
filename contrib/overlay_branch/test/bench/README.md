# T8 L2 pgbench 并发压测 Fixture（非门禁，手动调试用）

> R7 约束重申：正确性第一，性能第二。**本 L2 fixture 不是 `make check` 门禁**（门禁 = L1 5/5 isolation specs + L3 4/4 pg_regress = 9/9 全绿），仅在下列场景才需要手动跑：
> - 想对 FR2 V1 锁协议做高并发压力测试（CAS winner/loser 高频）
> - 想概率性触发 NOTIFY 路径（长连接 LISTEN 稳定）
> - 想复现 edge-case race 但 L1 9 permutations 无法覆盖时

---

## 目录内容

| 文件 | 作用 | 运行时机 |
|---|---|---|
| `apply_contention_fixture.sql` | **一次性初始化脚本**：CREATE EXTENSION / pgbench_accounts 10000 行 baseline rows / 8 branches `bs_pgbench_{1..8}` | 首次压测前，**只跑 1 次**（idempotent） |
| `apply_contention.sql` | **pgbench custom script**：每连接按 `CLIENT_ID mod 10` 分配 4 种角色（60% WR writer / 20% branch switcher / 10% applyer / 10% discarder），随机 DML / apply / discard / live↔snapshot rotate | 每次压测持续执行，由 `pgbench -T 60` 控时间 |

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
[L1 pg_isolation 5 specs 9 permutations] —— "所有并发关键拓扑精确调度"
         │  (FR2/FR3/FR4/FR5/FR6 逐条语义 verify)
         ▼
[L3 pg_regress 4 suites (basic/user/advanced/mvcc)] —— "L1 外的功能 + FR4 LISTEN 单连接路径"
         │  (本扩展 make check 默认门禁 = L1 + L3 合计 9/9 zero-diff 2×)
         ▼
[L2 pgbench (本目录, 手动)] —— "高并发真实负载下的正确性兜底 sanity check"
         │  (NOT 门禁。用于性能探索 / 触发 L1/L3 没测到的边缘 race)
         ▼
     发现 BUG  →  缩到最小复现 →  写成新 L1 spec  或  新 L3 SQL test  →  回到门禁
```

**规则**：任何在 L2 发现的正确性问题，**必须被沉淀到 L1 或 L3 测试里**（一旦缩成可稳定复现的最小 case，就固化成 pg_regress / isolation 门禁），不能一直停留在 L2 手动看。
