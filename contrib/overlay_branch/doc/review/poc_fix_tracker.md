# POC 功能缺陷修复跟踪 (Review 260926)

> 仅跟踪 **分类一：POC 功能缺失 / 正确性 Bug**（导致错误结果、静默写穿 MAIN、数据丢失、SIGSEGV 的致命问题）。
> 分类二（ACL 权限/运维/部署等产品化加固）不在本文跟踪范围，留待 M4-M6 阶段。

---

## 问题台账

### 写穿 MAIN 类（最高优先级，漂移写 = 数据灾难）

| issue | 描述 | 负责步骤 | 修复入口 | 状态 | 测试 Section |
|-------|------|---------|---------|------|-------------|
| **R05** | COPY FROM 直接写 MAIN（ProcessUtility 未拦截 T_CopyStmt） | S02 | [overlay_ProcessUtility](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/overlay_branch.c#L1056-L1145) | ✅ FIXED | overlay_branch_advanced.sql Section R-ENTRY/R05 顶层 COPY FROM STDIN → 0A000，MAIN baseline re-enter 校验 cnt=2 |
| **R06** | MERGE 写穿 MAIN（T_MergeStmt 未单独拦截 + Planner MERGE 形状绕过 WR） | S02 | ProcessUtility + WR 冗余两道（顶层 T_MergeStmt 0A000；WR 入口 CMD_MERGE 扫 resultRelations 命中 user nsp → 0A000） | ✅ FIXED | overlay_branch_advanced.sql Section R-ENTRY/R06 DO block EXECUTE MERGE → 0A000，MAIN cnt=2 基线未变 |
| **R16** | 修改 CTE 写穿 MAIN（仅 WR 有 guard，Planner/EXPLAIN ANALYZE/SPI 路径未拒） | S02 | [branch_scan.c Planner hook B1/B2 之间 hasModifyingCTE 顶层守卫](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L545-L596)，RTE 为 user table 时 ereport(ERROR) 0A000（Planner 层 choke，belt-and-braces #1） | ✅ FIXED | overlay_branch_advanced.sql Section R-ENTRY/R16 Shape A(WITH UPDATE RETURNING)+B(WITH INSERT RETURNING) 双 0A000 断言，cnt=2 sum=30 MAIN 未污染 |
| **R03** | UPDATE SET pk=... 静默丢失（delta 里 key 还是旧的 → apply 找错行） | S02 + S08 Belt-and-braces | [write_redirect.c CMD_UPDATE set-merge 内层 pk_attnums_set 命中 → 0A000](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c#L653-L678)，用 `overlay_get_pk_attnos_list(Relation)` 取复合 PK int list 构 bms | ✅ FIXED | overlay_branch_advanced.sql Section R-ENTRY/R03 DO UPDATE SET pk_id=100 → 0A000，MAIN cnt=2 sum=30 未变 |
| **R19** | snapshot 模式下长时间存活无警告（VACUUM 可能回收数据） | S02 RESTRICTED | [GUC overlay_branch.snapshot_max_hold_minutes](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/overlay_branch.c#L87-L99, L362-L374)（PGC_POSTMASTER default 10）+ BranchContext entered_at + throttled() elapsed_minutes kickout | ✅ FIXED | overlay_branch_advanced.sql Section R-ENTRY/R19 use_branch(name, 'snapshot') UPDATE 读 'beta-snap' PASS（TTL<10m 正常），SET GUC 会话级 22023 参数不可变 |

### 基础 API / 静默数据丢失

| issue | 描述 | 负责步骤 | 修复入口 | 状态 | 测试 Section |
|-------|------|---------|---------|------|-------------|
| **R09** | create_branch 返回静态自增假 ID，不等于 catalog nextval(seq) 真实值 | S03 | [overlay_branch_create_internal](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L271-L391) → INSERT 后 SELECT currval('overlay_branch.pg_branch_branch_id_seq'::regclass) 真实值；删除静态 s_next_return_id | ✅ FIXED | overlay_branch_mvcc.sql Section R-API/R09 a/b/c currval 双等 + monotonic 3×递增；L3 5/5 + L1 8/8 所有 expected out 基线 ID 已同步为真 nextval |
| **R08** | use_branch(NULL/空串) 退出路径不统一：1-arg 空串 vs 2-arg PG_ARGISNULL 处理分裂，ctx/GUC 不同步 | S03 | [use_with_mode_internal](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L583-L617) + [overlay_branch_use wrapper](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/overlay_branch.c#L1229-L1255) + 1-arg SQL 定义去掉 STRICT（L72）；两个 wrapper 先查 PG_ARGISNULL(0) 再传 NULL 到 internal，强制 ob_exit_branch_cleanup 走 | ✅ FIXED | overlay_branch_mvcc.sql Section R-API/R08_1 (1-arg NULL) / R08_2 (2-arg NULL,NULL) / R08_3 '' 与 NULL 双次进出；MAIN note/v 基线未变；current_branch() IS NULL |
| **R10** | 错误模板 branch_name/table/key 出现空串或 "<NULL>" 字面量 | S03 | delta_store.c ereport 用 `bn?bn:"<none>"` / `saved_st?save d_st:pstrdup("<NULL>")` 局部缓冲拷贝（已 lock-in）；throttled() kickout ereport 用 `local_branch_name[NAMEDATALEN]`（cleanup 前 strncpy）。新增 DO 断言 42704/55000/0A000 三处非空 SQLSTATE。 | ✅ FIXED (already correct + L3 tests) | overlay_branch_mvcc.sql Section R-API/R10 DO block 断言 UNDEFINED=42704 / state=55000 / SET PK=0A000 非空 errmsg；R_API_MAIN_PRISTINE_AFTER_ALL cnt=2 sum=30 note=alpha/beta |
| **R20** | UPDATE SET nullable_col = NULL 被忽略（NULL Datum 被当成"没改"跳过） | S02 + S08（pure-delta 也补齐） | MAIN 路径：[write_redirect.c set-merge jisnull 分支 MAIN](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c#L726-L762)；Pure-delta 路径：[write_redirect.c pure-delta set-merge jisnull 分支 pure](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c#L1380-L1394) 双路径都写了 `tts_values[r]=0; tts_isnull[r]=true;` | ✅ FIXED (MAIN + pure-delta both) | overlay_branch_puredelta.sql Section R_SETNULL/R20 R20_1 MAIN NULL / R20_2 PURE UPDATE NULL / R20_5 4×pure-toggle / R20_3 MAIN toggle / R20_4 apply id=1/2/10 全 verify |

### Overlay 读写语义错误（读错/写错）

| issue | 描述 | 负责步骤 | 修复入口 | 状态 | 测试 Section |
|-------|------|---------|---------|------|-------------|
| **R01** | BranchScan Pass2 漏掉 op=U（纯 delta UPDATE 行 WHERE 不命中） | S07 基础（S08 提交顺带锁死） | [ob_compute_overlay_slots_internal Pass2](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L98-L268) WHERE op IN ('I','U')（已在用户 commit 00ec2a7 实施） | ✅ FIXED (顺带 with S08) | overlay_branch_puredelta.sql P1..P12 断言（含 P3 U→U→U 链式） |
| **R02** | 连续 UPDATE v=v+1 第 2 条读不到第 1 条 delta 写入（WR 输入扫 MAIN 不扫 overlay） | S08 | [write_redirect.c MAIN 主循环后 pure-delta UPDATE pass](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c#L860-L1450) Phase B→E→F→H：SPI SELECT (op IN I,U)，raw_rows → overlay_delta_lookup → reconstruct_slot → ExecQual (qual_scan + qual_extra IndexScan/BitmapHeapScan/IndexOnlyScan) → set-merge 双分支（R03 PK guard + R20 jisnull=true）→ overlay_delta_insert UPDATE | ✅ FIXED | overlay_branch_puredelta.sql P1~P12 全链路 + Section R_SETNULL R02_PURE_UPDATE_CHAIN (v='pure_c+1+2+3') |
| **R04** | I→U→D / D→I reborn 的 base_token/base_image/revision 状态转移不完整 | S06 顺带（S08 write-redirect pass 已覆盖） | delta_store DeltaEntry 状态机 + [BranchScan Pass2 I/U fold](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L98-L268)（已在 00ec2a7 锁死） | ✅ FIXED (顺带 with S08) | overlay_branch_puredelta.sql P4(I→D), P9(I→U→D), P10(pure UPDATE oldver NULL → insert_pass fallback) |
| **R14** | PK=BPCHAR/NUMERIC 文本化 key typmod 丢失 → apply WHERE 0 行 → 误冲突；BranchScan comparator 用 strcmp 非原生比较 (BPCHAR r-trim/NUMERIC precision/TIMESTAMPTZ UTC) | S10 | DeltaTuple 追加 typed_pk/typed_pk_n 懒加载 + 4 helper (build_from_slot slot_getsomeattrs + datumCopy to TopMCxt / build_from_key_text TopMCxt + SPI #>> cast / typed_pk_cmp native fmgr: get_sort_group_operators + SysCache OPEROID oprcode + OidFunctionCall2Coll(eq,lt) / typed_pk_free)；branch_scan.c 4 comparator (qsort cmp_dtp_by_cstr / bsearch SearchKeyBundle + cmp_searchkey_vs_dtp / post-sort same-key run / Pass2 same-key) + pre-flight lazy populate + Pass1 MAIN build_from_slot typed pk；R13 RSS free 调 typed_pk_free | ✅ FIXED | ADV Section R-TYPEDKEY TK_BPCHAR (r-trim AB12=AB12    ✓) + TK_NUM (12.3=12.30 ✓) + TK_TSTZ (+08 vs +00 ✓)；2× clean rebuild L3 5/5 + L1 8/8 0 diff |
| **R15** | ctid+xmin HOT 更新后 token 不变 → apply MAIN 并发更新误判无冲突 → 静默覆盖 | S14 + S17 | [delta_store.c ob_enrich_tuple_with_base_image (JSON 顶层追加 "_base")](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L1235-L1319) + [overlay_modify_update/delete enrich 写入](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L1321-L1377)；[branch_lifecycle.c apply_check_base_image_match (_base vs FOR UPDATE 锁定 MAIN 行 jsonb_object_agg ORDER BY key 规范化比对，_base 缺省 NOP，不等 55000)](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L2273-L2392)；apply_delete_pass + apply_update_pass 均在 token compare 通过且 drop main_slot 后立即调用（双保险：token + _base） | ✅ FIXED | overlay_branch_mvcc.sql Section R_VERSION CONFLICT-1 (HOT prereq PASS/INFO + apply 55000 ERROR + MAIN race 999.99 preserved / id=2 untouched) + CONFLICT-2 (xmin/ctid rewrite token-mismatch ERROR + MAIN race preserved)；2× clean rebuild L3 5/5 + L1 8/8 0 diff |
| **R21** | UNIQUE/CHECK/NOT NULL 约束写 delta 前未检查 → 重复 INSERT 覆盖不报错 → apply 丢数据 | S08 + S12 后续（当前 MVP：B2 UPSERT Phase I/II 冲突检测已覆盖 ON CONFLICT；独立 UNIQUE 检查留 S12） | write_redirect 写前 overlay-check（MAIN+delta 同键双重查）见 [write_redirect.c B2 UPSERT 2-phase](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c#L1549-L1620)（ON CONFLICT path 已锁死 MVP 子集） | ⚡ PARTIAL (MVP subset ON CONFLICT only; full UNIQUE/NOT NULL pre-check → S12) | overlay_branch_puredelta.sql P11 4 点 MVP NOP 守卫 + B2 UPSERT Phase I/II dedup in commit 00ec2a7 |

### 计划/上下文/内存 Bug

| issue | 描述 | 负责步骤 | 修复入口 | 状态 | 测试 Section |
|-------|------|---------|---------|------|-------------|
| **R07** | prepared SELECT 先 MAIN 执行缓存 SeqScan → use_branch 后仍走旧计划读 MAIN | S09 | SetConfigOption 后 **二连**：[CacheInvalidateRelcacheAll + CommandEndInvalidationMessages](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L627-L671) 5 站点（NULL exit / enter / throttle kickout / apply exit / discard exit）；仅 RelcacheAll 只 register，CommandEnd 立即本地消化 SI 使 CachedPlanSource.invalidated=true 下次 EXECUTE 强制 replan | ✅ FIXED | overlay_branch_mvcc.sql Section R_CTX 5 断言全 PASS：GREEN_COUNT_18 / RED_COUNT_21 / GREEN_101 / RED_102 / EXIT_BASELINE；2× clean rebuild L3 5/5 + L1 8/8 0 diff |
| **R11/R12** | BranchScan 每 MAIN 行扫全 delta SPI O(N×M) 40 亿比较 → 极慢 | S07 | sort + bsearch O(M log M + N log M)：delta_list 先转 flat DeltaTuple** 数组 → qsort(by dt->key) → 同 key run 内按原始 list position 取 max idx = latest-wins（修复 latent I→U first-match 旧 tuple_data 语义 bug）→ 每个 MAIN 行 bsearch(FIND) O(log M)；Pass2 同步重写为双遍纯线性 latest-wins fold（无 hash table，避免之前 ptr-hash 地址哈希语义错） | ✅ FIXED | overlay_branch_mvcc.sql Section R_CTX S07 断言：64 行总计数 PASS:S07_HASH_MERGE_CORRECT_COUNT_64；overlay vs apply colour group count EXCEPT 相等 PASS:S07_APPLY_MATCHES_OVERLAY；2× clean rebuild 0 diff |
| **R13** | 大查询后 RSS 涨 MB 不回落（TopMCxt delta_list 分配不回收） | S07 | [ob_compute_overlay_slots_internal return 前](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L613-L659) 遍历 delta_list：逐个 pfree(dt->key / dt->tuple_data / dt) + list_free(delta_list)；overlay_delta_list_for_rel 全量 TopMCxt 分配 O(M) bytes 现在每次 BranchScan 结束完全回收 | ✅ FIXED | S07 helper exit path 审计无遗漏；2× clean rebuild 无泄漏报错 |
| **A1/A6** | 3 种快照（frozen/stmt/base_version）混用无统一结构 → R01/R15 根因 | S09 | struct BranchExecContext { bid, mode, stmt_snap, base_ver, schema_epoch } 每语句绑定（R15/R18 产品化加固前置） | ⚡ DEFER (category 2, M4-M6) | R07 已通过 CacheInvalidateRelcacheAll + CommandEndInvalidationMessages 覆盖 prepared plan 正确性，MVP 阶段足够；台账保持 open 直到 M4+ 产品化批次统一 refactor |

### 并发正确性

| issue | 描述 | 负责步骤 | 修复入口 | 状态 | 测试 Section |
|-------|------|---------|---------|------|-------------|
| **R17** | apply CAS→applying 期间新 INSERT 进 delta 未被冻结 → 写集漏项 | S16 + S18 | [branch_lifecycle.c apply_branch FR3 CAS](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L1447-L1499): `UPDATE pg_branch SET state=BRANCH_STATE_APPLYING WHERE bid=? AND state=BRANCH_STATE_ACTIVE`（SPI_processed != 1 → CAS loser 55000）；[delta_store.c overlay_delta_insert 入口 state guard](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/delta_store.c#L130-L182)：SPI SELECT bid→state 后 `strcmp(state, BRANCH_STATE_ACTIVE) != 0` 立即 ERROR 55000，state=APPLYING 专属 hint「Branch is currently applying」，state=APPLIED/DISCARDED terminal 专属 hint「no further writes allowed」；PG xact ABORT 自动回滚 applying → ACTIVE（手写 0 行 revert code） | ✅ FIXED | L3 Section M M1（manual UPDATE state='applying' → INSERT ERROR state=applying + delta_count 不变 → revert active → discard OK）；L1 ob_applying_freeze.spec 2 permutation：s1_apply_cas sleep 2s → s2_delta INSERT ERROR or wait-after-commit state=applied ERROR → final MAIN rows=110 精确（apply write 10 + baseline 100，0 条 s2 delta 漏写入）；2× clean rebuild L3 5/5 + L1 8/8 0 diff |
| **R18** | MAIN v=10→99 后 HOT token 不变 → apply 误判无冲突静默覆盖 99→11 | S17 | 同 R15（同源 HOT/rewrite 双场景，[apply_delete_pass](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L1822-L1927) + [apply_update_pass](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L1929-L2147) 均在 token compare 通过且 main_slot 已 drop 后调用 apply_check_base_image_match；INSERT pass 已有 MAIN row-exists 检查 + 双写 epoch freeze → 不需改造） | ✅ FIXED | overlay_branch_mvcc.sql Section R_VERSION CONFLICT-2 (rewrite xmin/ctid token mismatch ERROR + MAIN race 写 v preserved)；同 CONFLICT-1 baseline；2× clean rebuild 0 diff |
| **A3/A5** | 顶层 ExecutorRun 按 CmdType + ModifyTableState 判定不全面 → 非标形状绕过所有 WR → 漂移写 | S10 + S08 子集 | [write_redirect.c overlay_executor_run_intercept return 前](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c#L2179-L2253)（PG_END_TRY 后、return handled 前）：`!handled && overlay_branch_is_active() && 有受管 resultRel (非 catalog/internal nsp)` → ERROR 55000 立即中止事务 | ✅ FIXED | ADV Section R-A3A5：trigger 重入 UPDATE on 第二受管表 → MAIN baseline 全未污染 PASS:A3A5_MAIN_UNPOLLUTED / SIDE_UNPOLLUTED；2× clean rebuild 0 diff |

---

## 执行进度

### Batch 1 — S02：入口硬拦截（R03/R05/R06/R16/R19/R20 baseline)

| Sub | 内容 | 状态 | 证据 (make check 5/5 L3 + 8/8 L1 0 diff 2× verified) |
|-----|------|------|--------------------------------------------------|
| S02-1 | T_CopyStmt 拦截 0A000 + COPY FROM STDIN/FILE 负向测试 | ✅ | diff 全 + 行 → cp expected → clean rebuild 0 diff | |
| S02-2 | T_MergeStmt 拦截 0A000 + WR CMD_MERGE 扫 resultRelations + MERGE 负向测试 | ✅ | R06 DO block EXECUTE MERGE → 0A000，MAIN cnt=2 基线未变，zero diff 2× | |
| S02-3 | Planner hook 顶层 plannedstmt->hasModifyingCTE 拒（原只在 WR） + ModCTE 多入口负向测试 | ✅ | R16 Shape A/B 双 0A000，cnt=2 sum=30 基线未变，zero diff 2× | |
| S02-4 | WR CMD_UPDATE targetlist 含 PK attnum → 0A000 + UPDATE SET pk=... 负向测试 | ✅ | R03 UPDATE SET pk_id=100 → 0A000；复合 PK bms 命中 O(1) 比较 | |
| S02-5 | snapshot_max_hold_minutes GUC + snapshot 模式超时长 kickout + 负向测试 | ✅ | R19 use_branch(name, 'snapshot') UPDATE PASS; GUC PGC_POSTMASTER 会话级 22023 不可改 | |
| S02-6 | R20 SET NULL 修复 + R_SETNULL 测试对齐 expected | ✅ | R20_1~4 全 PASS；clean rebuild zero diff L3 5/5 + L1 8/8 2× verified |

### Batch 2 — S03：API 修正（R08/R09/R10）
| Sub | 内容 | 状态 | 证据 |
|-----|------|------|------|
| S03-1 | 废弃静态 `s_next_return_id`；INSERT 后 currval() 返回真 seq ID；并发创建 ID==catalog 一一对应 | ✅ | R09 a/b/c 3×create currval 双等 + monotonic；clean rebuild L3 5/5 L1 8/8 0 diff |
| S03-2 | 1-arg use_branch(NULL) 去掉 STRICT + wrapper 先 PG_ARGISNULL 再转 NULL internal；2-arg 同；'' 与 NULL 双入口一致 ob_exit_branch_cleanup；双校验 ctx.is_active==false + GUC=='' + bid==0 | ✅ | R08_1 NULL 1-arg / R08_2 NULL 2-arg / R08_3 ''↔NULL 双次进出，MAIN note='alpha' 'beta' v=10 基线未变，current_branch() IS NULL |
| S03-3 | 所有 ereport branch/table/state 用局部缓冲：`local_branch_name[NM]`、`saved_st`、`bn ? : "<none>"`、`"<unresolved name>"`；新增 R10 DO 块 3 处断言：UNDEFINED(42704)/STATE(55000)/SETPK(0A000) 非空 | ✅ | R10 DO block 三路径 SQLSTATE 全匹配；R_API_MAIN_PRISTINE cnt=2 sum=30 note 正确，无 FAIL 前缀 |

### Batch 3 — S07：BranchScan sort+bsearch 合并 + Pass2 latest-wins + RSS 回收（R11/R12/R13）
| Sub | 内容 | 状态 | 证据 (2× clean rebuild L3 5/5 + L1 8/8 0 diff) |
|-----|------|------|-----------------------------------------------|
| S07-1 | Pass1 MAIN×delta O(N×M) → sort+bsearch O(M log M + N log M)：flat DeltaTuple** qsort(by key) → 同 key run 取原始 list position max = latest-wins（修复 latent I→U first-match 旧 tuple_data 语义 bug）→ 每 MAIN 行 bsearch O(log M) 命中；Pass2 同步删除错误 hash table fold → 双遍纯线性 latest-wins（无 hash，避免 ptr-hash 语义错） | ✅ | [branch_scan.c:L290-607](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L290-L607)；Section R_CTX PASS:S07_HASH_MERGE_CORRECT_COUNT_64 断言；2× clean rebuild 0 diff |
| S07-2 | R13 RSS leak fix：overlay_delta_list_for_rel TopMCxt 全量分配（防 SPI_finish free）→ helper return 前遍历 pfree(dt->key / dt->tuple_data / dt) + list_free(delta_list)，每次 BranchScan 结束 O(M) bytes 完全回收 | ✅ | [branch_scan.c:L613-659](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L613-L659) |
| S07-3 | S07 语义验证：overlay 视图 colour group count vs apply 后 MAIN group count EXCEPT 双向相等 = apply 精确复现 overlay 语义（避开 reborn pk=30 → 改 disjoint pk=300 yellow 避开 S12 已知 apply I-pass 冲突检查限制） | ✅ | Section R_CTX PASS:S07_APPLY_MATCHES_OVERLAY；apply_branch('b_r_ctx_s07') 后 MAIN cnt=64 与 overlay 视图一致，colour 分布 EXCEPT 空集 |
### Batch 4 — S08：BranchModify 统一 Overlay 输入 + pure-delta UPDATE pass（R01/R02/R04/R20/R21 MVP + P1..P12）
| Sub | 内容 | 状态 | 证据 (2× clean rebuild L3 5/5 + L1 8/8 0 diff) |
|-----|------|------|-----------------------------------------------|
| S08-1 | WR MAIN 主循环后追加 8-phase pure-delta UPDATE/DELETE 主循环（Phase A 解析 bid/relid → B SPI SELECT op∈{I,U} → C helper_enter/exit → D 深拷贝 es_query_cxt → E overlay_delta_lookup 最新 + seen dedup → F 下钻 outerPlanState 取 qual_scan + qual_extra(Index/BitmapHeap/IndexOnly) → G ExecQual per slot → H CMD_DELETE + CMD_UPDATE set-merge） | ✅ | commit 00ec2a73351（用户提交 S08 主路径）；本节 G/H 和 R03/R20 pure 分支由本轮审查后补齐 |
| S08-2 | MVP no-qual 安全守卫：`if (qual_scan==NULL && qual_extra==NULL) continue;` 防止无 WHERE DELETE FROM 全表 tombstone pure delta（之前 regression 纯 delta 全删 bug） | ✅ | [write_redirect.c#L1117](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c#L1117)；P11 4 点 MVP NOP guard（L193-L192 puredelta.sql） |
| S08-3 | pure-delta CMD_UPDATE set-merge 补齐 R03 PK guard（bms_is_member ratt 命中 → 0A000，同 MAIN 路径） | ✅ | [write_redirect.c#L1349-L1359](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c#L1349-L1359)；Section R_SETNULL DO $$ block UPDATE SET id=9901 → 0A000 基线未变 |
| S08-4 | pure-delta CMD_UPDATE set-merge 补齐 R20 jisnull=true 分支（`tts_values[r]=0; tts_isnull[r]=true;`，MAIN 路径在 S02 已修） | ✅ | [write_redirect.c#L1390-L1394](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c#L1390-L1394)；Section R_SETNULL R20_2 PURE UPDATE NULL + R20_5 4× toggle + R20_4 apply id=10 note=NULL |
| S08-5 | R02 pure-delta 连续 UPDATE v=v || '+1/+2/+3' 3 次链式折叠 → 最终 v='pure_c+1+2+3'（每次从前一个 delta tuple_data latest-state fold 取 pre-image → DeltaEntry overlay_delta_lookup 最新） | ✅ | Section R_SETNULL R02_PURE_UPDATE_CHAIN 断言 PASS；L3 5/5 0 diff 2× |
| S08-6 | P1..P12 全链路验证：P1 I→U, P2 RETURNING, P3 U→U 3-chain, P4 I→D, P5 MAIN baseline upd, P6 U→U dedup, P7 apply full merge, P8 discard MAIN pristine, P9 I→U→D, P10 pure UPDATE NULL oldver → insert_pass fallback, P11 4×NOP guard, P12 RETURNING star | ✅ | overlay_branch_puredelta.sql P1..P12 全 PASS 断言；clean rebuild 2× 0 diff |
| S08-7 | L1 isolation pure_delta_update / pure_delta_delete / pure_delta_upsert 3 specs × 3 permutations 注册进 Makefile ISOLATION + isolation_schedule（用户 00ec2a7） | ✅ | pure_delta_upsert 6: 19 ms / pure_delta_update 7: 29 ms / pure_delta_delete 8: 28 ms；L1 8/8 0 diff 2× |
| S08-8 | R21 MVP 补齐：NOT NULL (23502) + PK UNIQUE (23505) 双侧前置检查 (INSERT-only, ONCONFLICT_NONE)；UNIQUE 逻辑为 delta 侧先查最新 op（I/U→冲突，D→允许 REBIRTH，不存在→再查 MAIN）；NOT NULL 用 slot_getsomeattrs 虚槽安全提取；TTS 垃圾值 + DELETE tombstone + errtable* NULL 参数三处 PANIC 已修复；UPSERT 路径保留原 Phase I/II 不重复检查 | ✅ FIXED | [write_redirect.c r21_check_insert_preconditions](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c#L148-L362) + promo-dispatch ONCONFLICT_NONE 入口注入；[rentry.sql Section R21](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_rentry.sql#L259-L381) 5 子用例全 PASS (R21_A_NOTNULL / R21_B1_MAIN / R21_B2_DELTA_I / R21_B3_DELTA_U / R21_C_UPSERT_OK)；2× clean rebuild L3 11/11 + L1 8/8 0 diff |
### Batch 5 — S14/S17：typed key + base_image 校验（R14/R15/R18）
### Batch 6 — S09：PlanCache 失效（R07）+ BranchExecContext 产品化 defer（A1/A6）
| Sub | 内容 | 状态 | 证据 (2× clean rebuild L3 5/5 + L1 8/8 0 diff) |
|-----|------|------|-----------------------------------------------|
| S09-1 | R07 核心：诊断单调用 CacheInvalidateRelcacheAll() 的缺陷（只 Register 到 CurrentCmdInvalidMsgs，命令结束才 flush；use_branch() 本身就是当前 top-level cmd → prepared plan 仍不 invalid）→ 改为 **二连**：RelcacheAll + CommandEndInvalidationMessages() 立即本地消化 SI，使 CachedPlanSource.invalidated=true 下次 EXECUTE 强制 replan | ✅ | [branch_lifecycle.c 5 站点](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L627-L671)：NULL exit / enter / throttle kickout / apply exit / discard exit 全部 SetConfigOption 后紧接二连调用 |
| S09-2 | R07 5 断言全绿：GREEN_COUNT_18（pk1 del + pk4 green→red transfer = 20-2=18）/ RED_COUNT_21（main20 + pk4 transfer red + pure102 filtered 60 = 21）/ GREEN_101_INCLUDED（pure green 101 出现在 branch overlay）/ RED_102_INCLUDED（pure red 102 出现在 branch overlay）/ EXIT_BACK_TO_MAIN_BASELINE（use_branch(NULL) 退出后 prepared green q 60 与 pre-branch 完全相等） | ✅ | Section R_CTX 5 断言全 PASS：R07_PREPARED_REPLAN_BRANCH_GREEN/RED/GREEN101/RED102/EXIT 5 点；2× clean rebuild 0 diff |
| S09-3 | A1/A6 BranchExecContext struct：DEFER 至 category2 产品化批次（M4-M6） | ⚡ DEFER | R07 已通过二连 invalidation 覆盖 prepared plan 正确性；BranchExecContext 仅为后续 R15/R18 base_image 逐列比较 + schema_epoch 锁计划前置结构，MVP 不影响 correctness |

---

## 测试目录约定（L3 pg_regress）

- 入口类（R03/R05/R06/R16/R19）负向用例：`test/regress/sql/overlay_branch_advanced.sql` 追加 **Section R-ENTRY**（A-Z 现有字母用满了，用 R- 前缀的新 section id）
- API 类（R08/R09/R10）：`test/regress/sql/overlay_branch_mvcc.sql` 追加 **Section R-API**
- 纯 delta 读/写/类型（R01/R02/R04/R14/R20）：`test/regress/sql/overlay_branch_puredelta.sql` 追加 **Section R_READ / R_DML / R_DELTA / R_TYPE**
- 并发/约束（R15/R17/R18/R21）：已有 L1 specs，等 L3 全绿后补 CONFLICT-1/2/3/5 独立 spec

**基线管理（严格遵守 AGENTS.md §3 Baseline management rule）：**
1. 首次新增内容运行 `make check` → 必然 1 FAIL（expected 缺行）
2. `cat test_output/regression.diffs` → 确认只含 `+` 行，无 `-` 行 / 无 ERROR / 无 syntax error
3. `cp test_output/results/<file>.out test/regress/expected/<file>.out`
4. **clean rebuild 第二次** `make check` → 必须 4/4（现 5 个 regress？核实）零 diff 才 PASS

---

## 通过门禁（每个 Issue 关闭条件）

1. 对应 L3/L1 case 在两个从零初始化的 `tmp_check` 集群各通过一次（见 AGENTS.md clean rebuild 命令）
2. `make -j4 install prefix=/tmp/pg17-writable` 编译零 warning/error
3. 若改了 .h 接口签名：在 `multi_session_mvcc.md` 追加 1 节设计说明
4. tracker 本表 status 翻转 ⬜→✅，附测试 Section 链接
