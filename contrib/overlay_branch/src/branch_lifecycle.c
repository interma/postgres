/*-------------------------------------------------------------------------
 *
 * branch_lifecycle.c
 *	  Overlay Branch lifecycle: create / use / current / apply / discard
 *	  internals plus 3-pass optimistic apply (DELETE → UPDATE → INSERT)
 *	  with per-row version-based conflict detection.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "overlay_branch.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "storage/lmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#define OBTABLE_DELTA   OBSCHEMA ".pg_branch_delta"
#define OBTABLE_BRANCH  OBSCHEMA ".pg_branch"

/* ---- External globals owned by overlay_branch.c ---- */
extern bool ob_in_apply_operation;
extern bool ob_in_guc_setconfig;

/* ============ Internal apply helpers (static to this TU) ============ */

static char *ob_build_pk_where_clause(Relation rel, const char *pk_json_array);
static TupleTableSlot *ob_fetch_main_current_slot(Relation rel, const char *where_clause);
static void apply_relation_delete_pass(Relation rel, DeltaTuple *dt);
static void apply_relation_update_pass(Relation rel, DeltaTuple *dt);
static void apply_relation_insert_pass(Relation rel, DeltaTuple *dt);

/* ================================================================
 * ---------- Branch Context (internal implementations) ----------
 * ================================================================ */

int32
overlay_branch_create_internal(const char *branch_name)
{
	int			ret;
	Oid			owner;
	int32		new_branch_id;
	char	   *esc_name;
	StringInfoData sql;
	static int32 s_next_return_id = 0;

	if (branch_name == NULL || *branch_name == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("branch name cannot be empty")));

	if (strlen(branch_name) >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_NAME_TOO_LONG),
				 errmsg("branch name too long (max %d characters)",
						NAMEDATALEN - 1)));

	owner = GetUserId();
	esc_name = quote_literal_cstr(branch_name);
	initStringInfo(&sql);

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "overlay_branch: SPI_connect() failed in create_branch");

	/* 1) Duplicate check */
	resetStringInfo(&sql);
	appendStringInfo(&sql,
					 "SELECT 1 FROM " OBTABLE_BRANCH " WHERE branch_name = %s LIMIT 1",
					 esc_name);
	ret = SPI_execute(sql.data, true, 1);
	if (ret != SPI_OK_SELECT)
	{
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_branch: could not check existing branch '%s': SPI ret=%d",
						branch_name, ret)));
	}
	if (SPI_processed > 0)
	{
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("branch \"%s\" already exists", branch_name)));
	}

	/* 2) INSERT new row */
	resetStringInfo(&sql);
	appendStringInfo(&sql,
					 "INSERT INTO " OBTABLE_BRANCH " "
					 "(branch_name, owner, created_at, mode, state) "
					 "VALUES (%s, %u, now(), '%s', '%s')",
					 esc_name,
					 owner,
					 BRANCH_MODE_LIVE,
					 BRANCH_STATE_ACTIVE);

	ret = SPI_execute(sql.data, false, 0);
	if (ret != SPI_OK_INSERT || SPI_processed != 1)
	{
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_branch: could not insert new branch '%s': SPI ret=%d processed=%lu",
						branch_name, ret, (unsigned long) SPI_processed)));
	}

	new_branch_id = ++s_next_return_id;

	SPI_finish();

	elog(DEBUG1, "overlay_branch_create_internal: name='%s' return_id=%d owner=%u",
		 branch_name, (int) new_branch_id, (unsigned) owner);

	return new_branch_id;
}

void
overlay_branch_use_internal(const char *branch_name)
{
	if (branch_name == NULL || *branch_name == '\0')
	{
		/* Leave the branch */
		if (CurrentBranchContext && CurrentBranchContext->is_active)
		{
			CurrentBranchContext->is_active = false;
			CurrentBranchContext->branch_id = 0;
			CurrentBranchContext->branch_name[0] = '\0';
			elog(DEBUG1, "overlay_branch: left current branch (via use_internal)");
		}
		{
			bool		saved_flag = ob_in_guc_setconfig;

			ob_in_guc_setconfig = true;
			SetConfigOption("overlay_branch.current", "",
							PGC_USERSET, PGC_S_SESSION);
			ob_in_guc_setconfig = saved_flag;
		}
		return;
	}

	{
		int			ret;
		char	   *esc_name;
		StringInfoData sql;

		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "overlay_branch: SPI_connect() failed in use_branch");

		esc_name = quote_literal_cstr(branch_name);
		initStringInfo(&sql);

		/* 1) Existence check */
		resetStringInfo(&sql);
		appendStringInfo(&sql,
						 "SELECT 1 FROM " OBTABLE_BRANCH " WHERE branch_name = %s LIMIT 1",
						 esc_name);
		ret = SPI_execute(sql.data, true, 1);
		if (ret != SPI_OK_SELECT)
		{
			SPI_finish();
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("overlay_branch: could not check existence of branch '%s': SPI ret=%d",
							branch_name, ret)));
		}
		if (SPI_processed == 0)
		{
			SPI_finish();
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("branch \"%s\" does not exist", branch_name)));
		}

		/* 2) State check (must be active) */
		resetStringInfo(&sql);
		appendStringInfo(&sql,
						 "SELECT 1 FROM " OBTABLE_BRANCH " "
						 "WHERE branch_name = %s AND state = '%s' LIMIT 1",
						 esc_name, BRANCH_STATE_ACTIVE);
		ret = SPI_execute(sql.data, true, 1);
		if (ret != SPI_OK_SELECT)
		{
			SPI_finish();
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("overlay_branch: could not check state of branch '%s': SPI ret=%d",
							branch_name, ret)));
		}
		if (SPI_processed == 0)
		{
			SPI_finish();
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("branch \"%s\" is not active",
							branch_name),
					 errhint("Create a fresh branch, or use DISCARD BRANCH to clean up.")));
		}

		/* 3) Fetch real branch_id */
		resetStringInfo(&sql);
		appendStringInfo(&sql,
						 "SELECT branch_id FROM " OBTABLE_BRANCH " "
						 "WHERE branch_name = %s LIMIT 1",
						 esc_name);
		ret = SPI_execute(sql.data, true, 1);
		if (ret != SPI_OK_SELECT || SPI_processed != 1)
		{
			SPI_finish();
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("overlay_branch: could not fetch branch_id for '%s'",
							branch_name)));
		}
		{
			bool	isnull;
			int32	real_branch_id = DatumGetInt32(
				SPI_getbinval(SPI_tuptable->vals[0],
							  SPI_tuptable->tupdesc,
							  1, &isnull));
			if (isnull)
			{
				SPI_finish();
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("overlay_branch: branch_id for '%s' is NULL",
								branch_name)));
			}

			if (CurrentBranchContext == NULL)
			{
				CurrentBranchContext =
					MemoryContextAllocZero(TopMemoryContext,
										   sizeof(BranchContext));
			}
			CurrentBranchContext->branch_id = real_branch_id;
			strlcpy(CurrentBranchContext->branch_name,
					branch_name, NAMEDATALEN);
			CurrentBranchContext->owner = GetUserId();
			CurrentBranchContext->is_active = true;
			strncpy(CurrentBranchContext->mode, BRANCH_MODE_LIVE,
					sizeof(CurrentBranchContext->mode) - 1);
			CurrentBranchContext->mode[sizeof(CurrentBranchContext->mode) - 1] = '\0';
			CurrentBranchContext->created_at = 0;
		}

		SPI_finish();

		{
			bool		saved_flag = ob_in_guc_setconfig;

			ob_in_guc_setconfig = true;
			SetConfigOption("overlay_branch.current", branch_name,
							PGC_USERSET, PGC_S_SESSION);
			ob_in_guc_setconfig = saved_flag;
		}

		elog(DEBUG1, "overlay_branch_use_internal: entered name='%s'",
			 branch_name);
	}
}

const char *
overlay_branch_get_current_name(void)
{
	if (CurrentBranchContext == NULL || !CurrentBranchContext->is_active)
		return NULL;
	return CurrentBranchContext->branch_name;
}

int32
overlay_branch_get_current_id(void)
{
	if (CurrentBranchContext == NULL || !CurrentBranchContext->is_active)
		return 0;
	return CurrentBranchContext->branch_id;
}

bool
overlay_branch_is_active(void)
{
	/* Dual-source truth check — see comments in the original monolithic file */
	extern bool overlay_branch_enabled;

	return (overlay_branch_enabled &&
			CurrentBranchContext != NULL &&
			CurrentBranchContext->is_active &&
			overlay_branch_current_name != NULL &&
			*overlay_branch_current_name != '\0');
}

/* ---------- apply_internal: 3-pass optimistic replay ---------- */
void
overlay_branch_apply_internal(const char *branch_name)
{
	int			ret;
	char	   *esc_name;
	int32		bid = 0;
	char	   *state = NULL;
	bool		leaving_current = false;
	List	   *relids = NIL;
	ListCell   *rlc;
	Relation	rel = NULL;

	if (branch_name == NULL || *branch_name == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("branch name cannot be empty")));

	elog(NOTICE, "overlay_branch: APPLY BRANCH '%s' (passes D→U→I with optimistic conflict detection)",
		 branch_name);

	/* Step 1: verify branch is ACTIVE */
	{
		StringInfoData sql1;

		initStringInfo(&sql1);
		esc_name = quote_literal_cstr(branch_name);
		appendStringInfo(&sql1,
						 "SELECT branch_id, state FROM " OBTABLE_BRANCH " "
						 "WHERE branch_name = %s LIMIT 1",
						 esc_name);
		ret = ob_spi_one_shot(sql1.data, true, 1);
		pfree(sql1.data);
		if (ret != SPI_OK_SELECT)
		{
			pfree(esc_name);
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("overlay_branch: apply existence check failed for '%s': SPI ret=%d",
							branch_name, ret)));
		}
		if (SPI_processed == 0)
		{
			SPI_finish();
			pfree(esc_name);
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("branch \"%s\" does not exist", branch_name)));
		}
		{
			bool		isnull;
			Datum		v;

			v = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
			if (isnull)
			{
				SPI_finish();
				pfree(esc_name);
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("overlay_branch: apply found NULL branch_id for '%s'",
								branch_name)));
			}
			bid = DatumGetInt32(v);

			v = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull);
			if (isnull)
			{
				state = NULL;
			}
			else
			{
				MemoryContext oldmc2;

				oldmc2 = MemoryContextSwitchTo(TopMemoryContext);
				state = TextDatumGetCString(v);
				MemoryContextSwitchTo(oldmc2);
			}
		}
		SPI_finish();
	}

	if (state == NULL || strcmp(state, BRANCH_STATE_ACTIVE) != 0)
	{
		char	   *reason;

		reason = psprintf("branch \"%s\" is not active (state=%s); "
						  "apply can only be called once on an active branch",
						  branch_name, state ? state : "NULL");
		if (state)
			pfree(state);
		pfree(esc_name);
		overlay_guard_ereport_fail("apply_branch", branch_name, reason);
	}

	/* Step 2: kick user out of this session branch (if on it) */
	if (CurrentBranchContext &&
		CurrentBranchContext->is_active &&
		strncmp(CurrentBranchContext->branch_name, branch_name,
				NAMEDATALEN) == 0)
	{
		leaving_current = true;
		CurrentBranchContext->is_active = false;
		CurrentBranchContext->branch_id = 0;
		CurrentBranchContext->branch_name[0] = '\0';
	}

	/* Step 3: gather relid set */
	{
		StringInfoData sql2;

		initStringInfo(&sql2);
		appendStringInfo(&sql2,
						 "SELECT DISTINCT relid::oid FROM " OBTABLE_DELTA " "
						 "WHERE branch_id = %d ORDER BY 1",
						 (int) bid);
		ret = ob_spi_one_shot(sql2.data, true, 0);
		pfree(sql2.data);
		if (ret != SPI_OK_SELECT)
		{
			if (state) pfree(state);
			pfree(esc_name);
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("overlay_branch: apply relid-set lookup failed bid=%d SPI ret=%d",
							bid, ret)));
		}
		if (SPI_processed > 0 && SPI_tuptable && SPI_tuptable->vals)
		{
			MemoryContext oldmc = NULL;
			uint64		i;

			oldmc = MemoryContextSwitchTo(TopMemoryContext);
			for (i = 0; i < SPI_processed; i++)
			{
				bool		isnull;
				Datum		d;

				d = SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull);
				if (!isnull)
					relids = lappend_oid(relids, DatumGetObjectId(d));
			}
			MemoryContextSwitchTo(oldmc);
		}
		SPI_finish();
	}

	elog(DEBUG1, "apply[dbg] relids count=%d", list_length(relids));

	/* For each relation: three-pass (D→U→I) replay with apply-bypass guard */
	{
		bool		saved_in_apply = ob_in_apply_operation;

		ob_in_apply_operation = true;
		PG_TRY();
		{
			foreach(rlc, relids)
			{
				Oid			relid = lfirst_oid(rlc);
				List	   *deltas;
				ListCell   *dlc;

				elog(DEBUG1, "apply[dbg] rel=%u table_open", (unsigned) relid);
				if (rel != NULL)
					table_close(rel, NoLock);
				rel = table_open(relid, RowExclusiveLock);

				elog(DEBUG1, "apply[dbg] overlay_delta_list_for_rel");
				deltas = overlay_delta_list_for_rel(bid, relid);
				elog(DEBUG1, "apply[dbg] delta count=%d", list_length(deltas));

				foreach(dlc, deltas)
				{
					DeltaTuple *dt = (DeltaTuple *) lfirst(dlc);
					if (dt->op == DELTA_OP_DELETE)
					{
						elog(DEBUG1, "apply[dbg] DELETE pass key=%s", dt->key ? dt->key : "NULL");
						apply_relation_delete_pass(rel, dt);
						elog(DEBUG1, "apply[dbg] DELETE pass DONE");
					}
				}
				foreach(dlc, deltas)
				{
					DeltaTuple *dt = (DeltaTuple *) lfirst(dlc);
					if (dt->op == DELTA_OP_UPDATE)
					{
						elog(DEBUG1, "apply[dbg] UPDATE pass key=%s", dt->key ? dt->key : "NULL");
						apply_relation_update_pass(rel, dt);
						elog(DEBUG1, "apply[dbg] UPDATE pass DONE");
					}
				}
				foreach(dlc, deltas)
				{
					DeltaTuple *dt = (DeltaTuple *) lfirst(dlc);
					if (dt->op == DELTA_OP_INSERT)
					{
						elog(DEBUG1, "apply[dbg] INSERT pass key=%s", dt->key ? dt->key : "NULL");
						apply_relation_insert_pass(rel, dt);
						elog(DEBUG1, "apply[dbg] INSERT pass DONE");
					}
				}

				elog(DEBUG1, "apply[dbg] freeing deltas");
				foreach(dlc, deltas)
				{
					DeltaTuple *dt = (DeltaTuple *) lfirst(dlc);
					if (dt->key) pfree(dt->key);
					if (dt->old_version) pfree(dt->old_version);
					if (dt->tuple_data) pfree(dt->tuple_data);
					pfree(dt);
				}
				list_free(deltas);
				elog(DEBUG1, "apply[dbg] rel loop end");
			}
			if (rel != NULL)
				table_close(rel, NoLock);
		}
		PG_CATCH();
		{
			ob_in_apply_operation = saved_in_apply;
			PG_RE_THROW();
		}
		PG_END_TRY();
		ob_in_apply_operation = saved_in_apply;
	}

	elog(DEBUG1, "apply[dbg] Step4 finalize");
	/* Step 4: finalize. state→applied + delete delta rows. */
	{
		StringInfoData sql_final;

		initStringInfo(&sql_final);
		appendStringInfo(&sql_final,
						 "UPDATE " OBTABLE_BRANCH " SET state = '%s' "
						 "WHERE branch_name = %s AND state = '%s'",
						 BRANCH_STATE_APPLIED,
						 esc_name,
						 BRANCH_STATE_ACTIVE);
		ret = ob_spi_one_shot(sql_final.data, false, 1);
		if (ret != SPI_OK_UPDATE)
		{
			pfree(sql_final.data);
			if (state) pfree(state);
			pfree(esc_name);
			list_free(relids);
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("overlay_branch: apply final UPDATE pg_branch failed for '%s' SPI ret=%d",
							branch_name, ret)));
		}
		SPI_finish();

		resetStringInfo(&sql_final);
		appendStringInfo(&sql_final,
						 "DELETE FROM " OBTABLE_DELTA " WHERE branch_id = %d",
						 (int) bid);
		ret = ob_spi_one_shot(sql_final.data, false, 0);
		if (ret != SPI_OK_DELETE)
		{
			pfree(sql_final.data);
			if (state) pfree(state);
			pfree(esc_name);
			list_free(relids);
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("overlay_branch: apply final DELETE delta rows failed for '%s' SPI ret=%d",
							branch_name, ret)));
		}
		SPI_finish();
		pfree(sql_final.data);
	}

	if (leaving_current)
	{
		SetConfigOption("overlay_branch.current", "",
						PGC_USERSET, PGC_S_SESSION);
	}

	pfree(esc_name);
	if (state) pfree(state);
	list_free(relids);

	elog(NOTICE, "overlay_branch: APPLY BRANCH '%s' completed (state=applied, delta rows deleted)",
		 branch_name);
}

/* ---------- discard_internal ---------- */
void
overlay_branch_discard_internal(const char *branch_name)
{
	int			ret;
	char	   *esc_name;
	StringInfoData sql;
	bool		leaving_current = false;
	int32		bid = 0;
	bool		already_discarded_applied = false;

	if (branch_name == NULL || *branch_name == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("branch name cannot be empty")));

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "overlay_branch: SPI_connect() failed in discard_branch");

	esc_name = quote_literal_cstr(branch_name);
	initStringInfo(&sql);

	/* 1) Existence + current-state check */
	resetStringInfo(&sql);
	appendStringInfo(&sql,
					 "SELECT branch_id, state FROM " OBTABLE_BRANCH " "
					 "WHERE branch_name = %s LIMIT 1",
					 esc_name);
	ret = SPI_execute(sql.data, true, 1);
	if (ret != SPI_OK_SELECT)
	{
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_branch: discard existence check failed for '%s': SPI ret=%d",
						branch_name, ret)));
	}
	if (SPI_processed == 0)
	{
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("branch \"%s\" does not exist", branch_name)));
	}
	{
		bool		isnull;
		Datum		v;
		char	   *st;

		v = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
		if (isnull)
		{
			SPI_finish();
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("overlay_branch: discard found NULL branch_id for '%s'",
							branch_name)));
		}
		bid = DatumGetInt32(v);

		v = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull);
		st = isnull ? NULL : TextDatumGetCString(v);
		if (st != NULL &&
			(strcmp(st, BRANCH_STATE_DISCARDED) == 0 ||
			 strcmp(st, BRANCH_STATE_APPLIED) == 0))
			already_discarded_applied = true;
	}

	if (already_discarded_applied)
	{
		SPI_finish();
		overlay_guard_ereport_fail("discard_branch",
								   branch_name,
								   psprintf("branch \"%s\" is not active (already discarded or applied); "
											"discard cannot be called twice on the same branch",
											branch_name));
	}

	/* 2) UPDATE state -> discarded */
	resetStringInfo(&sql);
	appendStringInfo(&sql,
					 "UPDATE " OBTABLE_BRANCH " SET state = '%s' "
					 "WHERE branch_name = %s AND state = '%s'",
					 BRANCH_STATE_DISCARDED,
					 esc_name,
					 BRANCH_STATE_ACTIVE);
	ret = SPI_execute(sql.data, false, 1);
	if (ret != SPI_OK_UPDATE)
	{
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_branch: discard update pg_branch failed for '%s' SPI ret=%d",
						branch_name, ret)));
	}

	/* 3) CASCADE: delete all delta rows for this branch_id */
	resetStringInfo(&sql);
	appendStringInfo(&sql,
					 "DELETE FROM " OBTABLE_DELTA " WHERE branch_id = %d",
					 (int) bid);
	ret = SPI_execute(sql.data, false, 0);
	if (ret != SPI_OK_DELETE)
	{
		SPI_finish();
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("overlay_branch: discard delete delta rows failed for '%s' SPI ret=%d",
						branch_name, ret)));
	}

	SPI_finish();

	/* 4) If user is on this branch, revert to Main */
	if (CurrentBranchContext &&
		CurrentBranchContext->is_active &&
		strncmp(CurrentBranchContext->branch_name, branch_name,
				NAMEDATALEN) == 0)
	{
		leaving_current = true;
		CurrentBranchContext->is_active = false;
		CurrentBranchContext->branch_id = 0;
		CurrentBranchContext->branch_name[0] = '\0';
	}

	if (leaving_current)
		ereport(NOTICE,
				(errmsg("discarding current branch \"%s\", reverting to Main",
						branch_name)));

	elog(DEBUG1, "overlay_branch_discard_internal: name='%s'", branch_name);
}


/* ================================================================
 * ============ Apply: PK WHERE builder + MAIN fetch + 3 passes ===
 * ================================================================ */

/*
 * ob_build_pk_where_clause — produce "pkcol1 = v1::type1 AND ..."
 * from a serialized JSON-array PK (DeltaTuple->key).
 */
static char *
ob_build_pk_where_clause(Relation rel, const char *pk_json_array)
{
	List	   *indexoids;
	ListCell   *lc;
	Oid			pk_index_oid = InvalidOid;
	Relation	pk_rel = NULL;
	TupleDesc	reldesc = RelationGetDescr(rel);
	int			n_pk;
	int			i;
	StringInfoData colbuf;
	StringInfoData sql;
	int			ret;
	char	   *result = NULL;

	Assert(rel != NULL && pk_json_array != NULL);

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
	if (!OidIsValid(pk_index_oid) || pk_rel == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("apply requires a primary key on %s",
						RelationGetRelationName(rel))));

	n_pk = pk_rel->rd_index->indnatts;
	if (n_pk == 0)
	{
		index_close(pk_rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("apply requires primary key columns on %s",
						RelationGetRelationName(rel))));
	}

	{
		int			json_len = 0;
		char	   *q_pk;
		StringInfoData len_sql;
		char	  **pk_texts;

		elog(DEBUG1, "bpk[dbg] 1 json_len check, pk=%s", pk_json_array);
		q_pk = quote_literal_cstr(pk_json_array);
		initStringInfo(&len_sql);
		appendStringInfo(&len_sql,
						 "SELECT jsonb_array_length(%s::jsonb)", q_pk);
		ret = ob_spi_one_shot(len_sql.data, true, 1);
		pfree(len_sql.data);
		if (ret != SPI_OK_SELECT || SPI_processed != 1 ||
			!SPI_tuptable || !SPI_tuptable->vals)
		{
			SPI_finish();
			pfree(q_pk);
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("apply jsonb_array_length SPI call failed ret=%d", ret)));
		}
		{
			bool		isnull;
			Datum		v;

			v = SPI_getbinval(SPI_tuptable->vals[0],
							 SPI_tuptable->tupdesc, 1, &isnull);
			if (!isnull)
				json_len = DatumGetInt32(v);
		}
		SPI_finish();
		elog(DEBUG1, "bpk[dbg] 2 json_len=%d n_pk=%d", json_len, n_pk);

		if (json_len != n_pk)
		{
			pfree(q_pk);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("apply pk JSON length %d differs from PK column count %d on %s",
							json_len, n_pk, RelationGetRelationName(rel))));
		}

		pk_texts = (char **) palloc0(sizeof(char *) * n_pk);
		elog(DEBUG1, "bpk[dbg] 3 pk_val loop n_pk=%d", n_pk);
		for (i = 0; i < n_pk; i++)
		{
			StringInfoData val_sql;

			elog(DEBUG1, "bpk[dbg] 3a i=%d START", i);
			initStringInfo(&val_sql);
			appendStringInfo(&val_sql,
							 "SELECT (%s::jsonb)->%d #>> '{}'",
							 q_pk, i);
			elog(DEBUG1, "bpk[dbg] 3b i=%d sql=%s", i, val_sql.data);
			ret = ob_spi_one_shot(val_sql.data, true, 1);
			elog(DEBUG1, "bpk[dbg] 3c i=%d ret=%d", i, ret);
			pfree(val_sql.data);
			if (ret != SPI_OK_SELECT || SPI_processed != 1 ||
				!SPI_tuptable || !SPI_tuptable->vals)
			{
				int j;
				for (j = 0; j < i; j++) if (pk_texts[j]) pfree(pk_texts[j]);
				pfree(pk_texts);
				SPI_finish();
				pfree(q_pk);
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("apply pk value extract failed i=%d", i)));
			}
			{
				bool		isnull;
				char	   *txt = SPI_getvalue(SPI_tuptable->vals[0],
											   SPI_tuptable->tupdesc, 1);

				elog(DEBUG1, "bpk[dbg] 3d i=%d txt=%s", i, txt ? txt : "NULL");
				isnull = (txt == NULL);
				if (!isnull)
				{
					MemoryContext oldmc;
					oldmc = MemoryContextSwitchTo(TopMemoryContext);
					pk_texts[i] = pstrdup(txt ? txt : "");
					MemoryContextSwitchTo(oldmc);
				}
				else
					pk_texts[i] = NULL;
				elog(DEBUG1, "bpk[dbg] 3e i=%d pktext=%s", i, pk_texts[i] ? pk_texts[i] : "NULL");
			}
			elog(DEBUG1, "bpk[dbg] 3f i=%d SPI_finish", i);
			SPI_finish();
			elog(DEBUG1, "bpk[dbg] 3g i=%d DONE", i);
		}
		elog(DEBUG1, "bpk[dbg] 4 pfree q_pk");
		pfree(q_pk);
		elog(DEBUG1, "bpk[dbg] 5 build colbuf VALUES");

		initStringInfo(&colbuf);
		for (i = 0; i < n_pk; i++)
		{
			AttrNumber	attno = pk_rel->rd_index->indkey.values[i];
			Form_pg_attribute att;
			char	   *qname;
			char	   *qtype;
			char	   *qval;

			elog(DEBUG1, "bpk[dbg] 5a i=%d", i);
			if (attno == 0)
				continue;
			elog(DEBUG1, "bpk[dbg] 5a2 i=%d attno=%d", i, attno);
			att = TupleDescAttr(reldesc, attno - 1);
			elog(DEBUG1, "bpk[dbg] 5a3 i=%d attname=%s", i, NameStr(att->attname));
			qname = quote_literal_cstr(NameStr(att->attname));
			elog(DEBUG1, "bpk[dbg] 5a4 i=%d qname=%s", i, qname);
			qtype = quote_literal_cstr(format_type_with_typemod(att->atttypid, att->atttypmod));
			elog(DEBUG1, "bpk[dbg] 5a5 i=%d qtype=%s", i, qtype);
			if (pk_texts[i] != NULL)
				qval = quote_literal_cstr(pk_texts[i]);
			else
				qval = pstrdup("NULL");
			elog(DEBUG1, "bpk[dbg] 5a6 i=%d qval=%s", i, qval);
			if (i > 0)
				appendStringInfoChar(&colbuf, ',');
			appendStringInfo(&colbuf,
							 "(%s,%s,%d,%s)",
							 qname, qtype, i + 1, qval);
			elog(DEBUG1, "bpk[dbg] 5a7 i=%d appended len=%d", i, (int) colbuf.len);
			(void) qname;
			(void) qtype;
			(void) qval;
			elog(DEBUG1, "bpk[dbg] 5a8 i=%d before pk_texts[i] pfree", i);
			if (pk_texts[i]) {
				pfree(pk_texts[i]);
			}
			elog(DEBUG1, "bpk[dbg] 5a9 i=%d loop end", i);
		}
		elog(DEBUG1, "bpk[dbg] 6 pfree pk_texts");
		pfree(pk_texts);
		elog(DEBUG1, "bpk[dbg] 7 index_close");
		index_close(pk_rel, AccessShareLock);
	}
	elog(DEBUG1, "bpk[dbg] 8 string_agg SPI");

	initStringInfo(&sql);
	appendStringInfo(&sql,
					 "SELECT string_agg("
					 "quote_ident(v.colname) || ' = ' || "
					 "CASE WHEN v.val IS NULL THEN 'NULL' ELSE quote_nullable(v.val) || '::' || v.typname END, "
					 "' AND ' ORDER BY v.n) "
					 "FROM (VALUES %s) AS v(colname,typname,n,val)",
					 colbuf.data);

	ret = ob_spi_one_shot(sql.data, true, 1);
	pfree(sql.data);
	elog(DEBUG1, "bpk[dbg] 9 copy result");
	if (ret == SPI_OK_SELECT && SPI_processed == 1 &&
		SPI_tuptable && SPI_tuptable->vals && SPI_tuptable->vals[0])
	{
		bool		isnull;
		char	   *txt = SPI_getvalue(SPI_tuptable->vals[0],
									   SPI_tuptable->tupdesc, 1);

		isnull = (txt == NULL);
		if (!isnull)
		{
			MemoryContext oldmc;

			oldmc = MemoryContextSwitchTo(TopMemoryContext);
			result = pstrdup(txt ? txt : "");
			MemoryContextSwitchTo(oldmc);
		}
	}
	elog(DEBUG1, "bpk[dbg] 10 SPI_finish + pfree colbuf");
	SPI_finish();
	pfree(colbuf.data);
	elog(DEBUG1, "bpk[dbg] END result=%s", result ? result : "NULL");
	return result;
}

/*
 * ob_fetch_main_current_slot: given pk WHERE clause, fetch MAIN row
 * and return a clean TupleTableSlot (or NULL).
 */
static TupleTableSlot *
ob_fetch_main_current_slot(Relation rel, const char *where_clause)
{
	StringInfoData sql;
	char	   *q_relname;
	int			ret;
	TupleTableSlot *result = NULL;
	TupleDesc	reldesc = RelationGetDescr(rel);
	TupleDesc	copy_desc;
	int			natts = reldesc->natts;

	q_relname = quote_qualified_identifier(
		get_namespace_name(RelationGetNamespace(rel)),
		RelationGetRelationName(rel));

	initStringInfo(&sql);
	appendStringInfo(&sql, "SELECT * FROM %s WHERE %s LIMIT 1",
					 q_relname, where_clause);
	pfree(q_relname);

	ret = ob_spi_one_shot(sql.data, true, 1);
	pfree(sql.data);

	if (ret == SPI_OK_SELECT && SPI_processed == 1 &&
		SPI_tuptable && SPI_tuptable->vals && SPI_tuptable->vals[0])
	{
		HeapTuple	htup = SPI_tuptable->vals[0];
		TupleDesc	td = SPI_tuptable->tupdesc;
		MemoryContext oldmc;
		TupleTableSlot *work;

		work = MakeSingleTupleTableSlot(td, &TTSOpsHeapTuple);
		ExecStoreHeapTuple(htup, work, false);
		ExecMaterializeSlot(work);

		oldmc = MemoryContextSwitchTo(TopMemoryContext);
		copy_desc = CreateTupleDescCopy(reldesc);
		result = MakeSingleTupleTableSlot(copy_desc, &TTSOpsVirtual);
		{
			int natts_result = copy_desc->natts;

			for (int i = 0; i < natts; i++)
			{
				Form_pg_attribute ratt = TupleDescAttr(reldesc, i);
				Datum		v;
				bool		isnull;
				AttrNumber	work_idx;

				if (ratt->attisdropped)
				{
					if (i < natts_result)
						result->tts_isnull[i] = true;
					continue;
				}

				work_idx = 0;
				for (int k = 0; k < td->natts; k++)
				{
					Form_pg_attribute x = TupleDescAttr(td, k);
					if (x->attisdropped)
						continue;
					if (strcmp(NameStr(x->attname), NameStr(ratt->attname)) == 0)
					{
						work_idx = k + 1;
						break;
					}
				}
				if (work_idx == 0)
				{
					ExecDropSingleTupleTableSlot(result);
					ExecDropSingleTupleTableSlot(work);
					result = NULL;
					MemoryContextSwitchTo(oldmc);
					SPI_finish();
					return NULL;
				}
				v = slot_getattr(work, work_idx, &isnull);
				if (isnull)
				{
					if (i < natts_result)
						result->tts_isnull[i] = true;
				}
				else
				{
					int16		typlen;
					bool		typbyval;
					char		typalign;
					Datum		copy_v;

					get_typlenbyvalalign(ratt->atttypid, &typlen, &typbyval, &typalign);
					copy_v = datumCopy(v, typbyval, typlen);
					if (i < natts_result)
					{
						result->tts_values[i] = copy_v;
						result->tts_isnull[i] = false;
					}
				}
			}
		}
		ExecStoreVirtualTuple(result);
		result->tts_tid = work->tts_tid;
		ExecDropSingleTupleTableSlot(work);
		MemoryContextSwitchTo(oldmc);
	}
	SPI_finish();
	return result;
}

/* ================================================================
 * --- 3 apply passes: DELETE → UPDATE → INSERT -----------------
 * ================================================================ */

static void
apply_relation_delete_pass(Relation rel, DeltaTuple *dt)
{
	char	   *where_clause;
	TupleTableSlot *main_slot;
	char	   *main_ver;

	elog(DEBUG1, "delpass[dbg] A1 key check");
	if (dt->key == NULL || *dt->key == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("apply DELETE on %s: delta has empty key",
						RelationGetRelationName(rel))));

	elog(DEBUG1, "delpass[dbg] A2 build_pk_where");
	where_clause = ob_build_pk_where_clause(rel, dt->key);
	elog(DEBUG1, "delpass[dbg] A3 build_pk_where DONE where=%s", where_clause ? where_clause : "NULL");
	if (where_clause == NULL || *where_clause == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("apply DELETE on %s: cannot build WHERE for pk=%s",
						RelationGetRelationName(rel), dt->key)));

	/* Pure-delta origin: old_version == NULL means this row never existed
	 * on MAIN (it was INSERTed then DELETEd entirely inside the branch).
	 * There is nothing to DELETE on MAIN; skipping prevents false-positive
	 * "row no longer present" conflict errors. */
	if (dt->old_version == NULL)
	{
		elog(DEBUG1, "delpass[dbg] A4 pure-delta DELETE (no MAIN baseline) → NOP skip pk=%s",
			 dt->key);
		pfree(where_clause);
		return;
	}

	elog(DEBUG1, "delpass[dbg] B fetch_main_slot");
	main_slot = ob_fetch_main_current_slot(rel, where_clause);
	elog(DEBUG1, "delpass[dbg] B2 fetch_main_slot DONE main_slot=%s", main_slot ? "nonNULL" : "NULL");
	if (main_slot == NULL)
	{
		char *reason = psprintf("DELETE delta pk=%s: MAIN row no longer present at apply time (concurrent DELETE or UPDATE rewrite).  expected old_version=%s.  Resolve manually and re-branch.",
							 dt->key, dt->old_version ? dt->old_version : "(null)");
		pfree(where_clause);
		overlay_guard_ereport_fail("apply_branch (conflict)",
								   RelationGetRelationName(rel),
								   reason);
	}

	elog(DEBUG1, "delpass[dbg] C tuple_version expected=%s", dt->old_version ? dt->old_version : "(null)");
	main_ver = overlay_tuple_version(rel, main_slot);
	elog(DEBUG1, "delpass[dbg] C2 tuple_version DONE main_ver=%s", main_ver ? main_ver : "(null)");
	if (dt->old_version == NULL || main_ver == NULL ||
		strcmp(dt->old_version, main_ver) != 0)
	{
		char *reason = psprintf("conflict pk=%s: old_version expected=%s but MAIN latest=%s",
							 dt->key,
							 dt->old_version ? dt->old_version : "(null)",
							 main_ver ? main_ver : "(null)");
		elog(DEBUG1, "delpass[dbg] CONFLICT %s", reason);
		if (main_ver) pfree(main_ver);
		ExecDropSingleTupleTableSlot(main_slot);
		pfree(where_clause);
		overlay_guard_ereport_fail("apply_branch (conflict)",
								   RelationGetRelationName(rel),
								   reason);
	}
	elog(DEBUG1, "delpass[dbg] D pfree main_ver");
	if (main_ver) pfree(main_ver);
	elog(DEBUG1, "delpass[dbg] E drop slot");
	ExecDropSingleTupleTableSlot(main_slot);
	elog(DEBUG1, "delpass[dbg] F build DELETE SQL");

	{
		StringInfoData del_sql;
		char	   *q_relname;
		int			ret;

		q_relname = quote_qualified_identifier(
			get_namespace_name(RelationGetNamespace(rel)),
			RelationGetRelationName(rel));

		initStringInfo(&del_sql);
		appendStringInfo(&del_sql, "DELETE FROM %s WHERE %s",
						 q_relname, where_clause);
		pfree(q_relname);
		pfree(where_clause);

		elog(DEBUG1, "delpass DELETE sql=%s", del_sql.data);
		ret = ob_spi_one_shot(del_sql.data, false, 1);
		elog(DEBUG1, "delpass DELETE ret=%d processed=%lu", ret, SPI_processed);
		SPI_finish();
		pfree(del_sql.data);
		if (ret != SPI_OK_DELETE)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("apply DELETE failed ret=%d", ret)));
	}
	elog(DEBUG1, "delpass[dbg] Z DONE");
}

static void
apply_relation_update_pass(Relation rel, DeltaTuple *dt)
{
	char	   *where_clause;
	TupleTableSlot *main_slot;
	char	   *main_ver;

	if (dt->key == NULL || *dt->key == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("apply UPDATE on %s: delta has empty key",
						RelationGetRelationName(rel))));
	if (dt->tuple_data == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("apply UPDATE on %s: delta has NULL tuple_data for pk=%s",
						RelationGetRelationName(rel), dt->key)));

	where_clause = ob_build_pk_where_clause(rel, dt->key);
	main_slot = ob_fetch_main_current_slot(rel, where_clause);
	if (main_slot == NULL)
	{
		char *reason = psprintf("UPDATE pk=%s MAIN row missing at apply time (concurrent DELETE)",
							 dt->key);
		pfree(where_clause);
		overlay_guard_ereport_fail("apply_branch (conflict)",
								   RelationGetRelationName(rel),
								   reason);
	}

	main_ver = overlay_tuple_version(rel, main_slot);
	if (dt->old_version == NULL || main_ver == NULL ||
		strcmp(dt->old_version, main_ver) != 0)
	{
		char *reason = psprintf("conflict pk=%s: old_version expected=%s MAIN latest=%s",
							 dt->key,
							 dt->old_version ? dt->old_version : "(null)",
							 main_ver ? main_ver : "(null)");
		if (main_ver) pfree(main_ver);
		ExecDropSingleTupleTableSlot(main_slot);
		pfree(where_clause);
		overlay_guard_ereport_fail("apply_branch (conflict)",
								   RelationGetRelationName(rel),
								   reason);
	}
	if (main_ver) pfree(main_ver);
	ExecDropSingleTupleTableSlot(main_slot);

	{
		TupleDesc	reldesc = RelationGetDescr(rel);
		StringInfoData colnames;
		StringInfoData colnames_parens;
		StringInfoData colspec;
		int			natts = reldesc->natts;
		Oid			bytea_out_func;
		bool		bytea_out_varlena;
		char	   *bytea_lit;
		StringInfoData upd_sql;
		char	   *q_relname;
		int			ret;
		bool		first = true;
		int			i;

		initStringInfo(&colnames);
		initStringInfo(&colnames_parens);
		initStringInfo(&colspec);
		for (i = 0; i < natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(reldesc, i);
			if (att->attisdropped)
				continue;
			if (!first)
			{
				appendStringInfoChar(&colnames, ',');
				appendStringInfoChar(&colnames_parens, ',');
				appendStringInfoChar(&colspec, ',');
			}
			appendStringInfo(&colnames, "%s",
							 quote_identifier(NameStr(att->attname)));
			appendStringInfo(&colnames_parens, "j.%s",
							 quote_identifier(NameStr(att->attname)));
			appendStringInfo(&colspec, "%s %s",
							 quote_identifier(NameStr(att->attname)),
							 format_type_with_typemod(att->atttypid, att->atttypmod));
			first = false;
		}

		getTypeOutputInfo(BYTEAOID, &bytea_out_func, &bytea_out_varlena);
		bytea_lit = OidOutputFunctionCall(bytea_out_func,
										  PointerGetDatum(dt->tuple_data));
		{
			char *ql = quote_literal_cstr(bytea_lit ? bytea_lit : "");
			(void) ql;
			q_relname = quote_qualified_identifier(
				get_namespace_name(RelationGetNamespace(rel)),
				RelationGetRelationName(rel));

			initStringInfo(&upd_sql);
			appendStringInfo(&upd_sql,
							 "UPDATE %s SET (%s) = ("
							 "SELECT %s FROM jsonb_populate_record(NULL::%s, "
							 "convert_from(decode(substring(%s from 3), 'hex'), 'UTF8')::jsonb) "
							 "AS j) "
							 "WHERE %s",
							 q_relname, colnames.data,
							 colnames_parens.data, q_relname,
							 ql,
							 where_clause);
		}
		pfree(q_relname);
		pfree(where_clause);
		if (bytea_lit) pfree(bytea_lit);
		pfree(colnames.data);
		pfree(colnames_parens.data);
		pfree(colspec.data);

		elog(DEBUG1, "updpass[dbg] G upd_sql=%s", upd_sql.data);
		ret = ob_spi_one_shot(upd_sql.data, false, 1);
		elog(DEBUG1, "updpass[dbg] G2 ret=%d processed=%lu", ret, SPI_processed);
		pfree(upd_sql.data);
		if (ret != SPI_OK_UPDATE && ret != SPI_OK_UPDATE_RETURNING)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("apply UPDATE failed ret=%d", ret)));
		SPI_finish();
	}
}

static void
apply_relation_insert_pass(Relation rel, DeltaTuple *dt)
{
	char	   *q_relname;
	int			ret;
	TupleDesc	reldesc;
	StringInfoData colnames;
	StringInfoData colspec;
	StringInfoData colnames_parens;
	int			natts;
	Oid			bytea_out_func;
	bool		bytea_out_varlena;
	char	   *bytea_lit;
	StringInfoData ins_sql;
	bool		first = true;
	int			i;
	char	   *where_clause;
	TupleTableSlot *main_slot;

	if (dt->tuple_data == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("apply INSERT on %s: delta has NULL tuple_data pk=%s",
						RelationGetRelationName(rel), dt->key ? dt->key : "(null)")));

	if (dt->key != NULL && *dt->key != '\0')
	{
		where_clause = ob_build_pk_where_clause(rel, dt->key);
		main_slot = ob_fetch_main_current_slot(rel, where_clause);
		if (main_slot != NULL)
		{
			char *reason = psprintf("INSERT pk=%s MAIN row already present (MAIN pk already contains this PK)",
								 dt->key);
			ExecDropSingleTupleTableSlot(main_slot);
			pfree(where_clause);
			overlay_guard_ereport_fail("apply_branch (conflict)",
									   RelationGetRelationName(rel),
									   reason);
		}
		pfree(where_clause);
	}

	reldesc = RelationGetDescr(rel);
	natts = reldesc->natts;
	initStringInfo(&colnames);
	initStringInfo(&colspec);
	initStringInfo(&colnames_parens);
	for (i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(reldesc, i);

		if (att->attisdropped)
			continue;
		if (!first)
		{
			appendStringInfoChar(&colnames, ',');
			appendStringInfoChar(&colspec, ',');
			appendStringInfoChar(&colnames_parens, ',');
		}
		appendStringInfo(&colnames, "%s",
						 quote_identifier(NameStr(att->attname)));
		appendStringInfo(&colnames_parens, "j.%s",
						 quote_identifier(NameStr(att->attname)));
		appendStringInfo(&colspec, "%s %s",
						 quote_identifier(NameStr(att->attname)),
						 format_type_be(att->atttypid));
		first = false;
	}

	getTypeOutputInfo(BYTEAOID, &bytea_out_func, &bytea_out_varlena);
	bytea_lit = OidOutputFunctionCall(bytea_out_func,
								   PointerGetDatum(dt->tuple_data));
	{
		char *ql = quote_literal_cstr(bytea_lit ? bytea_lit : "");
		(void) ql;
		q_relname = quote_qualified_identifier(
			get_namespace_name(RelationGetNamespace(rel)),
			RelationGetRelationName(rel));

		initStringInfo(&ins_sql);
		appendStringInfo(&ins_sql,
						 "INSERT INTO %s (%s) "
						 "SELECT %s FROM jsonb_populate_record(NULL::%s, "
						 "convert_from(decode(substring(%s from 3), 'hex'), 'UTF8')::jsonb) "
						 "AS j",
						 q_relname, colnames.data, colnames_parens.data,
						 q_relname,
						 ql);
	}
	pfree(q_relname);
	if (bytea_lit) pfree(bytea_lit);
	pfree(colnames.data);
	pfree(colnames_parens.data);
	pfree(colspec.data);

	elog(DEBUG1, "inspass[dbg] C ins_sql=%s", ins_sql.data);
	ret = ob_spi_one_shot(ins_sql.data, false, 1);
	elog(DEBUG1, "inspass[dbg] C2 ret=%d processed=%lu", ret, SPI_processed);
	pfree(ins_sql.data);
	if (ret != SPI_OK_INSERT && ret != SPI_OK_INSERT_RETURNING)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("apply INSERT failed ret=%d", ret)));
	SPI_finish();
}
