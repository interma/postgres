-- apply_contention.sql — pgbench custom script for T8 L2 probabilistic race detection
--
-- Run manually (NOT part of make check zero-regression gate):
--   cd contrib/overlay_branch
--   # First initialize the fixture (once):
--   psql $PG_REGRESS_DB -f test/bench/apply_contention_fixture.sql
--   # Run 60s 32 clients:
--   pgbench -n -c 32 -j 2 -T 60 -M prepared -f test/bench/apply_contention.sql $PG_REGRESS_DB
--
-- Client distribution (each client randomly picks ONE role on connect via MOD(CLIENT_ID,10)):
--   0..5 (60%) → WR writer: select + insert/update/delete on MAIN PK
--   6..7 (20%) → Branch switcher: live / snapshot rotate via use_branch
--   8      (10%) → applyer: apply random active branch; catch and ignore 55000
--   9      (10%) → discarder: discard random applied branch; catch 55000, ignore
--
-- Post-run verification SQL:
--   SELECT count(*) FROM overlay_branch.pg_branch WHERE state NOT IN ('active','applied','discarded');
--   -- Expect = 0.  Any other state = FR3 leak FAIL.
--   SELECT count(*) FROM overlay_branch.pg_branch WHERE state = 'applying';
--   -- Expect = 0.  Non-zero = applying state stuck FAIL.
--   SELECT COUNT(*) = 10000 FROM public.pgbench_accounts; -- MAIN rows baseline (exact T=0 count)

\set randid     (random()*9999 + 1)
\set role       (:client_id % 10)
\set branchno   (1 + (:client_id % 8))
\set branchname ('bs_pgbench_' || :branchno)

-- Branch switcher role
SELECT CASE WHEN :role BETWEEN 6 AND 7 THEN use_branch(:'branchname', CASE WHEN (:client_id % 2 = 0) THEN 'live' ELSE 'snapshot' END)::text ELSE 'noop' END AS role_branch_switch;

-- WR writer role
BEGIN;
SELECT count(*) FROM public.pgbench_accounts WHERE aid = :randid;
INSERT INTO public.pgbench_accounts(aid, bid, abalance, filler)
     VALUES (10000 + :randid, 1, 0, repeat('x', 84))
     ON CONFLICT (aid) DO NOTHING;
UPDATE public.pgbench_accounts SET abalance = abalance + 1 WHERE aid = :randid;
COMMIT;

-- Applyer role (10%)
SELECT CASE WHEN :role = 8 THEN (SELECT overlay_branch.apply_branch(:'branchname')::text) ELSE 'noop' END AS apply_out;

-- Discarder role (10%)
SELECT CASE WHEN :role = 9 THEN (SELECT overlay_branch.discard_branch(:'branchname')::text) ELSE 'noop' END AS discard_out;
