# AGENTS.md

> Agent-only instructions for the `contrib/overlay_branch` PostgreSQL extension.
> This is the "README for agents": context, build/test commands, and hard rules.
> Anything detailed lives in `doc/` — see the index at the end. Do NOT mirror
> trackers or test plans here.

---

## 1. Project overview

A PostgreSQL contrib extension that adds **overlay branch** semantics:

- `use_branch(name)` — the session sees a snapshot + write-redirected view of
  managed tables; writes land in the branch delta, not MAIN.
- `apply_branch(name)` — 3-pass (D→U→I) merge of the branch delta back to MAIN.
- `discard_branch(name)` — drop branch-local writes.

Think "git for SQL rows": xact-scoped, protected by branch-level advisory locks.

**Managed tables** = plain heap tables with a **single-column PRIMARY KEY**.
No-PK tables are rejected by default; they are supported only as an
**experimental** feature behind the GUC `overlay_branch.keyless_tables`
(`bool`, `PGC_SUSET`, default `off`), which falls back to a best-effort `ctid`
identity (`A:<blk>:<off>` anchored / `S:<seq>` synthetic) after a one-shot
WARNING. See `doc/keyless_ctid_design.md`.

**Flat version 1.0, no upgrade scripts.** Always mutate `overlay_branch--1.0.sql`
in place — append `CREATE OR REPLACE FUNCTION` / `ALTER TYPE ... ADD ATTRIBUTE`
at the bottom. Never (re)create `--1.0--1.x.sql`.

### Public API (copy these signatures, never guess)

| Callable | Signature | Notes |
|---|---|---|
| `create_branch` | `create_branch(branch_name text) RETURNS integer` | **single argument only** — pre-create user tables in MAIN first. |
| `use_branch` | `use_branch(branch_name text)` / `use_branch(branch_name text, isolation_mode text DEFAULT 'read committed')` | `isolation_mode` ∈ `'read committed'` / `'snapshot'`; pass `NULL` to exit branch mode. |
| `apply_branch` | `apply_branch(branch_name text) RETURNS void` | CAS `active→applying`, then 3-pass merge. |
| `discard_branch` | `discard_branch(branch_name text) RETURNS void` | CAS `active→discarding`, then bulk-delete the delta. |
| `overlay_main_plus_delta` | `overlay_main_plus_delta(tbl regclass) RETURNS SETOF record` | Live latest-wins overlay view; used by tests. |
| `is_active` / `force_invalidation_check` | notify/throttle layer | See `doc/multi_session_mvcc.md`. |

There is **no `diff_branch`** — it never existed.

---

## 2. Build & install

Always run from `contrib/overlay_branch`.

```bash
# clean rebuild (drop every stale artifact first)
rm -rf src/*.o src/*.o.d overlay_branch.so \
       test_output tmp_check output_iso tmp_check_iso ../../tmp_install \
       /home/ubuntu/pg17/lib/postgresql/overlay_branch.so

make -j4 install prefix=/home/ubuntu/pg17
#   Zero-warning policy: gcc -Wall must print 0 warnings + 0 errors.
#   `-Wdeclaration-after-statement` is a HARD FAIL (see §4).
```

---

## 3. Tests

Three layers — do **not** force cross-session races into L3.

```bash
# L3 — single-session deterministic sanity (pg_regress, 17 files)
make check prefix=/home/ubuntu/pg17
#   accept: ok 1..17  +  "# All 17 tests passed."

# L1 — exact scheduled cross-session waits (pg_isolation_regress, 12 specs)
make -C ../../src/test/isolation pg_isolation_regress \
  EXTRA_REGRESS_OPTS="--inputdir=$(pwd)/test/isolation --temp-config=$(pwd)/temp_instance_shared_libs.conf" \
  REGRESS_SHLIB=$(pwd)/../../src/test/isolation/pg_isolation_regress.so \
  SPECS="ob_apply_mutex ob_state_inval ob_applying_freeze ob_snapshot_mode ob_branchscan_rescan pure_delta_upsert pure_delta_update pure_delta_delete ob_view_unify_rescan keyless_basic keyless_pure_delta_repeat keyless_apply_conflict"
#   accept: 12/12 PASS 0 diff
```

- **L2** — `pgbench` stress harness under `test/bench/`, run manually; see
  `test/bench/README.md`.

**Locale prerequisite for `make check` / `make installcheck`.** The temp
`initdb` needs a *real, installed* UTF-8 locale. A `LANG`/`LC_*` naming a missing
locale (e.g. `zh_CN.UTF-8` on a bare host) aborts with `invalid locale settings`;
forcing `LANG=C` instead mis-measures multibyte widths and yields bogus
column-width diffs in `mvcc_review` / `rentry`. Use `LC_ALL=C.utf8`.

**Changing or adding a test → baseline management**

1. First run FAILs when you add lines; `cat test_output/regression.diffs` and
   confirm **only `+` lines** (no `-`, no `ERROR:`, no `syntax error`).
2. `cp test_output/results/<FILE>.out test/regress/expected/` (L3),
   or `cp output_iso/results/<SPEC>.out test/isolation/expected/` (L1).
3. Re-run and confirm **0 diff** (`diff -r test_output/results test/regress/expected`,
   `diff -r output_iso/results test/isolation/expected`). Never leave `.diffs`
   files behind.

**Green-Bar = definition of "delivered"** — ALL of: 0-warning build,
**two independent clean rebuilds**, and after rebuild #2 L3 17/17 + L1 12/12
with both `diff -r` = 0 lines.

---

## 4. Critical rules (read before editing C or SQL)

### C code
- **Strict C90 top-declaration.** Declare every local at the top of its block,
  before the first executable line. `-Wdeclaration-after-statement` is a hard fail.
- **Virtual tuple slots**: call `slot_getsomeattrs(slot, N)` before touching
  `tts_values` / `tts_isnull`. `tts_nvalid` is a "valid up to N" marker, not a
  promise the Datums are materialized.
- **Recursive re-entry guards**: balance the static depth / opt-in bool guards in
  the invalidation + planner hooks on every path (use `PG_TRY`/`PG_FINALLY`).
  Never widen the throttle kickout opt-in beyond DML CmdTypes.
- **CMD_UPDATE ProjInfo clone**: skip junk columns (`attnum ≤ 0`) — they point
  into the parent plan and dangle after `ExecEndNode`.
- **Error helpers**: never pass `NULL` to `errtablecol` / `errtableconstraint`.
  Inside the redirect helper, restore the WR counter **before** any
  `ereport(ERROR)`, or the session PANICs on the next command.

### API & SQL harness
- `create_branch` takes **exactly one** argument; the two-argument form never existed.
- pg_regress runs SQL through the backend, **not** the psql lexer: no `\gset`,
  `\set`, `:variable`, `\gexec`, `\if`. Carry a `branch_id` with an inline
  subquery or CTE.
- SQL test files are standalone-idempotent: start with
  `DROP EXTENSION IF EXISTS overlay_branch CASCADE; RESET ALL;
  CREATE EXTENSION IF NOT EXISTS overlay_branch;` plus a
  `Purpose / Coverage / Dependencies` header comment. Register new files in the
  Makefile at a dependency-correct position. Every `PASS:` label must be unique
  across `test/regress/sql/`.

### Locking / concurrency
- **Lock order**: take the CAS row-lock first; only the CAS winner takes the
  xact-scoped **Exclusive** advisory lock, *before* opening user relations and
  *before* the final catalog write. `use_branch` takes **no** advisory lock.
- Never reintroduce a SHARED→EXCLUSIVE advisory upgrade — it is a classic 40P01
  deadlock.
- **Apply freeze**: STEP ZERO is `UPDATE ... SET state='applying' WHERE
  state='active'` (row-count 0 → ERROR 55000). The write path re-checks
  `state='active'` right before inserting the delta.
- **Plan cache invalidation on branch switch is two steps**:
  `CacheInvalidateRelcacheAll()` **then** `CommandEndInvalidationMessages()`.
  Skipping the second step leaves prepared statements reading MAIN silently.

### Apply path
- **Verify the MAIN row before overwriting it**: fetch by `_base_ctid`, assert
  `xmin == _base_xmin`; mismatch → ERROR 55000. Pure-origin rows skip this.
- **Pure-delta DML loop** is latest-wins (I/U/D) and must NOP when the statement
  produces no net change (otherwise re-apply double-inserts).
- **SET NULL parity**: store the column as JSON `null` — never omit it — on both
  the MAIN and pure-delta paths.
- **R21 INSERT pre-check** fires only for `ONCONFLICT_NONE`: NOT NULL + PK UNIQUE
  checked on the delta **first** (a tombstone `op='D'` legally allows rebirth),
  then MAIN. UPSERT keeps its own 2-phase conflict detection.

### Identity & types
- **Never compare PKs as text.** Use the `TypedKey` struct (real datum +
  typid/typmod/collid + lt/eq `FmgrInfo`) so BPCHAR rtrim, NUMERIC precision, and
  TIMESTAMPTZ timezone differences don't collide.
- **Never call SPI inside a qsort/bsearch comparator** — SPI memory is freed at
  `SPI_finish`, giving a `pfree: invalid pointer` PANIC. Anchor TypedKey memory in
  `TopMemoryContext` (or the scan node's ExprContext).
- SNAPSHOT mode is TTL-bounded; exceeding `overlay_branch.snapshot_ttl_sec`
  forcibly exits branch mode (do not let snapshotters pin the MVCC horizon).

### Scope & gating
- Reject at `create_branch` time (0A000/55000), never at execution: partitioned /
  foreign tables, views, matviews, foreign keys, multi-column PKs, non-PK UNIQUE.
- **MVP write interceptions** — at the very top of the executor intercept before
  any redirect: `COPY FROM` → 0A000, `MERGE` → 0A000, data-modifying CTE on a
  managed table → 0A000.
- **Write-through choke**: if a managed-table write reaches
  `standard_ExecutorRun` without going through the redirect → ERROR 55000.
- **Never UPDATE the PK column** (0A000) — on both the MAIN and pure-delta paths.
- **Anti-leak (G06)**: PG 17 has no table-level `publish=false` for plain heaps,
  so emit a throttled one-shot WARNING at the four lifecycle entry points, and
  fail open under `PG_TRY` (never break user DML on a publication issue).

---

## 5. Development conventions (apply to EVERY change)

- **Conv-1 — every change ships with a test.** No behavior change (C, catalog SQL,
  hooks, GUC) without an L3 Section or L1 spec covering the positive path and,
  where applicable, the negative error path. Scope: public API → `user` /
  `mvcc_review`; gates / errors → `rentry`; cross-session → `test/isolation/specs/`
  + `isolation_schedule`; pure-delta → `puredelta` / `puredelta_mix`.
- **Conv-2 — design first.** Write the design into the matching doc BEFORE coding:
  MVCC / locks → `doc/multi_session_mvcc.md`; architecture / catalog →
  `doc/design.md`; BranchScan node → `doc/branchscan.md`; implementation detail →
  `doc/internals.md`. Then code (`include/*.h` → `src/*.c` → `overlay_branch--1.0.sql`),
  then tests, then Green-Bar.
- **Conv-3 — sync the user examples.** For every new/changed public API or
  recommended pattern, open `doc/example_sql.md` and add or update a minimal,
  runnable example block.

**Before declaring done**: Green-Bar passes (§3); docs synced (Conv-2 + Conv-3);
and the trackers are flipped (`doc/progress_tracker.md` +
`doc/review/review_260926_tracker.md`) with concrete evidence — a test
section/spec name and the fix location as `file : function_name` (never line
numbers). No "trust me" entries.

---

## 6. Commit hygiene

Avoid frequent tiny commits. Batch one milestone / one bug-fix batch — code +
its tests + baseline copy + doc sync — into a **single** commit. Subject format:
`overlay_branch: <short description>` (Chinese is fine inside the description when
clearer).

---

## 7. Documentation index

| File | What it covers |
|---|---|
| `README.md` | Human-facing feature overview + quick start. |
| `doc/design.md` | Top-level architecture, catalog tables, snapshot lifecycle. |
| `doc/internals.md` | Internal implementation: BranchScan, write redirect, concurrency, corner cases. |
| `doc/branchscan.md` | `BranchScan` node internals, materialization, ReScan. |
| `doc/multi_session_mvcc.md` | Multi-session MVCC design + lock protocol. |
| `doc/keyless_ctid_design.md` | No-PK (keyless) ctid identity — GUC, encoding, limits. |
| `doc/example_sql.md` | Copy-pasteable user scenarios (keep in sync — Conv-3). |
| `doc/ops_runbook.md` | Operations runbook. |
| `doc/test_coverage_index.md` | Test coverage index (L3 + L1). |
| `doc/progress_tracker.md` | Task / batch tracker. |
| `doc/review/review_260926.md`, `doc/review/review_260926_tracker.md` | Review ledger + fix tracker. |
| `doc/agent_workflow.md` | Agent workflow notes. |
| `test/bench/README.md` | L2 `pgbench` stress harness. |
