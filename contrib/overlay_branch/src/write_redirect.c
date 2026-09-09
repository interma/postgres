/*-------------------------------------------------------------------------
 *
 * write_redirect.c
 *	  Overlay Branch Step 4a: ExecutorRun DML write-redirection
 *
 *	  Intercepts ModifyTable (INSERT / UPDATE / DELETE) inside an active
 *	  branch and redirects writes into pg_branch_delta via overlay_delta_*
 *	  helpers, leaving the MAIN heap completely untouched.
 *
 *	  Entry point: overlay_executor_run_intercept() — called from the
 *	  overlay_ExecutorRun hook in overlay_branch.c after hard-guard checks
 *	  pass.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "overlay_branch.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "storage/lmgr.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#define OBTABLE_DELTA   OBSCHEMA ".pg_branch_delta"
#define OBTABLE_BRANCH  OBSCHEMA ".pg_branch"

/* ----------------------------------------------------------------
 * mt_state_result_rel: small helper that extracts the i-th
 * ResultRelInfo from a ModifyTableState.
 * ----------------------------------------------------------------
 */
ResultRelInfo *
mt_state_result_rel(ModifyTableState *mt, int i)
{
	Assert(i >= 0 && i < mt->mt_nrels);
	return &mt->resultRelInfo[i];
}

/* ----------------------------------------------------------------
 * overlay_executor_run_intercept
 *
 *   Full write-redirection body extracted from overlay_ExecutorRun.
 *   Returns true if the call handled the DML (caller must return
 *   immediately, must NOT fall through to standard ExecutorRun).
 *   Returns false if caller should fall through (catalog tables,
 *   multi-target DML, non-redirectable relations etc.)
 *
 *   Semantics preserved from the original monolithic L824-L1208:
 *     • Step7 DML guard runs over mt_nrels + es_range_table entries
 *     • CMD_UPDATE / CMD_DELETE:
 *         – no RETURNING support (MVP)
 *         – extract ctid from junk slot → re-fetch clean MAIN slot
 *         – capture old_version (ctid+xmin token) BEFORE mutation
 *         – for UPDATE, merge SET-values by column NAME into clean slot
 *         – serialize_pk + serialize_tuple → overlay_delta_insert
 *     • CMD_INSERT:
 *         – no RETURNING support (MVP)
 *         – serialize_pk + serialize_tuple per row → overlay_delta_insert
 *     • ob_write_redirect_done label placed AFTER
 *       prev/standard_ExecutorRun so catalog/non-redirected DML falls
 *       through to standard (otherwise overlay_branch catalog INSERTS
 *       from SPI silently never persist).
 * ----------------------------------------------------------------
 */
bool
overlay_executor_run_intercept(QueryDesc *queryDesc,
							   ScanDirection direction,
							   uint64 count, bool execute_once,
							   ExecutorRun_hook_type_fn prev_ExecutorRun)
{
	CmdType		cmd = queryDesc->operation;
	ModifyTableState *mt = NULL;
	ModifyTable      *plan = NULL;
	bool		handled = false;
	extern char *slot_get_ctid_cstr(TupleTableSlot *slot);
	extern TupleTableSlot *fetch_tuple_by_ctid(Relation rel, const char *ctid_cstr);

	if (!overlay_branch_is_active() ||
		queryDesc->planstate == NULL ||
		!IsA(queryDesc->planstate, ModifyTableState))
		return false;

	overlay_write_redirect_enter();
	PG_TRY();
	{
		mt = (ModifyTableState *) queryDesc->planstate;
		plan = (ModifyTable *) mt->ps.plan;

		/* --- Step7 DML hard guard (result rels + rtable) --- */
		{
			int			i;
			int			nres = mt->mt_nrels;
			int			planres = list_length(plan->resultRelations);
			EState	   *estate = mt->ps.state;

			if (nres <= 0) nres = planres;
			for (i = 0; i < nres; i++)
			{
				ResultRelInfo *rri = mt_state_result_rel(mt, i);
				Relation	rel;
				const char *opstr;
				char	   *gr = NULL;

				if (rri == NULL) continue;
				rel = rri->ri_RelationDesc;
				if (rel == NULL) continue;

				opstr = cmd == CMD_INSERT ? "INSERT" :
						cmd == CMD_UPDATE ? "UPDATE" :
						cmd == CMD_DELETE ? "DELETE" : "DML";

				if (!overlay_guard_rel_ok(rel, &gr))
					overlay_guard_ereport_fail(opstr,
											   RelationGetRelationName(rel), gr);
			}

			if (estate != NULL)
			{
				int			rt_size = estate->es_range_table_size;
				List	   *rtable = estate->es_range_table;

				if (rtable != NULL)
				{
					for (i = 1; i <= rt_size; i++)
					{
						RangeTblEntry *rte = exec_rt_fetch(i, estate);
						Relation	rel;
						const char *opstr;
						char	   *gr = NULL;

						if (rte == NULL) continue;
						if (rte->rtekind != RTE_RELATION) continue;

						rel = ExecGetRangeTableRelation(estate, i);
						if (rel == NULL) continue;

						{
							Oid			nspoid = RelationGetNamespace(rel);
							char	   *nsp = get_namespace_name(nspoid);
							bool		skip_catalog = false;
							if (nsp && (strcmp(nsp, "pg_catalog") == 0 ||
										strcmp(nsp, OBSCHEMA) == 0 ||
										strncmp(nsp, "pg_toast", 8) == 0))
								skip_catalog = true;
							if (skip_catalog) continue;
						}

						opstr = cmd == CMD_INSERT ? "INSERT" :
								cmd == CMD_UPDATE ? "UPDATE" :
								cmd == CMD_DELETE ? "DELETE" : "DML";
						if (!overlay_guard_rel_ok(rel, &gr))
							overlay_guard_ereport_fail(opstr,
													   RelationGetRelationName(rel), gr);
					}
				}
			}
		}

		/* --- Single target only (MVP) --- */
		if (mt->mt_nrels == 1 && list_length(plan->resultRelations) == 1)
		{
			ResultRelInfo *rri = mt_state_result_rel(mt, 0);
			Relation	rel = rri->ri_RelationDesc;

			/* --- CMD_UPDATE / CMD_DELETE redirect --- */
			if ((cmd == CMD_UPDATE || cmd == CMD_DELETE) &&
				overlay_should_redirect(rel))
			{
				PlanState  *subplan;
				uint64		ndone = 0;
				TupleTableSlot *junk_slot;
				char		op = (cmd == CMD_UPDATE) ? DELTA_OP_UPDATE
													 : DELTA_OP_DELETE;

				if (plan->returningLists != NIL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("overlay_branch Step 4 MVP does not yet support %s ... RETURNING inside an active branch",
									cmd == CMD_UPDATE ? "UPDATE" : "DELETE")));

				subplan = outerPlanState(mt);

				for (;;)
				{
					char	   *pk;
					char	   *tdata;
					char	   *oldver;
					TupleTableSlot *old_clean_slot;
					char	   *ctid_cstr;

					CHECK_FOR_INTERRUPTS();

					junk_slot = ExecProcNode(subplan);
					if (TupIsNull(junk_slot))
						break;
					if (count != 0 && ndone >= count)
						break;

					elog(DEBUG2, "UD[dbg] A got slot, cmd=%s",
						 cmd == CMD_UPDATE ? "UPDATE" : "DELETE");

					ctid_cstr = slot_get_ctid_cstr(junk_slot);
					if (ctid_cstr == NULL)
						ereport(ERROR,
								(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								 errmsg("overlay_branch %s hook: slot has no ctid column",
										cmd == CMD_UPDATE ? "UPDATE" : "DELETE")));
					elog(DEBUG2, "UD[dbg] B ctid=%s", ctid_cstr);
					old_clean_slot = fetch_tuple_by_ctid(rel, ctid_cstr);
					pfree(ctid_cstr);
					elog(DEBUG2, "UD[dbg] C fetched old row");

					oldver = overlay_tuple_version(rel, old_clean_slot);
					elog(DEBUG2, "UD[dbg] C2 old_version=%s", oldver ? oldver : "(null)");

					if (cmd == CMD_UPDATE)
					{
						TupleDesc	reldesc = RelationGetDescr(rel);
						TupleDesc	junkdesc = junk_slot->tts_tupleDescriptor;
						MemoryContext oldmc;
						int			r;

						elog(DEBUG2, "UD[dbg] D UPDATE set-merge: reldesc_natts=%d junk_natts=%d",
							 reldesc->natts, junkdesc->natts);

						oldmc = MemoryContextSwitchTo(TopMemoryContext);
						for (r = 0; r < reldesc->natts; r++)
						{
							Form_pg_attribute ratt = TupleDescAttr(reldesc, r);
							const char *rname = NameStr(ratt->attname);
							int			j;

							if (ratt->attisdropped)
								continue;

							for (j = 0; j < junkdesc->natts; j++)
							{
								Form_pg_attribute jatt = TupleDescAttr(junkdesc, j);
								if (jatt->attisdropped)
									continue;
								if (strcmp(NameStr(jatt->attname), rname) == 0)
								{
									bool		jisnull;
									Datum		jvalue = slot_getattr(junk_slot,
												  (AttrNumber) (j + 1),
												  &jisnull);

									elog(DEBUG2, "UD[dbg] D[%d] set %s: j=%d jtype=%u jisnull=%d",
										 r, rname, j, jatt->atttypid, jisnull ? 1 : 0);

									if (!jisnull)
									{
										int16		typlen;
										bool		typbyval;

										get_typlenbyval(ratt->atttypid, &typlen, &typbyval);
										jvalue = datumCopy(jvalue, typbyval, typlen);
										old_clean_slot->tts_values[r] = jvalue;
										old_clean_slot->tts_isnull[r] = false;
									}
									break;
								}
							}
						}
						MemoryContextSwitchTo(oldmc);
						elog(DEBUG2, "UD[dbg] E before serialize_tuple");
						tdata = overlay_serialize_tuple(rel, old_clean_slot);
						elog(DEBUG2, "UD[dbg] F serialize_tuple done, tdata=%p", tdata);
					}
					else
					{
						tdata = NULL;
					}

					elog(DEBUG2, "UD[dbg] G before serialize_pk");
					pk = overlay_serialize_pk(rel, old_clean_slot);
					elog(DEBUG2, "UD[dbg] H pk=%s", pk ? pk : "(null)");

					overlay_delta_insert(overlay_branch_get_current_id(),
										RelationGetRelid(rel), pk, op,
										oldver, tdata);
					elog(DEBUG2, "UD[dbg] I delta_insert done");
					ndone++;

					pfree(pk);
					if (tdata) pfree(tdata);
					if (oldver) pfree(oldver);
					ExecDropSingleTupleTableSlot(old_clean_slot);
					ExecClearTuple(junk_slot);
					elog(DEBUG2, "UD[dbg] J row done, ndone=%lu", (unsigned long) ndone);
				}

				queryDesc->estate->es_processed = ndone;
				handled = true;
				goto ob_write_redirect_done;
			}

			/* --- CMD_INSERT redirect --- */
			if (cmd == CMD_INSERT && overlay_should_redirect(rel))
			{
				PlanState  *subplan;
				uint64		ninserted = 0;
				TupleTableSlot *slot;

				if (plan->returningLists != NIL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("overlay_branch Step 3 MVP does not yet support INSERT ... RETURNING inside an active branch")));

				subplan = outerPlanState(mt);

				for (;;)
				{
					char	   *pk;
					char	   *tdata;

					CHECK_FOR_INTERRUPTS();

					slot = ExecProcNode(subplan);
					if (TupIsNull(slot))
						break;
					if (count != 0 && ninserted >= count)
						break;

					pk = overlay_serialize_pk(rel, slot);
					tdata = overlay_serialize_tuple(rel, slot);

					overlay_delta_insert(overlay_branch_get_current_id(),
										RelationGetRelid(rel), pk,
										DELTA_OP_INSERT, NULL, tdata);
					ninserted++;

					pfree(pk);
					pfree(tdata);
					ExecClearTuple(slot);
				}

				queryDesc->estate->es_processed = ninserted;
				handled = true;
				goto ob_write_redirect_done;
			}
		}

		/* Fall through to standard (catalog tables etc.) */
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count, execute_once);
		else
			standard_ExecutorRun(queryDesc, direction, count, execute_once);
		handled = true;

ob_write_redirect_done:
		/* If we explicitly handled DML via goto above, we MUST NOT
		 * fall through to standard.  If we fell through to standard,
		 * handled is already set true and we just exit guard. */
	}
	PG_CATCH();
	{
		overlay_write_redirect_exit();
		PG_RE_THROW();
	}
	PG_END_TRY();
	overlay_write_redirect_exit();
	return handled;
}
