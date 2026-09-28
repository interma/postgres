/* contrib/overlay_branch/overlay_branch--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION overlay_branch" to load this file. \quit

-- ============================================================
-- Catalog tables for the extension
--   The extension lives in the fixed schema `overlay_branch'
--   (see overlay_branch.control).  Tables go there, never
--   into pg_catalog, because PG disallows unprivileged writes
--   into pg_catalog unless allow_system_table_modifications is
--   enabled superuser-only.
--
--   Every C-callable function body below carries
--     SET search_path = @extschema@, pg_catalog
--   so that the SPI queries inside C code (which use plain
--   "pg_branch" and "pg_branch_delta", unqualified) find them
--   without ambiguity.
-- ============================================================

CREATE SEQUENCE @extschema@.pg_branch_branch_id_seq
    START WITH 1
    INCREMENT BY 1
    NO MINVALUE
    NO MAXVALUE
    CACHE 1;

CREATE TABLE @extschema@.pg_branch (
    branch_id       integer NOT NULL DEFAULT nextval('@extschema@.pg_branch_branch_id_seq'),
    branch_name     name NOT NULL,
    owner           oid NOT NULL,
    created_at      timestamp with time zone NOT NULL DEFAULT now(),
    mode            text NOT NULL DEFAULT 'live',
    state           text NOT NULL DEFAULT 'active',
    CONSTRAINT pg_branch_pkey PRIMARY KEY (branch_id),
    CONSTRAINT pg_branch_name_ukey UNIQUE (branch_name)
);

ALTER SEQUENCE @extschema@.pg_branch_branch_id_seq OWNED BY @extschema@.pg_branch.branch_id;

CREATE TABLE @extschema@.pg_branch_delta (
    branch_id       integer NOT NULL,
    relid           oid NOT NULL,
    key             text NOT NULL,
    op              char(1) NOT NULL,
    old_version     text,
    tuple_data      bytea,
    created_at      timestamp with time zone NOT NULL DEFAULT now(),
    updated_at      timestamp with time zone NOT NULL DEFAULT now(),
    CONSTRAINT pg_branch_delta_pkey PRIMARY KEY (branch_id, relid, key),
    CONSTRAINT pg_branch_delta_op_check CHECK (op IN ('I', 'U', 'D'))
);

CREATE INDEX pg_branch_delta_relid_idx
    ON @extschema@.pg_branch_delta (branch_id, relid);

-- ============================================================
-- §G06 最小防泄漏：显式内部表标记 publish=false (PG >=16 仅 publication 级别有效)
--
-- 注：PG 14-17 中 reloptions "publish=false" **并非堆表的合法 reloption**（publish
-- 是 publication 对象的 WITH 选项，控制该 publication 产生哪类 DML 事件
-- 'insert/update/delete/truncate'，作用于 pg_publication，不是 pg_class）。
-- PG 14-17 FOR ALL TABLES 选择集合只通过 is_publishable_class() 过滤：
--   - (RELKIND_RELATION / RELKIND_PARTITIONED_TABLE)
--   - !IsCatalogRelationOid(relid)  → 必须不是 pinned OID
--   - RELPERSISTENCE_PERMANENT
--   - relid >= FirstNormalObjectId
-- 扩展安装期间创建的表 relid 满足 FirstNormalObjectId 条件；用 pinned
-- OID 的方案也不可行（CREATE EXTENSION 使用 GetNewObjectId()）。
--
-- 因此 G06 MVP 只能通过 RUNTIME 检测 + 文档：
--   - E-2 ob_g06_check_publication_leak_throttled() 在 create/enter/apply/
--     discard 入口发现 puball=true 或 pubschema 含 overlay_branch schema
--     时 emit WARNING（S26 产品化可升级到 ERROR）；
--   - 本处仅保留占位 reloption 注释，不 SET publish=false（否则 CREATE EXTENSION
--     ERROR: unrecognized parameter "publish"）。
-- ============================================================
COMMENT ON TABLE @extschema@.pg_branch IS
  'overlay_branch internal branch catalog table (§G06: exclude from FOR ALL TABLES publication by site-local policy, see review_260926.md §2.5)';
COMMENT ON TABLE @extschema@.pg_branch_delta IS
  'overlay_branch internal per-branch per-table delta store (§G06: exclude from FOR ALL TABLES publication by site-local policy, see review_260926.md §2.5)';

-- ============================================================
-- SQL-callable (C-language) management functions.
-- We also publish them via public synonyms so callers can use
-- bare names (SELECT create_branch('b1')) without prefix.
-- ============================================================

CREATE FUNCTION @extschema@.create_branch(branch_name name)
RETURNS integer
AS 'MODULE_PATHNAME', 'overlay_branch_create'
LANGUAGE C STRICT VOLATILE
SET search_path = @extschema@, pg_catalog;

CREATE FUNCTION @extschema@.use_branch(branch_name name)
RETURNS void
AS 'MODULE_PATHNAME', 'overlay_branch_use'
LANGUAGE C VOLATILE
SET search_path = @extschema@, pg_catalog;
COMMENT ON FUNCTION @extschema@.use_branch(name) IS
'Enter a branch by name (live mode) or exit by passing NULL/empty string.';

-- ============================================================
-- 2-parameter use_branch(name, mode) with MVCC mode override.
--   mode = 'live'    : reads on MAIN use current snapshot (V2 behaviour)
--   mode = 'snapshot': reads on MAIN freeze at the snapshot taken when
--                      this function is called; see V3 docs.
-- OVERLOAD RESOLUTION NOTE: mode has NO SQL DEFAULT so use_branch('foo')
--   UNIQUELY resolves to the 1-parameter form above; without this PG 17
--   raises "ERROR: function use_branch(unknown) is not unique".
-- ============================================================
CREATE FUNCTION @extschema@.use_branch(branch_name name, mode text)
RETURNS void
AS 'MODULE_PATHNAME', 'overlay_branch_use_with_mode'
LANGUAGE C VOLATILE
SET search_path = @extschema@, pg_catalog;

COMMENT ON FUNCTION @extschema@.use_branch(name, text) IS
'Enter a branch with an explicit MVCC mode.
mode = ''live''    : reads on MAIN use current snapshot (V2 behaviour).
mode = ''snapshot'': reads on MAIN freeze at the MVCC snapshot taken
                     when this function is called; see V3 docs.';

CREATE FUNCTION @extschema@.current_branch()
RETURNS name
AS 'MODULE_PATHNAME', 'overlay_branch_current'
LANGUAGE C STRICT STABLE
SET search_path = @extschema@, pg_catalog;

CREATE FUNCTION @extschema@.apply_branch(branch_name name)
RETURNS void
AS 'MODULE_PATHNAME', 'overlay_branch_apply'
LANGUAGE C STRICT VOLATILE
SET search_path = @extschema@, pg_catalog;

CREATE FUNCTION @extschema@.discard_branch(branch_name name)
RETURNS void
AS 'MODULE_PATHNAME', 'overlay_branch_discard'
LANGUAGE C STRICT VOLATILE
SET search_path = @extschema@, pg_catalog;

-- NOTE: list_branches is intentionally implemented in pure SQL rather
-- than C for MVP Step 1.  The naive C SRF had a number of stability
-- problems (deep-copying SPI datum values by hand is error-prone and
-- produced SIGSEGV / all-NULL rows in several builds).  The query
-- here is semantically identical to what the C stub used to do: a
-- LEFT JOIN between pg_branch and the per-branch delta count.  Since
-- the extension's catalog tables live in @extschema@ and we run with
-- the fixed search_path, this SQL version is also read-only and
-- needs no upgrade to V2 to be composable with later Step code.
CREATE FUNCTION @extschema@.list_branches()
RETURNS TABLE(
    branch_id integer,
    branch_name name,
    owner oid,
    created_at timestamp with time zone,
    mode text,
    state text,
    delta_count bigint
)
LANGUAGE sql STABLE STRICT
SET search_path = @extschema@, pg_catalog
AS $$
    SELECT b.branch_id,
           b.branch_name,
           b.owner,
           b.created_at,
           b.mode,
           b.state,
           COALESCE(d.cnt, 0)::bigint AS delta_count
    FROM pg_branch b
    LEFT JOIN (SELECT branch_id, count(*) AS cnt
               FROM pg_branch_delta
               GROUP BY branch_id) d
      ON d.branch_id = b.branch_id
    ORDER BY b.branch_id;
$$;

CREATE FUNCTION @extschema@.overlay_branch_cached_mode(bid integer)
RETURNS text
AS 'MODULE_PATHNAME', 'overlay_branch_cached_mode'
LANGUAGE C STABLE STRICT
SET search_path = @extschema@, pg_catalog;

CREATE OR REPLACE FUNCTION @extschema@.list_branches()
RETURNS TABLE(
    branch_id integer,
    branch_name name,
    owner oid,
    created_at timestamp with time zone,
    mode text,
    state text,
    delta_count bigint
)
LANGUAGE sql STABLE STRICT
SET search_path = @extschema@, pg_catalog
AS $$
    SELECT b.branch_id,
           b.branch_name,
           b.owner,
           b.created_at,
           COALESCE(@extschema@.overlay_branch_cached_mode(b.branch_id), b.mode) AS mode,
           b.state,
           COALESCE(d.cnt, 0)::bigint AS delta_count
    FROM pg_branch b
    LEFT JOIN (SELECT branch_id, count(*) AS cnt
               FROM pg_branch_delta
               GROUP BY branch_id) d
      ON d.branch_id = b.branch_id
    ORDER BY b.branch_id;
$$;

CREATE FUNCTION @extschema@.overlay_main_plus_delta(regclass)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'overlay_main_plus_delta'
LANGUAGE C STRICT STABLE
SET search_path = @extschema@, pg_catalog;

-- ============================================================
-- Debug SQL-callable wrappers for Delta Store (Step 2c smoke)
--
-- These expose the C overlay_delta_* internal SPI path so it
-- can be exercised from psql before the Write Redirect hooks
-- are wired in.  Signature names are deliberately ugly to
-- discourage end-user use; they can be dropped in future by
-- creating an overlay_branch--1.0--1.1.sql upgrade script.
-- ============================================================

CREATE FUNCTION @extschema@.overlay_debug_delta_insert(
    branch_id integer,
    relid oid,
    key text,
    op "char",
    old_version text,
    tuple_data bytea
) RETURNS text
AS 'MODULE_PATHNAME', 'overlay_debug_delta_insert'
LANGUAGE C VOLATILE
SET search_path = @extschema@, pg_catalog;
COMMENT ON FUNCTION @extschema@.overlay_debug_delta_insert IS
'NOT STRICT: old_version and tuple_data are legitimately NULL (DELETE, no base version yet).';

CREATE FUNCTION @extschema@.overlay_debug_delta_count(
    branch_id integer
) RETURNS bigint
AS 'MODULE_PATHNAME', 'overlay_debug_delta_count'
LANGUAGE C STRICT STABLE
SET search_path = @extschema@, pg_catalog;

CREATE FUNCTION @extschema@.overlay_debug_delta_delete_all(
    branch_id integer
) RETURNS void
AS 'MODULE_PATHNAME', 'overlay_debug_delta_delete_all'
LANGUAGE C STRICT VOLATILE
SET search_path = @extschema@, pg_catalog;

-- ============================================================
-- V3 Task 2 Layer 1 (FR1 + FR4): throttled catalog recheck
--   * overlay_branch_is_active(name) RETURNS bool
--       - returns ground truth from pg_branch catalog, performing
--         throttled heavy SPI recheck if enough calls have elapsed
--         (see GUCs overlay_branch.invalidation_check_threshold and
--         overlay_branch.invalidation_check_interval_ms).
--       - NOT STRICT / VOLATILE per project conventions: we need to
--         handle NULL inputs manually and recheck state each call.
--   * overlay_branch_force_invalidation_check() RETURNS bool
--       - user-visible "check NOW" bypass: forces a heavy SPI recheck
--         regardless of throttle counter / elapsed time.
--       - returns true if the branch context remained active AFTER the
--         recheck; false and NOTICE when rechecked and found inactive.
--   * NOTIFY channel "ob_branch_state" payload format "<bid>:<new_state>"
--     is broadcast from apply_branch / discard_branch C entry points
--     after catalog mutations commit; no SQL entry needed (registered by
--     _PG_init / called from branch_lifecycle.c).
-- ============================================================

CREATE FUNCTION @extschema@.overlay_branch_is_active(name name)
RETURNS bool
LANGUAGE C VOLATILE SET search_path = ''
AS 'MODULE_PATHNAME', 'overlay_branch_is_active_by_name';

CREATE FUNCTION @extschema@.overlay_branch_force_invalidation_check()
RETURNS bool
LANGUAGE C VOLATILE SET search_path = ''
AS 'MODULE_PATHNAME', 'overlay_branch_force_invalidation_check';

-- ============================================================
-- Public aliases (views for tables, plain functions exposed)
-- ============================================================

-- Tables are private to extension schema.  Read-only views are
-- published for debugging / SELECT convenience.

CREATE OR REPLACE VIEW public.pg_branch
    WITH (security_barrier = true)
    AS SELECT * FROM @extschema@.pg_branch;
GRANT SELECT ON public.pg_branch TO PUBLIC;

CREATE OR REPLACE VIEW public.pg_branch_delta
    WITH (security_barrier = true)
    AS SELECT * FROM @extschema@.pg_branch_delta;
GRANT SELECT ON public.pg_branch_delta TO PUBLIC;

-- ============================================================
-- Public synonyms for convenience.
--   Extensions with schema != public must still be usable from
--   plain `SELECT create_branch('b1')` to match the documented
--   UX in doc/example_sql.md.  Since PostgreSQL has no CREATE
--   SYNONYM we build SQL-language wrappers in the public schema
--   that just forward to the real @extschema@ functions.
--   search_path visibility: wrappers are in public, which is in
--   every user's default search_path.
--   IMPORTANT: we also add @extschema@ to the database's
--   search_path via ALTER DATABASE below is overkill.  Just
--   wrappers suffice.
-- ============================================================

CREATE OR REPLACE FUNCTION public.create_branch(branch_name name)
RETURNS integer LANGUAGE sql VOLATILE SET search_path = @extschema@, pg_catalog
AS $$SELECT @extschema@.create_branch(branch_name)$$;

CREATE OR REPLACE FUNCTION public.use_branch(branch_name name)
RETURNS void LANGUAGE sql VOLATILE SET search_path = @extschema@, pg_catalog
AS $$SELECT @extschema@.use_branch(branch_name)$$;

CREATE OR REPLACE FUNCTION public.current_branch()
RETURNS name LANGUAGE sql STABLE SET search_path = @extschema@, pg_catalog
AS $$SELECT @extschema@.current_branch()$$;

CREATE OR REPLACE FUNCTION public.apply_branch(branch_name name)
RETURNS void LANGUAGE sql VOLATILE SET search_path = @extschema@, pg_catalog
AS $$SELECT @extschema@.apply_branch(branch_name)$$;

CREATE OR REPLACE FUNCTION public.discard_branch(branch_name name)
RETURNS void LANGUAGE sql VOLATILE SET search_path = @extschema@, pg_catalog
AS $$SELECT @extschema@.discard_branch(branch_name)$$;

CREATE OR REPLACE FUNCTION public.use_branch(branch_name name, mode text)
RETURNS void LANGUAGE sql VOLATILE SET search_path = @extschema@, pg_catalog
AS $$SELECT @extschema@.use_branch(branch_name, mode)$$;

CREATE OR REPLACE FUNCTION public.is_active(name name)
RETURNS bool
SET search_path = @extschema@, pg_catalog, pg_temp
LANGUAGE sql
AS $$
    SELECT @extschema@.overlay_branch_is_active(name);
$$;

CREATE OR REPLACE FUNCTION public.force_invalidation_check()
RETURNS bool
SET search_path = @extschema@, pg_catalog, pg_temp
LANGUAGE sql
AS $$
    SELECT @extschema@.overlay_branch_force_invalidation_check();
$$;

CREATE OR REPLACE FUNCTION public.list_branches()
RETURNS TABLE(branch_id integer, branch_name name, owner oid,
              created_at timestamp with time zone, mode text,
              state text, delta_count bigint)
LANGUAGE sql STABLE SET search_path = @extschema@, pg_catalog
AS $$SELECT * FROM @extschema@.list_branches()$$;

-- NOTE: overlay_main_plus_delta() is intentionally NOT wrapped in public
-- for MVP Step 1.  Its RETURNS SETOF record requires a column-definition
-- list at call time (e.g. FROM overlay_branch.overlay_main_plus_delta('t')
-- AS x(col type, ...)), which cannot be transparently forwarded through a
-- SQL-language wrapper.  Users can always qualify with the schema, and
-- Step 4 of the MVP will introduce a more usable signature.

GRANT EXECUTE ON FUNCTION public.create_branch(name) TO PUBLIC;
GRANT EXECUTE ON FUNCTION public.use_branch(name) TO PUBLIC;
GRANT EXECUTE ON FUNCTION public.current_branch() TO PUBLIC;
GRANT EXECUTE ON FUNCTION public.apply_branch(name) TO PUBLIC;
GRANT EXECUTE ON FUNCTION public.discard_branch(name) TO PUBLIC;
GRANT EXECUTE ON FUNCTION public.use_branch(name, text) TO PUBLIC;
GRANT EXECUTE ON FUNCTION public.list_branches() TO PUBLIC;

GRANT EXECUTE ON FUNCTION @extschema@.overlay_branch_is_active(name) TO PUBLIC;
GRANT EXECUTE ON FUNCTION @extschema@.overlay_branch_force_invalidation_check() TO PUBLIC;
GRANT EXECUTE ON FUNCTION public.is_active(name) TO PUBLIC;
GRANT EXECUTE ON FUNCTION public.force_invalidation_check() TO PUBLIC;

COMMENT ON EXTENSION overlay_branch IS 'Overlay Branch - speculative database state using table overlay and delta store';
