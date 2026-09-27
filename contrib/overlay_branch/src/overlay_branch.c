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
 * V3 multi-session tuning GUCs
 * ================================================================ */
int			ob_invalidation_check_threshold = 32;
int			ob_invalidation_check_interval_ms = 100;
bool		ob_apply_strict_pins = true;
bool		ob_use_shared_mem_pin_table = false;
bool		ob_in_snapshot_mode_helper = false;

/* ================================================================
 * Global session state
 * ================================================================ */
BranchContext *CurrentBranchContext = NULL;

/* ---------- Recursion guard flags (5-layer Bypass stack) ---------- */
bool		ob_in_apply_operation = false;		/* extern: branch_lifecycle.c writes */
bool		ob_in_guc_setconfig = false;		/* extern: branch_lifecycle.c writes */
bool		ob_in_invalidation_check = false;	/* extern: branch_lifecycle.c throttled() */
bool		ob_in_planner_hook = false;		/* extern: branch_scan planner_hook wrap */
bool		ob_throttled_allow_kickout = false;	/* extern: only WR ExecutorRun allows cleanup */
static bool ob_in_overlay_helper = false;
static bool ob_in_write_redirect = false;

/* T8 § B.9.1 Debug GUCs — § B.11 documented.  Default values match L3
 * expected baseline.  Underlying storage: these two are extern-accessible
 * from branch_lifecycle.c for actual enforcement. */
int			ob_debug_apply_sleep_sec = 0;			/* PGC_SUSET: pg_sleep(N)
													 * between CAS-winner
													 * state='applying' and
													 * MAIN merge; 0 = OFF */
int			ob_debug_invalidation_counter_throttle_override = -1;
											/* PGC_USERSET: >= 0 overrides
											 * DEFAULT_SPI_INVALIDATION_THROTTLE
											 * constant for L1 state_inval
											 * permutation; -1 = use default */

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
static void overlay_guc_assign_current_branch(const char *newval,
											  void *extra);
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
PG_FUNCTION_INFO_V1(overlay_branch_use_with_mode);
PG_FUNCTION_INFO_V1(overlay_branch_is_active_by_name);
PG_FUNCTION_INFO_V1(overlay_branch_force_invalidation_check);
PG_FUNCTION_INFO_V1(overlay_branch_cached_mode);

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
		CurrentBranchContext->branch_main_snapshot = NULL;
		CurrentBranchContext->snapshot_registered = false;
		CurrentBranchContext->invalidation_counter = 0;
		CurrentBranchContext->invalidation_last_check = 0;

		MemoryContextSwitchTo(oldctx);
	}

	/* GUC: overlay_branch.current (triggers USE via assign_hook —
	 *
	 * IMPORTANT PG PATTERN: check hooks MUST be side-effect free.  PG can
	 * and will re-invoke check hooks for pre-flight validation,
	 * multi-pass SET LOCAL savepoint restoration, subxact abort GUC
	 * rollback, etc.  All side-effects (use/exit branch) MUST live in
	 * the assign hook, which is called exactly once per real GUC
	 * assignment (once savepoint rollbacks have been negotiated).  If
	 * we put side-effects in the check hook, savepoint rollback after
	 * an ERROR inside a PL/pgSQL EXCEPTION block double-invokes
	 * ob_exit_branch_cleanup → TopMemoryContext double-pfree → PANIC.
	 * (Recorded as T5 踩坑 #3 in doc/progress_tracker.md.) */
	DefineCustomStringVariable("overlay_branch.current",
							   "Set the current active branch for this session.",
							   "Set to the branch name, or empty/NULL to leave the branch.",
							   &overlay_branch_current_name,
							   "",
							   PGC_USERSET,
							   0,
							   overlay_guc_check_assign_current_branch,
							   overlay_guc_assign_current_branch,
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

	/* -------- V3 multi-session GUCs (FR2/FR4/FR7 tunables) -------- */
	DefineCustomIntVariable("overlay_branch.invalidation_check_threshold",
							"Run a SPI state-recheck after N calls to overlay_branch_is_active (FR4 fast-path override of NOTIFY push).",
							"Set to 0 to disable call-count throttling and require time-based or NOTIFY-based invalidation only.",
							&ob_invalidation_check_threshold,
							32,
							0,
							INT_MAX / 2,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("overlay_branch.invalidation_check_interval_ms",
							"Minimum wall-clock interval between SPI state re-checks, in milliseconds (FR4 fallback).",
							"Set to 0 to disable time-based throttling and rely on NOTIFY + call-count triggers.",
							&ob_invalidation_check_interval_ms,
							100,
							0,
							INT_MAX / 2,
							PGC_USERSET,
							GUC_UNIT_MS,
							NULL,
							NULL,
							NULL);

	DefineCustomBoolVariable("overlay_branch.apply_strict_pins",
							 "apply_branch() fails with ERROR if other sessions still hold use_branch() pins on the branch.",
							 "Strict mode (on) guarantees zero post-apply data drift at the cost of requiring all users to exit the branch first; relaxed mode warns and proceeds.",
							 &ob_apply_strict_pins,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("overlay_branch.use_shared_mem_pin_table",
							 "Enable optional shared-memory pin-count table to speed up apply_branch pinning decisions (FR7).",
							 "Requires loading overlay_branch via shared_preload_libraries. When off, falls back to pure SPI-based state checks.",
							 &ob_use_shared_mem_pin_table,
							 false,
							 PGC_POSTMASTER,
							 0,
							 NULL,
							 NULL,
							 NULL);

	/* T8 § B.11: Debug GUCs — NOT ABI stable, prefixed with _debug_.
	 * Default values match L3 expected baseline. */
	DefineCustomIntVariable("overlay_branch._debug_apply_sleep_sec",
							"(DEBUG) Number of seconds to pg_sleep between apply_branch CAS-winner state='applying' and MAIN row merge.",
							"Set >0 to widen the applying-state window for L1 ob_applying_freeze.spec.  Production default 0 (off).",
							&ob_debug_apply_sleep_sec,
							0,
							0,
							3600,
							PGC_SUSET,
							GUC_UNIT_S,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("overlay_branch._debug_invalidation_counter_throttle_override",
							"(DEBUG) Override FR4 DEFAULT_SPI_INVALIDATION_THROTTLE call-count constant.",
							"Set >= 0 to force a custom throttle value; -1 (default) means use the compiled-in constant (32).  Used by L1 ob_state_inval.spec to shorten permutations.",
							&ob_debug_invalidation_counter_throttle_override,
							-1,
							-1,
							INT_MAX / 2,
							PGC_USERSET,
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
 * GUC hooks for overlay_branch.current
 *
 *   check hook  = SIDE-EFFECT FREE validation only.  PG can (and will)
 *                 re-invoke this for savepoint-rollback GUC restore,
 *                 multi-pass SET LOCAL pre-flight, etc.  Any side-effect
 *                 here → double cleanup → TopMemoryContext double-pfree
 *                 → PANIC (T5 踩坑 #3).
 *
 *                 MUST NOT touch ob_in_guc_setconfig here: the flag is
 *                 OWNED by the assign hook.  SetConfigOption calls the
 *                 hooks in order: (1) check — possibly repeated — then
 *                 (2) assign — exactly once per real assignment.  If we
 *                 reset the recursion flag in check, the assign hook
 *                 sees it as FALSE and re-runs the side-effect — a
 *                 second cleanup or a second use_branch — leading to
 *                 the exact double-pfree we saw in T5 Section N entry
 *                 after a PL/pgSQL EXCEPTION block.
 *
 *   assign hook = REAL side-effects: enter/leave branch.  Called exactly
 *                 once per true GUC assignment (post-negotiation).
 *                 THIS is where we inspect/flip ob_in_guc_setconfig to
 *                 break the SetConfigOption recursion that use_branch()
 *                 creates when it wants to persist the GUC.
 * ================================================================ */
static bool
overlay_guc_check_assign_current_branch(char **newval, void **extra,
										GucSource source)
{
	const char *name;

	/* ---- PG GUC CONTRACT -------------------------------------------------
	 * Check hook = SIDE-EFFECT-FREE VALIDATION ONLY (PG may call this
	 * MULTIPLE times for a single SET: preflight, savepoint rollback,
	 * post-check pre-assign revalidation).  We may use READ-ONLY SPI to
	 * query ground-truth catalog state, but we MUST NOT flip any flags,
	 * write any catalogs, take any locks, or touch CurrentBranchContext.
	 * Return false → PG aborts the SET (no GUC write, PG emits the
	 * standard "invalid value for parameter" error).  Return true →
	 * proceed to assign hook (which performs real side-effects and is
	 * GUARANTEED to succeed because validation is already complete).
	 * ------------------------------------------------------------------ */

	/* NULL / empty = "leave branch" → always valid (no SPI needed) */
	if (newval == NULL || *newval == NULL || **newval == '\0')
		return true;

	name = *newval;

	/* Hard length bound (must fit BranchContext.name[NAMEDATALEN]) */
	if (strlen(name) >= NAMEDATALEN)
		return false;

	/* ------------------------------------------------------------------
	 * EARLY-INIT GUARD: During _PG_init / post-startup GUC sweep, no
	 * database has been connected yet (MyDatabaseId is InvalidOid) and
	 * SPI_connect will fail (no active snapshot / transaction).  In
	 * that phase we simply accept any syntactically-valid name; the
	 * assign hook will still validate via use_internal() if the name
	 * is actually written, and first user SET during normal backend
	 * operation will re-run this full SPI check. */
	if (MyDatabaseId == InvalidOid)
		return true;

	/* ------------------------------------------------------------------
	 * READ-ONLY SPI VALIDATION — branch EXISTS + state = ACTIVE.
	 * Uses the current LatestSnapshot (same visibility as user SELECTs
	 * on the catalog).  Errors from SPI (e.g. table missing because
	 * CREATE EXTENSION not yet run in this database → fail-closed
	 * reject the value.  Extension not loaded? user can still call
	 * use_branch() SQL-callable directly; SET-based path is a luxury.
	 *
	 * Before returning false (PG will raise the generic "invalid value
	 * for parameter" error), we first emit a CUSTOM ereport(ERROR) with
	 * human-readable text matching the public use_branch() API.  PG's
	 * check-hook contract allows ereport + return false as a pair: the
	 * ERROR is delivered to the user exactly as-is, and the GUC value
	 * remains unchanged. */
	{
		bool		is_valid = false;
		bool		found = false;
		bool		state_active = false;
		int			spi_rc;
		StringInfoData sql;
		char	   *esc_name;

		if (SPI_connect() != SPI_OK_CONNECT)
			return false;

		initStringInfo(&sql);
		esc_name = quote_literal_cstr(name);
		appendStringInfo(&sql,
						 "SELECT state FROM " OBTABLE_BRANCH
						 " WHERE branch_name = %s LIMIT 1",
						 esc_name);

		spi_rc = SPI_execute(sql.data, true, 1);
		pfree(sql.data);

		if (spi_rc == SPI_OK_SELECT && SPI_processed == 1)
		{
			Datum		state_datum;
			bool		isnull;
			char	   *state_text;

			found = true;
			state_datum = SPI_getbinval(SPI_tuptable->vals[0],
										SPI_tuptable->tupdesc,
										1, &isnull);
			if (!isnull)
			{
				state_text = TextDatumGetCString(state_datum);
				if (state_text != NULL &&
					strcmp(state_text, BRANCH_STATE_ACTIVE) == 0)
				{
					is_valid = true;
					state_active = true;
				}
				else
				{
					state_active = false;
				}
				pfree(state_text);
			}
		}

		SPI_finish();

		if (!is_valid)
		{
			if (!found)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("branch \"%s\" does not exist", name)));
			else if (!state_active)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("branch \"%s\" is not active", name)));
			/* (fall-through) return false below in case ereport path
			 * was compiled out; normally ereport(ERROR) above does not
			 * return. */
		}
		return is_valid;
	}
}

static void
overlay_guc_assign_current_branch(const char *newval, void *extra)
{
	/* ---- Recursion guard (caller-owned flag) ---------------------------
	 *
	 * External callers of SetConfigOption (use_with_mode_internal /
	 * throttle cleanup helper / etc.) lift ob_in_guc_setconfig to TRUE
	 * *before* calling SetConfigOption and restore the saved pre-call
	 * value (typically FALSE) immediately after SetConfigOption returns.
	 *
	 * WE NEVER TOUCH THIS FLAG HERE.  If we cleared it inside the hook,
	 * a single SetConfigOption that internally invokes the assign hook
	 * TWO or more times (PG GUC machinery does internal two-pass updates
	 * for some value classes; savepoint rollback also re-checks) would
	 * see flag=FALSE on the SECOND pass and enter the real side-effect
	 * code — double-cleanup / double-pfree / state corruption. */
	if (ob_in_guc_setconfig)
		return;

	/* ---- PG GUC CONTRACT ------------------------------------------------
	 * Assign hook = REAL SIDE-EFFECTS ONLY, MUST NOT FAIL.
	 *
	 * The check hook overlay_guc_check_assign_current_branch() has
	 * already performed FULL validation:
	 *   • Name length < NAMEDATALEN
	 *   • (if non-empty) branch EXISTS in pg_branch AND state='active'
	 *
	 * If check returned true, this assign hook runs and the below
	 * operations are GUARANTEED error-free.  The only non-standard
	 * case is the PL/pgSQL abort-restore path (T5 PANIC fix), handled
	 * by LAYER-1 below.  No PG_TRY needed — error paths simply cannot
	 * happen given check-hook prevalidation. */

	/* Empty/NULL → leave any active branch.  Check hook has already
	 * validated empty strings (always valid). */
	if (newval == NULL || *newval == '\0')
	{
		if (CurrentBranchContext != NULL && CurrentBranchContext->is_active)
		{
			/* I8.2: persist cached mode BEFORE cleanup so GUC RESTORE +
			 * re-entry on the same bid re-enters the same mode the user
			 * had selected (snapshot survives leave→re-enter cycles). */
			ob_mode_cache_set(CurrentBranchContext->branch_id,
							  CurrentBranchContext->mode);
			ob_exit_branch_cleanup(CurrentBranchContext);
		}
		return;
	}

	/* Non-empty → use branch. */

	/* FIX-B: Zombie empty-ctx pass-through.
	 *
	 * When GUC "" reset runs on a session where CurrentBranchContext was
	 * never fully populated (bid=0 / name='') but is_active is still TRUE
	 * (leftover baseline init state), the next non-empty SET would enter
	 * use_internal() with a stale zombie context.  Guard above at L552
	 * handles empty newval (exit_branch_cleanup path), but that path only
	 * fires when is_active && (bid>0 || name[] nonempty).  Here we catch
	 * the bid=0 / name[0]='' / is_active=true zombie shell and let the
	 * GUC proceed without ERROR. */
	if (CurrentBranchContext != NULL &&
		CurrentBranchContext->is_active &&
		CurrentBranchContext->branch_id == 0 &&
		CurrentBranchContext->branch_name[0] == '\0')
		return;

	/* LAYER-1 (PL/pgSQL abort-restore guard):
	 *   If CurrentBranchContext EXISTS but is_active == FALSE, we are
	 *   on a PL/pgSQL subxact-abort GUC RESTORE PATH.  Throttle /
	 *   discard / apply have already kicked us off the branch and run
	 *   exit_cleanup.  Check hook ran BEFORE the abort started and
	 *   said the name is valid (exists+active at that time), but the
	 *   catalog state has moved on since — we MUST NOT call use_internal
	 *   (would call SPI inside an abort unwind → PANIC).  The GUC value
	 *   has already been written by PG; we simply skip the enter side-
	 *   effect.  Any subsequent user action will re-SET and re-check. */
	if (CurrentBranchContext != NULL && !CurrentBranchContext->is_active)
		return;

	/* I8.2 mode-preserve: GUC 1-arg SET (no mode param) — reuse cached mode.
	 * Without this, assign hook always calls use_internal() → mode='live'
	 * unconditionally → PART D/E snapshot modes always regress to 'live' in
	 * list_branches.  With cache hit, re-enter prior mode exactly (matches
	 * prior catalog-WRITE semantics but w/o RowExclusiveLock per use). */
	{
		int32 bid_from_name = 0;
		char   *esc;
		StringInfoData sq;
		const char *mode_for_hook = NULL;

		/* Allocate in outer Caller context BEFORE SPI_connect to avoid
		 * double-free: SPI_finish() frees SPI_proc context, so any
		 * allocations made while SPI_proc is CurrentMemoryContext would
		 * already be released before we pfree them below. */
		esc = quote_literal_cstr(newval);
		initStringInfo(&sq);
		appendStringInfo(&sq,
			"SELECT branch_id FROM overlay_branch.pg_branch "
			"WHERE branch_name = %s LIMIT 1", esc);

		if (SPI_connect() == SPI_OK_CONNECT)
		{
			if (SPI_execute(sq.data, true, 1) == SPI_OK_SELECT &&
				SPI_processed == 1)
			{
				bool isnull = false;
				Datum d = SPI_getbinval(SPI_tuptable->vals[0],
									  SPI_tuptable->tupdesc, 1, &isnull);
				if (!isnull)
					bid_from_name = DatumGetInt32(d);
			}
			SPI_finish();
		}
		pfree(esc);
		pfree(sq.data);
		if (bid_from_name > 0)
			mode_for_hook = ob_mode_cache_lookup(bid_from_name);
		if (mode_for_hook != NULL)
		{
			overlay_branch_use_with_mode_internal(newval, mode_for_hook);
			return;
		}
	}

	/* Check hook says branch EXISTS + state='active' at the latest
	 * snapshot.  This call will not raise any ERROR. */
	overlay_branch_use_internal(newval);
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
	NodeTag		tag;

	/* --- FR4 kickout guard: do NOT run the generic
	 * overlay_branch_is_active() → throttled(DQL) check for pure
	 * session/utility commands that never touch user-table rows.
	 * Running it on, say, a SET command (T_VariableSetStmt) would cause
	 * the following pathological race:
	 *
	 *   1. Session S is on branch B (state ACTIVE in catalog).
	 *   2. S manually flips overlay_branch.pg_branch.state = 'applied'
	 *      (catalog-only write; WR protected-schema guard correctly
	 *      skips the DML kickout so the manual flip persists).
	 *   3. S runs SET overlay_branch.invalidation_check_threshold = 1
	 *      so the *next* user-table DML deterministically runs SPI.
	 *   4. Without the whitelist below: SET triggers ProcessUtility
	 *      → overlay_branch_is_active() → throttled(false) with
	 *      counter already >= new threshold=1 → DQL NOTICE kickout
	 *      → ctx->is_active = false (and ctx->counter reset, GUC
	 *      overlay_branch.current cleared).
	 *   5. The *actual* user-table UPDATE (the drift-write the test
	 *      deliberately wanted to ERROR-block) then sees
	 *      ctx->is_active=false → throttled fast-path return false
	 *      (no ERROR because no "on branch"), falls through to MAIN
	 *      heap, silent drift.
	 *
	 * Solution: only overlay_branch_is_active() + DDL-guard the
	 * statement categories that *might* mutate user state.  Every
	 * Tag below is provably session-only / catalog-maintenance only
	 * and is allowed to skip the check.
	 *
	 * The list is intentionally conservative: add to it only when
	 * you can prove the command type never writes MAIN user heap
	 * rows (no silent pollution possible). */
	if (pstmt != NULL && pstmt->utilityStmt != NULL)
	{
		tag = nodeTag(pstmt->utilityStmt);
		switch (tag)
		{
			case T_VariableSetStmt:
			case T_VariableShowStmt:
			case T_ListenStmt:
			case T_NotifyStmt:
			case T_UnlistenStmt:
			case T_TransactionStmt:
			case T_ConstraintsSetStmt:
			case T_DiscardStmt:
			case T_LockStmt:
			case T_CheckPointStmt:
			case T_ExecuteStmt:
			case T_PrepareStmt:
			case T_DeallocateStmt:
			case T_CreateSubscriptionStmt:
			case T_DropSubscriptionStmt:
			case T_AlterSubscriptionStmt:
			case T_CreatePublicationStmt:
			case T_AlterPublicationStmt:
			case T_SecLabelStmt:
				/* Pure session / maintenance utility — no user-data
				 * mutation possible, so skip the is_active() call and
				 * its throttled(DQL) side-effect.  Still run normal
				 * command below. */
				break;
			default:
				if (overlay_branch_is_active() &&
					!overlay_guard_ddl_ok_for_branch(pstmt->utilityStmt,
													  &op_name, &obj_name,
													  &fail_reason))
				{
					overlay_guard_ereport_fail(op_name ? op_name : "execute utility/DDL",
											   obj_name, fail_reason);
				}
				break;
		}
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
overlay_branch_use_with_mode(PG_FUNCTION_ARGS)
{
	Name		branch_name = PG_GETARG_NAME(0);
	const char *mode;

	if (PG_ARGISNULL(1))
		mode = BRANCH_MODE_LIVE;
	else
		mode = text_to_cstring(PG_GETARG_TEXT_PP(1));

	overlay_branch_use_with_mode_internal(NameStr(*branch_name), mode);
	PG_RETURN_VOID();
}

/* --------------------------------------------------------------------
 * overlay_branch_cached_mode — SQL-callable helper: return per-bid
 * session-local cached mode, or NULL if not cached (so caller can
 * COALESCE with catalog mode).  Used by list_branches() SQL body to
 * overlay the I8.2 per-bid HTAB onto the pure-catalog SELECT. */
Datum
overlay_branch_cached_mode(PG_FUNCTION_ARGS)
{
	int32 bid = PG_GETARG_INT32(0);
	const char *cached;

	cached = ob_mode_cache_lookup(bid);
	if (cached == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(cached));
}

/* --------------------------------------------------------------------
 * overlay_branch_is_active_by_name — SQL-callable: check catalog state
 * of a named branch, PLUS if the named branch is exactly the current
 * session's in-use branch also run Layer1/FR4 invalidation + kickout.
 *
 * SQL signature: overlay_branch_is_active(branch_name name) RETURNS bool
 * Synonym: public.is_active(name name) RETURNS bool
 * -------------------------------------------------------------------- */
Datum
overlay_branch_is_active_by_name(PG_FUNCTION_ARGS)
{
	Name        branch_name = PG_GETARG_NAME(0);
	const char *bn;
	BranchContext *ctx = CurrentBranchContext;
	bool        result = false;

	if (PG_ARGISNULL(0) || branch_name == NULL)
		PG_RETURN_BOOL(false);
	bn = NameStr(*branch_name);
	if (*bn == '\0')
		PG_RETURN_BOOL(false);

	/* If this is OUR current branch, FIRST run our own inv check so the
	 * call side-effect kicks us out.  is_active → throttled DQL path. */
	if (ctx != NULL && ctx->is_active &&
		ctx->branch_id != 0 &&
		strncmp(ctx->branch_name, bn, NAMEDATALEN) == 0)
	{
		/* is_current = true; (formerly kept for logging — removed to suppress -Wunused-but-set) */
		(void) ob_invalidate_check_throttled(false);
		/* Re-read ctx after possible kick-out */
		if (!CurrentBranchContext || !CurrentBranchContext->is_active)
			PG_RETURN_BOOL(false);
	}

	/* Always hit catalog so user gets ground-truth, not just in-memory.
	 * SPI lifecycle probe (same pattern as ob_invalidate_check_throttled):
	 * we may be reached from inside a nested SPI (e.g. via SQL-language
	 * synonym `public.is_active` whose body is a SELECT that invokes us
	 * through the function executor — outer SQL func may already hold an
	 * SPI connection).  PG does not expose SPI_connected() public API, so
	 * we probe via SPI_connect() return value. */
	{
		int         spi_cr2;
		bool        did_con2 = false;
		StringInfoData sql2;
		int         ret2;
		char       *esc2;

		spi_cr2 = SPI_connect();
		if (spi_cr2 == SPI_OK_CONNECT)
			did_con2 = true;
		else if (spi_cr2 != SPI_ERROR_CONNECT)
			elog(ERROR, "overlay_branch_is_active: SPI_connect ret=%d", spi_cr2);

		esc2 = quote_literal_cstr(bn);
		initStringInfo(&sql2);
		appendStringInfo(&sql2,
					 "SELECT state FROM " OBTABLE_BRANCH " "
					 "WHERE branch_name = %s LIMIT 1",
					 esc2);
		pfree(esc2);

		ret2 = SPI_execute(sql2.data, true, 1);
		pfree(sql2.data);
		if (ret2 != SPI_OK_SELECT)
		{
			if (did_con2)
				SPI_finish();
			elog(ERROR, "overlay_branch_is_active: SPI_execute ret=%d", ret2);
		}
		if (SPI_processed == 1 && SPI_tuptable != NULL && SPI_tuptable->vals != NULL)
		{
			Datum   v2;
			bool    isnull2;
			char   *st2;

			v2 = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc,
							 1, &isnull2);
			if (!isnull2)
			{
				st2 = TextDatumGetCString(v2);
				if (st2 != NULL && strcmp(st2, BRANCH_STATE_ACTIVE) == 0)
					result = true;
				pfree(st2);
			}
		}
		if (did_con2)
			SPI_finish();
	}

	PG_RETURN_BOOL(result);
}

/* --------------------------------------------------------------------
 * overlay_branch_force_invalidation_check — SQL-callable debug helper
 * (no params): forces the Layer1/FR4 invalidation SPI recheck to run
 * NOW regardless of count/time throttles.  Returns true iff the session
 * is still on an ACTIVE branch after the check.  Mainly useful for
 * regression tests to exercise the kickout path without waiting N ms.
 *
 * SQL signature: overlay_branch_force_invalidation_check() RETURNS bool
 * Synonym: public.force_invalidation_check() RETURNS bool
 * -------------------------------------------------------------------- */
Datum
overlay_branch_force_invalidation_check(PG_FUNCTION_ARGS)
{
	BranchContext *ctx = CurrentBranchContext;

	if (ctx == NULL || !ctx->is_active)
		PG_RETURN_BOOL(false);

	/* Force both throttles to fire: counter += THRESHOLD, last_check = 0 */
	if (ob_invalidation_check_threshold > 0)
		ctx->invalidation_counter = (uint32) ob_invalidation_check_threshold + 1u;
	else
		ctx->invalidation_counter = 1u << 30;
	ctx->invalidation_last_check = 0;

	/* Run DQL-path check, but force_invalidation_check() is a deliberate
	 * user-visible "run the check NOW" request — unlike the "observer"
	 * callers (planner hook, rel_ok), force_check needs to actually raise
	 * the DQL NOTICE + clear ctx + reset GUC so the user can see it (J3,
	 * E4).  Opt-in to destructive kickout side-effects.  See branch_life
	 * cycle.c ob_throttled_allow_kickout comment. */
	{
		extern bool ob_throttled_allow_kickout;
		bool		saved_allow = ob_throttled_allow_kickout;
		bool		thr_ok;

		ob_throttled_allow_kickout = true;
		thr_ok = ob_invalidate_check_throttled(false);
		ob_throttled_allow_kickout = saved_allow;
		if (!thr_ok)
			PG_RETURN_BOOL(false);
	}

	PG_RETURN_BOOL(CurrentBranchContext != NULL &&
				   CurrentBranchContext->is_active);
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
				int32		this_bid = 0;
				bool		bid_isnull = true;

				if (tup == NULL)
				{
					for (col = 0; col < 7; col++)
					{
						st->values[i * 7 + col] = (Datum) 0;
						st->nulls[i * 7 + col] = true;
					}
					continue;
				}

				/* Peek bid (col 0) first for possible mode-cache overlay
				 * (FR2 V1 I8.2: mode no longer written to catalog per-use;
				 * it lives in a session-local bid HTAB to avoid cross-
				 * session RowExclusiveLock contention). */
				{
					Datum bid_datum = SPI_getbinval(tup, spi_td, 1,
													&bid_isnull);
					if (!bid_isnull)
						this_bid = DatumGetInt32(bid_datum);
				}

				for (col = 0; col < 7; col++)
				{
					Datum		val;
					bool		isnull;
					Form_pg_attribute att;

					val = SPI_getbinval(tup, spi_td, col + 1, &isnull);

					/* mode column (5th output = col index 4): overlay the
					 * session-local cached mode for this bid, if any.
					 * Fallback = catalog mode (pure read, no lock). */
					if (col == 4 && !bid_isnull)
					{
						const char *cached_mode =
							ob_mode_cache_lookup(this_bid);
						if (cached_mode != NULL)
						{
							st->nulls[i * 7 + col] = false;
							st->values[i * 7 + col] =
								CStringGetTextDatum(cached_mode);
							continue;
						}
					}

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
