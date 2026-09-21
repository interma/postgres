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
#include "commands/async.h"
#include "utils/timestamp.h"
#include "utils/syscache.h"
#include "utils/hsearch.h"

#define OBTABLE_DELTA   OBSCHEMA ".pg_branch_delta"
#define OBTABLE_BRANCH  OBSCHEMA ".pg_branch"

/* ---- External globals owned by overlay_branch.c ---- */
extern bool ob_in_apply_operation;
extern bool ob_in_guc_setconfig;

/* ============ V3 FR5: per-branch frozen snapshot cache (TopMC hash) ============
 *
 * Key = int32 branch_id. Value = the CopySnapshot-allocated frozen MAIN
 * snapshot for snapshot-mode branches that have been entered at least once.
 *
 * Why a global cache instead of just ctx->branch_main_snapshot:
 *   A session can use_branch('A', snapshot) → use_branch('') → then later
 *   use_branch('A', snapshot) AGAIN. If we re-capture GetTransactionSnapshot
 *   on the 2nd entry, MAIN rows that were committed between entry 1 and
 *   entry 2 (e.g. via apply_branch of another LIVE branch) become visible —
 *   which violates "frozen at t0" semantics.  So we must remember the
 *   ORIGINAL snapshot per branch_id and re-install the SAME pointer.
 *
 * Lifetime rules:
 *   • INSERT: ob_install_snapshot_for_mode when a snapshot-mode branch is
 *             entered for the FIRST time (hash miss → capture + insert).
 *   • REUSE:  ob_install_snapshot_for_mode when the SAME branch_id is
 *             re-entered (hash hit → just copy pointer, no re-capture).
 *   • DELETE: (a) apply_branch / discard_branch on that bid, OR
 *             (b) _PG_fini (extension unload).  pfree the snapshot AND remove
 *             the hash entry.  Idempotent.
 *   • The ctx->snapshot_registered flag is a local-per-ctx "soft ownership"
 *     bit: it means CurrentBranchContext holds a REFERENCE count pin on
 *     the global cached snapshot, so ensure_unregister MUST NOT pfree the
 *     snapshot pointer (that's owned by the hash).  It only clears ctx's
 *     reference.  The hash owns the underlying allocation.
 * ==================================================================== */

typedef struct OBSnapshotHashKey
{
	int32	branch_id;
} OBSnapshotHashKey;

typedef struct OBSnapshotHashEnt
{
	OBSnapshotHashKey	key;		/* hash key: MUST be first field */
	Snapshot			frozen;		/* non-NULL = captured CopySnapshot ptr */
	int					refcnt;		/* number of BranchContexts referencing */
} OBSnapshotHashEnt;

static HTAB *ob_snapshot_cache = NULL;

static void
ob_snapshot_cache_init(void)
{
	HASHCTL		ctl;

	if (ob_snapshot_cache != NULL)
		return;
	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(int32);
	ctl.entrysize = sizeof(OBSnapshotHashEnt);
	ctl.hcxt = TopMemoryContext;
	ob_snapshot_cache = hash_create("OverlayBranchSnapshotCache",
									16, &ctl,
									HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static Snapshot
ob_snapshot_cache_lookup(int32 bid)
{
	OBSnapshotHashEnt *ent;

	if (ob_snapshot_cache == NULL)
		return NULL;
	ent = (OBSnapshotHashEnt *) hash_search(ob_snapshot_cache,
											&bid, HASH_FIND, NULL);
	return ent ? ent->frozen : NULL;
}

static void
ob_snapshot_cache_insert(int32 bid, Snapshot snap)
{
	OBSnapshotHashEnt *ent;
	bool	found;

	Assert(bid > 0);
	Assert(snap != NULL);
	ob_snapshot_cache_init();
	ent = (OBSnapshotHashEnt *) hash_search(ob_snapshot_cache,
											&bid, HASH_ENTER, &found);
	if (found)
		elog(ERROR, "overlay_branch: snapshot cache duplicate insert bid=%d", bid);
	ent->frozen = snap;
	ent->refcnt = 0;
}

static void
ob_snapshot_cache_pin(int32 bid)
{
	OBSnapshotHashEnt *ent;

	if (ob_snapshot_cache == NULL)
		return;
	ent = (OBSnapshotHashEnt *) hash_search(ob_snapshot_cache,
											&bid, HASH_FIND, NULL);
	if (ent)
		ent->refcnt++;
}

static void
ob_snapshot_cache_unpin(int32 bid)
{
	OBSnapshotHashEnt *ent;

	if (ob_snapshot_cache == NULL)
		return;
	ent = (OBSnapshotHashEnt *) hash_search(ob_snapshot_cache,
											&bid, HASH_FIND, NULL);
	if (ent && ent->refcnt > 0)
		ent->refcnt--;
}

/* Delete-and-pfree for this bid (apply / discard). Safe if missing. */
static void
ob_snapshot_cache_remove(int32 bid)
{
	OBSnapshotHashEnt *ent;

	if (ob_snapshot_cache == NULL)
		return;
	ent = (OBSnapshotHashEnt *) hash_search(ob_snapshot_cache,
											&bid, HASH_REMOVE, NULL);
	if (ent && ent->frozen)
		pfree(ent->frozen);
}

/* ============ I8.2 V3: session-local per-bid mode cache ============
 *
 * Replaces catalog UPDATE pg_branch.mode in use_branch() with a pure
 * session-local bid → last-mode hash.
 *
 * Rationale (locked in § B.9.1 I8.2):
 *   Prior code persisted every use_branch(mode) call to pg_branch catalog via
 *   SPI UPDATE.  Each UPDATE acquires RowExclusiveLock on the matching
 *   pg_branch row.  Two concurrent sessions calling use_branch('same_bid')
 *   therefore serialized at the heavyweight row lock.  Under pg_isolation_regress
 *   this manifested as s2_p1_use `<waiting ...>` for 360s (test timeout) then
 *   cancel → apply_mutex P1 false deadlock.
 *
 * Semantics:
 *   • Set on every use_branch(name, mode) call (including default 1-arg use,
 *     which passes mode='live').
 *   • Overlaid inside list_branches() SRF (overlay_branch.c) for the matching
 *     bid; catalog mode is read as fallback.
 *   • Removed on apply / discard (bid no longer ACTIVE so re-use impossible
 *     until a future recreate, which inserts a fresh catalog row with the
 *     default mode='live').
 *
 * No heavyweight locks, no catalog serialization.  PART D/E/F mode tests all
 * run inside a single session so hash matches prior catalog semantics exactly.
 * ==================================================================== */

typedef struct OBModeHashEnt
{
	int32	branch_id;			/* hash key: MUST be first field */
	char	mode[16];			/* "live" or "snapshot", NUL-padded */
} OBModeHashEnt;

static HTAB *ob_mode_cache = NULL;

static void
ob_mode_cache_init_lazy(void)
{
	HASHCTL		ctl;

	if (ob_mode_cache != NULL)
		return;
	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(int32);
	ctl.entrysize = sizeof(OBModeHashEnt);
	ctl.hcxt = TopMemoryContext;
	ob_mode_cache = hash_create("OverlayBranchModeCache",
								16, &ctl,
								HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

const char *
ob_mode_cache_lookup(int32 bid)
{
	OBModeHashEnt *ent;

	if (ob_mode_cache == NULL)
		return NULL;
	ent = (OBModeHashEnt *) hash_search(ob_mode_cache,
										&bid, HASH_FIND, NULL);
	return ent ? ent->mode : NULL;
}

void
ob_mode_cache_set(int32 bid, const char *mode)
{
	OBModeHashEnt *ent;
	bool	found;

	Assert(bid > 0);
	Assert(mode != NULL);
	ob_mode_cache_init_lazy();
	ent = (OBModeHashEnt *) hash_search(ob_mode_cache,
										&bid, HASH_ENTER, &found);
	strlcpy(ent->mode, mode, sizeof(ent->mode));
}

void
ob_mode_cache_remove(int32 bid)
{
	if (ob_mode_cache == NULL)
		return;
	(void) hash_search(ob_mode_cache, &bid, HASH_REMOVE, NULL);
}

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
/* ------------------------------------------------------------------
 * V3 snapshot lifecycle helpers
 * ------------------------------------------------------------------ */
void
ob_ensure_unregister_snapshot(BranchContext *ctx)
{
	int32		saved_bid;

	if (ctx == NULL)
		return;

	/* CRITICAL idempotency guard: reset flags BEFORE releasing ref,
	 * so any re-entrant / concurrent (e.g. GUC savepoint rollback
	 * re-check) call skips the unpin branch even if ERROR occurs
	 * below.  Note: snapshot allocation is now OWNED by
	 * ob_snapshot_cache (hash table), so we MUST NOT pfree here —
	 * only clear ctx pointer and decrement hash refcnt.  The real
	 * pfree happens only when apply/discard the branch
	 * (ob_snapshot_cache_remove) or during _PG_fini. */
	saved_bid = ctx->branch_id;
	ctx->branch_main_snapshot = NULL;
	if (!ctx->snapshot_registered)
		return;
	ctx->snapshot_registered = false;

	if (saved_bid > 0)
	{
		ob_snapshot_cache_unpin(saved_bid);
		elog(DEBUG2, "overlay_branch: unregistered MAIN snapshot ref (unpin bid=%d)",
			 (int) saved_bid);
	}
}


void
ob_exit_branch_cleanup(BranchContext *ctx)
{
	if (ctx == NULL)
		return;

	/* Idempotency: mark inactive BEFORE any cleanup work, so nested or
	 * re-entrant calls from GUC hooks / abort paths become no-ops
	 * (ensures single-shot cleanup even if ERROR interrupts the flow
	 * mid-way). */
	if (!ctx->is_active)
	{
		/* !active path: only clean any lingering snapshot ref that
		 * might have leaked through a prior ERROR mid-cleanup. */
		ob_ensure_unregister_snapshot(ctx);
		ctx->branch_id = 0;
		ctx->branch_name[0] = '\0';
		ctx->invalidation_counter = 0;
		ctx->invalidation_last_check = 0;
		return;
	}
	ctx->is_active = false;
	ob_ensure_unregister_snapshot(ctx);
	ctx->branch_id = 0;
	ctx->branch_name[0] = '\0';
	ctx->invalidation_counter = 0;
	ctx->invalidation_last_check = 0;
	elog(DEBUG1, "overlay_branch: left current branch (cleanup helper)");
}

static void
ob_install_snapshot_for_mode(BranchContext *ctx, const char *mode)
{
	Assert(ctx != NULL);
	Assert(mode != NULL);
	Assert(ctx->branch_id > 0 ||
		   (strcmp(mode, BRANCH_MODE_SNAPSHOT) != 0 &&
			strcmp(mode, BRANCH_MODE_LIVE) == 0));

	if (strcmp(mode, BRANCH_MODE_SNAPSHOT) == 0)
	{
		Snapshot	existing;

		/* --- V3 FR5 freeze semantics: once a branch is first entered
		 * in snapshot mode, its frozen MAIN snapshot is stored in the
		 * global bid-keyed hash. Subsequent re-entries (use_branch('',
		 * snapshot) on the SAME bid) MUST reuse the original capture
		 * — re-running GetTransactionSnapshot would pick up MAIN rows
		 * committed between session entries and violate "frozen at t0".
		 *
		 * N4 re-entry (use twice in a row without exit): caller
		 * (use_with_mode_internal L417) calls
		 * ob_ensure_unregister_snapshot BEFORE reaching here, which
		 * decrements refcnt but leaves the hash entry intact (only
		 * apply/discard removes it). So first use inserts with
		 * refcnt=0; second use → unpin → refcnt back to 0; then we
		 * arrive here and find HASH HIT → reuse SAME snapshot.
		 * Correct! */
		existing = ob_snapshot_cache_lookup(ctx->branch_id);
		if (existing != NULL)
		{
			/* HASH HIT: re-install SAME frozen snapshot (not a copy). */
			ctx->branch_main_snapshot = existing;
			ctx->snapshot_registered = true;
			ob_snapshot_cache_pin(ctx->branch_id);
			elog(DEBUG1, "overlay_branch: snapshot REUSE (cache hit bid=%d) xmin=%u",
				 (int) ctx->branch_id, (uint32) existing->xmin);
			return;
		}

		/* HASH MISS: first-ever snapshot-mode entry for this bid →
		 * capture once & store in hash as canonical frozen view. */
		{
			Snapshot cur_snap;
			Snapshot copied;
			MemoryContext oldcxt;
			Size subxipoff;
			Size size_alloc;

			/* PERFORMANCE TODO (V3.x): long-lived snapshot holds back
			 * xmin horizon / VACUUM. Correctness first; add max-hold
			 * GUC + HINT later.
			 *
			 * Do NOT use RegisterSnapshot() / UnregisterSnapshot()
			 * here: RegisterSnapshot binds to CurrentResourceOwner
			 * (= the SQL callable's Portal owner) and auto-releases
			 * at statement end, or CurTransactionResourceOwner
			 * auto-drops on COMMIT/ROLLBACK — both wrong for a
			 * snapshot branch that must outlive many tx until apply/
			 * discard. We hash-cache with manual lifetime. */
			cur_snap = GetTransactionSnapshot();
			size_alloc = subxipoff = sizeof(SnapshotData) +
				cur_snap->xcnt * sizeof(TransactionId);
			if (cur_snap->subxcnt > 0)
				size_alloc += cur_snap->subxcnt * sizeof(TransactionId);
			oldcxt = MemoryContextSwitchTo(TopMemoryContext);
			copied = (Snapshot) palloc(size_alloc);
			memcpy(copied, cur_snap, sizeof(SnapshotData));
			/* ---- V3 CRITICAL REGISTRATION PSEUDO-PIN (T5 踩坑 fix)
			 *
			 * PG PopActiveSnapshot() [snapmgr.c L753-755] frees the
			 * Snapshot (via FreeSnapshot) when BOTH
			 * active_count==0 AND regd_count==0.  CopySnapshot above
			 * left both 0 → first push→pop around Pass1 MAIN SELECT
			 * would free our shared hash-owned snapshot WHILE IT IS
			 * STILL BEING REFERENCED by other concurrent sessions.
			 * Setting regd_count=1 masks as "persistently registered"
			 * so PopActiveSnapshot never auto-deallocates; real pfree
			 * happens only at apply/discard in
			 * ob_snapshot_cache_remove(). */
			copied->regd_count = 1;
			copied->active_count = 0;
			copied->copied = true;
			copied->snapXactCompletionCount = 0;
			if (cur_snap->xcnt > 0)
			{
				copied->xip = (TransactionId *) (copied + 1);
				memcpy(copied->xip, cur_snap->xip,
					cur_snap->xcnt * sizeof(TransactionId));
			}
			else
				copied->xip = NULL;
			if (cur_snap->subxcnt > 0 &&
				(!cur_snap->suboverflowed || cur_snap->takenDuringRecovery))
			{
				copied->subxip = (TransactionId *) ((char *) copied + subxipoff);
				memcpy(copied->subxip, cur_snap->subxip,
					cur_snap->subxcnt * sizeof(TransactionId));
			}
			else
				copied->subxip = NULL;
			MemoryContextSwitchTo(oldcxt);

			ob_snapshot_cache_insert(ctx->branch_id, copied);
			ctx->branch_main_snapshot = copied;
			ctx->snapshot_registered = true;
			ob_snapshot_cache_pin(ctx->branch_id);
			elog(DEBUG1, "overlay_branch: snapshot FIRST CAPTURE (bid=%d) xmin=%u",
				 (int) ctx->branch_id, (uint32) copied->xmin);
		}
	}
	else
	{
		ctx->branch_main_snapshot = NULL;
		ctx->snapshot_registered = false;
	}
}


void
overlay_branch_use_internal(const char *branch_name)
{
	/* V3 T8 contract: 1-arg use_branch(name) (explicit SQL call) ALWAYS
	 * resolves mode = 'live' regardless of any prior session-local mode
	 * cache entry.  Mode-preserve applies ONLY to the GUC RESTORE path
	 * (assign hook overlay_branch.c), which calls
	 * use_with_mode_internal(name, cached_mode) directly.  This matches
	 * PART F2/F1 semantics: 1-param = live. */
	overlay_branch_use_with_mode_internal(branch_name, BRANCH_MODE_LIVE);
}


void
overlay_branch_use_with_mode_internal(const char *branch_name, const char *mode)
{
	if (mode == NULL || *mode == '\0')
		mode = BRANCH_MODE_LIVE;

	if (strcmp(mode, BRANCH_MODE_LIVE) != 0 &&
		strcmp(mode, BRANCH_MODE_SNAPSHOT) != 0)
	{
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("invalid branch mode \"%s\"", mode),
			 errhint("Supported modes: 'live' (default), 'snapshot' (frozen MAIN MVCC view).")));
	}

	if (branch_name == NULL || *branch_name == '\0')
	{
		/* I8.2: save cached mode BEFORE clearing current, so that if we are
		 * on an ACTIVE branch its last-used mode survives GUC RESTORE +
		 * re-entry (which calls ob_mode_cache_lookup → cache wins over
		 * 'live' default). */
		if (CurrentBranchContext && CurrentBranchContext->is_active)
		{
			ob_mode_cache_set(CurrentBranchContext->branch_id,
							  CurrentBranchContext->mode);
			ob_exit_branch_cleanup(CurrentBranchContext);
		}
		{
			bool saved_flag = ob_in_guc_setconfig;
			ob_in_guc_setconfig = true;
			SetConfigOption("overlay_branch.current", "",
						PGC_USERSET, PGC_S_SESSION);
			ob_in_guc_setconfig = saved_flag;
		}
		return;
	}

	{
		int ret;
		char *esc_name;
		StringInfoData sql;
		int32 real_branch_id = 0;

		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "overlay_branch: SPI_connect() failed in use_branch");

		esc_name = quote_literal_cstr(branch_name);
		initStringInfo(&sql);

		resetStringInfo(&sql);
		appendStringInfo(&sql,
				 "SELECT 1 FROM " OBTABLE_BRANCH " WHERE branch_name = %s LIMIT 1",
				 esc_name);
		ret = SPI_execute(sql.data, true, 1);
		if (ret != SPI_OK_SELECT || SPI_processed == 0)
		{
			SPI_finish();
			if (SPI_processed == 0)
				ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("branch \"%s\" does not exist", branch_name)));
			else
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("overlay_branch: SPI existence check failed ret=%d", ret)));
		}

		resetStringInfo(&sql);
		appendStringInfo(&sql,
				 "SELECT 1 FROM " OBTABLE_BRANCH " "
				 "WHERE branch_name = %s AND state = '%s' LIMIT 1",
				 esc_name, BRANCH_STATE_ACTIVE);
		ret = SPI_execute(sql.data, true, 1);
		if (ret != SPI_OK_SELECT || SPI_processed == 0)
		{
			SPI_finish();
			if (SPI_processed == 0)
				ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("branch \"%s\" is not active", branch_name),
					 errhint("Create a fresh branch, or use DISCARD BRANCH to clean up.")));
			else
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("overlay_branch: SPI state check failed ret=%d", ret)));
		}

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
				 errmsg("overlay_branch: could not fetch branch_id for '%s' SPI ret=%d n=%lu",
					branch_name, ret, (unsigned long) SPI_processed)));
		}

		{
			bool isnull;
			real_branch_id = DatumGetInt32(
				SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc,
						 1, &isnull));
			if (isnull)
				elog(ERROR, "overlay_branch: branch_id for '%s' is NULL", branch_name);
		}

		/* V3 FR2 V1 (I8.1 L1 2026-09-17 dry-run fix): use_branch NO LONGER
		 * takes SHARED advisory lock.
		 *
		 * Rationale (locked in multi_session_mvcc.md § B.9.1 I8.1):
		 * Old V0: use=SHARED, apply=EXCLUSIVE → two sessions
		 *   S1=SHARED + S2=SHARED then S1=EXCLUSIVE-upgrade + S2=EXCLUSIVE-upgrade
		 *   = PostgreSQL classic shared→exclusive upgrade DEADLOCK 40P01.
		 * V1 fix: (a) use_branch = NO LOCK (just read catalog + install GUC/snapshot).
		 *          (b) apply_branch/discard_branch = FIRST run FR3 CAS
		 *              (UPDATE pg_branch SET state={applying,discarding} WHERE state='active'
		 *               — row-level UPDATE itself is 1-winner atomic CAS;
		 *               losers SPI_processed = 0 → kick out with ERROR immediately, no waiting).
		 *          (c) ONLY after CAS winner, take EXCLUSIVE advisory for the
		 *              duration of MAIN merge + catalog ops (single lock level
		 *              no upgrade → no deadlock possible).
		 * Deadlock freedom proof: lock graph has shared→exclusive upgrade edges removed;
		 * every lock transition is nolock→exclusive → wait-for graph is a forest of
		 * DAGs → cycles impossible (§ B.9.1 I8.1).
		 */

		if (CurrentBranchContext == NULL)
			CurrentBranchContext = MemoryContextAllocZero(TopMemoryContext,
									sizeof(BranchContext));
		else
			ob_ensure_unregister_snapshot(CurrentBranchContext);

		CurrentBranchContext->branch_id = real_branch_id;
		strlcpy(CurrentBranchContext->branch_name, branch_name, NAMEDATALEN);
		CurrentBranchContext->owner = GetUserId();
		CurrentBranchContext->is_active = true;
		strncpy(CurrentBranchContext->mode, mode, sizeof(CurrentBranchContext->mode) - 1);
		CurrentBranchContext->mode[sizeof(CurrentBranchContext->mode) - 1] = '\0';
		CurrentBranchContext->created_at = 0;
		CurrentBranchContext->invalidation_counter = 0;
		CurrentBranchContext->invalidation_last_check = 0;

		ob_install_snapshot_for_mode(CurrentBranchContext, mode);

		/* I8.2: persist bid→mode to session-local cache AFTER all state
		 * installs but BEFORE SetConfigOption so that if SetConfigOption
		 * triggers nested assign-hook re-entry (common on GUC RESTORE
		 * paths) the nested use_internal → mode_cache_lookup returns the
		 * mode we're ABOUT to commit, not some stale prior value.  Cache
		 * write here replaces catalog UPDATE (RowExclusiveLock removed). */
		ob_mode_cache_set(real_branch_id, mode);

		{
			bool saved_flag = ob_in_guc_setconfig;
			ob_in_guc_setconfig = true;
			SetConfigOption("overlay_branch.current", branch_name,
						PGC_USERSET, PGC_S_SESSION);
			ob_in_guc_setconfig = saved_flag;
		}
		SPI_finish();
		elog(DEBUG1, "overlay_branch_use_with_mode_internal: name='%s' mode='%s'",
			 branch_name, mode);
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
	extern bool overlay_branch_enabled;

	/* V3 FR4: run throttled invalidation check on every public "are we on
	 * a branch?" guard.  DQL path only (for_dml=false) so the caller —
	 * BranchScan, rel_ok, ddl_ok, planner shortcuts — all get a soft
	 * NOTICE + MAIN-fallback kickout.  The DML hard-ERROR path is
	 * triggered *separately* at write_redirect ExecutorRun entry and
	 * overlay_delta_insert safety net with for_dml=true.
	 *
	 * The throttled() call below itself carries ob_in_invalidation_check
	 * depth-1 recursion guard (include/overlay_branch.h L115 extern,
	 * overlay_branch.c L95 definition), so even if the SPI ground truth
	 * inside throttled re-enters PG's planner which re-calls THIS
	 * function, the inner invocation skips the SPI heavy block via the
	 * flag and just returns the pure-memory truth check.  See
	 * test/sql/overlay_branch_invalidation.sql Sections C/D/J. */
	(void) ob_invalidate_check_throttled(false);

	/* Fall-back to pure-memory truth check after kickout; if the
	 * throttled check decided the branch is dead, ctx->is_active is
	 * already false so this returns false. */
	return (overlay_branch_enabled &&
			CurrentBranchContext != NULL &&
			CurrentBranchContext->is_active &&
			overlay_branch_current_name != NULL &&
			*overlay_branch_current_name != '\0');
}

/* ----------
 * ob_broadcast_state_change — FR1 fast-path NOTIFY
 *
 * Payload format: "<int32 bid>:<new_state>"
 * Channel:       OB_NOTIFY_CHANNEL ("ob_branch_state")
 *
 * Called *immediately after* the catalog UPDATE + cascade (delta rows
 * DELETE) succeed, i.e. AFTER ground truth in pg_branch has flipped.
 * Receivers drain via AcceptInvalidationMessages() (sinval shared bus)
 * on their next ob_invalidate_check_throttled() call.
 * ----------
 */
void
ob_broadcast_state_change(int32 bid, const char *new_state)
{
	char   *payload;

	if (bid <= 0 || new_state == NULL || *new_state == '\0')
		return;

	payload = psprintf("%d:%s", (int) bid, new_state);
	Async_Notify(OB_NOTIFY_CHANNEL, payload);
	pfree(payload);

	elog(DEBUG1, "overlay_branch: broadcast bid=%d state=%s channel=%s",
		 (int) bid, new_state, OB_NOTIFY_CHANNEL);
}

/* ----------
 * ob_invalidate_check_throttled — FR4: counter + wall-clock + sinval
 * combined invalidation trigger.
 *
 * Three independent "run heavy SPI ground truth" conditions (OR):
 *   (B) ctx->invalidation_counter >= ob_invalidation_check_threshold
 *       (default 32 — L3 pg_regress can force this via
 *       force_invalidation_check() by bumping the counter)
 *   (C) wall-clock: (now - invalidation_last_check) >
 *       ob_invalidation_check_interval_ms ms (default 100 ms)
 *   (D) sinval drain: every call drains AcceptInvalidationMessages().
 *       NOTIFY via shared bus also re-checks so stale state never
 *       survives a sinval delivery.
 *
 * SPI ground truth: SELECT state FROM pg_branch WHERE branch_id = X AND
 * branch_name = Y LIMIT 1 (bid + name to prevent name reuse collision).
 *
 * @for_dml = split decision (FR1 + FR4):
 *   false → DQL guard: state != active → ereport(NOTICE) +
 *           ob_exit_branch_cleanup(ctx) + set overlay_branch.current='' +
 *           return false.  (Callers fall through to MAIN reads.)
 *   true  → DML guard: state != active → ereport(ERROR,
 *           ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE) + cleanup.
 *           (Drift-writes into MAIN are 100% disallowed.)
 *
 * Returns true iff branch is still active (either stale-true or just
 * verified true); false iff kickout happened.
 * ----------
 */
bool
ob_invalidate_check_throttled(bool for_dml)
{
	BranchContext *ctx;
	bool		need_heavy_check;
	TimestampTz now;
	bool		saved_flag;
	bool		result;
	bool		kicked_out = false;
	bool		local_is_applied = false;
	bool		local_is_discarded = false;
	char		local_state_buf[32];
	char		local_branch_name[NAMEDATALEN];
	const char *local_what_happened;

	local_state_buf[0] = '\0';
	local_branch_name[0] = '\0';
	local_what_happened = "invalidated";

	/* ---- Depth-1 recursion guard.  ANY caller path may re-enter here via
	 *   throttled → SPI_execute(pg_branch) → planner → is_active → throttled.
	 * Return the in-memory ctx->is_active snapshot; the outer call will do
	 * the real SPI work once it unwinds.  See long comment in the previous
	 * committed version for the full rationale (kept in git history).
	 * ---- */
	/* Fast path: nothing to throttle if not on an active branch. */
	if (ob_in_invalidation_check)
	{
		if (CurrentBranchContext == NULL)
			return false;
		return CurrentBranchContext->is_active;
	}

	/* Fast path: nothing to throttle if not on an active branch. */
	if (CurrentBranchContext == NULL || !CurrentBranchContext->is_active)
		return false;

	ctx = CurrentBranchContext;
	need_heavy_check = false;
	now = 0;
	result = true;

	/* — Outer exception safety: raise the guard flag and make sure it is
	 * always lowered on every exit path (normal-return / ereport /
	 * longjmp from PG_CATCH sub-blocks). */
	saved_flag = ob_in_invalidation_check;
	ob_in_invalidation_check = true;
	PG_TRY();
	{
		/* (D) Always drain the shared invalidation + NOTIFY bus first.
		 * Delivers cross-session ob_branch_state notifications too. */
		AcceptInvalidationMessages();

		/* (A) Count-based throttle.  Increment BEFORE compare so
		 * threshold=1 fires on the very first call (matches §J test).
		 *
		 * I8.3: Allow L1 _debug_invalidation_counter_throttle_override to
		 * force a smaller-than-default count (e.g. 2) so ob_state_inval
		 * spec permutations don't need 32 calls to force the kickout. */
		{
			uint32 effective_threshold;
			if (ob_debug_invalidation_counter_throttle_override >= 0)
				effective_threshold =
					(uint32) ob_debug_invalidation_counter_throttle_override;
			else
				effective_threshold =
					(uint32) ob_invalidation_check_threshold;
			if (effective_threshold > 0)
			{
				ctx->invalidation_counter++;
				if (ctx->invalidation_counter >= effective_threshold)
					need_heavy_check = true;
			}
		}

		/* (B) Wall-clock throttle.  First-call (last_check==0) always
		 * fires once to establish the baseline timestamp. */
		now = GetCurrentTimestamp();
		if (ob_invalidation_check_interval_ms > 0)
		{
			long	secs;
			int		usecs;
			long	interval_ms;

			if (ctx->invalidation_last_check == 0)
			{
				need_heavy_check = true;
			}
			else
			{
				TimestampDifference(ctx->invalidation_last_check, now,
									&secs, &usecs);
				interval_ms = secs * 1000L + (long) (usecs / 1000);
				if (interval_ms >= ob_invalidation_check_interval_ms)
					need_heavy_check = true;
			}
		}

		/* Fast path: stale-true return.  Both throttle gates say "not
		 * yet" — the optimistic contract of FR4. */
		if (!need_heavy_check)
		{
			result = true;
			goto throttle_clean_exit;
		}

		/* ==========  Heavy SPI ground-truth  ========== */
		{
			StringInfoData sql;
			int			ret;
			char	   *esc_name;
			bool		still_active = false;
			bool		got_row = false;
			int			spi_connect_ret;
			bool		did_spi_connect = false;

			spi_connect_ret = SPI_connect();
			if (spi_connect_ret == SPI_OK_CONNECT)
				did_spi_connect = true;
			else if (spi_connect_ret != SPI_ERROR_CONNECT)
				elog(ERROR, "overlay_branch: throttled SPI_connect "
					 "failed ret=%d", spi_connect_ret);

			ctx->invalidation_counter = 0;
			ctx->invalidation_last_check = now;

			esc_name = quote_literal_cstr(ctx->branch_name);
			initStringInfo(&sql);
			appendStringInfo(&sql,
							 "SELECT state FROM " OBTABLE_BRANCH " "
							 "WHERE branch_id = %d AND branch_name = %s LIMIT 1",
							 (int) ctx->branch_id, esc_name);
			pfree(esc_name);

			overlay_overlay_helper_enter();
			ret = SPI_execute(sql.data, true, 1);
			pfree(sql.data);

			if (ret == SPI_OK_SELECT && SPI_processed == 1)
			{
				bool		isnull;
				Datum		d;

				d = SPI_getbinval(SPI_tuptable->vals[0],
								  SPI_tuptable->tupdesc, 1, &isnull);
				if (!isnull)
				{
					char   *st = TextDatumGetCString(d);

					got_row = true;
					if (strcmp(st, BRANCH_STATE_ACTIVE) == 0)
						still_active = true;
					pfree(st);
				}
			}
			overlay_overlay_helper_exit();

			if (did_spi_connect)
			{
				SPI_finish();
				did_spi_connect = false;
			}

			if (still_active)
			{
				elog(DEBUG2, "overlay_branch: throttled verified "
					 "bid=%d name='%s' active=true",
					 (int) ctx->branch_id, ctx->branch_name);
				result = true;
				goto throttle_clean_exit;
			}

			/* ===== Kickout path: ground truth says state != active ===== */
			strcpy(local_state_buf, got_row ? "<known>" : "missing");

			if (got_row)
			{
				StringInfoData sq2;
				int			r2;
				char	   *esc2;
				int			reread_spi_ret;
				bool		reread_did_connect = false;

				reread_spi_ret = SPI_connect();
				if (reread_spi_ret == SPI_OK_CONNECT)
					reread_did_connect = true;
				else if (reread_spi_ret != SPI_ERROR_CONNECT)
					elog(ERROR, "overlay_branch: kickout reread "
						 "SPI_connect ret=%d", reread_spi_ret);

				esc2 = quote_literal_cstr(ctx->branch_name);
				initStringInfo(&sq2);
				appendStringInfo(&sq2,
								 "SELECT state FROM " OBTABLE_BRANCH " "
								 "WHERE branch_id = %d AND branch_name = %s LIMIT 1",
								 (int) ctx->branch_id, esc2);
				pfree(esc2);

				overlay_overlay_helper_enter();
				r2 = SPI_execute(sq2.data, true, 1);
				pfree(sq2.data);
				if (r2 == SPI_OK_SELECT && SPI_processed == 1)
				{
					bool	isnull2;
					Datum	d2;

					d2 = SPI_getbinval(SPI_tuptable->vals[0],
									   SPI_tuptable->tupdesc, 1,
									   &isnull2);
					if (!isnull2)
					{
						char   *st = TextDatumGetCString(d2);

						strncpy(local_state_buf, st,
								sizeof(local_state_buf) - 1);
						local_state_buf[sizeof(local_state_buf) - 1] = '\0';
						if (strcmp(st, BRANCH_STATE_APPLIED) == 0)
							local_is_applied = true;
						else if (strcmp(st, BRANCH_STATE_DISCARDED) == 0)
							local_is_discarded = true;
						pfree(st);
					}
				}
				overlay_overlay_helper_exit();
				if (reread_did_connect)
					SPI_finish();
			}

			/* --- Planner-hook / observer suppression gate (extended) ---
			 * The heavy SPI check has determined the branch is
			 * invalid, but WE MUST NOT DO THE DESTRUCTIVE CLEANUP
			 * SIDE-EFFECTS here unless the caller is the ONE entry
			 * point that explicitly opted in (see write_redirect.c
			 * ExecutorRun hook, which sets ob_throttled_allow_kickout
			 * when it calls throttled with dml_split already decided).
			 *
			 * Rationale — ordering race resolved by this gate:
			 *
			 * Consider an UPDATE statement against a user table whose
			 * branch context has stale `is_active=true` (another
			 * session flipped state to applied/discarded while we
			 * weren't looking).  With FR4 throttling threshold=1
			 * (user-settable), EVERY call to throttled() passes the
			 * counter gate, including "observer" calls that do SPI
			 * SELECTs on pg_branch (e.g. our own heavy-check internal
			 * SPI re-read, or the planner hook re-planning a
			 * trigger-side query, or rel_ok/ddl_ok guards examining
			 * the target RTE before the actual ModifyTable
			 * ExecutorRun hook does its dml_split-aware check).
			 *
			 * If any observer call with for_dml=false actually ran
			 * ob_exit_branch_cleanup() + cleared the GUC + emitted a
			 * NOTICE BEFORE the real ModifyTable ExecutorRun called
			 * throttled(for_dml=true), the DML ERROR split would
			 * NEVER fire (the L565 fast-path sees ctx.is_active=false
			 * and returns false immediately, fall-through writes
			 * silent MAIN pollution).
			 *
			 * D7 Section of test/sql/overlay_branch_invalidation.sql
			 * is the exact deterministic reproduction.
			 *
			 * The observer suppression gate therefore extends the
			 * planner-hook-only suppression (ob_in_planner_hook) to a
			 * broader, caller-opted-in model: ONLY the caller that
			 * has ALREADY committed to a specific dml_split outcome
			 * — write_redirect ExecutorRun — sets `ob_throttled_allow_
			 * kickout = true` around the call.  Every other caller
			 * (planner, rel_ok, ddl_ok, the is_active_by_name(name)
			 * synonym, force_invalidation_check SQL wrapper, Process
			 * Utility command guards) just gets the boolean truth
			 * WITHOUT destructive state mutation.  The actual cleanup
			 * + NOTICE / ERROR + GUC reset is deferred to the next
			 * opted-in ExecutorRun call, where CmdType is known so
			 * the DQL vs DML split can choose the correct NOTICE vs
			 * ERROR + HINT.
			 *
			 * Note: force_invalidation_check() is special: it MUST
			 * still kick the user off the branch for UX (user
			 * explicitly said "check NOW").  That call site is opted
			 * in by wrapping its throttled(false) call in allow_
			 * kickout=true.  See overlay_branch.c L1000. */
			if (!ob_throttled_allow_kickout)
			{
				result = false;
				kicked_out = false;
				goto throttle_clean_exit;
			}

			/* Save kickout inputs from ctx BEFORE cleanup zeroes it. */
			strncpy(local_branch_name, ctx->branch_name, NAMEDATALEN - 1);
			local_branch_name[NAMEDATALEN - 1] = '\0';
			local_what_happened = local_is_applied ? "applied" :
								  local_is_discarded ? "discarded"
												 : "invalidated";

			/* Planner-hook suppression (kept as dead-code but safe:
			 * ob_in_planner_hook implies allow_kickout is NOT set,
			 * so we bailed above.  Retained so future reordering of
			 * the code cannot accidentally expose the cleanup to
			 * planner paths even if someone removes the flag. */
			if (ob_in_planner_hook)
			{
				result = false;
				kicked_out = false;
				goto throttle_clean_exit;
			}

			ob_exit_branch_cleanup(ctx);

			{
				bool s2 = ob_in_guc_setconfig;
				ob_in_guc_setconfig = true;
				SetConfigOption("overlay_branch.current", "",
								PGC_USERSET, PGC_S_SESSION);
				ob_in_guc_setconfig = s2;
			}

			kicked_out = true;
			result = false;
		}
throttle_clean_exit:	;
	}
	PG_CATCH();
	{
		ob_in_invalidation_check = saved_flag;
		PG_RE_THROW();
	}
	PG_END_TRY();
	ob_in_invalidation_check = saved_flag;

	if (kicked_out)
	{
		/* EREPORT after lowering the guard flag and after PG_TRY so
		 * ereport's longjmp (ERROR) cannot see a stale guard flag. */
		if (for_dml)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("overlay_branch: branch \"%s\" is no longer "
							"active (state=%s); cannot write through it",
							local_branch_name, local_state_buf),
					 errhint("The branch was %s by another session.  "
							 "Re-connect to MAIN and retry the write.",
							 local_what_happened)));
		}
		else
		{
			ereport(NOTICE,
					(errmsg("overlay_branch: branch \"%s\" is no longer "
							"active (state=%s); reverting to Main",
							local_branch_name, local_state_buf)));
			return false;
		}
	}
	return result;
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

	/* V3 FR2: take branch-level EXCLUSIVE advisory lock BEFORE any catalog
	 * UPDATE / RowExclusiveLock on user tables, so we strictly follow
	 * Advisory → (catalog locks + user RowExclusiveLock) deadlock order.
	 * With ExclusiveLock held, the ACTIVE state check below and the later
	 * UPDATE pg_branch state→applied / DELETE pg_branch_delta are fully
	 * serialised vs any concurrent apply/discard on the same bid. */
	if (bid <= 0)
		elog(ERROR, "overlay_branch: apply found invalid branch_id=%d", bid);
	ob_take_branch_advisory_lock(bid, ExclusiveLock);

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

	/* V3 FR3: CAS transition to transient state='applying' BEFORE any 3-pass writes, to
	 * guarantee no other session can sneak in a delta-write while we are halfway
	 * between D-pass and I-pass (which would cause silent data loss — see
	 * multi_session_mvcc.md § D.8.1).
	 *
	 * Note: we already hold EXCLUSIVE advisory lock on bid (FR2) so no real
	 * concurrent apply/discard can race with us — still double guard (atomic CAS) ensures correctness.
	 */
	{
		StringInfoData cas_sql;
		int			cas_ret_local;

		cas_ret_local = 0;
		initStringInfo(&cas_sql);
		appendStringInfo(&cas_sql,
						 "UPDATE " OBTABLE_BRANCH " "
						 "SET state = '%s' WHERE branch_id = %d "
						 "AND state = '%s'",
						 BRANCH_STATE_APPLYING,
						 (int) bid,
						 BRANCH_STATE_ACTIVE);
		cas_ret_local = ob_spi_one_shot(cas_sql.data, false, 0);
		if (cas_ret_local != SPI_OK_UPDATE &&
			cas_ret_local != SPI_OK_UPDATE_RETURNING)
		{
			/* ob_spi_one_shot leaves SPI connected on non-fatal returns;
			 * finish it before ereport to balance connect count */
			SPI_finish();
			pfree(cas_sql.data);
			if (state) pfree(state);
			pfree(esc_name);
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("overlay_branch: apply_branch(\"%s\") failed to transition to \"%s\" state",
							branch_name, BRANCH_STATE_APPLYING),
					 errhint("Concurrent state mutation; branch may no longer be ACTIVE.")));
		}
		if (SPI_processed != 1)
		{
			SPI_finish();
			pfree(cas_sql.data);
			if (state) pfree(state);
			pfree(esc_name);
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("overlay_branch: apply_branch(\"%s\") CAS state transition refused"
							" (SPI_processed=%lu != 1); branch no longer ACTIVE)",
							branch_name, (unsigned long) SPI_processed),
					 errhint("Branch state was changed externally between SELECT and UPDATE.")));
		}
		SPI_finish();
		pfree(cas_sql.data);
	}

	/* I8.4: Debug apply-state window — widen the transient applying
	 * state for L1 ob_applying_freeze.spec.  After CAS-winner moved us
	 * into 'applying' state (above), pg_sleep(N) so s2_insert_delta can
	 * enter and hit the state guard while CAS winner is still holding
	 * EXCLUSIVE advisory.  0 = production default, no sleep. */
	if (ob_debug_apply_sleep_sec > 0)
	{
		StringInfoData slp;
		int slpret;
		initStringInfo(&slp);
		appendStringInfo(&slp, "SELECT pg_sleep(%d)",
						 ob_debug_apply_sleep_sec);
		if (SPI_connect() == SPI_OK_CONNECT)
		{
			slpret = SPI_execute(slp.data, false, 0);
			SPI_finish();
			(void) slpret;
		}
		pfree(slp.data);
	}

	/* Step 2: kick user out of this session branch (if on it) */
	if (CurrentBranchContext &&
		CurrentBranchContext->is_active &&
		strncmp(CurrentBranchContext->branch_name, branch_name,
				NAMEDATALEN) == 0)
	{
		leaving_current = true;
		ob_exit_branch_cleanup(CurrentBranchContext);
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
						 BRANCH_STATE_APPLYING);
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

	ob_broadcast_state_change(bid, BRANCH_STATE_APPLIED);
	/* V3 FR5: branch is APPLIED now — evict its frozen MAIN snapshot
	 * from the per-bid hash cache (if any) and free the TopMC
	 * CopySnapshot chunk.  After apply, bid no longer ACTIVE so no
	 * future snapshot-mode entry can reference it.  Safe if cache
	 * entry didn't exist (LIVE branches never capture one). */
	ob_snapshot_cache_remove(bid);
	/* I8.2: symmetric mode-cache cleanup.  After apply, bid → 'applying'
	 * then 'applied' state, so any future re-create would insert a new
	 * catalog row with mode='live' default.  Removing the stale session-
	 * local bid mode avoids cache collisions. */
	ob_mode_cache_remove(bid);


	if (leaving_current)
	{
		bool saved_flag = ob_in_guc_setconfig;
		ob_in_guc_setconfig = true;
		SetConfigOption("overlay_branch.current", "",
						PGC_USERSET, PGC_S_SESSION);
		ob_in_guc_setconfig = saved_flag;
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

	/* V3 FR2: take branch-level EXCLUSIVE advisory lock BEFORE the first
	 * catalog UPDATE, so discard serialises vs concurrent apply/discard
	 * on same bid AND obeys Advisory → catalog RowExclusiveLock order
	 * (prevents deadlock). */
	if (bid <= 0)
		elog(ERROR, "overlay_branch: discard found invalid branch_id=%d", bid);
	ob_take_branch_advisory_lock(bid, ExclusiveLock);

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

	/* FR1: ALWAYS broadcast DISCARDED state after catalog UPDATE +
	 * delta CASCADE succeed — other sessions may be LISTENing on this
	 * branch via NOTIFY even if *this* session isn't currently on it. */
	ob_broadcast_state_change(bid, BRANCH_STATE_DISCARDED);
	/* V3 FR5: branch DISCARDED — evict frozen snapshot cache for bid
	 * (if any) and free TopMC CopySnapshot.  After discard, bid no
	 * longer ACTIVE so no future snapshot-mode reference possible. */
	ob_snapshot_cache_remove(bid);
	/* I8.2: symmetric per-bid mode cache cleanup.  Discard moves bid to
	 * state='discarded'.  Any future recreate would INSERT a new catalog
	 * row with mode='live' default; dropping the session-local cached
	 * mode avoids overlaying a stale prior-mode on that fresh row. */
	ob_mode_cache_remove(bid);


	/* 4) If user is on this branch, revert to Main */
	if (CurrentBranchContext &&
		CurrentBranchContext->is_active &&
		strncmp(CurrentBranchContext->branch_name, branch_name,
				NAMEDATALEN) == 0)
	{
		leaving_current = true;
		ob_exit_branch_cleanup(CurrentBranchContext);
		{
			bool saved_flag = ob_in_guc_setconfig;
			ob_in_guc_setconfig = true;
			SetConfigOption("overlay_branch.current", "",
							PGC_USERSET, PGC_S_SESSION);
			ob_in_guc_setconfig = saved_flag;
		}
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
