#include "dlm_daemon.h"
#include "config.h"
#include <libcman.h>
#include "libfenced.h"

static cman_handle_t	ch;
static cman_handle_t	ch_admin;
static cman_node_t      old_nodes[MAX_NODES];
static int              old_node_count;
static cman_node_t      cman_nodes[MAX_NODES];
static int              cman_node_count;

void kick_node_from_cluster(int nodeid)
{
	if (!nodeid) {
		log_error("telling cman to shut down cluster locally");
		cman_shutdown(ch_admin, CMAN_SHUTDOWN_ANYWAY);
	} else {
		log_error("telling cman to remove nodeid %d from cluster",
			  nodeid);
		cman_kill_node(ch_admin, nodeid);
	}
}

static int is_member(cman_node_t *node_list, int count, int nodeid)
{
	int i;

	for (i = 0; i < count; i++) {
		if (node_list[i].cn_nodeid == nodeid)
			return node_list[i].cn_member;
	}
	return 0;
}

static int is_old_member(int nodeid)
{
	return is_member(old_nodes, old_node_count, nodeid);
}

int is_cluster_member(int nodeid)
{
	return is_member(cman_nodes, cman_node_count, nodeid);
}

static cman_node_t *find_cman_node(int nodeid)
{
	int i;

	for (i = 0; i < cman_node_count; i++) {
		if (cman_nodes[i].cn_nodeid == nodeid)
			return &cman_nodes[i];
	}
	return NULL;
}

char *nodeid2name(int nodeid)
{
	cman_node_t *cn;

	cn = find_cman_node(nodeid);
	if (!cn)
		return NULL;
	return cn->cn_name;
}

/* add a configfs dir for cluster members that don't have one,
   del the configfs dir for cluster members that are now gone */

static void statechange(void)
{
	int i, j, rv;
	struct cman_node_address addrs[MAX_NODE_ADDRESSES];
	int num_addrs;
	struct cman_node_address *addrptr = addrs;

	cluster_quorate = cman_is_quorate(ch);

	old_node_count = cman_node_count;
	memcpy(&old_nodes, &cman_nodes, sizeof(old_nodes));

	cman_node_count = 0;
	memset(&cman_nodes, 0, sizeof(cman_nodes));
	rv = cman_get_nodes(ch, MAX_NODES, &cman_node_count, cman_nodes);
	if (rv < 0) {
		log_debug("cman_get_nodes error %d %d", rv, errno);
		return;
	}

	/* Never allow node ID 0 to be considered a member #315711 */
	for (i = 0; i < cman_node_count; i++) {
		if (cman_nodes[i].cn_nodeid == 0) {
			cman_nodes[i].cn_member = 0;
			break;
		}
	}

	for (i = 0; i < old_node_count; i++) {
		if (old_nodes[i].cn_member &&
		    !is_cluster_member(old_nodes[i].cn_nodeid)) {

			log_debug("cluster node %d removed",
				  old_nodes[i].cn_nodeid);

			node_history_cluster_remove(old_nodes[i].cn_nodeid);

			del_configfs_node(old_nodes[i].cn_nodeid);
		}
	}

	for (i = 0; i < cman_node_count; i++) {
		if (cman_nodes[i].cn_member &&
		    !is_old_member(cman_nodes[i].cn_nodeid)) {

			rv = cman_get_node_addrs(ch, cman_nodes[i].cn_nodeid,
						 MAX_NODE_ADDRESSES,
						 &num_addrs, addrs);
			if (rv < 0) {
				log_debug("cman_get_node_addrs failed, falling back to single-homed. ");
				num_addrs = 1;
				addrptr = &cman_nodes[i].cn_address;
			}

			log_debug("cluster node %d added",
				  cman_nodes[i].cn_nodeid);

			node_history_cluster_add(cman_nodes[i].cn_nodeid);

			for (j = 0; j < num_addrs; j++) {
				add_configfs_node(cman_nodes[i].cn_nodeid,
						  addrptr[j].cna_address,
						  addrptr[j].cna_addrlen,
						  (cman_nodes[i].cn_nodeid ==
						   our_nodeid));
			}
		}
	}
}

static void cman_callback(cman_handle_t h, void *private, int reason, int arg)
{
	switch (reason) {
	case CMAN_REASON_TRY_SHUTDOWN:
		if (list_empty(&lockspaces))
			cman_replyto_shutdown(ch, 1);
		else {
			log_debug("no to cman shutdown");
			cman_replyto_shutdown(ch, 0);
		}
		break;
	case CMAN_REASON_STATECHANGE:
		statechange();
		break;
	case CMAN_REASON_CONFIG_UPDATE:
		setup_logging();
		setup_ccs();
		break;
	}
}

void process_cluster(int ci)
{
	int rv;

	rv = cman_dispatch(ch, CMAN_DISPATCH_ALL);
	if (rv == -1 && errno == EHOSTDOWN)
		cluster_dead(0);
}

int setup_cluster(void)
{
	cman_node_t node;
	int rv, fd;
	int init = 0, active = 0;

 retry_init:
	ch_admin = cman_admin_init(NULL);
	if (!ch_admin) {
		if (init++ < 2) {
			sleep(1);
			goto retry_init;
		}
		log_error("cman_admin_init error %d", errno);
		return -ENOTCONN;
	}

	ch = cman_init(NULL);
	if (!ch) {
		log_error("cman_init error %d", errno);
		cman_finish(ch_admin);
		return -ENOTCONN;
	}

 retry_active:
	rv = cman_is_active(ch);
	if (!rv) {
		if (active++ < 2) {
			sleep(1);
			goto retry_active;
		}
		log_error("cman_is_active error %d", errno);
		cman_finish(ch);
		cman_finish(ch_admin);
		return -ENOTCONN;
	}

	rv = cman_start_notification(ch, cman_callback);
	if (rv < 0) {
		log_error("cman_start_notification error %d %d", rv, errno);
		cman_finish(ch);
		cman_finish(ch_admin);
		return rv;
	}

	fd = cman_get_fd(ch);

	/* FIXME: wait here for us to be a member of the cluster */
	memset(&node, 0, sizeof(node));
	rv = cman_get_node(ch, CMAN_NODEID_US, &node);
	if (rv < 0) {
		log_error("cman_get_node us error %d %d", rv, errno);
		cman_stop_notification(ch);
		cman_finish(ch);
		cman_finish(ch_admin);
		fd = rv;
		goto out;
	}
	our_nodeid = node.cn_nodeid;

	old_node_count = 0;
	memset(&old_nodes, 0, sizeof(old_nodes));
	cman_node_count = 0;
	memset(&cman_nodes, 0, sizeof(cman_nodes));
 out:
	return fd;
}

void close_cluster(void)
{
	cman_finish(ch);
	cman_finish(ch_admin);
}

/* Force re-read of cman nodes */
void update_cluster(void)
{
	statechange();
}

int fence_node_time(int nodeid, uint64_t *last_fenced_time)
{
	struct fenced_node nodeinfo;
	int rv;

	memset(&nodeinfo, 0, sizeof(nodeinfo));

	rv = fenced_node_info(nodeid, &nodeinfo);
	if (rv < 0)
		return rv;

	*last_fenced_time = nodeinfo.last_fenced_time;
	return 0;
}

int fence_in_progress(int *count)
{
	struct fenced_domain domain;
	int rv;

	memset(&domain, 0, sizeof(domain));

	rv = fenced_domain_info(&domain);
	if (rv < 0)
		return rv;

	*count = domain.victim_count;
	return 0;
}

