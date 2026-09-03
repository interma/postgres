# overlay_branch — PostgreSQL 数据库级"草稿分支"扩展

> Speculative database state using table overlay and delta store.
> 在 PostgreSQL 里给 AI Agent / 草稿式工作流 提供 Git-Branch 风格的隔离能力。

## 它是什么

`overlay_branch` 是一个 PostgreSQL 扩展，它允许你在**不拷贝数据**的前提下，创建一个「数据库级别的草稿分支 (branch)」：

- **Branch 写操作自动重定向** — 分支内的 INSERT / UPDATE / DELETE 不写主表，只写入独立的 Delta 存储（`pg_branch_delta`）。
- **Branch 读操作自动叠加** — 分支内的查询自动做「主表最新可见版本 ⊕ 本分支 Delta」的合并（BranchScan）。
- **Live Branch 默认语义** — 不是严格的数据库 fork；没被分支改过的行永远跟随 Main 的最新提交，改过的行保留分支自己的版本。
- **事务性 Apply** — `APPLY BRANCH` 把分支增量合入 Main，带乐观冲突检测，冲突就报错交由业务层决定 merge 策略。

典型用途：AI Agent 工作流、交互式草稿编辑、多版本数据对比、A/B 变更预演。

## 设计文档

| 文档 | 说明 |
|------|------|
| [设计草案](doc/design.md) | 完整架构与设计决策：四层模型 (Branch Context / Write Redirect / BranchScan / Delta Store)、核心语义、V1 范围与未来扩展方向 |
| [目标 SQL 与输出示例](doc/example_sql.md) | **用户视角的最终目标接口**：CREATE/USE/DISCARD/APPLY BRANCH 全套 SQL + 期望输出、BranchScan 叠加效果演示、Apply 成功与冲突路径、Live Branch 语义演示 |

## 目录结构

```
overlay_branch/
├── include/               # C 头文件 (类型声明、函数声明)
│   └── overlay_branch.h
├── src/                   # C 源代码
│   └── overlay_branch.c   # 模块 init / GUC / 钩子 / 6 个 SQL-callable 函数
├── test/                  # 回归测试
│   ├── sql/               #   测试输入 SQL
│   └── expected/          #   期望输出 (待填充)
├── doc/                   # 设计文档 & 目标接口示例
│   ├── design.md
│   └── example_sql.md
├── overlay_branch.control # 扩展控制文件
├── overlay_branch--1.0.sql# CREATE EXTENSION 安装脚本
├── Makefile               # in-tree / PGXS 双模式构建
├── meson.build            # Meson 构建定义
└── README.md              # 本文件
```

## 构建 & 安装

当前采用 PostgreSQL contrib in-tree 方式构建（已在源码树 `contrib/overlay_branch/`）：

```bash
# 1) 先在 pg 源码根目录跑过 configure（示例来自当前环境）
cd /home/ubuntu/work/postgres
./configure --prefix=/home/ubuntu/pg17 \
    --enable-cassert --enable-debug \
    CFLAGS="-ggdb -Og -g3 -fno-omit-frame-pointer"

# 2) 构建扩展
cd contrib/overlay_branch
make -j4

# 3) 安装到 prefix
make install
```

> ⚠️ 启用扩展前必须先在 `postgresql.conf` 中追加：`shared_preload_libraries = 'overlay_branch'`，然后 **重启 postmaster**。

安装后在数据库里启用扩展：

```sql
CREATE EXTENSION overlay_branch;
```

验证安装：

```sql
SELECT current_branch();   -- 未进入分支时返回 NULL
```

## 跑测试

```bash
cd contrib/overlay_branch

# (A) TEMP-INSTANCE 模式（推荐，零依赖已有 postmaster，自动启/停）
make check

# or (B) USE-EXISTING 模式（复用一台已经把 shared_preload_libraries 配置好并已重启的 postmaster）
make installcheck EXTRA_REGRESS_OPTS="--use-existing --host=/tmp --port=15433"

# or (C) 手动 pg_regress（只在真的需要时用 — 下面 4 个参数一个都不能漏！）
pg_regress overlay_branch_basic \
  --inputdir=./test --outputdir=./test_output \
  --temp-config=$(pwd)/test/temp_instance_shared_libs.conf \
  --bindir=$(pg_config --bindir)
```

## 当前进度

把用户视角的 6 条 SQL 闭环（CREATE EXTENSION → CREATE b1 → USE → 3 条 DML → 叠加读 → 回 Main 验证主表没动）拆成 4 个 Step。Step 1 (Branch Context 真实化) 已完整验证通过。

- ✅ 扩展骨架（控制文件 / Makefile / meson.build / SQL 安装脚本 / C 头文件 & 源码骨架 / .gitignore）
- ✅ 代码骨架零警告编译通过，C 模块已生成 `overlay_branch.so`
- ✅ GUC 系统：`overlay_branch.current` (触发 USE/RESET branch)、`overlay_branch.enabled` 已注册
- ✅ 钩子链：`ExecutorStart/Run/Finish/End_hook`、`ProcessUtility_hook` 已链式安装（stub，Step 3/4 才填实）
- ✅ 独立 schema：扩展固定部署在 `overlay_branch` schema (non-relocatable)，catalog 表不碰 pg_catalog，public schema 有 view + SQL function wrapper 暴露裸名调用
- ✅ **Step 1: Branch Context 真实化 (2026-09-03 冒烟全通过, 0 SIGSEGV/SIGABRT 0 warning)**：
  - `create_branch(name)`：真写 `@extschema@.pg_branch`，重名报 duplicate，nextval 由 DEFAULT 自动分配（成功返回值为 session-local 自增计数，真 id 看 list_branches / public.pg_branch）
  - `use_branch(name)`：存在性检查 + state 必须 active；undefined/not active 报错；同时支持 `SET overlay_branch.current = name` / `RESET` 两种写法
  - `current_branch()`：返回当前进入的分支名；GUC+CurrentBranchContext 两路 fallback；RESET/discard 后返回 NULL
  - `discard_branch(name)`：真 UPDATE pg_branch.state = discarded + 删除对应 delta；丢弃当前活跃分支返回 NOTICE reverting to Main
  - `list_branches()`：纯 SQL 实现，LEFT JOIN 真表 + delta_count 聚合，7 列全部正确可读
  - `overlay_main_plus_delta(regclass)`：C stub SRF，返回 0 行 (Step 4 填实)
- ✅ **Step 2: Delta Store 真实化 (2026-09-03 冒烟 9/9 全通过, 0 SIGSEGV/SIGABRT 0 warning)**：
  - 6 个内部 SPI 函数全部真实化：`overlay_delta_insert` (UPSERT) / `overlay_delta_update` (alias insert) / `overlay_delta_lookup` (深拷贝 by-ref datum) / `overlay_delta_list_for_rel` (List * of DeltaTuple *) / `overlay_delta_count` (bigint) / `overlay_delta_delete_all`
  - 全限定名宏 `OBTABLE_DELTA` / `OBTABLE_BRANCH`：nested-SPI 新 connect 时也能定位表，不依赖父函数 `SET search_path`
  - bytea ↔ hex 双向编解码：C→SQL 写入用 `DECODE(hex,'hex')::bytea`，SQL→C 读用 `encode(col,'hex')` 验证
  - 内部 UPSERT `ON CONFLICT (branch_id,relid,key) DO UPDATE` 真生效：同一 key 重写后 COUNT 不变、updated_at > created_at
  - list_branches() LEFT JOIN count() = overlay_delta_count() 一致（b1=2, b2=0, 再 delete_all→全 0）
  - 附带 debug 3 SQL-callable wrapper（非 UX 永久部分，可后续 drop）：`overlay_debug_delta_insert` 返回 TEXT 诊断入口/参数/是否抛 ERROR，`overlay_debug_delta_count` / `overlay_debug_delta_delete_all`
- ✅ **Step 3: Write Redirect INSERT-only MVP (2026-09-03 冒烟 10/10 全通过, 0 SIGSEGV/SIGABRT 0 warning)**：
  - **关键修复 (多轮)**：① 嵌套 SPI / SPI_finish 后 SPIMemoryContext 释放野指针（pk/tdata 全 0x7F 毒化）→ 所有从 SPI 返回的字符串 / serialize* 返回值必须 pstrdup 到 TopMemoryContext；② 删 `_PG_init` 里 `process_shared_preload_libraries_in_progress` early return guard（毁灭性 bug：GUC 不注册 + 钩子不挂）；③ ExecutorRun hook 每 `ExecProcNode(subplan)` 后 ExprContext reset poison → serialize_tuple 结果移至 TopMemoryContext；④ JSON key 空 → **slot tupdesc 的 ValuesScan 槽 attname 全空**，必须改用 `RelationGetDescr(rel)` 拿真实列名。
  - `overlay_should_redirect(Relation)`：active 分支 + 非系统/非 overlay_branch schema + 普通 RELKIND_RELATION 才拦截；对扩展自己的 delta/pg_branch 表递归防护（直接跳回 standard_ExecutorRun，不写 meta-delta 死循环）；无 PK 的表 ERROR 大声提示（pk=ARRAY[..] 必须存在）。
  - `overlay_serialize_pk(Relation,Slot)`：按 pk 列顺序 → SPI `to_jsonb(ARRAY[$1,$2..])::text` → TopMCxt pstrdup，确保 ExprContext reset/SPI_finish 后 pk cstr 仍有效。
  - `overlay_serialize_tuple(Relation,Slot)` v4：逐列（Relation tupdesc 真实列名）手动拼 JSON → `"列名":"outfun 值"`，严格 JSON 转义（"`\` → `\"` / `\\`；\n\r\t\b\f；<0x20 → \uXXXX；NULL → `null`）→ **最后 TopMCxt palloc 一份拷贝返回**（绕开 ExprContext poison）。
  - `overlay_delta_insert(tuple_json_cstr)`：内部用 `quote_literal_cstr(JSON)::bytea` 构造 SQL bytea 字面量写入，绕开手动 varlena 头歧义 + SPI bytea datum 毒化；UPSERT `ON CONFLICT (branch_id,relid,key)` 正确。
  - `overlay_ExecutorRun(ModifyTableState CMD_INSERT)`：active 分支内，INSERT 改为 循环 `ExecProcNode(outerPlanState(mt))` → serialize_pk(TopMCxt) + serialize_tuple(TopMCxt) → overlay_delta_insert → pfree；`es_processed = ninserted` → 不调 standard（阻止主表写）。
  - MVP ERROR：UPDATE / DELETE 尚未实装 → 清晰 ERROR + HINT 说明（不静默失败，也不偷偷写主表）。
  - Step3d 10 步验收 10/10：Main baseline 2 行 INSERT 保留 → b1 INSERT 2 行 → Main cnt 仍 2（零污染）；b1_delta_count=2；物理 delta 行 pk=["3"],["4"], op=I；tuple_data decode→UTF8 得到 `{"id":"3","v":"v-b1-only"}` / `{"id":"4","v":"v-b1-4"}`；UPDATE/DELETE 明确 ERROR；discard 后 Main=2 delta=0；0 SIGSEGV/SIGABRT。
- ✅ **Step 4a: CMD_UPDATE/DELETE ExecutorRun 真 redirect (2026-09-03 冒烟 14/14, 0 SIGSEGV/SIGABRT 0 warning)**：
  - 解除 Step3 的 `U/D MVP ERROR`：ExecProcNode(子计划) 拿到真实 UPDATE/DELETE 候选行（含 junk `ctid`/`tableoid` atts）→ 列名按 NAME 匹配合并 SET 子句新值（不是 position/attno，避免 junk att 布局错位）→ `ctid` 字面量 `SELECT * FROM rel WHERE ctid='(blk,off)'` SPI 取旧行 → 整块包 TopMemoryContext（防止 ExprContext reset 毒化 slot）。
  - **写 delta**：`UPDATE` → key=serialize_pk(old)，op='U'，old_version=''，tuple_data=serialize_tuple(new merged)；`DELETE` → op='D'（墓碑），tuple_data=NULL。UPSERT `ON CONFLICT` 保证同 key 重写语义。
  - `overlay_should_redirect(Relation)` 递归防护：扩展的元表（pg_branch / pg_branch_delta）直接走 standard_ExecutorRun，永不写 meta-delta 死循环。
  - Step4a 14 步验收 14/14：Main baseline 3 行 → b1 `D id=2 / U id=1 / I id=4` → **Main 全程仍 baseline 3 行（id=1 v-base-1, id=2 存在, id=3 存在）零污染**；delta 恰好 3 行（`["1"]U has_tdata=t` / `["2"]D has_tdata=f` / `["4"]I has_tdata=t`）；无 SIGSEGV。

- ✅ **Step 4b: overlay_main_plus_delta C SRF 真 Main⊕Delta 叠加读 (2026-09-03 冒烟 8/8 全通过, 0 SIGSEGV/SIGABRT 0 warning)**：
  - **Live Branch 语义算法（2 Pass）**：
    - Pass1：SEQSCAN Main（SPI SELECT *）+ 每列 TopMCxt datumCopy → serialize_pk → `overlay_delta_list_for_rel` 线性查找匹配 key → `NULL match` 直出（passthrough）；`op='D'`（墓碑）跳过 + `ExecDropSlot`；`op='U'` `reconstruct_slot_from_delta` 覆盖输出。
    - Pass2：剩余 `!emitted & op='I'` 的纯 delta INSERT → reconstruct 追加。
  - **9 层连环 Bug 全部定位 & 修复（Cassert 构建精确定位）**：
    ① 手写 VARDATA memcpy varlena → 改用 `byteaout type output → convert_from(SQL)::jsonb` 原生路径（规避 compressed/toasted/1B/4B header 组合爆炸）；
    ② `datumCopy(typbyval=true)` 无拷贝 → 改用 `get_typlenbyval(atttypid)` 真 by-ref by-val 判定 + TopMCxt 拷贝；
    ③ **SPI col 1-based ↔ TupleDescAttr 0-based 错位**（delta 表 SELECT 6 列，col=3 key TEXT 对应 TupleDescAttr(td,2) 非 (td,3) — 错位后 key atttypid=1042 BPCHAR，触发后续 type lookup 0x7F7F7F7F）；
    ④ `slot → table_close(rel)` 后 rd_att 释放 → 一律 `CreateTupleDescCopy(reldesc)` 独立 tupdesc 副本；
    ⑤ CreateTupleDescCopy+MakeSingleTupleTableSlot 再 DecrTupleDescRefCount → PG17 cassert `tdrefcount > 0` SIGABRT；
    ⑥ `overlay_delta_lookup` 中 BPCHAR op 列 `DatumGetChar(d)` 截断 varlena 指针低位 → 改用 `getTypeOutputInfo(BPCHAROID) → OidOutputFunctionCall → cstring[0]`；
    ⑦ `overlay_delta_list_for_rel` 同上 BPCHAR 截断（Pass1 foreach 实际走 list_for_rel 非 lookup，第 8 层漏掉差点翻车）；
    ⑧ Pass1 开始 → 切 TopMCxt（`lappend` List 节点分配在 SPIMemoryContext → SPI_finish 释放 + 0x7F 毒化 → Pass2 lappend 触发 `IsPointerList` cassert SIGABRT，最后一层 9 号）。
  - **`reconstruct_slot_from_delta`**：bytea tuple_data → `convert_from(byteaout_lit::bytea,'UTF8')::jsonb` → `SELECT (x).* FROM jsonb_to_record(JSON) AS x(id int4,v text)` SPI → 每列 `get_typlenbyval(reldesc_attr)` datumCopy TopMCxt → `VirtualTuple`。列名匹配用 `NameStr(attname)` 比较，支持 `AS x(id,v)` 投影重排。
  - **最终 SRF 输出（Live Branch 语义，ORDER BY id）**：
    ```
     id |      v       
    ----+--------------
      1 | v-b1-UPDATED   (op=U 覆盖 Main v-base-1)
      3 | v-base-3       (无 delta，passthrough)
      4 | v-b1-NEW       (op=I 纯 delta 新增)
    (3 rows)            ← id=2 **完全不出现** (op=D 墓碑正确跳过!)
    ```
  - **Main 零污染铁律**：`main_FINAL_BASELINE_3 = 3`；discard_branch(b1) 后 `delta_cnt_0_final = 0`；Main 仍 baseline 3 行。

- ✅ **Step 7: Hard Guard 零静默穿透 (2026-09-07 16 场景全拦截, 0 漏网)**：
  - **6 级 Guard Order (最便宜→最贵)**：G0 catalog namespace white-list → G1 overlay_branch meta 表递归透传 → G2 relkind 白名单 (仅 RELKIND_RELATION) → G3 relispartition 拒绝 partition 根/叶 → G4 遍历 trigdesc 拒绝非 internal trigger → G5 `pg_constraint` FK COUNT SPI 拒绝任何 FK 主/从 → G6 PK 必须存在且非空。
  - **全链路接入点 (双重 Belt-and-braces)**：① `overlay_ExecutorRun` 入口 per-result-rel 循环 + 全 RTE 扫描（防 VIEW/MATVIEW rewrite 成 base-table 后漏拦）② `overlay_ProcessUtility` 原 passthrough 前插入 `overlay_guard_ddl_ok_for_branch` → 40+ 个 `T_*` 白名单（DML utility / CTAS / TRUNCATE / ALTER / DROP / TRIGGER / RULE / FOREIGN / SEQUENCE / VACUUM FULL / CLUSTER 全拦截）。
  - **统一错误界面**：`overlay_guard_ereport_fail` + HINT `Switch back to the MAIN database before running this statement`，所有场景带 DETAIL 精确说明命中哪一级 guard。
  - 16 场景 guard 冒烟全 ERROR 断言正确：`CREATE VIEW / MV / TRIGGER / FK / partition / TABLE rel / CREATE RULE / ALTER TABLE / DROP TABLE / TRUNCATE / CLUSTER / VACUUM FULL / seq nextval / INSERT INTO view(TODO rewrite) / REINDEX / REFRESH MV`。
  - Step7 后 Step1-4 pg_regress 0 diff (回归 guard 不干扰正常 regular PK table)。

- ✅ **Step 8: discard_branch 级联删除 (2026-09-07 烟雾 MAIN 不变)**：
  - `overlay_branch_discard_internal` 重写：统一走 `CurrentBranchContext` in-memory 清 + leaving_current 判据（不再直接写 GUC 字符串 `overlay_branch_current_name[0]`，避免 GUC framework free SIGABRT）。
  - 级联 DELETE：`DELETE FROM ` OBTABLE_DELTA `WHERE branch_id = $1` 真执行，之前的纯 state = discarded 只靠 LEFT JOIN count = 0 的"假清零"已修。
  - **幂等保护**：pg_branch 行 state != active（=discarded 或 applied）时，`overlay_guard_ereport_fail` "already discarded or applied; discard cannot be called twice on the same branch"。
  - apply_race 烟雾回归：3 delta → cnt=0 state='discarded' + double-discard ERROR + MAIN 仍 baseline 3 行不变。

- ✅ **Step 5: apply_branch 3-Pass 乐观原子写回 (2026-09-07 A/B/C 三场景全 PASS, Step1-4 pg_regress 0 diff)**：
  - **old_version 乐观并发 token (ctid + xmin)**：write redirect 时 `fetch_tuple_by_ctid` 回填 slot→tts_tid，`overlay_tuple_version(rel, slot)` 生成 `"<blocknum>:<offset>-x<xmin>"` 存 delta；apply 时 `ob_fetch_main_current_slot` 按 PK WHERE 重查 MAIN 最新行 → 再算 token 逐字 strcmp。任何 MAIN UPDATE rewrite / HOT / concurrent write 导致 token 变 → conflict ERROR 整个 apply 由上层事务回滚，MAIN 不变。
  - **3-Pass 原子 (D→U→I)**：每条 delta 独立 `ob_spi_one_shot` (connect→execute→SPI_finish)，绝不跨 finish 引用 SPI 指针；需要 outlive 的 datum/pk_texts/result 全部 `datumCopy / pstrdup` 到 TopMemoryContext。
  - **apply 内部 redirect bypass 安全 (ob_in_apply_operation + PG_TRY)**：`apply_internal` 3-pass 前置 flag=true，`overlay_should_redirect` 顶部直接 return false；整条 replay 包 `PG_TRY / PG_CATCH / PG_END_TRY`，conflict ERROR 长jmp 必走 CATCH 恢复 flag——否则后续 branch DML 永久 bypass 直写 MAIN（毁灭性 bug）。
  - **RESET GUC ghost-branch 双系统一致性修复 (2 个根因同时命中)**：① use_internal ENTER/LEAVE branch 时 `SetConfigOption("overlay_branch.current", ...)` 同步真实 GUC 字符串（配 `ob_in_guc_setconfig` anti-recursion 防 check hook→use_internal→check hook 死循环）；② `overlay_branch_is_active()` 双源真值判据：`CurrentBranchContext->is_active=true AND overlay_branch_current_name 非空`——彻底干掉 RESET 时 session GUC 值 = boot default "" 跳过 check hook 导致 is_active 残留 true、MAIN DML 被二次 redirect 进 delta 的 bug。
  - **Apply 烟雾 A/B/C 全验收**：
    - A (no-conflict)：MAIN 从 `1=v-base-1,2=v-base-2,3=v-base-3` 精确变 `1=v-b1-updated,3=v-base-3,4=v-branch-new-4`；state=applied delta_cnt=0；re-apply 正确 ERROR "not active"。
    - B (conflict)：RESET + MAIN UPDATE id=1→v-RACE-MAIN-WRITE **真实落 MAIN**（证明 ghost-branch bug 已修）→ apply_race old_version mismatch 抛 conflict ERROR + 事务回滚 MAIN 不变（仍保留 v-RACE-MAIN-WRITE 值）。
    - C (discard cascade 回归)：3 delta→cnt=0 state='discarded' + double-discard ERROR + MAIN 仍 baseline 不变。
  - 清理：所有 `delpass/updpass/inspass/apply[dbg]/bpk[dbg]` 共 50+ 条调试 NOTICE 统一降为 `elog(DEBUG1, …)`，client 默认 client_min_messages=notice 下不再看到任何内部调试输出。

- 🔜 未来：BranchScan（CustomScan 替换 SeqScan 透明叠加，不再需手动调 SRF）、多版本 A/B 预演命令行工具。

## 许可证

PostgreSQL License（同 PG contrib 其余扩展）。
