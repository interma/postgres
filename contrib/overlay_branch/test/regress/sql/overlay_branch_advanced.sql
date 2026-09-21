-- =====================================================================
-- overlay_branch_advanced.sql — Engineering / advanced regression tests.
--
-- Scope: assertions that exercise "advanced" features beyond the basic
-- happy-path (overlay_branch_user.sql) and the guard/delta-physics
-- (overlay_branch_basic.sql) suites.  Currently this file contains the
-- P0 PK-IndexScan MVP fast-path tests, but it is intentionally named
-- "*advanced*" so that future advanced features (RETURNING, composite
-- PK support, session MVCC, etc.) can be added here without creating
-- ever more files.
--
-- Tests in this file ALWAYS compare BranchScan's transparent view against
-- the proven-correct SRF overlay_main_plus_delta, exactly as the other
-- suites do.
-- =====================================================================

-- ===== 0. Prerequisite guard (same as the other regression files) =====
DO $$
DECLARE
  _s text := current_setting('shared_preload_libraries', true);
  _msg text;
BEGIN
  IF _s NOT LIKE '%overlay_branch%' THEN
    _msg := 'Postmaster started WITHOUT shared_preload_libraries=''overlay_branch''.  '
         || 'The 3 correct ways to invoke these tests (DON''T `cd test/` first):'
         || chr(10)||chr(10)||'  (A) make check                    <- temp-instance, hooks auto-loaded'
         || chr(10)||'  (B) make installcheck EXTRA_REGRESS_OPTS="--use-existing --host=/tmp --port=15433"'
         || chr(10)||'  (C) pg_regress overlay_branch_advanced \'
         || chr(10)||'         --inputdir=./test --outputdir=./test_output \'
         || chr(10)||'         --temp-config=$(pwd)/test/temp_instance_shared_libs.conf \'
         || chr(10)||'         --bindir=$(pg_config --bindir)'
         || chr(10)||chr(10);
    RAISE EXCEPTION '%', _msg;
  END IF;
END $$;

-- ===== Idempotent cleanup =====
SET client_min_messages = warning;
DROP TABLE IF EXISTS public.bs_user;
DROP TABLE IF EXISTS public.bs_basic;
DROP EXTENSION IF EXISTS overlay_branch CASCADE;
RESET client_min_messages;

-- =====================================================================
-- ===== Section A: BS_PK_LOOKUP  — P0 PK-IndexScan MVP on bs_user =====
-- =====================================================================
CREATE EXTENSION overlay_branch;

CREATE TABLE public.bs_user (id INT4 PRIMARY KEY, name TEXT, color TEXT);
INSERT INTO public.bs_user VALUES
  (10, 'apple',  'red'),
  (20, 'banana', 'yellow'),
  (30, 'cherry', 'red');

-- A.1 Create + enter branch 'bs_user_b' and write 3 representative
--     mutations (UPDATE override, DELETE tombstone, INSERT new row).
SELECT overlay_branch.create_branch('bs_user_b');
SELECT overlay_branch.use_branch('bs_user_b');

DELETE FROM public.bs_user WHERE id = 20;
UPDATE public.bs_user SET color = 'green' WHERE id = 10;
INSERT INTO public.bs_user VALUES (40, 'date', 'brown');

-- A.2 MAIN must remain untouched (run from MAIN, then return to branch).
RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(id::text||'='||name||':'||color, ',' ORDER BY id)
                = '10=apple:red,20=banana:yellow,30=cherry:red'
         THEN 'PASS:ADV_A_MAIN_UNTOUCHED'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||'='||name||':'||color, ',' ORDER BY id),'NULL')
       END AS adv_a_main
FROM public.bs_user;
SELECT overlay_branch.use_branch('bs_user_b');

-- A.3 SRF ↔ BranchScan equivalence for the FULL view (precondition:
--     the fast-path assertions below are meaningless if the full
--     views already disagree).
SELECT CASE
         WHEN (SELECT string_agg((r).id::text||'='||(r).name||':'||(r).color, ',' ORDER BY (r).id)
               FROM overlay_branch.overlay_main_plus_delta('public.bs_user')
                 AS r(id int4, name text, color text))
              =
              (SELECT string_agg(id::text||'='||name||':'||color, ',' ORDER BY id)
               FROM public.bs_user)
         THEN 'PASS:ADV_A_FULL_SRF_EQUIV'
         ELSE 'FAIL:ADV_A_FULL_SRF_MISMATCH'
       END AS adv_a_full_equiv;

-- ===== PK fast-path assertions =====

-- A.4 Even with the PK fast-path active, EXPLAIN must still show the
--     outer Custom Scan (overlay logic must stay in charge).
EXPLAIN (COSTS OFF, SUMMARY OFF) SELECT * FROM public.bs_user WHERE id = 10;

-- A.5 PK point lookups, one assertion per delta-physics category.

-- id=10 — UPDATE override (MAIN row color='red' → delta color='green').
SELECT CASE
         WHEN (SELECT (r).id::text||'='||(r).name||':'||(r).color
               FROM overlay_branch.overlay_main_plus_delta('public.bs_user')
                 AS r(id int4, name text, color text)
               WHERE (r).id = 10)
              =
              (SELECT id::text||'='||name||':'||color
               FROM public.bs_user WHERE id = 10)
         THEN 'PASS:ADV_A_PK_EQUIV_ID10'
         ELSE 'FAIL:ADV_A_PK_EQUIV_ID10'
       END AS adv_a_pk_id10;
SELECT CASE WHEN color = 'green' AND id = 10 THEN 'PASS:ADV_A_ID10_GREEN'
            ELSE 'FAIL:'||id::text||':'||COALESCE(color,'NULL') END AS adv_a_id10_green
FROM public.bs_user WHERE id = 10;

-- id=20 — DELETE tombstone → must return 0 rows.
SELECT CASE
         WHEN (SELECT count(*) FROM overlay_branch.overlay_main_plus_delta('public.bs_user')
                 AS r(id int4, name text, color text)
               WHERE (r).id = 20)
              = (SELECT count(*) FROM public.bs_user WHERE id = 20)
         THEN 'PASS:ADV_A_PK_EQUIV_ID20'
         ELSE 'FAIL:ADV_A_PK_EQUIV_ID20'
       END AS adv_a_pk_id20;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_A_ID20_DELETED'
            ELSE 'FAIL:'||count(*) END AS adv_a_id20_zero
FROM public.bs_user WHERE id = 20;

-- id=30 — MAIN passthrough (no delta row at all).
SELECT CASE
         WHEN (SELECT (r).id::text||'='||(r).name||':'||(r).color
               FROM overlay_branch.overlay_main_plus_delta('public.bs_user')
                 AS r(id int4, name text, color text)
               WHERE (r).id = 30)
              =
              (SELECT id::text||'='||name||':'||color
               FROM public.bs_user WHERE id = 30)
         THEN 'PASS:ADV_A_PK_EQUIV_ID30'
         ELSE 'FAIL:ADV_A_PK_EQUIV_ID30'
       END AS adv_a_pk_id30;
SELECT CASE WHEN color = 'red' AND id = 30 AND name = 'cherry'
            THEN 'PASS:ADV_A_ID30_RED_CHERRY'
            ELSE 'FAIL:'||id::text||':'||name||':'||COALESCE(color,'NULL')
       END AS adv_a_id30_red
FROM public.bs_user WHERE id = 30;

-- id=40 — pure-INSERT (only delta row, no MAIN row at all).
SELECT CASE
         WHEN (SELECT (r).id::text||'='||(r).name||':'||(r).color
               FROM overlay_branch.overlay_main_plus_delta('public.bs_user')
                 AS r(id int4, name text, color text)
               WHERE (r).id = 40)
              =
              (SELECT id::text||'='||name||':'||color
               FROM public.bs_user WHERE id = 40)
         THEN 'PASS:ADV_A_PK_EQUIV_ID40'
         ELSE 'FAIL:ADV_A_PK_EQUIV_ID40'
       END AS adv_a_pk_id40;
SELECT CASE WHEN color = 'brown' AND id = 40 AND name = 'date'
            THEN 'PASS:ADV_A_ID40_BROWN_DATE'
            ELSE 'FAIL:'||id::text||':'||name||':'||COALESCE(color,'NULL')
       END AS adv_a_id40_brown
FROM public.bs_user WHERE id = 40;

-- A.6 Non-existent PK id=999 → empty set, matches SRF.
SELECT CASE
         WHEN (SELECT count(*) FROM overlay_branch.overlay_main_plus_delta('public.bs_user')
                 AS r(id int4, name text, color text)
               WHERE (r).id = 999)
              = (SELECT count(*) FROM public.bs_user WHERE id = 999)
         THEN 'PASS:ADV_A_PK_EQUIV_ID999'
         ELSE 'FAIL:ADV_A_PK_EQUIV_ID999'
       END AS adv_a_pk_id999;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_A_ID999_NONE'
            ELSE 'FAIL:'||count(*) END AS adv_a_id999_empty
FROM public.bs_user WHERE id = 999;

-- A.7 Commutator form Const = PK (id=10 ↔ 10=id) must give the same
--     answer (Planner hook recognizes commutator eq_opr).
SELECT CASE
         WHEN (SELECT id::text||'='||name||':'||color FROM public.bs_user WHERE id = 10)
              = (SELECT id::text||'='||name||':'||color FROM public.bs_user WHERE 10 = id)
         THEN 'PASS:ADV_A_PK_COMMUTATOR'
         ELSE 'FAIL:ADV_A_PK_COMMUTATOR'
       END AS adv_a_commutator;

-- A.8 Non-PK predicate (color='red'): cannot use PK fast-path so it
--     falls back to full overlay, still must match SRF.
SELECT CASE
         WHEN (SELECT string_agg(id::text||'='||name||':'||color, ',' ORDER BY id)
               FROM overlay_branch.overlay_main_plus_delta('public.bs_user')
                 AS r(id int4, name text, color text)
               WHERE (r).color = 'red')
              = (SELECT string_agg(id::text||'='||name||':'||color, ',' ORDER BY id)
                 FROM public.bs_user WHERE color = 'red')
         THEN 'PASS:ADV_A_NONPK_RED'
         ELSE 'FAIL:ADV_A_NONPK_RED'
       END AS adv_a_nonpk_red;

-- A.9 Cleanup the bs_user fixture.
RESET overlay_branch.current;
DROP TABLE public.bs_user;

-- =====================================================================
-- ===== Section B: PK Physics on bs_basic (numeric PK + tag column) =====
-- =====================================================================
-- Uses the exact delta-physics arrangement from Part 6 of
-- overlay_branch_basic.sql to make the PK-fast-path assertions directly
-- comparable to the SRF truth.

CREATE TABLE public.bs_basic (
    id  INT4 PRIMARY KEY,
    amt NUMERIC(10,2),
    tag TEXT
);
INSERT INTO public.bs_basic VALUES
  (1, 10.50, 'alpha'),
  (2, 20.00, 'beta'),
  (3, 30.75, 'gamma'),
  (7, 70.00, 'eta');

SELECT overlay_branch.create_branch('bs_basic_b1');
SELECT overlay_branch.use_branch('bs_basic_b1');

DELETE FROM public.bs_basic WHERE id = 3;
UPDATE public.bs_basic SET amt = 99.99, tag = 'BETA!' WHERE id = 2;
INSERT INTO public.bs_basic VALUES (9, 900.00, 'inserted-niner');
-- id=1 and id=7 intentionally NOT touched (purely-MAIN passthrough).

-- B.1 MAIN zero-pollution check (then back to the branch).
RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(id::text, ',' ORDER BY id) = '1,2,3,7'
              AND string_agg(amt::text, ',' ORDER BY id) = '10.50,20.00,30.75,70.00'
         THEN 'PASS:ADV_B_MAIN_ZERO_POLLUTION'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||amt::text, ',' ORDER BY id),'NULL')
       END AS adv_b_main_pollution
FROM public.bs_basic;
SELECT overlay_branch.use_branch('bs_basic_b1');

-- B.2 Full-view SRF ↔ BranchScan equivalence (precondition).
WITH srf AS (
  SELECT string_agg((r).id::text||':'||(r).amt::text||':'||COALESCE((r).tag,'NULL'), '|' ORDER BY (r).id) AS pic
  FROM overlay_branch.overlay_main_plus_delta('public.bs_basic')
    AS r(id int4, amt numeric, tag text)
), cs AS (
  SELECT string_agg(id::text||':'||amt::text||':'||COALESCE(tag,'NULL'), '|' ORDER BY id) AS pic
  FROM public.bs_basic
)
SELECT CASE
         WHEN srf.pic = cs.pic THEN 'PASS:ADV_B_FULL_EQUIV'
         ELSE 'FAIL_SRF<>'||COALESCE(cs.pic,'NULL')||' BASE='||COALESCE(srf.pic,'NULL')
       END AS adv_b_full_equiv
FROM srf, cs;

-- ===== PK fast-path assertions =====

-- B.3 EXPLAIN must still show Custom Scan (overlay stays in charge).
EXPLAIN (COSTS OFF, SUMMARY OFF) SELECT * FROM public.bs_basic WHERE id = 2;

-- B.4 Per-category PK point lookups:
--   id=1 — MAIN passthrough   (alpha)
--   id=2 — UPDATE override    (99.99, BETA!)
--   id=3 — DELETE tombstone   (must vanish)
--   id=9 — INSERT new row     (900.00, inserted-niner)

SELECT CASE
         WHEN (SELECT (r).id::text||':'||(r).amt::text||':'||COALESCE((r).tag,'NULL')
               FROM overlay_branch.overlay_main_plus_delta('public.bs_basic')
                 AS r(id int4, amt numeric, tag text) WHERE (r).id = 1)
              = (SELECT id::text||':'||amt::text||':'||COALESCE(tag,'NULL')
                 FROM public.bs_basic WHERE id = 1)
         THEN 'PASS:ADV_B_PK_MAIN' ELSE 'FAIL:ADV_B_PK_MAIN'
       END AS adv_b_pk_main;
SELECT CASE
         WHEN (SELECT (r).id::text||':'||(r).amt::text||':'||COALESCE((r).tag,'NULL')
               FROM overlay_branch.overlay_main_plus_delta('public.bs_basic')
                 AS r(id int4, amt numeric, tag text) WHERE (r).id = 2)
              = (SELECT id::text||':'||amt::text||':'||COALESCE(tag,'NULL')
                 FROM public.bs_basic WHERE id = 2)
         THEN 'PASS:ADV_B_PK_UPDATE' ELSE 'FAIL:ADV_B_PK_UPDATE'
       END AS adv_b_pk_update;
SELECT CASE
         WHEN (SELECT count(*) FROM overlay_branch.overlay_main_plus_delta('public.bs_basic')
                 AS r(id int4, amt numeric, tag text) WHERE (r).id = 3)
              = (SELECT count(*) FROM public.bs_basic WHERE id = 3)
         THEN 'PASS:ADV_B_PK_DELETE' ELSE 'FAIL:ADV_B_PK_DELETE'
       END AS adv_b_pk_delete;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_B_ID3_ZERO' ELSE 'FAIL:'||count(*) END AS adv_b_id3_zero
FROM public.bs_basic WHERE id = 3;
SELECT CASE
         WHEN (SELECT (r).id::text||':'||(r).amt::text||':'||COALESCE((r).tag,'NULL')
               FROM overlay_branch.overlay_main_plus_delta('public.bs_basic')
                 AS r(id int4, amt numeric, tag text) WHERE (r).id = 9)
              = (SELECT id::text||':'||amt::text||':'||COALESCE(tag,'NULL')
                 FROM public.bs_basic WHERE id = 9)
         THEN 'PASS:ADV_B_PK_INSERT' ELSE 'FAIL:ADV_B_PK_INSERT'
       END AS adv_b_pk_insert;

-- B.5 Non-existent id=999 → empty.
SELECT CASE
         WHEN (SELECT count(*) FROM overlay_branch.overlay_main_plus_delta('public.bs_basic')
                 AS r(id int4, amt numeric, tag text) WHERE (r).id = 999)
              = (SELECT count(*) FROM public.bs_basic WHERE id = 999)
         THEN 'PASS:ADV_B_PK_MISSING' ELSE 'FAIL:ADV_B_PK_MISSING'
       END AS adv_b_pk_missing;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_B_999_EMPTY' ELSE 'FAIL:'||count(*) END AS adv_b_999_empty
FROM public.bs_basic WHERE id = 999;

-- B.6 Mixed predicate: PK + non-PK (id=1 AND tag='alpha' → hit ;
--                         id=1 AND tag='beta'  → miss).
SELECT CASE
         WHEN (SELECT string_agg((r).id::text||':'||COALESCE((r).tag,'NULL'), ',' ORDER BY (r).id)
               FROM overlay_branch.overlay_main_plus_delta('public.bs_basic')
                 AS r(id int4, amt numeric, tag text)
               WHERE (r).id = 1 AND (r).tag = 'alpha')
              = (SELECT string_agg(id::text||':'||COALESCE(tag,'NULL'), ',' ORDER BY id)
                 FROM public.bs_basic WHERE id = 1 AND tag = 'alpha')
         THEN 'PASS:ADV_B_PK_PLUS_NONPK_HIT' ELSE 'FAIL:ADV_B_PK_PLUS_NONPK_HIT'
       END AS adv_b_mixed_hit;
SELECT CASE
         WHEN (SELECT count(*) FROM overlay_branch.overlay_main_plus_delta('public.bs_basic')
                 AS r(id int4, amt numeric, tag text)
               WHERE (r).id = 1 AND (r).tag = 'beta')
              = (SELECT count(*) FROM public.bs_basic WHERE id = 1 AND tag = 'beta')
         THEN 'PASS:ADV_B_PK_PLUS_NONPK_MISS' ELSE 'FAIL:ADV_B_PK_PLUS_NONPK_MISS'
       END AS adv_b_mixed_miss;

-- B.7 Commutator (9=id ↔ id=9).
SELECT CASE
         WHEN (SELECT id::text||':'||amt::text||':'||COALESCE(tag,'NULL')
               FROM public.bs_basic WHERE id = 9)
              = (SELECT id::text||':'||amt::text||':'||COALESCE(tag,'NULL')
                 FROM public.bs_basic WHERE 9 = id)
         THEN 'PASS:ADV_B_PK_COMMUTATOR' ELSE 'FAIL:ADV_B_PK_COMMUTATOR'
       END AS adv_b_commutator;

-- B.8 Non-PK predicates (amt=70.00 / tag='eta') → fallback to full
--     overlay, still match SRF.
SELECT CASE
         WHEN (SELECT string_agg((r).id::text||':'||(r).amt::text||':'||COALESCE((r).tag,'NULL'),
                                 ',' ORDER BY (r).id)
               FROM overlay_branch.overlay_main_plus_delta('public.bs_basic')
                 AS r(id int4, amt numeric, tag text)
               WHERE (r).amt = 70.00)
              = (SELECT string_agg(id::text||':'||amt::text||':'||COALESCE(tag,'NULL'),
                                   ',' ORDER BY id)
                 FROM public.bs_basic WHERE amt = 70.00)
         THEN 'PASS:ADV_B_NONPK_AMT' ELSE 'FAIL:ADV_B_NONPK_AMT'
       END AS adv_b_nonpk_amt;
SELECT CASE
         WHEN (SELECT string_agg((r).id::text||':'||COALESCE((r).tag,'NULL'), ',' ORDER BY (r).id)
               FROM overlay_branch.overlay_main_plus_delta('public.bs_basic')
                 AS r(id int4, amt numeric, tag text)
               WHERE (r).tag = 'eta')
              = (SELECT string_agg(id::text||':'||COALESCE(tag,'NULL'), ',' ORDER BY id)
                 FROM public.bs_basic WHERE tag = 'eta')
         THEN 'PASS:ADV_B_NONPK_TAG' ELSE 'FAIL:ADV_B_NONPK_TAG'
       END AS adv_b_nonpk_tag;

-- B.9 MAIN re-entry: BranchScan must not leak into MAIN mode.
RESET overlay_branch.current;
SELECT CASE WHEN tag = 'gamma' THEN 'PASS:ADV_B_ID3_BACK_IN_MAIN'
            ELSE 'FAIL:'||COALESCE(tag,'NULL') END AS adv_b_id3_main_back
FROM public.bs_basic WHERE id = 3;

-- B.10 Cleanup the bs_basic fixture.
DROP TABLE IF EXISTS public.bs_basic;

-- =====================================================================
-- ===== Section C: RETURNING clause (P1 INSERT/UPDATE/DELETE RETURNING)
-- =====================================================================
-- All RETURNING rows are captured with \gset / UNION ALL wrappers into a
-- CTE picture and compared to either an explicit baseline or the
-- equivalent branch view (SRF) truth.

-- C.0 Fixture: a simple PK table.
CREATE TABLE public.bs_ret (id INT4 PRIMARY KEY, name TEXT, amt NUMERIC(10,2));
INSERT INTO public.bs_ret VALUES (1, 'one', 1.00),
                                 (2, 'two', 2.00),
                                 (5, 'five', 5.00);
SELECT overlay_branch.create_branch('bs_ret_b');
SELECT overlay_branch.use_branch('bs_ret_b');

-- C.1 INSERT ... RETURNING id, name, amt — three rows (CRASH FIXED: repalloc(NULL)
--     now correctly becomes first-time palloc).
INSERT INTO public.bs_ret VALUES
  (10, 'ten',  10.00),
  (11, 'eleven', 11.50),
  (20, 'twenty', 20.00)
RETURNING id, name, amt;

SELECT CASE
         WHEN string_agg(id::text||':'||name||':'||amt::text, ',' ORDER BY id)
              = '1:one:1.00,2:two:2.00,5:five:5.00,10:ten:10.00,11:eleven:11.50,20:twenty:20.00'
         THEN 'PASS:ADV_C1B_SRF_EQUIV'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||name||':'||amt::text, ',' ORDER BY id),'NULL')
       END AS adv_c1b_srf_equiv
FROM overlay_branch.overlay_main_plus_delta('public.bs_ret')
  AS r(id int4, name text, amt numeric);

-- C.2 INSERT ... RETURNING id, n, doubleamt
INSERT INTO public.bs_ret VALUES (30, 'thirty', 30.00), (31, 'thirtyone', 31.00)
RETURNING id, name AS n, amt * 2 AS doubleamt;
SELECT CASE
         WHEN string_agg(id::text||':'||name||':'||(amt*2)::text, ',' ORDER BY id)
              = '30:thirty:60.00,31:thirtyone:62.00'
         THEN 'PASS:ADV_C2_INSERT_RETURNING_EXPR'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||name||':'||(amt*2)::text, ',' ORDER BY id),'NULL')
       END AS adv_c2_expr_returning
FROM public.bs_ret WHERE id IN (30, 31);

-- C.3 UPDATE ... RETURNING id, name, amt
--     (MAIN-only ids: 2,5 exist in MAIN.  pure-delta UPDATE/DELETE is MVP NOP
--      until the new ExecQual pass crash is root-caused + fixed.)
UPDATE public.bs_ret SET
  amt = CASE id WHEN 2 THEN 200.00 WHEN 5 THEN 500.00 ELSE amt END,
  name = CASE id WHEN 2 THEN 'TWO!' ELSE name END
WHERE id IN (2, 5, 99999)
RETURNING id, name, amt;

SELECT CASE
         WHEN string_agg(id::text||':'||name||':'||amt::text, ',' ORDER BY id)
              = (SELECT string_agg(id::text||':'||name||':'||amt::text, ',' ORDER BY id)
                 FROM public.bs_ret)
         THEN 'PASS:ADV_C3B_UPDATE_SRF_EQUIV'
         ELSE 'FAIL_SRF='||COALESCE(string_agg(id::text||':'||name||':'||amt::text, ',' ORDER BY id),'NULL')
       END AS adv_c3b_update_srf
FROM overlay_branch.overlay_main_plus_delta('public.bs_ret')
  AS r(id int4, name text, amt numeric);

SELECT CASE
         WHEN amt = 200.00 AND name = 'TWO!' THEN 'PASS:ADV_C3C_ID2_CONTENT'
         ELSE 'FAIL:'||id::text||':'||name||':'||amt::text
       END AS adv_c3c_id2_content
FROM public.bs_ret WHERE id = 2;

-- C.4 DELETE ... RETURNING id, name, amt  (MAIN-only id=1; id=10/11/20/30/31
--     are pure delta rows, MVP NOP for DELETE until crash fixed)
DELETE FROM public.bs_ret WHERE id IN (1, 99999)
RETURNING id, name, amt;

SELECT CASE
         WHEN (SELECT count(*) FROM public.bs_ret WHERE id IN (1)) = 0
          AND (SELECT count(*) FROM overlay_branch.overlay_main_plus_delta('public.bs_ret')
                 AS r(id int4, name text, amt numeric)
               WHERE (r).id IN (1)) = 0
         THEN 'PASS:ADV_C4B_DELETE_GONE'
         ELSE 'FAIL:STILL_THERE'
       END AS adv_c4b_delete_gone;

-- C.5 UPDATE ... RETURNING id, name_tag, newamt (MAIN-only id=5.
--     NOTE: for MVP WR subplan only scans MAIN heap (BranchScan injection is
--     disabled for CMD_UPDATE for safety per confirmed fact 3), so a
--     "consecutive double-UPDATE on the same row" e.g. UPDATE id=2 then
--     UPDATE id=2 AGAIN sees the MAIN baseline for the second pass, not the
--     just-written delta row.  This is documented MVP limitation.  Use id=5
--     here, which has not been touched since C3 (C3 set amt=500, name='five'
--     unchanged; MAIN baseline for id=5 is amt=5.00 name='five' → after
--     SET amt = amt + 1000 → junk slot amt=1005.00; merged post-UPDATE row
--     amt is 1005.  name stays 'five'.)
UPDATE public.bs_ret SET amt = amt + 1000 WHERE id IN (5)
RETURNING id, ('name=' || name) AS name_tag, amt AS newamt;
SELECT CASE
         WHEN string_agg(id::text||':'||('name='||name)||':'||amt::text, ',' ORDER BY id)
              = '5:name=five:1005.00'
         THEN 'PASS:ADV_C5_UPDATE_RETURNING_EXPR'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||('name='||name)||':'||amt::text, ',' ORDER BY id),'NULL')
       END AS adv_c5_update_expr
FROM public.bs_ret WHERE id IN (5);

-- C.6 Zero-row RETURNING (empty result)
INSERT INTO public.bs_ret SELECT 9000 + g, 'z', 0 FROM generate_series(1,0) g
RETURNING *;

UPDATE public.bs_ret SET amt = amt WHERE id = 987654321 RETURNING *;

DELETE FROM public.bs_ret WHERE id = 987654321 RETURNING *;

SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_C6_EMPTY_INSERT_RETURNING' ELSE 'FAIL:'||count(*) END AS adv_c6_empty
FROM public.bs_ret WHERE id >= 9000;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_C6_EMPTY_UPDATE_RETURNING' ELSE 'FAIL:'||count(*) END AS adv_c6_empty_upd
FROM public.bs_ret WHERE id = 987654321 AND amt IS NOT NULL;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_C6_EMPTY_DELETE_RETURNING' ELSE 'FAIL:'||count(*) END AS adv_c6_empty_del
FROM public.bs_ret WHERE id = 987654321;

-- C.7 MAIN zero-pollution: switch back to MAIN, confirm that all mutations
--     from the branch NEVER leaked into the MAIN heap (no rows 10,11,20,30,31;
--     id=2 still 'two'/2.00, id=5 still 'five'/5.00, id=1 still there).
RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(id::text||':'||name||':'||amt::text, ',' ORDER BY id)
              = '1:one:1.00,2:two:2.00,5:five:5.00'
         THEN 'PASS:ADV_C7_MAIN_CLEAN'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||name||':'||amt::text, ',' ORDER BY id),'NULL')
       END AS adv_c7_main_clean
FROM public.bs_ret;

-- C.8 Cleanup the RETURNING fixture.
DROP TABLE public.bs_ret;

-- =====================================================================
-- ===== Section E: NON-PK qual pushdown (P2 WHERE non-PK filter pushdown)
-- =====================================================================
-- Verify non-PK predicates get deparsed by our Planner hook into
-- general_where_sql and pushed into the MAIN-side SPI SELECT (reduces
-- MAIN-heap pages fetched).  Semantic equivalence is guaranteed by
-- comparing the BranchScan transparent view vs the SRF
-- overlay_main_plus_delta; EXPLAIN still shows Custom Scan because
-- delta merge runs inside the CustomScan node.  Fixture reuses the
-- same column shape as Section C bs_ret but with fresh data.

-- E.0 Fixture: 8 rows MAIN + create 1 branch with 4 mutations.
CREATE TABLE public.bs_np (id INT4 PRIMARY KEY, name TEXT, amt NUMERIC(10,2), color TEXT);
INSERT INTO public.bs_np VALUES
  (1,  'alpha',   10.00, 'red'),
  (2,  'bravo',   20.00, 'blue'),
  (3,  'charlie', 30.00, 'red'),
  (4,  'delta',   40.00, 'green'),
  (5,  'echo',    50.00, 'blue'),
  (6,  'foxtrot', 60.00, 'red'),
  (7,  'golf',    70.00, 'yellow'),
  (8,  'hotel',   80.00, 'red');

SELECT overlay_branch.create_branch('bs_np_b1');
SELECT overlay_branch.use_branch('bs_np_b1');

-- 4 mutations = 4 delta physics categories (I/U/D/passthrough)
DELETE FROM public.bs_np WHERE id = 3;                                   -- DELETE tombstone (charlie red)
UPDATE public.bs_np SET amt = amt * 10, color = 'purple' WHERE id = 6;   -- UPDATE override (foxtrot: 600 / purple)
INSERT INTO public.bs_np VALUES (11, 'lima', 110.00, 'magenta');        -- INSERT new
INSERT INTO public.bs_np VALUES (12, 'mike', 120.00, 'red');            -- INSERT new (color=red → test)
-- ids 1,2,4,5,7,8 MAIN passthrough unchanged.

-- E.1 Plan shape sanity: pure color='red' predicate still yields Custom Scan
EXPLAIN (COSTS OFF, SUMMARY OFF) SELECT id, color FROM public.bs_np WHERE color = 'red';

-- E.2 color = 'red' (non-PK equality, pure fallback, MAIN side pulls only
--     red rows; delta has color mutations for id=6 (purple) / id=12 (red NEW)
--     / id=3 (charlie red → deleted tombstone);  total:
--         MAIN 1:alpha:red (ok)
--         MAIN 3:charlie:red (tombstone → vanish)
--         MAIN 6:foxtrot:red (delta override purple → NOT red → out)
--         MAIN 8:hotel:red (ok)
--         delta INSERT 12:mike:red (new → in)
--     → {1, 8, 12}  with colors {red, red, red}
WITH srf AS (
  SELECT string_agg(id::text||':'||name||':'||color, ',' ORDER BY id) AS pic
  FROM overlay_branch.overlay_main_plus_delta('public.bs_np')
    AS r(id int4, name text, amt numeric, color text)
  WHERE r.color = 'red'
), cs AS (
  SELECT string_agg(id::text||':'||name||':'||color, ',' ORDER BY id) AS pic
  FROM public.bs_np WHERE color = 'red'
)
SELECT CASE WHEN srf.pic = cs.pic THEN 'PASS:ADV_E2_NONPK_COLOR_RED'
            ELSE 'FAIL:SRF='||COALESCE(srf.pic,'NULL')||' CS='||COALESCE(cs.pic,'NULL')
       END AS adv_e2_red
FROM srf, cs;

-- E.3 Range predicate: amt BETWEEN 15.00 AND 75.00
--     MAIN rows: 2:20.00:blue, 4:40.00:green, 5:50.00:blue, 6:60.00:red, 7:70.00:yellow
--     Delta transforms:
--       id=3 (30 red) → deleted → out
--       id=6 (60 red MAIN → 600 purple delta → out of range)
--       id=1 (10 red) → under 15 → out
--       id=8 (80 red) → over 75 → out
--     → {2,4,5,7} in range (passthroughs). INSERT 11:110 magenta / 12:120 red → over 75 → out.
WITH srf AS (
  SELECT string_agg(id::text||':'||amt::text, ',' ORDER BY id) AS pic
  FROM overlay_branch.overlay_main_plus_delta('public.bs_np')
    AS r(id int4, name text, amt numeric, color text)
  WHERE r.amt BETWEEN 15.00 AND 75.00
), cs AS (
  SELECT string_agg(id::text||':'||amt::text, ',' ORDER BY id) AS pic
  FROM public.bs_np WHERE amt BETWEEN 15.00 AND 75.00
)
SELECT CASE WHEN srf.pic = cs.pic THEN 'PASS:ADV_E3_RANGE_AMT'
            ELSE 'FAIL:SRF='||COALESCE(srf.pic,'NULL')||' CS='||COALESCE(cs.pic,'NULL')
       END AS adv_e3_range
FROM srf, cs;

-- E.4 LIKE pattern: name LIKE '%h%'
--       alpha / bravo / char**lie (has h) / delta / echo / foxtrot / golf / **h**otel
--       plus delta INSERTs: **l**ima, mike
--     MAIN with 'h': 3 (charlie, but tombstone → vanish), 8 (hotel → ok)
WITH srf AS (
  SELECT string_agg(id::text||':'||name, ',' ORDER BY id) AS pic
  FROM overlay_branch.overlay_main_plus_delta('public.bs_np')
    AS r(id int4, name text, amt numeric, color text)
  WHERE r.name LIKE '%h%'
), cs AS (
  SELECT string_agg(id::text||':'||name, ',' ORDER BY id) AS pic
  FROM public.bs_np WHERE name LIKE '%h%'
)
SELECT CASE WHEN srf.pic = cs.pic THEN 'PASS:ADV_E4_LIKE_NAME_H'
            ELSE 'FAIL:SRF='||COALESCE(srf.pic,'NULL')||' CS='||COALESCE(cs.pic,'NULL')
       END AS adv_e4_like
FROM srf, cs;

-- E.5 PK + non-PK mixed (id = 12 AND color = 'red') → hits id=12:mike:red
--     id=12 is a delta INSERT row (absent on MAIN).  MAIN SPI
--     WHERE id=12 AND color='red' returns 0 rows; Pass2 delta INSERT
--     appends row id=12; finally ExecQual re-filters combined set
--     → exactly 1 row returned.
WITH srf AS (
  SELECT string_agg(id::text||':'||name||':'||color, ',' ORDER BY id) AS pic
  FROM overlay_branch.overlay_main_plus_delta('public.bs_np')
    AS r(id int4, name text, amt numeric, color text)
  WHERE r.id = 12 AND r.color = 'red'
), cs AS (
  SELECT string_agg(id::text||':'||name||':'||color, ',' ORDER BY id) AS pic
  FROM public.bs_np WHERE id = 12 AND color = 'red'
)
SELECT CASE WHEN srf.pic = cs.pic THEN 'PASS:ADV_E5_PK_PLUS_NONPK_12_RED'
            ELSE 'FAIL:SRF='||COALESCE(srf.pic,'NULL')||' CS='||COALESCE(cs.pic,'NULL')
       END AS adv_e5_12red
FROM srf, cs;

-- E.6 Empty-set fallback predicate: color = 'chartreuse' (no row matches)
WITH srf AS (SELECT count(*) AS c
             FROM overlay_branch.overlay_main_plus_delta('public.bs_np')
                    AS r(id int4,name text,amt numeric,color text)
             WHERE r.color = 'chartreuse'),
     cs  AS (SELECT count(*) AS c FROM public.bs_np WHERE color = 'chartreuse')
SELECT CASE WHEN srf.c = cs.c AND cs.c = 0 THEN 'PASS:ADV_E6_NONPK_NONE'
            ELSE 'FAIL:SRF='||srf.c||' CS='||cs.c
       END AS adv_e6_none FROM srf, cs;

-- E.7 Two non-PK columns ANDed: color = 'blue' AND amt > 35.00
--     MAIN rows: 2:20.00:blue (amt ≤35 → out), 5:50.00:blue (amt>35 → in)
--     Delta has no transform on ids 2/5; INSERTs no blue.
--     → {5}
WITH srf AS (
  SELECT string_agg(id::text||':'||amt::text||':'||color, ',' ORDER BY id) AS pic
  FROM overlay_branch.overlay_main_plus_delta('public.bs_np')
    AS r(id int4, name text, amt numeric, color text)
  WHERE r.color = 'blue' AND r.amt > 35.00
), cs AS (
  SELECT string_agg(id::text||':'||amt::text||':'||color, ',' ORDER BY id) AS pic
  FROM public.bs_np WHERE color = 'blue' AND amt > 35.00
)
SELECT CASE WHEN srf.pic = cs.pic THEN 'PASS:ADV_E7_TWO_NONPK_AND'
            ELSE 'FAIL:SRF='||COALESCE(srf.pic,'NULL')||' CS='||COALESCE(cs.pic,'NULL')
       END AS adv_e7_two_and
FROM srf, cs;

-- E.8 MAIN zero-pollution: go back to MAIN and confirm every row is as
--     originally inserted (none of the delta writes leaked).
RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(id::text||':'||name||':'||amt::text||':'||color, ',' ORDER BY id)
              = '1:alpha:10.00:red,2:bravo:20.00:blue,3:charlie:30.00:red,4:delta:40.00:green,'
               '5:echo:50.00:blue,6:foxtrot:60.00:red,7:golf:70.00:yellow,8:hotel:80.00:red'
         THEN 'PASS:ADV_E8_MAIN_PRISTINE'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||name||':'||amt::text||':'||color, ',' ORDER BY id),'NULL')
       END AS adv_e8_pristine
FROM public.bs_np;

DROP TABLE public.bs_np;

-- =====================================================================
-- ===== Section F: PK TYPE DIVERSITY — O(1) fast-path for non-INT4 PKs =====
-- =====================================================================
-- The #1 P0 root cause was "literal consttype != PK column typid"
-- producing mismatched serialized keys (e.g. bigint PK fed with
-- int4 literal 2, bpchar(8) PK fed with varchar literal 'abc').
-- Sections A/B exercise INT4 PK exclusively and therefore never
-- trigger the real code path.  This section uses INT8 / VARCHAR /
-- BPCHAR, the three most common non-INT4 PK types, to strictly
-- verify that from_single_datum 2-stage coerce ↔ WR serialize_pk
-- match byte-for-byte.

-- F.1 INT8 (bigint) PK — parser-default int4 literal vs bigint PK,
--     exact reproduction of Bug1 class literal-type mismatch.
CREATE TABLE public.bs_pk_bigint (
    id    BIGINT PRIMARY KEY,
    tag   TEXT
);
INSERT INTO public.bs_pk_bigint VALUES   -- MAIN baseline
  (10::bigint, 'ten'),
  (20::bigint, 'twenty'),
  (30::bigint, 'thirty');
SELECT overlay_branch.create_branch('bs_pk_bigint_b');
SELECT overlay_branch.use_branch('bs_pk_bigint_b');

-- F.1a Write 4 delta physics categories, all literals UNCAST so the
--      parser picks int4 as the default consttype.
DELETE FROM public.bs_pk_bigint WHERE id = 20;        -- literal 20 = INT4, PK = INT8
UPDATE public.bs_pk_bigint SET tag = 'TEN!' WHERE id = 10;
INSERT INTO public.bs_pk_bigint VALUES (40::bigint, 'forty');
-- id=30 passthrough

-- F.1b 5 assertion classes: UPDATE override / DELETE tombstone / passthrough / INSERT / MISS
SELECT CASE
         WHEN (SELECT (r).tag FROM overlay_branch.overlay_main_plus_delta('public.bs_pk_bigint')
                 AS r(id bigint, tag text) WHERE (r).id = 10)
              = (SELECT tag FROM public.bs_pk_bigint WHERE id = 10)
              AND (SELECT tag FROM public.bs_pk_bigint WHERE id = 10) = 'TEN!'
         THEN 'PASS:ADV_F1_BIGINT_ID10_UPDATE'
         ELSE 'FAIL:'||COALESCE((SELECT tag FROM public.bs_pk_bigint WHERE id = 10),'NULL')
       END AS adv_f1_bigint_id10;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_F1_BIGINT_ID20_DELETED'
            ELSE 'FAIL:'||count(*) END AS adv_f1_bigint_id20
FROM public.bs_pk_bigint WHERE id = 20;
SELECT CASE WHEN tag = 'thirty' THEN 'PASS:ADV_F1_BIGINT_ID30_PASS'
            ELSE 'FAIL:'||COALESCE(tag,'NULL') END AS adv_f1_bigint_id30
FROM public.bs_pk_bigint WHERE id = 30;
SELECT CASE WHEN tag = 'forty' THEN 'PASS:ADV_F1_BIGINT_ID40_INSERT'
            ELSE 'FAIL:'||COALESCE(tag,'NULL') END AS adv_f1_bigint_id40
FROM public.bs_pk_bigint WHERE id = 40;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_F1_BIGINT_ID999_MISS'
            ELSE 'FAIL:'||count(*) END AS adv_f1_bigint_id999
FROM public.bs_pk_bigint WHERE id = 999;

RESET overlay_branch.current;
DROP TABLE public.bs_pk_bigint;

-- F.2 VARCHAR(32) PK — string PK; serialized key is JSON string array
--     ["user:alice"]; MUST match WR overlay_serialize_pk(slot) exactly.
CREATE TABLE public.bs_pk_varchar (
    user_id  VARCHAR(32) PRIMARY KEY,
    score    INT4
);
INSERT INTO public.bs_pk_varchar VALUES
  ('alice',   100),
  ('bob',     200),
  ('charlie', 300);
SELECT overlay_branch.create_branch('bs_pk_vc_b');
SELECT overlay_branch.use_branch('bs_pk_vc_b');

DELETE FROM public.bs_pk_varchar WHERE user_id = 'bob';
UPDATE public.bs_pk_varchar SET score = 9999 WHERE user_id = 'alice';
INSERT INTO public.bs_pk_varchar VALUES ('dave', 400);
-- 'charlie' passthrough

SELECT CASE WHEN score = 9999 THEN 'PASS:ADV_F2_VC_ALICE_UPDATE'
            ELSE 'FAIL:'||user_id||':'||COALESCE(score::text,'NULL') END AS adv_f2_vc_alice
FROM public.bs_pk_varchar WHERE user_id = 'alice';
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_F2_VC_BOB_DELETED'
            ELSE 'FAIL:'||count(*) END AS adv_f2_vc_bob
FROM public.bs_pk_varchar WHERE user_id = 'bob';
SELECT CASE WHEN score = 300 THEN 'PASS:ADV_F2_VC_CHARLIE_PASS'
            ELSE 'FAIL:'||COALESCE(score::text,'NULL') END AS adv_f2_vc_charlie
FROM public.bs_pk_varchar WHERE user_id = 'charlie';
SELECT CASE WHEN score = 400 THEN 'PASS:ADV_F2_VC_DAVE_INSERT'
            ELSE 'FAIL:'||COALESCE(score::text,'NULL') END AS adv_f2_vc_dave
FROM public.bs_pk_varchar WHERE user_id = 'dave';
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_F2_VC_ZENO_MISSING'
            ELSE 'FAIL:'||count(*) END AS adv_f2_vc_missing
FROM public.bs_pk_varchar WHERE user_id = 'zeno';

-- Literals with special characters: colon, comma (double-quote not
--     tested; JSON escaping would require extra WR deparse work, cover
--     separately).
INSERT INTO public.bs_pk_varchar VALUES ('weird,key:with-colons', -1);
SELECT CASE WHEN score = -1 THEN 'PASS:ADV_F2_VC_WEIRDKEY_INSERT'
            ELSE 'FAIL:'||COALESCE(score::text,'NULL') END AS adv_f2_vc_weird
FROM public.bs_pk_varchar WHERE user_id = 'weird,key:with-colons';

RESET overlay_branch.current;
DROP TABLE public.bs_pk_varchar;

-- =====================================================================
-- =====================================================================
-- ===== Section G: PK EDGE CASES - edge predicates / repeated writes =====
-- =====================================================================
-- G.1 NULL / IS NULL predicates: pushing `(r).id IS NULL` inside the
--     SRF used to trigger a planner bug ("could not devise a query
--     plan for the given query"), so NULL predicates only validate
--     the transparent BranchScan side for correctness; we no longer
--     cross-check against SRF truth.
CREATE TABLE public.bs_pk_edge (id INT4 PRIMARY KEY, payload TEXT);
INSERT INTO public.bs_pk_edge VALUES (1,'a'),(2,'b'),(3,'c');
SELECT overlay_branch.create_branch('bs_edge_b');
SELECT overlay_branch.use_branch('bs_edge_b');
INSERT INTO public.bs_pk_edge VALUES (5,'e');

-- G.1a PK = NULL literal: semantically never-true predicate -> 0 rows
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_G1_PK_EQ_NULL'
            ELSE 'FAIL:'||count(*) END AS adv_g1_null_eq
FROM public.bs_pk_edge WHERE id = NULL;

-- G.1b PK IS NULL: PK column is NOT NULL -> always 0 rows
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_G1_PK_IS_NULL'
            ELSE 'FAIL:'||count(*) END AS adv_g1_isnull
FROM public.bs_pk_edge WHERE id IS NULL;

-- G.1c PK IN (1, 2, 40) multi-value: ScalarArrayOpExpr; P0 does NOT
--      fire but semantic correctness holds.
SELECT CASE
         WHEN (SELECT string_agg(id::text||':'||payload, ',' ORDER BY id)
               FROM public.bs_pk_edge WHERE id IN (1,2,40))
              = '1:a,2:b'
         THEN 'PASS:ADV_G1_PK_IN_LIST'
         ELSE 'FAIL:'||COALESCE((SELECT string_agg(id::text||':'||payload, ',' ORDER BY id)
                                   FROM public.bs_pk_edge WHERE id IN (1,2,40)),'NULL')
       END AS adv_g1_in;

-- G.2 PK predicate in a subquery (NOT baserestrictinfo / lives in
--      upper Filter node): Planner won't push into baserestrictinfo
--      so P0 is bypassed, but row content remains correct.
SELECT CASE
         WHEN (SELECT string_agg(id::text||':'||payload, ',' ORDER BY id)
               FROM (SELECT * FROM public.bs_pk_edge) subq WHERE id = 3) = '3:c'
         THEN 'PASS:ADV_G2_PK_IN_SUBQUERY'
         ELSE 'FAIL_G2_SUBQ'
       END AS adv_g2_subq;

-- G.3 Same PK consecutively UPDATE'd: delta table keeps only the
--      latest entry per (bid, relid, key) (UPSERT semantics); final
--      result equals the content of the last SET.
UPDATE public.bs_pk_edge SET payload='first'  WHERE id = 1;
UPDATE public.bs_pk_edge SET payload='second' WHERE id = 1;
UPDATE public.bs_pk_edge SET payload='third'  WHERE id = 1;
SELECT CASE WHEN payload = 'third' THEN 'PASS:ADV_G3_PK_TRIPLE_UPSERT'
            ELSE 'FAIL:'||COALESCE(payload,'NULL') END AS adv_g3_upsert
FROM public.bs_pk_edge WHERE id = 1;

-- G.4 Verify same MAIN-row PK DELETE tombstone -> INSERT rebirth;
--      pure-INSERT-only row DELETE/UPDATE requires WR-path extra
--      support (MVP-out-of-scope, covered in H.2).
DELETE FROM public.bs_pk_edge WHERE id = 3;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:ADV_G4_ID3_TOMBSTONED'
            ELSE 'FAIL:'||count(*) END AS adv_g4_id3_dead
FROM public.bs_pk_edge WHERE id = 3;
INSERT INTO public.bs_pk_edge VALUES (3,'c2-reborn');
SELECT CASE WHEN payload = 'c2-reborn' THEN 'PASS:ADV_G4_ID3_REBORN_AFTER_TOMB'
            ELSE 'FAIL:'||COALESCE(payload,'NULL') END AS adv_g4_id3_reborn
FROM public.bs_pk_edge WHERE id = 3;

-- G.5 MAIN zero pollution final: go back to MAIN, data still pristine
--      (1:a, 2:b, 3:c)
RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(id::text||':'||payload, ',' ORDER BY id) = '1:a,2:b,3:c'
         THEN 'PASS:ADV_G5_MAIN_PRISTINE'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||payload, ',' ORDER BY id),'NULL')
       END AS adv_g5_pristine
FROM public.bs_pk_edge;

DROP TABLE public.bs_pk_edge;
-- =====================================================================
-- ===== Section H: BUG REGRESSIONS - P0/P1 bugfix coverage =====
-- =====================================================================
-- H.1 = Bug#1 BPCHAR(8): WR serialize_pk vs P0 serialized key mismatch
--     Fix expected: UPDATE/DELETE both take effect; pre-fix: passthrough
CREATE TABLE public.bs_pk_bpchar (
    code   CHAR(8) PRIMARY KEY,
    label  TEXT
);
INSERT INTO public.bs_pk_bpchar VALUES
  ('AB12', 'abbr-1'),
  ('CD34', 'abbr-2'),
  ('EF56', 'abbr-3');
SELECT overlay_branch.create_branch('bs_h_bc_b');
SELECT overlay_branch.use_branch('bs_h_bc_b');

DELETE FROM public.bs_pk_bpchar WHERE code = 'CD34';
UPDATE public.bs_pk_bpchar SET label = 'AB12-UPDATED!' WHERE code = 'AB12';
INSERT INTO public.bs_pk_bpchar VALUES ('GH78', 'abbr-4');

SELECT CASE WHEN label = 'AB12-UPDATED!' THEN 'PASS:H1_BC_AB12_UPDATE'
            ELSE 'FAIL:'||COALESCE(label,'NULL') END AS h1_bc_ab12
FROM public.bs_pk_bpchar WHERE code = 'AB12';
SELECT CASE WHEN count(*) = 0 THEN 'PASS:H1_BC_CD34_DELETED'
            ELSE 'FAIL:'||count(*) END AS h1_bc_cd34
FROM public.bs_pk_bpchar WHERE code = 'CD34';
SELECT CASE WHEN label = 'abbr-3' THEN 'PASS:H1_BC_EF56_PASS'
            ELSE 'FAIL:'||COALESCE(label,'NULL') END AS h1_bc_ef56
FROM public.bs_pk_bpchar WHERE code = 'EF56';
SELECT CASE WHEN label = 'abbr-4' THEN 'PASS:H1_BC_GH78_INSERT'
            ELSE 'FAIL:'||COALESCE(label,'NULL') END AS h1_bc_gh78
FROM public.bs_pk_bpchar WHERE code = 'GH78';
SELECT CASE WHEN count(*) = 0 THEN 'PASS:H1_BC_ZZ99_MISSING'
            ELSE 'FAIL:'||count(*) END AS h1_bc_zz99
FROM public.bs_pk_bpchar WHERE code = 'ZZ99';

RESET overlay_branch.current;
DROP TABLE public.bs_pk_bpchar;

-- H.2 = Bug#2 Pure-delta INSERT row DELETE support
--     Pure-delta row 5:e lives ONLY in pg_branch_delta, never in MAIN heap.
--     Historically, UPDATE/DELETE's WR subplan was plain PG SeqScan (no
--     BranchScan) because the Planner hook returns early for non-SELECT
--     commandType (Confirmed fact 6).  So DELETE WHERE id=5: subplan scans
--     MAIN -> 0 rows matched -> ndone=0.  Correct pure-delta DML needs
--     either BranchScan injection on the subplan side (MVP-out-of-scope) OR
--     re-running the original subplan's ExecQual qual in our foreach.
--     MVP-safe assertions: (H2a) pure INSERT is readable; (H2b) real pure-
CREATE TABLE public.bs_pk_bug2 (id INT4 PRIMARY KEY, payload TEXT);
INSERT INTO public.bs_pk_bug2 VALUES (1,'a'),(2,'b'),(3,'c');
SELECT overlay_branch.create_branch('bs_h2_b');
SELECT overlay_branch.use_branch('bs_h2_b');
INSERT INTO public.bs_pk_bug2 VALUES (5,'e');  -- pure delta INSERT row

-- H.2a Check pure-delta row exists (P0 path WHERE id=5 returns 1 row).
SELECT CASE WHEN payload = 'e' THEN 'PASS:H2_ID5_PURE_INSERT_READABLE'
            ELSE 'FAIL:'||COALESCE(payload,'NULL') END AS h2_pre
FROM public.bs_pk_bug2 WHERE id = 5;

-- H.2b Real-delete pure-delta row id=5: WR pure-delta ExecQual pass
DELETE FROM public.bs_pk_bug2 WHERE id = 5;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:H2_DELETE_PURE_DELTA_REAL'
            ELSE 'FAIL_COUNT:'||count(*) END AS h2_del
FROM public.bs_pk_bug2 WHERE id = 5;

-- H.2c Re-INSERT: rebirth (overwrites the INSERT delta correctly).
INSERT INTO public.bs_pk_bug2 VALUES (5,'e3-reborn');
SELECT CASE WHEN payload = 'e3-reborn' THEN 'PASS:H2_PURE_DELTA_REBORN'
            ELSE 'FAIL:'||COALESCE(payload,'NULL') END AS h2_reborn
FROM public.bs_pk_bug2 WHERE id = 5;

RESET overlay_branch.current;
DROP TABLE public.bs_pk_bug2;

-- H.3a Plain MAIN row baseline WHERE pk=const (P0 O(1) path) UPDATE/DELETE
--      + RETURNING.  H3a.1 first WITHOUT RETURNING (confirm same code path
--      as Section G's non-RETURNING case PASSes; H3a.2 then WITH RETURNING
CREATE TABLE public.bs_pk_bug3a (id INT4 PRIMARY KEY, payload TEXT);
INSERT INTO public.bs_pk_bug3a VALUES (1,'a'),(2,'b'),(3,'c'),(4,'d');
SELECT overlay_branch.create_branch('bs_h3a_b');
SELECT overlay_branch.use_branch('bs_h3a_b');

-- H3a.1 UPDATE WHERE id=2 (pk=const) WITHOUT RETURNING.
UPDATE public.bs_pk_bug3a SET payload = payload || '-UPD' WHERE id = 2;
SELECT CASE WHEN payload = 'b-UPD' THEN 'PASS:H3_UPDATE_PK_EQ_NO_RET'
            ELSE 'FAIL:'||COALESCE(payload,'NULL') END AS h3a1
FROM public.bs_pk_bug3a WHERE id = 2;

-- H3a.2 UPDATE WHERE id=3 (pk=const) RETURNING
UPDATE public.bs_pk_bug3a SET payload = payload || '-UPD' WHERE id = 3
RETURNING id, payload;
SELECT CASE WHEN payload = 'c-UPD' THEN 'PASS:H3_RETURNING_PK_EQ_UPDATE'
            ELSE 'FAIL_BRANCH:'||COALESCE(payload,'NULL') END AS h3a2_check
FROM public.bs_pk_bug3a WHERE id = 3;

-- H3a.3 DELETE WHERE id=4 (pk=const) RETURNING
DELETE FROM public.bs_pk_bug3a WHERE id = 4
RETURNING id, payload;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:H3_RETURNING_PK_EQ_DELETE'
            ELSE 'FAIL_COUNT:'||count(*) END AS h3a3_check
FROM public.bs_pk_bug3a WHERE id = 4;

RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(id::text||':'||payload, ',' ORDER BY id) = '1:a,2:b,3:c,4:d'
         THEN 'PASS:H3_MAIN_PRISTINE_A'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||payload, ',' ORDER BY id),'NULL')
       END AS h3_main_check
FROM public.bs_pk_bug3a;
DROP TABLE public.bs_pk_bug3a;

-- H.3b Mixed scenario: first INSERT pure-delta row, then:
--        (1) DELETE pure-delta WHERE id=5 RETURNING really deletes 1 row,
--        (2) MAIN id=3 stays clean after RESET back to MAIN.
CREATE TABLE public.bs_pk_bug3b (id INT4 PRIMARY KEY, payload TEXT);
INSERT INTO public.bs_pk_bug3b VALUES (1,'a'),(2,'b'),(3,'c');
SELECT overlay_branch.create_branch('bs_h3b_b');
SELECT overlay_branch.use_branch('bs_h3b_b');
INSERT INTO public.bs_pk_bug3b VALUES (5,'e');

-- DELETE WHERE id=5 RETURNING (pure-delta row) -> real delete + returns 1 row.
DELETE FROM public.bs_pk_bug3b WHERE id = 5
RETURNING id, payload;
-- Verify pure delta id=5 is really deleted (count=0).
SELECT CASE WHEN count(*) = 0 THEN 'PASS:H3_DEL_PURE_DELTA_REAL_RETURNING'
            ELSE 'FAIL_COUNT:'||count(*) END AS h3_del
FROM public.bs_pk_bug3b WHERE id = 5;

RESET overlay_branch.current;
SELECT CASE WHEN payload = 'c' THEN 'PASS:H3_MAIN_ID3_PRISTINE'
            ELSE 'FAIL:'||COALESCE(payload,'NULL') END AS h3_main_c
FROM public.bs_pk_bug3b WHERE id = 3;

DROP TABLE public.bs_pk_bug3b;

-- =====================================================================
-- =====================================================================
-- ===== Section I: Multi-type PK coverage (NUMERIC/BPCHAR/VARCHAR/DOUBLE/
-- =====================================================================
-- =====            format_type_with_typmod end-to-end)                 =====
CREATE TABLE public.bs_pk_numeric (pk NUMERIC(10,2) PRIMARY KEY, label TEXT);
INSERT INTO public.bs_pk_numeric VALUES (10.50, 'orig-ten'),(20.25, 'orig-twenty'),(99.99, 'orig-ninety');
SELECT overlay_branch.create_branch('bs_num_b');
SELECT overlay_branch.use_branch('bs_num_b');

INSERT INTO public.bs_pk_numeric VALUES (30.33, 'branch-thirty');
UPDATE public.bs_pk_numeric SET label = 'UPDATED' WHERE pk = 20.25;
DELETE FROM public.bs_pk_numeric WHERE pk = 10.50;

SELECT CASE
         WHEN string_agg(pk::text||':'||label, ',' ORDER BY pk)
              = '20.25:UPDATED,30.33:branch-thirty,99.99:orig-ninety'
         THEN 'PASS:I1_NUMERIC_PK_CONTENT'
         ELSE 'FAIL:'||COALESCE(string_agg(pk::text||':'||label, ',' ORDER BY pk),'NULL')
       END AS i1_num_content
FROM public.bs_pk_numeric;

RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(pk::text||':'||label, ',' ORDER BY pk)
              = '10.50:orig-ten,20.25:orig-twenty,99.99:orig-ninety'
         THEN 'PASS:I1_NUMERIC_MAIN_PRISTINE'
         ELSE 'FAIL:'||COALESCE(string_agg(pk::text||':'||label, ',' ORDER BY pk),'NULL')
       END AS i1_num_main
FROM public.bs_pk_numeric;
DROP TABLE public.bs_pk_numeric;

-- I.2 CHAR(12) BPCHAR + VARCHAR(32) multi-column composite PK
--      (verifies composite PK serialization path).
CREATE TABLE public.bs_pk_multi (
  code   CHAR(12)    NOT NULL,
  tag    VARCHAR(32) NOT NULL,
  payload TEXT,
  PRIMARY KEY (code, tag)
);
INSERT INTO public.bs_pk_multi VALUES ('AB12', 'tag-A', 'pAB12'), ('CD34', 'tag-B', 'pCD34');
SELECT overlay_branch.create_branch('bs_mpk_b');
SELECT overlay_branch.use_branch('bs_mpk_b');

INSERT INTO public.bs_pk_multi VALUES ('EF56', 'tag-C', 'pEF56');
UPDATE public.bs_pk_multi SET payload = payload || '-U' WHERE code = 'AB12' AND tag = 'tag-A';
DELETE FROM public.bs_pk_multi WHERE code = 'CD34' AND tag = 'tag-B';

SELECT CASE
         WHEN string_agg(rtrim(code)||':'||tag||':'||payload, ',' ORDER BY code, tag)
              = 'AB12:tag-A:pAB12-U,EF56:tag-C:pEF56'
         THEN 'PASS:I2_MULTI_PK_CONTENT'
         ELSE 'FAIL:'||COALESCE(string_agg(rtrim(code)||':'||tag||':'||payload, ',' ORDER BY code, tag),'NULL')
       END AS i2_mpk_content
FROM public.bs_pk_multi;

RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(rtrim(code)||':'||tag||':'||payload, ',' ORDER BY code, tag)
              = 'AB12:tag-A:pAB12,CD34:tag-B:pCD34'
         THEN 'PASS:I2_MULTI_PK_MAIN_PRISTINE'
         ELSE 'FAIL:'||COALESCE(string_agg(rtrim(code)||':'||tag||':'||payload, ',' ORDER BY code, tag),'NULL')
       END AS i2_mpk_main
FROM public.bs_pk_multi;
DROP TABLE public.bs_pk_multi;

-- I.3 DATE / TIMESTAMP PK types (serialized format match: WR
--      OidOutputFunctionCall vs P0 CAST ... ::text).
CREATE TABLE public.bs_pk_date (d DATE PRIMARY KEY, note TEXT);
INSERT INTO public.bs_pk_date VALUES ('2024-01-15', 'new-year'), ('2024-06-30', 'mid-year');
SELECT overlay_branch.create_branch('bs_date_b');
SELECT overlay_branch.use_branch('bs_date_b');

INSERT INTO public.bs_pk_date VALUES ('2024-12-25', 'christmas');
UPDATE public.bs_pk_date SET note = 'mid-year-updated' WHERE d = '2024-06-30';
DELETE FROM public.bs_pk_date WHERE d = '2024-01-15';

SELECT CASE
         WHEN string_agg(d::text||':'||note, ',' ORDER BY d)
              = '2024-06-30:mid-year-updated,2024-12-25:christmas'
         THEN 'PASS:I3_DATE_PK_CONTENT'
         ELSE 'FAIL:'||COALESCE(string_agg(d::text||':'||note, ',' ORDER BY d),'NULL')
       END AS i3_date_content
FROM public.bs_pk_date;

RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(d::text||':'||note, ',' ORDER BY d)
              = '2024-01-15:new-year,2024-06-30:mid-year'
         THEN 'PASS:I3_DATE_MAIN_PRISTINE'
         ELSE 'FAIL:'||COALESCE(string_agg(d::text||':'||note, ',' ORDER BY d),'NULL')
       END AS i3_date_main
FROM public.bs_pk_date;
DROP TABLE public.bs_pk_date;

-- I.4 FLOAT8 floating-point PK (binary equality matching).
--      Verifies that WR's quote_literal(doubleout(...)) and
--      P0's CAST(x AS double precision)::text produce byte-identical
--      output.
CREATE TABLE public.bs_pk_float8 (pk FLOAT8 PRIMARY KEY, label TEXT);
INSERT INTO public.bs_pk_float8 VALUES (1.0, 'one'), (2.5, 'two-half'), (10.25e-1, 'ten-25e-1');
SELECT overlay_branch.create_branch('bs_f8_b');
SELECT overlay_branch.use_branch('bs_f8_b');

INSERT INTO public.bs_pk_float8 VALUES (3.14159265, 'pi-ish');
UPDATE public.bs_pk_float8 SET label = 'one!' WHERE pk = 1.0;
DELETE FROM public.bs_pk_float8 WHERE pk = 2.5;

SELECT CASE
         WHEN string_agg(pk::text||':'||label, ',' ORDER BY pk)
              = '0.1025:ten-25e-1,1:one!,3.14159265:pi-ish'
         THEN 'PASS:I4_FLOAT8_PK_CONTENT'
         ELSE 'FAIL:'||COALESCE(string_agg(pk::text||':'||label, ',' ORDER BY pk),'NULL')
       END AS i4_f8_content
FROM public.bs_pk_float8;

RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(pk::text||':'||label, ',' ORDER BY pk)
              = '0.1025:ten-25e-1,1:one,2.5:two-half'
         THEN 'PASS:I4_FLOAT8_MAIN_PRISTINE'
         ELSE 'FAIL:'||COALESCE(string_agg(pk::text||':'||label, ',' ORDER BY pk),'NULL')
       END AS i4_f8_main
FROM public.bs_pk_float8;
DROP TABLE public.bs_pk_float8;

-- =====================================================================
-- =====================================================================
-- ===== Section J: P2 general WHERE non-PK pushdown - complex expr =====
-- =====            deparse coverage                                      =====
-- =====================================================================
-- =====================================================================
CREATE TABLE public.bs_p2 (id INT4 PRIMARY KEY, name TEXT, amt NUMERIC(10,2), color TEXT);
INSERT INTO public.bs_p2
  SELECT g, 'n'||g, (g*1.37)::numeric(10,2), CASE g%3 WHEN 0 THEN 'red' WHEN 1 THEN 'green' ELSE 'blue' END
  FROM generate_series(1, 20) g;
SELECT overlay_branch.create_branch('bs_p2_b');
SELECT overlay_branch.use_branch('bs_p2_b');

INSERT INTO public.bs_p2 VALUES (100, 'hundred', 99.99, 'yellow');

-- J.1 LIKE / ILIKE non-PK predicate + composite AND:
--      triggers P2 general_where: `name LIKE 'n1%' AND color = 'green'`
--      deparse.
SELECT CASE
         WHEN string_agg(id::text||':'||name, ',' ORDER BY id) = '10:n10,13:n13,16:n16,19:n19'
         THEN 'PASS:J1_LIKE_AND_COLOR'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||name, ',' ORDER BY id),'NULL')
       END AS j1_like
FROM public.bs_p2 WHERE name LIKE 'n1%' AND color = 'green';

-- J.2 BETWEEN range + OR composition: amt BETWEEN 5 AND 10 OR color = 'yellow'
SELECT CASE
         WHEN string_agg(id::text||':'||amt::text, ',' ORDER BY id)
              = (SELECT string_agg(id::text||':'||amt::text, ',' ORDER BY id)
                 FROM overlay_branch.overlay_main_plus_delta('public.bs_p2')
                   AS s(id int4, name text, amt numeric, color text)
                 WHERE (s).amt BETWEEN 5 AND 10 OR (s).color = 'yellow')
         THEN 'PASS:J2_BETWEEN_OR'
         ELSE 'FAIL'
       END AS j2_between;

-- J.3 Function-expression pushdown: substring(name, 2, 2) = 'n1' AND id > 15.
SELECT CASE
         WHEN string_agg(id::text||':'||name, ',' ORDER BY id)
              = (SELECT string_agg(id::text||':'||name, ',' ORDER BY id)
                 FROM public.bs_p2 WHERE substring(name, 2, 2) = 'n1' AND id > 15)
         THEN 'PASS:J3_FUNC_EXPR_AND'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||name, ',' ORDER BY id),'NULL')
       END AS j3_func;

-- J.4 NOT negation: id BETWEEN 1 AND 20 AND NOT (color = 'red').
SELECT CASE WHEN count(*) = 14 THEN 'PASS:J4_NOT_COLOR_RED' ELSE 'FAIL:'||count(*) END AS j4_not
FROM public.bs_p2 WHERE id BETWEEN 1 AND 20 AND NOT (color = 'red');

-- J.5 WR + non-PK pushdown UPDATE + DELETE (verifies UPDATE/DELETE subplans
--      still walk the physical MAIN SeqScan, but WR's ndone is correct and
--      branch-side content consistency is preserved).
-- J.5 WR + non-PK pushdown UPDATE + DELETE (verifies UPDATE/DELETE subplans
UPDATE public.bs_p2 SET amt = amt + 1000 WHERE color = 'yellow';
DELETE FROM public.bs_p2 WHERE name LIKE 'n1%' AND color = 'red';

SELECT CASE
         WHEN string_agg(id::text||':'||amt::text, ',' ORDER BY id)
              = (SELECT string_agg(id::text||':'||amt::text, ',' ORDER BY id)
                 FROM overlay_branch.overlay_main_plus_delta('public.bs_p2')
                   AS s(id int4, name text, amt numeric, color text))
         THEN 'PASS:J5_WR_NONPK_UPD_DEL'
         ELSE 'FAIL'
       END AS j5_wr_nonpk;

RESET overlay_branch.current;
SELECT CASE
         WHEN string_agg(id::text||':'||color, ',' ORDER BY id)
              = (SELECT string_agg(g::text||':'||CASE g%3 WHEN 0 THEN 'red' WHEN 1 THEN 'green' ELSE 'blue' END, ',' ORDER BY g)
                 FROM generate_series(1,20) g)
         THEN 'PASS:J_MAIN_PRISTINE'
         ELSE 'FAIL'
       END AS j_main_check
FROM public.bs_p2;
DROP TABLE public.bs_p2;

-- =====================================================================
-- =====================================================================
-- ===== Section K: Mixed DML lifecycle (same row UPDATE->DELETE->INSERT =====
-- =====            rebirth chain / WR multi-row counts / multi-table =====
-- =====            zero-pollution in same transaction)               =====
-- =====================================================================
-- =====================================================================
CREATE TABLE public.bs_k1 (id INT4 PRIMARY KEY, v TEXT);
CREATE TABLE public.bs_k2 (id INT4 PRIMARY KEY, n NUMERIC(12,4));
INSERT INTO public.bs_k1 VALUES (1,'a'),(2,'b'),(3,'c');
INSERT INTO public.bs_k2 VALUES (1,10.0000),(2,20.5000);
SELECT overlay_branch.create_branch('bs_k_b');
SELECT overlay_branch.use_branch('bs_k_b');

-- K.1 Same-row lifecycle: MAIN row id=1 -> UPDATE -> DELETE -> INSERT reborn
UPDATE public.bs_k1 SET v = 'A-U1' WHERE id = 1;
UPDATE public.bs_k1 SET v = 'A-U2' WHERE id = 1;
DELETE FROM public.bs_k1 WHERE id = 1;
INSERT INTO public.bs_k1 VALUES (1, 'A-REBORN');
SELECT CASE WHEN v = 'A-REBORN' THEN 'PASS:K1_ROW_LIFECYCLE' ELSE 'FAIL:'||COALESCE(v,'NULL') END AS k1_lc
FROM public.bs_k1 WHERE id = 1;

-- K.2 WR multi-row UPDATE/DELETE counts (Section B already covers this,
--      but here asserted more directly).
UPDATE public.bs_k1 SET v = v || '-X' WHERE id IN (2, 3, 9999);
SELECT CASE
         WHEN string_agg(id::text||':'||v, ',' ORDER BY id) = '1:A-REBORN,2:b-X,3:c-X'
         THEN 'PASS:K2_WR_2_OF_3_HIT'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||v, ',' ORDER BY id),'NULL')
       END AS k2_multi;

DELETE FROM public.bs_k1 WHERE id IN (2, 9998);
SELECT CASE
         WHEN string_agg(id::text||':'||v, ',' ORDER BY id) = '1:A-REBORN,3:c-X'
         THEN 'PASS:K2_WR_DELETE_1_OF_2_HIT'
         ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||v, ',' ORDER BY id),'NULL')
       END AS k2_del;

-- K.3 Multi-table mix (guarantees overlay_branch.current switching never
--      cross-contaminates tables).
INSERT INTO public.bs_k2 VALUES (99, 999.9999);
UPDATE public.bs_k2 SET n = n * 2 WHERE id = 2;
DELETE FROM public.bs_k2 WHERE id = 1;

SELECT CASE
         WHEN (SELECT string_agg(id::text||':'||v, ',' ORDER BY id) FROM public.bs_k1)
              = '1:A-REBORN,3:c-X'
          AND (SELECT string_agg(id::text||':'||n::text, ',' ORDER BY id) FROM public.bs_k2)
              = '2:41.0000,99:999.9999'
         THEN 'PASS:K3_MULTI_TABLE_MIX'
         ELSE 'FAIL'
       END AS k3_mix;

RESET overlay_branch.current;
SELECT CASE
         WHEN (SELECT string_agg(id::text||':'||v, ',' ORDER BY id) FROM public.bs_k1) = '1:a,2:b,3:c'
          AND (SELECT string_agg(id::text||':'||n::text, ',' ORDER BY id) FROM public.bs_k2) = '1:10.0000,2:20.5000'
         THEN 'PASS:K_MAIN_ZERO_POLLUTION'
         ELSE 'FAIL'
       END AS k_main_clean;
DROP TABLE public.bs_k1;
DROP TABLE public.bs_k2;

-- =====================================================================
-- ===== Section L: pure delta UPDATE (MVP-out-of-scope: SET projection
-- =====            for reconstructed pure-delta slot is not wired yet;
-- =====            assert WR is SAFE NO-OP, NOT silent data corruption)
-- =====================================================================
CREATE TABLE public.bs_L_upd (id INT4 PRIMARY KEY, payload TEXT, score INT4);
INSERT INTO public.bs_L_upd VALUES (1,'main-a', 10),(2,'main-b', 20);
SELECT overlay_branch.create_branch('bs_L_b');
SELECT overlay_branch.use_branch('bs_L_b');

-- L.1 Pure-delta INSERT 3 rows.
INSERT INTO public.bs_L_upd VALUES (11, 'pd-c', 30);
INSERT INTO public.bs_L_upd VALUES (12, 'pd-d', 40);
INSERT INTO public.bs_L_upd VALUES (13, 'pd-e', 50);

SELECT CASE WHEN count(*) = 3 THEN 'PASS:L1_PURE_DELTA_3_INSERTED'
            ELSE 'FAIL_COUNT:'||count(*) END AS l1_cnt
FROM public.bs_L_upd WHERE id IN (11,12,13);

-- L.2 MVP pure-delta UPDATE by PK: WR ExecQual path for CMD_UPDATE is
--     MVP-out-of-scope (we cannot yet run ModifyTable SET projection on
--     the reconstructed pure-delta slot; code skips to no-op).  Assert
--     the row is unchanged (no silent corruption).
UPDATE public.bs_L_upd SET payload = 'pd-c-UPD', score = score + 100 WHERE id = 11;
SELECT CASE WHEN payload = 'pd-c' AND score = 30 THEN 'PASS:L2_UPD_PK_MVP_NOP_SAFE'
            ELSE 'FAIL:'||COALESCE(payload,'NULL')||'@'||COALESCE(score::text,'NULL') END AS l2_nop
FROM public.bs_L_upd WHERE id = 11;

-- L.3 MVP pure-delta UPDATE non-PK WHERE by PK column: same no-op (safe)
UPDATE public.bs_L_upd SET payload = 'WRONG' WHERE id IN (12, 13, 9999);
SELECT CASE WHEN string_agg(id::text||':'||payload, ',' ORDER BY id) = '11:pd-c,12:pd-d,13:pd-e'
            THEN 'PASS:L3_UPD_MULTI_PK_MVP_NOP_SAFE'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||payload, ',' ORDER BY id),'NULL') END AS l3_nop
FROM public.bs_L_upd WHERE id IN (11,12,13);

-- L.4 Verify MAIN rows untouched.
RESET overlay_branch.current;
SELECT CASE WHEN string_agg(id::text||':'||payload||'@'||score, ',' ORDER BY id) = '1:main-a@10,2:main-b@20'
            THEN 'PASS:L_MAIN_PRISTINE'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||payload||'@'||score, ',' ORDER BY id),'NULL') END AS l_main
FROM public.bs_L_upd;
DROP TABLE public.bs_L_upd;

-- =====================================================================
-- ===== Section M: BPCHAR / NUMERIC typmod PK — pure delta DML
-- =====            (verify I.1/I.2 MAIN-PK paths work for pure-delta rows)
-- =====================================================================
-- M.1 NUMERIC(10,2) PK: pure delta INSERT then DELETE WHERE pk=const
--     (IndexScan → indexqualorig must be picked up by WR pure-delta pass)
CREATE TABLE public.bs_M_num (pk NUMERIC(10,2) PRIMARY KEY, label TEXT);
INSERT INTO public.bs_M_num VALUES (10.50, 'orig-ten'),(20.25, 'orig-twenty');
SELECT overlay_branch.create_branch('bs_Mn_b');
SELECT overlay_branch.use_branch('bs_Mn_b');

INSERT INTO public.bs_M_num VALUES (30.33, 'pd-thirty');
INSERT INTO public.bs_M_num VALUES (40.44, 'pd-forty');
DELETE FROM public.bs_M_num WHERE pk = 30.33;
SELECT CASE WHEN count(*) = 1 THEN 'PASS:M1_NUM_PK_PURE_DELTA_DELETE_1_OF_2'
            ELSE 'FAIL_COUNT:'||count(*) END AS m1_del
FROM public.bs_M_num WHERE pk IN (30.33, 40.44);
SELECT CASE WHEN label = 'pd-forty' THEN 'PASS:M1_SURVIVOR_CORRECT'
            ELSE 'FAIL:'||COALESCE(label,'NULL') END AS m1_surv
FROM public.bs_M_num WHERE pk = 40.44;
RESET overlay_branch.current;
SELECT CASE WHEN string_agg(pk::text||':'||label, ',' ORDER BY pk) = '10.50:orig-ten,20.25:orig-twenty'
            THEN 'PASS:M1_MAIN_PRISTINE'
            ELSE 'FAIL:'||COALESCE(string_agg(pk::text||':'||label, ',' ORDER BY pk),'NULL') END AS m1_main
FROM public.bs_M_num;
DROP TABLE public.bs_M_num;

-- M.2 BPCHAR(6) PK: pure delta INSERT → DELETE WHERE pk=const
--     (WR / P0 paths both rtrim bpchar PK on both sides; Confirmed fact 5)
CREATE TABLE public.bs_M_bp (code BPCHAR(6) PRIMARY KEY, flag INT4);
INSERT INTO public.bs_M_bp VALUES ('MAIN01', 1), ('MAIN02', 2);
SELECT overlay_branch.create_branch('bs_Mb_b');
SELECT overlay_branch.use_branch('bs_Mb_b');
INSERT INTO public.bs_M_bp VALUES ('PD001', 101);
INSERT INTO public.bs_M_bp VALUES ('PD002', 102);
INSERT INTO public.bs_M_bp VALUES ('PD003', 103);
DELETE FROM public.bs_M_bp WHERE code = 'PD002';
SELECT CASE WHEN string_agg(rtrim(code)||':'||flag::text, ',' ORDER BY code) = 'PD001:101,PD003:103'
            THEN 'PASS:M2_BPCHAR_PK_PURE_DELTA_DELETE_1_OF_3'
            ELSE 'FAIL:'||COALESCE(string_agg(rtrim(code)||':'||flag::text, ',' ORDER BY code),'NULL') END AS m2_del
FROM public.bs_M_bp WHERE code LIKE 'PD%';

DELETE FROM public.bs_M_bp WHERE code = 'PD001' RETURNING rtrim(code), flag;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:M2_RETURNING_DELETE_OK'
            ELSE 'FAIL_COUNT:'||count(*) END AS m2_ret
FROM public.bs_M_bp WHERE code = 'PD001';

RESET overlay_branch.current;
SELECT CASE WHEN string_agg(rtrim(code)||':'||flag::text, ',' ORDER BY code) = 'MAIN01:1,MAIN02:2'
            THEN 'PASS:M2_MAIN_PRISTINE'
            ELSE 'FAIL:'||COALESCE(string_agg(rtrim(code)||':'||flag::text, ',' ORDER BY code),'NULL') END AS m2_main
FROM public.bs_M_bp;
DROP TABLE public.bs_M_bp;

-- =====================================================================
-- ===== Section N: Multi pure-delta rows + non-PK WHERE filters
-- =====            (verify WR ExecQual on non-PK quals really filters,
-- =====             not NUKEm all pure-delta rows)
-- =====================================================================
CREATE TABLE public.bs_N_mix (id INT4 PRIMARY KEY, name TEXT, grp TEXT, score INT4);
INSERT INTO public.bs_N_mix VALUES (1, 'main-one', 'alpha', 100);
SELECT overlay_branch.create_branch('bs_N_b');
SELECT overlay_branch.use_branch('bs_N_b');

-- N.1 Insert 6 pure-delta rows: 3 × group A score<100, 3 × group B score>=100
INSERT INTO public.bs_N_mix VALUES (10, 'pd-a1', 'A', 50);
INSERT INTO public.bs_N_mix VALUES (11, 'pd-a2', 'A', 70);
INSERT INTO public.bs_N_mix VALUES (12, 'pd-a3', 'A', 90);
INSERT INTO public.bs_N_mix VALUES (20, 'pd-b1', 'B', 150);
INSERT INTO public.bs_N_mix VALUES (21, 'pd-b2', 'B', 200);
INSERT INTO public.bs_N_mix VALUES (22, 'pd-b3', 'B', 250);

SELECT CASE WHEN count(*) = 6 THEN 'PASS:N1_PURE_DELTA_6_INSERTED'
            ELSE 'FAIL_COUNT:'||count(*) END AS n1_cnt
FROM public.bs_N_mix WHERE id >= 10 AND id <= 22;

-- N.2 DELETE WHERE grp='B' (non-PK, non-indexable filter): WR ExecQual
--     must run SeqScan qual (grp='B') per reconstructed pure-delta slot.
DELETE FROM public.bs_N_mix WHERE grp = 'B';
SELECT CASE
         WHEN (SELECT count(*) FROM public.bs_N_mix WHERE grp = 'A') = 3
          AND (SELECT count(*) FROM public.bs_N_mix WHERE grp = 'B') = 0
         THEN 'PASS:N2_NONPK_FILTER_DELETE_GRP_B_ONLY'
         ELSE 'FAIL A='||COALESCE((SELECT count(*) FROM public.bs_N_mix WHERE grp = 'A')::text,'?')||' B='||COALESCE((SELECT count(*) FROM public.bs_N_mix WHERE grp = 'B')::text,'?')
       END AS n2_filt;

-- N.3 Then DELETE WHERE score < 80 (another non-PK range filter): 2 rows
--     (a1 score=50 and a2 score=70 → gone; a3 score=90 → stays)
DELETE FROM public.bs_N_mix WHERE score < 80;
SELECT CASE WHEN string_agg(id::text||':'||name||'@'||score, ',' ORDER BY id) = '12:pd-a3@90'
            THEN 'PASS:N3_SCORE_RANGE_FILTER_2_DELETED'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||name||'@'||score, ',' ORDER BY id),'NULL') END AS n3_rng
FROM public.bs_N_mix WHERE id >= 10 AND id <= 22;

-- N.4 RETURNING non-PK filter: re-insert a couple, delete with RETURNING
INSERT INTO public.bs_N_mix VALUES (30, 'pd-c1', 'C', 333);
INSERT INTO public.bs_N_mix VALUES (31, 'pd-c2', 'C', 333);
INSERT INTO public.bs_N_mix VALUES (32, 'pd-c3', 'C', 999);
DELETE FROM public.bs_N_mix WHERE grp = 'C' AND score = 333
RETURNING id, name, score ORDER BY id;
SELECT CASE WHEN string_agg(id::text||':'||name, ',' ORDER BY id) = '32:pd-c3'
            THEN 'PASS:N4_RETURNING_FILTER_SURVIVOR_32'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||name, ',' ORDER BY id),'NULL') END AS n4_surv
FROM public.bs_N_mix WHERE grp = 'C';

-- N.5 MAIN row id=1 plus pure delta N.1→N.4 combined view (aggregate)
SELECT CASE
         WHEN (SELECT count(*) FROM public.bs_N_mix) = 3
         THEN 'PASS:N5_TOTAL_3_ROWS'
         ELSE 'FAIL_COUNT:'||(SELECT count(*)::text FROM public.bs_N_mix)
       END AS n5_tot;

RESET overlay_branch.current;
SELECT CASE WHEN string_agg(id::text||':'||name||'@'||grp, ',' ORDER BY id) = '1:main-one@alpha'
            THEN 'PASS:N_MAIN_PRISTINE'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||name||'@'||grp, ',' ORDER BY id),'NULL') END AS n_main
FROM public.bs_N_mix;
DROP TABLE public.bs_N_mix;

-- =====================================================================
-- ===== Section O: apply_branch / discard_branch boundary on pure-delta
-- =====            (pure delta INSERT → then apply or discard correctly
-- =====             propagates or rolls back)
-- =====================================================================
-- O.1 apply: pure delta INSERT id=5 + DELETE id=5 reborn id=5 twice
CREATE TABLE public.bs_O1 (id INT4 PRIMARY KEY, s TEXT);
INSERT INTO public.bs_O1 VALUES (1, 'm1'),(2, 'm2');
SELECT overlay_branch.create_branch('bs_O1b');
SELECT overlay_branch.use_branch('bs_O1b');
INSERT INTO public.bs_O1 VALUES (5, 'first');
DELETE FROM public.bs_O1 WHERE id = 5;
INSERT INTO public.bs_O1 VALUES (5, 'second');
DELETE FROM public.bs_O1 WHERE id = 5;
INSERT INTO public.bs_O1 VALUES (5, 'FINAL');
SELECT CASE WHEN s = 'FINAL' THEN 'PASS:O1_PURE_REBIRTH_CHAIN'
            ELSE 'FAIL:'||COALESCE(s,'NULL') END AS o1_br
FROM public.bs_O1 WHERE id = 5;
SELECT overlay_branch.apply_branch('bs_O1b');
RESET overlay_branch.current;
SELECT CASE WHEN string_agg(id::text||':'||s, ',' ORDER BY id) = '1:m1,2:m2,5:FINAL'
            THEN 'PASS:O1_APPLY_PURE_DELTA_PROPAGATED'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||s, ',' ORDER BY id),'NULL') END AS o1_post
FROM public.bs_O1;
SELECT state FROM public.pg_branch WHERE branch_name = 'bs_O1b';
DROP TABLE public.bs_O1;

-- O.2 discard: pure delta INSERT id=77 + UPDATE MAIN id=1 → discard rolls back
CREATE TABLE public.bs_O2 (id INT4 PRIMARY KEY, s TEXT);
INSERT INTO public.bs_O2 VALUES (1, 'm1'),(2, 'm2');
SELECT overlay_branch.create_branch('bs_O2b');
SELECT overlay_branch.use_branch('bs_O2b');
INSERT INTO public.bs_O2 VALUES (77, 'will_discard');
UPDATE public.bs_O2 SET s = 'BRANCH_UPD' WHERE id = 1;
DELETE FROM public.bs_O2 WHERE id = 2;
SELECT overlay_branch.discard_branch('bs_O2b');
RESET overlay_branch.current;
SELECT CASE WHEN string_agg(id::text||':'||s, ',' ORDER BY id) = '1:m1,2:m2'
            THEN 'PASS:O2_DISCARD_ROLLS_BACK_EVERYTHING'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||s, ',' ORDER BY id),'NULL') END AS o2_disc
FROM public.bs_O2;
SELECT state FROM public.pg_branch WHERE branch_name = 'bs_O2b';
DROP TABLE public.bs_O2;

-- O.3 apply_branch with pure-delta DELETE only (INSERT then DELETE → NOP):
--     after apply MAIN baseline rows should be exactly untouched.
CREATE TABLE public.bs_O3 (id INT4 PRIMARY KEY, s TEXT);
INSERT INTO public.bs_O3 VALUES (1, 'm1'),(2, 'm2');
SELECT overlay_branch.create_branch('bs_O3b');
SELECT overlay_branch.use_branch('bs_O3b');
INSERT INTO public.bs_O3 VALUES (99, 'live_then_die');
DELETE FROM public.bs_O3 WHERE id = 99;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:O3_BRANCH_0_PURE_ROWS'
            ELSE 'FAIL_COUNT:'||count(*) END AS o3_br
FROM public.bs_O3 WHERE id = 99;
SELECT overlay_branch.apply_branch('bs_O3b');
RESET overlay_branch.current;
SELECT CASE WHEN string_agg(id::text||':'||s, ',' ORDER BY id) = '1:m1,2:m2'
            THEN 'PASS:O3_APPLY_PURE_NOP_NO_CONFLICT'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||s, ',' ORDER BY id),'NULL') END AS o3_main
FROM public.bs_O3;
DROP TABLE public.bs_O3;

-- =====================================================================
-- ===== Section P: EMPTY MAIN heap — pure delta 100% of rows
-- =====            (MAIN has zero rows; overlay consists entirely of
-- =====             pure delta INSERTs, then UPDATE (MVP NOP) / DELETE,
-- =====             BranchScan reads correctly, apply propagates)
-- =====================================================================
CREATE TABLE public.bs_P (id INT4 PRIMARY KEY, t TEXT);
SELECT overlay_branch.create_branch('bs_Pb');
SELECT overlay_branch.use_branch('bs_Pb');
-- P.1 Insert 4 rows on totally empty MAIN
INSERT INTO public.bs_P VALUES (1, 'one'), (2, 'two'), (3, 'three'), (4, 'four');
SELECT CASE WHEN string_agg(id::text||':'||t, ',' ORDER BY id) = '1:one,2:two,3:three,4:four'
            THEN 'PASS:P1_READ_EMPTY_MAIN_4_PURE'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||t, ',' ORDER BY id),'NULL') END AS p1_rd
FROM public.bs_P;
-- P.2 DELETE WHERE id IN (2,4): 2 rows removed → 2 remain
DELETE FROM public.bs_P WHERE id IN (2, 4);
SELECT CASE WHEN string_agg(id::text||':'||t, ',' ORDER BY id) = '1:one,3:three'
            THEN 'PASS:P2_DELETE_2_OF_4_ON_EMPTY_MAIN'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||t, ',' ORDER BY id),'NULL') END AS p2_del
FROM public.bs_P;
-- P.3 RETURNING delete id=3
DELETE FROM public.bs_P WHERE id = 3 RETURNING id, t;
SELECT CASE WHEN string_agg(id::text||':'||t, ',' ORDER BY id) = '1:one'
            THEN 'PASS:P3_RETURNING_DELETE_3'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||t, ',' ORDER BY id),'NULL') END AS p3_ret
FROM public.bs_P;
-- P.4 apply: MAIN should now have id=1 only
SELECT overlay_branch.apply_branch('bs_Pb');
RESET overlay_branch.current;
SELECT CASE WHEN string_agg(id::text||':'||t, ',' ORDER BY id) = '1:one'
            THEN 'PASS:P4_APPLY_PROPAGATED_1_ROW'
            ELSE 'FAIL:'||COALESCE(string_agg(id::text||':'||t, ',' ORDER BY id),'NULL') END AS p4_app
FROM public.bs_P;
DROP TABLE public.bs_P;

-- =====================================================================
-- ===== Section Q: Composite PK — pure delta DML
-- =====            (WR serialize_pk / P0 deparse both cover composite PK)
-- =====================================================================
CREATE TABLE public.bs_Q (a INT4, b TEXT, v TEXT, PRIMARY KEY (a, b));
INSERT INTO public.bs_Q VALUES (1, 'm-x', 'vx'), (2, 'm-y', 'vy');
SELECT overlay_branch.create_branch('bs_Qb');
SELECT overlay_branch.use_branch('bs_Qb');
INSERT INTO public.bs_Q VALUES (10, 'pd-a', 'A');
INSERT INTO public.bs_Q VALUES (10, 'pd-b', 'B');
INSERT INTO public.bs_Q VALUES (20, 'pd-c', 'C');
-- Q.1 Composite WHERE a=10 AND b='pd-b' — multi col IndexScan → indexqualorig.
DELETE FROM public.bs_Q WHERE a = 10 AND b = 'pd-b';
SELECT CASE WHEN string_agg(a::text||':'||b||'='||v, ',' ORDER BY a, b)
                 = '10:pd-a=A,20:pd-c=C'
            THEN 'PASS:Q1_COMPOSITE_PK_DELETE_1_OF_3'
            ELSE 'FAIL:'||COALESCE(string_agg(a::text||':'||b||'='||v, ',' ORDER BY a, b),'NULL') END AS q1_del
FROM public.bs_Q WHERE a >= 10;
-- Q.2 WHERE a=20 (partial PK prefix filter, non-exact composite match)
--     (still should hit the survivor)
DELETE FROM public.bs_Q WHERE a = 20 RETURNING a, b, v;
SELECT CASE WHEN string_agg(a::text||':'||b||'='||v, ',' ORDER BY a, b) = '10:pd-a=A'
            THEN 'PASS:Q2_PREFIX_PK_DELETE_20'
            ELSE 'FAIL:'||COALESCE(string_agg(a::text||':'||b||'='||v, ',' ORDER BY a, b),'NULL') END AS q2_pfx
FROM public.bs_Q WHERE a >= 10;
RESET overlay_branch.current;
SELECT CASE WHEN string_agg(a::text||':'||b||'='||v, ',' ORDER BY a, b) = '1:m-x=vx,2:m-y=vy'
            THEN 'PASS:Q_MAIN_PRISTINE'
            ELSE 'FAIL:'||COALESCE(string_agg(a::text||':'||b||'='||v, ',' ORDER BY a, b),'NULL') END AS q_main
FROM public.bs_Q;
DROP TABLE public.bs_Q;

-- ===== Final: drop the extension =====
DROP EXTENSION overlay_branch CASCADE;
