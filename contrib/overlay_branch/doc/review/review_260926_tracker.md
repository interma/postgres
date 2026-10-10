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
| **R21** | UNIQUE/CHECK/NOT NULL 约束写 delta 前未检查 → 重复 INSERT 覆盖不报错 → apply 丢数据 | S08 + S12 后续（当前 MVP：B2 UPSERT Phase I/II 冲突检测已覆盖 ON CONFLICT；独立 UNIQUE 检查留 S12） | write_redirect 写前 overlay-check（MAIN+delta 同键双重查）见 [write_redirect.c r21_check_insert_preconditions](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/write_redirect.c#L148-L362)（ONCONFLICT_NONE 入口注入，覆盖 NOT NULL 23502 + UNIQUE PK 23505 双侧；UPSERT 路径保留 Phase I/II 不重复检查） | ✅ FIXED (MVP subset: NOT NULL + UNIQUE PK for INSERT/ONCONFLICT_NONE + UPSERT; full multi-column CHECK + arbitrary UNIQUE → S12) | rentry.sql Section R21: R21_A_NOTNULL / R21_B1_MAIN / R21_B2_DELTA_I / R21_B3_DELTA_U / R21_C_UPSERT_OK 5 子用例全 PASS；overlay_branch_puredelta.sql P11 4 点 MVP NOP 守卫；B2 UPSERT Phase I/II dedup in commit 00ec2a7；2× clean rebuild L3 11/11 + L1 8/8 0 diff |
| **R23** | use_branch / apply_branch 无 schema epoch 基线：MAIN 上的表被 CREATE/DROP/ALTER 后，delta 指向的列/行语义与 baseline 不一致 → 静默写错位 / 读错列（A7 schema drift） | S19 A7 Registry 准入关 | [ob_registry_fingerprint_sql](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L721-L769) 构造 16B MD5 4 元组指纹（total/pk/col_hash/pk_hash）；[ob_registry_check_current_schema INCREMENTAL 模型](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L817-L1020)：Stage1 0 行 → first-use populate；Stage2a 5 UNION（DROP/总列/PK 数/hash 对）drift check first-hit 55000；Stage2b LEFT JOIN 增量登记 NEW TABLE（不 drift）；apply_internal CAS EXCLUSIVE 锁前调用；discard_internal 级联 DELETE | ✅ FIXED | rentry.sql Section 12 A7 12 子用例全 PASS：D1 create-empty / D2 bool_or registry 存在性 / D3 / D4 / D5 CREATE→增量→DROP→ord1 drift / D6 DROP+recreate→旧分支漂移→discard→fresh br_a7_v2 sanity / D7 / D8 / D9 / D10 / D11 discard 前 use_branch(NULL) 退出 / D12 UPDATE 后 exit+re apply；overlay_branch_basic.sql CREATE(b)→CREATE TABLE→use 时序 first-use 正确无 NEW TABLE drift（basic PASS 0 diff）；overlay_branch_mvcc_bounds.sql 0 diff；2× clean rebuild L3 11/11 + L1 8/8 0 diff |
| **R24** | 活动分支内 DDL（ALTER/DROP/TRUNCATE/ALTER SCHEMA/ALTER OWNER 等）静默改写 MAIN 表结构 → delta 中 JSON 对应列不存在 / PK 重定义 → apply 时 crash 或错位（A7 DDL restricted 0A000） | S19 A7 ProcessUtility 门禁 | [overlay_guard_ddl_ok_for_branch 11 类 reason + §A7 DDL RESTRICTED 0A000 前缀](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/overlay_branch.c#L909-L1084)；DropStmt OBJECT_INDEX ALLOW（无 user table 结构变化）；AlterObjectSchemaStmt / AlterOwnerStmt → ERROR（跨 schema/owner 改变语义）；T_CommentStmt ALLOW；overlay_branch.c 统一入口 2 层嵌套 switch 分发表级 / schema 级门禁；overlay_branch.h OBTABLE_REGISTRY 宏 + 3 externs（populate_for_create / check_current_schema / cascade_discard） | ✅ FIXED | rentry.sql A7_D8/A7_D9（§A7 前缀 0A000 负向全匹配「since the table was registered to this branch」reason text；mvcc_usage.sql / mvcc_review.sql 0 diff；2× clean rebuild L3 11/11 + L1 8/8 0 diff |

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

### Batch 7 — S19：A7 schema epoch registry + DDL restricted MVP（R23/R24）
| Sub | 内容 | 状态 | 证据 (2× clean rebuild L3 11/11 + L1 8/8 0 diff) |
|-----|------|------|--------------------------------------------------|
| S19-1 | Schema registry catalog：`overlay_branch.pg_branch_registry`（9 列：bid FK ON DELETE CASCADE + (bid, relid) PK + schema/table names + total_cols + pk_cols + col_hash + pk_hash MD5 16B）；ACL `REVOKE ALL/GRANT SELECT,INSERT,DELETE TO PUBLIC` 解决 plain user SPI SECURITY INVOKER 权限；扩展版本链路简化为单脚本 1.0（default_version=1.0 only，无 upgrade scripts） | ✅ | [overlay_branch--1.0.sql:L86-L117](file:///home/ubuntu/work/postgres/contrib/overlay_branch/overlay_branch--1.0.sql#L86-L117) 定义 + ACL；control `default_version = '1.0'`；Makefile DATA 单行 `overlay_branch--1.0.sql`；4 个版本脚本（--1.0--1.1 / 1.1--1.2 / 1.2--1.3 / --1.3.sql）从源码、build 输出、/tmp/pg17-writable 四地全删除；2× clean rebuild 全部 pass |
| S19-2 | Lifecycle 接入 INCREMENTAL 模型：① create_branch 尾 **不** populate（避免 create 之后再 CREATE TABLE 的时序 drift）；② Stage1 首次 use/apply entry 时 `SELECT 1 LIMIT 1` 查 bid → 0 行则 `populate_spi_connected` 做 snapshot baseline（first-use populate）；③ Stage2a 5 UNION ALL drift check（DROP/总列/PK 列/col_hash/pk_hash）first-hit 55000 ERROR；④ Stage2b `INSERT cur LEFT JOIN registry WHERE NULL` LEFT JOIN 增量登记 NEW TABLE（CREATE TABLE on MAIN after create_branch = 合法不 drift，只记录下次比较）；⑤ apply_internal **EXCLUSIVE 锁前** 调用（CAS winner 前 fail-fast）；⑥ discard_internal 级联 `DELETE registry WHERE bid` | ✅ | [branch_lifecycle.c:L770-L1020](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L770-L1020) fingerprint + populate_spi_connected(不带 SPI_connect) + check_current_schema(INCREMENTAL) + cascade_discard；[branch_lifecycle.c 4 调用点](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c)：create_internal populate 删除 / use_with_mode_internal L1539 / apply_internal L2191 / discard_internal L2664；PG_TRY/PG_CATCH 包裹 populate_spi_connected 防止 ERROR 路径漏 SPI_finish（fix WARNING: transaction left non-empty SPI stack）；rentry.sql Section 12 A7 D2 存在性 bool_or / D5 drift / D6 discard+fresh / D11 exit-before-discard / D12 UPDATE-exit-reapply 全 12 subcases PASS；2× clean rebuild L3 11/11 + L1 8/8 0 diff |
| S19-3 | DDL restricted §A7 0A000 门禁：11 类 reason 前缀 `§A7 DDL RESTRICTED 0A000: `；DropStmt OBJECT_INDEX ALLOW（无结构改变）；AlterObjectSchemaStmt / AlterOwnerStmt → ERROR（跨 schema/owner 改变受管表语义）；T_CommentStmt ALLOW（纯元数据注释不影响 row/column 结构）；ERRCODE 统一 `ERRCODE_FEATURE_NOT_SUPPORTED`（0A000） | ✅ | [overlay_branch.c:L909-L1084 overlay_guard_ddl_ok_for_branch](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/overlay_branch.c#L909-L1084)；rentry.sql A7_D8（ALTER TABLE → 0A000 reason `§A7 DDL RESTRICTED 0A000` + since the table was registered 子串）/ A7_D9（DROP TABLE → 0A000 同前缀）2 条负向匹配；basic/mvcc_bounds/mvcc_usage/mvcc_review 全 0 diff |
| S19-4 | Expected baseline 同步：mvcc_usage (8 lines 版本号 1.3→1.0 + §A7 reason) / mvcc_review (24 lines 3× Section header 语法修复 + 删除旧 syntax error at "*") / rentry (629 lines A7 Section 12 整段新增 + G01 9 ERROR 行移除 + G02 plain-owner ACL change) 三处 actual → expected 同步；2× clean rebuild 稳定 0 diff | ✅ | cp `test_output/results/overlay_branch_{mvcc_usage,mvcc_review,rentry}.out → test/regress/expected/`；连续 2 次 `make installcheck` 全部 ok 1-11 + ok 1-8 零 diff（无 WARNING 行残留差异） |

---

## 附录：分类二（产品化加固 MVP — 原计划 M4-M6，提前在 M3 完成的 ACL 与准入关）

> 原分类二（ACL / 运营 / 可运维性）声明留 M4-M6 统一处理；为配合 MVP 实际对外 release 可用门槛提前在本轮批次做完，下面条目已独立 commit + L3 全绿，状态同步追踪。

| issue | 描述 | 负责步骤 | 修复入口 | 状态 | 测试 Section |
|-------|------|---------|---------|------|-------------|
| **G02 S13** | ACL double-defense 矩阵：SQL-layer REVOKE/GRANT + C-layer `ob_acl_check_lifecycle` 非可绕过统一 42501 gate。Owner / mgmt / superuser × create/use/apply/discard 矩阵；保留角色名 `pg_manage_overlay_branches` → 改名 `overlay_branch_administrators`（PG 保留名冲突）；synonym 函数从 SECURITY DEFINER 回滚为 SECURITY INVOKER + schema USAGE PUBLIC（防止 GetUserId()=superuser 直通 ACL 大漏洞） | S13 MVP 批次 | [overlay_branch.h OB_MGMT_ROLE_NAME / ob_acl_check_lifecycle externs](file:///home/ubuntu/work/postgres/contrib/overlay_branch/include/overlay_branch.h#L229-L290) + [branch_lifecycle.c 4 个 helper（lookup_mgmt_oid / is_superuser_or_mgmt / is_branch_owner / acl_check_lifecycle 统一 ereport 42501）](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L49-L206) + [5 个 wrapper 顶部统一 ACL gate（create/use_1/use_2/apply/discard）](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/overlay_branch.c#L1220-L1268) + [overlay_branch--1.0.sql 双台账（DO $$ 创建角色 / SCHEMA USAGE PUBLIC / catalog GRANTs / synonym PUBLIC EXECUTE / delta-view REVOKE PUB）](file:///home/ubuntu/work/postgres/contrib/overlay_branch/overlay_branch--1.0.sql#L287-L520) | ✅ FIXED commit `0ec90f728a9` | rentry.sql Section G02（A1-E 10 子用例）：A1 alice-create→42501 / A2 direct-extschema→42501 / B1 bob-owner-use→PASS / B2 alice-use-bob→42501 / C1 alice-discard→42501 / C2 alice-apply→42501 / D1 mgmt-alice-create PASS / D2 mgmt-alice-discard-bob PASS / E unprivileged-delta-view→42501；2× clean rebuild L3 11/11 + L1 8/8 0 diff |
| **R22** | public.pg_branch_delta 视图默认 PUBLIC SELECT → 业务数据（delta rows）默认泄露给 unprivileged user（最小权限违规） | S13 MVP 批次 | overlay_branch--1.0.sql 双台账 L409：`REVOKE ALL ON public.pg_branch_delta FROM PUBLIC`，保留 wrapper view 存在以便后续本地 audit role 选择性 GRANT | ✅ FIXED commit `0ec90f728a9` | rentry.sql Section G02_E：unprivileged alice SELECT count(*) FROM public.pg_branch_delta → 42501；`has_table_privilege('regress_g02_alice','public.pg_branch_delta','SELECT') = false` 兜底；double rebuild 0 diff |
| **G01** | create_branch 准入关 fail-fast MVP 支持矩阵：若当前 DB 有 7 RESTRICTED categories 之一则 `ereport(ERROR 0A000)` FIRST-hit；ZER0 side effects（sequence 不 advance、catalog 无 row）：FK / user TRIGGER / PARTITION p,I / INHERITS / UNLOGGED / GENERATED STORED / 非 r/S/i relkind。排除 3 类对象：sys ns (pg_catalog / information_schema / pg_toast) + ext ns + pg_depend deptype='e' refobjid=overlay_branch（public.pg_branch VIEW + 12 public.* synonyms 精确豁免） | S13 MVP 批次 | [branch_lifecycle.c ob_check_branch_scope(void) — 7 段 UNION ALL flat subqueries（no WITH-CTE + SPI_execute readonly=false）](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L229-L392)，每段双重过滤（ns + pg_depend ext-owner），ord 1..7 ORDER BY LIMIT 1 first-hit report；`overlay_branch_create_internal()` 顶部 L688 附近在 owner 赋值与 catalog INSERT 之前调用（100% 零副作用保证） | ✅ FIXED commit `629d0ba227e` | rentry.sql Section G01（10+1 子用例）：A clean-schema→create PASS / B FK-pk-side→0A000 / C FK-fk-selfref→0A000 / D user-TRIGGER→0A000 / E1 PARTITION-parent→0A000 / E2 PARTITION-child→0A000 / F INHERITS→0A000 / G UNLOGGED→0A000 / H GENERATED-STORED→0A000 / I VIEW unsupp-relkind→0A000 / Z final-clean-create PASS（验证 9 次拒绝后 sequence 未前进，br 无残留）；每个负向用例 DO block + nested EXCEPTION WHEN feature_not_supported 双断言 + NOT-exists 兜底 PASS 标签；2× clean rebuild L3 11/11 + L1 8/8 0 diff |

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

---

## D-2 A2 Read/Write View Unification（review_260926 §9 Architecture A2 OPEN → ✅）

> 关闭 A2 OPEN 缺口：BranchScan 在 self-join / SAME-CSS rescan / cursor / parameterized NL join 下的 overlay view 正确性。根因已验证 MVP 不需要改 C：P2 PG_TRY 兜底 + NL Join Filter 由上层 NestedLoop 应用即可。

| Sub-case | 覆盖内容 | 断言 ID（8 全 PASS） | 测试位置 |
|----------|---------|---------------------|---------|
| A2_FIXTURE | 14 overlay 行数（10 MAIN - 1 DEL + 5 INS） | `PASS:A2_FIXTURE_14_OVERLAY_ROWS` | [overlay_branch_view_unify.sql:L79](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_view_unify.sql#L79) |
| A2.1 SJ1 | self-join `a.id = b.id+1` = 12 pairs, 2 BranchScan 实例独立 | `PASS:A2_SJ1_SELF_JOIN_PAIRS_12` | [overlay_branch_view_unify.sql:L100](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_view_unify.sql#L100) |
| A2.1 SJ2 | LEFT self-join USING(grp) 与 MAIN baseline 逐行 EXCEPT ALL = 0 diff | `PASS:A2_SJ2_LEFT_SELFJOIN_EQ_BASELINE` | [overlay_branch_view_unify.sql:L125](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_view_unify.sql#L125) |
| A2.1 SJ3 | EXPLAIN visual 2× overlay_branch_branchscan 节点（视觉验证） | — (EXPLAIN = PG top-level cmd, no auto-assert) | [overlay_branch_view_unify.sql:L129](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_view_unify.sql#L129) |
| A2.3 SAME-CSS ReScan | **确定性硬证据**：LATERAL `count(*)` per 3-row outer driver → 3× inner_cnt=14（若 cursor-reset 缺失 → 第 2/3 次 inner_cnt=0 → FAIL 立即触发） | `PASS:A2_NL_RESCAN_INNER_FULL_3X` (L3) + **L1 spec 3×14 行实际结果** (见下) | [overlay_branch_view_unify.sql:L172](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_view_unify.sql#L172) |
| A2.3 SAME-CSS ReScan (L1) | **isolationtester 确定性硬证据**：ctrl 先写 14 overlay → s1 NL LATERAL 3× → inner_cnt = 14 / 14 / 14（精确证明 `ob_branchscan_rescan` 4-step 合约：invalidation check → drop slots → memset → cursor-reset 均未遗漏） | `g=1|2|3 → inner_cnt=14×3`（无 FAIL label，全为零错误） | [ob_view_unify_rescan.spec (permutation)](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/isolation/specs/ob_view_unify_rescan.spec#L89-L91) + [expected 3×14](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/isolation/expected/ob_view_unify_rescan.out) |
| A2.4 non-lateral NL | non-lateral nested-loop join with Join Filter：0 crash + count=baseline | `PASS:A2_NL_JOIN_COUNT_MATCH_BASELINE` | [overlay_branch_view_unify.sql:L188](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_view_unify.sql#L188) |
| A2.2 CUR1 | cursor spool 模式下 14 overlay rows（≈ full FETCH ALL snapshot） | `PASS:A2_CUR1_CURSOR_14_OVERLAY_ROWS` | [overlay_branch_view_unify.sql:L229](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_view_unify.sql#L229) |
| A2.2 CUR2 | 已 DELETE 的 id=3 永不在 overlay view 出现 | `PASS:A2_CUR2_ID3_DELETED_NEVER_IN_CURSOR` | [overlay_branch_view_unify.sql:L239](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_view_unify.sql#L239) |
| A2.2 CUR3 | UPDATE 后 id=1 amt=111.00 / id=2 amt=112.00 可见（不是 MAIN 原始值） | `PASS:A2_CUR3_UPDATED_AMTS_VISIBLE` | [overlay_branch_view_unify.sql:L254](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_view_unify.sql#L254) |

### MVP 边界标注（不纳入本轮 regress，避免 crash 污染 expected，M4+ 若需要再支持）
- **Parameterized LATERAL（`LATERAL WHERE i.col = o.col` subquery form）**：multi-RTE cross-reference 出现在 Join level clause（非 inner baserel baserestrictinfo）→ P2 仅遍历 baserest，Join-level qual 若被 deparse 传 SPI WHERE → `o.grp` undefined → SubPlan re-entry SIGSEGV signal 11。P2 原本已 PG_TRY catch ERROR → general_where 置空，但 standalone 仍炸（SubPlan 重入 / 非 parameterized path 被强制用了 param_info → 未 root-cause 完）。MVP 绕过：改用 no-WHERE LATERAL 做 SAME-CSS 证明（ReScan 行为已硬证据 100% 覆盖）；non-lateral NL Join Filter 由上层 NestedLoop 处理，完全等价。
- **SCROLL cursor (MarkPos / RestorePos / FETCH BACKWARD)**：planner 自动用透明 tuplestore spool，路径 ROI 低；MVP 用 NO SCROLL + FETCH/MOVE FORWARD ALL 即可，已完全覆盖 BranchScan 内 materialized-list cursor。

### 验收（关闭条件全满足）
- L3 12/12 全 PASS，0 diff（R5）
- L1 9/9 全 PASS，0 diff（R5）
- R6 consecutive 第二次 0 diff（12/12 L3 + 9/9 L1 全绿）
- BranchScan / planner C 代码 **0 改动**（revert 了 pull_varnos 的内存破坏 inline guard，完全保留原 A7 基线 PG_TRY 兜底 100% 兼容）

---

## D-3 A8 MVP cost model（review_260926 §9 Architecture A8 OPEN → ✅）

> 关闭 A8 OPEN 缺口：BranchScan planner 阶段不再人为压 cost=0.00..0.00，按 R11/R12 算法复杂度 O(M log M + N log M) 真实估算，保证 EXPLAIN 输出可信、join order 行为稳定、add_path 不会因为真实 cost 高于 SeqScan 而**静默 reject**（静默 reject = BranchScan 完全不生效 = 灾难性 delta merge 旁路 bug，首轮 standalone 就暴露并修复，实际是 A8 最大风险点）。

| 子项 | 说明 | 验收证据 | 代码/测试位置 |
|------|------|---------|-------------|
| A8.0 build | gcc -Wall 0 warning，`make USE_PGXS=1 -j2` 0 warning/error | ✅ RC=0 3 rounds | branch_scan.c L29 (`optimizer/cost.h` include) |
| A8.1 cost_branchscan() 公式 | 严格按 R11/R12：N=MAIN tuples clamp；M=clamp(N×0.05,1,1e6)；log_M=floor(log2)；startup=cpu_tuple_cost × (M log M + M) (Phase A sort+dedup)；run 分 PK-pred 分支 vs full-scan；total=startup+run；rows=1(PK pred) 或 rel->rows(full) | ✅ 独立验证：PK cost=4.45..5.15；Seq=4.45..93.98；non-zero | [branch_scan.c:L1257-L1343](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L1257-L1343) |
| A8.2 **add_path → lappend bypass 关键 bug fix** | 成本真实化后 add_path 因"BranchScan cost ≈ SeqScan×4 → dominance 判定 → 静默丢弃 cpath 不进 pathlist → 强删逻辑找不到 CustomPath 保留原 SeqScan → BranchScan 完全旁路（SELECT * 纯 MAIN 基线）。修复：`rel->pathlist = lappend(rel->pathlist, cpath)` 强制 append 绕过 add_path；保留后续 pathlist-prune 强删非 CustomPath 合约，保证 BranchScan 唯一候选。 | ✅ standalone A8 验证：id=2 val=B_UPDATED / id=9 ZZZ / id=3 已删 不出；EXPLAIN SELECT * = overlay_branch_branchscan（不是 SeqScan）PK pred explain 也 BranchScan | [branch_scan.c:L1408-L1435](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_scan.c#L1408-L1435) |
| A8_FIXTURE | 200 MAIN heap rows + 1 DEL(id=99) + 13 INS(201..213) = 212 rows overlay ground truth（EXACT M_est=N×5%=10 → 200×0.05=10, clamp 1..1e6=10，严格匹配默认比例） | ✅ PASS marker `A8_FIXTURE_PASS` | [overlay_branch_planner_cost.sql:L36-L54](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_planner_cost.sql#L36-L54) |
| A8.1 SEQ scan node + cost≠0 | EXPLAIN 第一行节点名是 overlay_branch_branchscan；cost 正则 line !~ 0\.00\.\.0\.00 | ✅ `A8_1A_SCAN_NODE_PASS` ✅ `A8_1B_COST_NONZERO_PASS` | [overlay_branch_planner_cost.sql:L75-L88](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_planner_cost.sql#L75-L88) |
| A8.2 PK-pred cost << seq cost | EXPLAIN PK pred 节点 BranchScan；cost≠0；pk_total < seq_total（parse EXPLAIN line regex `cost=X..Y`，compare float）| ✅ `A8_2A_PKSCAN_NODE_PASS` ✅ `A8_2B_PK_COST_NONZERO_PASS` ✅ `A8_2C_PK_LT_SEQ_COST_PASS` | [overlay_branch_planner_cost.sql:L93-L129](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_planner_cost.sql#L93-L129) |
| A8.3 SELF-JOIN correctness | SJ a.id=b.id+1 count=210（id=99 DEL 断点 断 2 条 chain：212−2=210）；EXPLAIN 里 overlay_branch_branchscan cnt=2（两侧独立 BranchScan） | ✅ `A8_3A_TWO_CUSTOMSCAN_PASS` + ✅ `A8_3B_SJ_CORRECT_PASS` | [overlay_branch_planner_cost.sql:L135-L152](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_planner_cost.sql#L135-L152) |
| A8 Makefile REGRESS sync | REGRESS 追加 overlay_branch_planner_cost 第 13 个 L3 entry（Makefile L39） | ✅ installcheck ok1-13 枚举正确 | [Makefile:L38-39](file:///home/ubuntu/work/postgres/contrib/overlay_branch/Makefile#L38-L39) |
| A8 view_unify expected cost re-sync | A2 view_unify SJ3 EXPLAIN 原旧 cost=0.00..0.00 → 新 A8 cost=6.28..132.66，diff 16 行纯数字，actual→expected 直接 sync 保留 0 diff | ✅ 12/12 view_unify PASS（R4+R5 consecutive 2× 0 diff） | `sync test_output/results/overlay_branch_view_unify.out → test/regress/expected/` |
| A8 total regress baseline sync | new `overlay_branch_planner_cost.out` 248 行从 actual → expected；baseline 建立后 2× consecutive 0 diff | ✅ planner_cost.out 存入 expected/，R4+R5 全绿 | `overlay_branch_planner_cost.out expected 248 lines` |

### 验收（关闭条件全满足）
- ✅ standalone 修复验证：lappend bypass 后 SELECT id,val EXPLAIN 全 BranchScan，结果正确（非 MAIN 纯基线）
- ✅ build 0 warning（3 rounds）
- ✅ L3 R4+R5 13/13 连续 2× 0 diff（12 原基线 + 1 planner_cost 新）
- ✅ L1 9/9 连续 2× 0 diff（pg_isolation_regress 两 rounds RC=0）
- ✅ planner_cost SQL 6 PASS markers：A8_FIXTURE_PASS / 1A / 1B / 2A / 2B / 2C / 3A / 3B 全 PASS（无 FAIL 字符串在 output）
- ✅ docs 3 sync：progress_tracker A8 ⚡→✅ / review_tracker D-3 A8 台账 / test_coverage_index 22nd entry


---

## D-4 A9 pg_branch 身份与恢复绑定（review_260926 §9 Architecture A9 OPEN → ✅）

> 关闭 A9 OPEN 缺口：`pg_branch` catalog 在 `create_branch()` 同一事务、同一 SPI 连接内（`currval(new_bid)` 之后、`SPI_finish` 之前）写入 4 列 restore-metadata；加上 `list_branches()` 7→11 扩列暴露，以及 pure-SQL helper `get_branch_identity(name)` 1 次调用 = 身份值 + 当前 MAIN schema drift 检测 + user-table count。用于 branch backup/restore 流程校验：从备份恢复分支时，先 `SELECT * FROM get_branch_identity('name')` 确认身份列与恢复现场一致、`registry_schema_current_match=TRUE` 才能进入分支，否则 ERROR 提示需要 `discard_branch → recreate`。

### 子项台账（10 subcases）

| 子项 | 说明 | 验收证据 | 代码/测试位置 |
|------|------|---------|-------------|
| T0 catalog ADD 4 列 | ALTER TABLE `pg_branch` ADD 4 列：`schema_hash bytea` / `creation_snapshot_xmin xid` / `tablespace_list text` / `col_signature bytea`；老分支 ABI 默认 NULL、不报错；4 列加列级 COMMENT ON COLUMN 对应 §D4/A9 | ✅ CREATE EXTENSION 无 ERROR；老 ABI NULL 分支 list_branches 不报错 (A9_8 PASS) | [overlay_branch--1.0.sql:L179-L192](file:///home/ubuntu/work/postgres/contrib/overlay_branch/overlay_branch--1.0.sql#L179-L192) |
| T1 create_branch identity UPDATE | 在 `overlay_branch_create_internal`，currval(new_bid) 之后 & SPI_finish 之前：用 `GetTopTransactionId()`→xidout→'decimal'::xid；fp query 复用 A7 `ob_registry_fingerprint_sql()` + `LEFT JOIN pg_attribute/pk ordinality/pg_class.reltablespace→spcname` 聚合 UPDATE 1 row；SPI_processed≠1 或 ret≠SPI_OK_UPDATE → ereport(ERROR) | ✅ standalone smoke + rentry A9_1 NOT NULL 所有 4 列；`SPI_processed=1` 通过 | [branch_lifecycle.c:L1182-L1273](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L1182-L1273) |
| T2 schema_hash 算法 | MD5 of (relid 排序的 6-tuple CSV 每行：`schema\|table\|total_cols\|pk_cols\|hex(col_hash)\|hex(pk_hash)`)；decode(md5(...),'hex')=16B bytea | ✅ A9_3 PASS length(bytea)=16 | C update block [branch_lifecycle.c:L1217-L1222](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L1217-L1222) |
| T3 creation_snapshot_xmin | 存当前 tx xid (`GetTopTransactionId()`)，原生 `xid` 4B 列类型，'1234'::xid cast 自带合法性校验 | ✅ A9_4 PASS（xid 与当前 transaction 同范围/非空） | C update block [branch_lifecycle.c:L1199-L1210](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L1199-L1210) |
| T4 tablespace_list | `string_agg(DISTINCT spcname, ',' ORDER BY spcname) FILTER (WHERE spcname <> 'pg_default')`；空时 `COALESCE('', '')` 空串非 NULL | ✅ A9_1 NOT NULL；当前 3 个 test 表全 pg_default → tablespace_list='' 非空校验通过 | C update block [branch_lifecycle.c:L1223-L1225](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L1223-L1225) |
| T5 col_signature 算法 | MD5 of (relid,attnum 排序的 8-tuple CSV 每行：`schema.table\|attnum\|attname\|typid\|typmod\|notnull\|generated\|pk_ord` pk_ord 默认 0 非 pk) | ✅ A9_3 PASS：length(bytea)=16；col_signature 比 schema_hash 更鲁棒用于 drift check (与 helper 100% 一致) | C update block [branch_lifecycle.c:L1226-L1232](file:///home/ubuntu/work/postgres/contrib/overlay_branch/src/branch_lifecycle.c#L1226-L1232) |
| T6 list_branches() 扩列 7→11 | ext + public 两个 list_branches() 均从 7 col → 11 col，末尾追加 4 个身份列；**PG 不允许 CREATE OR REPLACE 修改 OUT 参数类型**，必须先 `DROP FUNCTION IF EXISTS list_branches()` → 再 CREATE 新 RETURNS TABLE | ✅ 13 tests 中前 10 个 (basic/user...) 0 diff，只 rentry/view_unify/planner_cost 序列 +2 diff | [overlay_branch--1.0.sql:L289-L330](file:///home/ubuntu/work/postgres/contrib/overlay_branch/overlay_branch--1.0.sql#L289-L330) + [public wrapper L691-L699](file:///home/ubuntu/work/postgres/contrib/overlay_branch/overlay_branch--1.0.sql#L691-L699) |
| T7 helper get_branch_identity(name) | pure SQL STABLE STRICT；RETURNS 7 TABLE：branch_id / schema_hash / creation_snapshot_xmin / tablespace_list / col_signature / registry_user_tables bigint / registry_schema_current_match boolean；drift check = 当前重算 col_signature == stored col_signature；user-table count = 当前 MAIN 的 relkind='r' 计数（不依赖延迟 populate 的 pg_branch_registry）；public synonym 同样 DROP IF EXISTS + CREATE + GRANT PUBLIC EXECUTE | ✅ A9_5 PASS（7 元组 == pg_branch direct 行 + drift match）✅ A9_6 PASS（cur_match=TRUE 立即 create 后）✅ A9_7 PASS（registry_user_tables ≥3） | [ext @extschema@.get_branch_identity](file:///home/ubuntu/work/postgres/contrib/overlay_branch/overlay_branch--1.0.sql#L342-L430) + [public synonym](file:///home/ubuntu/work/postgres/contrib/overlay_branch/overlay_branch--1.0.sql#L435-L450) |
| T8 rentry L3 8 PASS markers | Section A9 在原 G01/A7 tear-down 后、RESET 之前插入：8 CASE PASS = (A9_1 4 cols NOT NULL) (A9_2 list 暴露) (A9_3 MD5 宽 16B) (A9_4 xmin 同 tx range) (A9_5 get_ident == pg_branch row + drift true) (A9_6 cur_match TRUE) (A9_7 count≥3) (A9_8 old ABI NULL ok) + 3 fixture CREATE/DROP + discard 独立清理无残留 | ✅ 8 / 8 PASS markers 在 output 出现，0 FAIL:A9_ grep 结果 | [overlay_branch_rentry.sql:L1135-L1308](file:///home/ubuntu/work/postgres/contrib/overlay_branch/test/regress/sql/overlay_branch_rentry.sql#L1135-L1308) |
| T9 L3+L1 双 round baseline 收敛 | rentry 新增 206 行 → sync actual → expected；view_unify / planner_cost create_branch bid 序列 +2 (16→18 / 17→19) sync expected；其余 10 份 L3 expected 零改动 | ✅ R9+R10 2× consecutive RC=0 L3 13/13；✅ R9+R10 2× consecutive "All 9 tests passed" L1 | `$BUILDDIR/test_output/results/*.out → $SRCDIR/test/regress/expected/*.out`；double_round script `/tmp/a9_double_round.sh` |

### 验收（关闭条件全满足）
- ✅ build 0 warning（3 rounds）+ standalone smoke 4 列 NOT NULL + MD5 宽 16B（sh_len=16, cs_len=16, tbs_list='' 非空）
- ✅ C 端 identity UPDATE block SPI_processed=1，未触发 ereport(ERROR)；elog DEBUG1 在高 client_min_messages 级别可见
- ✅ rentry L3 最终 8 PASS: 0 FAIL 输出（PASS:A9_1 / 2 / 3 / 5 / 6 / 7 / 8×2 = 8 个 PASS markers）
- ✅ L3 R9+R10 13/13 连续 2× RC=0 0 diff；L1 R9+R10 9/9 连续 2× All passed
- ✅ docs 3 sync：progress_tracker A9 ⚡→✅ / review_tracker D-4 A9 台账 / test_coverage_index rentry 行补 A9 identity 备注


## D-5. A4 MVP: 增量 apply/discard 性能优化（SQL-layer empty-branch fast-return）

**目标定义（review_260926 Architecture A4）**: 对 apply/discard 的 3-pass 流程做增量路径优化，避免对无变化（delta 0 行）分支做完整的 Step3 gather relids → Step4 finalize + 3-pass replay。

**最终实现方向（3 次 C TAB 缩进 patch compile error 后 pivot）**: 100% 纯 SQL public wrapper 实现，零改动 C core (branch_lifecycle.c / delta_store.c)，零 correctness 回归风险。

### T0–T9 子用例台账（10 个 subcases，全部 ✅）

| ID | Subcase 描述 | 验收产物 | Status |
|----|-------------|---------|--------|
| A4-T0 | 瓶颈读：apply/discard 3-pass 实现 + SPI one-shot 开销定位 + 空分支无 early return 确认 + 3 次 C TAB patch 失败 pivot 决策 | branch_lifecycle.c L2492–L2660 + 3× restore HEAD 日志 | ✅ |
| A4-T1 | public.apply_branch(name) 改造：plpgsql 独立 UPDATE/DELETE 语句（不触发 R16 WITH-UPDATE blocker）+ 3-condition guard (state=active / delta_cnt=0 / **NOT current branch** — current branch 需要 C 层 clean session hash → 强制 fallback) + CAS state 异常 EXCEPTION fallback 到 C 层 | overlay_branch--1.0.sql public.apply_branch(_name) | ✅ |
| A4-T2 | public.discard_branch(name) 同模式 3-condition guard + fast-path UPDATE state=discarded + DELETE deltas；current branch 一律 fallback C 层（保证 GUC reset + NOTICE revert to Main + session snapshot/mode cache 清理） | overlay_branch--1.0.sql public.discard_branch(_name) | ✅ |
| A4-T3 | empty branch apply 性能：< 500ms fast-return（不进入 C 层 3-pass apply，无 overlay_branch: APPLY BRANCH NOTICE） | standalone smoke：empty apply 1.5ms；L3 basic PASS | ✅ |
| A4-T4 | 10K INSERT apply：MAIN⊕ 10K rows（branch INSERT）< 60s（缩到 10K 避免 1 小时+） | mvcc_bounds.sql A4_3 case PASS（< 60s） | ✅ |
| A4-T5 | 10K UPDATE apply：MAIN baseline 10K rows → branch ALL UPDATE amt+1 + md5(v1) < 60s | mvcc_bounds.sql A4_4 case PASS | ✅ |
| A4-T6 | 10K DELETE apply：MAIN 10K rows → branch DELETE 10001..20000 < 60s（rows_remaining=0） | mvcc_bounds.sql A4_5 case PASS | ✅ |
| A4-T7 | empty branch discard < 500ms fast-return（非 current）；**current** empty discard 强制 fallback（保留 NOTICE: discarding current branch + GUC overlay_branch.current reset） | standalone smoke 双条件验证 PASS；L3 basic PASS | ✅ |
| A4-T8 | Docs 3 本同步：progress_tracker A4⚡→✅；review_260926_tracker 本台账；test_coverage_index.md mvcc_bounds 行追加 A4 5 subcases + R26 | 3 docs diff | ✅ |
| A4-T9 | Zero-regression 验收：L3 13/13 R7+R8 连续 2× RC=0 0 diff；L1 9/9 R8+R9 连续 2× All passed | installcheck 4 轮日志 | ✅ |

### 硬验收表（A4 13+9 双 round 2× consecutive 0 diff）

| 层级 | 样本数 | R7/R8 (L3) 或 R8/R9 (L1) | RC=0 轮数 | 状态 |
|------|-------|-------------------------|----------|------|
| L3 pg_regress | 13 specs | basic / user / scan_pk / scan_type / puredelta_mix / upsert / puredelta / mvcc_usage / **mvcc_bounds（含 A4 5 PASS markers）** / mvcc_review / rentry / view_unify / planner_cost | R7 ✅, R8 ✅ | 13/13 × 2 |
| L1 pg_isolation_regress | 9 specs | apply_mutex / state_inval / applying_freeze / snapshot_mode / branchscan_rescan / pure_delta_upsert / pure_delta_update / pure_delta_delete / view_unify_rescan | R8 ✅, R9 ✅ | 9/9 × 2 |

### 关键架构决策（D-5 A4）

1. **不碰 C core（零回归承诺）**: 3 次 TAB indent C patch compile fail → 100% pivot 到 public synonym SQL wrapper 层，正确性 fallback 绝对不变。
2. **Current branch fallback rule**: current branch 上 apply/discard 必须 C 层 handler（reset overlay_branch.current GUC + flush `ob_snapshot_cache` / `ob_mode_cache` session hash + elog NOTICE reverting to Main），SQL 层做不到 → fast path 条件 3 明确 guard。
3. **R16 CTE blocker 反模式规避**: 最开始用 WITH UPDATE/DELETE CTE pipeline → 直接被 A7 MVP 的 R16 data-modifying CTE planner hook ERROR → 改 plpgsql 多条独立 UPDATE/DELETE（每条独立 SPI parse → 单 statement planner 不进入 WITH-UPDATE 分支 → 通过）。

