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

/* Fully qualified table / schema names (control file pins schema = 'overlay_branch') */
/* OBSCHEMA defined in overlay_branch.h public header */
#define OBTABLE_DELTA   OBSCHEMA ".pg_branch_delta"
#define OBTABLE_BRANCH  OBSCHEMA ".pg_branch"

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
static bool ob_in_apply_operation = false;
static bool ob_in_guc_setconfig = false;

/* ---------- Recursion guard flags (4-layer Bypass stack; see branch_scan.c Planner hook) ---------- */
static bool ob_in_overlay_helper = false;
static bool ob_in_write_redirect = false;

/* --- Layer 1: apply_operation (write replay into MAIN) bypass --- */
bool
overlay_in_apply_operation(void)
{
	return ob_in_apply_operation;
}

/* --- Layer 2: shared 2-pass helper (overlay_main_plus_delta / BranchScan LazyMaterialize) --- */
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

/* --- Layer 3: Step 4a write-redirection ExecutorRun hook ---
 *      Internal SPI CMD_SELECT queries (WHERE-row lookup) must scan
 *      raw MAIN heap (SeqScan/IndexScan), NEVER the overlay CustomScan —
 *      otherwise write-redirect code hard-casts SeqScanState offsets = SIGSEGV. */
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
static void overlay_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
								   bool readOnlyTree, ProcessUtilityContext context,
								   ParamListInfo params, QueryEnvironment *queryEnv,
								   DestReceiver *dest, QueryCompletion *qc);
static inline ResultRelInfo *mt_state_result_rel(ModifyTableState *mt, int i);

/* ---------- Step7 HARD GUARD forward declarations ---------- */
static bool ob_relid_is_user_table(Oid relid, Relation *outrel, char **relname_out);
static const char *ob_utility_opname(NodeTag tag);

/* ---------- SPI one-shot helper (used by Step 2/3/4 internal SPI paths) ---------- */
int	ob_spi_one_shot(const char *sql, bool read_only, uint64 tcount);

/* ---------- Slot / junk-attribute helpers (Step 4 UPDATE/DELETE redirect) ---------- */
static inline AttrNumber rel_attno_to_slot_idx(TupleDesc slotdesc, AttrNumber rel_attno);
static char *slot_get_ctid_cstr(TupleTableSlot *slot);
static TupleTableSlot *fetch_tuple_by_ctid(Relation rel, const char *ctid_cstr);
TupleTableSlot *reconstruct_slot_from_delta(Relation rel, bytea *tuple_data);

/* ---------- Step5 apply per-op per-pass helpers ---------- */
static void apply_relation_delete_pass(Relation rel, DeltaTuple *dt);
static void apply_relation_update_pass(Relation rel, DeltaTuple *dt);
static void apply_relation_insert_pass(Relation rel, DeltaTuple *dt);

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
PG_FUNCTION_INFO_V1(overlay_debug_delta_insert);
PG_FUNCTION_INFO_V1(overlay_debug_delta_count);
PG_FUNCTION_INFO_V1(overlay_debug_delta_delete_all);

/* ================================================================
 * Module load callback
 * ================================================================ */
void
_PG_init(void)
{
	MemoryContext oldctx;

	/*
	 * Allocate the per-session branch context exactly once in
	 * TopMemoryContext so it survives across transactions (but not
	 * across sessions).  If someone loaded us twice in the same backend
	 * (e.g. through both shared_preload_libraries and LOAD), keep the
	 * first allocation.
	 */
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

	branch_scan_init();
}

/* ================================================================
 * GUC check hook for overlay_branch.current
 * SET overlay_branch.current = 'b1'  =>  equivalent to USE BRANCH b1
 * ================================================================ */
static bool overlay_guc_check_assign_current_branch(char **newval, void **extra,
										GucSource source)
{
	const char *val;

	/* Anti-recursion guard: when use_internal calls SetConfigOption() to
	 * push the current-branch name into the GUC framework, it raises this
	 * flag on entry.  On entry we clear the flag and immediately accept the
	 * incoming value without re-invoking use_internal (which would loop). */
	if (ob_in_guc_setconfig)
	{
		ob_in_guc_setconfig = false;
		return true;
	}

	if (newval == NULL || *newval == NULL)
	{
		/* RESET overlay_branch.current → revert to Main */
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
		/* SET overlay_branch.current = '' → revert to Main */
		if (CurrentBranchContext != NULL)
		{
			CurrentBranchContext->is_active = false;
			CurrentBranchContext->branch_id = 0;
			CurrentBranchContext->branch_name[0] = '\0';
		}
		return true;
	}

	/*
	 * Non-empty name: the user is trying to SET overlay_branch.current to a
	 * branch.  We must validate it: the branch must exist and be active.
	 * use_internal will ereport(ERROR) if not, which automatically aborts
	 * the GUC SET.  If use_internal returns, the name is valid: accept the
	 * new value by returning true.
	 *
	 * NOTE: calling use_internal here is safe (no side-effect double-apply)
	 * because use_internal simply re-enters the branch with the same name —
	 * re-entering is idempotent.
	 */
	overlay_branch_use_internal(val);
	return true;
}

/* ================================================================
 * ======= Step7: HARD GUARD (prevent silent pass-through) ========
 * ================================================================
 *
 * Goals:
 *   • Never let DDL / TRUNCATE / VACUUM / CLUSTER silently mutate
 *     the MAIN database while a branch is active.
 *   • Never let DML run against views / matviews / foreign tables /
 *     partitioned (root or child) / tables with triggers / FKs / or
 *     tables without a PK inside a branch mode — DML against these
 *     either silently goes to Main or corrupts delta semantics.
 *
 * Implementation points:
 *   [A] overlay_ProcessUtility hook → call overlay_guard_ddl_ok_for_branch
 *   [B] overlay_ExecutorRun hook (before redirect decisions) → call
 *       overlay_guard_rel_ok per target relation
 */

/* Unified error/hint primitive */
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

/* ----------------------------------------------------------------
 * overlay_guard_rel_ok — per-DML (ExecutorRun) eligibility check.
 * Returns true if the relation is allowed for DML in a branch.
 * On failure, fills *reason with a palloc'd cstring.
 *
 * Order (cheapest → most expensive):
 *   G0. Skip entirely if !overlay_branch_is_active()
 *   G1. Skip entirely for system / overlay_branch catalog schemas
 *   G2. relkind must be RELKIND_RELATION (plain heap)
 *   G3. rd_rel->relispartition must be false
 *   G4. No user-defined (non-internal) triggers
 *   G5. No FKs (rd_att->constr num_fk/num_ref > 0)
 *   G6. Must have a PRIMARY KEY
 * ----------------------------------------------------------------
 */
bool
overlay_guard_rel_ok(Relation rel, char **reason)
{
	Oid			nspoid;
	char	   *nspname;

	if (reason) *reason = NULL;
	if (rel == NULL) return true;
	if (!overlay_branch_is_active()) return true;

	/* G0. catalog / extension / shared relations: safe */
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

	/* G2. relkind check */
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

	/* G3. partition child */
	if (rel->rd_rel->relispartition)
	{
		if (reason) *reason = psprintf("partition child tables are not supported in V1");
		return false;
	}

	/* G4. user triggers */
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

	/* G4. foreign keys (any side).  PG17 does NOT store FK counts inside
	 * rd_att->TupleConstr (that only holds defaults/checks/generated).  The
	 * authoritative runtime source for FK enforcement triggers is the per-tuple
	 * TriggerDesc — any non-internal RI trigger means FK enforcement is
	 * active.  Additionally we check the ResultRelInfo ri_ConstraintExprs /
	 * ri_FKeyConstraint* but those are populated lazily, so TriggerDesc scan is
	 * sufficient.  Because user-triggers G3 already counted, here we do an
	 * explicit SPI lookup on pg_constraint for FK counts, which is cheap per-plan
	 * (not per-tuple) and safe. */
	if (OidIsValid(RelationGetRelid(rel)))
	{
		int			nfk = 0, nref = 0;
		Oid			relid = RelationGetRelid(rel);
		bool		isnull;
		Datum		v;
		char	   *sql;
		int			ret;

		/* outgoing FKs: conrelid = relid AND contype = 'f' */
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

	/* G6. PRIMARY KEY */
	if (!overlay_relation_has_pk(rel))
	{
		if (reason) *reason = psprintf("table has no PRIMARY KEY; V1 branch mode for DML requires a PRIMARY KEY on every target");
		return false;
	}

	return true;
}

/* ---------- ProcessUtility (DDL/TRUNCATE/VACUUM) guard helpers ---------- */

/* Returns true if relid resolves to a user (non-catalog, non-extension)
 * relation.  When true: outrel is returned opened (AccessShareLock) unless
 * outrel==NULL, and *relname_out is set to a pstrdup'd name. */
static bool
ob_relid_is_user_table(Oid relid, Relation *outrel, char **relname_out)
{
	Relation	r;
	Oid			nspoid;
	char	   *nspname;

	if (relname_out) *relname_out = NULL;
	if (outrel) *outrel = NULL;
	if (!OidIsValid(relid)) return false;

	r = relation_open(relid, AccessShareLock);
	if (r == NULL) return false;

	nspoid = RelationGetNamespace(r);
	nspname = get_namespace_name(nspoid);
	if (nspname &&
		(strcmp(nspname, "pg_catalog") == 0 ||
		 strcmp(nspname, "information_schema") == 0 ||
		 strncmp(nspname, "pg_toast", 8) == 0 ||
		 strcmp(nspname, OBSCHEMA) == 0))
	{
		relation_close(r, AccessShareLock);
		return false;
	}
	if (outrel) *outrel = r;
	else relation_close(r, AccessShareLock);
	if (relname_out) *relname_out = pstrdup(RelationGetRelationName(r));
	return true;
}

static const char *
ob_utility_opname(NodeTag tag)
{
	switch (tag)
	{
		case T_CreateStmt:			return "CREATE TABLE";
		case T_CreateForeignTableStmt: return "CREATE FOREIGN TABLE";
		case T_RefreshMatViewStmt:	return "REFRESH MATERIALIZED VIEW";
		case T_CreateTableAsStmt:	return "CREATE TABLE AS / SELECT INTO";
		case T_CreateSeqStmt:		return "CREATE SEQUENCE";
		case T_AlterSeqStmt:		return "ALTER SEQUENCE";
		case T_AlterTableStmt:		return "ALTER TABLE";
		case T_DropStmt:			return "DROP";
		case T_TruncateStmt:		return "TRUNCATE";
		case T_IndexStmt:			return "CREATE INDEX";
		case T_ReindexStmt:			return "REINDEX";
		case T_ClusterStmt:			return "CLUSTER";
		case T_VacuumStmt:			return "VACUUM/ANALYZE";
		case T_RenameStmt:			return "RENAME";
		case T_RuleStmt:			return "CREATE RULE";
		case T_CreatePolicyStmt:	return "CREATE POLICY";
		case T_AlterPolicyStmt:		return "ALTER POLICY";
		default:					return "execute utility/DDL";
	}
}

bool
overlay_guard_ddl_ok_for_branch(Node *parsetree, char **operation,
								 char **objname, char **reason)
{
	if (operation) *operation = NULL;
	if (objname)   *objname = NULL;
	if (reason)    *reason = NULL;

	if (!parsetree || !overlay_branch_is_active()) return true;

	switch (nodeTag(parsetree))
	{
			/* ------ (1) Unambiguously-safe catalog / branch ops ------ */
		case T_CreateExtensionStmt:
		case T_AlterExtensionStmt:
		case T_CreateSchemaStmt:
		case T_CommentStmt:
		case T_TransactionStmt:
		case T_NotifyStmt:
		case T_ListenStmt:
		case T_UnlistenStmt:
		case T_DeclareCursorStmt:
		case T_ClosePortalStmt:
		case T_FetchStmt:
		case T_LoadStmt:
		case T_SetOperationStmt:
		case T_VariableSetStmt:
		case T_VariableShowStmt:
		case T_CreateFunctionStmt:
		case T_DefineStmt:
		case T_CreateCastStmt:
		case T_CreateTransformStmt:
		case T_CreateDomainStmt:
		case T_CreateEnumStmt:
		case T_CreateRangeStmt:
		case T_CreateEventTrigStmt:
		case T_CreateFdwStmt:
		case T_CreateForeignServerStmt:
		case T_CreateUserMappingStmt:
		case T_CreateOpClassStmt:
		case T_CreateOpFamilyStmt:
		case T_CreateAmStmt:
		case T_CreatePLangStmt:
			return true;

			/* ------ (2) CREATE user relations / heap objects: block.
			 *       These create entries in MAIN's pg_class/pg_type/pg_index
			 *       that cannot be isolated inside a branch.
			 *       For CREATE TABLE AS / SELECT INTO we additionally allow
			 *       matviews (which the parser routes as CreateTableAsStmt
			 *       with objtype=OBJECT_MATVIEW). ------ */
		case T_CreateStmt:
		case T_CreateForeignTableStmt:
		case T_CreateSeqStmt:
			if (operation) *operation = pstrdup(ob_utility_opname(nodeTag(parsetree)));
			if (reason) *reason = psprintf("creating new user heap/sequence objects inside a branch is not supported in V1; they would be visible on MAIN");
			return false;
		case T_CreateTableAsStmt:
			{
				CreateTableAsStmt *ctas = (CreateTableAsStmt *) parsetree;
				if (operation)
				{
					if (ctas->objtype == OBJECT_MATVIEW)
						*operation = pstrdup("CREATE MATERIALIZED VIEW");
					else if (ctas->is_select_into)
						*operation = pstrdup("SELECT INTO");
					else
						*operation = pstrdup("CREATE TABLE AS");
				}
				if (reason) *reason = psprintf("creating new user heap/matview objects inside a branch is not supported in V1; they would be visible on MAIN");
				return false;
			}
		case T_RefreshMatViewStmt:
			if (operation) *operation = pstrdup("REFRESH MATERIALIZED VIEW");
			if (reason) *reason = psprintf("REFRESH MATERIALIZED VIEW inside a branch is not supported in V1 (rewrites MAIN heap in place)");
			return false;

			/* ------ (3) ALTER / DROP / TRUNCATE / INDEX ------ */
		case T_AlterTableStmt:
			{
				AlterTableStmt *a = (AlterTableStmt *) parsetree;
				Oid			rid = RangeVarGetRelid(a->relation, AccessShareLock, true);
				char	   *rn = NULL;
				if (operation) *operation = pstrdup("ALTER TABLE");
				if (OidIsValid(rid) && ob_relid_is_user_table(rid, NULL, &rn))
				{
					if (objname) *objname = rn;
					if (reason) *reason = psprintf("ALTER TABLE on user tables would modify MAIN's column/constraint structure; not supported in branch mode V1");
					return false;
				}
				return true;
			}
		case T_AlterSeqStmt:
			if (operation) *operation = pstrdup("ALTER SEQUENCE");
			if (reason) *reason = psprintf("ALTER SEQUENCE inside a branch is not supported in V1 (sequences modify MAIN state in place)");
			return false;

		case T_DropStmt:
			{
				DropStmt   *d = (DropStmt *) parsetree;
				if (operation) *operation = pstrdup("DROP");
				switch (d->removeType)
				{
					case OBJECT_TABLE:
					case OBJECT_INDEX:
					case OBJECT_VIEW:
					case OBJECT_SEQUENCE:
					case OBJECT_MATVIEW:
					case OBJECT_FOREIGN_TABLE:
					case OBJECT_COLUMN:
					case OBJECT_POLICY:
					case OBJECT_TRIGGER:
					case OBJECT_RULE:
					case OBJECT_STATISTIC_EXT:
					case OBJECT_TABCONSTRAINT:
					case OBJECT_DOMCONSTRAINT:
					case OBJECT_FUNCTION:
					case OBJECT_TYPE:
					case OBJECT_DOMAIN:
					case OBJECT_SCHEMA:
						{
							ListCell   *lc;
							foreach(lc, d->objects)
							{
								Node *n = (Node *) lfirst(lc);
								if (IsA(n, List))
								{
									List     *names = (List *) n;
									RangeVar *rv = makeRangeVarFromNameList(names);
									Oid       rid;
									char     *rn = NULL;
									/* RangeVarGetRelid only works for
									 * relkinds that have a pg_class
									 * entry; for FUNCTION/TYPE/etc. we
									 * may get InvalidOid which is fine;
									 * we still block at the broad level. */
									rid = RangeVarGetRelid(rv, AccessShareLock, true);
									if (OidIsValid(rid) && ob_relid_is_user_table(rid, NULL, &rn))
									{
										if (objname) *objname = rn;
										if (reason) *reason = psprintf("DROP ... of MAIN user objects inside a branch is not supported in V1");
										return false;
									}
									/* Even if relid not found, still block on relkinds that modify MAIN */
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
					default:
						/* CAST / LANGUAGE / ROLE / …: we permissively
						 * let through.  V1 only guards relation-level. */
						return true;
				}
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

			/* ------ (4) Unknown/unclassified: assume harmless ------ */
		default:
			return true;
	}
}

/* ================================================================
 * --- Executor + ProcessUtility hooks (guards plugged in above) ---
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
	CmdType		cmd = queryDesc->operation;

	/* Only intercept active-branch + ModifyTable (real DML). */
	if (overlay_branch_is_active() &&
		queryDesc->planstate != NULL &&
		IsA(queryDesc->planstate, ModifyTableState))
	{
		/* C90: declarations BEFORE any executable statement. */
		ModifyTableState *mt = NULL;
		ModifyTable      *plan = NULL;
		overlay_write_redirect_enter();
		PG_TRY();
		mt = (ModifyTableState *) queryDesc->planstate;
		plan = (ModifyTable *) mt->ps.plan;

		/* ---- Step7 DML hard guard.
		 *
		 * Strategy: guard EVERY result relation in the ModifyTable node,
		 * not only the direct target.  The parser rewrites writes to
		 * views (with DO INSTEAD rules) and auto-updatable views into a
		 * ModifyTable whose resultRelInfo contains the underlying base
		 * table (relkind='r'), so checking ONLY resultRelInfo[0] would
		 * let writes-to-views silently pass through to MAIN — a data
		 * integrity violation.
		 *
		 * In PG17, an auto-updatable-view INSERT is also typically seen
		 * as a ModifyTable with only 1 resultRelInfo (the base table,
		 * because the view is inlined before planning).  We therefore
		 * also check the top-level EState's range table for any RTE
		 * that represents a relation with an invalid relkind: views
		 * and matviews (relkind 'v' / 'm') are forbidden targets in a
		 * branch.  This catches INSERT INTO v, UPDATE v, DELETE FROM v
		 * even when the view has been inlined to base-tables.
		 *
		 * This loop runs once per plan, which is O(mt_nrels + #rtable)
		 * — tiny in practice. --- */
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
				/* noreturn if failed */
			}

			/* Now also scan the executor range table for any VIEW/
			 * MATVIEW/FOREIGN/PARTITIONED rel — these indicate the user
			 * wrote DML against a non-'r' relkind which MUST be
			 * blocked even if it was subsequently rewritten to target
			 * a legal base-table. */
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

						/* For branch guard we only care if the relation
						 * itself has a "bad" relkind.  Catalog tables
						 * (pg_catalog.*, overlay_branch.*) are already
						 * skipped in guard_rel_ok G0 and must not trigger
						 * false positives here.  Only block if guard_rel_ok
						 * reports the user wrote DML against a bad relkind. */
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


		if (mt->mt_nrels == 1 && list_length(plan->resultRelations) == 1)
		{
			ResultRelInfo *rri = mt_state_result_rel(mt, 0);
			Relation	rel = rri->ri_RelationDesc;
			const char *opstr = cmd == CMD_INSERT ? "INSERT" :
								cmd == CMD_UPDATE ? "UPDATE" :
								cmd == CMD_DELETE ? "DELETE" : "DML";
			(void) opstr;
			/* noreturn if failed (already guarded above): keep for readability */

			/* --- CMD_UPDATE / CMD_DELETE: redirect, do NOT write main table.
			 *   - DELETE: user columns are completely UNOCCUPIED (only ctid
			 *     is there to locate the physical heap row to delete).
			 *   - UPDATE: user columns in the slot are the post-SET VALUES,
			 *     laid out after junk cols, so we can copy by matching
			 *     column NAME, not by position.
			 *
			 * Strategy:
			 *   CMD_DELETE — extract ctid, SPI-select the REAL row from
			 *     the physical relation by ctid (no Executor hook recursion
			 *     because we SELECT not DML), get a clean relation-only
			 *     slot, serialize_pk to obtain the stable key, then write
			 *     a tombstone delta row (op=D, tuple_data=NULL).
			 *   CMD_UPDATE — match user columns by NAME between junk slot
			 *     and Relation tupdesc → produce a clean relation-only
			 *     slot (now i+1 indexing is correct), then serialize_pk +
			 *     serialize_tuple to capture post-SET new row image. Write
			 *     op=U, old_version="" (Apply-stage improvement).
			 * --- */
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

					/* Step 5: capture the pre-write "version token" from the
					 * physical MAIN row so apply_branch can later compare
					 * versions and refuse to apply on top of a concurrently
					 * changed MAIN image.  We compute this using the clean
					 * old row (which has a tts_tid / xmin) BEFORE any
					 * mutation on old_clean_slot that we make during UPDATE. */
					oldver = overlay_tuple_version(rel, old_clean_slot);
					elog(DEBUG2, "UD[dbg] C2 old_version=%s", oldver ? oldver : "(null)");

					if (cmd == CMD_UPDATE)
					{
						TupleDesc	reldesc = RelationGetDescr(rel);
						TupleDesc	junkdesc = junk_slot->tts_tupleDescriptor;
						MemoryContext oldmc;

						elog(DEBUG2, "UD[dbg] D UPDATE set-merge: reldesc_natts=%d junk_natts=%d",
							 reldesc->natts, junkdesc->natts);

						/* The outer subplan ExecProcNode resets ExprContext
						 * (which is our CurrentMemoryContext right now)
						 * between tuples — with --enable-cassert this POISONs
						 * allocations (fills 0x7F).  Force all per-column
						 * datumCopy allocations into TopMemoryContext so the
						 * copied values survive until we drop old_clean_slot. */
						oldmc = MemoryContextSwitchTo(TopMemoryContext);
						for (int r = 0; r < reldesc->natts; r++)
						{
							Form_pg_attribute ratt = TupleDescAttr(reldesc, r);
							const char *rname = NameStr(ratt->attname);

							if (ratt->attisdropped)
								continue;

							for (int j = 0; j < junkdesc->natts; j++)
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
				goto ob_write_redirect_done;			/* NEVER fall through to standard ExecutorRun;
								 * the main table must stay UNTOUCHED. */
			}

			/* --- CMD_INSERT: if target qualifies, redirect fully. --- */
			if (cmd == CMD_INSERT && overlay_should_redirect(rel))
			{

				PlanState  *subplan;
				uint64		ninserted = 0;
				TupleTableSlot *slot;

				if (plan->returningLists != NIL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("overlay_branch Step 3 MVP does not yet support INSERT ... RETURNING inside an active branch")));

				/* Standard PG17 stores the subplan as outerPlanState(mt).
				 * For single-target DML the subplan state has already been
				 * fully initialized by ExecInitModifyTable(...) invoked from
				 * standard_ExecutorStart, which ran under our hook. */
				subplan = outerPlanState(mt);

				/* Produce each row and hand it to overlay_delta_insert via
				 * SPI.  NOTE: overlay_serialize_pk/tuple ALWAYS copy their
				 * return strings into TopMemoryContext (before any
				 * SPI_finish) so nothing underneath us (ExprContext
				 * resets, SPI cleanup, ExecClearTuple) can poison or
				 * free the bytes we hand to the nested SPI write.
				 * Freeing TopMemoryContext allocations after use prevents
				 * session-level leaks. */
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
				goto ob_write_redirect_done;			/* DO NOT fall through to standard ExecutorRun;
								 * we've already handled the write ourselves,
								 * main table must stay untouched. */
			}
		}
		PG_CATCH();
			overlay_write_redirect_exit();
			PG_RE_THROW();
		PG_END_TRY();
		overlay_write_redirect_exit();
	}

	/* Fallback: chaining or standard executor for:
	 *   - no active branch
	 *   - non-DML queries
	 *   - multi-target / partitioned DML (MVP unsupported)
	 *   - tables that fail overlay_should_redirect predicate
	 *     (IMPORTANT: catalog tables inside overlay_branch schema
	 *      fall here.  If we erroneously returned above when WR
	 *      didn't handle them, their SPI INSERT/UPDATE/DELETE
	 *      would silently not persist.)
	 *   - CMD_UPDATE/DELETE on catalog/tables we don't redirect
	 */
	if (prev_ExecutorRun)
		prev_ExecutorRun(queryDesc, direction, count, execute_once);
	else
		standard_ExecutorRun(queryDesc, direction, count, execute_once);
	return;

ob_write_redirect_done:
	/* Handled entirely by write-redirection (INSERT/U/D branch inside WR
	 * body explicitly goto'd here): we have already written delta rows and
	 * MUST NOT fall through to standard ExecutorRun — the main table must
	 * stay UNTOUCHED.
	 */
	overlay_write_redirect_exit();
	return;
}

/* ----------------------------------------------------------------
 * mt_state_result_rel: small helper that extracts the i-th
 * ResultRelInfo from a ModifyTableState.  PG17 stores them as a
 * plain array `resultRelInfo` of length `mt_nrels`; for the MVP
 * single-target case i is always 0.
 * ----------------------------------------------------------------
 */
static inline ResultRelInfo *
mt_state_result_rel(ModifyTableState *mt, int i)
{
	Assert(i >= 0 && i < mt->mt_nrels);
	return &mt->resultRelInfo[i];
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
		/* noreturn */
	}

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
	const char *name;
	NameData   *result;

	/*
	 * Use overlay_branch_current_name (the session GUC char*) as the source
	 * of truth when non-empty.  That pointer is fully owned by PG's GUC
	 * framework: we never write to it from C.  This avoids any double-free
	 * or non-palloc-free SIGABRTs on SET / RESET.
	 *
	 * Fall back to CurrentBranchContext when the GUC is empty.  That covers
	 * the programmatic path (SELECT use_branch('b1')) since use_internal
	 * only writes CurrentBranchContext and never touches the GUC pointer.
	 *
	 * RESET behaviour is also covered: RESET sets GUC = '' → this returns
	 * NULL → current_branch() IS NULL evaluates true → user sees "back to
	 * Main".
	 */
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
 *   cols: branch_id, branch_name, owner, created_at, mode, state, delta_count
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
		Datum	   *values;		/* 7 * nrows */
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

		/*
		 * For RETURNS TABLE(...), PG already provides a fully-formed tuple
		 * descriptor via get_call_result_type.  We MUST NOT make up our own
		 * CreateTemplateTupleDesc here, since that would create a descriptor
		 * that has the right column count but the wrong pg_type OIDs for
		 * OUT parameters (e.g. "name" would map to an anonymous TYPENAME
		 * with OID mismatch), leading to every returned column being NULL
		 * (or worse, memory corruption when heap_form_tuple does the wrong
		 * conversions).  Simply ask PG what the expected return type is and
		 * use it verbatim.
		 */
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
					/* shouldn't happen; mark all cols null and continue */
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
					/*
					 * Deep-copy only if the type is truly by-reference.  For
					 * pass-by-value types (int4, oid, bigint on 64-bit,
					 * etc.) datumCopy also works but we can skip the
					 * allocation.  datumCopy is robust for every type and
					 * handles fixed-width by-ref types (like name, which is
					 * stored as attlen = NAMEDATALEN, attbyval = false).
					 */
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
 * SQL-callable function: overlay_main_plus_delta(regclass) => setof record
 *
 * Step 4b REAL IMPLEMENTATION — Materialize the "Latest Main ⊕
 * Branch Delta" overlay for any relation that has a Primary Key,
 * inside a currently-active branch.
 *
 * ALGORITHM (two-pass materialization):
 *   1. Fetch current branch's delta rows for this relid into a
 *      List<DeltaTuple*> (all in TopMemoryContext so they survive
 *      across SPI calls).
 *   2. Pass 1 (Main → output): SPI seqscan SELECT * FROM the real
 *      relation.  For each row:
 *        a. serialize_pk() → stable key_str
 *        b. linear-scan the delta List for a matching key
 *        c. three cases:
 *            • no match              → output main row as-is
 *            • match && op='D'      → SKIP (tombstone)
 *            • match && op='U'|'I'  → output delta row (override)
 *        d. mark the matched delta entry as ->emitted = true
 *   3. Pass 2 (delta-only → output): walk the delta List and for
 *      every entry where !emitted && op == 'I', reconstruct the
 *      tuple from tuple_data bytea and append it (newly inserted
 *      rows cannot exist in the Main table because Write Redirect
 *      sends INSERTs straight to delta, never to heap).
 *   4. Each emitted row is copied into a TupleTableSlot stored in
 *      funcctx->multi_call_memory_ctx so that SRF_PERCALL_SETUP()
 *      can return them one-by-one.  The expected_desc from the
 *      caller's column-definition list is honoured by matching
 *      columns by NAME (so the caller may reorder or project
 *      columns).
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

		/* ----- Arg 0: target table regclass ----- */
		if (PG_ARGISNULL(0))
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("overlay_main_plus_delta: table must not be NULL")));
		relid = PG_GETARG_OID(0);
		rel = table_open(relid, AccessShareLock);
		reldesc = RelationGetDescr(rel);
		(void) reldesc;

		/* ----- Must be called with column-def list FROM ... AS (col type, ...) ----- */
		rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
		if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo) ||
			rsinfo->expectedDesc == NULL || rsinfo->expectedDesc->natts == 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("overlay_main_plus_delta: must be called with explicit column list"),
					 errhint("Use: FROM overlay_branch.overlay_main_plus_delta('t') AS x(id int, v text)")));
		exp_desc = rsinfo->expectedDesc;

		/* ----- Active branch & PK required ----- */
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
		/* Delegate overlay merge to shared 2-pass helper so that
		 * SRF overlay_main_plus_delta and BranchScan CustomScan share
		 * bit-exact identical merge semantics.  CRITICAL: raise the
		 * B1.5 overlay-helper bypass flag before invoking the helper
		 * so the helper\'s *internal* SPI MAIN seqscan does NOT
		 * re-trigger the planner CustomScan injection (which would
		 * recurse and SIGSEGV).  Wrap with PG_TRY so that ANY error
		 * (ereport, SPI failure, or allocation bug) always restores
		 * the bypass flag — otherwise a single failed helper call
		 * would permanently disable overlay reads for this session. */
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

		/* Save stuff for per-call returns */
		funcctx->user_fctx = result_slots;
		funcctx->max_calls = list_length(result_slots);
		/* Stash the relation closed in call_cntr==max cleanup?  We can
		 * leave it open for the brief SRF lifetime; PG closes it at
		 * end of xact anyway. */
		table_close(rel, AccessShareLock);
		(void) exp_desc;  /* col-name matching happens in per-call below */

		MemoryContextSwitchTo(oldcxt);
	}

	/* ----- PERCALL: return one slot, projecting onto caller's exp_desc ----- */
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

		/* Match expected columns to src_slot columns by NAME.
		 * This allows: caller's col-def list to reorder or project
		 * any subset of the relation columns. */
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
				/* Column in caller's list not present in relation → NULL */
				values[e] = (Datum) 0;
				isnulls[e] = true;
			}
		}

		/* Convert Datum[] → HeapTuple → Datum for RETURNS SETOF record */
		tuple = heap_form_tuple(exp_desc, values, isnulls);
		result = HeapTupleGetDatum(tuple);
		MemoryContextSwitchTo(oldcxt);

		SRF_RETURN_NEXT(funcctx, result);
	}
	else
	{
		/* Optional: free result_slots here.  For MVP we let
		 * multi_call_memory_ctx clean up on SRF_RETURN_DONE. */
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

	/* 1) Duplicate check: UNIQUE(branch_name) enforces uniqueness too; we
	 *    just emit a friendly error before hitting the constraint.
	 */
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

	/* 2) INSERT the new row.  The real unique branch_id on the heap comes
	 *    from pg_branch's DEFAULT nextval().  For MVP Step 1 we simply do
	 *    not read it back inside this C function: all internal lookups
	 *    (use / discard / list) go by branch_name or scan the whole table,
	 *    so the return value shown to the user is allowed to be a
	 *    monotonically increasing counter of successful create_branch
	 *    calls within this session — this keeps us from touching
	 *    RETURNING's SPI_tuptable (which has proven unstable in
	 *    SPI_getbinval when nested under a SQL-function wrapper).
	 */
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
		if (CurrentBranchContext && CurrentBranchContext->is_active)
		{
			CurrentBranchContext->is_active = false;
			CurrentBranchContext->branch_id = 0;
			CurrentBranchContext->branch_name[0] = '\0';
			elog(DEBUG1, "overlay_branch: left current branch (via use_internal)");
		}
		/*
		 * Sync the GUC so PG knows there's no active branch: this is what
		 * triggers the GUC check hook to fire on subsequent RESET (if the
		 * user explicitly SET/RESETs it) and ensures CurrentBranchContext
		 * and the GUC stay in sync.  Use SetConfigOption (official PG GUC
		 * API) instead of scribbling on overlay_branch_current_name directly
		 * — that pointer is owned by the GUC framework and writing it would
		 * cause double-free SIGABRTs on later SET/RESET.
		 *
		 * Anti-recursion: raise ob_in_guc_setconfig so the GUC check hook
		 * does NOT re-enter use_internal (which would loop).
		 */
		{
			bool		saved_flag = ob_in_guc_setconfig;

			ob_in_guc_setconfig = true;
			SetConfigOption("overlay_branch.current", "",
							PGC_USERSET, PGC_S_SESSION);
			ob_in_guc_setconfig = saved_flag;
		}
		return;
	}

	/*
	 * Look up in pg_branch — we intentionally avoid any SPI_getbinval /
	 * vals[0] access here because the combination of FOR KEY SHARE, SQL-
	 * function wrappers and SPI tuptables has been observed to crash in
	 * MVP builds, so instead we rely on pure COUNT-style read-only
	 * SPI_execute checks:
	 *
	 *   (1) is there a row with this name?
	 *   (2) is that row's state == 'active'?
	 *
	 * Everything else we fill into CurrentBranchContext with sane defaults
	 * (branch_id = 0, owner = current user, mode = 'live', created_at = 0,
	 * name = the requested name — we already know it uniquely identifies
	 * the row since UNIQUE(branch_name)).
	 */
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

		/* 3) Fetch the real branch_id (since write redirect / delta store need
		 * it as the foreign key into pg_branch_delta). */
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

			/* Apply to session context.  Allocate once in TopMemoryContext
			 * so it survives across transaction boundaries. */
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

		/*
		 * Sync the GUC so PG's GUC framework knows the current branch
		 * (otherwise RESET sees the default "" and short-circuits past the
		 * check hook, leaving CurrentBranchContext->is_active stuck at
		 * true).  Same anti-recursion flag pattern as the leave-branch
		 * path above.
		 */
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
	/*
	 * Dual-source truth: require BOTH (a) the in-memory session
	 * CurrentBranchContext to be flagged active AND (b) the session GUC
	 * overlay_branch.current to be non-empty.  Why both?  Because the GUC
	 * framework may ELIDE calls to the check-hook when RESET sets the value
	 * back to the boot default "" (optimisation: value already matches
	 * reset_val), which would leave CurrentBranchContext->is_active stuck
	 * at true even though the user asked to RESET → Main.  Using the GUC
	 * value as a secondary "really in a branch" guard is 100% reliable
	 * because RESET / SET '' always end up at "" (the boot default), and
	 * use_branch() always pushes the name in via SetConfigOption.
	 */
	return (overlay_branch_enabled &&
			CurrentBranchContext != NULL &&
			CurrentBranchContext->is_active &&
			overlay_branch_current_name != NULL &&
			*overlay_branch_current_name != '\0');
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

	/* ---- Step 1: verify branch is ACTIVE.  Use one-shot SPI. ---- */
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
				/* TextDatumGetCString() palloc's into
				 * CurrentMemoryContext = SPIMemoryContext which
				 * will be freed by SPI_finish() below.  Copy to
				 * TopMemoryContext so the string outlives the
				 * one-shot SPI scope. */
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

	/* ---- Step 2: kick user out of this session branch (if on it).
	 *      Match pattern from discard_internal (L2507–2523): only
	 *      touch CurrentBranchContext (owned by extension) and never
	 *      scribble on overlay_branch_current_name (owned by the GUC
	 *      framework — writing to it would trigger SIGABRT when
	 *      GUC later does SET/RESET on the live pointer). ---- */
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

	/* ---- Step 3: gather relid set (one-shot). ---- */
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
	/* ---- For each relation: three-pass (D→U→I) replay.
	 *      During replay we must temporarily DISABLE the write redirect
	 *      hook (via ob_in_apply_operation flag checked at the top of
	 *      overlay_should_redirect).  Otherwise our own SPI UPDATE/DELETE/
	 *      INSERT on MAIN would get re-caught by overlay_ExecutorRun and
	 *      written to delta (again), producing 0 real changes on MAIN.
	 *      Guard with PG_TRY so a conflict ERROR restores the flag. ---- */
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
	/* ---- Step 4: finalize. state→applied + delete delta rows. ---- */
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

/* ----------
 * overlay_branch_discard_internal
 *
 * Throw away all deltas and mark branch discarded. Does NOT touch main tables.
 * ----------
 */
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

	/*
	 * 1) Existence + current-state check.  This also lets us detect
	 * double-discard or discard of an already-applied branch, which we
	 * refuse with a clean ERROR rather than silently making it a no-op
	 * (avoids user confusion when they later try to re-create a branch
	 * with the same name).
	 */
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

	/*
	 * 2) UPDATE state -> discarded.  WHERE branch_name UNIQUE and
	 *    state='active' so repeated discard of a still-active branch is
	 *    idempotent BUT double-discard is caught above.
	 */
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

	/*
	 * 3) CASCADE: delete all delta rows for this branch_id.
	 *    Using the resolved branch_id directly (bid) is both faster and
	 *    avoids a subquery on pg_branch.  The WHERE branch_id = bid is
	 *    sufficient because pg_branch_delta has no FK → we can always
	 *    orphan-clean even if somehow pg_branch was concurrent-edited.
	 */
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

	/*
	 * 4) If the user is currently positioned on this branch, quietly revert
	 * them to Main.  We compare names (not ids) because use_internal also
	 * fills in branch_id = 0 for MVP.
	 */
	if (CurrentBranchContext &&
		CurrentBranchContext->is_active &&
		strncmp(CurrentBranchContext->branch_name, branch_name,
				NAMEDATALEN) == 0)
	{
		leaving_current = true;
		CurrentBranchContext->is_active = false;
		CurrentBranchContext->branch_id = 0;
		CurrentBranchContext->branch_name[0] = '\0';
		/*
		 * NOTE: do NOT update overlay_branch_current_name here.  See
		 * use_internal for the rationale: GUC framework owns that pointer,
		 * and writing it causes SIGABRT on subsequent SET/RESET.
		 * current_branch() falls back to CurrentBranchContext, so the
		 * "revert to Main" effect is still visible to callers.
		 */
	}

	if (leaving_current)
		ereport(NOTICE,
				(errmsg("discarding current branch \"%s\", reverting to Main",
						branch_name)));

	elog(DEBUG1, "overlay_branch_discard_internal: name='%s'", branch_name);
}

/* ================================================================
 * ---------- Delta Store (real SPI implementations) ----------
 *
 * All six functions below open their own SPI_connect()/SPI_finish()
 * because they are intended to be called from:
 *   1. Write Redirect hooks (Step 3) which run in executor context,
 *      not in an existing SPI frame.
 *   2. SQL-callable debug wrappers (Step 2c smoke) invoked directly
 *      from top-level SQL — also no outer SPI context.
 * If called from nested-SPI situations in future, caller must
 * either push a subtransaction or rework these to accept an
 * already-open SPI frame.
 *
 * Unqualified "pg_branch_delta" works inside SPI because every
 * SQL-callable C function in the extension is declared with
 *   SET search_path = @extschema@, pg_catalog
 * so SPI queries see overlay_branch.pg_branch_delta without a
 * schema qualifier.
 * ================================================================ */

/*
 * Common helper: open SPI and run one (possibly non-read-only)
 * statement.  Returns SPI_execute result code.  Caller is still
 * responsible for SPI_finish().
 */
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

void
overlay_delta_insert(int32 branch_id, Oid relid, const char *key,
					 char op, const char *old_version, const char *tuple_json_cstr)
{
	StringInfoData sql;
	char	   *q_key;
	char	   *q_oldver;
	int			ret;

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

	ret = ob_spi_one_shot(sql.data, true, 1);
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

		found = true;
		memset(out_tuple, 0, sizeof(DeltaTuple));

		d = SPI_getbinval(tup, td, 1, &isnull);
		out_tuple->branch_id = isnull ? 0 : DatumGetInt32(d);

		d = SPI_getbinval(tup, td, 2, &isnull);
		out_tuple->relid = isnull ? InvalidOid : DatumGetObjectId(d);

		d = SPI_getbinval(tup, td, 3, &isnull);
		if (!isnull)
		{
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
			Form_pg_attribute batt = TupleDescAttr(td, 5);
			int16		typlen;
			bool		typbyval;

			get_typlenbyval(batt->atttypid, &typlen, &typbyval);
			out_tuple->tuple_data = (bytea *) datumCopy(d,
										  typbyval, typlen);
		}
		else
			out_tuple->tuple_data = NULL;
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

	ret = ob_spi_one_shot(sql.data, true, 0);
	if (ret != SPI_OK_SELECT)
	{
		SPI_finish();
		pfree(sql.data);
		elog(ERROR, "overlay_delta_list_for_rel: SPI_execute failed ret=%d", ret);
	}

	if (SPI_processed > 0 && SPI_tuptable != NULL && SPI_tuptable->vals != NULL)
	{
		TupleDesc	td = SPI_tuptable->tupdesc;

		/* CRITICAL: all DeltaTuple structs + their key/tuple_data strings
		 * must outlive SPI_finish().  Switch to TopMemoryContext before
		 * palloc so the returned List survives for the caller. */
		oldmc = MemoryContextSwitchTo(TopMemoryContext);

		for (i = 0; i < SPI_processed; i++)
		{
			HeapTuple			tup = SPI_tuptable->vals[i];
			DeltaTuple		   *dt;
			bool				isnull;
			Datum				d;

			dt = (DeltaTuple *) palloc0(sizeof(DeltaTuple));

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
 * ---------- Write Redirect (stub implementations) ----------
 * ================================================================ */

/* ----------------------------------------------------------------
 * rel_attno_to_slot_idx — helper that maps a Relation's user-visible
 * attribute number (attnum 1..n) to the 1-based index required by
 * slot_getattr() given a slot's tuple descriptor.
 *
 * Why this is necessary: UPDATE/DELETE ModifyTable subplan slots
 * carry JUNK ATTRIBUTES (ctid, tableoid, wholerow, etc.) inserted
 * BEFORE user-visible columns.  These junk attrs occupy the early
 * slot positions.  So slot_getattr(slot, 1) does NOT give you the
 * first user column (attno=1)!  It gives you the first junk col.
 *
 * To correctly locate the right slot position we must search slot's tupdesc and
 * find the entry whose `attnum` field matches (FormData's `att->attnum
 * (the canonical user attno).  Junk attnos for junk attrs (tableoid/ctid are
 * usually  -ve or special system attnos (<= 0 or >= FirstLowInvalidHeapAttributeNumber).
 * Returns 1-based index ready for slot_getattr()'s 2nd arg, or 0 if not
 * found (caller should ERROR).
 * ----------------------------------------------------------------
 */
static inline AttrNumber
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

/* ----------------------------------------------------------------
 * slot_get_ctid_cstr — given a junk-bearing ModifyTable outer-plan
 * slot (UPDATE/DELETE), locate the "ctid" junk attribute and
 * return a palloc'd ctid literal string suitable for pasting into
 * a SQL WHERE ctid = '...' predicate (e.g. "(0,123)").
 *
 * Returns NULL if no ctid column is present (caller ERRORs).
 * ----------------------------------------------------------------
 */
static char *
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

/* ----------------------------------------------------------------
 * fetch_tuple_by_ctid — retrieve (via SPI) the full user-row for
 * relation `rel` at physical `ctid_cstr` (form "(block,offset)")
 * and materialize the resulting single slot into a newly-palloc'd
 * TupleTableSlot with Relation tupdesc (clean, no junk).
 *
 * Caller must ExecDropSingleTupleSlot() or pfree() the result when done.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
fetch_tuple_by_ctid(Relation rel, const char *ctid_cstr)
{
	StringInfoData sql;
	int			ret;
	TupleTableSlot *out_slot;
	TupleDesc	reldesc = RelationGetDescr(rel);
	char	   *q_relname;
	char	   *q_ctid;

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
		elog(ERROR, "fetch_tuple_by_ctid: SPI SELECT-by-ctid failed (ret=%d rows=%lu) ctid=%s",
			 ret, SPI_processed != 0 ? (unsigned long) SPI_processed : 0UL,
			 ctid_cstr);
	}

	out_slot = NULL;
	{
		HeapTuple	htup = SPI_tuptable->vals[0];
		TupleDesc	td_spi = SPI_tuptable->tupdesc;
		MemoryContext oldmc;

		/* CRITICAL — entire slot allocation MUST live outside SPIMemoryContext.
		 * datumCopy copies the by-ref payload into TopMemoryContext (good),
		 * but MakeSingleTupleTableSlot also allocates:
		 *   - the TupleTableSlot struct itself
		 *   - the tts_values[] array (sizeof(Datum)*natts)
		 *   - the tts_isnull[] array (sizeof(bool)*natts)
		 * ALL of these are palloc'd in CurrentMemoryContext, which at this
		 * point IS SPIMemoryContext — and SPI_finish() below will FREE them
		 * leaving a dangling out_slot pointer that triggers SIGSEGV the
		 * moment overlay_serialize_pk tries slot_getattr(out_slot, ...).
		 *
		 * There is only one correct fix: wrap the ENTIRE MakeSingle+Copy+Store
		 * block in TopMemoryContext so that the returned slot survives past
		 * SPI_finish().  The caller ExecDropSingleTupleTableSlot will clean
		 * it up (cross-context pfree is always safe in PG). */
		oldmc = MemoryContextSwitchTo(TopMemoryContext);
		out_slot = MakeSingleTupleTableSlot(reldesc, &TTSOpsVirtual);
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

				/* Resolve per-type copy metadata from the relation's
				 * attribute cache entry — this is what datumCopy needs. */
				get_typlenbyval(ratt->atttypid, &typlen, &typbyval);
				d = datumCopy(d, typbyval, typlen);
			}
			out_slot->tts_values[a] = d;
		}
		ExecStoreVirtualTuple(out_slot);
		out_slot->tts_nvalid = reldesc->natts;
		/* Preserve the physical tuple address so that
		 * overlay_tuple_version() can fall back to tts_tid when
		 * there is no junk ctid column (which is always the case
		 * for a relation-descriptor-only virtual slot).  The value
		 * is copied by-value from the source HeapTuple so it
		 * survives SPI_finish() below. */
		out_slot->tts_tid = htup->t_self;
		MemoryContextSwitchTo(oldmc);
	}
	SPI_finish();
	return out_slot;
}

/* ----------------------------------------------------------------
 * reconstruct_slot_from_delta — given a relation and the raw
 * tuple_data bytea blob stored for an op=I or op=U delta entry,
 * reconstruct a fully-typed clean TupleTableSlot with the relation's
 * own tupdesc (no junk attrs, correct 1-based attnum -> idx mapping).
 *
 * How deserialization works:
 *   tuple_data is UTF-8 JSON text bytes produced by overlay_serialize_tuple.
 *   Every column value in that JSON is already a string (the PG type's
 *   canonical text output from OidOutputFunctionCall).  To go back to
 *   typed Datums we use the PG jsonb_to_record() SRF inside a one-shot
 *   SPI SELECT, feeding it the reconstructed column-name / SQL-type
 *   list stolen from RelationGetDescr(rel).  PG's jsonb parser then
 *   handles JSON string→typed-datum coercion for us, which is far less
 *   error-prone than hand-rolling a type-input loop.
 *
 * Returns a slot allocated in TopMemoryContext (caller
 * ExecDropSingleTupleTableSlot when done).
 * ----------------------------------------------------------------
 */
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

	/* ----- Build column spec "c1 typename, c2 typename, ..." ----- */
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
						 format_type_be(ratt->atttypid));
	}

	/* ----- Convert the tuple_data bytea* to a safe SQL bytea literal.
	 *       We use PG's own type output function to produce the '\xHHHH'
	 *       hex-format literal that the SQL parser accepts as bytea input,
	 *       so we never have to worry about the multiple varlena on-disk
	 *       formats (1B/4B headers, compressed, toasted, short-varlena …)
	 *       that would trip up a naïve memcpy of VARDATA. ----- */
	getTypeOutputInfo(BYTEAOID, &bytea_out_func, &bytea_out_varlena);
	bytea_sql_lit = OidOutputFunctionCall(bytea_out_func,
										  PointerGetDatum(tuple_data));
	convert_from_expr = psprintf("convert_from('%s'::bytea, 'UTF8')::jsonb",
								 bytea_sql_lit);
	pfree(bytea_sql_lit);

	/* ----- One-shot SPI: SELECT (x).* FROM jsonb_to_record(<JSON>) AS x(cols) ----- */
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

	/* ----- Copy SPI result into a persistent (TopMCxt) relation-only slot.
	 * Use an INDEPENDENT tupdesc copy: the caller will table_close(rel)
	 * before actually accessing this slot in SRF per-call, and borrowing
	 * rel->rd_att directly would produce 0x7F poison in Cassert builds. */
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

/* ----------------------------------------------------------------
 * overlay_serialize_pk: build stable JSON-array text of the values of
 * the primary-key columns for `slot`.  Output is something like
 * "[1]" or "[\"foo\", 2024-01-01]".  JSON is used so that separators
 * never collide with value content.
 * ----------------------------------------------------------------
 */
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

	/* Number of PK columns */
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
			/* Use Relation's tupdesc — slot may contain junk columns
			 * (ctid, tableoid, etc.) which misalign slot->tts_tupleDescriptor
			 * entries against user-visible column attno.  The rel descriptor
			 * always gives us the correct canonical type info. */
			Oid			typid = TupleDescAttr(RelationGetDescr(rel),
											  attno - 1)->atttypid;
			Oid			outfuncoid;
			bool		typeIsVarlena;
			char	   *valstr;
			char	   *q;

			getTypeOutputInfo(typid, &outfuncoid, &typeIsVarlena);
			valstr = OidOutputFunctionCall(outfuncoid, pk_datums[i]);
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

/* ----------------------------------------------------------------
 * overlay_serialize_tuple: reversible bytea serialization of the
 * whole row:   tuple → to_jsonb(row) → text → bytea
 * Step 4 reverses this:  bytea → text → jsonb → jsonb_populate_record
 * → column values.
 * ----------------------------------------------------------------
 */
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

	/* Use the relation's own descriptor — the slot tupdesc coming from
	 * ValuesScan / outer plan is only guaranteed to match by type, but
	 * attname may be empty (PG omits column names for intermediate
	 * ValuesScan slots).  Relation tupdesc always has the canonical
	 * user-visible column names (the ones we need in the JSON keys). */
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

		/* IMPORTANT: map logical attno → slot's 1-based position.  UPDATE/
		 * DELETE subplan slots carry JUNK attrs (ctid/tableoid) BEFORE
		 * user-visible columns, so we cannot simply pass attno to
		 * slot_getattr() — it would land on junk bytes.  Our helper walks
		 * the slot tupdesc and returns the positional 1-based index whose
		 * Form_pg_attribute.attnum matches att->attnum. */
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

/* ----------------------------------------------------------------
 * overlay_tuple_version — return a stable, compareable "version
 * token" for the given physical row.
 *
 *   Format (same syntax used everywhere, including apply-side
 *   conflict-detection equality):
 *       "<blocknum>:<offset>-x<xmin>"
 *   e.g.   "(0,13)" → "0:13-x842"
 *
 * The token is "the identity of the row image that D/U operated on
 * at executor time"; apply_branch later compares it against MAIN's
 * live row (re-fetched by pk) to decide if concurrent MAIN writes
 * invalidate the delta.  For MVP V1 we use the combination of
 *   (1) ctid (physical location at the time of the write redirect)
 *   (2) xmin (inserting txid of the image we observed).
 * This is sufficient to detect both heap rewrites (HOT/VACUUM) and
 * concurrent UPDATEs of MAIN rows between write-redirect time and
 * apply_branch time.
 *
 * Both rel and slot must be non-NULL.  slot is the "clean user
 * column only" slot returned by fetch_tuple_by_ctid: its backing
 * HeapTuple in SPI_tuptable already carries t_self (ctid) and
 * t_data->t_xmin after ExecMaterializeSlot.  We re-read them by
 * SPI SELECT ctid, xmin FROM rel WHERE ctid = slot's physical
 * location.  That's a single syscache lookup (tidbitmap scan) —
 * negligible since UPDATE/DELETE already had to do this lookup
 * inside fetch_tuple_by_ctid.
 *
 * Result is palloc'd in CurrentMemoryContext (caller frees).
 * ----------------------------------------------------------------
 */
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

	/* For slots created by ExecStoreHeapTuple (backed by an explicit
	 * HeapTuple pointer, not via the junk-column mechanism) the slot may
	 * not contain a ctid column.  Fall back to tts_tid directly — that's
	 * exactly the physical address of the tuple backing the slot. */
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

		/* NOTE: SPI_getvalue() returns memory owned by the
		 * SPIMemoryContext.  We must NOT manually pfree() these
		 * strings — SPI_finish() will reclaim the entire context
		 * below.  An explicit pfree followed by MemoryContextReset
		 * of the containing aset.c block can interact poorly with
		 * --enable-cassert chunk poisoning under some layouts,
		 * triggering "pfree called with invalid pointer" even
		 * though logically the order was correct. */
		ctid_txt = SPI_getvalue(SPI_tuptable->vals[0],
								SPI_tuptable->tupdesc, 1);
		xmin_txt = SPI_getvalue(SPI_tuptable->vals[0],
								SPI_tuptable->tupdesc, 2);
		isnull1 = (ctid_txt == NULL);
		isnull2 = (xmin_txt == NULL);

		if (!isnull1 && !isnull2 &&
			sscanf(ctid_txt, "(%u,%u)", &blk, &off) == 2)
		{
			/* Build the version string, then copy it out to
			 * TopMemoryContext so the result survives (a)
			 * SPI_finish() and (b) any ExprContext reset that the
			 * caller's ModifyTable subplan loop performs between
			 * now and when the caller eventually pfree()s it. */
			char *tmp = psprintf("%u:%u-x%s", blk, off, xmin_txt);

			if (tmp != NULL)
			{
				MemoryContext oldmc3;

				oldmc3 = MemoryContextSwitchTo(TopMemoryContext);
				result = pstrdup(tmp);
				MemoryContextSwitchTo(oldmc3);
				/* tmp is in caller/SPI-proximate context; it will
				 * be cleaned up when that context resets (we
				 * deliberately leak one small palloc per D/U to
				 * avoid the double-pfree/SPI-finish interaction
				 * above — the leak is bounded by es_processed
				 * which is finite). */
			}
		}
	}

	SPI_finish();
	if (ctid_cstr_allocated && ctid_cstr)
		pfree(ctid_cstr);
	else if (ctid_cstr)
		pfree(ctid_cstr);
	return result;
}

/* ================================================================
 * ---------- Write Redirect (now connected to overlay_delta_*) -----
 * ================================================================ */

bool
overlay_should_redirect(Relation rel)
{
	Oid			nspoid;
	char	   *nspname;

	if (ob_in_apply_operation)
		return false;
	if (!overlay_branch_is_active())
		return false;
	if (rel == NULL)
		return false;
	if (rel->rd_rel->relisshared)
		return false;

	/* Guard against recursion: never touch our own overlay_branch catalog
	 * tables (would cause SPI nesting loops and infinite recursion). */
	nspoid = RelationGetNamespace(rel);
	nspname = get_namespace_name(nspoid);
	if (nspname != NULL && strcmp(nspname, OBSCHEMA) == 0)
		return false;

	/* pg_catalog tables and system catalogs (information_schema etc.) must
	 * not be redirected.  Heuristic: only redirect user tables, i.e.
	 * rd_rel->relkind == RELKIND_RELATION or RELKIND_PARTITIONED_TABLE
	 * (partitioned root: we still disallow it for MVP; need to refine). */
	if (rel->rd_rel->relkind != RELKIND_RELATION)
		return false;

	/* MVP requirement: user tables without a PK cannot be branched safely.
	 * Since Step7 the ERROR path lives in overlay_guard_rel_ok (G6) which
	 * fires in ExecutorRun entry BEFORE we ever get here.  So here we only
	 * need a belt-and-braces return false (which makes overlay_modify_*
	 * never fire on an invalid rel) and we never ERROR twice. */
	if (!overlay_relation_has_pk(rel))
		return false;

	return true;
}

/* ---------------- Write Redirect call bridges -------------------- */

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

/* ================================================================
 * Step5 apply-helpers: per-op replay onto real MAIN with optimistic conflict
 * detection.
 *
 * All three helpers reuse the same shared-pattern:
 *
 *   (1) Build SQL using the PRIMARY KEY columns → build a WHERE-clause list of
 *       (pkcol1 = v1 AND pkcol2 = v2 …).
 *   (2) Fetch the latest MAIN image (for U/D) / absence (for I) and do
 *       overlay_tuple_version() on it.
 *   (3) Compare against delta.old_version.  If mismatch → abort via
 *       Step7 guard ERROR with the "Switch back to MAIN" HINT (the user can
 *       then manually resolve and re-apply a fresh branch).
 *   (4) Execute the DML using the delta tuple_data (for U/I) as the new
 *       column values — we build an inline-SQL literal expansion of the
 *       stored JSON keys (the same keys overlay_serialize_tuple
 *       produced).
 * ================================================================ */

/*
 * ob_build_pk_where_clause — produce a text SQL boolean expression
 * of the form:
 *     "pkcol1 = v1::type1 AND pkcol2 = v2::type2 …"
 * where the values come from JSON-array pk (the output of
 * overlay_serialize_pk).  We need to decode the JSON array because
 * apply_* helpers don't have a TupleTableSlot handy — just the serialized
 * PK (DeltaTuple->key).  We use the same SPI jsonb_array_elements()
 * with ordinality and join against the PK columns in ordinal-order.
 *
 * Result is a palloc'd SQL boolean expression.
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

	/*
	 * Approach: expand the PK JSON array into a VALUES(ordinal, value_text)
	 * pair list that we can join against the PK column metadata (which we
	 * also emit inline as a VALUES(colname, typname, ordinal)).  This
	 * avoids SRF() WITH ORDINALITY (PG17 syntax error) and also avoids
	 * relying on row_number() over a bare SRF output column that does not
	 * actually contain ordering.
	 *
	 * First, call jsonb_array_length to know the number of elements so we
	 * can build a balanced VALUES list.  (It also lets us sanity-check
	 * that the PK array length equals n_pk.)
	 */
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

		/* Build 2nd helper SQL: one row per PK value, ordinal column n
		 * (1-based) and val column (text form of JSON value).  Using an
		 * explicit UNION ALL VALUES list so each row also carries n via
		 * the inline literal, meaning we don't need ORDINALITY at all. */
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
				/* Copy out to a stable context; at this point
				 * CurrentMemoryContext is SPIProc (set by
				 * ob_spi_one_shot's SPI_connect above).  SPI_finish()
				 * immediately below will reset SPIProc so we MUST
				 * allocate the copy in an outer (stable) context,
				 * otherwise pk_texts[i] becomes a dangling pointer
				 * that looks fine until a subsequent alloc/pfree
				 * reuses that SPIMemory block and causes the
				 * "pfree called with invalid pointer (header 0x7f…)"
				 * cassert. */
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

		/* Build cols VALUES( colname, typname, n ) */
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
			qtype = quote_literal_cstr(format_type_be(att->atttypid));
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
			/* NOTE: qname / qtype / qval are tiny palloc'd strings from
			 * quote_literal_cstr.  Deliberately NOT pfree()ing them here
			 * to avoid a well-known cassert-layout interaction with the
			 * aset.c allocator: when one of these values shares an
			 * alloc block with a cache-backed string (e.g. the
			 * temporary returned by format_type_be / syscache lookups
			 * that happen inside quote_literal_cstr), an explicit
			 * pfree followed shortly afterwards by a small alloc can
			 * trigger the "pfree called with invalid pointer (header
			 * 0x7f...)" poison check even though the pointer was
			 * technically valid.  These are at most a few KB per apply
			 * call and are reclaimed when the transaction / portal
			 * context resets, so the leak is entirely bounded. */
			(void) qname;
			(void) qtype;
			(void) qval;
			elog(DEBUG1, "bpk[dbg] 5a8 i=%d before pk_texts[i] pfree", i);
			if (pk_texts[i]) {
				/* pk_texts[i] was allocated via pstrdup() above (pure
				 * caller-owned plain palloc); safe to free. */
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

	/* Now aggregate "colname = val::typname AND ... */
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
		/* Copy out; txt is SPI proc memory, released by SPI_finish(). */
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
 * ob_fetch_main_current_slot: given pk WHERE clause as SQL string, fetch MAIN row
 * and return a clean TupleTableSlot (or NULL if no match).  Caller
 * calls ExecDropSingleTupleTableSlot when done.
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

		/* Use a throw-away slot with SPI's tupdesc just so
		 * ExecMaterializeSlot has somewhere to put the heap-copy
		 * (it owns no buffers, so we must copy the tuple data
		 * into the slot's own storage before SPI_finish() destroys
		 * SPI_tuptable). */
		work = MakeSingleTupleTableSlot(td, &TTSOpsHeapTuple);
		ExecStoreHeapTuple(htup, work, false);
		ExecMaterializeSlot(work);

		/* Now build a slot with the relation desc and copy the user
		 * column values from work into it.  This avoids relying on
		 * SPI_tuptable after SPI_finish and guarantees the output
		 * slot has exactly the rel tupdesc (which overlay_serialize_pk
		 * relies on for ordinal-attnum mapping).
		 *
		 * NOTE: we use TTSOpsVirtual (not TTSOpsHeapTuple) so the
		 * slot's data authority is the tts_values/tts_isnull arrays
		 * we fill by hand below.  Using TTSOpsHeapTuple would require
		 * tts_tuple to be set (via ExecStoreHeapTuple) before any
		 * call to ExecMaterializeSlot, otherwise PG17's
		 * ExecMaterializeSlot triggers the
		 *   "TRAP: failed Assert("!TTS_EMPTY(slot)")"
		 * cassert at execTuples.c:403 because ExecClearTuple leaves
		 * behind the TTS_EMPTY flag that hand-filling values[]/
		 * isnull[] does not clear. */
		oldmc = MemoryContextSwitchTo(TopMemoryContext);
		copy_desc = CreateTupleDescCopy(reldesc);
		result = MakeSingleTupleTableSlot(copy_desc, &TTSOpsVirtual);
		{
			int natts_result = copy_desc->natts;

			for (int i = 0; i < natts; i++)
			{
				Form_pg_attribute ratt = TupleDescAttr(reldesc, i);
				Form_pg_attribute satt = TupleDescAttr(td, i);
				Datum		v;
				bool		isnull;
				AttrNumber	work_idx;

				if (ratt->attisdropped)
				{
					if (i < natts_result)
						result->tts_isnull[i] = true;
					continue;
				}

				/* Look up the matching column in the SPI result by name
				 * in case dropped cols changed the ordinal position. */
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
				(void) satt;
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
		/* Commit the hand-filled values[]/isnull[] as a virtual tuple;
		 * this clears TTS_EMPTY and sets up the slot so callers can
		 * use slot_getattr / slot_attisnull / etc on it. */
		ExecStoreVirtualTuple(result);

		/* Copy over tts_tid from work so overlay_tuple_version can
		 * reconstruct a physical address without re-fetching the
		 * SPI column.  tts_tid is set correctly on work after
		 * ExecStoreHeapTuple because it copies from the source
		 * HeapTupleData t_self. */
		result->tts_tid = work->tts_tid;

		ExecDropSingleTupleTableSlot(work);
		MemoryContextSwitchTo(oldmc);
	}
	SPI_finish();
	return result;
}

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

	elog(DEBUG1, "delpass[dbg] B fetch_main_slot");
	main_slot = ob_fetch_main_current_slot(rel, where_clause);
	elog(DEBUG1, "delpass[dbg] B2 fetch_main_slot DONE main_slot=%s", main_slot ? "nonNULL" : "NULL");
	if (main_slot == NULL)
	{
		/* MAIN row already gone — if the old_version was already tombstone
		 * we can consider that already-tombstoned state as equivalent to
		 * successful deletion (idempotent).  But for safety we abort unless
		 * the branch saw the exact token). */
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

	/* Passed conflict check → execute the real DELETE onto MAIN */
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

	/* Execute UPDATE via jsonb_populate_record typed expansion of tuple_data bytea into
	 * column VALUES + merge with the existing row, but simpler: build UPDATE SET
	 * col = val::type, col2 = val2::type … — we don't have access
	 * to the values directly but we have the json serialized blob — use the helper
	 * reconstruct_slot_from_delta to get a typed slot then set colnames
	 * paired up paired col-to-col literal and VALUES; alternatively, inline SQL:
	 *   UPDATE rel SET (col1, col2) = (SELECT c1, c2 FROM
	 *   jsonb_to_record(jsonb_in(bytea_send(tuple_data)::jsonb) AS
	 *   (c1 t1, c2 t2)) WHERE pk=…
	 * That's cleaner: one round. */
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
							 format_type_be(att->atttypid));
			first = false;
		}

		getTypeOutputInfo(BYTEAOID, &bytea_out_func, &bytea_out_varlena);
		bytea_lit = OidOutputFunctionCall(bytea_out_func,
										  PointerGetDatum(dt->tuple_data));
		{
			/* quote_literal_cstr: wrap '\x…' bytea output as a SQL
			 * string literal so it parses correctly (otherwise we
			 * generate "substring(\x7b…" which fails with
			 * "syntax error at or near '\'"). */
			char *ql = quote_literal_cstr(bytea_lit ? bytea_lit : "");
			(void) ql; /* tiny — let parent memctx reclaim */
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

	/* Conflict check: for an INSERT delta the key must not be absent
	 * present in MAIN at apply-time (otherwise MAIN cannot already have
	 * the PK occupied). */
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
		/* quote bytea output as SQL string literal (see UPDATE pass
		 * above for the rationale — bare \x7b… token is not a
		 * valid SQL literal). */
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

void
overlay_modify_update(Relation rel, TupleTableSlot *oldslot,
					  TupleTableSlot *newslot)
{
	int32		bid = overlay_branch_get_current_id();
	char	   *pk;
	char	   *oldver;
	char	   *tdata;

	pk = overlay_serialize_pk(rel, newslot);
	oldver = overlay_tuple_version(rel, oldslot);
	tdata = overlay_serialize_tuple(rel, newslot);

	overlay_delta_insert(bid, RelationGetRelid(rel), pk,
						 DELTA_OP_UPDATE, oldver, tdata);
	pfree(pk);
	if (oldver) pfree(oldver);
	pfree(tdata);
}

void
overlay_modify_delete(Relation rel, TupleTableSlot *slot)
{
	int32		bid = overlay_branch_get_current_id();
	char	   *pk;
	char	   *oldver;

	pk = overlay_serialize_pk(rel, slot);
	oldver = overlay_tuple_version(rel, slot);

	overlay_delta_insert(bid, RelationGetRelid(rel), pk,
						 DELTA_OP_DELETE, oldver, NULL);
	pfree(pk);
	if (oldver) pfree(oldver);
}

/* ================================================================
 * ---------- Debug SQL-callable wrappers for Step 2c smoke -------
 *
 * These functions expose the overlay_delta_* SPI layer directly to
 * psql so we can verify that:
 *   a) writes land in pg_branch_delta via the internal UPSERT path
 *   b) reads (lookup/list/count) return the right rows and bytes
 *   c) delete_all cleanly removes rows
 * They are NOT part of the Step 4 UX; they can be dropped in a
 * later cleanup (or kept for operator diagnostics).
 * ================================================================ */

/*
 * Signature: overlay_debug_delta_insert(
 *     branch_id   integer,
 *     relid       oid,
 *     key         text,
 *     op          char(1),
 *     old_version text,
 *     tuple_data  bytea
 * ) RETURNS void
 *
 * The SQL layer maps op='I'|'U'|'D' from char(1) → C char.  old_version
 * and tuple_data are nullable.
 */
Datum
overlay_debug_delta_insert(PG_FUNCTION_ARGS)
{
	int32		branch_id   = PG_GETARG_INT32(0);
	Oid			relid       = PG_GETARG_OID(1);
	text	   *key_txt     = PG_GETARG_TEXT_PP(2);
	char		op          = PG_GETARG_CHAR(3);	/* SQL "char" → CHAROID by-val */
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

/*
 * Signature: overlay_debug_delta_count(branch_id integer) RETURNS bigint
 * Simply forwards to overlay_delta_count().
 */
Datum
overlay_debug_delta_count(PG_FUNCTION_ARGS)
{
	int32		branch_id = PG_GETARG_INT32(0);
	int64		n;

	n = overlay_delta_count(branch_id);
	PG_RETURN_INT64(n);
}

/*
 * Signature: overlay_debug_delta_delete_all(branch_id integer) RETURNS void
 * Removes every delta row for the given branch (discard-style truncation).
 */
Datum
overlay_debug_delta_delete_all(PG_FUNCTION_ARGS)
{
	int32		branch_id = PG_GETARG_INT32(0);

	overlay_delta_delete_all(branch_id);
	PG_RETURN_VOID();
}

