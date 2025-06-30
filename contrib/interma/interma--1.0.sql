/* contrib/interma/interma--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION interma" to load this file. \quit

-- function to return debug info
CREATE FUNCTION my_info(
    OUT id integer,
    OUT name text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'my_info'
LANGUAGE C PARALLEL SAFE;

-- function to return backend status
CREATE FUNCTION my_backend_status()
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME', 'my_backend_status'
LANGUAGE C PARALLEL SAFE;
-- a view for convenient access.
CREATE VIEW my_backends AS
	SELECT P.* FROM my_backend_status() AS P
	(pid integer, query text, status text);

-- Don't want these to be available to public.
REVOKE ALL ON FUNCTION my_backend_status() FROM PUBLIC;
