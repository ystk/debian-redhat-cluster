#include <sys/poll.h>
#include <sys/types.h>
#include <sys/errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <inttypes.h>

/* corosync headers */
#include <corosync/corotypes.h>
#include <corosync/corotypes.h>
#include <corosync/coroipc_types.h>
#include <corosync/coroipcc.h>
#include <corosync/corodefs.h>
#include <corosync/mar_gen.h>
#include <corosync/engine/coroapi.h>
#include <corosync/engine/logsys.h>
#include <corosync/engine/quorum.h>
#include <corosync/lcr/lcr_comp.h>

#include "list.h"
#include "cnxman-socket.h"
#include "cnxman-private.h"
#include "commands.h"

#include "ais.h"
#include "cman.h"
#define OBJDB_API struct corosync_api_v1
#include "nodelist.h"
#include "cmanconfig.h"
#include "daemon.h"

extern char cluster_name[MAX_CLUSTER_NAME_LEN+1];
extern unsigned int quorumdev_poll;
extern unsigned int ccsd_poll_interval;
extern unsigned int enable_disallowed;
extern unsigned int shutdown_timeout;
extern unsigned int startup_config_timeout;
extern int init_config(struct corosync_api_v1 *api);

struct totem_ip_address mcast_addr[MAX_INTERFACES];
struct totem_ip_address ifaddrs[MAX_INTERFACES];
int num_interfaces;
uint64_t incarnation;
int num_ais_nodes;
quorum_set_quorate_fn_t corosync_set_quorum;
struct memb_ring_id cman_ring_id;
extern unsigned int config_version;
static hdb_handle_t cluster_parent_handle;

static int startup_pipe;
static int first_trans = 1;
struct corosync_api_v1 *corosync;

static hdb_handle_t group_handle;
static struct corosync_tpg_group cman_group[1] = {
        { .group          = "CMAN", .group_len      = 4},
};

LOGSYS_DECLARE_SUBSYS (CMAN_NAME);

/* This structure is tacked onto the start of a cluster message packet for our
 * own nefarious purposes. */
struct cl_protheader {
	unsigned char  tgtport; /* Target port number */
	unsigned char  srcport; /* Source (originating) port number */
	unsigned short pad;
	unsigned int   flags;
	int            srcid;	/* Node ID of the sender */
	int            tgtid;	/* Node ID of the target */
};

/* Plugin-specific code */
/* Need some better way of determining these.... */
#define CMAN_SERVICE 9

static int cman_exit_fn(void *conn_info);
static int cman_exec_init_fn(struct corosync_api_v1 *api);
static void cman_confchg_fn(enum totem_configuration_type configuration_type,
			    const unsigned int *member_list, size_t member_list_entries,
			    const unsigned int *left_list, size_t left_list_entries,
			    const unsigned int *joined_list, size_t joined_list_entries,
			    const struct memb_ring_id *ring_id);
static void cman_deliver_fn(unsigned int nodeid, const void *msg, unsigned int msg_len,
			    int endian_conversion_required);
static void cman_quorum_init(struct corosync_api_v1 *api, quorum_set_quorate_fn_t report);

/*
 * Exports the interface for the service
 */
static struct corosync_service_engine cman_service_handler = {
	.name		    		= (char *)"corosync CMAN membership service 2.90",
	.id			        = CMAN_SERVICE,
	.flow_control			= COROSYNC_LIB_FLOW_CONTROL_NOT_REQUIRED,
	.lib_exit_fn		       	= cman_exit_fn,
	.exec_init_fn		       	= cman_exec_init_fn,
	.config_init_fn                 = NULL,
	.sync_mode			= CS_SYNC_V1,
};

static struct corosync_service_engine *cman_get_handler_ver0(void)
{
	return (&cman_service_handler);
}

static struct corosync_service_engine_iface_ver0 cman_service_handler_iface = {
	.corosync_get_service_engine_ver0 = cman_get_handler_ver0
};
static struct quorum_services_api_ver1 cman_quorum_iface_ver0 = {
	.init				= cman_quorum_init
};

static struct lcr_iface ifaces_ver0[2] = {
	{
		.name		        = "corosync_cman",
		.version	        = 0,
		.versions_replace     	= 0,
		.versions_replace_count = 0,
		.dependencies	      	= 0,
		.dependency_count      	= 0,
		.constructor	       	= NULL,
		.destructor		= NULL,
		.interfaces		= NULL,
	},
	{
		.name			= "quorum_cman",
		.version		= 0,
		.versions_replace	= 0,
		.versions_replace_count	= 0,
		.dependencies		= 0,
		.dependency_count	= 0,
		.constructor		= NULL,
		.destructor		= NULL,
		.interfaces		= (void **)(void *)&cman_quorum_iface_ver0,
	},
};

static struct lcr_comp cman_comp_ver0 = {
	.iface_count				= 2,
	.ifaces					= ifaces_ver0,
};


__attribute__ ((constructor)) static void cman_comp_register(void) {
	lcr_interfaces_set(&ifaces_ver0[0], &cman_service_handler_iface);
	lcr_interfaces_set(&ifaces_ver0[1], &cman_quorum_iface_ver0);
	lcr_component_register(&cman_comp_ver0);
}

/* ------------------------------- */

static void cman_quorum_init(struct corosync_api_v1 *api, quorum_set_quorate_fn_t report)
{
	corosync = api;
	corosync_set_quorum = report;
}

static int cman_exec_init_fn(struct corosync_api_v1 *api)
{
	hdb_handle_t object_handle;
	hdb_handle_t find_handle;
	hdb_handle_t totem_handle;
	char pipe_msg[256];
	unsigned int totem_token;

	corosync = api;

	if (getenv("CMAN_PIPE"))
                startup_pipe = atoi(getenv("CMAN_PIPE"));

        /* Get our config variables */
	corosync->object_find_create(OBJECT_PARENT_HANDLE, "cluster", strlen("cluster"), &find_handle);
	corosync->object_find_next(find_handle, &cluster_parent_handle);
	corosync->object_find_destroy(find_handle);

	/*
	 * quorum_dev_poll should default to the token timeout so that quorum devices behave
	 * like nodes
	 */
	corosync->object_find_create(OBJECT_PARENT_HANDLE, "totem", strlen("totem"), &find_handle);
	corosync->object_find_next(find_handle, &totem_handle);
	objdb_get_int(api, totem_handle, "token", &totem_token, 1000);
	corosync->object_find_destroy(find_handle);

	corosync->object_find_create(cluster_parent_handle, "cman", strlen("cman"), &find_handle);
	if (corosync->object_find_next(find_handle, &object_handle) == 0)
	{
		objdb_get_int(api, object_handle, "quorum_dev_poll", &quorumdev_poll, totem_token);
		objdb_get_int(api, object_handle, "shutdown_timeout", &shutdown_timeout, DEFAULT_SHUTDOWN_TIMEOUT);
		objdb_get_int(api, object_handle, "ccsd_poll", &ccsd_poll_interval, DEFAULT_CCSD_POLL);
		objdb_get_int(api, object_handle, "disallowed", &enable_disallowed, DEFAULT_DISALLOWED);
		objdb_get_int(api, object_handle, "startup_config_timeout", &startup_config_timeout, DEFAULT_STARTUP_CONFIG_TIMEOUT);
	}
	corosync->object_find_destroy(find_handle);
	log_printf(LOGSYS_LEVEL_DEBUG, CMAN_NAME " starting");

	/* Open local sockets and initialise I/O queues */
	if (read_cman_config(api, &config_version)) {
		/* An error message will have been written to cman_pipe */
		exit(9);
	}
	cman_init(api);

	/* Let cman_tool know we are running and our PID */
	sprintf(pipe_msg,"SUCCESS: %d", getpid());
	write_cman_pipe(pipe_msg);
	close(startup_pipe);
	startup_pipe = 0;

	/* Start totem */
	api->tpg_init(&group_handle, cman_deliver_fn, cman_confchg_fn);
	api->tpg_join(group_handle, cman_group, 1);

	return 0;
}


int cman_exit_fn(void *conn_info)
{
	cman_finish();
	return 0;
}

/* END Plugin-specific code */

int comms_send_message(void *buf, int len,
		       unsigned char toport, unsigned char fromport,
		       int nodeid,
		       unsigned int flags)
{
	struct iovec iov[2];
	struct cl_protheader header;
	int totem_flags = TOTEM_AGREED;

	log_printf(LOGSYS_LEVEL_DEBUG, "ais: comms send message %p len = %d\n", buf,len);
	header.tgtport = toport;
	header.srcport = fromport;
	header.flags   = flags;
	header.srcid   = our_nodeid();
	header.tgtid   = nodeid;

	iov[0].iov_base = &header;
	iov[0].iov_len  = sizeof(header);
	iov[1].iov_base = buf;
	iov[1].iov_len  = len;

	if (flags & MSG_TOTEM_SAFE)
		totem_flags = TOTEM_SAFE;

	return corosync->tpg_joined_mcast(group_handle, iov, 2, totem_flags);
}

static void cman_deliver_fn(unsigned int nodeid, const void *msg, unsigned int msg_len,
			    int endian_conversion_required)
{
	const struct cl_protheader *original_header = msg;
	struct cl_protheader header;
	const char *buf = msg;

	log_printf(LOGSYS_LEVEL_DEBUG, "ais: deliver_fn source nodeid = %d, len=%d, endian_conv=%d\n",
	      nodeid, msg_len, endian_conversion_required);

	if (endian_conversion_required) {
		header.srcid = swab32(original_header->srcid);
		header.tgtid = swab32(original_header->tgtid);
		header.flags = swab32(original_header->flags);
		header.srcport = original_header->srcport;
		header.tgtport = original_header->tgtport;
	} 
	else {
	        memcpy(&header, original_header, sizeof(header));
	}

	/* Only pass on messages for us or everyone */
	if (header.tgtid == our_nodeid() ||
	    header.tgtid == 0) {
		send_to_userport(header.srcport, header.tgtport,
				 header.srcid, header.tgtid,
				 buf + sizeof(struct cl_protheader), msg_len - sizeof(struct cl_protheader),
				 endian_conversion_required);
	}
}

static void cman_confchg_fn(enum totem_configuration_type configuration_type,
			    const unsigned int *member_list, size_t member_list_entries,
			    const unsigned int *left_list, size_t left_list_entries,
			    const unsigned int *joined_list, size_t joined_list_entries,
			    const struct memb_ring_id *ring_id)
{
	int i;
	static int last_memb_count = 0;
	static size_t saved_left_list_entries;
	static size_t saved_left_list_size;
	static unsigned int *saved_left_list = NULL;

	log_printf(LOGSYS_LEVEL_DEBUG, "ais: confchg_fn called type = %d, seq=%lld\n", configuration_type, ring_id->seq);

	memcpy(&cman_ring_id, ring_id, sizeof(*ring_id));
	incarnation = ring_id->seq;
	num_ais_nodes = member_list_entries;

	/* Tell the cman membership layer */
	for (i=0; i<left_list_entries; i++)
		del_ais_node(left_list[i]);

	/* Joining nodes are only added after a valid TRANSITION message
	 * is received.
	 */

	/* Save the left list for later so we can do a consolidated confchg message */
	if (configuration_type == TOTEM_CONFIGURATION_TRANSITIONAL) {
		if (saved_left_list == NULL) {
			saved_left_list_size = left_list_entries*2;
			saved_left_list = malloc(sizeof(int) * saved_left_list_size);
			if (!saved_left_list) {
				log_printf(LOGSYS_LEVEL_CRIT, "cannot allocate memory for confchg message");
				exit(3);
			}
		}
		if (saved_left_list_size < left_list_entries) {
			saved_left_list_size = left_list_entries*2;
			saved_left_list = realloc(saved_left_list, sizeof(int) * saved_left_list_size);
			if (!saved_left_list) {
				log_printf(LOGSYS_LEVEL_CRIT, "cannot reallocate memory for confchg message");
				exit(3);
			}
		}
		saved_left_list_entries = left_list_entries;
		memcpy(saved_left_list, left_list, left_list_entries * sizeof(int));
	}

	if (configuration_type == TOTEM_CONFIGURATION_REGULAR) {
		log_printf(LOGSYS_LEVEL_DEBUG, "ais: last memb_count = %d, current = %"PRIuFAST32"\n", last_memb_count, member_list_entries);
		send_transition_msg(last_memb_count, first_trans);
		last_memb_count = member_list_entries;
		if (member_list_entries > 1)
			first_trans = 0;

		cman_send_confchg(member_list,  member_list_entries,
				  saved_left_list, saved_left_list_entries,
				  joined_list, joined_list_entries);
	}
}

void corosync_shutdown(void)
{
	corosync->shutdown_request();
}

/* Write an error message down the CMAN startup pipe so
   that cman_tool can display it */
int write_cman_pipe(const char *message)
{
	if (startup_pipe)
		return write(startup_pipe, message, strlen(message)+1);

	return 0;
}
