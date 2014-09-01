#include "fd.h"
#include "config.h"
#include <arpa/inet.h>
#include <libcman.h>

#define BUFLEN		MAX_NODENAME_LEN+1

static cman_handle_t	ch;
static cman_handle_t	ch_admin;
static cman_node_t	old_nodes[MAX_NODES];
static int		old_node_count;
static cman_node_t	cman_nodes[MAX_NODES];
static int		cman_node_count;

void set_cman_dirty(void)
{
	int rv;

	rv = cman_set_dirty(ch_admin);
	if (rv)
		log_error("cman_set_dirty error %d", rv);
}

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

static cman_node_t *get_node(cman_node_t *node_list, int count, int nodeid)
{
	int i;

	for (i = 0; i < count; i++) {
		if (node_list[i].cn_nodeid == nodeid)
			return &node_list[i];
	}
	return NULL;
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

static int is_cluster_member(int nodeid)
{
	return is_member(cman_nodes, cman_node_count, nodeid);
}

static int name_equal(char *name1, char *name2)
{
	char name3[BUFLEN], name4[BUFLEN];
	char addr1[INET6_ADDRSTRLEN];
	int i, len1, len2;

	len1 = strlen(name1);
	len2 = strlen(name2);

	if (len1 == len2 && !strncmp(name1, name2, len1))
		return 1;

	/*
	 * If the names are IP addresses then don't compare
	 * what is in front of the dots.
	 */
	if (inet_pton(AF_INET, name1, addr1) == 0)
		return 0;

	if (inet_pton(AF_INET6, name1, addr1) == 0)
		return 0;

	memset(name3, 0, BUFLEN);
	memset(name4, 0, BUFLEN);

	for (i = 0; i < BUFLEN && i < len1; i++) {
		if (name1[i] != '.')
			name3[i] = name1[i];
		else
			break;
	}

	for (i = 0; i < BUFLEN && i < len2; i++) {
		if (name2[i] != '.')
			name4[i] = name2[i];
		else
			break;
	}

	len1 = strlen(name3);
	len2 = strlen(name4);

	if (len1 == len2 && !strncmp(name3, name4, len1))
		return 1;

	return 0;
}

static cman_node_t *find_cman_node_name(char *name)
{
	int i;

	for (i = 0; i < cman_node_count; i++) {
		if (name_equal(cman_nodes[i].cn_name, name))
			return &cman_nodes[i];
	}
	return NULL;
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

char *nodeid_to_name(int nodeid)
{
	cman_node_t *cn;

	cn = find_cman_node(nodeid);
	if (cn)
		return cn->cn_name;

	return NULL;
}

int name_to_nodeid(char *name)
{
	cman_node_t *cn;

	cn = find_cman_node_name(name);
	if (cn)
		return cn->cn_nodeid;

	return -1;
}

static void update_cluster(void)
{
	cman_cluster_t info;
	cman_node_t *old;
	int quorate = cluster_quorate;
	int removed = 0, added = 0;
	int i, rv;

	rv = cman_get_cluster(ch, &info);
	if (rv < 0) {
		log_error("cman_get_cluster error %d %d", rv, errno);
		return;
	}
	cluster_ringid_seq = info.ci_generation;

	cluster_quorate = cman_is_quorate(ch);

	if (!quorate && cluster_quorate)
		quorate_time = time(NULL);

	old_node_count = cman_node_count;
	memcpy(&old_nodes, &cman_nodes, sizeof(old_nodes));

	cman_node_count = 0;
	memset(&cman_nodes, 0, sizeof(cman_nodes));
	rv = cman_get_nodes(ch, MAX_NODES, &cman_node_count, cman_nodes);
	if (rv < 0) {
		log_error("cman_get_nodes error %d %d", rv, errno);
		return;
	}

	for (i = 0; i < old_node_count; i++) {
		if (old_nodes[i].cn_member &&
		    !is_cluster_member(old_nodes[i].cn_nodeid)) {

			log_debug("cluster node %d removed seq %u",
				  old_nodes[i].cn_nodeid, cluster_ringid_seq);

			node_history_cluster_remove(old_nodes[i].cn_nodeid);
			removed++;
		}
	}

	for (i = 0; i < cman_node_count; i++) {
		if (cman_nodes[i].cn_member &&
		    !is_old_member(cman_nodes[i].cn_nodeid)) {

			log_debug("cluster node %d added seq %u",
				  cman_nodes[i].cn_nodeid, cluster_ringid_seq);

			node_history_cluster_add(cman_nodes[i].cn_nodeid);
			added++;
		} else {
			/* look for any nodes that were members of both
			 * old and new but have a new incarnation number
			 * from old to new, indicating they left and rejoined
			 * in between */

			old = get_node(old_nodes, old_node_count, cman_nodes[i].cn_nodeid);

			if (!old)
				continue;
			if (cman_nodes[i].cn_incarnation == old->cn_incarnation)
				continue;

			log_debug("cluster node %d removed and added seq %u "
				  "old %u new %u",
				  cman_nodes[i].cn_nodeid, cluster_ringid_seq,
				  old->cn_incarnation,
				  cman_nodes[i].cn_incarnation);

			node_history_cluster_remove(cman_nodes[i].cn_nodeid);
			removed++;

			node_history_cluster_add(cman_nodes[i].cn_nodeid);
			added++;
		}
	}

	if (removed) {
		cluster_quorate_from_last_update = 0;
	} else if (added) {
		if (!quorate && cluster_quorate)
			cluster_quorate_from_last_update = 1;
		else
			cluster_quorate_from_last_update = 0;
	}
}

/* Note: in fence delay loop we aren't processing callbacks so won't
   have done an update_cluster() in response to a cman callback */

int is_cluster_member_reread(int nodeid)
{
	int rv;

	update_cluster();

	rv = is_cluster_member(nodeid);
	if (rv)
		return 1;

	/* log_debug("cman_member %d not member", nodeid); */
	return 0;
}

static void cman_callback(cman_handle_t h, void *private, int reason, int arg)
{
	int quorate = cluster_quorate;

	switch (reason) {
	case CMAN_REASON_TRY_SHUTDOWN:
		if (list_empty(&domains))
			cman_replyto_shutdown(ch, 1);
		else {
			log_debug("no to cman shutdown");
			cman_replyto_shutdown(ch, 0);
		}
		break;
	case CMAN_REASON_STATECHANGE:
		update_cluster();

		/* domain may have been waiting for quorum */
		if (!quorate && cluster_quorate && (group_mode == GROUP_LIBCPG))
			process_fd_changes();
		break;

	case CMAN_REASON_CONFIG_UPDATE:
		setup_logging();
		reread_ccs();
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

	update_cluster();

	fd = cman_get_fd(ch);

	/* FIXME: wait here for us to be a member of the cluster */
	memset(&node, 0, sizeof(node));
	rv = cman_get_node(ch, CMAN_NODEID_US, &node);
	if (rv < 0) {
		log_error("cman_get_node us error %d %d", rv, errno);
		cman_finish(ch);
		cman_finish(ch_admin);
		fd = rv;
		goto out;
	}

	memset(our_name, 0, sizeof(our_name));
	strncpy(our_name, node.cn_name, CMAN_MAX_NODENAME_LEN);
	our_nodeid = node.cn_nodeid;

	log_debug("our_nodeid %d our_name %s", our_nodeid, our_name);
 out:
	return fd;
}

void close_cluster(void)
{
	cman_finish(ch);
	cman_finish(ch_admin);
}

struct node *get_new_node(struct fd *fd, int nodeid)
{
	cman_node_t cn;
	struct node *node;
	int rv;

	node = malloc(sizeof(*node));
	if (!node)
		return NULL;
	memset(node, 0, sizeof(struct node));

	node->nodeid = nodeid;

	memset(&cn, 0, sizeof(cn));
	rv = cman_get_node(ch, nodeid, &cn);
	if (rv < 0)
		log_debug("get_new_node %d no cman node %d", nodeid, rv);
	else
		strncpy(node->name, cn.cn_name, MAX_NODENAME_LEN);

	return node;
}

