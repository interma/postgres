-- contrib/overlay_branch/test/regress/sql/overlay_branch_puredelta.sql
-- ==========================================================
-- Pure-Delta UPDATE/DELETE MAIN-miss regression suite (B1).
-- 12 PASS assertions covering:
--   P1  I→U  single chain (pure-delta INSERT→UPDATE MAIN-miss)
--   P2  I→U  WITH RETURNING projection correctness
--   P3  I→U→U  multi-chain UPDATE → UPDATE 0-bug fix
--   P4  I→D  single chain pure-delta DELETE MAIN-miss
--   P5  MAIN baseline UPDATE → still works (MAIN-hit path)
--   P6  MAIN baseline U→U  double chain → no double-write
--   P7  apply_branch  end-to-end: 5 rows applied correctly
--   P8  discard_branch on pure-delta writes: MAIN unchanged
--   P9  I→U→D  triple chain (pure-delta multi-op)
--   P10 apply pure UPDATE (no MAIN baseline) uses insert_pass fallback
-- ==========================================================

DROP EXTENSION IF EXISTS overlay_branch CASCADE;
CREATE EXTENSION overlay_branch;

-- ========== PART A: Pure-Delta WRITE scenarios ==========
CREATE TABLE pd_t1 (
    id  int PRIMARY KEY,
    val text,
    num int
);
INSERT INTO pd_t1 (id, val, num) VALUES (1, 'm1', 100), (2, 'm2', 200);
SELECT create_branch('b1');
SELECT use_branch('b1');

-- P1: I→U single chain (pure delta)
INSERT INTO pd_t1 (id, val, num) VALUES (11, 'i1', 101);
UPDATE pd_t1 SET val = 'u1', num = 111 WHERE id = 11;
SELECT CASE WHEN (val, num) = ('u1', 111) THEN 'PASS:P1'
            ELSE 'FAIL:P1(' || val || ',' || num || ')' END
FROM pd_t1 WHERE id = 11;

-- P2: I→U RETURNING
INSERT INTO pd_t1 (id, val, num) VALUES (14, 'i4', 104);
UPDATE pd_t1 SET val = 'u4', num = num + 100 WHERE id = 14 RETURNING (id=14 AND val='u4' AND num=204) AS p2_check;
SELECT CASE WHEN (val, num) = ('u4', 204) THEN 'PASS:P2'
            ELSE 'FAIL:P2(' || val || ',' || num || ')' END
FROM pd_t1 WHERE id = 14;

-- P3: I→U→U multi chain
INSERT INTO pd_t1 (id, val, num) VALUES (15, 'i5a', 500);
UPDATE pd_t1 SET val = 'i5b', num = 501 WHERE id = 15;
UPDATE pd_t1 SET val = 'i5c', num = 502 WHERE id = 15;
SELECT CASE WHEN (val, num) = ('i5c', 502) THEN 'PASS:P3'
            ELSE 'FAIL:P3(' || val || ',' || num || ')' END
FROM pd_t1 WHERE id = 15;

-- P4: I→D single chain
INSERT INTO pd_t1 (id, val, num) VALUES (12, 'i2', 102);
DELETE FROM pd_t1 WHERE id = 12;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:P4'
            ELSE 'FAIL:P4 count=' || count(*) END
FROM pd_t1 WHERE id = 12;

-- P5: MAIN baseline UPDATE (id=1) → MAIN-hit
UPDATE pd_t1 SET val = 'm1u', num = 999 WHERE id = 1;
SELECT CASE WHEN (val, num) = ('m1u', 999) THEN 'PASS:P5'
            ELSE 'FAIL:P5(' || val || ',' || num || ')' END
FROM pd_t1 WHERE id = 1;

-- P6: MAIN baseline U→U double chain → no double UPDATE
UPDATE pd_t1 SET val = 'm2b', num = 201 WHERE id = 2;
UPDATE pd_t1 SET val = 'm2c', num = 202 WHERE id = 2;
SELECT CASE WHEN (val, num) = ('m2c', 202) THEN 'PASS:P6'
            ELSE 'FAIL:P6(' || val || ',' || num || ')' END
FROM pd_t1 WHERE id = 2;

-- ========== PART B: apply_branch + discard + multi-op ==========

-- P7: apply_branch full merge: 5 rows (1,2,11,14,15) with correct values
SELECT overlay_branch.use_branch('');
SELECT apply_branch('b1');
SELECT CASE
    WHEN count(*) = 5 THEN 'PASS:P7-count'
    ELSE 'FAIL:P7-count=' || count(*)
END FROM pd_t1;
SELECT CASE WHEN (val, num) = ('m1u', 999)  THEN 'PASS:P7-r1'
            ELSE 'FAIL:P7-r1(' || val || ',' || num || ')' END
FROM pd_t1 WHERE id = 1;
SELECT CASE WHEN (val, num) = ('m2c', 202)  THEN 'PASS:P7-r2'
            ELSE 'FAIL:P7-r2(' || val || ',' || num || ')' END
FROM pd_t1 WHERE id = 2;
SELECT CASE WHEN (val, num) = ('u1', 111)   THEN 'PASS:P7-r11'
            ELSE 'FAIL:P7-r11(' || val || ',' || num || ')' END
FROM pd_t1 WHERE id = 11;
SELECT CASE WHEN (val, num) = ('u4', 204)   THEN 'PASS:P7-r14'
            ELSE 'FAIL:P7-r14(' || val || ',' || num || ')' END
FROM pd_t1 WHERE id = 14;
SELECT CASE WHEN (val, num) = ('i5c', 502)  THEN 'PASS:P7-r15'
            ELSE 'FAIL:P7-r15(' || val || ',' || num || ')' END
FROM pd_t1 WHERE id = 15;

-- P8: discard_branch with pure-delta writes → MAIN unchanged
CREATE TABLE pd_t2 (id int PRIMARY KEY, v text);
INSERT INTO pd_t2 VALUES (1, 'm1');
SELECT create_branch('b2');
SELECT use_branch('b2');
INSERT INTO pd_t2 VALUES (10, 'bi10');
UPDATE pd_t2 SET v = 'bi10u'  WHERE id = 10;
UPDATE pd_t2 SET v = 'bi10u2' WHERE id = 10;
SELECT discard_branch('b2');
SELECT CASE WHEN (array_agg(id ORDER BY id),
                  array_agg(v  ORDER BY id))
                 = (ARRAY[1], ARRAY['m1'])
            THEN 'PASS:P8'
            ELSE 'FAIL:P8' END
FROM pd_t2;

-- P9: I→U→D triple chain
CREATE TABLE pd_t3 (id int PRIMARY KEY, v text);
SELECT create_branch('b3');
SELECT use_branch('b3');
INSERT INTO pd_t3 VALUES (20, 'a');
UPDATE pd_t3 SET v = 'b' WHERE id = 20;
DELETE FROM pd_t3 WHERE id = 20;
SELECT CASE WHEN count(*) = 0 THEN 'PASS:P9'
            ELSE 'FAIL:P9 count=' || count(*) END
FROM pd_t3 WHERE id = 20;
SELECT discard_branch('b3');

-- P10: pure UPDATE (old_version NULL) apply uses insert_pass fallback
CREATE TABLE pd_t4 (id int PRIMARY KEY, v text);
SELECT create_branch('b4');
SELECT use_branch('b4');
INSERT INTO pd_t4 VALUES (99, 'x');
UPDATE pd_t4 SET v = 'y' WHERE id = 99;
UPDATE pd_t4 SET v = 'z' WHERE id = 99;
SELECT overlay_branch.use_branch('');
SELECT apply_branch('b4');
SELECT CASE WHEN v = 'z' THEN 'PASS:P10'
            ELSE 'FAIL:P10 v=' || v END
FROM pd_t4 WHERE id = 99;

-- P11: MVP NOP guards (CTID-based writes + multi-table JOIN WR)
--   These are intentionally out-of-scope for the MVP: WR hook must not
--   corrupt data by silently performing half-work; instead it must keep
--   the row untouched so the user can detect the no-op via CASE assertions
--   (matches the documented MVP scope: only PK-equality quals supported).
CREATE TABLE pd_t5 (id int PRIMARY KEY, v text, payload text);
CREATE TABLE pd_t6 (id int PRIMARY KEY, ref int);
SELECT create_branch('b11');
SELECT use_branch('b11');
INSERT INTO pd_t5 VALUES (1, 'orig', 'base');      -- pure delta row
INSERT INTO pd_t6 VALUES (101, 1);                   -- pure delta row (FK-emulation)

-- P11.1 UPDATE WHERE ctid = ...: ctid refers to MAIN storage only; pure
--       delta rows have no MAIN ctid.  MVP guard: no PK-equality → NOP.
UPDATE pd_t5 SET payload = 'WRONG-ctid-upd'
 WHERE ctid = (SELECT ctid FROM pd_t5 WHERE id = 1 LIMIT 1);
SELECT CASE WHEN payload = 'base' THEN 'PASS:P11_1_UPD_CTID_NOP'
            ELSE 'FAIL:P11_1 payload=' || COALESCE(payload,'NULL') END AS p11_1
  FROM pd_t5 WHERE id = 1;

-- P11.2 DELETE WHERE ctid = ...: same MVP guard → NOP.
DELETE FROM pd_t5
 WHERE ctid = (SELECT ctid FROM pd_t5 WHERE id = 1 LIMIT 1);
SELECT CASE WHEN count(*) = 1 THEN 'PASS:P11_2_DEL_CTID_NOP'
            ELSE 'FAIL:P11_2 count=' || count(*) END AS p11_2
  FROM pd_t5 WHERE id = 1;

-- P11.3 UPDATE ... FROM (join): MVP decomposes only single-rel PK-equality
--       quals; join quals cannot be rewritten into a PK exact-match.  Guard
--       MUST produce a safe NOP, not a partial or mis-joined write.
UPDATE pd_t5 SET payload = 'WRONG-join-upd'
  FROM pd_t6
 WHERE pd_t5.id = pd_t6.ref
   AND pd_t6.id = 101;
SELECT CASE WHEN payload = 'base' THEN 'PASS:P11_3_UPD_JOIN_NOP'
            ELSE 'FAIL:P11_3 payload=' || COALESCE(payload,'NULL') END AS p11_3
  FROM pd_t5 WHERE id = 1;

-- P11.4 RETURNING * on pure-delta UPDATE (PK-equality): pure-delta UPDATE
--       branch must correctly populate RETURNING projection with the
--       post-SET row (all columns).  Covers V1 RETURNING projection path.
UPDATE pd_t5 SET v = 'p114', payload = 'ret-upd' WHERE id = 1
  RETURNING id, v, payload;
SELECT CASE WHEN v = 'p114' AND payload = 'ret-upd'
            THEN 'PASS:P11_4_RETURNING_STAR'
            ELSE 'FAIL:P11_4 v=' || COALESCE(v,'NULL')
                 || ' payload=' || COALESCE(payload,'NULL') END AS p11_4
  FROM pd_t5 WHERE id = 1;

SELECT discard_branch('b11');

-- cleanup
DROP TABLE pd_t1, pd_t2, pd_t3, pd_t4, pd_t5, pd_t6;
DROP EXTENSION overlay_branch CASCADE;
