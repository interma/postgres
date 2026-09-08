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
#include "datatype/timestamp.h"
#include "executor/tuptable.h"
#include "nodes/pg_list.h"
#include "utils/relcache.h"

/* ----------
 * Branch modes
 * ----------
 */
#define BRANCH_MODE_LIVE		"live"
#define BRANCH_MODE_SNAPSHOT	"snapshot"

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
#define BRANCH_STATE_APPLIED	"applied"
#define BRANCH_STATE_DISCARDED	"discarded"

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
extern const char *overlay_branch_get_current_name(void);
extern int32	overlay_branch_get_current_id(void);
extern bool		overlay_branch_is_active(void);
extern void		overlay_branch_apply_internal(const char *branch_name);
extern void		overlay_branch_discard_internal(const char *branch_name);

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
 * Shared internals exposed to branch_scan.c
 * ----------
 *    overlay_in_apply_operation(): read-only getter for static
 *        ob_in_apply_operation flag (planner hook bypass).
 *
 *    overlay_in_overlay_helper(): shared helper recursion guard.
 *        `ob_compute_overlay_slots_internal` runs the Pass1 MAIN
 *        seqscan via SPI, which would otherwise re-enter the
 *        Planner hook (branch still "active") and cause infinite
 *        recursion.  Caller sets true around the SPI scan, Planner
 *        hook short-circuits on true.  Pair: _enter / _exit.
 *
 *    ob_spi_one_shot(): SPI connect/execute/finish one-shot helper
 *        (previously static in overlay_branch.c, made extern so
 *        branch_scan.c 2-pass helper can use it).
 *
 *    reconstruct_slot_from_delta(): reconstruct bytea tuple_data into
 *        a fresh TTSOpsVirtual slot with independent CreateTupleDescCopy
 *        of rel. (TopMemoryContext palloc.) */
extern bool             overlay_in_apply_operation(void);
extern bool             overlay_in_overlay_helper(void);
extern void             overlay_overlay_helper_enter(void);
extern void             overlay_overlay_helper_exit(void);
/* ----------
 *  overlay_in_write_redirect(): 4th recursion guard.
 *      Step 4a's write-redirection ExecutorRun hook performs its own
 *      internal SPI CMD_SELECT queries to locate WHERE-matching rows
 *      for UPDATE / DELETE.  Those are legitimate CMD_SELECT (so our
 *      `commandType != CMD_SELECT` B-1 guard does NOT skip them) yet
 *      they must scan the raw MAIN heap, never the overlay CustomScan.
 *      Pair: _enter / _exit.  The ExecutorRun hook wraps its intercept
 *      branch PG_TRY with enter/exit so Planner hook short-circuits.
 */
extern bool             overlay_in_write_redirect(void);
extern void             overlay_write_redirect_enter(void);
extern void             overlay_write_redirect_exit(void);
extern int              ob_spi_one_shot(const char *sql, bool read_only, uint64 tcount);
extern TupleTableSlot  *reconstruct_slot_from_delta(Relation rel, bytea *tuple_data);

#endif							/* OVERLAY_BRANCH_H */
