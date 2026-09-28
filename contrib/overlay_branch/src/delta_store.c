/*-------------------------------------------------------------------------
 *
 * delta_store.c
 *	  Overlay Branch extension — Delta Store SPI 实现 + 行序列化 +
 *	  Write-Redirect 调 overlay_delta_* 的 bridge (overlay_modify_*)
 *
 *	  职责：
 *		- ob_spi_one_shot(): SPI connect/execute/finish 三合一
 *		- overlay_delta_insert/update/lookup/list_for_rel/count/delete_all
 *		  (真实 SPI UPSERT / SELECT / DELETE 到 overlay_branch.pg_branch_delta)
 *		- overlay_relation_has_pk(): Relation 是否有 Primary Key
 *		- rel_attno_to_slot_idx(): 处理 UPDATE/DELETE 子计划 slot 的 junk attrs
 *		- slot_get_ctid_cstr() / fetch_tuple_by_ctid(): 物理 row → clean slot
 *		- overlay_serialize_pk() / overlay_serialize_tuple(): PK / 行 → JSON text
 *		- reconstruct_slot_from_delta(): bytea tuple_data → relation-descr slot
 *		- overlay_tuple_version(): ctid + xmin → "blk:off-x<xmin>" token
 *		- overlay_should_redirect(): 6 层前置检查（写重定向前置 gate）
 *		- overlay_modify_insert/update/delete(): 写重定向调用 bridge
 *		- overlay_debug_delta_insert/count/delete_all(): Step2c smoke debug wrappers
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "overlay_branch.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "parser/parse_oper.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "nodes/execnodes.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/* ---------------------------------------------------------------
 * Bypass helpers: all internal SPI calls that must hit the MAIN
 * physical heap (NOT the overlay view) must push/pop
 * overlay_overlay_helper_enter() / exit() so the BranchScan planner
 * hook skips CustomScan injection for that internal query.
 * --------------------------------------------------------------- */
extern void overlay_overlay_helper_enter(void);
extern void overlay_overlay_helper_exit(void);

/* Fully qualified table names (control file pins schema = 'overlay_branch').
 * OBSCHEMA 定义在公共头 overlay_branch.h */
#define OBTABLE_DELTA   OBSCHEMA ".pg_branch_delta"
#define OBTABLE_BRANCH  OBSCHEMA ".pg_branch"

/* ================================================================
 * SPI 一次性 helper (所有 overlay_delta_* / apply 内部 SQL 共用)
 * ================================================================ */
int
ob_spi_one_shot(const char *sql, bool read_only, uint64 tcount)
{
	int			cret;
	int			eret;

	cret = SPI_connect();
	if (cret != SPI_OK_CONNECT)
		elog(ERROR, "overlay_delta: SPI_connect failed: %d", cret);

	eret = SPI_execute(sql, read_only, tcount);
	return eret;
}

/* ================================================================
 * Delta Store (6 个真实 SPI 函数)
 * ================================================================ */
void
overlay_delta_insert(int32 branch_id, Oid relid, const char *key,
					 char op, const char *old_version, const char *tuple_json_cstr)
{
	StringInfoData sql;
	char	   *q_key;
	char	   *q_oldver;
	int			ret;
	StringInfoData gsql;
	int			gret;

	if (branch_id <= 0)
		elog(ERROR, "overlay_delta_insert: invalid branch_id=%d", branch_id);

	initStringInfo(&gsql);
	appendStringInfo(&gsql,
					 "SELECT branch_id, state FROM " OBTABLE_BRANCH " "
					 "WHERE branch_id = %d LIMIT 1",
					 (int) branch_id);
	gret = ob_spi_one_shot(gsql.data, true, 1);
	if (gret != SPI_OK_SELECT)
	{
		SPI_finish();
		pfree(gsql.data);
		elog(ERROR, "overlay_delta_insert: bid=%d state lookup SPI ret=%d",
			 branch_id, gret);
	}
	if (SPI_processed == 0)
	{
		const char *bn;
		SPI_finish();
		bn = overlay_branch_get_current_name();
		pfree(gsql.data);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("overlay_delta_insert: branch_id=%d does not exist",
						branch_id),
				 errhint("Current session branch: \"%s\". "
						 "Call create_branch() first.",
						 bn ? bn : "<none>")));
	}
	else
	{
		bool		isnull;
		Datum		v;
		char	   *st;
		MemoryContext oldmc;

		/* mirror apply_branch Step0 pattern: SELECT branch_id, state → state is col 2 */
		v = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull);
		if (isnull)
		{
			SPI_finish();
			pfree(gsql.data);
			{
				const char *bn2 = overlay_branch_get_current_name();
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("overlay_delta_insert: bid=%d has NULL state "
								"(corrupt pg_branch row)",
								branch_id),
						 errhint("Current session branch: \"%s\".",
								 bn2 ? bn2 : "<none>")));
			}
		}
		/* copy detoasted text C string into a context that outlives SPI_finish
		 * (SPI proc context is torn down by finish; TopMemoryContext is safe,
		 * mirroring apply_branch Step0).  Otherwise the returned string
		 * pointer becomes dangling after SPI_finish, producing empty string /
		 * garbage compare. */
		oldmc = MemoryContextSwitchTo(TopMemoryContext);
		st = TextDatumGetCString(v);
		MemoryContextSwitchTo(oldmc);
		SPI_finish();
		if (st == NULL || strcmp(st, BRANCH_STATE_ACTIVE) != 0)
		{
			const char *bn;
			char	   *saved_st = st ? pstrdup(st) : pstrdup("<NULL>");

			pfree(gsql.data);
			bn = overlay_branch_get_current_name();
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("overlay_branch: cannot write delta rows to branch "
							"\"%s\" (bid=%d state=%s). "
							"Writes are only allowed while branch is ACTIVE.",
							bn ? bn : "<unresolved name>",
							branch_id, saved_st),
					 (st && strcmp(st, BRANCH_STATE_APPLYING) == 0
					  ? errhint("Branch is currently applying. "
								"Retry apply once the current apply_branch() "
								"caller has finished.")
					  : 0),
					 (st && (strcmp(st, BRANCH_STATE_APPLIED) == 0
							 || strcmp(st, BRANCH_STATE_DISCARDED) == 0)
					  ? errhint("Branch is in a terminal state (%s); "
								"no further writes allowed. Create a new branch.",
								saved_st)
					  : 0)));
		}
		if (st) pfree(st);
	}
	pfree(gsql.data);

	if (key == NULL)
		elog(ERROR, "overlay_delta_insert: key must not be NULL");
	if (op != DELTA_OP_INSERT && op != DELTA_OP_UPDATE && op != DELTA_OP_DELETE)
		elog(ERROR, "overlay_delta_insert: invalid op '%c'", op);

	q_key = quote_literal_cstr(key);
	if (old_version != NULL)
		q_oldver = quote_literal_cstr(old_version);
	else
		q_oldver = pstrdup("NULL");

	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "INSERT INTO " OBTABLE_DELTA " "
					 "(branch_id, relid, key, op, old_version, tuple_data) "
					 "VALUES (%d, %u, %s, '%c', %s, ",
					 branch_id, relid, q_key, op, q_oldver);

	if (tuple_json_cstr == NULL)
	{
		appendStringInfoString(&sql, "NULL");
	}
	else
	{
		char *q_json = quote_literal_cstr(tuple_json_cstr);
		appendStringInfo(&sql, "(%s)::bytea", q_json);
		pfree(q_json);
	}

	appendStringInfoString(&sql,
						   ") ON CONFLICT (branch_id, relid, key) DO UPDATE SET "
						   "op = EXCLUDED.op, "
						   "old_version = EXCLUDED.old_version, "
						   "tuple_data = EXCLUDED.tuple_data, "
						   "updated_at = now()");

	ereport(DEBUG1,
			(errmsg_internal("ODI[dbg] calling SPI, sql=%s", sql.data)));

	ret = ob_spi_one_shot(sql.data, false, 1);

	ereport(DEBUG1,
			(errmsg_internal("ODI[dbg] SPI_execute ret=%d processed=%lu",
							 ret, (unsigned long) SPI_processed)));

	if (ret != SPI_OK_INSERT && ret != SPI_OK_INSERT_RETURNING)
	{
		SPI_finish();
		elog(ERROR, "overlay_delta_insert: SPI_execute failed ret=%d", ret);
	}

	SPI_finish();

	pfree(sql.data);
	pfree(q_key);
	pfree(q_oldver);
}

void
overlay_delta_update(int32 branch_id, Oid relid, const char *key,
					 char op, const char *old_version, const char *tuple_json_cstr)
{
	overlay_delta_insert(branch_id, relid, key, op, old_version, tuple_json_cstr);
}

bool
overlay_delta_lookup(int32 branch_id, Oid relid, const char *key,
					 DeltaTuple *out_tuple)
{
	StringInfoData sql;
	char	   *q_key;
	int			ret;
	bool		found = false;

	if (key == NULL || out_tuple == NULL)
		elog(ERROR, "overlay_delta_lookup: key and out_tuple must not be NULL");

	q_key = quote_literal_cstr(key);
	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "SELECT branch_id, relid, key, op, old_version, tuple_data "
					 "FROM " OBTABLE_DELTA " "
					 "WHERE branch_id = %d AND relid = %u AND key = %s "
					 "LIMIT 1",
					 branch_id, relid, q_key);

	/* The outer caller may be a cursor whose DECLARE-time snapshot freezes
	 * snapshot->curcid before this same-txn WR INSERT ran.  Push the latest
	 * secondary snapshot (GetSnapshotData refreshes curcid to the current
	 * command id) so delta rows inserted by later commands remain visible.
	 * Push/Pop uses FirstSnapshotSet; PopActiveSnapshot auto-frees snapshots
	 * with zero active_count+regd_count (see snapmgr.c L743-763). */
	PushActiveSnapshot(GetLatestSnapshot());
	ret = ob_spi_one_shot(sql.data, true, 1);
	PopActiveSnapshot();
	if (ret != SPI_OK_SELECT)
	{
		SPI_finish();
		pfree(sql.data);
		pfree(q_key);
		elog(ERROR, "overlay_delta_lookup: SPI_execute failed ret=%d", ret);
	}

	if (SPI_processed > 0 && SPI_tuptable != NULL && SPI_tuptable->vals != NULL)
	{
		HeapTuple			tup = SPI_tuptable->vals[0];
		TupleDesc			td  = SPI_tuptable->tupdesc;
		bool				isnull;
		Datum				d;
		MemoryContext		oldmc;

		found = true;

		/* 必须和 list_for_rel 一样在 TopMemoryContext 分配，
		 * 否则 datumCopy / TextDatumGetCString 的 palloc 都在
		 * SPI proc 上下文，SPI_finish() 一调用就全部悬垂！ */
		oldmc = MemoryContextSwitchTo(TopMemoryContext);
		memset(out_tuple, 0, sizeof(DeltaTuple));
		out_tuple->typed_pk = NULL;
		out_tuple->typed_pk_n = -1;

		d = SPI_getbinval(tup, td, 1, &isnull);
		out_tuple->branch_id = isnull ? 0 : DatumGetInt32(d);

		d = SPI_getbinval(tup, td, 2, &isnull);
		out_tuple->relid = isnull ? InvalidOid : DatumGetObjectId(d);

		d = SPI_getbinval(tup, td, 3, &isnull);
		if (!isnull)
		{
			/* ZERO-based: SPI col 3 (key TEXT) → attrs[2]. */
			Form_pg_attribute katt = TupleDescAttr(td, 2);
			int16		typlen;
			bool		typbyval;

			get_typlenbyval(katt->atttypid, &typlen, &typbyval);
			out_tuple->key = TextDatumGetCString(datumCopy(d, typbyval, typlen));
		}
		else
			out_tuple->key = pstrdup("");

		d = SPI_getbinval(tup, td, 4, &isnull);
		if (isnull)
			out_tuple->op = '?';
		else
		{
			Oid			outoid;
			bool		outvarlena;
			char	   *opstr;

			getTypeOutputInfo(BPCHAROID, &outoid, &outvarlena);
			opstr = OidOutputFunctionCall(outoid, d);
			out_tuple->op = (opstr && opstr[0]) ? opstr[0] : '?';
			pfree(opstr);
		}

		d = SPI_getbinval(tup, td, 5, &isnull);
		if (!isnull)
		{
			/* SPI col 5 (old_version TEXT) = attrs[4]. */
			Form_pg_attribute vatt = TupleDescAttr(td, 4);
			int16		typlen;
			bool		typbyval;

			get_typlenbyval(vatt->atttypid, &typlen, &typbyval);
			out_tuple->old_version = TextDatumGetCString(datumCopy(d,
											   typbyval, typlen));
		}
		else
			out_tuple->old_version = NULL;

		d = SPI_getbinval(tup, td, 6, &isnull);
		if (!isnull)
		{
			/* SPI col 6 (tuple_data BYTEA) = attrs[5]. */
			Form_pg_attribute batt = TupleDescAttr(td, 5);
			int16		typlen;
			bool		typbyval;

			get_typlenbyval(batt->atttypid, &typlen, &typbyval);
			out_tuple->tuple_data = (bytea *) datumCopy(d,
										  typbyval, typlen);
		}
		else
			out_tuple->tuple_data = NULL;

		MemoryContextSwitchTo(oldmc);
	}

	SPI_finish();
	pfree(sql.data);
	pfree(q_key);
	return found;
}

List *
overlay_delta_list_for_rel(int32 branch_id, Oid relid)
{
	StringInfoData sql;
	int			ret;
	List	   *result = NIL;
	uint64		i;
	MemoryContext oldmc;

	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "SELECT branch_id, relid, key, op, old_version, tuple_data "
					 "FROM " OBTABLE_DELTA " "
					 "WHERE branch_id = %d AND relid = %u "
					 "ORDER BY key",
					 branch_id, relid);

	/* Match the Push/Pop discipline used in overlay_delta_lookup: force the
	 * secondary latest snapshot so delta rows appended by a later command id
	 * inside the same transaction (e.g. same-txn WR INSERT after a DECLARE
	 * CURSOR) become visible even when the outer statement's active snapshot
	 * still has the older, frozen curcid. */
	PushActiveSnapshot(GetLatestSnapshot());
	ret = ob_spi_one_shot(sql.data, true, 0);
	PopActiveSnapshot();
	if (ret != SPI_OK_SELECT)
	{
		SPI_finish();
		pfree(sql.data);
		elog(ERROR, "overlay_delta_list_for_rel: SPI_execute failed ret=%d", ret);
	}

	if (SPI_processed > 0 && SPI_tuptable != NULL && SPI_tuptable->vals != NULL)
	{
		TupleDesc	td = SPI_tuptable->tupdesc;

		/* 所有 DeltaTuple + key/tuple_data 必须在 TopMemoryContext 分配才能
		 * 活过 SPI_finish() */
		oldmc = MemoryContextSwitchTo(TopMemoryContext);

		for (i = 0; i < SPI_processed; i++)
		{
			HeapTuple			tup = SPI_tuptable->vals[i];
			DeltaTuple		   *dt;
			bool				isnull;
			Datum				d;

			dt = (DeltaTuple *) palloc0(sizeof(DeltaTuple));
			dt->typed_pk = NULL;
			dt->typed_pk_n = -1;

			d = SPI_getbinval(tup, td, 1, &isnull);
			dt->branch_id = isnull ? 0 : DatumGetInt32(d);

			d = SPI_getbinval(tup, td, 2, &isnull);
			dt->relid = isnull ? InvalidOid : DatumGetObjectId(d);

			d = SPI_getbinval(tup, td, 3, &isnull);
			if (!isnull)
			{
				Form_pg_attribute katt = TupleDescAttr(td, 2);
				int16		typlen;
				bool		typbyval;
				get_typlenbyval(katt->atttypid, &typlen, &typbyval);
				dt->key = TextDatumGetCString(datumCopy(d,
									 typbyval, typlen));
			}
			else
				dt->key = pstrdup("");

			d = SPI_getbinval(tup, td, 4, &isnull);
			if (isnull)
				dt->op = '?';
			else
			{
				Oid			outoid;
				bool		outvarlena;
				char	   *opstr;

				getTypeOutputInfo(BPCHAROID, &outoid, &outvarlena);
				opstr = OidOutputFunctionCall(outoid, d);
				dt->op = (opstr && opstr[0]) ? opstr[0] : '?';
				pfree(opstr);
			}

			d = SPI_getbinval(tup, td, 5, &isnull);
			if (!isnull)
			{
				Form_pg_attribute vatt = TupleDescAttr(td, 4);
				int16		typlen;
				bool		typbyval;
				get_typlenbyval(vatt->atttypid, &typlen, &typbyval);
				dt->old_version = TextDatumGetCString(datumCopy(d,
											 typbyval, typlen));
			}
			else
				dt->old_version = NULL;

			d = SPI_getbinval(tup, td, 6, &isnull);
			if (!isnull)
			{
				Form_pg_attribute batt = TupleDescAttr(td, 5);
				int16		typlen;
				bool		typbyval;
				get_typlenbyval(batt->atttypid, &typlen, &typbyval);
				dt->tuple_data = (bytea *) datumCopy(d,
									   typbyval, typlen);
			}
			else
				dt->tuple_data = NULL;

			result = lappend(result, dt);
		}
		MemoryContextSwitchTo(oldmc);
	}

	SPI_finish();
	pfree(sql.data);
	return result;
}

int64
overlay_delta_count(int32 branch_id)
{
	StringInfoData sql;
	int			ret;
	int64		result = 0;

	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "SELECT count(*)::bigint FROM " OBTABLE_DELTA " "
					 "WHERE branch_id = %d",
					 branch_id);

	ret = ob_spi_one_shot(sql.data, true, 1);
	if (ret != SPI_OK_SELECT)
	{
		SPI_finish();
		pfree(sql.data);
		elog(ERROR, "overlay_delta_count: SPI_execute failed ret=%d", ret);
	}

	if (SPI_processed > 0 && SPI_tuptable != NULL && SPI_tuptable->vals != NULL)
	{
		bool	isnull;
		Datum	d = SPI_getbinval(SPI_tuptable->vals[0],
								  SPI_tuptable->tupdesc, 1, &isnull);
		if (!isnull)
			result = DatumGetInt64(d);
	}

	SPI_finish();
	pfree(sql.data);
	return result;
}

void
overlay_delta_delete_all(int32 branch_id)
{
	StringInfoData sql;
	int			ret;

	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "DELETE FROM " OBTABLE_DELTA " WHERE branch_id = %d",
					 branch_id);

	ret = ob_spi_one_shot(sql.data, false, 1);
	if (ret != SPI_OK_DELETE)
	{
		SPI_finish();
		pfree(sql.data);
		elog(ERROR, "overlay_delta_delete_all: SPI_execute failed ret=%d", ret);
	}

	elog(DEBUG1, "overlay_delta_delete_all: branch=%d deleted=%lu rows",
		 branch_id, (unsigned long) SPI_processed);
	SPI_finish();
	pfree(sql.data);
}

/* ================================================================
 * Slot / junk-attribute helpers (UPDATE/DELETE 写重定向需要)
 * ================================================================ */
AttrNumber
rel_attno_to_slot_idx(TupleDesc slotdesc, AttrNumber rel_attno)
{
	int			natts = slotdesc->natts;

	for (int j = 0; j < natts; j++)
	{
		Form_pg_attribute satt = TupleDescAttr(slotdesc, j);
		if (satt->attisdropped)
			continue;
		if (satt->attnum == rel_attno)
			return (AttrNumber) (j + 1);
	}
	return 0;
}

char *
slot_get_ctid_cstr(TupleTableSlot *slot)
{
	TupleDesc	td = slot->tts_tupleDescriptor;
	int			natts = td->natts;
	int			j;

	for (j = 0; j < natts; j++)
	{
		Form_pg_attribute satt = TupleDescAttr(td, j);

		if (satt->attisdropped)
			continue;
		if (strcmp(NameStr(satt->attname), "ctid") == 0)
		{
			bool		isnull;
			Datum		d = slot_getattr(slot, (AttrNumber) (j + 1), &isnull);
			ItemPointerData ip;

			if (isnull)
				elog(ERROR, "slot_get_ctid_cstr: ctid IS NULL (cannot locate row)");

			ip = *DatumGetItemPointer(d);
			{
				char	   *out = (char *) palloc(32);

				snprintf(out, 32, "(%u,%u)",
						 ItemPointerGetBlockNumber(&ip),
						 ItemPointerGetOffsetNumber(&ip));
				return out;
			}
		}
	}
	return NULL;
}

TupleTableSlot *
fetch_tuple_by_ctid(Relation rel, const char *ctid_cstr)
{
	StringInfoData sql;
	int			ret;
	TupleTableSlot *out_slot;
	TupleDesc	reldesc = RelationGetDescr(rel);
	char	   *q_relname;
	char	   *q_ctid;

	overlay_overlay_helper_enter();

	initStringInfo(&sql);
	q_relname = quote_qualified_identifier(
		get_namespace_name(RelationGetNamespace(rel)),
		RelationGetRelationName(rel));
	q_ctid = quote_literal_cstr(ctid_cstr);

	appendStringInfo(&sql, "SELECT * FROM %s WHERE ctid = %s::tid LIMIT 1",
					 q_relname, q_ctid);
	pfree(q_relname);
	pfree(q_ctid);

	ret = ob_spi_one_shot(sql.data, true, 1);
	pfree(sql.data);

	if (ret != SPI_OK_SELECT || SPI_processed != 1 ||
		SPI_tuptable == NULL || SPI_tuptable->vals == NULL)
	{
		SPI_finish();
		overlay_overlay_helper_exit();
		elog(ERROR, "fetch_tuple_by_ctid: SPI SELECT-by-ctid failed (ret=%d rows=%lu) ctid=%s",
			 ret, SPI_processed != 0 ? (unsigned long) SPI_processed : 0UL,
			 ctid_cstr);
	}

	out_slot = NULL;
	{
		HeapTuple	htup = SPI_tuptable->vals[0];
		TupleDesc	td_spi = SPI_tuptable->tupdesc;
		MemoryContext oldmc;

		oldmc = MemoryContextSwitchTo(TopMemoryContext);
		{
			TupleDesc	slot_desc = CreateTupleDescCopy(reldesc);

			out_slot = MakeSingleTupleTableSlot(slot_desc, &TTSOpsVirtual);
		}
		ExecClearTuple(out_slot);
		for (int a = 0; a < reldesc->natts; a++)
		{
			bool			isnull;
			Form_pg_attribute ratt = TupleDescAttr(reldesc, a);
			Datum			d;

			d = SPI_getbinval(htup, td_spi, a + 1, &isnull);
			out_slot->tts_isnull[a] = isnull;
			if (!isnull)
			{
				int16		typlen;
				bool		typbyval;

				get_typlenbyval(ratt->atttypid, &typlen, &typbyval);
				d = datumCopy(d, typbyval, typlen);
			}
			out_slot->tts_values[a] = d;
		}
		ExecStoreVirtualTuple(out_slot);
		out_slot->tts_nvalid = reldesc->natts;
		out_slot->tts_tid = htup->t_self;
		MemoryContextSwitchTo(oldmc);
	}
	SPI_finish();
	overlay_overlay_helper_exit();
	return out_slot;
}

TupleTableSlot *
reconstruct_slot_from_delta(Relation rel, bytea *tuple_data)
{
	StringInfoData sql;
	StringInfoData cols;
	TupleDesc	reldesc = RelationGetDescr(rel);
	int			ret;
	TupleTableSlot *out_slot;
	int			natts;
	MemoryContext oldmc;
	HeapTuple	htup;
	TupleDesc	td_spi;
	Oid			bytea_out_func;
	bool		bytea_out_varlena;
	char	   *bytea_sql_lit;
	char	   *convert_from_expr;

	Assert(rel != NULL && tuple_data != NULL);
	natts = reldesc->natts;

	/* 构建列规格 colname sqltype,... */
	initStringInfo(&cols);
	for (int a = 0; a < natts; a++)
	{
		Form_pg_attribute ratt = TupleDescAttr(reldesc, a);

		if (ratt->attisdropped)
			continue;
		if (cols.len > 0)
			appendStringInfoChar(&cols, ',');
		appendStringInfo(&cols, "%s %s",
						 quote_identifier(NameStr(ratt->attname)),
						 format_type_with_typemod(ratt->atttypid,
												  ratt->atttypmod));
	}

	/* 用 PG 自带 bytea output 生成 '\xHHHH' 字面量，兼容所有 varlena 格式 */
	getTypeOutputInfo(BYTEAOID, &bytea_out_func, &bytea_out_varlena);
	bytea_sql_lit = OidOutputFunctionCall(bytea_out_func,
										  PointerGetDatum(tuple_data));
	convert_from_expr = psprintf("convert_from('%s'::bytea, 'UTF8')::jsonb",
								 bytea_sql_lit);
	pfree(bytea_sql_lit);

	/* 一次性 SPI 把 JSON blob → typed 单列结果 */
	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "SELECT (x).* FROM jsonb_to_record(%s) AS x(%s) LIMIT 1",
					 convert_from_expr, cols.data);
	pfree(convert_from_expr);
	pfree(cols.data);

	ret = ob_spi_one_shot(sql.data, true, 1);
	pfree(sql.data);

	if (ret != SPI_OK_SELECT || SPI_processed != 1 ||
		SPI_tuptable == NULL || SPI_tuptable->vals == NULL)
	{
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("overlay_branch: failed to reconstruct tuple from delta JSON (SPI ret=%d rows=%lu)",
						ret, (unsigned long) SPI_processed)));
	}

	oldmc = MemoryContextSwitchTo(TopMemoryContext);
	{
		TupleDesc	slot_desc = CreateTupleDescCopy(reldesc);

		out_slot = MakeSingleTupleTableSlot(slot_desc, &TTSOpsVirtual);
	}
	ExecClearTuple(out_slot);
	htup = SPI_tuptable->vals[0];
	td_spi = SPI_tuptable->tupdesc;

	for (int a = 0; a < natts; a++)
	{
		bool			isnull;
		Form_pg_attribute ratt = TupleDescAttr(reldesc, a);
		Datum			d;

		d = SPI_getbinval(htup, td_spi, a + 1, &isnull);
		out_slot->tts_isnull[a] = isnull;
		if (!isnull)
		{
			int16		typlen;
			bool		typbyval;
			get_typlenbyval(ratt->atttypid, &typlen, &typbyval);
			d = datumCopy(d, typbyval, typlen);
		}
		out_slot->tts_values[a] = d;
	}
	ExecStoreVirtualTuple(out_slot);
	out_slot->tts_nvalid = natts;
	MemoryContextSwitchTo(oldmc);

	SPI_finish();
	return out_slot;
}

bool
overlay_relation_has_pk(Relation rel)
{
	List	   *indexoids;
	ListCell   *lc;
	bool		found = false;

	if (rel == NULL)
		return false;

	indexoids = RelationGetIndexList(rel);
	foreach(lc, indexoids)
	{
		Oid			idxoid = lfirst_oid(lc);
		Relation	idxrel;

		idxrel = index_open(idxoid, AccessShareLock);
		if (idxrel->rd_index && idxrel->rd_index->indisprimary)
		{
			found = true;
			index_close(idxrel, AccessShareLock);
			break;
		}
		index_close(idxrel, AccessShareLock);
	}
	list_free(indexoids);
	return found;
}

/* ================================================================
 * PK / row 序列化 (JSON)
 * ================================================================ */
char *
overlay_serialize_pk(Relation rel, TupleTableSlot *slot)
{
	List	   *indexoids;
	ListCell   *lc;
	Oid			pk_index_oid = InvalidOid;
	Relation	pk_rel = NULL;
	Datum	   *pk_datums;
	bool	   *pk_isnulls;
	int			n_pk;
	StringInfoData arr_sql;
	StringInfoData qbuf;
	int			ret;
	char	   *result;

	Assert(rel != NULL && slot != NULL);
	if (!overlay_relation_has_pk(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("overlay_serialize_pk requires a primary key on %s",
						RelationGetRelationName(rel))));

	indexoids = RelationGetIndexList(rel);
	foreach(lc, indexoids)
	{
		Oid			idxoid = lfirst_oid(lc);
		Relation	idxrel;

		idxrel = index_open(idxoid, AccessShareLock);
		if (idxrel->rd_index && idxrel->rd_index->indisprimary)
		{
			pk_index_oid = idxoid;
			pk_rel = idxrel;
			break;
		}
		index_close(idxrel, AccessShareLock);
	}
	Assert(OidIsValid(pk_index_oid) && pk_rel != NULL);

	n_pk = pk_rel->rd_index->indnatts;
	pk_datums = (Datum *) palloc0(sizeof(Datum) * n_pk);
	pk_isnulls = (bool *) palloc0(sizeof(bool) * n_pk);

	for (int i = 0; i < n_pk; i++)
		{
			AttrNumber	attno = pk_rel->rd_index->indkey.values[i];
			AttrNumber	slot_idx = rel_attno_to_slot_idx(slot->tts_tupleDescriptor, attno);

			if (slot_idx == 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("overlay_serialize_pk: cannot find PK column (attno=%d) inside UPDATE/DELETE junk-column slot (natts=%d)",
								attno, slot->tts_tupleDescriptor->natts)));

			pk_datums[i] = slot_getattr(slot, slot_idx, &pk_isnulls[i]);
		}

	initStringInfo(&arr_sql);
	appendStringInfoString(&arr_sql, "SELECT to_jsonb(ARRAY[");

	for (int i = 0; i < n_pk; i++)
	{
		if (i > 0)
			appendStringInfoChar(&arr_sql, ',');

		if (pk_isnulls[i])
			appendStringInfoString(&arr_sql, "NULL::text");
		else
		{
			AttrNumber	attno = pk_rel->rd_index->indkey.values[i];
			Oid			typid = TupleDescAttr(RelationGetDescr(rel),
											  attno - 1)->atttypid;
			Oid			outfuncoid;
			bool		typeIsVarlena;
			char	   *valstr;
			char	   *q;

			getTypeOutputInfo(typid, &outfuncoid, &typeIsVarlena);
			valstr = OidOutputFunctionCall(outfuncoid, pk_datums[i]);
			/* BPCHAR r-trim: PG bpchar→text cast silently strips trailing
			 * blanks, so P0 fast-path (which casts via ::text) would
			 * produce 'AB12' for a char(8) value 'AB12    '.  To match we
			 * r-trim the WRITE-side output string (bpcharout keeps the
			 * full N-blank-padded length).  Since bpchar equality ignores
			 * trailing blanks anyway, this preserves PK semantics. */
			if (typid == BPCHAROID)
			{
				int len = strlen(valstr);
				while (len > 0 && valstr[len-1] == ' ')
					valstr[--len] = '\0';
			}
			q = quote_literal_cstr(valstr);
			appendStringInfo(&arr_sql, "%s::text", q);
			pfree(q);
			pfree(valstr);
		}
	}
	appendStringInfoString(&arr_sql, "])::text");

	index_close(pk_rel, AccessShareLock);
	list_free(indexoids);
	pfree(pk_datums);
	pfree(pk_isnulls);

	initStringInfo(&qbuf);
	ret = ob_spi_one_shot(arr_sql.data, true, 1);
	pfree(arr_sql.data);

	if (ret != SPI_OK_SELECT || SPI_processed != 1)
	{
		pfree(qbuf.data);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_serialize_pk: SPI to_jsonb failed (ret=%d, rows=%lu)",
						ret, (unsigned long) SPI_processed)));
	}
	{
		const char *spival = SPI_getvalue(SPI_tuptable->vals[0],
										  SPI_tuptable->tupdesc, 1);
		MemoryContext oldmc;

		oldmc = MemoryContextSwitchTo(TopMemoryContext);
		result = pstrdup(spival != NULL ? spival : "");
		MemoryContextSwitchTo(oldmc);
	}
	SPI_finish();
	return result;
}

char *
overlay_serialize_tuple(Relation rel, TupleTableSlot *slot)
{
	TupleDesc	reldesc;
	int			natts;
	StringInfoData jsonbuf;
	StringInfoData scratch;
	bool		first;
	int			len;
	const char *data;
	char	   *result;

	Assert(rel != NULL && slot != NULL);
	ExecMaterializeSlot(slot);
	if (TTS_IS_VIRTUAL(slot))
	{
		TupleDesc	std = slot->tts_tupleDescriptor;
		if (std && slot->tts_nvalid < std->natts)
			slot->tts_nvalid = std->natts;
	}

	reldesc = RelationGetDescr(rel);
	natts = reldesc->natts;

	initStringInfo(&jsonbuf);
	initStringInfo(&scratch);
	appendStringInfoChar(&jsonbuf, '{');
	first = true;

	for (int i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(reldesc, i);
		Datum		d;
		bool		isnull_col;

		if (att->attisdropped)
			continue;

		if (!first)
			appendStringInfoChar(&jsonbuf, ',');
		first = false;

		{
			const char *aname = NameStr(att->attname);

			appendStringInfoChar(&jsonbuf, '"');
			for (const char *p = aname; *p != '\0'; p++)
			{
				if (*p == '"' || *p == '\\')
					appendStringInfoChar(&jsonbuf, '\\');
				appendStringInfoChar(&jsonbuf, *p);
			}
			appendStringInfoChar(&jsonbuf, '"');
			appendStringInfoChar(&jsonbuf, ':');
		}

		/* 使用 NameStr 匹配 att→attnum → slot index，避免 junk attrs 错列 */
		{
			AttrNumber	slot_idx = rel_attno_to_slot_idx(
									  slot->tts_tupleDescriptor,
									  att->attnum);

			if (slot_idx == 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("overlay_serialize_tuple: cannot find user column attno=%d name=%s inside UPDATE/DELETE junk-column slot (slot_natts=%d)",
								att->attnum, NameStr(att->attname),
								slot->tts_tupleDescriptor->natts)));

			d = slot_getattr(slot, slot_idx, &isnull_col);
		}
		if (isnull_col)
		{
			appendStringInfoString(&jsonbuf, "null");
		}
		else
		{
			Oid			typid = att->atttypid;
			Oid			outfuncoid;
			bool		typeIsVarlena;
			char	   *valstr;
			const char *vp;

			getTypeOutputInfo(typid, &outfuncoid, &typeIsVarlena);
			valstr = OidOutputFunctionCall(outfuncoid, d);

			resetStringInfo(&scratch);
			appendStringInfoChar(&scratch, '"');
			for (vp = valstr; *vp != '\0'; vp++)
			{
				unsigned char c = (unsigned char) *vp;

				switch (c)
				{
					case '"':	appendStringInfoString(&scratch, "\\\""); break;
					case '\\':	appendStringInfoString(&scratch, "\\\\"); break;
					case '\n':	appendStringInfoString(&scratch, "\\n");  break;
					case '\r':	appendStringInfoString(&scratch, "\\r");  break;
					case '\t':	appendStringInfoString(&scratch, "\\t");  break;
					case '\b':	appendStringInfoString(&scratch, "\\b");  break;
					case '\f':	appendStringInfoString(&scratch, "\\f");  break;
					default:
						if (c < 0x20)
							appendStringInfo(&scratch, "\\u%04x", c);
						else
							appendStringInfoChar(&scratch, (char) c);
				}
			}
			appendStringInfoChar(&scratch, '"');
			appendStringInfoString(&jsonbuf, scratch.data);
			pfree(valstr);
		}
	}
	appendStringInfoChar(&jsonbuf, '}');
	pfree(scratch.data);

	data = jsonbuf.data;
	len  = jsonbuf.len;
	{
		MemoryContext oldmc;

		oldmc = MemoryContextSwitchTo(TopMemoryContext);
		result = (char *) palloc((Size) len + 1);
		if (len > 0)
			memcpy(result, data, (Size) len);
		result[len] = '\0';
		MemoryContextSwitchTo(oldmc);
	}

	ereport(DEBUG1,
			(errmsg_internal("OST[dbg] json=%s len=%d",
							 result, len)));

	pfree(jsonbuf.data);
	return result;
}

/* ================================================================
 * overlay_tuple_version: 物理 ctid + xmin 构建可比较版本串
 * ================================================================ */
char *
overlay_tuple_version(Relation rel, TupleTableSlot *slot)
{
	StringInfoData sql;
	int			ret;
	char	   *q_relname;
	char	   *q_ctid;
	char	   *ctid_cstr;
	char	   *result = NULL;
	bool		ctid_cstr_allocated = false;

	if (rel == NULL || slot == NULL)
		return NULL;

	ctid_cstr = slot_get_ctid_cstr(slot);
	if (ctid_cstr == NULL)
	{
		ItemPointer ip = &(slot->tts_tid);

		if (!ItemPointerIsValid(ip) ||
			ItemPointerGetBlockNumber(ip) == InvalidBlockNumber ||
			ItemPointerGetOffsetNumber(ip) == 0)
			return NULL;
		ctid_cstr = (char *) palloc(32);
		snprintf(ctid_cstr, 32, "(%u,%u)",
				 ItemPointerGetBlockNumber(ip),
				 ItemPointerGetOffsetNumber(ip));
		ctid_cstr_allocated = true;
	}

	overlay_overlay_helper_enter();

	initStringInfo(&sql);
	q_relname = quote_qualified_identifier(
		get_namespace_name(RelationGetNamespace(rel)),
		RelationGetRelationName(rel));
	q_ctid = quote_literal_cstr(ctid_cstr);
	appendStringInfo(&sql,
					 "SELECT ctid::text, xmin::text FROM %s WHERE ctid = %s::tid LIMIT 1",
					 q_relname, q_ctid);
	pfree(q_relname);
	pfree(q_ctid);

	ret = ob_spi_one_shot(sql.data, true, 1);
	pfree(sql.data);

	if (ret == SPI_OK_SELECT && SPI_processed == 1 &&
		SPI_tuptable && SPI_tuptable->vals && SPI_tuptable->vals[0])
	{
		bool		isnull1, isnull2;
		char	   *ctid_txt;
		char	   *xmin_txt;
		uint32		blk, off;

		ctid_txt = SPI_getvalue(SPI_tuptable->vals[0],
								SPI_tuptable->tupdesc, 1);
		xmin_txt = SPI_getvalue(SPI_tuptable->vals[0],
								SPI_tuptable->tupdesc, 2);
		isnull1 = (ctid_txt == NULL);
		isnull2 = (xmin_txt == NULL);

		if (!isnull1 && !isnull2 &&
			sscanf(ctid_txt, "(%u,%u)", &blk, &off) == 2)
		{
			char *tmp = psprintf("%u:%u-x%s", blk, off, xmin_txt);

			if (tmp != NULL)
			{
				MemoryContext oldmc3;

				oldmc3 = MemoryContextSwitchTo(TopMemoryContext);
				result = pstrdup(tmp);
				MemoryContextSwitchTo(oldmc3);
			}
		}
	}

	SPI_finish();
	overlay_overlay_helper_exit();
	if (ctid_cstr_allocated && ctid_cstr)
		pfree(ctid_cstr);
	else if (ctid_cstr)
		pfree(ctid_cstr);
	return result;
}

/* ================================================================
 * overlay_should_redirect：写重定向的快速判定 gate
 * ================================================================ */
bool
overlay_should_redirect(Relation rel)
{
	Oid			nspoid;
	char	   *nspname;

	/* 第一层：apply 内部的 MAIN 真实写回放必须绝对绕过，否则写入又被吃进 delta 死循环 */
	if (overlay_in_apply_operation())
		return false;
	if (!overlay_branch_is_active())
		return false;
	if (rel == NULL)
		return false;
	if (rel->rd_rel->relisshared)
		return false;

	/* 第二层：绝不要把对 overlay_branch.* catalog 的写又当成 delta 存 */
	nspoid = RelationGetNamespace(rel);
	nspname = get_namespace_name(nspoid);
	if (nspname != NULL && strcmp(nspname, OBSCHEMA) == 0)
		return false;

	/* 第三层：MVP 仅 regular heap 表 */
	if (rel->rd_rel->relkind != RELKIND_RELATION)
		return false;

	/* 第四层：无 PK 直接拒绝（Guard G6 在 ExecutorRun 入口已经 ERROR 了，这里保守 belt-and-braces） */
	if (!overlay_relation_has_pk(rel))
		return false;

	return true;
}

/* ================================================================
 * overlay_modify_insert/update/delete — Write Redirect → DeltaStore bridges
 * ================================================================ */
void
overlay_modify_insert(Relation rel, TupleTableSlot *slot)
{
	int32		bid = overlay_branch_get_current_id();
	char	   *pk;
	char	   *tdata;

	pk = overlay_serialize_pk(rel, slot);
	tdata = overlay_serialize_tuple(rel, slot);

	overlay_delta_insert(bid, RelationGetRelid(rel), pk,
						 DELTA_OP_INSERT, NULL, tdata);
	pfree(pk);
	pfree(tdata);
}

/* ob_enrich_tuple_with_base_image: attach "_base" top-level key to the
 * new-image JSON produced by overlay_serialize_tuple().  Used for
 * UPDATE/DELETE deltas so apply can compare the persisted MAIN base
 * image with the currently-locked MAIN row.  INSERTs carry no base
 * image (they have no MAIN baseline to compare against).
 *
 * Produces output like:  {"c1":"v1","c2":"v2","_base":{"c1":"old1","c2":"old2"}}
 * Caller is responsible for pfree()'ing the returned palloc'd cstring. */
static char *
ob_enrich_tuple_with_base_image(const char *new_image_json,
								const char *base_image_json)
{
	StringInfoData out;
	const char *p;
	bool		inside_string;
	bool		escaped;
	bool		object_closed;
	int			depth;

	if (new_image_json == NULL)
		return NULL;
	if (base_image_json == NULL || *base_image_json == '\0')
		return pstrdup(new_image_json);

	initStringInfo(&out);

	/* Walk the new-image JSON up to its final '}', strip a trailing '}',
	 * append ,"_base":<base_json>, then close with '}'.  We verify strict
	 * object syntax by tracking brace depth and string/escape state. */
	inside_string = false;
	escaped = false;
	depth = 0;
	object_closed = false;
	for (p = new_image_json; *p != '\0'; p++)
	{
		char c = *p;

		if (inside_string)
		{
			appendStringInfoChar(&out, c);
			if (escaped)
				escaped = false;
			else if (c == '\\')
				escaped = true;
			else if (c == '"')
				inside_string = false;
			continue;
		}
		switch (c)
		{
			case '"':
				inside_string = true;
				appendStringInfoChar(&out, c);
				break;
			case '{':
				depth++;
				appendStringInfoChar(&out, c);
				break;
			case '}':
				depth--;
				if (depth == 0)
				{
					/* Replace the final closing '}' with our enriched tail. */
					appendStringInfoString(&out, ",\"_base\":");
					appendStringInfoString(&out, base_image_json);
					appendStringInfoChar(&out, '}');
					object_closed = true;
				}
				else
					appendStringInfoChar(&out, c);
				break;
			default:
				appendStringInfoChar(&out, c);
				break;
		}
	}

	if (!object_closed)
	{
		/* Malformed input — fall back to returning original without base. */
		pfree(out.data);
		return pstrdup(new_image_json);
	}
	return out.data;
}

void
overlay_modify_update(Relation rel, TupleTableSlot *oldslot,
					  TupleTableSlot *newslot)
{
	int32		bid = overlay_branch_get_current_id();
	char	   *pk;
	char	   *oldver;
	char	   *tdata_new;
	char	   *tdata_base;
	char	   *tdata_enriched;

	pk = overlay_serialize_pk(rel, newslot);
	oldver = overlay_tuple_version(rel, oldslot);
	tdata_new = overlay_serialize_tuple(rel, newslot);
	tdata_base = overlay_serialize_tuple(rel, oldslot);
	tdata_enriched = ob_enrich_tuple_with_base_image(tdata_new, tdata_base);

	overlay_delta_insert(bid, RelationGetRelid(rel), pk,
						 DELTA_OP_UPDATE, oldver, tdata_enriched);
	pfree(pk);
	if (oldver) pfree(oldver);
	pfree(tdata_new);
	pfree(tdata_base);
	pfree(tdata_enriched);
}

void
overlay_modify_delete(Relation rel, TupleTableSlot *slot)
{
	int32		bid = overlay_branch_get_current_id();
	char	   *pk;
	char	   *oldver;
	char	   *base_json;
	char	   *sentinel_new;
	char	   *tdata_enriched;

	pk = overlay_serialize_pk(rel, slot);
	oldver = overlay_tuple_version(rel, slot);
	base_json = overlay_serialize_tuple(rel, slot);

	/* DELETEs carry no new-image columns.  We still want the base image
	 * attached for apply conflict detection, so we synthesise an empty
	 * new-image object {"_deleted":true} and enrich it with _base.  The
	 * apply INSERT fallback path already guards on dt->op; jsonb_populate_
	 * record() ignores extra keys it doesn't recognise, so the sentinel
	 * will not corrupt existing reconstructions. */
	sentinel_new = pstrdup("{\"_deleted\":true}");
	tdata_enriched = ob_enrich_tuple_with_base_image(sentinel_new, base_json);

	overlay_delta_insert(bid, RelationGetRelid(rel), pk,
						 DELTA_OP_DELETE, oldver, tdata_enriched);
	pfree(pk);
	if (oldver) pfree(oldver);
	pfree(base_json);
	pfree(sentinel_new);
	pfree(tdata_enriched);
}

/* ================================================================
 * overlay_debug_delta_*：Step 2c smoke debug wrappers
 * ================================================================ */

#include "fmgr.h"
PG_FUNCTION_INFO_V1(overlay_debug_delta_insert);
PG_FUNCTION_INFO_V1(overlay_debug_delta_count);
PG_FUNCTION_INFO_V1(overlay_debug_delta_delete_all);

Datum
overlay_debug_delta_insert(PG_FUNCTION_ARGS)
{
	int32		branch_id   = PG_GETARG_INT32(0);
	Oid			relid       = PG_GETARG_OID(1);
	text	   *key_txt     = PG_GETARG_TEXT_PP(2);
	char		op          = PG_GETARG_CHAR(3);
	const char *old_version = NULL;
	bytea	   *tuple_data  = NULL;
	char	   *key;

	elog(DEBUG1, "ODI_ENTRY: branch=%d rel=%u op=%c", branch_id, relid, op);

	if (!PG_ARGISNULL(4))
		old_version = text_to_cstring(PG_GETARG_TEXT_PP(4));

	if (!PG_ARGISNULL(5))
		tuple_data = PG_GETARG_BYTEA_PP(5);

	key = text_to_cstring(key_txt);
	if (key == NULL || *key == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("overlay_debug_delta_insert: key must not be empty")));

	elog(DEBUG1, "overlay_debug_delta_insert: branch=%d rel=%u key='%s' op='%c' oldver=%s tdatalen=%d",
		 branch_id, relid, key, op,
		 old_version ? old_version : "(null)",
		 tuple_data ? (int) VARSIZE_ANY_EXHDR(tuple_data) : -1);

	{
		const char *tuple_json_cstr = NULL;

		if (tuple_data != NULL)
		{
			Size payload_len = VARSIZE_ANY_EXHDR(tuple_data);
			const unsigned char *payload = (const unsigned char *) VARDATA_ANY(tuple_data);

			tuple_json_cstr = (const char *) palloc(payload_len + 1);
			if (payload_len > 0)
				memcpy(unconstify(char *, tuple_json_cstr), payload, payload_len);
			unconstify(char *, tuple_json_cstr)[payload_len] = '\0';
		}

		overlay_delta_insert(branch_id, relid, key, op, old_version, tuple_json_cstr);
		PG_RETURN_TEXT_P(cstring_to_text_with_len("OK", 2));
	}
}

Datum
overlay_debug_delta_count(PG_FUNCTION_ARGS)
{
	int32		branch_id = PG_GETARG_INT32(0);
	int64		n;

	n = overlay_delta_count(branch_id);
	PG_RETURN_INT64(n);
}

Datum
overlay_debug_delta_delete_all(PG_FUNCTION_ARGS)
{
	int32		branch_id = PG_GETARG_INT32(0);

	overlay_delta_delete_all(branch_id);
	PG_RETURN_VOID();
}

/* ================================================================
 * Planner 侧 PK 辅助函数（BranchScan PK IndexScan 适配新增）
 * ================================================================ */

/*
 * overlay_get_pk_single_attno
 *
 *   快速返回（MVP 限定：单列表的 PRIMARY KEY 列的 attno + 列名。
 *   复合 PK（indnatts>1）返回 false，Planner 就走全量路径。
 *   out_pk_attno 是 1-based 真实 attno（匹配 rd_index->indkey.values[i]）；
 *   out_pk_colname 指向 relcache 内的 NameData（caller 不得 pfree）。
 */
bool
overlay_get_pk_single_attno(Relation rel,
							AttrNumber *out_pk_attno,
							const char **out_pk_colname)
{
	List	   *indexoids;
	ListCell   *lc;
	Oid			pk_index_oid = InvalidOid;
	Relation	pk_rel = NULL;
	bool		found = false;

	if (rel == NULL)
		return false;
	if (out_pk_attno) *out_pk_attno = 0;
	if (out_pk_colname) *out_pk_colname = NULL;

	indexoids = RelationGetIndexList(rel);
	foreach(lc, indexoids)
	{
		Oid			idxoid = lfirst_oid(lc);
		Relation	idxrel;

		idxrel = index_open(idxoid, AccessShareLock);
		if (idxrel->rd_index && idxrel->rd_index->indisprimary)
		{
			pk_index_oid = idxoid;
			pk_rel = idxrel;
			break;
		}
		index_close(idxrel, AccessShareLock);
	}
	list_free(indexoids);

	if (!OidIsValid(pk_index_oid) || pk_rel == NULL)
		return false;

	/* MVP: 仅支持单列 PK */
	if (pk_rel->rd_index->indnatts == 1)
	{
		AttrNumber	pk_attno = pk_rel->rd_index->indkey.values[0];
		TupleDesc	reldesc = RelationGetDescr(rel);
		Form_pg_attribute pkatt;

		if (pk_attno > 0 && pk_attno <= reldesc->natts)
		{
			pkatt = TupleDescAttr(reldesc, pk_attno - 1);
			if (out_pk_attno)
				*out_pk_attno = pk_attno;
			if (out_pk_colname)
				*out_pk_colname = NameStr(pkatt->attname);
			found = true;
		}
	}

	index_close(pk_rel, AccessShareLock);
	return found;
}

/*
 * overlay_get_pk_attnos_list
 *
 *   Returns a list of 1-based attno integers for every column in the
 *   relation's PRIMARY KEY (supports composite PKs).  Returns NIL if
 *   the relation has no primary key.
 *
 *   Caller is responsible for list_free_deep() / pfree-ing the result
 *   (a List in CurrentMemoryContext).
 *
 *   This is used by write_redirect.c R03 PK-mutation guard to detect
 *   when a SET targetlist includes ANY of the PK columns. */
List *
overlay_get_pk_attnos_list(Relation rel)
{
	List	   *result = NIL;
	List	   *indexoids;
	ListCell   *lc;
	Oid			pk_index_oid = InvalidOid;
	Relation	pk_rel = NULL;

	if (rel == NULL)
		return NIL;

	indexoids = RelationGetIndexList(rel);
	foreach(lc, indexoids)
	{
		Oid			idxoid = lfirst_oid(lc);
		Relation	idxrel;

		idxrel = index_open(idxoid, AccessShareLock);
		if (idxrel->rd_index && idxrel->rd_index->indisprimary)
		{
			pk_index_oid = idxoid;
			pk_rel = idxrel;
			break;
		}
		index_close(idxrel, AccessShareLock);
	}
	list_free(indexoids);

	if (!OidIsValid(pk_index_oid) || pk_rel == NULL)
		return NIL;

	{
		int			n = pk_rel->rd_index->indnatts;
		int			i;

		for (i = 0; i < n; i++)
		{
			AttrNumber	attno = pk_rel->rd_index->indkey.values[i];

			if (attno > 0)
				result = lappend_int(result, attno);
		}
	}

	index_close(pk_rel, AccessShareLock);
	return result;
}
/*
 * overlay_serialize_pk_from_single_datum
 *
 *   从单个 Datum（MVP 单列 PK 场景）构造与 overlay_serialize_pk
 *   完全字节兼容的 JSON array ::text 序列化串。
 *
 *   CRITICAL TYPE COERCION NOTE (see p0_pk_oidx_deparse_strategy.md §4):
 *   The caller passes `con->constvalue` with its OWN declared type
 *   `consttype` (as the parser saw it), which is almost never equal to
 *   `pkatt->atttypid` in real queries:
 *
 *     CREATE TABLE t(id bigint PRIMARY KEY);
 *     SELECT * FROM t WHERE id = 2;  -- Const.consttype = INT4OID !!
 *
 *   Calling getTypeOutputInfo(INT8OID) + OidOutputFunctionCall on a
 *   4-byte int4 Datum makes int8out deref 8 bytes, where the upper 4
 *   bytes are stack garbage → key becomes "[\"140703...\"]" while the
 *   slot-side overlay_serialize_pk() correctly produces "[\"2\"]" →
 *   O(1) delta lookup misses ALL UPDATE/DELETE tombstones for the row.
 *
 *   FIX (2 stages, coercion-safe):
 *     Stage 1: stringify the literal with ITS OWN type output fn
 *       4-byte int4 Datum → "2"; varchar Datum → "alpha"; etc.
 *       Dereference size always matches consttype's layout → no garbage.
 *     Stage 2: SQL-level
 *       CAST ('<literal_str>' AS <pk_coltype>) ::text
 *       Let PG's parser itself perform the canonical coercion (int4 →
 *       int8, bpchar blank-padding, numeric rounding, custom type casts
 *       …).  The resulting ::text representation is byte-identical to
 *       what the WRITE side emits for the same stored-in-table value.
 */
char *
overlay_serialize_pk_from_single_datum(Relation rel,
									   AttrNumber pk_attno,
									   Datum pk_val,
									   bool pk_isnull,
									   Oid consttype)
{
	TupleDesc	reldesc;
	Form_pg_attribute pkatt;
	Oid			pk_typid;
	StringInfoData arr_sql;
	int			ret;
	char	   *result;
	MemoryContext oldmc;

	Assert(rel != NULL);
	reldesc = RelationGetDescr(rel);
	pkatt = TupleDescAttr(reldesc, pk_attno - 1);
	pk_typid = pkatt->atttypid;

	initStringInfo(&arr_sql);
	appendStringInfoString(&arr_sql, "SELECT to_jsonb(ARRAY[");

	if (pk_isnull)
	{
		appendStringInfoString(&arr_sql, "NULL::text");
	}
	else
	{
		Oid			lit_outfuncoid;
		bool		lit_typeIsVarlena;
		char	   *litstr;
		char	   *q;
		const char *pktypname;

		/* Stage 1: use the CONST'S OWN type to stringify its Datum. */
		getTypeOutputInfo(consttype, &lit_outfuncoid, &lit_typeIsVarlena);
		litstr = OidOutputFunctionCall(lit_outfuncoid, pk_val);

		/* Stage 2: SQL-level literal quote + CAST onto PK type.
		 *
		 * IMPORTANT: we MUST cast to ::text after the CAST onto the real
		 * PK type, so that to_jsonb(ARRAY[...]) treats every element as
		 * TEXT — matching overlay_serialize_pk() (the WRITE side), which
		 * always uses `quote_literal_cstr(OidOutputFunctionCall(...))
		 * ::text` per element.  Without this trailing `::text`, numeric
		 * types would be JSON *numbers* (`[10]`) vs WRITE-side JSON
		 * *strings* (`["10"]`) → byte mismatch → every P0 O(1) lookup
		 * misses!
		 *
		 * BUG#1 BPCHAR CAVEAT: PG's bpchar→text cast silently r-trims
		 * trailing blanks, so CAST('AB12' AS char(8))::text yields
		 * 'AB12' (4 chars).  To match, the WRITE-side datum_out-based
		 * bpchar value is also r-trimmed before jsonb-ification (see
		 * overlay_serialize_pk's "BPCHAR r-trim" comment).  Since PG
		 * treats 'AB12'::char(8) = 'AB12    '::char(8) as true anyway,
		 * r-trimming on both sides preserves PK equality semantics. */
		q = quote_literal_cstr(litstr);
		pktypname = format_type_with_typemod(pk_typid, pkatt->atttypmod);

		appendStringInfo(&arr_sql, "CAST (%s AS %s)::text",
						 q, pktypname);
		pfree(q);
		pfree(litstr);
	}
	appendStringInfoString(&arr_sql, "])::text");

	ret = ob_spi_one_shot(arr_sql.data, true, 1);
	pfree(arr_sql.data);

	if (ret != SPI_OK_SELECT || SPI_processed != 1)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_serialize_pk_from_single_datum: SPI to_jsonb failed (ret=%d rows=%lu)",
						ret, (unsigned long) SPI_processed)));
	}
	{
		const char *spival = SPI_getvalue(SPI_tuptable->vals[0],
										  SPI_tuptable->tupdesc, 1);

		oldmc = MemoryContextSwitchTo(TopMemoryContext);
		result = pstrdup(spival != NULL ? spival : "");
		MemoryContextSwitchTo(oldmc);
	}
	SPI_finish();
	return result;
}

/* ====================================================================
 * ===== Typed PK helpers — REVIEW-260926/R14 MVP
 *
 * Perform semantic PostgreSQL-native equality/ordering on the real
 * primary-key column types instead of JSON-text strcmp on dt->key.
 * strcmp is WRONG for BPCHAR (r-trim semantics), NUMERIC precision
 * (12.3 vs 12.30 are numerically equal), and TIMESTAMPTZ UTC absolute
 * time vs textual-offset representations.  The typed-pk path always
 * wins when available; callers fall back to legacy strcmp ONLY when
 * the cache has not been populated (old/direct-call paths).
 *
 * All helpers follow ISO C90: variables declared at block top, no
 * mixed declarations, no goto across initialisations.
 * ====================================================================
 */

TypedKey *
overlay_build_typed_pk_from_slot(Relation rel,
								 TupleTableSlot *slot,
								 int *out_n_pk)
{
	List	   *indexoids;
	ListCell   *lc;
	Oid			pk_index_oid;
	Relation	pk_rel;
	int			n_pk;
	int			i;
	TupleDesc	reldesc;
	TypedKey  *result;
	MemoryContext oldmc;

	Assert(rel != NULL && slot != NULL && out_n_pk != NULL);

	pk_index_oid = InvalidOid;
	pk_rel = NULL;
	indexoids = RelationGetIndexList(rel);
	foreach(lc, indexoids)
	{
		Oid			idxoid = lfirst_oid(lc);
		Relation	idxrel;

		idxrel = index_open(idxoid, AccessShareLock);
		if (idxrel->rd_index != NULL && idxrel->rd_index->indisprimary)
		{
			pk_index_oid = idxoid;
			pk_rel = idxrel;
			break;
		}
		index_close(idxrel, AccessShareLock);
	}
	list_free(indexoids);
	if (!OidIsValid(pk_index_oid) || pk_rel == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("overlay_build_typed_pk_from_slot requires a primary key on %s",
						RelationGetRelationName(rel))));
	n_pk = pk_rel->rd_index->indnatts;
	if (n_pk <= 0)
	{
		index_close(pk_rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("overlay_build_typed_pk_from_slot found 0-column PK on %s",
						RelationGetRelationName(rel))));
	}

	reldesc = RelationGetDescr(rel);

	/* ---------------------------------------------------------------
	 * R14 hardening: all allocations (TypedKey array + per-column
	 * datum deep copies) MUST live in TopMemoryContext, otherwise
	 * they could end up inside a SPI_proc context (when this helper
	 * is called from within a SPI-connected code path) and get
	 * freed on SPI_finish — producing the classic
	 * "pfree called with invalid pointer 0x... (header 0x7f..7f)"
	 * crash when the caller later releases the returned array.
	 * --------------------------------------------------------------- */
	oldmc = MemoryContextSwitchTo(TopMemoryContext);
	result = (TypedKey *) palloc0(sizeof(TypedKey) * n_pk);
	MemoryContextSwitchTo(oldmc);

	for (i = 0; i < n_pk; i++)
	{
		AttrNumber	attno = pk_rel->rd_index->indkey.values[i];
		Form_pg_attribute att;
		TypedKey  *tk;
		bool		attisnull;
		Datum		d;

		if (attno == 0)
		{
			index_close(pk_rel, AccessShareLock);
			overlay_typed_pk_free(result, i);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("overlay typed pk does not support expression index "
							"attno=0 on column %d of relation \"%s\"",
							i, RelationGetRelationName(rel))));
		}
		att = TupleDescAttr(reldesc, attno - 1);
		tk = &result[i];
		tk->typid = att->atttypid;
		tk->typmod = att->atttypmod;
		if (att->attcollation != InvalidOid)
			tk->collid = att->attcollation;
		else
			tk->collid = InvalidOid;
		tk->typlen = get_typlen(tk->typid);
		tk->typbyval = get_typbyval(tk->typid);

		/* Project memory rule (see project_memory.md §2.3): strictly
		 * forbid calling slot_getattr on a TTSOpsVirtual slot whose
		 * tts_nvalid trails the requested attno.  Either materialise
		 * or ensure the virtual slot has been fully validated first.
		 * Here we use the portable slot_getsomeattrs() pre-condition
		 * (works for all TTSOps variants, not only virtual). */
		slot_getsomeattrs(slot, attno);
		d = slot_getattr(slot, attno, &attisnull);
		tk->isnull = attisnull;
		if (!attisnull)
		{
			/* datumCopy() into TopMemoryContext so the payload outlives
			 * any enclosing SPI / expression-eval context that may be
			 * active during this call. */
			oldmc = MemoryContextSwitchTo(TopMemoryContext);
			tk->value = datumCopy(d, tk->typbyval, tk->typlen);
			MemoryContextSwitchTo(oldmc);
		}
		else
		{
			tk->value = (Datum) 0;
		}
	}

	index_close(pk_rel, AccessShareLock);
	*out_n_pk = n_pk;
	return result;
}

TypedKey *
overlay_build_typed_pk_from_key_text(Relation rel,
									 const char *key_json,
									 int *out_n_pk)
{
	List	   *indexoids;
	ListCell   *lc;
	Oid			pk_index_oid;
	Relation	pk_rel;
	int			n_pk;
	int			i;
	TupleDesc	reldesc;
	TypedKey  *result;
	char	   *q_key;
	MemoryContext oldmc;

	Assert(rel != NULL && key_json != NULL && out_n_pk != NULL);

	pk_index_oid = InvalidOid;
	pk_rel = NULL;
	indexoids = RelationGetIndexList(rel);
	foreach(lc, indexoids)
	{
		Oid			idxoid = lfirst_oid(lc);
		Relation	idxrel;

		idxrel = index_open(idxoid, AccessShareLock);
		if (idxrel->rd_index != NULL && idxrel->rd_index->indisprimary)
		{
			pk_index_oid = idxoid;
			pk_rel = idxrel;
			break;
		}
		index_close(idxrel, AccessShareLock);
	}
	list_free(indexoids);
	if (!OidIsValid(pk_index_oid) || pk_rel == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("overlay_build_typed_pk_from_key_text requires PK on %s",
						RelationGetRelationName(rel))));
	n_pk = pk_rel->rd_index->indnatts;
	if (n_pk <= 0)
	{
		index_close(pk_rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("overlay_build_typed_pk_from_key_text 0-col PK on %s",
						RelationGetRelationName(rel))));
	}
	reldesc = RelationGetDescr(rel);

	/* Build the TypedKey skeleton in TopMemoryContext so the returned
	 * payload outlives any SPI context (see build_from_slot rationale). */
	oldmc = MemoryContextSwitchTo(TopMemoryContext);
	result = (TypedKey *) palloc0(sizeof(TypedKey) * n_pk);
	MemoryContextSwitchTo(oldmc);

	q_key = quote_literal_cstr(key_json);

	for (i = 0; i < n_pk; i++)
	{
		AttrNumber	attno;
		Form_pg_attribute att;
		TypedKey  *tk;
		const char *pktypname;
		Datum		binval;
		bool		isnull;
		StringInfoData valsql;
		int			ret;

		attno = pk_rel->rd_index->indkey.values[i];
		if (attno == 0)
		{
			index_close(pk_rel, AccessShareLock);
			pfree(q_key);
			overlay_typed_pk_free(result, i);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("typed key expression-index PK not supported at col %d of %s",
							i, RelationGetRelationName(rel))));
		}
		att = TupleDescAttr(reldesc, attno - 1);
		tk = &result[i];
		tk->typid = att->atttypid;
		tk->typmod = att->atttypmod;
		if (att->attcollation != InvalidOid)
			tk->collid = att->attcollation;
		else
			tk->collid = InvalidOid;
		tk->typlen = get_typlen(tk->typid);
		tk->typbyval = get_typbyval(tk->typid);
		pktypname = format_type_with_typemod(tk->typid, tk->typmod);

		initStringInfo(&valsql);
		appendStringInfo(&valsql,
						 "SELECT (((%s::jsonb)->%d) #>> '{}'::text[])::%s",
						 q_key, i, pktypname);
		ret = ob_spi_one_shot(valsql.data, true, 1);
		pfree(valsql.data);
		if (ret != SPI_OK_SELECT || SPI_processed != 1 ||
			SPI_tuptable == NULL || SPI_tuptable->vals == NULL)
		{
			/* NB: overlay_typed_pk_free already pfree's the TypedKey array
			 * itself after releasing the per-column datum payloads, so we
			 * MUST NOT call pfree(result) separately (would double-free
			 * the palloc chunk and corrupt the memory context). */
			overlay_typed_pk_free(result, i);
			SPI_finish();
			pfree(q_key);
			index_close(pk_rel, AccessShareLock);
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("typed pk from key_text: SPI failed at col %d", i)));
		}
		binval = SPI_getbinval(SPI_tuptable->vals[0],
							   SPI_tuptable->tupdesc, 1, &isnull);
		tk->isnull = isnull;
		if (!isnull)
		{
			/* datumCopy into TopMemoryContext: the SPI_tuptable pool is
			 * released at SPI_finish below so any non-typbyval payload
			 * must be copied to a stable memory context NOW. */
			oldmc = MemoryContextSwitchTo(TopMemoryContext);
			tk->value = datumCopy(binval, tk->typbyval, tk->typlen);
			MemoryContextSwitchTo(oldmc);
		}
		else
		{
			tk->value = (Datum) 0;
		}
		SPI_finish();
	}
	pfree(q_key);
	index_close(pk_rel, AccessShareLock);
	*out_n_pk = n_pk;
	return result;
}

int
overlay_typed_pk_cmp(const TypedKey *a, int a_n,
					 const TypedKey *b, int b_n)
{
	int			i;
	int			sign;

	if (a_n != b_n)
		return a_n < b_n ? -1 : 1;
	if (a_n == 0)
		return 0;

	sign = 0;
	for (i = 0; i < a_n; i++)
	{
		const TypedKey *ta;
		const TypedKey *tb;

		ta = &a[i];
		tb = &b[i];

		if (ta->isnull || tb->isnull)
		{
			if (!(ta->isnull && tb->isnull))
				return ta->isnull ? 1 : -1;
			continue;
		}
		if (ta->typid != tb->typid)
			return ta->typid < tb->typid ? -1 : 1;

		/* ------------------------------------------------------------
		 * Native-PG per-column compare using the type's registered
		 * btree sort operators.  THIS IS THE CRITICAL FIX that
		 * replaces the previous SPI-CASE-SELECT comparator which:
		 *   (a) allocated transient stuff inside SPI_proc memory
		 *       context that got torn down on SPI_finish, causing
		 *       "pfree called with invalid pointer" when the
		 *       comparator's side-effects leaked out to the caller;
		 *   (b) could not be safely invoked from inside libc qsort/
		 *       bsearch callbacks because longjmp(SPI error) would
		 *       leave libc comparator state undefined.
		 *
		 * Strategy (standard PG btree compare recipe):
		 *   1. get_sort_group_operators(ta->typid, true, true, false,
		 *      &ltOpr, &eqOpr, &dummy, NULL) to recover the canonical
		 *      < and = operators for the PK type (works for all PK
		 *      eligible types because any PK requires a btree opclass).
		 *   2. Look up each operator OID via SysCache OPEROID to
		 *      recover its underlying function regproc (oprcode).
		 *   3. Call the equality function first via
		 *      OidFunctionCall2Coll(oprcode_eq, ta->collid, a, b)
		 *      returning 0 if true; otherwise call the less-than
		 *      function returning -1 if true, else 1.
		 *   4. Collation is threaded through so text/bpchar/varchar
		 *      follow the column's real collation rather than the
		 *      default C locale that memcmp() would impose.
		 * ------------------------------------------------------------ */
		{
			Oid			ltOpr = InvalidOid;
			Oid			eqOpr = InvalidOid;
			Oid			gtOpr = InvalidOid;
			HeapTuple	tup_eq;
			HeapTuple	tup_lt;
			Datum		d_eqfn;
			Datum		d_ltfn;
			bool		isnull_fn;
			Oid			fn_eq;
			Oid			fn_lt;
			Datum		eq_result;
			bool		eq_null;
			Datum		lt_result;
			bool		lt_null;

			get_sort_group_operators(ta->typid,
									 true, true, false,
									 &ltOpr, &eqOpr, &gtOpr,
									 NULL);
			if (!OidIsValid(eqOpr) || !OidIsValid(ltOpr))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("overlay_typed_pk_cmp: no btree sort operators "
								"for PK column type %u at col %d",
								ta->typid, i)));

			tup_eq = SearchSysCache1(OPEROID, ObjectIdGetDatum(eqOpr));
			if (!HeapTupleIsValid(tup_eq))
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("overlay_typed_pk_cmp: eqOpr %u not in OPEROID cache at col %d",
								eqOpr, i)));
			d_eqfn = SysCacheGetAttr(OPEROID, tup_eq, 13, &isnull_fn);
			if (isnull_fn)
			{
				ReleaseSysCache(tup_eq);
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("overlay_typed_pk_cmp: eqOpr %u has NULL oprcode at col %d",
								eqOpr, i)));
			}
			fn_eq = DatumGetObjectId(d_eqfn);
			ReleaseSysCache(tup_eq);

			tup_lt = SearchSysCache1(OPEROID, ObjectIdGetDatum(ltOpr));
			if (!HeapTupleIsValid(tup_lt))
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("overlay_typed_pk_cmp: ltOpr %u not in OPEROID cache at col %d",
								ltOpr, i)));
			d_ltfn = SysCacheGetAttr(OPEROID, tup_lt, 13, &isnull_fn);
			if (isnull_fn)
			{
				ReleaseSysCache(tup_lt);
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("overlay_typed_pk_cmp: ltOpr %u has NULL oprcode at col %d",
								ltOpr, i)));
			}
			fn_lt = DatumGetObjectId(d_ltfn);
			ReleaseSysCache(tup_lt);

			/* Equality short-circuit first. */
			eq_result = OidFunctionCall2Coll(fn_eq, ta->collid,
											  ta->value, tb->value);
			eq_null = false;
			if (DatumGetBool(eq_result) && !eq_null)
			{
				sign = 0;
				continue;
			}

			/* Not equal: strict less-than → -1, otherwise → +1. */
			lt_result = OidFunctionCall2Coll(fn_lt, ta->collid,
											  ta->value, tb->value);
			lt_null = false;
			if (DatumGetBool(lt_result) && !lt_null)
				sign = -1;
			else
				sign = 1;
			return sign;
		}
	}
	return 0;
}

void
overlay_typed_pk_free(TypedKey *pk, int n_pk)
{
	int			i;

	if (pk == NULL || n_pk <= 0)
		return;
	for (i = 0; i < n_pk; i++)
	{
		if (!pk[i].isnull && !pk[i].typbyval &&
			pk[i].value != (Datum) 0)
			pfree(DatumGetPointer(pk[i].value));
	}
	pfree(pk);
}

