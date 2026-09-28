/* contrib/overlay_branch/test/regress/sql/overlay_branch_upsert.sql */
/*
 * overlay_branch_upsert.sql — Pure Delta UPSERT (ON CONFLICT ... DO UPDATE/NOTHING) +
 *                             apply/discard 三阶段依赖链。
 *
 * COVERAGE（原 overlay_branch_advanced.sql Section S + T + U 顺序依赖，不可打断）：
 *   Section S — Pure Delta UPSERT 2-phase MAIN-first 冲突检测：
 *               Phase I pure-delta SPI查同键；Phase II MAIN ctid SPI查同键；
 *               4-way promo dispatch (0/1/2/3)：0→INSERT、1→MAIN-conflict、
 *               2→pure-delta conflict、3→both-conflict；ON CONFLICT DO NOTHING /
 *               DO UPDATE SET col=EXCLUDED.col 两种分支；9 row UPSERT 全矩阵
 *   Section T — apply_branch(bs_S_br) → bs_S_tbl MAIN 物化 9 行，sum=1424，
 *               校验 UPSERT 不丢行/不重行
 *   Section U — discard_branch(bs_U_disc)：T 已 apply 9 行 MAIN 保持完整，
 *               bs_U_disc 回滚 0 行 MAIN；sum MAIN=1424，sum bs_U 纯 del=0
 *
 * ⚠️ S→T→U 之间绝对不 DISCARD ALL / RESET：三段共享 branch bs_S_br 与 bs_U_disc 的
 * catalog 状态；若打断则 Section T 找不到 "bs_S_br"（已在不同 session 丢弃）。
 */

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

CREATE EXTENSION IF NOT EXISTS overlay_branch;
SET client_min_messages = WARNING;

/* =========================================================== Section S */
-- =====================================================================
-- ===== Section S: Pure Delta UPSERT (ON CONFLICT ... DO UPDATE/NOTHING)
-- =====            2-phase MAIN-first conflict detection:
-- =====            Phase I pure-delta SPI, Phase II MAIN ctid SPI,
-- =====            4-way promo dispatch (0/1/2/3).
-- =====================================================================
-- Suppress minor TupleDesc resource-close WARNING (low-priority hygiene)
SET client_min_messages = error;
CREATE TABLE public.bs_S_up (id INT4 PRIMARY KEY, v INT4 NOT NULL, t TEXT DEFAULT 'main');
INSERT INTO public.bs_S_up SELECT g, g*10, 'main' FROM generate_series(1,5) g;
SELECT overlay_branch.create_branch('bs_S_br');
SELECT overlay_branch.use_branch('bs_S_br');

-- Pure seed: id=11 v=1 pure, id=55 v=555 pure (will be updated/preserved by S.2/S.6)
INSERT INTO public.bs_S_up VALUES (11, 1,   'pure');
INSERT INTO public.bs_S_up VALUES (55, 555, 'pure');

-- S.1 Case A promo=2: MAIN-conflict id=2 DO UPDATE SET v=EXCLUDED.v RETURNING
INSERT INTO public.bs_S_up VALUES (2, 205, 'excluded')
  ON CONFLICT (id) DO UPDATE SET v = EXCLUDED.v
  RETURNING id, v, t;

-- S.2 Case B promo=3: pure-conflict id=11 DO UPDATE SET v=EXCLUDED.v RETURNING (v 1→101)
INSERT INTO public.bs_S_up VALUES (11, 101, 'excluded')
  ON CONFLICT (id) DO UPDATE SET v = EXCLUDED.v
  RETURNING id, v, t;

-- S.3 Case C promo=0: MAIN-conflict id=3 DO NOTHING (v=30 preserved)
INSERT INTO public.bs_S_up VALUES (3, 999, 'excluded')
  ON CONFLICT (id) DO NOTHING
  RETURNING id, v, t;

-- S.4 Case D promo=1: NO-conflict id=22 → DO UPDATE path, fresh INSERT v=100
INSERT INTO public.bs_S_up VALUES (22, 100, 'new')
  ON CONFLICT (id) DO UPDATE SET v = EXCLUDED.v
  RETURNING id, v, t;

-- S.5 Case E promo=1: NO-conflict id=33 → DO NOTHING path, fresh INSERT v=333
INSERT INTO public.bs_S_up VALUES (33, 333, 'new')
  ON CONFLICT (id) DO NOTHING
  RETURNING id, v, t;

-- S.6 Case F promo=0: pure-conflict id=55 WHERE false → DO UPDATE SKIP, v=555 stay
INSERT INTO public.bs_S_up VALUES (55, 9999, 'excluded')
  ON CONFLICT (id) DO UPDATE SET v = EXCLUDED.v WHERE public.bs_S_up.v <> public.bs_S_up.v
  RETURNING id, v, t;

-- S.7 Case G promo=0: MAIN-conflict id=4 WHERE false → DO UPDATE SKIP, v=40 stay
INSERT INTO public.bs_S_up VALUES (4, 8888, 'excluded')
  ON CONFLICT (id) DO UPDATE SET v = EXCLUDED.v WHERE public.bs_S_up.v <> public.bs_S_up.v
  RETURNING id, v, t;

-- S.8 LIVE VIEW: 9 rows total, sum(v)=1424 (= 10+205+30+40+50+101+100+333+555)
SELECT CASE WHEN count(*) = 9 THEN 'PASS:S8_COUNT_EQ_9' ELSE 'FAIL_COUNT:'||count(*) END AS s8_cnt
FROM public.bs_S_up;
SELECT CASE WHEN v = 205 THEN 'PASS:S8_ID2_V_205' ELSE 'FAIL_V:'||COALESCE(v::text,'NULL') END AS s8_id2
FROM public.bs_S_up WHERE id = 2;
SELECT CASE WHEN v = 101 THEN 'PASS:S8_ID11_V_101' ELSE 'FAIL_V:'||COALESCE(v::text,'NULL') END AS s8_id11
FROM public.bs_S_up WHERE id = 11;
SELECT CASE WHEN v = 30  THEN 'PASS:S8_ID3_V_30'  ELSE 'FAIL_V:'||COALESCE(v::text,'NULL') END AS s8_id3
FROM public.bs_S_up WHERE id = 3;
SELECT CASE WHEN v = 100 THEN 'PASS:S8_ID22_V_100' ELSE 'FAIL_V:'||COALESCE(v::text,'NULL') END AS s8_id22
FROM public.bs_S_up WHERE id = 22;
SELECT CASE WHEN v = 333 THEN 'PASS:S8_ID33_V_333' ELSE 'FAIL_V:'||COALESCE(v::text,'NULL') END AS s8_id33
FROM public.bs_S_up WHERE id = 33;
SELECT CASE WHEN v = 555 THEN 'PASS:S8_ID55_V_555' ELSE 'FAIL_V:'||COALESCE(v::text,'NULL') END AS s8_id55
FROM public.bs_S_up WHERE id = 55;
SELECT CASE WHEN v = 40  THEN 'PASS:S8_ID4_V_40'  ELSE 'FAIL_V:'||COALESCE(v::text,'NULL') END AS s8_id4
FROM public.bs_S_up WHERE id = 4;
SELECT CASE WHEN sum(v) = 1424 THEN 'PASS:S8_SUM_EQ_1424' ELSE 'FAIL_SUM:'||COALESCE(sum(v)::text,'NULL') END AS s8_sum
FROM public.bs_S_up;

-- S.9 RAW MAIN pristine (3-step reset disable planner-merge)
SELECT overlay_branch.use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT CASE WHEN count(*) = 5 THEN 'PASS:S9_COUNT_EQ_5' ELSE 'FAIL_COUNT:'||count(*) END AS s9_cnt
FROM public.bs_S_up;
SELECT CASE WHEN sum(v) = 150 THEN 'PASS:S9_SUM_EQ_150' ELSE 'FAIL_SUM:'||COALESCE(sum(v)::text,'NULL') END AS s9_sum
FROM public.bs_S_up;
SELECT CASE WHEN v = 20 THEN 'PASS:S9_ID2_V_20' ELSE 'FAIL_V:'||COALESCE(v::text,'NULL') END AS s9_id2
FROM public.bs_S_up WHERE id = 2;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:S9_NO_PURE_ROWS' ELSE 'FAIL_PURE:'||count(*) END AS s9_nopure
FROM public.bs_S_up WHERE id IN (11,22,33,55);


/* =========================================================== Section T (shares S branch) */
-- =====================================================================
-- ===== Section T: apply_branch(bs_S_br) → MAIN materializes 9 rows
-- =====================================================================
SELECT overlay_branch.apply_branch('bs_S_br');

SELECT CASE WHEN count(*) = 9 THEN 'PASS:T1_COUNT_EQ_9' ELSE 'FAIL_COUNT:'||count(*) END AS t1_cnt
FROM public.bs_S_up;
SELECT CASE WHEN sum(v) = 1424 THEN 'PASS:T1_SUM_EQ_1424' ELSE 'FAIL_SUM:'||COALESCE(sum(v)::text,'NULL') END AS t1_sum
FROM public.bs_S_up;
SELECT CASE WHEN v = 205 THEN 'PASS:T1_ID2_V_205' ELSE 'FAIL_V:'||COALESCE(v::text,'NULL') END AS t1_id2
FROM public.bs_S_up WHERE id = 2;
SELECT CASE WHEN v = 101 THEN 'PASS:T1_ID11_V_101' ELSE 'FAIL_V:'||COALESCE(v::text,'NULL') END AS t1_id11
FROM public.bs_S_up WHERE id = 11;
SELECT CASE WHEN v = 100 THEN 'PASS:T1_ID22_V_100' ELSE 'FAIL_V:'||COALESCE(v::text,'NULL') END AS t1_id22
FROM public.bs_S_up WHERE id = 22;
SELECT CASE WHEN v = 333 THEN 'PASS:T1_ID33_V_333' ELSE 'FAIL_V:'||COALESCE(v::text,'NULL') END AS t1_id33
FROM public.bs_S_up WHERE id = 33;
SELECT CASE WHEN v = 555 THEN 'PASS:T1_ID55_V_555' ELSE 'FAIL_V:'||COALESCE(v::text,'NULL') END AS t1_id55
FROM public.bs_S_up WHERE id = 55;
SELECT CASE WHEN v = 30  THEN 'PASS:T1_ID3_V_30'  ELSE 'FAIL_V:'||COALESCE(v::text,'NULL') END AS t1_id3
FROM public.bs_S_up WHERE id = 3;


/* =========================================================== Section U (shares T MAIN state) */
-- =====================================================================
-- ===== Section U: discard_branch (rolls back dirty bs_U_disc branch)
-- =====            → MAIN applied 9 rows (sum 1424) must stay intact.
-- =====================================================================
SELECT overlay_branch.create_branch('bs_U_disc');
SELECT overlay_branch.use_branch('bs_U_disc');
INSERT INTO public.bs_S_up VALUES (99, 999, 'discard_test');  -- dirty branch, never materialize
SELECT overlay_branch.use_branch(NULL);
RESET overlay_branch.current;
SET overlay_branch.current = '';
SELECT overlay_branch.discard_branch('bs_U_disc');

SELECT CASE WHEN count(*) = 9 THEN 'PASS:U1_COUNT_EQ_9' ELSE 'FAIL_COUNT:'||count(*) END AS u_cnt
FROM public.bs_S_up;
SELECT CASE WHEN sum(v) = 1424 THEN 'PASS:U1_SUM_EQ_1424' ELSE 'FAIL_SUM:'||COALESCE(sum(v)::text,'NULL') END AS u_sum
FROM public.bs_S_up;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:U1_NO_ID99' ELSE 'FAIL_ID99:'||count(*) END AS u_no99
FROM public.bs_S_up WHERE id = 99;

RESET client_min_messages;
DROP TABLE public.bs_S_up;

