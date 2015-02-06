-- BDR release 0.9.0.0
SET LOCAL search_path = bdr;
SET LOCAL bdr.permit_unsafe_ddl_commands = true;
SET LOCAL bdr.skip_ddl_replication = true;

CREATE FUNCTION bdr_get_remote_nodeinfo(
	dsn text, sysid OUT text, timeline OUT oid, dboid OUT oid,
	variant OUT text, version OUT text, version_num OUT integer,
	min_remote_version_num OUT integer, is_superuser OUT boolean)
RETURNS record LANGUAGE c AS 'MODULE_PATHNAME';

REVOKE ALL ON FUNCTION bdr_get_remote_nodeinfo(text) FROM public;

COMMENT ON FUNCTION bdr_get_remote_nodeinfo(text) IS 'Get node identity and BDR info from a remote server by dsn';

RESET bdr.permit_unsafe_ddl_commands;
RESET bdr.skip_ddl_replication;
RESET search_path;