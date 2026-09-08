#ifndef OVERLAY_BRANCH_BRANCH_SCAN_H
#define OVERLAY_BRANCH_BRANCH_SCAN_H

#include "postgres.h"
#include "executor/tuptable.h"
#include "nodes/pg_list.h"

/* Entry point: called by _PG_init() to register planner hooks + CustomScanMethods */
extern void branch_scan_init(void);

/*
 * Public 2-pass overlay helper shared by
 * SRF overlay_main_plus_delta() and BranchScan BeginCustomScan().
 * Returns List<TupleTableSlot*> allocated in TopMemoryContext; each slot is
 * TTSOpsVirtual with its own independent CreateTupleDescCopy() of the relation.
 * Caller is responsible for list_free_deep(ExecDrop) at end. */
extern List *ob_compute_overlay_slots(Oid relid, int32 branch_id);

#endif
