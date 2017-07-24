/*-------------------------------------------------------------------------
 *
 * bdr_pgl_plugin.c
 * 		pglogical plugin for multi-master replication
 *
 * Copyright (c) 2017, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  bdr_pgl_plugin.c
 *
 * This is the postgres extension and pglogical plugin that integrates BDR
 * functionality with pglogical.
 *
 * User-callable functions, various BDR subsystems, etc should all be
 * elsewhere; keep this file for the integration/glue that makes the extension
 * work.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>

#include "miscadmin.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "access/xlog.h"

#include "fmgr.h"

#include "miscadmin.h"

#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"

#include "storage/dsm.h"
#include "storage/ipc.h"
#include "storage/proc.h"

#include "pglogical_worker.h"
#include "pglogical_plugins.h"
#include "pglogical.h"

#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/int8.h"
#include "utils/rel.h"

#include "pglogical.h"
#include "pglogical_plugins.h"
#include "pglogical_messaging.h"
#include "pglogical_output_config.h"
#include "pglogical_output_plugin.h"

#include "bdr_version.h"
#include "bdr_worker.h"
#include "bdr_sync.h"
#include "bdr_apply.h"
#include "bdr_output.h"
#include "bdr_manager.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(bdr_init_pgl_plugin);

void _PG_init(void);

static void
bdr_worker_start(void)
{
	switch (MyPGLogicalWorker->worker_type)
	{
		case PGLOGICAL_WORKER_SYNC:
			bdr_sync_worker_start();
			break;
		case PGLOGICAL_WORKER_MANAGER:
			bdr_manager_worker_start();
			break;
		case PGLOGICAL_WORKER_APPLY:
			bdr_apply_worker_start();
			break;
		case PGLOGICAL_WORKER_OUTPUT:
			ereport(ERROR,
				(errmsg_internal("bdr_worker_start() called for output plugin instead of bdr_init_pgl_plugin")));
		case PGLOGICAL_WORKER_NONE:
			ereport(ERROR,
				(errmsg_internal("bdr_worker_start() called for empty worker slot")));
	}
}

/*
 * pglogical calls this plugin entrypoint when it sees our
 * 'bdr','bdr_init_pgl_plugin' row in the pglogical.plugins table.
 *
 * Here we register all the pglogical hooks and callbacks we need
 * to implement full BDR on top of pglogical. See pglogical_plugins.[ch]
 */
Datum
bdr_init_pgl_plugin(PG_FUNCTION_ARGS)
{
	int pglogical_version = PG_GETARG_INT32(0);
	PGLogicalWorkerType type PG_USED_FOR_ASSERTS_ONLY = PG_GETARG_INT32(1);
	PGLPlugin *plugin = (PGLPlugin*)PG_GETARG_POINTER(2);
	static int plugin_loaded = false;

	Assert(type != PGLOGICAL_WORKER_NONE);

	if (plugin_loaded)
		PG_RETURN_BOOL(false);

	/*
	 * We record the pglogical version we were built against and complain if it
	 * doesn't match at runtime. Currently an exact match is required.
	 */
	if (pglogical_version != PGLOGICAL_VERSION_NUM)
		elog(ERROR, "compiled against pglogical version %d but got %d",
			 PGLOGICAL_VERSION_NUM, pglogical_version);

	strncpy(NameStr(plugin->plugin_name), "bdr", NAMEDATALEN);

	plugin->worker_start = bdr_worker_start;
	plugin->output_start = bdr_output_start;
	plugin->start_replication_params = bdr_start_replication_params;
	plugin->handle_startup_param = bdr_handle_startup_param;
	plugin->prepare_startup_params = bdr_prepare_startup_params;
	plugin->process_output_param = bdr_process_output_params;
	
	plugin_loaded = true;

	/* enable plugin */
	PG_RETURN_BOOL(true);
}

/*
 * This is the normal postgres extension entrypoint.
 *
 * Because BDR registers a static shared memory segment it must be loaded
 * during shared_preload_libraries. So this only gets called once at
 * that time, not for each backend load.
 */
void
_PG_init(void)
{
	if (!IsPostmasterEnvironment)
	{
		elog(WARNING, "BDR disabled: not running under a normal postmaster");
		return;
	}

	if (!process_shared_preload_libraries_in_progress)
		elog(ERROR, "bdr is not in shared_preload_libraries");

	elog(DEBUG1, "loading bdr %s (%06d)", BDR_VERSION, BDR_VERSION_NUM);
}
