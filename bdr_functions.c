/*-------------------------------------------------------------------------
 *
 * bdr_functions.c
 * 		pglogical plugin for multi-master replication
 *
 * Copyright (c) 2017, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  bdr_functions.c
 *
 * SQL-callable function interface for BDR
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"

#include "access/xact.h"

#include "commands/dbcommands.h"

#include "miscadmin.h"

#include "storage/lwlock.h"

#include "utils/builtins.h"

#include "pglogical_node.h"
#include "pglogical_repset.h"
#include "pglogical_worker.h"
#include "pglogical_functions.h"

#include "bdr_catalogs.h"
#include "bdr_catcache.h"
#include "bdr_consensus.h"
#include "bdr_functions.h"
#include "bdr_join.h"
#include "bdr_shmem.h"
#include "bdr_state.h"

/*
 * Ensure that the local BDR node exists
 */
static BdrNodeInfo *
bdr_check_local_node(bool for_update)
{
	BdrNodeInfo *nodeinfo;

	nodeinfo = bdr_get_local_node_info(for_update, true);
	if (nodeinfo == NULL || nodeinfo->bdr_node == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("current database is not configured as bdr node"),
				 errhint("create bdr node first")));

	if (strcmp(nodeinfo->pgl_node->name, nodeinfo->pgl_interface->name) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("bdr requires that the pglogical interface %s have the same name as the pglogical node %s",
				        nodeinfo->pgl_interface->name, nodeinfo->pgl_node->name)));

	bdr_refresh_cache_local_nodeinfo();
	return nodeinfo;
}

PG_FUNCTION_INFO_V1(bdr_create_node_sql);

/*
 * Create a new local BDR node.
 *
 * If there's an existing pglogical node, it will be re-used if it has the same name
 * or the specified name is null.
 *
 * If no local pglogical node exists, one is created.
 */
Datum
bdr_create_node_sql(PG_FUNCTION_ARGS)
{
	BdrNode bnode;
	PGLogicalLocalNode *pgllocal;
	const char * node_name = NULL;
	const char * local_dsn = NULL;

	if (!PG_ARGISNULL(0))
		node_name = text_to_cstring(PG_GETARG_TEXT_P(0));

	if (!PG_ARGISNULL(1))
		local_dsn = text_to_cstring(PG_GETARG_TEXT_P(1));

	/* Look up underlying pglogical node (if any) */
	pgllocal = get_local_node(false, true);

	/*
	 * Ensure local pglogical node exists and has matching characteristics,
	 * creating it if necessary.
	 */
	if (pgllocal == NULL)
	{
		if (node_name == NULL || local_dsn == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("node name and connection string must be specified when no local pglogical node already exists")));
		elog(NOTICE, "creating new pglogical node");
		create_node_defaults((char*)node_name, (char*)local_dsn);
		pgllocal = get_local_node(false, false);
	}
	else if (node_name != NULL && strcmp(pgllocal->node->name, node_name) != 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("A local pglogical node with name %s exists, cannot create node with name %s",
						pgllocal->node->name, node_name)));
	}
	else if (local_dsn != NULL && strcmp(pgllocal->node_if->dsn, local_dsn) != 0)
	{
		ereport(WARNING,
				(errmsg("connection string for existing local node does not match supplied connstring"),
				 errhint("Check the connection string for the local pglogical interface after node creation")));
	}

	if (strcmp(pgllocal->node->name, pgllocal->node_if->name) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("bdr requires that the pglogical interface %s have the same name as the pglogical node %s",
				        pgllocal->node_if->name, pgllocal->node->name)));

	/*
	 * Then make the BDR node on top. Most of the node is populated later.
	 */
	bnode.node_id = pgllocal->node->id;
	bnode.seq_id = -1;
	bnode.confirmed_our_join = false;
	bnode.node_group_id = InvalidOid;
	bnode.dbname = get_database_name(MyDatabaseId);
	bnode.local_state = 0; /* TODO */

	bdr_node_create(&bnode);
	bdr_refresh_cache_local_nodeinfo();

	pglogical_subscription_changed(InvalidOid);

	/*
	 * Create the initial entry in the BDR state journal.
	 *
	 * (This is one of the few places it's OK to use state_push directly)
	 */
	bdr_state_insert_initial(BDR_NODE_STATE_CREATED);

	PG_RETURN_OID(bnode.node_id);
}

PG_FUNCTION_INFO_V1(bdr_create_node_group_sql);

/*
 * Create a new BDR nodegroup and make it the nodegroup
 * of the local BDR node.
 */
Datum
bdr_create_node_group_sql(PG_FUNCTION_ARGS)
{
	BdrNodeGroup nodegroup;
	BdrNodeInfo *info;
	PGLogicalRepSet		repset;
	char * nodegroup_name;
	BdrStateEntry cur_state;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("node group name may not be null")));

	nodegroup_name = text_to_cstring(PG_GETARG_TEXT_P(0));

	info = bdr_check_local_node(true);

	if (info->bdr_node->node_group_id != InvalidOid)
	{
		if (info->bdr_node_group == NULL)
		{
			/* shoudln't happen */
			elog(ERROR, "local node has node group set but no catalog entry for it found");
		}
		if (info->pgl_node == NULL)
		{
			/* shouldn't happen */
			elog(ERROR, "bdr node exists but no local pglogical node");
		}
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("local BDR node %s is already a member of a node group named %s",
				 		info->pgl_node->name, info->bdr_node_group->name)));
	}
	else
	{
		Assert(info->bdr_node_group == NULL);
	}

	/* Lock state table for update and check state */
	state_get_expected(&cur_state, true, false, BDR_NODE_STATE_CREATED);

	/*
	 * BDR creates an 'internal' replication set with the same name as the BDR
	 * node group.
	 */
	repset.id = InvalidOid;
	repset.nodeid = info->bdr_node->node_id;
	repset.name = nodegroup_name;
	repset.replicate_insert = true;
	repset.replicate_update = true;
	repset.replicate_delete = true;
	repset.replicate_truncate = true;
	repset.isinternal = true;

	nodegroup.id = InvalidOid;
	nodegroup.name = nodegroup_name;
	nodegroup.default_repset = create_replication_set(&repset);
	nodegroup.id = bdr_nodegroup_create(&nodegroup);

	/* Assign the nodegroup to the local node */
	info->bdr_node->node_group_id = nodegroup.id;
	bdr_modify_node(info->bdr_node);

	bdr_refresh_cache_local_nodeinfo();

	/*
	 * This is the first node in a group so there's none of the usual
	 * join process to do and we can jump straight to the active
	 * state.
	 */
	state_transition(&cur_state, BDR_NODE_STATE_ACTIVE, 0, NULL);

	pglogical_subscription_changed(InvalidOid);

	PG_RETURN_OID(nodegroup.id);
}

PG_FUNCTION_INFO_V1(bdr_join_node_group_sql);

/*
 * Join a local BDR node with no nodegroup to a peer node's nodegroup
 * and establish replication with all existing peers of the nodegroup.
 *
 * This is the guts of BDR's node join. It's too complex to be documented
 * entirely here. The final result is to:
 *
 * - Create bdr.nodes entry for this node on peer nodes, in catchup status
 *   (Reserves the node's name as unique across the nodegroup)
 * - Discover all other nodes and create local bdr.node entries for them
 * - Create slots on all other nodes for this node
 * - 
 * 
 * Later, we'll split out these later steps into a separate promote function
 * and optionally run promotion immediately or delayed:
 *
 * - Create slots for peer nodes on this node
 * - Allocate a global sequence ID to the node
 * - Start replicating from this node to remote nodes
 * - Create bdr.node entries for this node on remotes
 * - Make node read/write
 */
Datum
bdr_join_node_group_sql(PG_FUNCTION_ARGS)
{
	const char *join_target_dsn;
	const char *node_group_name = NULL;
	BdrNodeInfo *local;
	BdrNodeInfo *remote;
	PGconn *conn;
	BdrStateEntry cur_state;
	ExtraDataJoinStart extra;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("node join target connection string may not be null")));

	join_target_dsn = text_to_cstring(PG_GETARG_TEXT_P(0));

	if (!PG_ARGISNULL(1))
		node_group_name = text_to_cstring(PG_GETARG_TEXT_P(1));

	/*
	 * TODO FIXME should take for-update lock here
	 *
	 * We should probably lock our local node for update, but if we do that at
	 * the moment we'll deadlock with the manager's attempts to manage apply
	 * workers.
	 */
	local = bdr_check_local_node(false);

	bdr_cache_local_nodeinfo();

	if (local->bdr_node_group != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("local node (%s) is already a member of a nodegroup (%s), cannot join",
				 		local->pgl_node->name,
				 		local->bdr_node_group->name)));

	state_get_expected(&cur_state, true, false, BDR_NODE_STATE_CREATED);

	conn = bdr_join_connect_remote(local, join_target_dsn);

	/*
	 * Here we're going to ask the remote node for some information about its
	 * self, validate that the join looks reasonable, create the initial
	 * catalog state, and hand over to the manager by updating the local node
	 * state journal.
	 */
	PG_TRY();
	{
		remote = get_remote_node_info(conn);

		if (remote->bdr_node == NULL)
			elog(ERROR, "no BDR node found on join target");

		if (remote->bdr_node_group == NULL)
			elog(ERROR, "no BDR node group found on join target, create one before joining");

		Assert(remote->pgl_node->id == remote->bdr_node->node_id);

		if (remote->bdr_node_group->name != NULL
		    && node_group_name != NULL
			&& strcmp(remote->bdr_node_group->name, node_group_name) != 0)
		{
			elog(ERROR, "remote node is member of nodegroup %s but we asked to join nodegroup %s",
				 remote->bdr_node_group->name, node_group_name);
		}
		node_group_name = remote->bdr_node_group->name;

		if (remote->bdr_node_group->id == 0)
			elog(ERROR, "invalid remote nodegroup id 0");

		/*
		 * Create a local copy of the remote node's nodegroup
		 * entry. Also copy the join target node's entry
		 * to the local node.
		 */
		bdr_join_copy_remote_nodegroup(local, remote);
		bdr_join_copy_remote_node(local, remote);

		extra.group_name = node_group_name;
		state_transition(&cur_state, BDR_NODE_STATE_JOIN_START,
			remote->pgl_node->id, &extra);

		/*
		 * TODO: more sanity checks here. Connectback to validate our dsn, etc.
		 * Borrow liberally from pglogical.
		 */

		ereport(NOTICE,
				(errmsg("node join started"),
				 errdetail("%u joining nodegroup %s (%u) via remote node %s (node_id %u)",
						   local->bdr_node->node_id,
						   remote->bdr_node_group->name, remote->bdr_node_group->id,
						   remote->pgl_node->name, remote->bdr_node->node_id),
				 errhint("Further join progress will be reported in the PostgreSQL logs. Use bdr.wait_for_join_completion() to wait until the join completes.")));
	}
	PG_CATCH();
	{
		bdr_finish_connect_remote(conn);
		PG_RE_THROW();
	}
	PG_END_TRY();

	bdr_finish_connect_remote(conn);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bdr_internal_submit_join_request);

/*
 * Remote node is asking to join this node's nodegroup.
 *
 * Called via bdr_submit_join_request (bdr_join.c)
 */
Datum
bdr_internal_submit_join_request(PG_FUNCTION_ARGS)
{
	BdrNodeInfo *local;
	char handle_str[MAX_DIGITS_INT64];
	uint64 handle;
	BdrMsgJoinRequest jreq;

	memset(&jreq, 0, sizeof(BdrMsgJoinRequest));

	if (PG_ARGISNULL(0))
		jreq.nodegroup_name = NULL;
	else
		jreq.nodegroup_name = text_to_cstring(PG_GETARG_TEXT_P(0));

	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("remote node name cannot be NULL")));

	jreq.joining_node_name = text_to_cstring(PG_GETARG_TEXT_P(1));

	if (PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("remote node id cannot be NULL")));

	jreq.joining_node_id = PG_GETARG_OID(2);

	if (PG_ARGISNULL(3))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("remote node state cannot be NULL")));

	jreq.joining_node_state = PG_GETARG_INT32(3);

	if (PG_ARGISNULL(4))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("remote node interface name cannot be NULL")));

	jreq.joining_node_if_name = text_to_cstring(PG_GETARG_TEXT_P(4));

	if (PG_ARGISNULL(5))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("remote node interface id cannot be NULL")));

	jreq.joining_node_if_id = PG_GETARG_OID(5);

	if (PG_ARGISNULL(6))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("remote node interface connection string cannot be NULL")));

	jreq.joining_node_if_dsn = text_to_cstring(PG_GETARG_TEXT_P(6));

	jreq.joining_node_dbname = text_to_cstring(PG_GETARG_TEXT_P(7));

	/* We're not updating the node, just asking the manager to */
	local = bdr_check_local_node(false);

	if (local->bdr_node_group == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("this bdr node %s is not part of a node group, cannot be used as join target by node %s",
				 		local->pgl_node->name, jreq.joining_node_name)));

	if (jreq.nodegroup_name == NULL)
		jreq.nodegroup_name = local->bdr_node_group->name;
	else if (strcmp(jreq.nodegroup_name, local->bdr_node_group->name) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("this bdr node %s is a member of nodegroup %s, joining peer %s cannot join to %s",
				 		local->pgl_node->name, local->bdr_node_group->name,
						jreq.joining_node_name, jreq.nodegroup_name)));
	/*
	 * Submit a consensus proposal asking that the new node be allowed to
	 * join the node group.
	 */
	jreq.nodegroup_id = local->bdr_node_group->id;
	jreq.join_target_node_name = local->pgl_node->name;
	jreq.join_target_node_id = local->pgl_node->id;

	bdr_cache_local_nodeinfo();
	handle = bdr_consensus_enqueue_proposal(BDR_MSG_NODE_JOIN_REQUEST, &jreq);

	snprintf(handle_str, MAX_DIGITS_INT64, UINT64_FORMAT, handle);
	PG_RETURN_TEXT_P(cstring_to_text(handle_str));
}

PG_FUNCTION_INFO_V1(bdr_replication_set_add_table);

/*
 * BDR wrapper around pglogical's pglogical.replication_set_add_table
 * that adds a node to repsets on all nodes.
 *
 * If no repset is named, the default repset for the current node group
 * is used.
 */
Datum
bdr_replication_set_add_table(PG_FUNCTION_ARGS)
{
	Oid					reloid;
	Node			   *row_filter = NULL;
	List			   *att_list = NIL;
	PGLogicalRepSet    *repset = NULL;
	BdrNodeInfo		   *local;

	local = bdr_check_local_node(true);

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relation cannot be NULL")));

	if (local->bdr_node_group == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("this bdr node is not part of a node group, cannot alter replication sets")));

	reloid = PG_GETARG_OID(0);

	if (PG_ARGISNULL(1))
	{
		/* Find default repset for local node's nodegroup */
		repset = get_replication_set(local->bdr_node_group->default_repset);
	}
	else
	{
		const char *repset_name = text_to_cstring(PG_GETARG_TEXT_P(1));
		repset = get_replication_set_by_name(local->bdr_node->node_id, repset_name, false);
		if (!repset->isinternal)
		{
			/*
			 * TODO: we should be checking a "bdr nodegroup repsets" catalog here to be sure
			 * it's really ours, but for now we don't actually support creation of repsets
			 * other than the default so it's kind of moot.
			 */
			elog(ERROR, "replication set '%s' does not appear to owned by a BDR node group",
				 repset->name);
		}
	}

	Assert(repset != NULL);

	if (!PG_ARGISNULL(2) && PG_GETARG_BOOL(2))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("initial table synchronisation not supported on bdr replication sets yet")));

	if (!PG_ARGISNULL(3))
		/* TODO Need to generalise and call the code in pglogical_replication_set_add_table */
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("column filter not supported on bdr replication sets yet")));

	if (!PG_ARGISNULL(4))
		/* TODO Need to generalise and call the code in pglogical_replication_set_add_table */
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("row filter not supported on bdr replication sets yet")));

	replication_set_add_table(repset->id, reloid, att_list, row_filter);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bdr_replication_set_remove_table);

/*
 * BDR wrapper around pglogical's pglogical.replication_set_add_table that
 * removes a table from the nodegroup's repset on all nodes.
 */
Datum
bdr_replication_set_remove_table(PG_FUNCTION_ARGS)
{
	Oid			reloid = PG_GETARG_OID(0);
	PGLogicalRepSet    *repset;
	BdrNodeInfo		   *local;

	local = bdr_check_local_node(true);

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("relation cannot be NULL")));

	if (local->bdr_node_group == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("this bdr node is not part of a node group, cannot alter replication sets")));

	reloid = PG_GETARG_OID(0);

	if (PG_ARGISNULL(1))
	{
		/* Find default repset for local node's nodegroup */
		repset = get_replication_set(local->bdr_node_group->default_repset);
	}
	else
	{
		const char *repset_name = text_to_cstring(PG_GETARG_TEXT_P(1));
		repset = get_replication_set_by_name(local->bdr_node->node_id, repset_name, false);
		if (!repset->isinternal)
		{
			/*
			 * TODO: we should be checking a "bdr nodegroup repsets" catalog here to be sure
			 * it's really ours, but for now we don't actually support creation of repsets
			 * other than the default so it's kind of moot.
			 */
			elog(ERROR, "replication set '%s' does not appear to owned by a BDR node group",
				 repset->name);
		}
	}

	Assert(repset != NULL);

	replication_set_remove_table(repset->id, reloid, false);

	PG_RETURN_VOID();
}

/*
 * Both bdr_local_node_info_sql and bdr_node_group_member_info share
 * the same output tuple format, prepared here.
 */
static HeapTuple
make_nodeinfo_result(BdrNodeInfo *info, TupleDesc tupdesc)
{
	Datum				values[10];
	bool				nulls[10];

	check_nodeinfo(info);

	memset(nulls, 1, sizeof(nulls));
	/* node_id, node_name */
	if (info->pgl_node != NULL)
	{
		values[0] = ObjectIdGetDatum(info->pgl_node->id);
		nulls[0] = false;
		values[1] = CStringGetTextDatum(info->pgl_node->name);
		nulls[1] = false;
	}

	/* node_local_state, node_seq_id */
	if (info->bdr_node != NULL)
	{
		values[2] = ObjectIdGetDatum(info->bdr_node->local_state);
		nulls[2] = false;
		values[3] = Int32GetDatum(info->bdr_node->seq_id);
		nulls[3] = false;
		values[9] = CStringGetTextDatum(info->bdr_node->dbname);
		nulls[9] = false;
	}

	/* node_group_id, node_group_name */
	if (info->bdr_node_group != NULL)
	{
		values[4] = ObjectIdGetDatum(info->bdr_node_group->id);
		nulls[4] = false;
		values[5] = CStringGetTextDatum(info->bdr_node_group->name);
		nulls[5] = false;
	}

	/* interface_id, interface_name, interface_dsn */
	if (info->pgl_interface != NULL)
	{
		values[6] = ObjectIdGetDatum(info->pgl_interface->id);
		nulls[6] = false;
		values[7] = CStringGetTextDatum(info->pgl_interface->name);
		nulls[7] = false;
		values[8] = CStringGetTextDatum(info->pgl_interface->dsn);
		nulls[8] = false;
	}

	return heap_form_tuple(tupdesc, values, nulls);
}

PG_FUNCTION_INFO_V1(bdr_local_node_info_sql);

/*
 * Look up details of the BDR node and node group.
 *
 * Don't change it too casually (except adding cols) as you'll break join by
 * older versions. See bdr_join.c
 */
Datum
bdr_local_node_info_sql(PG_FUNCTION_ARGS)
{
	TupleDesc			tupdesc;
	HeapTuple			htup;
	BdrNodeInfo		   *info;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context "
					 "that cannot accept type record")));

	tupdesc = BlessTupleDesc(tupdesc);

	info = bdr_get_local_node_info(false, true);

	htup = make_nodeinfo_result(info, tupdesc);

	PG_RETURN_DATUM(HeapTupleGetDatum(htup));
}

PG_FUNCTION_INFO_V1(bdr_node_group_member_info);

/*
 * Look up a nodegroup and report bdr and pglogical node information for
 * the nodegroup.
 *
 * This is used during node join to try to shield from catalog changes;
 * we can make new versions of this function if needed, add cols, etc.
 *
 * Don't change it too casually (except adding cols) as you'll break join by
 * older versions. See bdr_join_copy_remote_nodes(...).
 */
Datum
bdr_node_group_member_info(PG_FUNCTION_ARGS)
{
	TupleDesc			tupdesc;
	FuncCallContext	   *funcctx;
	ListCell		   *lc;

	if (SRF_IS_FIRSTCALL())
	{
		Oid				nodegroup_id;
		List		   *nodes;
		MemoryContext   oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		funcctx->max_calls = PG_GETARG_UINT32(0);

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
						 "that cannot accept type record")));

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		if (PG_ARGISNULL(0))
			nodegroup_id = 0;
		else
			nodegroup_id = PG_GETARG_OID(0);

		/* Prepare to iterate the node-list */
		nodes = bdr_get_nodes_info(nodegroup_id);
		funcctx->user_fctx = list_head(nodes);

		(void) MemoryContextSwitchTo(oldcontext);
	}

    funcctx = SRF_PERCALL_SETUP();
	lc = funcctx->user_fctx;
	tupdesc = funcctx->tuple_desc;
	
	if (lc != NULL)
	{
		HeapTuple			htup;
		BdrNodeInfo		   *info;

		info = lfirst(lc);

		Assert(info->bdr_node != NULL);
		Assert(info->pgl_node != NULL);

		htup = make_nodeinfo_result(info, tupdesc);

		funcctx->user_fctx = lnext(lc);

		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(htup));
	}
	else
	{
		SRF_RETURN_DONE(funcctx);
	}

}

PG_FUNCTION_INFO_V1(bdr_decode_message_payload);

Datum
bdr_decode_message_payload(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("unimplemented")));
}


PG_FUNCTION_INFO_V1(bdr_decode_state);

/*
 * Given a composite bdr.state_journal tuple, decode it into human
 * readable form.
 */
Datum
bdr_decode_state(PG_FUNCTION_ARGS)
{
	HeapTupleHeader 	t = PG_GETARG_HEAPTUPLEHEADER(0);
	BdrStateEntry		state;
	TupleDesc			tupdesc;
	HeapTuple			htup;
	Datum				values[2];
	bool				nulls[2] = {false, false};
	const char		   *state_name;
	static const char  *state_name_prefix = "BDR_NODE_STATE_";

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context "
					 "that cannot accept type record")));

	tupdesc = BlessTupleDesc(tupdesc);

	state_decode_tuple(&state, t);

	state_name = bdr_node_state_name(state.current);
	if (strncmp(state_name, state_name_prefix, strlen(state_name_prefix)) == 0)
		state_name = &state_name[strlen(state_name_prefix)];

	values[0] = CStringGetTextDatum(state_name);

	if (state.extra_data == NULL)
		nulls[1] = true;
	else
		values[1] = CStringGetTextDatum(state_stringify_extradata(&state));

	htup = heap_form_tuple(tupdesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(htup));
}

PG_FUNCTION_INFO_V1(bdr_submit_comment);

/*
 * Test function to submit a no-op message into the consensus
 * messaging system for replay to peer nodes.
 */
Datum
bdr_submit_comment(PG_FUNCTION_ARGS)
{
	const char *dummy_payload = text_to_cstring(PG_GETARG_TEXT_P(0));
	uint64 handle;
	char handle_str[33];

	(void) bdr_check_local_node(false);

	handle = bdr_consensus_enqueue_proposal(BDR_MSG_COMMENT,
											(void *) dummy_payload);
	if (handle == 0)
		/*
		 * TODO: block
		 */
		elog(WARNING, "manager couldn't enqueue message, try again later");
	else
		elog(INFO, "manager enqueued message with handle "UINT64_FORMAT, handle);

	snprintf(&handle_str[0], 33, UINT64_FORMAT, handle);
	PG_RETURN_TEXT_P(cstring_to_text(handle_str));
}

PG_FUNCTION_INFO_V1(bdr_consensus_message_outcome);

Datum
bdr_consensus_message_outcome(PG_FUNCTION_ARGS)
{
	const char * handle_str = text_to_cstring(PG_GETARG_TEXT_P(0));
	uint64 handle;
	MNConsensusStatus status;

	(void) bdr_check_local_node(false);

	if (sscanf(handle_str, UINT64_FORMAT, &handle) != 1)
		elog(ERROR, "could not parse %s as uint64", handle_str);

	status = mn_consensus_status(handle);

	PG_RETURN_INT32((int32) status);
}

PG_FUNCTION_INFO_V1(bdr_replicate_ddl_command);

/*
 * This function wraps pglogical.replicate_ddl_command with a convenience
 * lookup to preset the default replication set to the active BDR group's
 * default replication set.
 */
Datum
bdr_replicate_ddl_command(PG_FUNCTION_ARGS)
{
	const char *query;
	BdrNodeInfo *local = bdr_check_local_node(false);
	List	   *replication_sets;
	bool		ret;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot replicate null DDL string")));

	query = text_to_cstring(PG_GETARG_TEXT_PP(0));
	
	if (PG_ARGISNULL(1))
		replication_sets = NIL;
	else
	{
		ArrayType  *rep_set_names = PG_GETARG_ARRAYTYPE_P(1);
		replication_sets = textarray_to_list(rep_set_names);
	}

	if (local->bdr_node_group == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("current bdr node is not part of a node group"),
				 errhint("Create or join a node group first.")));

	/*
	 * BDR uses the node-group's replication set by default, unlike pglogical's
	 * default repset 'sql_ddl' for replicated ddl commands.
	 */
	if (replication_sets == NIL)
	{
		PGLogicalRepSet *repset =
			get_replication_set(local->bdr_node_group->default_repset);
		replication_sets = list_make1(repset->name);
	}

	ret = pglogical_replicate_ddl_command_base(query, replication_sets);
	PG_RETURN_BOOL(ret);
}

PG_FUNCTION_INFO_V1(bdr_gen_slot_name_sql);

Datum
bdr_gen_slot_name_sql(PG_FUNCTION_ARGS)
{
	char * dbname = text_to_cstring(PG_GETARG_TEXT_P(0));
	char * nodegroup_name = text_to_cstring(PG_GETARG_TEXT_P(1));
	char * origin_node_name = text_to_cstring(PG_GETARG_TEXT_P(2));
	char * target_node_name = text_to_cstring(PG_GETARG_TEXT_P(3));
	NameData slot_name;

	bdr_gen_slot_name(&slot_name, dbname, nodegroup_name, origin_node_name, target_node_name);

	PG_RETURN_TEXT_P(cstring_to_text(NameStr(slot_name)));
}