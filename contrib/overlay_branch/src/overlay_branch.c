/*-------------------------------------------------------------------------
 *
 * overlay_branch.c
 *	  Overlay Branch extension: speculative database state using
 *	  table overlay and delta store.
 *
 *	  MAIN MODULE (kept lean after 4-way split):
 *		- Module load callback (_PG_init) with GUC registration
 *		- GUC variables + check-assign hook
 *		- 4-layer recursion-guard flags (apply/guc_setconfig/
 *		  overlay_helper/write_redirect) + their getters/setters
 *		- 4 Executor hooks (Start/Run/Finish/End) + ProcessUtility hook
 *		  (ExecutorRun dispatches to write_redirect.c intercept)
 *		- Step7 HARD GUARD functions (rel_ok / ddl_ok / ereport_fail)
 *		- SQL-callable function thin-dispatch wrappers (all real logic
 *		  lives in branch_lifecycle.c / delta_store.c / write_redirect.c)
 *		- overlay_main_plus_delta SRF (V1 manual Main+Delta view) and
 *		  overlay_branch_list SRF — both kept here because they use
 *		  public helpers only and are simple SQL-callable entry points.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "overlay_branch.h"
#include "branch_scan.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_database.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "executor/spi.h"
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
#include "utils/datetime.h"
#include "utils/datum.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#ifndef BOOTSTRAP_SUPERUSERID
#define BOOTSTRAP_SUPERUSERID	((Oid) 10)
#endif

/* Fully qualified table / schema names */
#define OBTABLE_DELTA   OBSCHEMA ".pg_branch_delta"
#define OBTABLE_BRANCH  OBSCHEMA ".pg_branch"

/* ================================================================
 * Module magic — EXACTLY ONE definition per extension, lives here.
 * ================================================================ */
PG_MODULE_MAGIC;

/* ================================================================
 * GUC variables (session-scoped; owned by GUC framework)
 * ================================================================ */
char	   *overlay_branch_current_name = NULL;
bool		overlay_branch_enabled = true;

/* ================================================================
 * Global session state
 * ================================================================ */
BranchContext *CurrentBranchContext = NULL;

/* ---------- Recursion guard flags (4-layer Bypass stack) ---------- */
bool		ob_in_apply_operation = false;		/* extern: branch_lifecycle.c writes */
bool		ob_in_guc_setconfig = false;		/* extern: branch_lifecycle.c writes */
static bool ob_in_overlay_helper = false;
static bool ob_in_write_redirect = false;

/* --- Layer 1: apply_operation bypass --- */
bool
overlay_in_apply_operation(void)
{
	return ob_in_apply_operation;
}

/* --- Layer 2: shared 2-pass helper bypass --- */
bool
overlay_in_overlay_helper(void)
{
	return ob_in_overlay_helper;
}

void
overlay_overlay_helper_enter(void)
{
	ob_in_overlay_helper = true;
}

void
overlay_overlay_helper_exit(void)
{
	ob_in_overlay_helper = false;
}

/* --- Layer 3: write-redirection bypass --- */
bool
overlay_in_write_redirect(void)
{
	return ob_in_write_redirect;
}

void
overlay_write_redirect_enter(void)
{
	ob_in_write_redirect = true;
}

void
overlay_write_redirect_exit(void)
{
	ob_in_write_redirect = false;
}

/* ================================================================
 * Saved hook values (chaining)
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
static void overlay_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
								   bool readOnlyTree, ProcessUtilityContext context,
								   ParamListInfo params, QueryEnvironment *queryEnv,
								   DestReceiver *dest, QueryCompletion *qc);

/* Step7 HARD GUARD forward declarations */
static bool ob_relid_is_user_table(Oid relid, Relation *outrel, char **relname_out);
static const char *ob_utility_opname(NodeTag tag);

/* ================================================================
 * SQL-callable function declarations
 * ================================================================ */
PG_FUNCTION_INFO_V1(overlay_branch_create);
PG_FUNCTION_INFO_V1(overlay_branch_use);
PG_FUNCTION_INFO_V1(overlay_branch_current);
PG_FUNCTION_INFO_V1(overlay_branch_apply);
PG_FUNCTION_INFO_V1(overlay_branch_discard);
PG_FUNCTION_INFO_V1(overlay_branch_list);
PG_FUNCTION_INFO_V1(overlay_main_plus_delta);
/* debug_delta_* live in delta_store.c; NOT redeclared here to avoid
 * duplicate-PG_FUNCTION_INFO_V1 symbol collisions. */

/* ================================================================
 * Module load callback
 * ================================================================ */
void
_PG_init(void)
{
	MemoryContext oldctx;

	if (CurrentBranchContext == NULL)
	{
		oldctx = MemoryContextSwitchTo(TopMemoryContext);

		CurrentBranchContext = (BranchContext *) palloc0(sizeof(BranchContext));
		CurrentBranchContext->branch_id = 0;
		CurrentBranchContext->branch_name[0] = '\0';
		CurrentBranchContext->is_active = false;
		strcpy(CurrentBranchContext->mode, BRANCH_MODE_LIVE);
		CurrentBranchContext->created_at = 0;

		MemoryContextSwitchTo(oldctx);
	}

	/* GUC: overlay_branch.current (triggers USE via check_hook) */
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

	/* Install executor + utility hooks */
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

	branch_scan_init();
}

/* ================================================================
 * GUC check hook for overlay_branch.current
 * ================================================================ */
static bool
overlay_guc_check_assign_current_branch(char **newval, void **extra,
										GucSource source)
{
	const char *val;

	if (ob_in_guc_setconfig)
	{
		ob_in_guc_setconfig = false;
		return true;
	}

	if (newval == NULL || *newval == NULL)
	{
		if (CurrentBranchContext != NULL)
		{
			CurrentBranchContext->is_active = false;
			CurrentBranchContext->branch_id = 0;
			CurrentBranchContext->branch_name[0] = '\0';
		}
		return true;
	}
	val = *newval;
	if (*val == '\0')
	{
		if (CurrentBranchContext != NULL)
		{
			CurrentBranchContext->is_active = false;
			CurrentBranchContext->branch_id = 0;
			CurrentBranchContext->branch_name[0] = '\0';
		}
		return true;
	}

	overlay_branch_use_internal(val);
	return true;
}

/* ================================================================
 * ======= Step7: HARD GUARD ========
 * ================================================================ */

static const char *ob_guard_hint_tmpl =
	"Switch back to the MAIN database before running this statement:\n"
	"    SELECT overlay_branch.discard_branch('%s');\n"
	"  or\n"
	"    RESET overlay_branch.current;";

void
overlay_guard_ereport_fail(const char *operation, const char *relname,
						   const char *reason)
{
	const char *cur = overlay_branch_get_current_name();
	char		hintbuf[1024];

	snprintf(hintbuf, sizeof(hintbuf), ob_guard_hint_tmpl,
			 (cur && *cur) ? cur : "");
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("overlay_branch: cannot %s%s inside active branch \"%s\": %s",
					operation,
					relname ? " relation " : "",
					(cur && *cur) ? cur : "<unknown>",
					reason ? reason : "operation is not supported in branch mode V1"),
			 relname ? errdetail("Target relation: %s.", relname) : 0,
			 errhint("%s", hintbuf)));
}

bool
overlay_guard_rel_ok(Relation rel, char **reason)
{
	Oid			nspoid;
	char	   *nspname;

	if (reason) *reason = NULL;
	if (rel == NULL) return true;
	if (!overlay_branch_is_active()) return true;

	if (rel->rd_rel->relisshared)
		return true;
	nspoid = RelationGetNamespace(rel);
	nspname = get_namespace_name(nspoid);
	if (nspname == NULL) return true;
	if (strcmp(nspname, "pg_catalog") == 0 ||
		strcmp(nspname, "information_schema") == 0 ||
		strncmp(nspname, "pg_toast", 8) == 0 ||
		strcmp(nspname, OBSCHEMA) == 0)
		return true;

	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION: break;
		case RELKIND_VIEW:
			if (reason) *reason = psprintf("views are not supported (V1 branch mode only accepts plain heap tables with a PRIMARY KEY)");
			return false;
		case RELKIND_MATVIEW:
			if (reason) *reason = psprintf("materialized views are not supported in V1");
			return false;
		case RELKIND_FOREIGN_TABLE:
			if (reason) *reason = psprintf("foreign tables are not supported in V1");
			return false;
		case RELKIND_PARTITIONED_TABLE:
			if (reason) *reason = psprintf("partitioned root tables are not supported in V1");
			return false;
		default:
			if (reason) *reason = psprintf("relkind='%c' is not supported in V1 branch mode", rel->rd_rel->relkind);
			return false;
	}

	if (rel->rd_rel->relispartition)
	{
		if (reason) *reason = psprintf("partition child tables are not supported in V1");
		return false;
	}

	if (rel->trigdesc != NULL && rel->trigdesc->numtriggers > 0)
	{
		int			i;
		for (i = 0; i < rel->trigdesc->numtriggers; i++)
		{
			if (!rel->trigdesc->triggers[i].tgisinternal)
			{
				if (reason) *reason = psprintf("user-defined triggers are not supported in V1 (found trigger \"%s\")",
											   rel->trigdesc->triggers[i].tgname);
				return false;
			}
		}
	}

	if (OidIsValid(RelationGetRelid(rel)))
	{
		int			nfk = 0, nref = 0;
		Oid			relid = RelationGetRelid(rel);
		bool		isnull;
		Datum		v;
		char	   *sql;
		int			ret;

		sql = psprintf("SELECT "
					   "(SELECT count(*) FROM pg_catalog.pg_constraint WHERE conrelid = %u AND contype='f'),"
					   "(SELECT count(*) FROM pg_catalog.pg_constraint WHERE confrelid = %u AND contype='f')",
					   relid, relid);
		ret = SPI_connect();
		if (ret != SPI_OK_CONNECT)
			elog(ERROR, "overlay_guard_rel_ok: SPI_connect failed %d", ret);
		ret = SPI_execute(sql, true, 1);
		pfree(sql);
		if (ret != SPI_OK_SELECT || SPI_processed != 1)
		{
			SPI_finish();
			elog(ERROR, "overlay_guard_rel_ok: SPI FK counts failed %d rows=%lu",
				 ret, SPI_processed);
		}
		v = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
		nfk = isnull ? 0 : DatumGetInt64(v);
		v = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull);
		nref = isnull ? 0 : DatumGetInt64(v);
		SPI_finish();

		if (nfk > 0 || nref > 0)
		{
			if (reason) *reason = psprintf("FOREIGN KEYs are not supported in V1 (this table has %d outgoing and is referenced by %d FK constraints)",
										   nfk, nref);
			return false;
		}
	}

	if (!overlay_relation_has_pk(rel))
	{
		if (reason) *reason = psprintf("table must have a PRIMARY KEY for branch-mode DML (V1 constraint)");
		return false;
	}

	return true;
}

void
overlay_guard_ensure_branch_or_main_active(void)
{
	/* no-op: overlay_branch_is_active() already composes the dual-source
	 * truth-check.  Callers use this for explicit semantic documentation
	 * before entering apply/discard code paths that mutate catalog state. */
}

/* ---------- ob_relid_is_user_table: helper for DDL guard ---------- */
static bool
ob_relid_is_user_table(Oid relid, Relation *outrel, char **relname_out)
{
	Relation	rel;
	Oid			nspoid;
	char	   *nspname;
	bool		result = false;

	if (outrel) *outrel = NULL;
	if (relname_out) *relname_out = NULL;
	if (!OidIsValid(relid))
		return false;

	rel = try_relation_open(relid, AccessShareLock);
	if (rel == NULL)
		return false;
	nspoid = RelationGetNamespace(rel);
	nspname = get_namespace_name(nspoid);
	if (nspname == NULL)
	{
		relation_close(rel, AccessShareLock);
		return false;
	}

	if (strcmp(nspname, "pg_catalog") == 0 ||
		strcmp(nspname, "information_schema") == 0 ||
		strncmp(nspname, "pg_toast", 8) == 0 ||
		strcmp(nspname, OBSCHEMA) == 0 ||
		rel->rd_rel->relisshared)
	{
		relation_close(rel, AccessShareLock);
		return false;
	}

	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_VIEW:
		case RELKIND_MATVIEW:
		case RELKIND_FOREIGN_TABLE:
		case RELKIND_PARTITIONED_TABLE:
		case RELKIND_SEQUENCE:
			result = true;
			break;
		default:
			result = false;
			break;
	}

	if (result)
	{
		if (relname_out)
			*relname_out = psprintf("%s.%s", nspname,
									RelationGetRelationName(rel));
		if (outrel)
			*outrel = rel;
		else
			relation_close(rel, AccessShareLock);
	}
	else
	{
		relation_close(rel, AccessShareLock);
	}
	return result;
}

static const char *
ob_utility_opname(NodeTag tag)
{
	switch (tag)
	{
		case T_AlterTableStmt:	return "ALTER TABLE";
		case T_CreateStmt:		return "CREATE TABLE";
		case T_IndexStmt:		return "CREATE INDEX";
		case T_ReindexStmt:		return "REINDEX";
		case T_TruncateStmt:	return "TRUNCATE";
		case T_DropStmt:		return "DROP";
		case T_VacuumStmt:		return "VACUUM/ANALYZE";
		case T_ClusterStmt:		return "CLUSTER";
		case T_RenameStmt:		return "RENAME";
		case T_RuleStmt:		return "CREATE RULE";
		case T_CreatePolicyStmt:return "CREATE POLICY";
		case T_AlterPolicyStmt:	return "ALTER POLICY";
		default:				return "utility/DDL";
	}
}

/* ---------- overlay_guard_ddl_ok_for_branch ---------- */
bool
overlay_guard_ddl_ok_for_branch(Node *parsetree, char **operation,
								 char **objname, char **reason)
{
	if (operation) *operation = NULL;
	if (objname) *objname = NULL;
	if (reason) *reason = NULL;
	if (!overlay_branch_is_active()) return true;
	if (parsetree == NULL) return true;

	switch (nodeTag(parsetree))
	{
		case T_AlterTableStmt:
		case T_CreateStmt:
		{
			if (operation) *operation = pstrdup(ob_utility_opname(nodeTag(parsetree)));
			if (reason) *reason = psprintf("%s inside a branch is not supported in V1 (would modify MAIN's catalog schemas in place)",
										  operation ? *operation : "DDL");
			return false;
		}

		case T_DropStmt:
		{
			DropStmt *d = (DropStmt *) parsetree;
			if (operation) *operation = pstrdup("DROP");
			{
				ListCell *lc;
				foreach(lc, d->objects)
				{
					Node *n = (Node *) lfirst(lc);
					if (IsA(n, List))
					{
						List     *names = (List *) n;
						RangeVar *rv = makeRangeVarFromNameList(names);
						Oid       rid;
						char     *rn = NULL;
						rid = RangeVarGetRelid(rv, AccessShareLock, true);
						if (OidIsValid(rid) && ob_relid_is_user_table(rid, NULL, &rn))
						{
							if (objname) *objname = rn;
							if (reason) *reason = psprintf("DROP ... of MAIN user objects inside a branch is not supported in V1");
							return false;
						}
						switch (d->removeType)
						{
							case OBJECT_TABLE: case OBJECT_VIEW: case OBJECT_MATVIEW:
							case OBJECT_SEQUENCE: case OBJECT_FOREIGN_TABLE: case OBJECT_COLUMN:
							case OBJECT_POLICY: case OBJECT_TRIGGER: case OBJECT_RULE:
							case OBJECT_TABCONSTRAINT: case OBJECT_DOMCONSTRAINT:
								if (reason && !*reason) *reason = psprintf("DROP of this object class inside a branch is not supported in V1 (would affect MAIN)");
								return false;
							default:
								break;
						}
					}
				}
			}
			return true;
		}

		case T_TruncateStmt:
		{
			TruncateStmt *t = (TruncateStmt *) parsetree;
			ListCell   *lc;
			if (operation) *operation = pstrdup("TRUNCATE");
			foreach(lc, t->relations)
			{
				RangeVar   *rv = (RangeVar *) lfirst(lc);
				Oid			rid = RangeVarGetRelid(rv, AccessShareLock, true);
				char	   *rn = NULL;
				if (OidIsValid(rid) && ob_relid_is_user_table(rid, NULL, &rn))
				{
					if (objname) *objname = rn;
					if (reason) *reason = psprintf("TRUNCATE inside a branch is not supported in V1 (would immediately empty the MAIN table)");
					return false;
				}
			}
			return true;
		}

		case T_IndexStmt:
		{
			IndexStmt  *i = (IndexStmt *) parsetree;
			Oid			rid = RangeVarGetRelid(i->relation, AccessShareLock, true);
			char	   *rn = NULL;
			if (operation) *operation = pstrdup("CREATE INDEX");
			if (OidIsValid(rid) && ob_relid_is_user_table(rid, NULL, &rn))
			{
				if (objname) *objname = rn;
				if (reason) *reason = psprintf("CREATE INDEX inside a branch is not supported in V1 (would write to MAIN's pg_index/pg_class catalogs)");
				return false;
			}
			return true;
		}

		case T_ReindexStmt:
			if (operation) *operation = pstrdup("REINDEX");
			if (reason) *reason = psprintf("REINDEX inside a branch is not supported in V1 (modifies MAIN indexes in place)");
			return false;
		case T_ClusterStmt:
			if (operation) *operation = pstrdup("CLUSTER");
			if (reason) *reason = psprintf("CLUSTER inside a branch is not supported in V1 (rewrites MAIN heap in place)");
			return false;
		case T_VacuumStmt:
			if (operation) *operation = pstrdup("VACUUM/ANALYZE");
			if (reason) *reason = psprintf("VACUUM/ANALYZE inside a branch is not supported in V1 (modifies MAIN visibility map and statistics)");
			return false;
		case T_RenameStmt:
			if (operation) *operation = pstrdup("RENAME");
			if (reason) *reason = psprintf("RENAME inside a branch is not supported in V1 (modifies MAIN catalog names in place)");
			return false;
		case T_RuleStmt:
			if (operation) *operation = pstrdup("CREATE RULE");
			if (reason) *reason = psprintf("CREATE RULE inside a branch is not supported in V1 (modifies MAIN pg_rules)");
			return false;
		case T_CreatePolicyStmt:
		case T_AlterPolicyStmt:
			if (operation) *operation = pstrdup("(ALTER) POLICY");
			if (reason) *reason = psprintf("CREATE/ALTER POLICY inside a branch is not supported in V1 (modifies MAIN row-level security in place)");
			return false;

		default:
			return true;
	}
}

/* ================================================================
 * --- Executor + ProcessUtility hooks ---
 * ================================================================ */
static void
overlay_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

static void
overlay_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
					uint64 count, bool execute_once)
{
	/* Dispatch: if the intercept handled it (true return → handled +
	 * write-redirect exit already called internally), return
	 * immediately.  Otherwise fall through to chained / standard. */
	if (overlay_executor_run_intercept(queryDesc, direction, count,
									   execute_once, prev_ExecutorRun))
		return;

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
	char	   *op_name = NULL;
	char	   *obj_name = NULL;
	char	   *fail_reason = NULL;

	if (overlay_branch_is_active() && pstmt != NULL &&
		pstmt->utilityStmt != NULL &&
		!overlay_guard_ddl_ok_for_branch(pstmt->utilityStmt,
										  &op_name, &obj_name,
										  &fail_reason))
	{
		overlay_guard_ereport_fail(op_name ? op_name : "execute utility/DDL",
								   obj_name, fail_reason);
	}

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
}

/* ================================================================
 * SQL-callable thin-dispatch wrappers (real impl → branch_lifecycle.c)
 * ================================================================ */

Datum
overlay_branch_create(PG_FUNCTION_ARGS)
{
	Name		branch_name = PG_GETARG_NAME(0);
	int32		new_branch_id;

	new_branch_id = overlay_branch_create_internal(NameStr(*branch_name));
	PG_RETURN_INT32(new_branch_id);
}

Datum
overlay_branch_use(PG_FUNCTION_ARGS)
{
	Name		branch_name = PG_GETARG_NAME(0);

	overlay_branch_use_internal(NameStr(*branch_name));
	PG_RETURN_VOID();
}

Datum
overlay_branch_current(PG_FUNCTION_ARGS)
{
	const char *name;
	NameData   *result;

	if (overlay_branch_current_name != NULL &&
		*overlay_branch_current_name != '\0')
	{
		name = overlay_branch_current_name;
	}
	else if (CurrentBranchContext != NULL &&
			 CurrentBranchContext->is_active &&
			 CurrentBranchContext->branch_name[0] != '\0')
	{
		name = CurrentBranchContext->branch_name;
	}
	else
	{
		name = NULL;
	}

	if (name == NULL)
		PG_RETURN_NULL();

	result = (NameData *) palloc0(sizeof(NameData));
	namestrcpy(result, name);
	PG_RETURN_NAME(result);
}

Datum
overlay_branch_apply(PG_FUNCTION_ARGS)
{
	Name		branch_name = PG_GETARG_NAME(0);

	overlay_branch_apply_internal(NameStr(*branch_name));
	PG_RETURN_VOID();
}

Datum
overlay_branch_discard(PG_FUNCTION_ARGS)
{
	Name		branch_name = PG_GETARG_NAME(0);

	overlay_branch_discard_internal(NameStr(*branch_name));
	PG_RETURN_VOID();
}

/* ================================================================
 * overlay_branch_list() SRF
 * ================================================================ */
Datum
overlay_branch_list(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	int			call_cntr;
	int			max_calls;
	struct ob_list_state
	{
		int			nrows;
		Datum	   *values;
		bool	   *nulls;
	};
	struct ob_list_state *st;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;
		int			ret, i;
		TypeFuncClass tfc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		tfc = get_call_result_type(fcinfo, NULL, &tupdesc);
		if (tfc != TYPEFUNC_COMPOSITE || tupdesc == NULL ||
			tupdesc->natts != 7)
		{
			elog(ERROR, "overlay_branch: list_branches() must be called "
				 "with an explicit 7-column composite return type "
				 "(RETURNS TABLE); got tfc=%d nattrs=%d",
				 (int) tfc,
				 tupdesc ? (int) tupdesc->natts : -1);
		}
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		st = (struct ob_list_state *) palloc0(sizeof(struct ob_list_state));
		funcctx->user_fctx = (void *) st;

		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "overlay_branch: SPI_connect failed in list_branches");

		ret = SPI_execute("SELECT b.branch_id, b.branch_name, b.owner, "
						  "       b.created_at, b.mode, b.state, "
						  "       COALESCE(d.cnt, 0)::bigint AS delta_count "
						  "FROM " OBTABLE_BRANCH " b "
						  "LEFT JOIN (SELECT branch_id, count(*) AS cnt "
						  "           FROM " OBTABLE_DELTA " "
						  "           GROUP BY branch_id) d "
						  "  ON d.branch_id = b.branch_id "
						  "ORDER BY b.branch_id",
						  true, 0);
		if (ret != SPI_OK_SELECT)
		{
			SPI_finish();
			elog(ERROR, "overlay_branch: list_branches SELECT failed ret=%d", ret);
		}

		st->nrows = SPI_processed;
		if (st->nrows > 0)
		{
			TupleDesc	spi_td = SPI_tuptable->tupdesc;

			if (SPI_tuptable->vals == NULL)
			{
				SPI_finish();
				elog(ERROR, "overlay_branch: list_branches got vals=NULL despite nrows=%d",
					 (int) st->nrows);
			}

			st->values = (Datum *) palloc(sizeof(Datum) * 7 * st->nrows);
			st->nulls = (bool *) palloc(sizeof(bool) * 7 * st->nrows);
			for (i = 0; i < st->nrows; i++)
			{
				HeapTuple	tup = SPI_tuptable->vals[i];
				int			col;

				if (tup == NULL)
				{
					for (col = 0; col < 7; col++)
					{
						st->values[i * 7 + col] = (Datum) 0;
						st->nulls[i * 7 + col] = true;
					}
					continue;
				}

				for (col = 0; col < 7; col++)
				{
					Datum		val;
					bool		isnull;
					Form_pg_attribute att;

					val = SPI_getbinval(tup, spi_td, col + 1, &isnull);
					st->nulls[i * 7 + col] = isnull;
					if (isnull)
					{
						st->values[i * 7 + col] = (Datum) 0;
						continue;
					}
					att = TupleDescAttr(spi_td, col + 1);
					if (att == NULL)
					{
						st->values[i * 7 + col] = (Datum) 0;
						st->nulls[i * 7 + col] = true;
						continue;
					}
					st->values[i * 7 + col] = datumCopy(val,
													  att->attbyval,
													  att->attlen);
				}
			}
		}

		SPI_finish();
		funcctx->max_calls = st->nrows;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;
	st = (struct ob_list_state *) funcctx->user_fctx;

	if (call_cntr < max_calls)
	{
		Datum		values[7];
		bool		nulls[7];
		HeapTuple	tuple;
		int			col;

		for (col = 0; col < 7; col++)
		{
			values[col] = st->values[call_cntr * 7 + col];
			nulls[col] = st->nulls[call_cntr * 7 + col];
		}
		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	else
	{
		SRF_RETURN_DONE(funcctx);
	}
}

/* ================================================================
 * overlay_main_plus_delta SRF — V1 manual Main+Delta view
 * ================================================================ */
Datum
overlay_main_plus_delta(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext	oldcxt;
		Oid				relid;
		Relation		rel;
		TupleDesc		reldesc;
		ReturnSetInfo  *rsinfo;
		TupleDesc		exp_desc;
		int32			branch_id;
		List		   *result_slots;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcxt = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (PG_ARGISNULL(0))
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("overlay_main_plus_delta: table must not be NULL")));
		relid = PG_GETARG_OID(0);
		rel = table_open(relid, AccessShareLock);
		reldesc = RelationGetDescr(rel);
		(void) reldesc;

		rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
		if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo) ||
			rsinfo->expectedDesc == NULL || rsinfo->expectedDesc->natts == 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("overlay_main_plus_delta: must be called with explicit column list"),
					 errhint("Use: FROM overlay_branch.overlay_main_plus_delta('t') AS x(id int, v text)")));
		exp_desc = rsinfo->expectedDesc;

		if (!overlay_branch_is_active())
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("overlay_main_plus_delta: no active branch"),
					 errhint("Call use_branch('name') first.")));
		if (!overlay_relation_has_pk(rel))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("overlay_main_plus_delta requires a primary key on %s",
							RelationGetRelationName(rel))));

		branch_id = overlay_branch_get_current_id();

		overlay_overlay_helper_enter();
		PG_TRY();
		{
			result_slots = ob_compute_overlay_slots(relid, branch_id);
		}
		PG_CATCH();
		{
			overlay_overlay_helper_exit();
			PG_RE_THROW();
		}
		PG_END_TRY();
		overlay_overlay_helper_exit();

		funcctx->user_fctx = result_slots;
		funcctx->max_calls = list_length(result_slots);
		table_close(rel, AccessShareLock);
		(void) exp_desc;

		MemoryContextSwitchTo(oldcxt);
	}

	funcctx = SRF_PERCALL_SETUP();
	if (funcctx->call_cntr < funcctx->max_calls)
	{
		List		   *result_slots = (List *) funcctx->user_fctx;
		TupleTableSlot *src_slot = (TupleTableSlot *) list_nth(result_slots,
															  funcctx->call_cntr);
		ReturnSetInfo  *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
		TupleDesc		exp_desc = rsinfo->expectedDesc;
		TupleDesc		src_desc = src_slot->tts_tupleDescriptor;
		Datum		   *values;
		bool		   *isnulls;
		int				exp_natts = exp_desc->natts;
		MemoryContext	oldcxt;
		HeapTuple		tuple;
		Datum			result;

		oldcxt = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		values = (Datum *) palloc0(sizeof(Datum) * exp_natts);
		isnulls = (bool *) palloc0(sizeof(bool) * exp_natts);

		for (int e = 0; e < exp_natts; e++)
		{
			Form_pg_attribute eatt = TupleDescAttr(exp_desc, e);
			const char	   *ename = NameStr(eatt->attname);
			bool			found = false;

			for (int s = 0; s < src_desc->natts; s++)
			{
				Form_pg_attribute satt = TupleDescAttr(src_desc, s);
				if (satt->attisdropped)
					continue;
				if (strcmp(NameStr(satt->attname), ename) == 0)
				{
					bool	sisnull;
					Datum	sd = slot_getattr(src_slot,
											  (AttrNumber) (s + 1),
											  &sisnull);

					if (!sisnull)
					{
						int16	typlen;
						bool	typbyval;
						get_typlenbyval(satt->atttypid, &typlen, &typbyval);
						sd = datumCopy(sd, typbyval, typlen);
					}
					values[e] = sd;
					isnulls[e] = sisnull;
					found = true;
					break;
				}
			}
			if (!found)
			{
				values[e] = (Datum) 0;
				isnulls[e] = true;
			}
		}

		tuple = heap_form_tuple(exp_desc, values, isnulls);
		result = HeapTupleGetDatum(tuple);
		MemoryContextSwitchTo(oldcxt);

		SRF_RETURN_NEXT(funcctx, result);
	}
	else
	{
		SRF_RETURN_DONE(funcctx);
	}
}
