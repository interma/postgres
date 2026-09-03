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
LANGUAGE C STRICT VOLATILE
SET search_path = @extschema@, pg_catalog;

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
GRANT EXECUTE ON FUNCTION public.list_branches() TO PUBLIC;

COMMENT ON EXTENSION overlay_branch IS 'Overlay Branch - speculative database state using table overlay and delta store';
