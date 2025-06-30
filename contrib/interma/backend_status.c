#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "storage/procsignal.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "miscadmin.h"
#include "utils/backend_status.h"  // 关键结构
#include "storage/lwlock.h"
#include "storage/shmem.h"

PG_MODULE_MAGIC;

// 基于materialized set returning function
Datum my_info(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(my_info);

// 基于func call的函数，用于获取当前所有后端的状态信息
// 返回的结果为一个setof record，包含后端进程ID、查询文本
Datum my_backend_status(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(my_backend_status);

#define NUM_INFO	2
#define NUM_BACKEND_STATUS	3

/**
ubuntu=# select my_info();
   my_info
--------------
 (1,interma)
 (2,xudiudiu)
 (3,anyu)
(3 rows) 
 */
Datum
my_info(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Datum		values[NUM_INFO];
	bool		nulls[NUM_INFO] = {0};

    // dumped info
    char *info[] = {"interma", "xudiudiu", "anyu"};
    int cnt = sizeof(info) / sizeof(info[0]);

	InitMaterializedSRF(fcinfo, 0);

	for (int i = 0; i < cnt; i++)
	{
		values[0] = Int32GetDatum(i+1);
		values[1] = CStringGetTextDatum(info[i]);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}

/**
ubuntu=# select * from  my_backend;
  pid   |           query            | status
--------+----------------------------+---------
        |                            |
 156116 | select * from  my_backend; | running
 122670 |                            | unknown
 122671 |                            | unknown
 122667 |                            | unknown
 122668 |                            | unknown
(6 rows)
 */
Datum
my_backend_status(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    int call_cntr;
    int max_calls;
    TupleDesc tupledesc;

    if (SRF_IS_FIRSTCALL())
    {
        int n;
        MemoryContext oldcontext;
        funcctx = SRF_FIRSTCALL_INIT();
        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        /* 获取所有 backend 状态信息 */
        n = pgstat_fetch_stat_numbackends();
        funcctx->max_calls = n;

        /* 定义返回的元组结构：(pid int, query text) */
        tupledesc = CreateTemplateTupleDesc(NUM_BACKEND_STATUS);
        TupleDescInitEntry(tupledesc, (AttrNumber) 1, "pid",
                           INT4OID, -1, 0);
        TupleDescInitEntry(tupledesc, (AttrNumber) 2, "query",
                           TEXTOID, -1, 0);
        TupleDescInitEntry(tupledesc, (AttrNumber) 3, "state",
                           TEXTOID, -1, 0);
        funcctx->tuple_desc = BlessTupleDesc(tupledesc);

        MemoryContextSwitchTo(oldcontext);
    }

    funcctx = SRF_PERCALL_SETUP();
    call_cntr = funcctx->call_cntr;
    max_calls = funcctx->max_calls;

    if (call_cntr < max_calls)
    {
        Datum values[NUM_BACKEND_STATUS];
        bool nulls[NUM_BACKEND_STATUS];
        HeapTuple tuple;
        LocalPgBackendStatus *lc_beentry;
        PgBackendStatus *beentry;
    
        for (int i = 0; i < NUM_BACKEND_STATUS; i++)
            nulls[i] = false;       // 默认不为 NULL

        lc_beentry = pgstat_get_local_beentry_by_index(call_cntr);
        if (lc_beentry == NULL)
        {
            nulls[0] = true;
            nulls[1] = true;
            nulls[2] = true;
        }
        else
        {
            beentry = &lc_beentry->backendStatus;
            values[0] = Int32GetDatum(beentry->st_procpid);
            if (beentry->st_activity_raw)
                values[1] = CStringGetTextDatum(beentry->st_activity_raw);
            else
                nulls[1] = true;  // 如果没有活动信息，则设置为 NULL
            values[2] = CStringGetTextDatum(beentry->st_state == STATE_IDLE ? "idle" :
                                            beentry->st_state == STATE_RUNNING ? "running" :
                                            beentry->st_state == STATE_IDLEINTRANSACTION ? "idle in transaction" :
                                            beentry->st_state == STATE_FASTPATH ? "fastpath" :
                                            beentry->st_state == STATE_IDLEINTRANSACTION_ABORTED ? "idle in transaction aborted" :
                                            beentry->st_state == STATE_DISABLED ? "disabled" : "unknown");
        }

        tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
        SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
    }
    else
    {
        SRF_RETURN_DONE(funcctx);
    }
}
