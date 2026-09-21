# AGENTS.md

> Agent-only instructions for the `contrib/overlay_branch` PostgreSQL extension
> (Apache 2.0 licensed — flat version 1.0 with NO upgrade scripts).
>
> For human-facing README and API docs, see `doc/` (links in §5 below).
> This file complements — not duplicates — what's in `doc/` by keeping the
> "tribal knowledge" an agent needs to pick up where the last agent left off:
> exact build commands, subtle API/signature gotchas, lock-ordering deadlock
> rules, layered test architecture, and a jump table to the right doc section
> for each deliverable (T1…T8).

---

## 1. Project overview (agent context)

**What this is**: A PostgreSQL contrib extension that provides
"overlay branch" semantics — each session can `use_branch(name)` to see a
snapshot + write-redirected branch-specific `pg_branch_delta` view of user
tables, then `apply_branch(name)` to 3-pass (D→U→I) merge back to MAIN, or
`discard_branch(name)` to throw away branch-local writes.  Think "git for
SQL rows" xact-scoped, heavyweight-lock-protected (V3 onwards).

**Current milestone (2026-09-19)**: V3 T1 → T8 ALL DELIVERED
(L3 4/4 + L1 5/5 = **9/9 zero-diff verified 2×+**, stale clean rebuild green).
See `doc/progress_tracker.md` §T8 acceptance row and `doc/multi_session_mvcc.md`
for full FR architecture and C-level implementation notes.

**Public API signatures (agent: copy-paste these, never guess)**:

| Callable | Signature | Notes |
|----------|-----------|-------|
| `create_branch` | `create_branch(branch_name text) RETURNS integer` | **SINGLE arg ONLY** — no DDL-in-one-call form. Pre-create user tables in MAIN first. See §4.R1. |
| `use_branch` | `use_branch(branch_name text)` + 2-param `use_branch(branch_name text, isolation_mode text DEFAULT 'read committed')` | 2-param added V3 T1, `isolation_mode` ∈ `'read committed'` / `'snapshot'`. |
| `apply_branch` | `apply_branch(branch_name text) RETURNS void` | V3 T2 FR2: takes xact-scoped EXCLUSIVE advisory lock on `0x4F42<<48 | bid`. |
| `discard_branch` | `discard_branch(branch_name text) RETURNS void` | V3 T2 FR2: takes xact-scoped EXCLUSIVE advisory lock on same key. |
| `is_active` / `force_invalidation_check` | from V3 T2 FR1 NOTIFY + FR4 throttle layer | See doc. |

---

## 2. Setup / build commands (agent MUST use these)

Working directory: **always** `cd /home/ubuntu/work/postgres/contrib/overlay_branch`
before any build or test command.

```bash
# --- Clean rebuild (force zero stale artifacts) ---
rm -rf src/*.o src/*.o.d overlay_branch.so \
       test_output tmp_check ../../tmp_install \
       /tmp/pg17-writable/lib/postgresql/overlay_branch.so

# --- Compile + install into writable prefix ---
make -j4 install prefix=/tmp/pg17-writable
#   Confirm: 0 lines of "warning:" or "error:" in gcc output
#   BEFORE accepting any change, grep for warnings/errors and fix all.

# --- Run L3 pg_regress suite (4 tests: basic/user/advanced/mvcc) ---
make check prefix=/tmp/pg17-writable
#   Accept verdict only when tail shows:
#       ok 1 overlay_branch_basic
#       ok 2 overlay_branch_user
#       ok 3 overlay_branch_advanced
#       ok 4 overlay_branch_mvcc
#       1..4
#       # All 4 tests passed.
#
#   If mvcc.out differs because you ADDED a new Section (not broke anything):
#       cp test_output/results/overlay_branch_mvcc.out test/regress/expected/
#   then re-run `make check` a SECOND clean time to confirm 0 diff.
```

**Zero-warning policy**: `gcc -Wall` must yield **0 warnings + 0 errors**.
Any `-Wdeclaration-after-statement` warning is a hard fail (we enforce C90
top-declaration style — see §4.R3).

---

## 3. Testing instructions (3-layer concurrency architecture — agent, READ THIS)

This project intentionally splits concurrency testing across THREE tools.
Agents MUST NOT try to force cross-session races into the L3 pg_regress suite
— that suite is **single-session, fully deterministic sanity only**.

| Layer | Tool | What it tests | When to use |
|-------|------|---------------|-------------|
| **L3** | `make check` (pg_regress) | Single-session sequential behavior, API signature sanity, GUC resets, S→X lock upgrade same-backend, magic-key bitcast parity C↔SQL. | **Every T delivery — GATING CHECKLIST ITEM**. 4/4 PASS 0 diff required. |
| **L1** | `pg_isolation_regress` (`test/isolation/specs/*.spec`) | Exact step-by-step scheduled cross-session waits: s1 apply blocks s2 apply, s1 use+write vs s2 apply deadlock-order, apply↔discard race-free states. 5 specs × 9 permutations green as of T8. | **T8 delivered** — FR2/FR3 concurrency guarantees, FR4 NOTIFY+SPI fallback, FR5 snapshot mode. Use this layer when regression-testing cross-session behavior changes. |
| **L2** | `pgbench` custom scripts (`test/bench/apply_contention.sql`) | 50-thread 60-second stress mix (30% apply / 50% use_write / 20% discard) to surface deadlock counts, MAIN pk-uniqueness violations, state↔delta inconsistency. | **T8 delivered** — NFR acceptance (no data corruption). Run manually per `test/bench/README.md`. |

**Baseline management rule (L3)**:
1. First `make check` run → expect 1 FAIL when you add SQL content (expected
   file missing the new lines).
2. `cat test_output/regression.diffs` — confirm **ONLY `+` lines** appear
   (no `-` lines = no regression on existing sections), no `ERROR:`, no
   `syntax error`.
3. `cp test_output/results/overlay_branch_mvcc.out test/regress/expected/`
4. **Clean rebuild second time** (`rm -rf ...test_output...` above) →
   confirm **4/4 0 diff** before marking that T "verified".

---

## 4. Critical NON-NEGOTIABLE rules (break these = silent data bugs)

Memorize or re-read this section BEFORE editing C code or SQL tests.
Every item here was learned the hard way with a compile error or silently
wrong test baseline.

### R1. `create_branch` takes exactly ONE argument.
- **Never** write `SELECT create_branch('name', 'CREATE TABLE ...')`.
- Correct pattern (L3 tests everywhere):
  ```sql
  CREATE TABLE IF NOT EXISTS t_xxx (pk int PRIMARY KEY, ...);  -- MAIN DDL first
  SELECT create_branch('b_xxx') >= 1 AS cr_ok;                -- single-arg call
  ```
- Why: The extension only ships `CREATE FUNCTION create_branch(text)`.
  The dual-arg form has NEVER existed.  Any `\df create_branch` in psql
  will confirm `Argument data types: text`.  Got this wrong → Section L
  v1 entire block `ERROR: function create_branch(unknown, unknown) does
  not exist`.  Don't repeat the 85 wasted lines.

### R2. pg_regress SQL runner ≠ psql interactive.
- **NEVER use** `\gset`, `\set`, `:variable_name`, `\gexec`, `\if` inside
  `test/regress/sql/*.sql` files.  The pg_regress driver feeds the file straight
  into the backend SPI executor — it does NOT pass through the psql
  frontend lexer.  Result: `syntax error at or near ":"`.
- Correct way to carry a `branch_id` across statements: use an **inline
  correlated subquery** or CTE every time.  Section L L4 canonical form:
  ```sql
  SELECT pg_advisory_xact_lock_shared(
    (0x4F42::bigint << 48) |
    (SELECT branch_id FROM overlay_branch.pg_branch
      WHERE branch_name = 'b_fr2_l4')::bigint
  ) IS NULL AS l4_sql_shared_reentry_ok;
  ```

### R3. C code: strict C90 top-declaration.
- `gcc -Wall` **includes `-Wdeclaration-after-statement`**.  Mixing
  declarations and statements inside a block (e.g. `int x = 5; printf;`)
  breaks the build.
- Rule: Every local variable (`int64 key; LOCKTAG tag;`) must be declared
  at the TOP of its enclosing `{ }` block, BEFORE the first executable
  line.  The two `static inline` helpers in
  [overlay_branch.h](include/overlay_branch.h) show the idiom.

### R4. Deadlock prevention: Advisory Lock BEFORE RowExclusiveLock; FR2 V1 removed SHARED (use=zero lock).

**Lock order invariant (V3 FR2 V1 onwards)**:
`(zero lock on use) → ONLY on CAS-winner: Exclusive advisory → user-table RowExclusive → catalog RowExclusive (auto)`

- **SHARED advisory lock was REMOVED in FR2 V1** (2026-09-19 V3 T8).
  The original SHARED→EXCLUSIVE upgrade path caused classic 40P01 deadlock
  (two sessions both SHARED, both try upgrade → no one can proceed).
  FR2 V1 proof: lock graph has NO shared→exclusive upgrade edges, so
  wait-for graph cannot contain a cycle → **zero deadlock possibility**
  (L1 ob_apply_mutex 9 permutations all pass without 360s deadlock timeout).
- `use_branch` = **NO ADVISORY LOCK AT ALL** (only reads state via GUC /
  session-local mode cache HTAB). Zero write to shared state.
- `apply_branch` / `discard_branch`: **CAS gate FIRST**
  (`UPDATE pg_branch SET state='applying'/'discarding' WHERE state='active'`),
  PG UPDATE row lock = CAS semantics, row-count=0 → ERROR kick out, NO
  advisory lock taken on CAS-loser. CAS winner THEN takes Exclusive advisory.

**Exact FR2 V1 lock take-points (CAS winner only — do NOT change unless you update
doc/multi_session_mvcc.md §I8.1 and this file together)**
- [branch_lifecycle.c:1268](src/branch_lifecycle.c#L1268)
  apply_internal CAS winner → Exclusive advisory (BEFORE MAIN merge opens
  each relation RowExclusiveLock for D-U-I 3-pass; BEFORE
  `UPDATE pg_branch SET state='applied'` catalog RowExclusiveLock).
  Call site = `ob_take_branch_advisory_lock(bid, ExclusiveLock)`.
- [branch_lifecycle.c:1648](src/branch_lifecycle.c#L1648)
  discard_internal CAS winner → Exclusive advisory (BEFORE
  `DELETE pg_branch_delta` + `UPDATE pg_branch SET state='discarded'`
  catalog RowExclusiveLock). Same call pattern.

**Invariant on `allow_kickout` (CmdType split, FAIL#1 root cause)**:
`ob_throttled_allow_kickout` MUST be raised ONLY for CmdType in
{INSERT,UPDATE,DELETE,MERGE} (dml_split=true), and lowered immediately
after throttle check returns. Pure SELECT/DQL MUST be observer-only: never
exit_branch_cleanup() inside throttled() (else next real DML silently
writes MAIN with NOTHING ≥ WARNING — see multi_session_mvcc.md §I8.3).

PG Advisory internals cheat sheet:
- Key layout 100% equal to `pg_advisory_xact_lock(int8)` SQL:
  `int64 key = (0x4F42ULL<<48) | (uint32)bid`.
- `sessionLock=false` → **xact-scoped auto-release on COMMIT/ABORT**
  (never leaks, no per-backend HT cleanup needed).
- `dontWait=false` → block until granted; CAS winner takes Exclusive,
  blocks CAS winner of the other branch on same bid.

### R5. Source-head guard flags (3-level depth system — DO NOT disturb).
The invalidation + planner hooks use three static `bool` counters to
prevent recursive re-entry (if you break the counter pair on entry/exit,
you infinite-loop the backend or SIGSEGV inside ereport).

| Guard | Owner file | Purpose |
|-------|-----------|---------|
| `ob_in_invalidation_check` (depth) | `src/invalidation.c` / planner | Throttle FR4 kickout re-entry; MUST be 0 unless actively inside a throttle check. |
| `ob_in_planner_hook` (depth) | src planner hook wrapper | |
| `ob_throttled_allow_kickout` (opt-in only) | src invalidation subscriber | Destructive side-effects only go through the explicit opt-in gate.  Do NOT widen this to all callers. |
- When adding new call sites: balance `++guard` / `--guard` in the same
  function (PG_TRY / PG_FINALLY if needed).

### R6. Flat extension version 1.0 — no upgrade scripts.
- Control file: `default_version = '1.0'`.
- Makefile `DATA = overlay_branch--1.0.sql` only.
- **Never create** `overlay_branch--1.0--1.1.sql` / `--1.1--1.2.sql` again.
  User confirmed the extension is "unpublished; just mutate 1.0 in place".
  Past upgrade scripts were deleted and won't come back.
- New functions, types, casts → **append `CREATE OR REPLACE FUNCTION /
  ALTER TYPE ... ADD ATTRIBUTE` DIRECTLY to the bottom of
  `overlay_branch--1.0.sql`**.

### R7. Delivery flow = "Design doc FIRST → code → L3 test → green baseline".
User standing order (9/14): *"关键实现设计先落 doc 再编码"* + *"先保证
正确和可测试，性能问题先记录到 doc，不急于优化"*.
1. Append relevant design section to `doc/multi_session_mvcc.md` (T→§ D.x).
2. Write code (`include/*.h` then `src/*.c` then `overlay_branch--1.0.sql`).
3. Write L3 Section (A…Z sequential) in the matching SQL test file.
4. 2x clean rebuild `make check` → **4/4 0 diff** → mark verified.
5. If you changed FR2 include/entry points, sync § D.7 AND this file's
   § R4 take-point links BEFORE committing.

---

## 5. Documentation jump table (no duplication — click the links)

Everything detailed lives in `doc/`.  This section is a card catalogue so
an agent can jump directly to the relevant design appendix BEFORE starting
work on a task.

### Master task tracker
- **[doc/progress_tracker.md](doc/progress_tracker.md)** — Task table
  (T1…T8), per-Task detailed blocks with **Test Rules (TR-x.y)** checkboxes
  and the "doc sync confirmation" user reminder anchor.  Agents: always
  start a new T by reading the T's progress_tracker section first.

### Design / implementation appendices (V3 architecture)
- **[doc/multi_session_mvcc.md](doc/multi_session_mvcc.md)** — V3 single
  source of truth for all multi-session MVCC design + C-level
  implementation notes:

| Task / Feature | Exact section in multi_session_mvcc.md |
|---|---|
| T1: 2-param `use_branch(name, mode)` + SNAPSHOT infrastructure | § B Implementation Appendix T1 |
| T2: FR1 NOTIFY (Publisher/Subscriber) + FR4 throttle DQL/DML split | § C (high-level) + § D.1–D.6 (appendices) + §I8.6 NOTIFY+SPI fallback |
| T3/V3 FR2 V1 Branch-level Advisory Lock (CAS-first + zero lock on use, deadlock-free) | **§I8.1 FR2 锁协议降级 + §B.10 硬约束** |
| T4: FR3 applying transient state + delta-write guard | §I8.4 `_debug_apply_sleep_sec` freeze window |
| T5: FR5 SNAPSHOT Mode MAIN Freeze (scope contract MAIN vs Delta) | **§I8.5 Snapshot freeze 范围契约** |
| T6: BranchScan ReScan fresh materialization | doc/branchscan.md + L1 ob_branchscan_rescan.spec |
| T7: Shared-memory Pin Table **CANCELLED-by-design** | §B.8 FR7 Cancel Rationale |
| T8: 5 L1 specs + 9/9 zero-diff total acceptance | §B.9 L1/L2/L3 三层测试架构 + §B.9.1 I8.1-I8.6 关键设计变更 |

### API / non-concurrency docs
- **[doc/design.md](doc/design.md)** — Top-level architecture (components,
  catalog tables `pg_branch` / `pg_branch_delta`, snapshot install
  lifecycle).
- **[doc/example_sql.md](doc/example_sql.md)** — Copy-pasteable user
  scenarios (create/use/apply/discard single session, no internal test
  noise).
- **[doc/branchscan.md](doc/branchscan.md)** — `BranchScan` node
  internals, materialization, WR interaction.  Relevant for T6 (AC-C1
  ReScan fresh materialization).

---

## 6. Task delivery status (T1 → T8 ALL COMPLETE)

**All V3 tasks have been delivered and verified with 9/9 zero-diff green
(L3 4/4 + L1 5/5, 2×+ stale clean rebuild confirmed 2026-09-19).**

Each item below links to its `progress_tracker.md` acceptance block and
its `multi_session_mvcc.md` design appendix for future reference / bug fixes.

1. **T1 ✅ 2-param `use_branch(name, mode)` + SNAPSHOT infrastructure**
   → [progress_tracker §Task 1](doc/progress_tracker.md). API addition.

2. **T2 ✅ FR1 NOTIFY (Publisher/Subscriber) + FR4 throttle DQL/DML split**
   → [progress_tracker §Task 2](doc/progress_tracker.md) + §I8.6 NOTIFY+SPI fallback.

3. **T3 ✅ V3 FR2 V1 Branch Advisory Lock: CAS-first + use=zero lock**
   → [progress_tracker §Task 3](doc/progress_tracker.md) + §I8.1 FR2 锁协议降级;
   L1 ob_apply_mutex 9 permutations deadlock-free.

4. **T4 ✅ FR3 `applying` transient state + delta-write guard**
   → [progress_tracker §Task 4](doc/progress_tracker.md) + §I8.4 freeze window harness.
   State machine: `active → applying → applied/discarded`; delta insertions
   ERROR 55000 if state ≠ active.

5. **T5 ✅ FR5 SNAPSHOT Mode MAIN Freeze (MAIN frozen / Delta always-latest)**
   → [progress_tracker §Task 5](doc/progress_tracker.md) + **§I8.5 Snapshot freeze 范围契约**.
   L1 ob_snapshot_mode.spec PASS: C1/C2=102, C3=103, C4=104.

6. **T6 ✅ AC-C1 BranchScan ReScan fresh materialization**
   → `doc/branchscan.md` Appendix A + L1 `ob_branchscan_rescan.spec`.
   Clear `result_slots` + `materialized=false` on ReScan; cross-session
   S1 read1=3 → S2 INSERT id=4 → S1 new plan fresh count=4.

7. **T7 ✅ (CANCELLED-by-design) Shared-memory Pin Table**
   → **multi_session_mvcc.md §B.8** for full rationale: PG isolationtester
   cannot inject backend crash; NOTIFY/SPI counter fallback sufficient.
   Two legacy GUCs retained as no-op (deprecation NOTICE only).

8. **T8 ✅ Final acceptance: 5 L1 specs + L2 pgbench fixture**
   → [progress_tracker §Task 8](doc/progress_tracker.md) + §B.9 三层架构.
   Baselines: L3 4 tests (basic/user/advanced/mvcc .out in test/regress/expected/)
   + L1 5 tests (.spec + .out in test/isolation/{specs,expected}/).
   FAIL#1 CmdType split drift write BUG: root cause + fix in §I8.3.

---

## 7. Commit / agent handoff hygiene

When you finish a T (or a sub-T batch):

1. Confirm `make check` 4/4 0 diff on a **clean second rebuild**.
2. Confirm `gcc` 0 warnings 0 errors.
3. If you touched anything in § R4 / § D.7 (locks), confirm this file's
   take-point line ranges still match (browse the actual file, don't
   guess).  Out-of-date line numbers in AGENTS.md are worse than no line
   numbers because the next agent will jump to the wrong place.
4. Update `doc/progress_tracker.md`: flip status ⬜→✅, append TR-x.y
   checkbox verdicts with evidence.
5. If you added a new Section A–Z to `overlay_branch_mvcc.sql` (or any
   SQL file), **never leave `regression.diffs` un-copied** — the next
   `make check` by another agent will fail and they won't know if it's a
   real regression or an unmerged baseline.
