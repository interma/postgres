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
									  const char *old_version, bytea *tuple_data);
extern void		overlay_delta_update(int32 branch_id, Oid relid,
									  const char *key, char op,
									  const char *old_version, bytea *tuple_data);
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
extern bytea   *overlay_serialize_tuple(Relation rel, TupleTableSlot *slot);
extern char    *overlay_tuple_version(Relation rel, TupleTableSlot *slot);
extern bool		overlay_relation_has_pk(Relation rel);

#endif							/* OVERLAY_BRANCH_H */
