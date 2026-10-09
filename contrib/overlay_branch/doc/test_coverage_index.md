# Overlay Branch 测试覆盖索引 (21 entries: 12 L3 + 9 L1)

> L3 = pg_regress 单会话 SQL 文件 (12 个)；L1 = isolationtester 多会话隔离测试 (9 个 specs)。

| 文件名 (去掉前缀/后缀) | 中文覆盖范围 (30-60 字) | 对应 Rxx / Gxx / Bxx 编号 |
|----------|-------------------------|------------------------|
| basic (L3) | 基础用例：CREATE/USE/APPLY/DISCARD 全流程 + simple CRUD inside branch + 主表增量回读 | R01 (lifecycle 4 fns) + G01 (基础路径) |
| user (L3)  | 角色切换：SET ROLE 普通用户在分支写数据 / 跨 role 可见性 / pg_branch owner 语义比对 | R08 (multi-role visibility) + §G02 OWNER 矩阵 |
| scan_pk (L3) | BranchScan 走 PK 索引/UNIQUE 约束命中：等值查询 / 范围 / 纯 PK 场景，含 junk attnum 验证 | R02 (BranchScan PK align) + G07 |
| scan_type (L3) | 多列类型覆盖：int/bigint/text/numeric/timestamptz/date 的 PK 序列化 + BranchScan 类型 dispatch | R07 (PK serialize 多类型) |
| mvcc_bounds (L3) | Live Mode 下 MAIN 已提交写入的可见性边界；分支内对 MAIN 新版本 RC 级可见性 | R10 (MAIN write → Branch visible) + S05 MVCC |
| mvcc_usage (L3) | 典型 Live Mode 使用流：并发 MAIN 写入 → Branch 叠加读一致；含未提交 vs 已提交分界 | S05 Live MVCC § |
| mvcc_review (L3) | MVCC 历史 bug 回归；多 session 并发 apply/discard + state 机 correctness | S05 MVCC review § + S12 |
| puredelta (L3) | Pure-delta (主表空) + 仅 INSERT 路径；UPSERT 纯 delta 两阶段 (无 MAIN) | B1 (pure-delta path) + R04 INSERT |
| puredelta_mix (L3) | 混合 Pure-delta (主表有行) + INSERT promotion 4-phase + MAIN key 冲突预检 | B1 promotion rules + R21 INSERT pre-check |
| rentry (L3) | R03/R05/R06/R16/R19 入口漂移写负向 + R21 INSERT 约束 5 子用例 + §G02 ACL 10 子用例 (A1..E) + §G01 SCOPE 10+1 负向/基线 (A..Z) + Section 12 **A7 schema epoch registry × 12 subcases (D1-D12)**: D2 bool_or 存在性 / D5 CREATE→增量→DROP→55000 drift / D6 DROP+recreate→discard→fresh / D11 exit-before-discard / D12 UPDATE-exit+re-apply + **R23** schema drift 55000 / **R24** DDL restricted §A7 0A000 | R03/R05/R06/R16/R19/R21 + §G02 S13 全矩阵 + §G01 MVP RESTRICTED 7 categories + **R23 (A7 schema epoch registry drift)** + **R24 (A7 DDL restricted 0A000)** |
| upsert (L3) | 完整 B2 UPSERT：INSERT ON CONFLICT DO NOTHING / DO UPDATE 针对 PK 的 2-phase 23505 双侧冲突检测 | B2 (upsert impl) |
| **a2 (L3)** | D-2 review_260926 A2 Read/Write View Unification：A2.1 self-join (SJ1 12-pair / SJ2 baseline-EXCEPT-0 / SJ3 visual) / A2.3 SAME-CSS ReScan 3×14 deterministic proof / A2.4 non-lateral NL no-crash / A2.2 DECLARE/FETCH/MOVE cursor overlay-only 3 assertions (CUR1 14rows / CUR2 id=3 NOT EXISTS / CUR3 amt=111,112) | §A2 architecture (OPEN → ✅) |
| ob_apply_mutex (L1) | isolationtester: 2 session 并发 apply 同一 branch → CAS loser 55000, winner 成功; delta 写入期间状态 guard | S16 FR3 CAS + R17 write-set freeze |
| ob_state_inval (L1) | isolationtester: apply/discard 后 LISTEN ob_branch_state payload `<bid>:<new_state>` 广播; 相邻 session is_active() 立即失效节流重查 | S13 FR1 NOTIFY + FR4 throttled catalog recheck |
| ob_applying_freeze (L1) | isolationtester: APPLY 过程 MAIN 并发读/写 freeze 边界; discarded/applied 终态禁止二次 apply/discard | S04 Apply freeze § + FR1/FR5 boundary + R17 applying state guard |
| ob_snapshot_mode (L1) | isolationtester: Snapshot mode 冻结 MAIN 视图 + session 并发 MAIN 写 → Branch 不可见; 死锁防护 | S04 Snapshot Spec + FR4 Snapshot 节流 + R19 TTL kickout |
| ob_branchscan_rescan (L1) | isolationtester: BranchScan rescan 路径 cursor / NO MarkRestore 定位 / 并发 apply 后 recheck (G07) | S07 rescan + G07 read-only path invariant |
| pure_delta_upsert (L1) | isolationtester: pure-delta table INSERT ON CONFLICT DO NOTHING/UPDATE 并发 2-session 冲突不丢行 | B2 upsert concurrent |
| pure_delta_update (L1) | isolationtester: pure-delta table 并发 UPDATE SET v=v+1 同键 2-way 冲突 latest-wins 无写穿 MAIN | S08 pure-delta update pass + R02 chain |
| pure_delta_delete (L1) | isolationtester: pure-delta table 并发 DELETE + INSERT reborn 同键互斥，MAIN 始终零污染 | S08 pure-delta delete pass + R04 state transitions |
| **ob_view_unify_rescan (L1)** | D-2 A2.3 SAME-CSS deterministic ReScan proof: ctrl 写 14 overlay → s1 NL LATERAL 3 outer → g=1/2/3 inner_cnt 恒 = 14（若 rescan 4-step 合约漏掉 cursor-reset → 2/3 次 inner_cnt=0 → FAIL） | §A2 architecture A2.3 (OPEN → ✅) + S07 ob_branchscan_rescan 4-step contract verification
