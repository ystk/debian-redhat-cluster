#include "fd.h"
#include "config.h"

void free_node_list(struct list_head *head)
{
	struct node *node;

	while (!list_empty(head)) {
		node = list_entry(head->next, struct node, list);
		list_del(&node->list);
		free(node);
	}
}

void add_complete_node(struct fd *fd, int nodeid)
{
	struct node *node;

	node = get_new_node(fd, nodeid);
	list_add(&node->list, &fd->complete);

	if (group_mode == GROUP_LIBGROUP)
		return;

	node_history_init(fd, nodeid);
}

int list_count(struct list_head *head)
{
	struct list_head *tmp;
	int count = 0;

	list_for_each(tmp, head)
		count++;
	return count;
}

int is_victim(struct fd *fd, int nodeid)
{
	struct node *node;

	list_for_each_entry(node, &fd->victims, list) {
		if (node->nodeid == nodeid)
			return 1;
	}
	return 0;
}

static void victim_done(struct fd *fd, int victim, int how)
{
	if (group_mode == GROUP_LIBGROUP)
		return;

	node_history_fence(fd, victim, our_nodeid, how, time(NULL));
	send_victim_done(fd, victim);
}

/* This routine should probe other indicators to check if victims
   can be reduced.  Right now we just check if the victim has rejoined the
   cluster. */

static int reduce_victims(struct fd *fd)
{
	struct node *node, *safe;
	int num_victims;

	num_victims = list_count(&fd->victims);

	list_for_each_entry_safe(node, safe, &fd->victims, list) {
		if (is_cluster_member_reread(node->nodeid) &&
		    is_clean_daemon_member(node->nodeid)) {
			log_debug("reduce victim %s", node->name);
			victim_done(fd, node->nodeid, VIC_DONE_MEMBER);
			list_del(&node->list);
			free(node);
			num_victims--;
		}
	}

	return num_victims;
}

static inline void close_override(int *fd, const char *path)
{
	unlink(path);
	if (fd) {
		if (*fd >= 0)
			close(*fd);
		*fd = -1;
	}
}

static int open_override(const char *path)
{
	int ret;
	mode_t om;

	om = umask(077);
	ret = mkfifo(path, (S_IRUSR | S_IWUSR));
	umask(om);

	if (ret < 0)
		return -1;
        return open(path, O_RDONLY | O_NONBLOCK);
}

static int check_override(int ofd, char *nodename, int timeout)
{
	char buf[128];
	fd_set rfds;
	struct timeval tv = {0, 0};
	int ret, x, rv;

	query_unlock();

	if (ofd < 0 || !nodename || !strlen(nodename)) {
		sleep(timeout);
		rv = 0;
		goto out;
	}

	FD_ZERO(&rfds);
	FD_SET(ofd, &rfds);
	tv.tv_usec = 0;
	tv.tv_sec = timeout;

	ret = select(ofd + 1, &rfds, NULL, NULL, &tv);
	if (ret < 0) {
		log_debug("check_override select: %s", strerror(errno));
		rv = -1;
		goto out;
	}

	if (ret == 0) {
		rv = 0;
		goto out;
	}

	memset(buf, 0, sizeof(buf));
	ret = read(ofd, buf, sizeof(buf) - 1);
	if (ret < 0) {
		log_debug("check_override read: %s", strerror(errno));
		rv = -1;
		goto out;
	}

	/* chop off control characters */
	for (x = 0; x < ret; x++) {
		if (buf[x] < 0x20) {
			buf[x] = 0;
			break;
		}
	}

	if (!strcasecmp(nodename, buf)) {
		/* Case insensitive, but not as nice as, say, name_equal
		   in the other file... */
		rv = 1;
		goto out;
	}

	rv = 0;
 out:
	query_lock();
	return rv;
}

/* If there are victims after a node has joined, it's a good indication that
   they may be joining the cluster shortly.  If we delay a bit they might
   become members and we can avoid fencing them.  This is only really an issue
   when the fencing method reboots the victims.  Otherwise, the nodes should
   unfence themselves when they start up. */

void delay_fencing(struct fd *fd, int node_join)
{
	struct timeval first, last, start, now;
	int victim_count, last_count = 0, delay = 0;
	struct node *node;
	const char *delay_type;

	if (list_empty(&fd->victims))
		return;

	if (node_join || cluster_quorate_from_last_update) {
		delay = cfgd_post_join_delay;
		delay_type = "post_join_delay";
	} else {
		delay = cfgd_post_fail_delay;
		delay_type = "post_fail_delay";
	}

	log_debug("delay %s %d quorate_from_last_update %d",
		  delay_type, delay, cluster_quorate_from_last_update);

	if (delay == 0)
		goto out;

	gettimeofday(&first, NULL);
	gettimeofday(&start, NULL);

	for (;;) {
		query_unlock();
		sleep(1);
		query_lock();

		victim_count = reduce_victims(fd);

		if (victim_count == 0)
			break;

		if (victim_count < last_count) {
			gettimeofday(&start, NULL);
			if (delay > 0 && cfgd_post_join_delay > delay) {
				delay = cfgd_post_join_delay;
				delay_type = "post_join_delay (modified)";
			}
		}

		last_count = victim_count;

		/* negative delay means wait forever */
		if (delay == -1)
			continue;

		gettimeofday(&now, NULL);
		if (now.tv_sec - start.tv_sec >= delay)
			break;
	}

	gettimeofday(&last, NULL);

	log_debug("delay of %ds leaves %d victims",
		  (int) (last.tv_sec - first.tv_sec), victim_count);
 out:
	list_for_each_entry(node, &fd->victims, list) {
		log_debug("%s not a cluster member after %d sec %s",
		          node->name, delay, delay_type);
	}
}

void defer_fencing(struct fd *fd)
{
	char *master_name;

	if (list_empty(&fd->victims))
		return;

	master_name = nodeid_to_name(fd->master);

	log_level(LOG_INFO, "fencing deferred to %s",
		  master_name ? master_name : "unknown");
}

static const char *fe_str(int r)
{
	switch (r) {
	case FE_AGENT_SUCCESS:
		return "success";
	case FE_AGENT_ERROR:
		return "error from agent";
	case FE_AGENT_FORK:
		return "error from fork";
	case FE_NO_CONFIG:
		return "error from ccs";
	case FE_NO_METHOD:
		return "error no method";
	case FE_NO_DEVICE:
		return "error no device";
	case FE_READ_AGENT:
		return "error config agent"; 
	case FE_READ_ARGS:
		return "error config args"; 
	case FE_READ_METHOD:
		return "error config method"; 
	case FE_READ_DEVICE:
		return "error config device"; 
	default:
		return "error unknown";
	}
}

#define FL_SIZE 32
static struct fence_log flog[FL_SIZE];
static struct fence_log prev_flog[FL_SIZE];

void fence_victims(struct fd *fd)
{
	struct node *node;
	int error, i, ll, flog_count, prev_flog_count;
	int override = -1;
	int cluster_member, cpg_member, ext;
	unsigned int limit, retries;

	list_for_each_entry(node, &fd->victims, list) {
		if (node->local_victim_done) {
			/* local_victim_done means we've successfully fenced
			   this node and will remove it from victims list
			   upon receipt of the victim_done message we sent */

			/* I don't believe fence_victims() can come across
			   a node with local_victim_done set.  That would
			   mean: pass 1 through fence_victims() fences node A,
			   send victim_done(A), return, process confchgs
			   adding and removing A again, receive msgs for start
			   cycle, but *not* receive victim_done(A) msg before
			   coming through fence_victims() again. */

			log_error("skip local_victim_done node %d",
				  node->nodeid);
			continue;
		}

		/* limit repeated logging of the same failure messages
		   when retrying fencing */

		limit = 0;
		retries = 0;
		memset(&prev_flog, 0, sizeof(flog));
		prev_flog_count = 0;

 retry:
		if (retries > 2)
			limit = 1;
		if (limit && !(retries % 600))
			log_level(LOG_INFO, "fencing node %s still retrying",
				  node->name);

		/* for queries */
		fd->current_victim = node->nodeid;

		cluster_member = is_cluster_member_reread(node->nodeid);
		cpg_member = is_clean_daemon_member(node->nodeid);
		if (group_mode == GROUP_LIBCPG)
			ext = is_fenced_external(fd, node->nodeid);
		else
			ext = 0;

		if ((cluster_member && cpg_member) || ext) {
			log_debug("averting fence of node %s "
				  "cluster member %d cpg member %d external %d",
				  node->name, cluster_member, cpg_member, ext);

			node->local_victim_done = 1;
			victim_done(fd, node->nodeid,
				    ext ? VIC_DONE_EXTERNAL : VIC_DONE_MEMBER);
			continue;
		}

		memset(&flog, 0, sizeof(flog));
		flog_count = 0;

		if (!limit)
			log_level(LOG_INFO, "fencing node %s", node->name);

		query_unlock();
		error = fence_node(node->name, flog, FL_SIZE, &flog_count);
		query_lock();

		if (flog_count > FL_SIZE) {
			log_error("fence_node log overflow %d", flog_count);
			flog_count = FL_SIZE;
		}

		if (limit && error &&
		    flog_count == prev_flog_count &&
		    !memcmp(&flog, &prev_flog, sizeof(flog))) {
			goto skip_log_message;
		}

		memcpy(&prev_flog, &flog, sizeof(flog));
		prev_flog_count = flog_count;

		for (i = 0; i < flog_count; i++) {
			ll = (flog[i].error == FE_AGENT_SUCCESS) ? LOG_DEBUG:
								  LOG_ERR;
			log_level(ll, "fence %s dev %d.%d agent %s result: %s",
				  node->name,
				  flog[i].method_num, flog[i].device_num,
				  flog[i].agent_name[0] ?
				  	flog[i].agent_name : "none",
				  fe_str(flog[i].error));
		}

		log_error("fence %s %s", node->name,
			  error ? "failed" : "success");

		if (!cfgd_disable_dbus) {
			fd_dbus_send(node->name, node->nodeid, error);
		}

 skip_log_message:
		if (!error) {
			node->local_victim_done = 1;
			victim_done(fd, node->nodeid, VIC_DONE_AGENT);
			continue;
		}

		if (!cfgd_override_path) {
			query_unlock();
			sleep(5);
			query_lock();
			retries++;
			goto retry;
		}

		/* Check for manual intervention */
		override = open_override(cfgd_override_path);
		if (check_override(override, node->name,
				   cfgd_override_time) > 0) {
			log_level(LOG_WARNING, "fence %s overridden by "
				  "administrator intervention", node->name);
			node->local_victim_done = 1;
			victim_done(fd, node->nodeid, VIC_DONE_OVERRIDE);
			close_override(&override, cfgd_override_path);
			continue;
		} else {
			close_override(&override, cfgd_override_path);
			retries++;
			goto retry;
		}
	}

	fd->current_victim = 0;
}

