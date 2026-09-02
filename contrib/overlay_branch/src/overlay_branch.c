/*-------------------------------------------------------------------------
 *
 * overlay_branch.c
 *	  Overlay Branch extension: speculative database state using
 *	  table overlay and delta store.
 *
 *	  This file contains:
 *	  - Module load callback (_PG_init) with GUC registration
 *	  - SQL-callable functions (create/use/apply/discard/current/list)
 *	  - Stub implementations for:
 *	      Branch Context | Write Redirect | BranchScan | Delta Store
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
#include "catalog/pg_database.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/makefuncs.h"
#include "storage/lmgr.h"
#include "tcop/utility.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/* ================================================================
 * Module magic
 * ================================================================ */
PG_MODULE_MAGIC;

/* ================================================================
 * GUC variables
 * ================================================================ */
char	   *overlay_branch_current_name = NULL;	/* session GUC */
static bool overlay_branch_enabled = true;

/* ================================================================
 * Global session state
 * ================================================================ */
BranchContext *CurrentBranchContext = NULL;

/* ================================================================
 * Saved hook values (for chaining in future)
 * ================================================================ */
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* ================================================================
 * Forward declarations
 * ================================================================ */
void		_PG_init(void);

static bool overlay_guc_check_assign_current_branch(char **newval,
												   void **extra,
												   GucSource source);
static void overlay_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void overlay_ExecutorRun(QueryDesc *queryDesc,
								ScanDirection direction,
								uint64 count, bool execute_once);
static void overlay_ExecutorFinish(QueryDesc *queryDesc);
static void overlay_ExecutorEnd(QueryDesc *queryDesc);
static void overlay_ProcessUtility(PlannedStmt *pstmt,
								   const char *queryString,
								   bool readOnlyTree,
								   ProcessUtilityContext context,
								   ParamListInfo params,
								   QueryEnvironment *queryEnv,
								   DestReceiver *dest,
								   QueryCompletion *qc);

/* ================================================================
 * SQL-callable function declarations
 * ================================================================ */
PG_FUNCTION_INFO_V1(overlay_branch_create);
PG_FUNCTION_INFO_V1(overlay_branch_use);
PG_FUNCTION_INFO_V1(overlay_branch_current);
PG_FUNCTION_INFO_V1(overlay_branch_apply);
PG_FUNCTION_INFO_V1(overlay_branch_discard);
PG_FUNCTION_INFO_V1(overlay_branch_list);

/* ================================================================
 * Module load callback
 * ================================================================ */
void
_PG_init(void)
{
	MemoryContext oldctx;

	if (!process_shared_preload_libraries_in_progress &&
		!IsUnderPostmaster)
		return;

	/*
	 * Allocate the per-session branch context in TopMemoryContext so it
	 * survives across transactions (but not across sessions).
	 */
	oldctx = MemoryContextSwitchTo(TopMemoryContext);

	CurrentBranchContext = (BranchContext *) palloc0(sizeof(BranchContext));
	CurrentBranchContext->branch_id = 0;
	CurrentBranchContext->branch_name[0] = '\0';
	CurrentBranchContext->is_active = false;
	strcpy(CurrentBranchContext->mode, BRANCH_MODE_LIVE);
	CurrentBranchContext->created_at = 0;

	MemoryContextSwitchTo(oldctx);

	/* ----------
	 * Define GUC: overlay_branch.current
	 * Setting this name triggers USE BRANCH via check_hook.
	 * ----------
	 */
	DefineCustomStringVariable("overlay_branch.current",
							   "Set the current active branch for this session.",
							   "Set to the branch name, or empty/NULL to leave the branch.",
							   &overlay_branch_current_name,
							   "",
							   PGC_USERSET,
							   0,
							   overlay_guc_check_assign_current_branch,
							   NULL,
							   NULL);

	DefineCustomBoolVariable("overlay_branch.enabled",
							 "Enable or disable overlay branch processing.",
							 NULL,
							 &overlay_branch_enabled,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	MarkGUCPrefixReserved("overlay_branch");

	/* ----------
	 * Install executor hooks for:
	 *   - Write Redirect (ModifyTable -> BranchModify)
	 *   - BranchScan (planner / executor integration TBD)
	 * ----------
	 */
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = overlay_ExecutorStart;

	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = overlay_ExecutorRun;

	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = overlay_ExecutorFinish;

	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = overlay_ExecutorEnd;

	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = overlay_ProcessUtility;

	elog(DEBUG1, "overlay_branch: module loaded");
}

/* ================================================================
 * GUC check hook for overlay_branch.current
 * SET overlay_branch.current = 'b1'  =>  equivalent to USE BRANCH b1
 * ================================================================ */
static bool
overlay_guc_check_assign_current_branch(char **newval, void **extra,
										GucSource source)
{
	const char *val = (newval && *newval) ? *newval : "";

	if (*val == '\0')
	{
		/* Leave current branch */
		if (CurrentBranchContext && CurrentBranchContext->is_active)
		{
			CurrentBranchContext->is_active = false;
			CurrentBranchContext->branch_id = 0;
			CurrentBranchContext->branch_name[0] = '\0';
			elog(DEBUG1, "overlay_branch: left branch (via GUC)");
		}
	}
	else
	{
		/* Enter named branch: forward to internal use_branch */
		overlay_branch_use_internal(val);
	}
	return true;
}

/* ================================================================
 * Executor hooks (stubs for Write Redirect & BranchScan)
 * ================================================================ */
static void
overlay_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	/*
	 * TODO: Planner integration.
	 * When overlay_branch_is_active() == true, walk the plan tree and
	 * replace SeqScan/IndexScan on user tables with CustomScan (BranchScan).
	 */

	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

static void
overlay_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
					uint64 count, bool execute_once)
{
	if (prev_ExecutorRun)
		prev_ExecutorRun(queryDesc, direction, count, execute_once);
	else
		standard_ExecutorRun(queryDesc, direction, count, execute_once);
}

static void
overlay_ExecutorFinish(QueryDesc *queryDesc)
{
	if (prev_ExecutorFinish)
		prev_ExecutorFinish(queryDesc);
	else
		standard_ExecutorFinish(queryDesc);
}

static void
overlay_ExecutorEnd(QueryDesc *queryDesc)
{
	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

static void
overlay_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					   bool readOnlyTree, ProcessUtilityContext context,
					   ParamListInfo params, QueryEnvironment *queryEnv,
					   DestReceiver *dest, QueryCompletion *qc)
{
	/*
	 * TODO: intercept CREATE BRANCH / USE BRANCH / APPLY BRANCH /
	 * DISCARD BRANCH utility statements here when we add grammar support.
	 * For now these operations go through the SQL functions below.
	 */

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
						   params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
}

/* ================================================================
 * SQL-callable function: create_branch(branch_name name) => branch_id
 * ================================================================ */
Datum
overlay_branch_create(PG_FUNCTION_ARGS)
{
	Name		branch_name = PG_GETARG_NAME(0);
	int32		new_branch_id;

	new_branch_id = overlay_branch_create_internal(NameStr(*branch_name));
	PG_RETURN_INT32(new_branch_id);
}

/* ================================================================
 * SQL-callable function: use_branch(branch_name name) => void
 * ================================================================ */
Datum
overlay_branch_use(PG_FUNCTION_ARGS)
{
	Name		branch_name = PG_GETARG_NAME(0);

	overlay_branch_use_internal(NameStr(*branch_name));
	PG_RETURN_VOID();
}

/* ================================================================
 * SQL-callable function: current_branch() => name
 * ================================================================ */
Datum
overlay_branch_current(PG_FUNCTION_ARGS)
{
	const char *name = overlay_branch_get_current_name();
	NameData	result;

	if (name == NULL || *name == '\0')
		PG_RETURN_NULL();

	namestrcpy(&result, name);
	PG_RETURN_NAME(&result);
}

/* ================================================================
 * SQL-callable function: apply_branch(branch_name name) => void
 * ================================================================ */
Datum
overlay_branch_apply(PG_FUNCTION_ARGS)
{
	Name		branch_name = PG_GETARG_NAME(0);

	overlay_branch_apply_internal(NameStr(*branch_name));
	PG_RETURN_VOID();
}

/* ================================================================
 * SQL-callable function: discard_branch(branch_name name) => void
 * ================================================================ */
Datum
overlay_branch_discard(PG_FUNCTION_ARGS)
{
	Name		branch_name = PG_GETARG_NAME(0);

	overlay_branch_discard_internal(NameStr(*branch_name));
	PG_RETURN_VOID();
}

/* ================================================================
 * SQL-callable function: list_branches() => setof record
 * ================================================================ */
Datum
overlay_branch_list(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	int			call_cntr;
	int			max_calls;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		tupdesc = CreateTemplateTupleDesc(7);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "branch_id",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "branch_name",
						   NAMEOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "owner",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "created_at",
						   TIMESTAMPTZOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 5, "mode",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 6, "state",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 7, "delta_count",
						   INT8OID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		/*
		 * TODO: actually read from pg_branch + count from pg_branch_delta.
		 * For the skeleton we return an empty set.
		 */
		funcctx->max_calls = 0;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;

	if (call_cntr < max_calls)
	{
		SRF_RETURN_NEXT(funcctx, (Datum) 0);
	}
	else
	{
		SRF_RETURN_DONE(funcctx);
	}
}

/* ================================================================
 * ---------- Branch Context (internal implementations) ----------
 * ================================================================ */

/* ----------
 * overlay_branch_create_internal
 *
 * Insert a new row into pg_branch. Returns the new branch_id.
 * ----------
 */
int32
overlay_branch_create_internal(const char *branch_name)
{
	int32		new_branch_id = 0;

	if (branch_name == NULL || *branch_name == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("branch name cannot be empty")));

	if (strlen(branch_name) >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_NAME_TOO_LONG),
				 errmsg("branch name too long")));

	/*
	 * TODO: check duplicate name, insert into pg_catalog.pg_branch.
	 * For the skeleton we just allocate a dummy id and elog NOTICE.
	 */
	elog(NOTICE, "overlay_branch: CREATE BRANCH '%s' (skeleton stub - not inserting to pg_branch yet)",
		 branch_name);

	/* Simulated id for skeleton */
	new_branch_id = 1;

	elog(DEBUG1, "overlay_branch_create_internal: new_branch_id=%d", new_branch_id);
	return new_branch_id;
}

/* ----------
 * overlay_branch_use_internal
 *
 * Set CurrentBranchContext to the named branch.
 * ----------
 */
void
overlay_branch_use_internal(const char *branch_name)
{
	if (branch_name == NULL || *branch_name == '\0')
	{
		/* Leave the branch */
		if (CurrentBranchContext)
		{
			CurrentBranchContext->is_active = false;
			CurrentBranchContext->branch_id = 0;
			CurrentBranchContext->branch_name[0] = '\0';
		}
		elog(DEBUG1, "overlay_branch: left current branch (via use_internal)");
		return;
	}

	/*
	 * TODO: look up branch_name in pg_branch.
	 * For the skeleton we simply switch with a NOTICE.
	 */
	elog(NOTICE, "overlay_branch: USE BRANCH '%s' (skeleton stub)", branch_name);

	if (CurrentBranchContext)
	{
		CurrentBranchContext->branch_id = 1;	/* placeholder */
		strlcpy(CurrentBranchContext->branch_name, branch_name, NAMEDATALEN);
		CurrentBranchContext->is_active = true;
		strcpy(CurrentBranchContext->mode, BRANCH_MODE_LIVE);
		CurrentBranchContext->created_at = GetCurrentTimestamp();
	}
}

/* ----------
 * overlay_branch_get_current_name / _id / is_active
 * ----------
 */
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
	return (overlay_branch_enabled &&
			CurrentBranchContext != NULL &&
			CurrentBranchContext->is_active);
}

/* ----------
 * overlay_branch_apply_internal
 *
 * "Reality Commit": replay each delta entry into the real table with
 * optimistic conflict detection. Then mark branch state = 'applied'.
 * ----------
 */
void
overlay_branch_apply_internal(const char *branch_name)
{
	elog(NOTICE, "overlay_branch: APPLY BRANCH '%s' (skeleton stub - conflict detection TBD)",
		 branch_name);

	/* TODO:
	 * 1. BEGIN (implicit via current xact)
	 * 2. SELECT * FROM pg_branch_delta WHERE branch_name = $1 ORDER BY relid, key
	 * 3. For each delta:
	 *    a. SELECT current tuple from the real table by PK
	 *    b. Compare old_version (conflict check)
	 *    c. If OK: INSERT/UPDATE/DELETE the real table
	 *    d. If MISMATCH: ERROR with conflict info
	 * 4. UPDATE pg_branch SET state = 'applied'
	 */
}

/* ----------
 * overlay_branch_discard_internal
 *
 * Throw away all deltas and mark branch discarded. Does NOT touch main tables.
 * ----------
 */
void
overlay_branch_discard_internal(const char *branch_name)
{
	elog(NOTICE, "overlay_branch: DISCARD BRANCH '%s' (skeleton stub)", branch_name);

	/* TODO:
	 * 1. DELETE FROM pg_branch_delta WHERE branch_name = $1
	 * 2. UPDATE pg_branch SET state = 'discarded'
	 * 3. If it was the current branch, also leave it
	 */
	if (CurrentBranchContext &&
		CurrentBranchContext->is_active &&
		strcmp(CurrentBranchContext->branch_name, branch_name) == 0)
	{
		CurrentBranchContext->is_active = false;
		CurrentBranchContext->branch_id = 0;
		CurrentBranchContext->branch_name[0] = '\0';
	}
}

/* ================================================================
 * ---------- Delta Store (stub implementations) ----------
 * ================================================================ */

void
overlay_delta_insert(int32 branch_id, Oid relid, const char *key,
					 char op, const char *old_version, bytea *tuple_data)
{
	elog(DEBUG1, "overlay_delta_insert: branch_id=%d relid=%u key=%s op=%c",
		 branch_id, relid, key ? key : "(null)", op);
	/* TODO: INSERT INTO pg_branch_delta ON CONFLICT (branch_id,relid,key) DO UPDATE */
}

void
overlay_delta_update(int32 branch_id, Oid relid, const char *key,
					 char op, const char *old_version, bytea *tuple_data)
{
	elog(DEBUG1, "overlay_delta_update: branch_id=%d relid=%u key=%s op=%c",
		 branch_id, relid, key ? key : "(null)", op);
	/* TODO: UPDATE pg_branch_delta ... */
}

bool
overlay_delta_lookup(int32 branch_id, Oid relid, const char *key,
					 DeltaTuple *out_tuple)
{
	/* TODO: SELECT FROM pg_branch_delta ... */
	return false;
}

List *
overlay_delta_list_for_rel(int32 branch_id, Oid relid)
{
	/* TODO: list build from pg_branch_delta */
	return NIL;
}

int64
overlay_delta_count(int32 branch_id)
{
	/* TODO: SELECT count(*) FROM pg_branch_delta WHERE branch_id = ... */
	return 0;
}

void
overlay_delta_delete_all(int32 branch_id)
{
	/* TODO: DELETE FROM pg_branch_delta WHERE branch_id = $1 */
}

/* ================================================================
 * ---------- Write Redirect (stub implementations) ----------
 * ================================================================ */

bool
overlay_should_redirect(Relation rel)
{
	if (!overlay_branch_is_active())
		return false;
	if (rel == NULL)
		return false;

	/* Only redirect user heap tables with a PK, skip catalog tables,
	 * our own pg_branch / pg_branch_delta, partitions, etc. */
	if (rel->rd_rel->relisshared)
		return false;

	/* Skip our own catalog tables (guard against recursion) */
	/* TODO: match by namespace + relname properly */

	if (!overlay_relation_has_pk(rel))
	{
		/* TODO: should this be an error or silently pass-through?
		 * For V1 scope we can ERROR on tables without PK. */
		return false;
	}

	return true;
}

void
overlay_modify_insert(Relation rel, TupleTableSlot *slot)
{
	int32		bid = overlay_branch_get_current_id();
	char	   *pk = overlay_serialize_pk(rel, slot);
	bytea	   *tdata = overlay_serialize_tuple(rel, slot);

	overlay_delta_insert(bid, RelationGetRelid(rel), pk,
						 DELTA_OP_INSERT, NULL, tdata);
}

void
overlay_modify_update(Relation rel, TupleTableSlot *oldslot,
					  TupleTableSlot *newslot)
{
	int32		bid = overlay_branch_get_current_id();
	char	   *pk = overlay_serialize_pk(rel, newslot);
	char	   *oldver = overlay_tuple_version(rel, oldslot);
	bytea	   *tdata = overlay_serialize_tuple(rel, newslot);

	overlay_delta_insert(bid, RelationGetRelid(rel), pk,
						 DELTA_OP_UPDATE, oldver, tdata);
}

void
overlay_modify_delete(Relation rel, TupleTableSlot *slot)
{
	int32		bid = overlay_branch_get_current_id();
	char	   *pk = overlay_serialize_pk(rel, slot);
	char	   *oldver = overlay_tuple_version(rel, slot);

	overlay_delta_insert(bid, RelationGetRelid(rel), pk,
						 DELTA_OP_DELETE, oldver, NULL);
}

/* ================================================================
 * ---------- Helper utilities (stub implementations) ----------
 * ================================================================ */

char *
overlay_serialize_pk(Relation rel, TupleTableSlot *slot)
{
	/* TODO: lookup primary key index, extract PK columns, serialize
	 * to a stable string (eg. json or tab-separated text). */
	return pstrdup("TODO-pk-serialization");
}

bytea *
overlay_serialize_tuple(Relation rel, TupleTableSlot *slot)
{
	/* TODO: heap_deform_tuple + datumSend each column into bytea, or
	 * use SendIndirect / record_send style serializer. */
	return NULL;
}

char *
overlay_tuple_version(Relation rel, TupleTableSlot *slot)
{
	/*
	 * For conflict detection we need a stable identifier of the base
	 * tuple version.  Candidates: (xmin, ctid), or (xmin, xmax, cid).
	 * For the skeleton: NULL.
	 */
	return NULL;
}

bool
overlay_relation_has_pk(Relation rel)
{
	/* TODO: check RelationGetIndexList for a primary index. */
	/* Skeleton: be permissive, let the real check come later. */
	return true;
}
