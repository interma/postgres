/*-------------------------------------------------------------------------
 *
 * overlay_branch.h
 *	  Type and function declarations for the Overlay Branch extension.
 *
 *-------------------------------------------------------------------------
 */
#ifndef OVERLAY_BRANCH_H
#define OVERLAY_BRANCH_H

#include "postgres.h"
#include "miscadmin.h"
#include "datatype/timestamp.h"
#include "executor/tuptable.h"
#include "nodes/pg_list.h"
#include "utils/relcache.h"
#include "storage/lmgr.h"
#include "storage/lockdefs.h"
#include "storage/lock.h"

/* ----------
 * Branch modes
 * ----------
 */
#define BRANCH_MODE_LIVE		"live"
#define BRANCH_MODE_SNAPSHOT	"snapshot"

/* ----------
 * V3 FR2: Branch-level Advisory Lock magic key.
 *
 * int64 key layout (exactly matches PG's native SET_LOCKTAG_ADVISORY form
 * used by pg_advisory_xact_lock(int8)):
 *   bits 63..48  = 0x4F42 = magic prefix 'O' 'B'  ("overlay branch")
 *   bits 47..32  = reserved, set 0
 *   bits 31..0   = uint32 view of int32 branch_id (from pg_branch.branch_id)
 *
 * Collision probability with user-generated advisory keys is ~1/2^16 since
 * ordinary keys almost never carry high bits 0x4F42.
 *
 * We locally re-define SET_LOCKTAG_INT64 here (mirroring lockfuncs.c L613)
 * because that macro is a file-local helper in the backend, not exported
 * through headers.
 * ----------
 */
#define OB_ADVISORY_MAGIC_HI16  ((uint64) 0x4F42ULL << 48)
#define OB_MAKE_ADVISORY_KEY(bid)  \
	(OB_ADVISORY_MAGIC_HI16 | ((uint64) ((uint32) (bid))))

#ifndef SET_LOCKTAG_INT64
#define SET_LOCKTAG_INT64(tag, key64) \
	SET_LOCKTAG_ADVISORY(tag, \
						 MyDatabaseId, \
						 (uint32) ((key64) >> 32), \
						 (uint32) (key64), \
						 1)
#endif

/* ----------
 * FR2 helpers: take/release xact-scoped advisory lock on a branch_id.
 *
 * Use sessionLock = false so the lock is tied to the current transaction
 * (auto-released on COMMIT / ABORT, never leaks).
 * dontWait       = false so callers block until granted (or the deadlock
 * detector picks a victim with ERRCODE 40P01, which is the safe behaviour).
 *
 * Same-backend same-xact ShareLock → ExclusiveLock upgrade works correctly in PG
 * (promoted without wait, but we document Section L tests to cover this path).
 * ----------
 */
static inline void
ob_take_branch_advisory_lock(int32 bid, LOCKMODE lockmode)
{
	int64		key = (int64) OB_MAKE_ADVISORY_KEY(bid);
	LOCKTAG		tag;

	if (bid <= 0)
		elog(ERROR,
			 "overlay_branch: invalid branch_id=%d for advisory lock",
			 bid);
	SET_LOCKTAG_INT64(tag, key);
	(void) LockAcquire(&tag, lockmode, false, false);
}

static inline void
ob_release_branch_advisory_lock(int32 bid, LOCKMODE lockmode)
{
	int64		key = (int64) OB_MAKE_ADVISORY_KEY(bid);
	LOCKTAG		tag;

	if (bid <= 0)
		return;
	SET_LOCKTAG_INT64(tag, key);
	(void) LockRelease(&tag, lockmode, false);
}

/* ----------
 * Fully qualified schema (control file pins schema='overlay_branch',
 * non-relocatable).  Exposed so branch_scan.c Planner hook can skip
 * overlay_branch-owned catalog tables (pg_branch, pg_branch_delta)
 * which would otherwise cause infinite recursion via SPI lookups.
 * ----------
 */
#define OBSCHEMA				"overlay_branch"

/* ----------
 * Branch states
 * ----------
 */
#define BRANCH_STATE_ACTIVE	"active"
#define BRANCH_STATE_APPLYING	"applying"	/* FR3 transient CAS state while apply runs */
#define BRANCH_STATE_APPLIED	"applied"
#define BRANCH_STATE_DISCARDED	"discarded"

/* ----------
 * Layer1 NOTIFY channel name (FR1: broadcast branch state changes).
 * Payload format: "<branch_id>:<new_state>"
 * ----------
 */
#define OB_NOTIFY_CHANNEL		"ob_branch_state"

/* ----------
 * Delta operation types
 * ----------
 */
#define DELTA_OP_INSERT		'I'
#define DELTA_OP_UPDATE		'U'
#define DELTA_OP_DELETE		'D'

/* ----------
 * BranchContext: per-session current branch state
 * ----------
 */
typedef struct BranchContext
{
	int32		branch_id;		/* current branch id, 0 means none */
	char		branch_name[NAMEDATALEN];	/* current branch name */
	bool		is_active;		/* whether we are inside a branch */
	char		mode[16];		/* "live" or "snapshot" */
	TimestampTz	created_at;		/* branch creation time */
	Oid			owner;			/* branch owner (from pg_branch.owner) */
	/* ===== V3 multi-session fields (append-only, keep ABI compat) ===== */
	struct SnapshotData *branch_main_snapshot;	/* NULL=live; non-NULL=snapshot frozen MAIN reads */
	bool		snapshot_registered;			/* true iff UnregisterSnapshot is pending */
	uint32		invalidation_counter;			/* FR4: throttle SPI re-checks */
	TimestampTz	invalidation_last_check;		/* FR4: throttle SPI re-checks */
} BranchContext;

/* ----------
 * DeltaTuple: in-memory representation of one delta entry
 * ----------
 */
typedef struct DeltaTuple
{
	int32		branch_id;
	Oid			relid;
	char	   *key;			/* serialized PK value */
	char		op;				/* 'I', 'U', or 'D' */
	char	   *old_version;	/* base version for conflict check */
	bytea	   *tuple_data;		/* serialized new tuple (NULL for delete) */
	bool		emitted;		/* Step4b: has this entry been output in the main pass? */
} DeltaTuple;

/* ----------
 * GUC: current_branch session variable
 * ----------
 */
extern char *overlay_branch_current_name;
extern bool overlay_branch_enabled;

/* ----------
 * V3 multi-session tuning GUCs
 * ----------
 */
extern int	ob_invalidation_check_threshold;	/* every N is_active() calls → SPI check */
extern int	ob_invalidation_check_interval_ms;	/* every M ms → SPI check */
extern bool	ob_apply_strict_pins;				/* apply fails if other sessions hold pins */
extern bool	ob_use_shared_mem_pin_table;		/* optional: shmem pin tracking */
extern bool	ob_in_snapshot_mode_helper;		/* internal: PushActiveSnapshot guard */

/* ----------
 * Recursion-guard globals (owned by overlay_branch.c; written
 * directly by use_internal/apply_internal in branch_lifecycle.c
 * to avoid layering extra trivial setters/getters).
 * ----------
 */
extern bool ob_in_apply_operation;
extern bool ob_in_guc_setconfig;
extern bool ob_in_invalidation_check;
extern bool ob_in_planner_hook;  /* set = throttled SPI running, depth-1 guard. */
extern bool ob_throttled_allow_kickout;  /* only ExecutorRun WR dml_split entry sets true = do cleanup. */

/* ----------
 * Global branch context (for the current session)
 * ----------
 */
extern BranchContext *CurrentBranchContext;

/* ----------
 * Function declarations for branch management
 * ----------
 */
extern void		overlay_branch_init(void);
extern int32	overlay_branch_create_internal(const char *branch_name);
extern void		overlay_branch_use_internal(const char *branch_name);
extern void		overlay_branch_use_with_mode_internal(const char *branch_name,
													   const char *mode);
extern const char *overlay_branch_get_current_name(void);
extern int32	overlay_branch_get_current_id(void);
extern bool		overlay_branch_is_active(void);
extern void		overlay_branch_apply_internal(const char *branch_name);
extern void		overlay_branch_discard_internal(const char *branch_name);
extern void		ob_ensure_unregister_snapshot(BranchContext *ctx);
extern void		ob_exit_branch_cleanup(BranchContext *ctx);
	/* Layer1: NOTIFY broadcast + throttled invalidation SPI recheck */
	extern void		ob_broadcast_state_change(int32 bid, const char *new_state);
	extern bool		ob_invalidate_check_throttled(bool for_dml);

/* I8.2 (2026-09-17 L1 ob_apply_mutex deadlock-timeout fix):
 * session-local per-bid mode cache, replaces use_branch catalog UPDATE.
 *
 * Old pre-I8.2 code persisted every use_branch(name, mode) via SPI
 * UPDATE pg_branch.mode → each UPDATE took RowExclusiveLock on the
 * matching catalog row. Two sessions calling use_branch('same_bid')
 * therefore serialized at the heavyweight lock → under
 * pg_isolation_regress this manifest as s2_p1_use `<waiting ...>`
 * 360s timeout → false deadlock cancel on permutation P2.
 *
 * Cache semantics (identical to catalog behavior within one session):
 *   • bid→last-used-mode HTAB, session lifetime.
 *   • Set on every with_mode_internal success (incl. 1-arg default live).
 *   • Overlaid in list_branches SRF over catalog mode.
 *   • Removed by apply_internal / discard_internal when bid no longer
 *     ACTIVE (no re-enter possible until future recreate; catalog row
 *     then inserts mode='live' default).
 * No heavyweight locks, no cross-session serialization.
 */
extern const char *ob_mode_cache_lookup(int32 bid);
extern void     ob_mode_cache_set(int32 bid, const char *mode);
extern void     ob_mode_cache_remove(int32 bid);

/* T8 § B.11 debug GUCs (NOT ABI-stable, _debug_ prefix) */
extern int		ob_debug_apply_sleep_sec;
extern int		ob_debug_invalidation_counter_throttle_override;

/* ----------
 * Function declarations for Delta Store operations
 * ----------
 */
extern void		overlay_delta_insert(int32 branch_id, Oid relid,
									  const char *key, char op,
									  const char *old_version, const char *tuple_json_cstr);
extern void		overlay_delta_update(int32 branch_id, Oid relid,
									  const char *key, char op,
									  const char *old_version, const char *tuple_json_cstr);
extern bool		overlay_delta_lookup(int32 branch_id, Oid relid,
									  const char *key, DeltaTuple *out_tuple);
extern List    *overlay_delta_list_for_rel(int32 branch_id, Oid relid);
extern int64	overlay_delta_count(int32 branch_id);
extern void		overlay_delta_delete_all(int32 branch_id);

/* ----------
 * Function declarations for Write Redirect
 * ----------
 */
extern bool		overlay_should_redirect(Relation rel);
extern void		overlay_modify_insert(Relation rel, TupleTableSlot *slot);
extern void		overlay_modify_update(Relation rel, TupleTableSlot *oldslot,
									   TupleTableSlot *newslot);
extern void		overlay_modify_delete(Relation rel, TupleTableSlot *slot);

/* ----------
 * Helper utilities
 * ----------
 */
extern char    *overlay_serialize_pk(Relation rel, TupleTableSlot *slot);
extern char    *overlay_serialize_tuple(Relation rel, TupleTableSlot *slot);
extern char    *overlay_tuple_version(Relation rel, TupleTableSlot *slot);
extern bool		overlay_relation_has_pk(Relation rel);

extern bool		overlay_get_pk_single_attno(Relation rel,
											 AttrNumber *out_pk_attno,
											 const char **out_pk_colname);
extern char    *overlay_serialize_pk_from_single_datum(Relation rel,
													   AttrNumber pk_attno,
													   Datum pk_val,
													   bool pk_isnull,
													   Oid consttype);

/* ----------
 * Step7 HARD GUARD: shared helper for checking rel eligibility inside an
 * active branch.  Returns NULL if ok, otherwise returns a malloc'd cstring
 * explaining why the guard fired (caller ereports it with a uniform
 * HINT to leave branch mode).
 *
 * We also expose individual predicate helpers so both ExecutorRun (per-DML)
 * and ProcessUtility (per-DDL) hooks can compose the same error messages.
 *
 *  Guard level order (cheapest → most expensive; any one true → ERROR):
 *    G1: relkind != RELKIND_RELATION       (分区表/视图/matview/外部/序列/组合/TOAST)
 *    G2: rel is partition child            (rd_rel->relispartition)
 *    G3: rel has triggers (excluding internal) → ri_Triggers != NIL or
 *        trigdesc != NULL && any NOT tgisinternal
 *    G4: rel is FK-referenced / has FKs    (rd_att->constr has FK / ref FK count>0)
 *    G5: NO PRIMARY KEY                    (overlay_relation_has_pk == false)
 * ----------
 */
extern bool overlay_guard_rel_ok(Relation rel, char **reason);
extern void overlay_guard_ensure_branch_or_main_active(void); /* prerequisite */
extern void overlay_guard_ereport_fail(const char *operation, const char *relname, const char *reason);

/* Step7 ProcessUtility guard: T_TruncateStmt / T_DropStmt / T_AlterTable /
 * T_CreateStmt / T_IndexStmt / T_VacuumStmt / T_ClusterStmt / T_RenameStmt etc.
 * — any DDL on user rels inside branch mode. */
extern bool overlay_guard_ddl_ok_for_branch(Node *parsetree, char **operation,
											 char **objname, char **reason);

/* ----------
 * Shared internals exposed to branch_scan.c / write_redirect.c /
 * branch_lifecycle.c
 * ---------- */
extern bool             overlay_in_apply_operation(void);
extern bool             overlay_in_overlay_helper(void);
extern void             overlay_overlay_helper_enter(void);
extern void             overlay_overlay_helper_exit(void);
extern bool             overlay_in_write_redirect(void);
extern void             overlay_write_redirect_enter(void);
extern void             overlay_write_redirect_exit(void);
extern int              ob_spi_one_shot(const char *sql, bool read_only, uint64 tcount);
extern TupleTableSlot  *reconstruct_slot_from_delta(Relation rel, bytea *tuple_data);

/* ----------
 * Slot/fetch helpers (delta_store.c) — used by write_redirect.c
 * to translate junk-slot attnos → clean relation slots via ctid.
 * ----------
 */
extern AttrNumber       rel_attno_to_slot_idx(TupleDesc slotdesc, AttrNumber rel_attno);
extern char            *slot_get_ctid_cstr(TupleTableSlot *slot);
extern TupleTableSlot  *fetch_tuple_by_ctid(Relation rel, const char *ctid_cstr);

/* ----------
 * write_redirect.c entry point — thin dispatch from
 * overlay_ExecutorRun in main overlay_branch.c.  mt_state_result_rel
 * is also exposed because the original forward-decl was static.
 * ----------
 */
#include "executor/execdesc.h"      /* QueryDesc + ScanDirection */
struct ModifyTableState;            /* forward decl */
struct ResultRelInfo;
typedef void (*ExecutorRun_hook_type_fn)(QueryDesc *queryDesc,
										 ScanDirection direction,
										 uint64 count, bool execute_once);
extern bool             overlay_executor_run_intercept(QueryDesc *queryDesc,
													   ScanDirection direction,
													   uint64 count,
													   bool execute_once,
													   ExecutorRun_hook_type_fn prev_ExecutorRun);
extern struct ResultRelInfo *mt_state_result_rel(struct ModifyTableState *mt, int i);

#endif							/* OVERLAY_BRANCH_H */
