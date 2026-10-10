# overlay_branch 运维 runbook（G03 + G04 + G06 MVP）

> 本文件对应 review_260926 §9 的 3 项 G 类 OPEN：
>   - G03：未达产品目标时缺继续/缩范围决策流程（FAIL → BLOCKED SOP）
>   - G04：早期原型缺时间盒与投入评估（13d 时间盒 manifest）
>   - G06：未审阅草稿随复制/备份传播缺范围策略（DBA 配置 SOP，PG publication / pg_hba / RPO）

---

## G03. 发布门禁 FAIL → BLOCKED 书面处置 SOP

**原则**：任何一项门禁（R01–R24 correctness / L3 13 + L1 9 双 round 0 diff / G01–G06 加固）未通过，**绝不** 推进到 S26 PACKAGE / S30 release。流程如下：

```
    门禁 FAIL
        │
        ▼
  ┌──────────────┐
  │ 1. 冻结现场   │ → git stash / commit (--no-verify 如需)，保留复现脚本 + 失败 expected
  └──────┬───────┘    pg_regress:  test_output/  results/  tarball 归档
         ▼
  ┌──────────────┐
  │ 2. 三选一判定 │ → (a) retry；(b) 缩范围继续；(c) BLOCKED 回设计
  └──────┬───────┘
         │  ┌────────── retry 2× max ──────────┐
         ├──┤ L3/L1 flaky threshold ≤ 0.1%   │ → 通过则继续；否则仍 BLOCKED
         │  └─────────────────────────────────┘
         │  ┌────────── 缩范围（MVP 子集）────────┐
         ├──┤ 把未通过项从支持矩阵移出，标成 ❌ RESTRICTED 0A000； │
         │  │ 更新 example_sql.md §9 / progress_tracker 行；       │
         │  │ 合入入口 guard 硬拦截；                           │
         │  │ 同时写入 review_260926_tracker.md §下一周期回补清单  │
         │  └────────────────────────────────────┘
         │  ┌────────── BLOCKED 回设计 ──────────────┐
         └──┤ 1. 停止一切实现代码；               │
            │ 2. design.md 新增 BLOCKED 条目；         │
            │ 3. 1 个工作日内出根因报告（RCA）；        │
            │ 4. 开新一轮 review 评估替代方案           │
            └────────────────────────────────────┘
```

### G03.1 retry 条件 + 缩范围决策卡（单页 A4 版）

| 判断项 | 条件 | 走的分支 |
| --- | --- | --- |
| 连续 2× round 0 diff 再次复现吗？ | 否（1/2 通过即可认为 flaky） | retry（max 2 次） |
| 未通过项 属于 **支持矩阵 ❌ RESTRICTED 已覆盖**（例：分区/ FK） | 是 | 缩范围：在 entry guard 加 first-hit 拦截 + 更新文档 |
| 未通过项 会造成 MAIN 数据写穿/静默错（L3 rentry G01 C/D/E） | 是 | **直接 BLOCKED** |
| 未通过项 是 **性能 > 阈值**（A4 10K 100s > 60s）且正确性=0 diff | 否正确性 OK | 缩范围：标记为 `LIMITED <= 5K` 支持，或 BLOCKED 调优 |
| 未通过项 是 L1 isolation 死锁 40P01（未声明的） | 是 | **直接 BLOCKED**（锁协议问题，重试无意义） |

---

## G04. 13 工作日（~104 人时）时间盒 manifest

> review_260926 §9 G04 要求：写入 S04 M1 负责人 + 时间盒 + 到期产物 triad。
> **历史实际（已归档 2026-09-26 → 2026-10-10）**：首版 review_260926 启动 → 13 工作日 × 单人 ≈ 104 人时 实际交付 A2/A4/A7/A8/A9 5 大架构项。
> **以下 manifest 用于下一周期（M4+ A1/A6 或 G03–G06 加固），在未冻结前保持模板，不提前声明认证。**

### G04.1 M4+ 下一周期 manifest 模板（冻结后填入具体值）

```
PROJECT: overlay_branch M4+ A1 snapshot + A6 BranchExecContext 结构性重构
OWNER  : [填负责人]
DATE   : [YYYY-MM-DD → YYYY-MM-DD]
HOURS  : N 工作日 × 单人 = 人时
TRIAD  :
  [INPUT 冻结]:   真实业务 schema + SQL 样本 SHA256；支持矩阵 ≥80% 已达；
                  A1/A6 设计文档初稿（design.md §E1 + §E2）
  [THROUGHPUT]:   L3 13 + L1 9 双 round 2× RC=0 0 diff；
                  G03 FAIL→BLOCKED 流程书面；
                  G06 复制/备份范围策略配置；
                  1 个真实业务工作流 E2E 脚本通过
  [OUTPUT/验收]:  1 commit squashed per 子项；
                  docs sync agent_workflow + ops_runbook 更新；
                  review 下一轮 台账 §D-7 关闭

TIME BOX HARD RULE:
  - 到期前 1 工作日执行 G03 门禁检查（R01–R24 / L3 L1 2× / G01–G06）
  - 不通过 = 立即走 G03 三选一，不延期
  - 如需延期超过 3 工作日：review_260926_tracker.md 追加 BLOCKED 条目 + RCA
```

---

## G06. DBA 配置 SOP（草稿不随复制/备份传播范围策略）

> **风险**：overlay_branch 的草稿分支（state=active/applying）里 `pg_branch_delta` 全是未审阅数据；
> 如果这些数据被 pg_basebackup / logical replication / pg_dump 意外带到 standby / 只读副本 / 开发恢复环境，
> 就造成了 **未审阅草稿泄漏**。以下 3 条配置 + 1 条 RPO/RTO 策略为 MVP 硬性建议，未配置前不建议进入生产。

### G06.1 pg_hba.conf 访问分离（连接层）

```conf
# /path/to/$PGDATA/pg_hba.conf — MVP 建议
# 行 1：overlay_branch 专用管理账号（ob_committer/ob_reviewer）只允许来自堡垒机网段
host    all     +overlay_branch_administrators  10.0.1.0/24     scram-sha-256
# 行 2：editor 账号只允许来自 CI/CD runner 或 agent 网关
host    all     ob_editor                       10.0.2.0/24     scram-sha-256
# 行 3：普通业务只读账号（main OLTP）必须 DENY 连接 overlay_branch catalog 库（如果放独立 DB）
#        或使用 pg_hba + database=!overlay_branch_db 方式分离
hostssl all     +app_users                      0.0.0.0/0       scram-sha-256
```
> 落地建议：使用独立 DB 存放 overlay_branch（`CREATE DATABASE branch_db` + `CREATE EXTENSION overlay_branch` in branch_db），
> 业务主库走 main_db，天然避免 pg_dump / basebackup 混载草稿 — 这是最简单 G06 形态。

### G06.2 Logical Replication 白名单排除（发布层）

```sql
-- 在 MAIN 发布库执行：CREATE PUBLICATION，明确排除 overlay_branch.* 和 pg_branch_delta / pg_branch 视图
CREATE PUBLICATION main_tables_only FOR TABLE
    public.products, public.orders, public.users;   -- 白名单式，只发布 MAIN 业务表

-- 反模式 ❌（不要用）：CREATE PUBLICATION all_tables FOR ALL TABLES;
-- 因为 overlay_branch 会在 schema overlay_branch 下有 catalog，FOR ALL TABLES 会把草稿一起发布。

-- 检查：发布里有没有 overlay_branch schema 下的表？必须=0
SELECT schemaname, tablename
  FROM pg_publication_tables
 WHERE pubname  = 'main_tables_only'
   AND schemaname IN ('overlay_branch', 'pg_catalog');
-- Expect: 0 rows。

-- (Optional) 若确实存在内部表，ALTER PUBLICATION DROP TABLE：
-- ALTER PUBLICATION main_tables_only DROP TABLE overlay_branch.pg_branch_delta;
```

### G06.3 pg_basebackup / pg_dump 备份排除（备份层）

```bash
# 备份 MAIN 业务数据时 — 如果 overlay_branch 在独立 DB（强烈推荐），直接备份 main_db 即可
pg_dump -d main_db -Fc -Z9 -f /backup/main_db_$(date +%Y%m%d).dump
# 分支数据放 branch_db：不备份（草稿，不需长期保存；已 applied 的结果在 main_db）
# 如需审计 trail：仅备份 list_branches() 结果（metadata），不备份 delta 内容

# 如果 overlay_branch 不幸和 MAIN 在同一个 DB（不推荐）— pg_dump -N 显式排除 schema
pg_dump -d both_db \
    --exclude-schema=overlay_branch \
    -Fc -Z9 -f /backup/main_without_overlay_$(date +%Y%m%d).dump
# 同时 pg_restore 恢复时，绝不要 --schema-only include overlay_branch
```

### G06.4 草稿 RPO/RTO（已 discard/超期分支的删除流程）

```sql
-- 建议的清理 cron（每周一 03:00，由 ob_committer 执行）
-- 条件：state = discarded 或 state = applied 且 applied_at < now() - interval '7 days'
-- (需要 applied_at 列在 pg_branch catalog，MVP 暂未有；可借用 get_branch_identity 的 creation_snapshot_xmin 粗估)

WITH stale AS (
    SELECT branch_name FROM public.list_branches()
     WHERE state IN ('applied','discarded')
       -- creation_snapshot_xmin 粗估时间（MVP 无 timestamp）
       AND creation_snapshot_xmin::text::bigint < (txid_current() - 1000000)   -- 约 1M 事务前 = old
)
SELECT discard_branch(branch_name) FROM stale;
```

> **RPO（草稿泄漏）= 0 要求**：overlay_branch 独立 DB + 不进入备份/发布白名单 = 草稿只在运行实例内存/磁盘，泄漏面最低。
> **RTO（恢复 MAIN）= 0（与 overlay 无关）**：MAIN 恢复流程不变，因为 apply 写的是 MAIN 原 heap；overlay_branch catalog 不恢复也不影响 MAIN 业务读。

---

## 附：G 系列 全 6 项 状态总结（2026-10-10 MVP manifest）

| G 项 | 状态 | 证据或 runbook 链接 |
| --- | --- | --- |
| G01 scope preflight（FK/trigger/分区/UNLOGGED/generated/inherits/views FDW） | ✅ MVP subset first-hit 0A000 | [branch_lifecycle.c L229-L392](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L229-L392) |
| G02 ACL double-defense editor/committer/reviewer 三权 | ✅ MVP subset 4 helper + 5 wrapper 顶部 gate | [agent_workflow.md §1 3 roles](file:///home/ubuntu/work/postgres/contrib/overlay_branch/doc/agent_workflow.md#L1) |
| G03 FAIL → BLOCKED SOP 书面流程 | ✅ MANIFEST 写入（本文件 §G03） | [G03 流程图 + 决策卡 A4](#g03-发布门禁-fail--blocked-书面处置-sop)（需真实 FAIL case 演练 1 次后升级 ✅ FIXED） |
| G04 13d time box + manifest | ✅ MANIFEST 写入（历史 104 人时归档 + M4+ 模板） | [G04 manifest](#g04-13-工作日104-人时时间盒-manifest)（需下一轮真实负责人填入后升级 ✅） |
| G05 MAIN 业务 CPU/IO/锁影响预算（S28 指标） | ⚡ OPEN（无 100 并发 baseline） | 冻结：S24 前不进入生产；真实接入时再补 100 OLTP + 1 Agent 基线 P999 |
| G06 草稿传播范围策略（pg_hba / publication / 备份） | ✅ MANIFEST 写入 3 配置 + 1 RPO/RTO | [G06 配置 SOP](#g06-dba-配置-sop草稿不随复制备份传播范围策略)（需 DBA 实机演练 1 次升级 ✅ FIXED） |
