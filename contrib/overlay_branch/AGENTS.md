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

**Current milestone (2026-09-28)**: V3 T1–T8 + B1–B5 = ALL DELIVERED
(L3 11/11 + L1 8/8 = 19/19 **Green-Bar Acceptance** per §3, 2× clean rebuild green).
See `doc/progress_tracker.md` + `doc/review/poc_fix_tracker.md` (分类一 14/14 + R21 ✅),
and `doc/multi_session_mvcc.md` for full FR architecture.

**Public API signatures (agent: copy-paste these, never guess)**:

| Callable | Signature | Notes |
|----------|-----------|-------|
| `create_branch` | `create_branch(branch_name text) RETURNS integer` | **SINGLE arg ONLY** — no DDL-in-one-call form. Pre-create user tables in MAIN first. See §4.R1. |
| `use_branch` | `use_branch(branch_name text)` + 2-param `use_branch(branch_name text, isolation_mode text DEFAULT 'read committed')` | 2-param added V3 T1, `isolation_mode` ∈ `'read committed'` / `'snapshot'`. Pass `NULL` to exit branch mode (unified cleanup path). |
| `apply_branch` | `apply_branch(branch_name text) RETURNS void` | V3 T2 FR2: takes xact-scoped EXCLUSIVE advisory lock on `0x4F42<<48 | bid`. **CAS gate first**: `active→applying` (R17 freeze). |
| `discard_branch` | `discard_branch(branch_name text) RETURNS void` | V3 T2 FR2: takes xact-scoped EXCLUSIVE advisory lock on same key. |
| `is_active` / `force_invalidation_check` | from V3 T2 FR1 NOTIFY + FR4 throttle layer | See doc. |
| `overlay_main_plus_delta` | `overlay_main_plus_delta(tbl regclass) RETURNS SETOF record` | Debug view: LIVE overlay of MAIN + branch delta (latest-wins). Used in L3 count assertions. |
| `diff_branch` | **Removed / never existed.** DO NOT USE. Use `overlay_main_plus_delta` instead. | |

---

## 2. Setup / build commands (agent MUST use these)

Working directory: **always** `cd /home/ubuntu/work/postgres/contrib/overlay_branch`
before any build or test command.

```bash
# --- Clean rebuild (force zero stale artifacts) ---
rm -rf src/*.o src/*.o.d overlay_branch.so \
       test_output tmp_check output_iso tmp_check_iso ../../tmp_install \
       /home/ubuntu/pg17/lib/postgresql/overlay_branch.so

# --- Compile + install into writable prefix ---
make -j4 install prefix=/home/ubuntu/pg17
#   Confirm: 0 lines of "warning:" or "error:" in gcc output
#   BEFORE accepting any change, grep for warnings/errors and fix all.

# --- Run L3 pg_regress suite (11 tests: basic/user/scan_pk/scan_type/puredelta_mix/upsert/puredelta/mvcc_usage/mvcc_bounds/mvcc_review/rentry) ---
make check prefix=/home/ubuntu/pg17
#   Accept verdict only when tail shows:
#       ok 1 overlay_branch_basic
#       ok 2 overlay_branch_user
#       ok 3 overlay_branch_scan_pk
#       ok 4 overlay_branch_scan_type
#       ok 5 overlay_branch_puredelta_mix
#       ok 6 overlay_branch_upsert
#       ok 7 overlay_branch_puredelta
#       ok 8 overlay_branch_mvcc_usage
#       ok 9 overlay_branch_mvcc_bounds
#       ok 10 overlay_branch_mvcc_review
#       ok 11 overlay_branch_rentry
#       1..11
#       # All 11 tests passed.
#
#   If any .out differs because you ADDED a new Section (not broke anything):
#       cp test_output/results/<FILE>.out test/regress/expected/
#   (e.g. `cp test_output/results/overlay_branch_rentry.out test/regress/expected/`)
#   Repeat for ALL files that show + only lines, then re-run clean second time.
#
# --- Run L1 pg_isolation_regress suite (8 specs) ---
make -C ../../src/test/isolation \
  pg_isolation_regress \
  EXTRA_REGRESS_OPTS="--inputdir=$(pwd)/test/isolation --temp-config=$(pwd)/temp_instance_shared_libs.conf" \
  REGRESS_SHLIB=$(pwd)/../../src/test/isolation/pg_isolation_regress.so \
  SPECS="ob_apply_mutex ob_state_inval ob_applying_freeze ob_snapshot_mode ob_branchscan_rescan pure_delta_upsert pure_delta_update pure_delta_delete"
#   Expected output dir: `output_iso/results/` → cp to `test/isolation/expected/`
#   Accept: 8/8 PASS 0 diff.
```

**Zero-warning policy**: `gcc -Wall` must yield **0 warnings + 0 errors**.
Any `-Wdeclaration-after-statement` warning is a hard fail (we enforce C90
top-declaration style — see §4.R3).

---

## 3. Testing instructions (3-layer concurrency architecture — agent, READ THIS)

This project intentionally splits concurrency testing across THREE tools.
Agents MUST NOT try to force cross-session races into the L3 pg_regress suite
— that suite is **single-session, fully deterministic sanity only**.

### Green-Bar Acceptance (绿标验收 — define once, reference everywhere)
Before marking **any** T-task, B-milestone, or bug-fix as DELIVERED, the agent
MUST confirm **ALL** of:
1. `gcc -Wall` compile/install → 0 warnings, 0 errors (grep the build log;
   `-Wdeclaration-after-statement` is a hard fail, see §4.R3).
2. **TWO independent full clean rebuilds** (§2 `rm -rf` + `make install` each time —
   a single build can mask stale `.so` copies or cached plans).
3. After **rebuild #2 only** (so drift between baseline and current is fully surfaced):
   - L3: `make check` → **11/11 PASS**.
   - L1: `pg_isolation_regress` 8-spec run → **8/8 PASS**.
   - `diff -r test_output/results test/regress/expected` → **0 lines**.
   - `diff -r output_iso/results   test/isolation/expected` → **0 lines**.
The phrase **"2× clean rebuild green"** in any other section of this file is a
shorthand reference to the full checklist above.

| Layer | Tool | What it tests | When to use |
|-------|------|---------------|-------------|
| **L3** | `make check` (pg_regress) | Single-session sequential behavior, API signature sanity, GUC resets, WR pre-check (R21), entry-choke (A3/A5), magic-key bitcast parity C↔SQL. 11 files: basic/user/scan_pk/scan_type/puredelta_mix/upsert/puredelta/mvcc_usage/mvcc_bounds/mvcc_review/rentry. | **Every T/B delivery — GATING.** 11/11 PASS 0 diff (part of Green-Bar Acceptance above). |
| **L1** | `pg_isolation_regress` (`test/isolation/specs/*.spec`) | Exact step-by-step scheduled cross-session waits: CAS applying freeze, pure-delta update/delete concurrency, branchscan rescan, snapshot mode, apply mutex. **8 specs**: `ob_apply_mutex`, `ob_state_inval`, `ob_applying_freeze`, `ob_snapshot_mode`, `ob_branchscan_rescan`, `pure_delta_upsert`, `pure_delta_update`, `pure_delta_delete`. | **Required for any cross-session correctness change** (R17 CAS freeze, Pure-Delta, A3/A5). 8/8 0 diff (part of Green-Bar Acceptance). |
| **L2** | `pgbench` custom scripts (`test/bench/apply_contention.sql`) | 50-thread 60-second stress mix (30% apply / 50% use_write / 20% discard) to surface deadlock counts, MAIN pk-uniqueness violations, state↔delta inconsistency. **EXCEPTION catch** inside `pgbench` DO block so 55000/0A000 kicks don't abort the whole run. | **NFR acceptance** (no data corruption). Run manually per `test/bench/README.md`; not part of default Green-Bar unless the change is explicitly concurrency-tuning. |

### Baseline management
**L3**:
1. First `make check` run → expect FAIL when you add SQL content (expected file missing the new lines).
2. `cat test_output/regression.diffs` — confirm **ONLY `+` lines** appear (no `-` lines = no regression on existing assertions), no `ERROR:`, no `syntax error`.
3. **For EACH file with diffs**: `cp test_output/results/<FILE>.out test/regress/expected/`
   (Files: basic / user / scan_pk / scan_type / puredelta_mix / upsert / puredelta / mvcc_usage / mvcc_bounds / mvcc_review / rentry — copy ALL that changed.)
**L1**: After isolation run, `cp output_iso/results/<SPEC>.out test/isolation/expected/` for each modified spec.
**Then proceed to Green-Bar rebuild #2** — the acceptance check is ONLY performed on rebuild #2 (after baseline copy).

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

**Exact FR2 V1 lock take-points (CAS winner only — update doc/multi_session_mvcc.md §I8.1 and this file together on change). Use function names as anchors, NOT line numbers — line numbers drift; function names don't.**
- **`apply_internal()` in `src/branch_lifecycle.c`**: After `UPDATE pg_branch SET state='applying' WHERE state='active'` CAS-winner check passes (rc=1), call `ob_take_branch_advisory_lock(bid, ExclusiveLock)` **BEFORE** opening user relations for D-U-I 3-pass MAIN merge and **BEFORE** the final `UPDATE pg_branch SET state='applied'` catalog write.
- **`discard_internal()` in `src/branch_lifecycle.c`**: After CAS-winner `state='discarding'` update rc=1, same `ob_take_branch_advisory_lock(bid, ExclusiveLock)` **BEFORE** `DELETE pg_branch_delta` bulk clear and final `UPDATE pg_branch SET state='discarded'` catalog write.

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
4. 2x clean rebuild `make check` + isolation → **L3 11/11 + L1 8/8 0 diff** → mark verified.
5. If you changed FR2 include/entry points, sync § D.7 AND this file's
   § R4 take-point links BEFORE committing.

---

### R8. MVP scope limit: ONLY plain heap tables with SINGLE-COLUMN PRIMARY KEY.
- **DO NOT support**: partitioned tables, foreign tables, views, materialized views,
  tables with foreign keys, tables with multi-column PKs, tables with UNIQUE constraints
  other than PK.
- Trigger gate: these must ERROR 55000 (object_not_in_prerequisite_state) or
  0A000 (feature_not_supported) at `create_branch` time latest — never let the
  executor see them or you get silent write-through MAIN.
- DML drift: if user somehow gets a non-PK table into the system, the WR entry
  choke (A3/A5 R10) must ERROR before any heap_write.

### R9. CMD_UPDATE ProjInfo deep-copy: SKIP junk columns (attnum ≤ 0).
- In the CMD_UPDATE fallback (pure-delta rows), when walking `targetList` to
  clone `ProjInfo->pi_targetlist` into the new slot, **skip any entry where
  `resno ≤ 0`** (junk columns: `ctid`, `tableoid`, etc. — these are
  pass-by-reference pointers into the parent plan that become invalid after
  ExecEndNode → SIGSEGV on dereference).
- Canonical code: `src/write_redirect.c` CMD_UPDATE branch — grep `attnum > 0`.

### R10. A3/A5 WR entry PANIC choke (non-negotiable write-through protection).
- `overlay_executor_run_intercept` **MUST** contain a PANIC-level (ERROR 55000)
  guard at the VERY TOP, immediately after the `hasModifyingCTE` / `COPY FROM`
  / `MERGE` MVP safety checks (R11).
- Rule: if the DML targets a managed table (registered in the branch's overlay
  set) and we reach `standard_ExecutorRun` WITHOUT having redirected through
  `overlay_write_redirect_*`, **ERROR 55000 IMMEDIATELY**. Never silently let
  a managed write touch MAIN heap.
- Test: Section R-A3A5 in `overlay_branch_rentry.sql`.

### R11. MVP safety intercepts at WR TOP (R05/R06/R16).
Before ANY redirect logic, at the top of `overlay_executor_run_intercept`:
1. **R05 — COPY FROM**: If query source = CopyFrom → ERROR 0A000 "COPY FROM on
   overlay-managed tables is not supported in MVP".
2. **R06 — MERGE**: If CmdType = CMD_MERGE → ERROR 0A000 "MERGE on overlay-managed
   tables is not supported in MVP".
3. **R16 — Modifying CTE**: If PlannedStmt->hasModifyingCTE is true AND the
   inner DML targets a managed table → ERROR 0A000 "CTEs containing DML on
   overlay-managed tables are not supported in MVP".
- Order matters: these go BEFORE any relid lookup or WR counter bump.

### R12. R03 — UPDATE MUST NOT modify the PK column (MAIN path AND pure-delta path).
- In CMD_UPDATE, compare `targetList` entries against `pk_attno`. If ANY SET
  target matches the PK attnum → ERROR 0A000 "Updating the PRIMARY KEY column
  of an overlay-managed table is not supported".
- **Both paths must check**: MAIN-origin UPDATE (PlanSlot with ctid junk) AND
  pure-origin UPDATE (delta Slot, ctid = NULL). Don't let the main-path check
  skip the pure path.
- Test: Section R-API (mvcc_review / rentry).

### R13. R19 — SNAPSHOT mode TTL timeout enforced.
- Snapshot mode is GUC-gated with a TTL. If the session has been in snapshot
  mode longer than `overlay_branch.snapshot_ttl_sec` → next WR-invoked DML
  or throttle check EXITS branch mode forcibly (return to MAIN read-committed
  view of life). Do NOT let snapshotters accumulate MVCC horizon pinning.

### R14. Typed PK comparison — NEVER compare PKs as bare text strings.
- **All** BranchScan comparator / delta-store PK lookup / qsort-bsearch key
  equality MUST use the `TypedKey` structure:
  ```c
  typedef struct TypedKey {
    Datum     datum;      /* real datum, NOT text-cast */
    bool      isnull;
    Oid       typid;      /* from TupleDesc->attrs[pk_idx].atttypid */
    int32     typmod;
    Oid       collid;
    FmgrInfo  *lt_op;     /* from get_sort_group_operators(typid, ...) */
    FmgrInfo  *eq_op;
  } TypedKey;
  ```
- **What R14 prevents**: BPCHAR right-trim drift (PG coerces `char(N)` to
  `text` via rtrim → key collision `'a '` = `'a'`); NUMERIC precision loss
  (text `'1.0'` vs `'1.00'` hash differently); TIMESTAMPTZ timezone skew.
- **Never use SPI inside comparator callbacks** (e.g. qsort comparator). SPI
  allocates in `SPI_proc` context which is freed when SPI_finish returns —
  you'll get `pfree: invalid pointer` PANIC on the next comparison. Anchor
  all TypedKey memory in `TopMemoryContext` or the BranchScan node's
  per-query ExprContext.
- Comparator OIDs: obtain via `get_sort_group_operators(typid, false, false,
  &ltOid, &eqOid, &gtOid, NULL)` ONCE per scan and cache.

### R15. TTSOpsVirtual access: `slot_getsomeattrs` BEFORE `slot_getattr`.
- On virtual tuple slots (TTSOpsVirtual), `tts_nvalid` is NOT a promise that
  `tts_values/tts_isnull` are populated — it's a "valid up to N" marker on
  the assumption the node above will `slot_getattr` serially.
- **Rule**: Before ANY raw `tts_values[i]` / `tts_isnull[i]` access on a
  slot you didn't heap_deform yourself, call `slot_getsomeattrs(slot, N)`
  with N ≥ the highest attno you'll touch. This forces the missing-bitmap
  walk to actually materialize the Datums.
- Correct pattern (NOT "bump `tts_nvalid` as a flag" — that reads uninitialized
  stack memory into `tts_isnull[]` → false 23502 positives / missed UNIQUE).
  - NOT NULL whole-tuple check: `slot_getsomeattrs(slot, natts)`
  - Single-column PK extraction: `slot_getsomeattrs(slot, pk_attidx + 1)`

### R16. R07 — PlanCache invalidation on branch switch = 2-hit combo.
When `use_branch(x)` CHANGES the active branch (including exiting via NULL):
1. Call `CacheInvalidateRelcacheAll()` to blow away all relcache entries.
2. Immediately call `CommandEndInvalidationMessages()` to DIGEST the SI
   message queue on the CURRENT backend — just posting invalidation messages
   is NOT sufficient for PreparedStatements / plancache entries already
   pinned in the current command lifetime.
- If you skip step 2 → next SELECT replans using the OLD plan (no BranchScan
  node → silent MAIN-only reads, data drift ≥ data loss). This is NOT
  detectable by `make check` unless you test multi-statement transactions.

### R17. R17 — CAS state gate on apply: `active → applying` FREEZES delta writes.
- `apply_branch()` STEP ZERO (before any advisory lock, before opening any
  user relation):
  ```sql
  UPDATE overlay_branch.pg_branch
     SET state = 'applying'
   WHERE branch_name = $1 AND state = 'active';
  ```
  This is a CAS (compare-and-swap) using PG's MVCC row lock. If row-count
  = 0 → ERROR 55000 "branch is not active".
- Mirror rule on `overlay_delta_insert()` (write path): **re-check** that
  `pg_branch.state = 'active'` using a fresh SPI SELECT right before the
  INSERT into pg_branch_delta, after opening the SPI manager but before
  heap_insert. If state is `applying`/`applied`/`discarded` → ERROR 55000.
  This is the actual freeze enforcement — concurrent writers that read
  `state='active'` from cache just before `apply_branch` does the UPDATE
  will fail here.
- L1 test: `ob_applying_freeze.spec`.

### R18. R15/R18 — Apply base-image 2-step verification to prevent silent MAIN overwrite.
When applying a delta row (D/U/I pass):
1. Fetch the MAIN row by `_base_ctid` (from delta's metadata) using
   `heap_fetch(_base_ctid, SnapshotAny)` → get raw xmin.
2. ASSERT that `xmin == _base_xmin` from delta JSON metadata.
3. If mismatch (HOT updated / concurrent UPDATE changed the ctid/xmin pair):
   **ERROR 55000 with explicit detail** naming the PK value, old vs new
   xmin/ctid.
- Never trust _base_ctid alone (HOT chains re-link tuples; UPDATE in a
  concurrent session can create a new tuple at same t_block/offnum with a
  different xmin). The _base_xmin cross-check is REQUIRED.
- Pure-origin delta rows (ctid=NULL, xmin=NULL) skip this check by design.

### R19. Pure-Delta UPDATE/DELETE 8-phase loop: no WHERE + op=I/U must NOP.
The pure-delta main loop (rows with no MAIN origin) must handle:
- Op codes: I (insert-tombstone) / U (update) / D (delete) in latest-wins
  order from pg_branch_delta (bid, relid, key) UPSERT key.
- **Fold rule**: If current delta op=I AND MAIN already has this key → op
  folds to UPDATE; if op=U and MAIN has no key → op folds to INSERT.
- **NOP rule**: An 8-phase DML that has `WHERE FALSE` OR targets a key whose
  latest delta op is I/U and the user's DML doesn't produce a net change
  MUST leave the delta row unchanged (no spurious UPDATE pg_branch_delta rows).
- If violated: `UPDATE t SET col = col WHERE id = 3` on a pure-origin row
  produces a duplicate-key error on re-apply (apply pass double-inserts into MAIN-side UPSERT collision).

### R20. R20 — SET NULL semantic parity between MAIN path and pure-delta path.
When user executes `UPDATE t SET col = NULL WHERE ...`:
- Both paths must produce a delta tuple whose JSON representation stores
  the column as JSON `null` (NOT omitted / absent). Omission would be
  interpreted as "column not touched" by the apply pass, leaving the
  previous value.
- L3 test: Section R-API SET NULL cases.

### R21. R21 MVP — INSERT (ONCONFLICT_NONE only) write-pre CHECK (NOT NULL + PK UNIQUE, MAIN/delta 2-sided).
**Scope (strict MVP — DO NOT widen)**:
- Only `ONCONFLICT_NONE` plain INSERTs (not UPSERT; UPSERT already has
  Phase I/II MAIN-first 2-phase conflict detection in promo table).
- Constraints checked:
  (a) NOT NULL on all user non-dropped columns with `attnotnull=true`;
  (b) SINGLE-column PRIMARY KEY UNIQUE on MAIN heap + branch delta BOTH sides.
- Other constraints (CHECK, FK, multi-col PK, non-PK UNIQUE): **continue to
  rely on apply_branch() native PG check** — don't re-invent in MVP.

**Semantic rules for UNIQUE (delta-latest-wins overlay)**:
pg_branch_delta PK = `(branch_id, relid, key_text)` guarantees exactly 1 row
per key with latest `op ∈ {I,U,D}`. R21 UNIQUE MUST check **latest op**
(NOT row count):
1. SPI SELECT `op FROM pg_branch_delta WHERE bid=? AND relid=? AND key=?`
   → (no row) → continue to step 2; `op ∈ {I,U}` → 23505 immediately;
   `op='D'` → **RETURN SUCCESS (rebirth: tombstone covers MAIN, this
   INSERT legally resurrects the key)**.
2. (delta absent/ tombstone): SPI SELECT `ctid FROM MAIN_table WHERE pk = ?`
   (using PK index lookup, not seqscan). Row found → 23505; not found → OK.
- **Order critical**: delta FIRST (determines whether MAIN check is skipped).
  MAIN-first ordering breaks rebirth semantics (DELETE→INSERT on same PK
  is a valid user pattern — tombstone covers MAIN).

**Implementation constraints (PANIC if wrong)**:
- Entry point: `write_redirect.c` ONCONFLICT_NONE branch — call helper
  `r21_check_insert_preconditions()` RIGHT BEFORE `promotion = 1`.
- **TTS safety**: NOT NULL loop uses `slot_getsomeattrs(slot, natts)`; PK
  extraction uses `slot_getsomeattrs(slot, pk_attidx+1)` (see R15).
- **WR counter balance on ereport(ERROR)**: the helper opens SPI with
  `overlay_overlay_helper_enter() + overlay_write_redirect_exit()` (WR OFF
  during helper SPI queries so we don't recursively redirect). Before
  **ANY** `ereport(ERROR)` branch — including 23502 / 23505 — **you MUST
  emit** `overlay_write_redirect_enter(); overlay_overlay_helper_exit();`
  FIRST. If you skip this, WR depth counter underflows on longjmp →
  session-level PANIC "overlay_write_redirect counter < 0" on the next
  command.
- **No errtablecol(NULL) / errtableconstraint(rel, NULL)**: passing NULL to
  PG's error-reporting helpers dereferences a NULL pointer → SIGSEGV →
  backend crash (the user sees "server closed the connection"). Only use
  these when you KNOW you have a valid attnum / valid constraint name;
  otherwise stick to plain `errmsg_internal()` with explicit names.

**Test coverage**: Section R21 in `overlay_branch_rentry.sql` (5 subcases A–C).


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
- **[doc/review/poc_fix_tracker.md](doc/review/poc_fix_tracker.md)** —
  Review-260926 **分类一 (POC correctness) 14 项 Bug + R21** 修复跟踪台账。
  Contains per-item: batch id (Sxx), rule id (Rxx), root-cause summary,
  fix entry reference by **file + function name** (never line numbers — they drift),
  test Section anchor, and verification evidence.
  **Gating document**: any Rxx item here marked "⚡ PARTIAL" blocks new feature work
  until the item is closed.
- **[doc/review/review_260926.md](doc/review/review_260926.md)** — Full
  review-260926 input (2 categories: POC correctness Bugfixes = Cat 1,
  Productization ACL/hardening = Cat 2). Cat 1 is closed (R21 last item);
  Cat 2 (S13 G02 ACL, S04 snapshot spec, etc.) deferred to M4-M6.

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
| B1 Pure-Delta UPDATE (CMD_UPDATE 8-phase loop) | §D.11 |
| B2 Pure-Delta UPSERT (ON CONFLICT 4-way promo dispatch) | §D.12 |
| R14 TypedKey comparator | §D.13 (PK type-safety comparator design) |

### API / non-concurrency docs
- **[doc/design.md](doc/design.md)** — Top-level architecture (components,
  catalog tables `pg_branch` / `pg_branch_delta`, snapshot install
  lifecycle).
- **[doc/example_sql.md](doc/example_sql.md)** — Copy-pasteable user
  scenarios (create/use/apply/discard single session, no internal test
  noise).
- **[doc/branchscan.md](doc/branchscan.md)** — `BranchScan` node
  internals, materialization, WR interaction.  Relevant for T6 (AC-C1
  ReScan fresh materialization) and R14 (TypedKey sort/bsearch O((N+M) log M)).

### G06 minimal anti-leak (FOR ALL TABLES publication)
- **Scope**: PG 17 has **NO table-level `publish=false` reloption for
  plain heap tables**. Any CREATE EXTENSION-created internal table
  (`pg_branch`, `pg_branch_delta`) is auto-picked up by `FOR ALL TABLES`
  publications. G06 MVP uses **documented deploy contract + throttled 1×
  WARNING at 4 entry points**.
- Entry points (all in `src/branch_lifecycle.c` `ob_g06_check_publication_leak_throttled()`):
  `create_branch` / `use_branch` / `apply_branch` / `discard_branch`.
- Safety pattern: `static bool s_checked = false; s_warned = false;`
  (per-backend throttle) + `PG_TRY/PG_CATCH` fail-open (if publication
  catalog lookup fails, WARN and continue — never break user DML on a
  publication issue).
- L3 test: Section G06 in `overlay_branch_rentry.sql` (verifies WARNING
  fires 1× max; throttled subsequent calls silent).


---

## 6. Current Status & Jump Table (DO NOT mirror trackers here — keep 1 paragraph max)

**All T1–T8 + B1–B5 = DELIVERED.** Status source of truth:
- **Task/Batch status**: [doc/progress_tracker.md](doc/progress_tracker.md)
- **Bug-fix ledger (分类一 POC correctness)**: [doc/review/poc_fix_tracker.md](doc/review/poc_fix_tracker.md)
  - 分类一 14/14 + R21 = **CLOSED**
  - 分类二 (ACL / S04 / 分区外键报错等) = **DEFERRED M4–M6** — see [doc/review/review_260926.md](doc/review/review_260926.md)
- **Acceptance gate**: see §3 Testing — L3 11/11 + L1 8/8 = 19/19 2× clean rebuild green.
- **Hard rules deposited by B-series**: §4.R8–R21 (MVP scope, WR choke, TypedKey, PlanCache combo, CAS apply-freeze, 2-step base-image verify, Pure-delta 8-phase, R21 pre-check, G06).

Only append to §6.1 when a NEW B-series milestone completes **and** the corresponding new hard rule(s) have been written to §4. Do NOT keep long-running status logs here — trackers own that.

---

## 6.1 B-series Milestones (compact scope → rule → test entries only)

Post-baseline features. Each line = 1 B-item, 3 columns max.
**NO commit SHAs, NO build timestamps, NO debug stories in this table.**
Write the post-mortem lessons ONCE into §4 as R-rules; keep B-series as a navigable index for future agents to find the rule/test that corresponds to a feature name.

| B | Feature scope | Hard rule(s) deposited | Test anchor(s) |
|---|---|---|---|
| B1 | Pure-delta UPDATE fallback via reconstruct Slot + SET-merge | R9 (skip junk attnum≤0), R12 (PK mutation block pure-path), R19 (8-phase NOP), R20 (SET NULL parity) | `overlay_branch_puredelta.sql`; L1 `pure_delta_update.spec`, `pure_delta_delete.spec` |
| B2 | Pure-delta UPSERT ON CONFLICT 4-way promo 0/1/2/3 | R11 §UPSERT skip + **R21: only ONCONFLICT_NONE fires pre-check** (UPSERT Phase I/II richer); R12/R20 apply equally | `overlay_branch_upsert.sql`; L1 `pure_delta_upsert.spec` |
| B3 | 分类一 14/14 Bug fixes (S02→S10) + L3 2-giant → 11-file split | R8 (MVP scope), R10 (A3/A5 WR choke), R11 (COPY/MERGE/ModCTE 0A000), R12 (PK mutation 0A000), R13 (TTL kickout), R14 (TypedKey + no-SPI-in-comparator), R15 (TTSOpsVirtual materialize), R16 (PlanCache 2-hit), R17 (CAS `applying` freeze), R18 (2-step base-image verify) | L3: 11 files (see §6.2 split table). L1: `pure_delta_*`, `ob_applying_freeze`, `ob_state_inval`. rentry.sql Sections R-ENTRY, R-A3A5, R-API |
| B4 | G06 minimal anti-leak (FOR ALL TABLES pub) | §5 G06 entry: throttled 1× WARNING at 4 lifecycle points + COMMENT deploy contract; fail-open `PG_TRY` | rentry.sql Section G06 |
| B5 | R21 INSERT pre-check (ONCONFLICT_NONE only) | §4.R21 = NOT NULL + MAIN/delta PK UNIQUE 2-sided; **delta-first D-skip MAIN (rebirth)**; WR-counter pair on every ereport(ERROR); no errtable*(NULL) | rentry.sql Section R21 (A/B1/B2/B3/C) |

---

## 6.2 L3 Test File Index (post-B3 split — 11 modular files)

**Rule for agents adding new SQL coverage**: drop the Section into the file whose "Coverage" column matches closest. Don't create 13th file unless it genuinely spans ≥2 existing files AND would push any file over ~800 lines.
Every file starts with the idempotent header in §7 rule 7.

| File (under test/regress/sql/) | Purpose + Coverage (1 line each — user profile preference) |
|--------------------------------|------------------------------------------------------------|
| `overlay_branch_basic.sql`     | Create/use/apply/discard smoke + extension power-on. |
| `overlay_branch_user.sql`      | Public API signatures: all variants listed + `RETURNS` type asserted. |
| `overlay_branch_scan_pk.sql`   | BranchScan PK correctness: int4/int8/uuid PK overlay. |
| `overlay_branch_scan_type.sql` | BranchScan **typed PK** (R14): BPCHAR / NUMERIC / TIMESTAMPTZ; rebirth semantics. |
| `overlay_branch_puredelta_mix.sql` | MAIN-origin + pure-origin mixed overlay rows; mixed WR paths correctness. |
| `overlay_branch_upsert.sql`    | B2 ON CONFLICT: 7 promo cases A–G (promo 0/1/2/3). |
| `overlay_branch_puredelta.sql` | B1 pure-origin UPDATE/DELETE 8-phase loop; CTID/JOIN NOP. |
| `overlay_branch_mvcc_usage.sql` | MVCC daily-usage patterns: read-committed overlay, apply side-effects. |
| `overlay_branch_mvcc_bounds.sql` | MVCC edge/corner: WHERE-misses, boundary keys, anti-join coverage. |
| `overlay_branch_mvcc_review.sql` | S02/S03 R-entry: base API hardening, SET NULL parity. |
| `overlay_branch_rentry.sql`    | **Hard-negative gating** ONLY: R05/R06/R16 entry intercept; A3/A5 choke (R10); R-API PK-mutation 0A000; G06; R21. |

---

## 7. Development conventions (3 NON-NEGOTIABLE user standing orders — MUST do EVERY time)

These three rules are explicit user requirements from project kick-off; they apply to
EVERY feature delivery, bug fix, and incremental refactor. No exceptions.

### Conv-1. Every feature / bug fix ships with tests.
- **DO NOT submit** a code change (C function, catalog SQL change, planner hook tweak,
  lifecycle state transition, GUC, ANYTHING that changes observable behavior)
  without a matching L3 Section (single-session) OR L1 spec (cross-session) that
  exercises both the positive path and, where applicable, the negative error path
  (expected errcode 23505/0A000/55000 etc.).
- **Scope guidance**:
  - Public API behavior → `overlay_branch_user.sql` or `mvcc_review.sql`.
  - Constraint gating / error paths / MVP rejection gates → `overlay_branch_rentry.sql`.
  - Cross-session correctness → `test/isolation/specs/*.spec` + `isolation_schedule`.
  - Pure-delta correctness → `overlay_branch_puredelta.sql` or `puredelta_mix.sql`.
- **Evidence required in tracker**: Every closed tracker row must name the exact
  Section / spec file that proves correctness — no "trust me it works" entries.

### Conv-2. Significant design / implementation writes to doc/ first (R7 formalized).
User standing order (2026-09-14): *"关键实现设计先落 doc 再编码"* +
*"先保证正确和可测试，性能问题先记录到 doc，不急于优化"*.
1. For every new algorithm, state machine change, lock-ordering change, hook
   interception protocol, cross-backend consistency rule, or performance trade-off:
   write the design section FIRST into the matching doc file.
   - MVCC / lifecycle / concurrency → [doc/multi_session_mvcc.md](doc/multi_session_mvcc.md)
     (append under the correct D.x / I8.x appendix, don't create orphan headings).
   - Component architecture / catalog schema / snapshot install → [doc/design.md](doc/design.md).
   - BranchScan node internals, comparator strategy, ReScan semantics → [doc/branchscan.md](doc/branchscan.md).
   - Bug-fix batches / review item trackers with root-cause → [doc/review/poc_fix_tracker.md](doc/review/poc_fix_tracker.md)
     (link by Rxx id + function name).
2. THEN implement code (`include/*.h` → `src/*.c` → `overlay_branch--1.0.sql`).
3. THEN add tests (Conv-1).
4. THEN verify with Green-Bar Acceptance (§3).

### Conv-3. EVERY new feature → CHECK [doc/example_sql.md](doc/example_sql.md) for user-example sync.
`example_sql.md` is the user-facing copy-pasteable showcase of "how do I actually
USE this extension, from 0 to first `apply_branch`". It does NOT contain internal
test noise. It IS the canonical contract for every public API and recommended
usage pattern.

**What triggers an example_sql.md update?**
- New public SQL-callable function (or signature change of an existing one).
- New lifecycle pattern (e.g. `use_branch(NULL)` exit idiom was added; or a
  recommended parameter for SNAPSHOT mode changed).
- New negative-example "you probably SHOULDN'T do X" pattern that users will
  actually hit (e.g. ON CONFLICT limitations, PK-column UPDATE rejection).
- New canonical best-practice (e.g. recommended session setup, txn boundaries,
  discard-in-favor-of-apply guidance).

**Agent workflow for Conv-3**: After closing a feature (Green-Bar + doc sync),
**open `doc/example_sql.md` and scan it**. If the document is missing an example
for the new API / pattern, APPEND a minimal, runnable, copy-pasteable example
block. If the new feature changes a previously-documented example, **UPDATE the
matching block in place** (don't leave stale examples that will fail when users
copy-paste). Do NOT leave this check for "later" — later never comes.

---

## 8. Delivery / handoff hygiene (agent checklist)

Run through this list IN ORDER when you believe a task/batch is ready.

1. **Green-Bar Acceptance (§3)** = shorthand for: 0-warn build,
   2× independent clean rebuild, L3 11/11 PASS + L1 8/8 PASS after rebuild #2,
   **both** `diff -r` (test_output↔expected, output_iso↔expected) = 0 lines.
   If any item fails → NOT DELIVERABLE.
2. **Doc sync (Conv-2 + Conv-3 audit)**:
   - Significant design decisions? → appended to correct doc file (no orphan headings).
   - Changed `ob_take_branch_advisory_lock` / CAS gate / TypedKey structure /
     PlanCache invalidation combo / WR entry choke / R21 helper signature /
     ANY §4 R-rule implementation? → confirm the rule's entry-point FUNCTION
     NAMES (never line numbers) in §4 still match the actual code. Drifted? → fix §4.
   - New feature visible to users? → `example_sql.md` checked and updated per Conv-3.
3. **Tracker sync (2 docs)**:
   * [doc/progress_tracker.md](doc/progress_tracker.md): flip T/B status ⬜→✅;
     append TR-x.y checkbox verdicts with concrete evidence (Section names, spec
     names, not vague prose).
   * [doc/review/poc_fix_tracker.md](doc/review/poc_fix_tracker.md): if any POC
     Rxx item moved state, flip it (⚡ PARTIAL → ✅ FIXED only after Green-Bar)
     and append:
     (a) **fix location as `file : function_name`** (never `file#Lx-Ly` — line numbers drift),
     (b) test Section anchor (e.g. "R21 rentry.sql A/B1/B2/B3/C"),
     (c) Green-Bar evidence ("§3 Green-Bar confirmed: L3 11/11, L1 8/8, 2× rebuild").
4. **SQL-file idempotency / naming (if tests were added / refactored)**:
   - If you created / split a L3 SQL file, EVERY file in the suite MUST start with:
     ```sql
     /* Purpose:    single-line description
      * Coverage:   1-line what scenarios it tests (see §6.2 table for style)
      * Dependencies: overlay_branch basic setup; standard single-PK heap tables only
      */
     DROP EXTENSION IF EXISTS overlay_branch CASCADE;
     RESET ALL;
     CREATE EXTENSION IF NOT EXISTS overlay_branch;
     ```
     (Reason: a single file must be runnable standalone via
     `pg_regress --inputdir=. overlay_branch_foo.sql` without prior state.
     Split-phase bugs have hit us 3× when this prefix was omitted on new files.)
   - Register any new file in `Makefile REGRESS = ...` at a dependency-correct
     position (basic → user → scan_pk → … → mvcc_review → rentry is the current
     topological order; don't insert a pure-delta test before `basic.sql` runs).
   - Section names (A / B / R-ENTRY / G06 / …) + PASS labels: every `PASS:X_Y_Z`
     literal must be UNIQUE across the entire `test/regress/sql/` directory.
     Run: `grep -roh 'PASS:[A-Z0-9_]*' test/regress/sql/ | sort | uniq -c | awk '$1>1'`
     → must print 0 lines. Non-unique PASS labels make it impossible to grep "which
     Section does this failure belong to?"
5. **Baseline copy**: Any Section added to L3 or L1 → cp the actual `.out` into
   `test/*/expected/` per §3 Baseline management, THEN re-run Green-Bar to confirm
   0 diff. NEVER leave `.diffs` files for the NEXT agent to clean up — that's the
   #1 source of false "this regressed" panics.
6. **Commit hygiene (user preference)**: Avoid frequent tiny commits. Batch
   related changes (one B-milestone, OR one Sxx bug-fix batch, OR one refactor +
   its matching tests + baseline copy + doc sync) into a SINGLE commit.
   Subject line format: `overlay_branch: <milestone id + short description>`.
   English body-free subject line is fine; Chinese is acceptable for the short
   description when it's clearer (e.g. 分类一台账 / R21 前置检查).


