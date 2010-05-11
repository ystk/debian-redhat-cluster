#include "gfs_daemon.h"
#include "config.h"
#include <pthread.h>
#include "copyright.cf"

#include <linux/netlink.h>

#define LOCKFILE_NAME	"/var/run/gfs_controld.pid"
#define CLIENT_NALLOC   32
#define UEVENT_BUF_SIZE 4096

static int client_maxi;
static int client_size;
static struct client *client;
static struct pollfd *pollfd;
static pthread_t query_thread;
static pthread_mutex_t query_mutex;

struct client {
	int fd;
	void *workfn;
	void *deadfn;
	struct mountgroup *mg;
};

static void do_withdraw(char *name);

int do_read(int fd, void *buf, size_t count)
{
	int rv, off = 0;

	while (off < count) {
		rv = read(fd, (char *)buf + off, count - off);
		if (rv == 0)
			return -1;
		if (rv == -1 && errno == EINTR)
			continue;
		if (rv == -1)
			return -1;
		off += rv;
	}
	return 0;
}

int do_write(int fd, void *buf, size_t count)
{
	int rv, off = 0;

 retry:
	rv = write(fd, (char *)buf + off, count);
	if (rv == -1 && errno == EINTR)
		goto retry;
	if (rv < 0) {
		log_error("write errno %d", errno);
		return rv;
	}

	if (rv != count) {
		count -= rv;
		off += rv;
		goto retry;
	}
	return 0;
}

static void client_alloc(void)
{
	int i;

	if (!client) {
		client = malloc(CLIENT_NALLOC * sizeof(struct client));
		pollfd = malloc(CLIENT_NALLOC * sizeof(struct pollfd));
	} else {
		client = realloc(client, (client_size + CLIENT_NALLOC) *
					 sizeof(struct client));
		pollfd = realloc(pollfd, (client_size + CLIENT_NALLOC) *
					 sizeof(struct pollfd));
		if (!pollfd)
			log_error("can't alloc for pollfd");
	}
	if (!client || !pollfd)
		log_error("can't alloc for client array");

	for (i = client_size; i < client_size + CLIENT_NALLOC; i++) {
		client[i].workfn = NULL;
		client[i].deadfn = NULL;
		client[i].fd = -1;
		pollfd[i].fd = -1;
		pollfd[i].revents = 0;
	}
	client_size += CLIENT_NALLOC;
}

void client_dead(int ci)
{
	close(client[ci].fd);
	client[ci].workfn = NULL;
	client[ci].fd = -1;
	pollfd[ci].fd = -1;
}

int client_add(int fd, void (*workfn)(int ci), void (*deadfn)(int ci))
{
	int i;

	if (!client)
		client_alloc();
 again:
	for (i = 0; i < client_size; i++) {
		if (client[i].fd == -1) {
			client[i].workfn = workfn;
			if (deadfn)
				client[i].deadfn = deadfn;
			else
				client[i].deadfn = client_dead;
			client[i].fd = fd;
			pollfd[i].fd = fd;
			pollfd[i].events = POLLIN;
			if (i > client_maxi)
				client_maxi = i;
			return i;
		}
	}

	client_alloc();
	goto again;
}

int client_fd(int ci)
{
	return client[ci].fd;
}

void client_ignore(int ci, int fd)
{
	pollfd[ci].fd = -1;
	pollfd[ci].events = 0;
}

void client_back(int ci, int fd)
{
	pollfd[ci].fd = fd;
	pollfd[ci].events = POLLIN;
}

static void sigterm_handler(int sig)
{
	daemon_quit = 1;
}

struct mountgroup *create_mg(char *name)
{
	struct mountgroup *mg;

	mg = malloc(sizeof(struct mountgroup));
	if (!mg)
		return NULL;
	memset(mg, 0, sizeof(struct mountgroup));

	if (group_mode == GROUP_LIBGROUP)
		mg->old_group_mode = 1;

	INIT_LIST_HEAD(&mg->members);
	INIT_LIST_HEAD(&mg->members_gone);
	INIT_LIST_HEAD(&mg->plock_resources);
	INIT_LIST_HEAD(&mg->saved_messages);
	INIT_LIST_HEAD(&mg->changes);
	INIT_LIST_HEAD(&mg->journals);
	INIT_LIST_HEAD(&mg->node_history);
	mg->init = 1;
	mg->master_nodeid = -1;
	mg->low_nodeid = -1;

	strncpy(mg->name, name, GFS_MOUNTGROUP_LEN);

	return mg;
}

struct mountgroup *find_mg(char *name)
{
	struct mountgroup *mg;

	list_for_each_entry(mg, &mountgroups, list) {
		if ((strlen(mg->name) == strlen(name)) &&
		    !strncmp(mg->name, name, strlen(name)))
			return mg;
	}
	return NULL;
}

struct mountgroup *find_mg_id(uint32_t id)
{
	struct mountgroup *mg;

	list_for_each_entry(mg, &mountgroups, list) {
		if (mg->id == id)
			return mg;
	}
	return NULL;
}

static void ping_kernel_mount(char *name)
{
	struct mountgroup *mg;
	int rv, val;

	mg = find_mg(name);
	if (!mg)
		return;

	rv = read_sysfs_int(mg, "block", &val);

	log_group(mg, "ping_kernel_mount %d", rv);
}

enum {
	Env_ACTION = 0,
	Env_SUBSYSTEM,
	Env_LOCKPROTO,
	Env_LOCKTABLE,
	Env_DEVPATH,
	Env_RECOVERY,
	Env_FIRSTMOUNT,
	Env_JID,
	Env_Last, /* Flag for end of vars */
};

static const char *uevent_vars[] = {
	[Env_ACTION]		= "ACTION=",
	[Env_SUBSYSTEM]		= "SUBSYSTEM=",
	[Env_LOCKPROTO]		= "LOCKPROTO=",
	[Env_LOCKTABLE]		= "LOCKTABLE=",
	[Env_DEVPATH]		= "DEVPATH=",
	[Env_RECOVERY]		= "RECOVERY=",
	[Env_FIRSTMOUNT]	= "FIRSTMOUNT=",
	[Env_JID]		= "JID=",
};

/*
 * Parses a uevent message for the interesting bits. It requires a list
 * of variables to look for, and an equally long list of pointers into
 * which to write the results.
 */
static void decode_uevent(const char *buf, unsigned len, const char *vars[],
			  unsigned nvars, const char *vals[])
{
	const char *ptr;
	unsigned int i;
	int slen, vlen;

	memset(vals, 0, sizeof(const char *) * nvars);

	while (len > 0) {
		ptr = buf;
		slen = strlen(ptr);
		buf += slen;
		len -= slen;
		buf++;
		len--;

		for (i = 0; i < nvars; i++) {
			vlen = strlen(vars[i]);
			if (vlen > slen)
				continue;
			if (memcmp(vars[i], ptr, vlen) != 0)
				continue;
			vals[i] = ptr + vlen;
			break;
		}
	}
}

static char *uevent_fsname(const char *vars[])
{
	char *name = NULL;

	if (vars[Env_LOCKTABLE])
		name = strchr(vars[Env_LOCKTABLE], ':');

	/* When all kernels are converted, we can dispose with the following
	 * grotty bit. This is for backward compatibility only.
	 */
	if (!name && vars[Env_DEVPATH]) {
		name = strchr(vars[Env_DEVPATH], ':');
		if (name) {
			char *end = strstr(name, "/lock_module");
			if (end)
				*end = 0;
		}
	}
	return (name && name[0]) ? name + 1 : NULL;
}

static void process_uevent(int ci)
{
	char buf[UEVENT_BUF_SIZE];
	const char *uevent_vals[Env_Last];
	char *fsname;
	int rv;

 retry_recv:
	rv = recv(client[ci].fd, &buf, sizeof(buf), 0);
	if (rv < 0) {
		if (errno == EINTR)
			goto retry_recv;
		if (errno != EAGAIN)
			log_error("uevent recv error %d errno %d", rv, errno);
		return;
	}
	buf[rv] = 0;

	decode_uevent(buf, rv, uevent_vars, Env_Last, uevent_vals);

	if (!uevent_vals[Env_DEVPATH] ||
	    !uevent_vals[Env_ACTION] ||
	    !uevent_vals[Env_SUBSYSTEM])
		return;

	if (!strstr(uevent_vals[Env_DEVPATH], "/fs/gfs"))
		return;

	log_debug("uevent %s %s %s",
		  uevent_vals[Env_ACTION],
		  uevent_vals[Env_SUBSYSTEM],
		  uevent_vals[Env_DEVPATH]);

	fsname = uevent_fsname(uevent_vals);
	if (!fsname) {
		log_error("no fsname uevent %s %s %s",
		  	  uevent_vals[Env_ACTION],
		  	  uevent_vals[Env_SUBSYSTEM],
		  	  uevent_vals[Env_DEVPATH]);
		return;
	}

	if (!strcmp(uevent_vals[Env_ACTION], "remove")) {
		/* We want to trigger the leave at the very end of the kernel's
		   unmount process, i.e. at the end of put_super(), so we do the
		   leave when the second uevent (from the gfs kobj) arrives. */

		if (strcmp(uevent_vals[Env_SUBSYSTEM], "lock_dlm") == 0)
			return;
		if (group_mode == GROUP_LIBGROUP)
			do_leave_old(fsname, 0);
		else
			do_leave(fsname, 0);

	} else if (!strcmp(uevent_vals[Env_ACTION], "change")) {
		int jid, status = -1, first = -1;

		if (!uevent_vals[Env_JID] ||
		    (sscanf(uevent_vals[Env_JID], "%d", &jid) != 1))
			jid = -1;

		if (uevent_vals[Env_RECOVERY]) {
			if (strcmp(uevent_vals[Env_RECOVERY], "Done") == 0)
				status = LM_RD_SUCCESS;
			if (strcmp(uevent_vals[Env_RECOVERY], "Failed") == 0)
				status = LM_RD_GAVEUP;
		}

		if (uevent_vals[Env_FIRSTMOUNT] &&
		    (strcmp(uevent_vals[Env_FIRSTMOUNT], "Done") == 0))
			first = 1;

		if (group_mode == GROUP_LIBGROUP)
			process_recovery_uevent_old(fsname, jid, status, first);
		else
			process_recovery_uevent(fsname, jid, status, first);

	} else if (!strcmp(uevent_vals[Env_ACTION], "offline")) {
		do_withdraw(fsname);
	} else {
		ping_kernel_mount(fsname);
	}
}

static int setup_uevent(void)
{
	struct sockaddr_nl snl;
	int s, rv;

	s = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
	if (s < 0) {
		log_error("uevent netlink socket");
		return s;
	}

	memset(&snl, 0, sizeof(snl));
	snl.nl_family = AF_NETLINK;
	snl.nl_pid = getpid();
	snl.nl_groups = 1;

	rv = bind(s, (struct sockaddr *) &snl, sizeof(snl));
	if (rv < 0) {
		log_error("uevent bind error %d errno %d", rv, errno);
		close(s);
		return rv;
	}

	return s;
}

static void init_header(struct gfsc_header *h, int cmd, char *name, int result,
			int extra_len)
{
	memset(h, 0, sizeof(struct gfsc_header));

	h->magic = GFSC_MAGIC;
	h->version = GFSC_VERSION;
	h->len = sizeof(struct gfsc_header) + extra_len;
	h->command = cmd;
	h->data = result;

	if (name)
		strncpy(h->name, name, GFS_MOUNTGROUP_LEN);
}

static void query_dump_debug(int fd)
{
	struct gfsc_header h;
	int extra_len;
	int len;

	/* in the case of dump_wrap, extra_len will go in two writes,
	   first the log tail, then the log head */
	if (dump_wrap)
		extra_len = GFSC_DUMP_SIZE;
	else
		extra_len = dump_point;

	init_header(&h, GFSC_CMD_DUMP_DEBUG, NULL, 0, extra_len);
	do_write(fd, &h, sizeof(h));

	if (dump_wrap) {
		len = GFSC_DUMP_SIZE - dump_point;
		do_write(fd, dump_buf + dump_point, len);
		len = dump_point;
	} else
		len = dump_point;

	/* NUL terminate the debug string */
	dump_buf[dump_point] = '\0';

	do_write(fd, dump_buf, len);
}

static void query_dump_plocks(int fd, char *name)
{
	struct mountgroup *mg;
	struct gfsc_header h;
	int rv;

	mg = find_mg(name);
	if (!mg) {
		plock_dump_len = 0;
		rv = -ENOENT;
	} else {
		/* writes to plock_dump_buf and sets plock_dump_len */
		rv = fill_plock_dump_buf(mg);
	}

	init_header(&h, GFSC_CMD_DUMP_PLOCKS, name, rv, plock_dump_len);

	do_write(fd, &h, sizeof(h));

	if (plock_dump_len)
		do_write(fd, plock_dump_buf, plock_dump_len);
}

/* combines a header and the data and sends it back to the client in
   a single do_write() call */

static void do_reply(int fd, int cmd, char *name, int result, void *buf,
		     int buflen)
{
	char *reply;
	int reply_len;

	reply_len = sizeof(struct gfsc_header) + buflen;
	reply = malloc(reply_len);
	if (!reply)
		return;
	memset(reply, 0, reply_len);

	init_header((struct gfsc_header *)reply, cmd, name, result, buflen);

	if (buf && buflen)
		memcpy(reply + sizeof(struct gfsc_header), buf, buflen);

	do_write(fd, reply, reply_len);

	free(reply);
}

static void query_mountgroup_info(int fd, char *name)
{
	struct mountgroup *mg;
	struct gfsc_mountgroup mountgroup;
	int rv;

	mg = find_mg(name);
	if (!mg) {
		rv = -ENOENT;
		goto out;
	}

	memset(&mountgroup, 0, sizeof(mountgroup));
	mountgroup.group_mode = group_mode;

	if (group_mode == GROUP_LIBGROUP)
		rv = set_mountgroup_info_group(mg, &mountgroup);
	else
		rv = set_mountgroup_info(mg, &mountgroup);
 out:
	do_reply(fd, GFSC_CMD_MOUNTGROUP_INFO, name, rv,
		 (char *)&mountgroup, sizeof(mountgroup));
}

static void query_node_info(int fd, char *name, int nodeid)
{
	struct mountgroup *mg;
	struct gfsc_node node;
	int rv;

	mg = find_mg(name);
	if (!mg) {
		rv = -ENOENT;
		goto out;
	}

	if (group_mode == GROUP_LIBGROUP)
		rv = set_node_info_group(mg, nodeid, &node);
	else
		rv = set_node_info(mg, nodeid, &node);
 out:
	do_reply(fd, GFSC_CMD_NODE_INFO, name, rv,
		 (char *)&node, sizeof(node));
}

static void query_mountgroups(int fd, int max)
{
	int mg_count = 0;
	struct gfsc_mountgroup *mgs = NULL;
	int rv, result;

	if (group_mode == GROUP_LIBGROUP)
		rv = set_mountgroups_group(&mg_count, &mgs);
	else
		rv = set_mountgroups(&mg_count, &mgs);

	if (rv < 0) {
		result = rv;
		mg_count = 0;
		goto out;
	}

	if (mg_count > max) {
		result = -E2BIG;
		mg_count = max;
	} else {
		result = mg_count;
	}
 out:
	do_reply(fd, GFSC_CMD_MOUNTGROUPS, NULL, result,
		 (char *)mgs, mg_count * sizeof(struct gfsc_mountgroup));

	if (mgs)
		free(mgs);
}

static void query_mountgroup_nodes(int fd, char *name, int option, int max)
{
	struct mountgroup *mg;
	int node_count = 0;
	struct gfsc_node *nodes = NULL;
	int rv, result;

	mg = find_mg(name);
	if (!mg) {
		result = -ENOENT;
		node_count = 0;
		goto out;
	}

	if (group_mode == GROUP_LIBGROUP)
		rv = set_mountgroup_nodes_group(mg, option, &node_count, &nodes);
	else
		rv = set_mountgroup_nodes(mg, option, &node_count, &nodes);

	if (rv < 0) {
		result = rv;
		node_count = 0;
		goto out;
	}

	/* node_count is the number of structs copied/returned; the caller's
	   max may be less than that, in which case we copy as many as they
	   asked for and return -E2BIG */

	if (node_count > max) {
		result = -E2BIG;
		node_count = max;
	} else {
		result = node_count;
	}
 out:
	do_reply(fd, GFSC_CMD_MOUNTGROUP_NODES, name, result,
		 (char *)nodes, node_count * sizeof(struct gfsc_node));

	if (nodes)
		free(nodes);
}

void client_reply_join(int ci, struct gfsc_mount_args *ma, int result)
{
	char *name = strstr(ma->table, ":") + 1;

	log_debug("client_reply_join %s ci %d result %d", name, ci, result);

	do_reply(client[ci].fd, GFSC_CMD_FS_JOIN,
		 name, result, ma, sizeof(struct gfsc_mount_args));
}

void client_reply_join_full(struct mountgroup *mg, int result)
{
	char nodir_str[32];

	if (result)
		goto out;

	if (mg->our_jid < 0) {
		snprintf(mg->mount_args.hostdata, PATH_MAX,
			 "hostdata=id=%u:first=%d",
			 mg->id, mg->first_mounter);
	} else {
		snprintf(mg->mount_args.hostdata, PATH_MAX,
			 "hostdata=jid=%d:id=%u:first=%d",
			 mg->our_jid, mg->id, mg->first_mounter);
	}

	memset(nodir_str, 0, sizeof(nodir_str));

	read_ccs_nodir(mg, nodir_str);
	if (nodir_str[0])
		strcat(mg->mount_args.hostdata, nodir_str);
 out:
	log_group(mg, "client_reply_join_full ci %d result %d %s",
		  mg->mount_client, result, mg->mount_args.hostdata);

	client_reply_join(mg->mount_client, &mg->mount_args, result);
}

static void do_join(int ci, struct gfsc_mount_args *ma)
{
	struct mountgroup *mg = NULL;
	char table2[PATH_MAX];
	char *cluster = NULL, *name = NULL;
	int rv;

	log_debug("join: %s %s %s %s %s %s", ma->dir, ma->type, ma->proto,
		  ma->table, ma->options, ma->dev);

	if (strcmp(ma->proto, "lock_dlm")) {
		log_error("join: lockproto %s not supported", ma->proto);
		rv = -EPROTONOSUPPORT;
		goto fail;
	}

	if (strstr(ma->options, "jid=") ||
	    strstr(ma->options, "first=") ||
	    strstr(ma->options, "id=")) {
		log_error("join: jid, first and id are reserved options");
		rv = -EOPNOTSUPP;
		goto fail;
	}

	/* table is <cluster>:<name> */

	memset(table2, 0, sizeof(table2));
	strncpy(table2, ma->table, sizeof(table2));

	name = strstr(table2, ":");
	if (!name) {
		rv = -EBADFD;
		goto fail;
	}

	*name = '\0';
	name++;
	cluster = table2;

	if (strlen(name) > GFS_MOUNTGROUP_LEN) {
		rv = -ENAMETOOLONG;
		goto fail;
	}

	mg = find_mg(name);
	if (mg) {
		if (strcmp(mg->mount_args.dev, ma->dev)) {
			log_error("different fs dev %s with same name",
				  mg->mount_args.dev);
			rv = -EADDRINUSE;
		} else if (mg->leaving) {
			/* we're leaving the group */
			log_error("join: reject mount due to unmount");
			rv = -ESTALE;
		} else if (mg->mount_client || !mg->kernel_mount_done) {
			log_error("join: other mount in progress %d %d",
				  mg->mount_client, mg->kernel_mount_done);
			rv = -EBUSY;
		} else {
			log_group(mg, "join: already mounted");
			rv = -EALREADY;
		}
		goto fail;
	}

	mg = create_mg(name);
	if (!mg) {
		rv = -ENOMEM;
		goto fail;
	}
	mg->mount_client = ci;
	memcpy(&mg->mount_args, ma, sizeof(struct gfsc_mount_args));

	if (strlen(cluster) != strlen(clustername) ||
	    strlen(cluster) == 0 || strcmp(cluster, clustername)) {
		log_error("join: fs requires cluster=\"%s\" current=\"%s\"",
			  cluster, clustername);
		rv = -EBADR;
		goto fail_free;
	}
	log_group(mg, "join: cluster name matches: %s", clustername);

	if (strstr(ma->options, "spectator")) {
		log_group(mg, "join: spectator mount");
		mg->spectator = 1;
	} else {
		if (!we_are_in_fence_domain()) {
			log_error("join: not in default fence domain");
			rv = -ENOANO;
			goto fail_free;
		}
	}

	if (!mg->spectator && strstr(ma->options, "rw"))
		mg->rw = 1;
	else if (strstr(ma->options, "ro")) {
		if (mg->spectator) {
			log_error("join: readonly invalid with spectator");
			rv = -EROFS;
			goto fail_free;
		}
		mg->ro = 1;
	}

	list_add(&mg->list, &mountgroups);

	if (group_mode == GROUP_LIBGROUP)
		rv = gfs_join_mountgroup_old(mg, ma);
	else
		rv = gfs_join_mountgroup(mg);

	if (rv) {
		log_error("join: group join error %d", rv);
		list_del(&mg->list);
		goto fail_free;
	}
	return;

 fail_free:
	free(mg);
 fail:
	client_reply_join(ci, ma, rv);
}

/* The basic rule of withdraw is that we don't want to tell the kernel to drop
   all locks until we know gfs has been stopped/blocked on all nodes.
   A withdrawing node is very much like a readonly node, differences are
   that others recover its journal when they remove it from the group,
   and when it's been removed from the group, it tells the locally withdrawing
   gfs to clear out locks. */

static void do_withdraw(char *name)
{
	struct mountgroup *mg;
	int rv;

	log_debug("withdraw: %s", name);

	if (!cfgd_enable_withdraw) {
		log_error("withdraw feature not enabled");
		return;
	}

	mg = find_mg(name);
	if (!mg) {
		log_error("do_withdraw no mountgroup %s", name);
		return;
	}

	mg->withdraw_uevent = 1;

	rv = run_dmsetup_suspend(mg, mg->mount_args.dev);
	if (rv) {
		log_error("do_withdraw %s: dmsetup %s error %d", mg->name,
			  mg->mount_args.dev, rv);
		return;
	}

	dmsetup_wait = 1;
}

static void do_mount_done(char *table, int result)
{
	struct mountgroup *mg;
	char *name = strstr(table, ":") + 1;

	log_debug("mount_done: %s result %d", name, result);

	mg = find_mg(name);
	if (!mg) {
		log_error("mount_done: %s not found", name);
		return;
	}

	mg->mount_client = 0;
	mg->kernel_mount_done = 1;
	mg->kernel_mount_error = result;

	if (group_mode == GROUP_LIBGROUP)
		send_mount_status_old(mg);
	else
		gfs_mount_done(mg);
}

void client_reply_remount(struct mountgroup *mg, int ci, int result)
{
	do_reply(client[ci].fd, GFSC_CMD_FS_REMOUNT, mg->name, result,
		 &mg->mount_args, sizeof(struct gfsc_mount_args));
}

/* mount.gfs creates a special ma->options string with only "ro" or "rw" */

static void do_remount(int ci, struct gfsc_mount_args *ma)
{
	struct mountgroup *mg;
	char *name = strstr(ma->table, ":") + 1;
	int ro = 0, result = 0;

	log_debug("remount: %s ci %d options %s", name, ci, ma->options);

	mg = find_mg(name);
	if (!mg) {
		log_error("remount: %s not found", name);
		result = -1;
		goto out;
	}

	if (mg->spectator) {
		log_error("remount of spectator not allowed");
		result = -1;
		goto out;
	}

	if (!strcmp(ma->options, "ro"))
		ro = 1;

	if ((mg->ro && ro) || (!mg->ro && !ro))
		goto out;

	if (group_mode == GROUP_LIBGROUP) {
		/* the receive calls client_reply_remount */
		mg->remount_client = ci;
		send_remount_old(mg, ma);
		return;
	}

	send_remount(mg, ma);
 out:
	client_reply_remount(mg, ci, result);
}

void process_connection(int ci)
{
	struct gfsc_header h;
	struct gfsc_mount_args empty;
	struct gfsc_mount_args *ma;
	char *extra = NULL;
	char *fsname;
	int rv, extra_len;

	rv = do_read(client[ci].fd, &h, sizeof(h));
	if (rv < 0) {
		log_debug("connection %d read error %d", ci, rv);
		goto out;
	}

	if (h.magic != GFSC_MAGIC) {
		log_debug("connection %d magic error %x", ci, h.magic);
		goto out;
	}

	if ((h.version & 0xFFFF0000) != (GFSC_VERSION & 0xFFFF0000)) {
		log_debug("connection %d version error %x", ci, h.version);
		goto out;
	}

	if (h.len > sizeof(h)) {
		extra_len = h.len - sizeof(h);
		extra = malloc(extra_len);
		if (!extra) {
			log_error("process_connection no mem %d", extra_len);
			goto out;
		}
		memset(extra, 0, extra_len);

		rv = do_read(client[ci].fd, extra, extra_len);
		if (rv < 0) {
			log_debug("connection %d extra read error %d", ci, rv);
			goto out;
		}
	}

	ma = (struct gfsc_mount_args *)extra;

	if (!ma) {
		memset(&empty, 0, sizeof(empty));

		if (h.command == GFSC_CMD_FS_JOIN ||
		    h.command == GFSC_CMD_FS_REMOUNT) {
			do_reply(client[ci].fd, h.command, h.name, -EINVAL,
				 &empty, sizeof(empty));
		}
		log_debug("connection %d cmd %d no data", ci, h.command);
		goto out;
	}

	switch (h.command) {

	case GFSC_CMD_FS_JOIN:
		do_join(ci, ma);
		break;

	case GFSC_CMD_FS_LEAVE:
		fsname = strstr(ma->table, ":") + 1;
		if (!fsname) {
			log_error("process_connection no fsname in table");
			goto out;
		}

		if (group_mode == GROUP_LIBGROUP)
			do_leave_old(fsname, h.data);
		else
			do_leave(fsname, h.data);
		break;

	case GFSC_CMD_FS_MOUNT_DONE:
		do_mount_done(ma->table, h.data);
		break;

	case GFSC_CMD_FS_REMOUNT:
		do_remount(ci, ma);
		break;

	default:
		log_error("process_connection %d unknown command %d",
			  ci, h.command);
	}
 out:
	if (extra)
		free(extra);

	/* no client_dead(ci) here, since the connection for
	   join/remount is reused */
}

static void process_listener(int ci)
{
	int fd, i;

	fd = accept(client[ci].fd, NULL, NULL);
	if (fd < 0) {
		log_error("process_listener: accept error %d %d", fd, errno);
		return;
	}

	i = client_add(fd, process_connection, NULL);

	log_debug("client connection %d fd %d", i, fd);
}

static int setup_listener(const char *sock_path)
{
	struct sockaddr_un addr;
	socklen_t addrlen;
	int rv, s;

	/* we listen for new client connections on socket s */

	s = socket(AF_LOCAL, SOCK_STREAM, 0);
	if (s < 0) {
		log_error("socket error %d %d", s, errno);
		return s;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_LOCAL;
	strcpy(&addr.sun_path[1], sock_path);
	addrlen = sizeof(sa_family_t) + strlen(addr.sun_path+1) + 1;

	rv = bind(s, (struct sockaddr *) &addr, addrlen);
	if (rv < 0) {
		log_error("bind error %d %d", rv, errno);
		close(s);
		return rv;
	}

	rv = listen(s, 5);
	if (rv < 0) {
		log_error("listen error %d %d", rv, errno);
		close(s);
		return rv;
	}
	return s;
}

void query_lock(void)
{
	pthread_mutex_lock(&query_mutex);
}

void query_unlock(void)
{
	pthread_mutex_unlock(&query_mutex);
}

/* This is a thread, so we have to be careful, don't call log_ functions.
   We need a thread to process queries because the main thread may block
   for long periods. */

static void *process_queries(void *arg)
{
	struct gfsc_header h;
	int f, rv, s;

	rv = setup_listener(GFSC_QUERY_SOCK_PATH);
	if (rv < 0)
		return NULL;

	s = rv;

	for (;;) {
		f = accept(s, NULL, NULL);
		if (f < 0)
			return NULL;

		rv = do_read(f, &h, sizeof(h));
		if (rv < 0) {
			goto out;
		}

		if (h.magic != GFSC_MAGIC) {
			goto out;
		}

		if ((h.version & 0xFFFF0000) != (GFSC_VERSION & 0xFFFF0000)) {
			goto out;
		}

		query_lock();

		switch (h.command) {
		case GFSC_CMD_DUMP_DEBUG:
			query_dump_debug(f);
			break;
		case GFSC_CMD_DUMP_PLOCKS:
			query_dump_plocks(f, h.name);
			break;
		case GFSC_CMD_MOUNTGROUP_INFO:
			query_mountgroup_info(f, h.name);
			break;
		case GFSC_CMD_NODE_INFO:
			query_node_info(f, h.name, h.data);
			break;
		case GFSC_CMD_MOUNTGROUPS:
			query_mountgroups(f, h.data);
			break;
		case GFSC_CMD_MOUNTGROUP_NODES:
			query_mountgroup_nodes(f, h.name, h.option, h.data);
			break;
		default:
			break;
		}
		query_unlock();

 out:
		close(f);
	}
}

static int setup_queries(void)
{
	int rv;

	pthread_mutex_init(&query_mutex, NULL);

	rv = pthread_create(&query_thread, NULL, process_queries, NULL);
	if (rv < 0) {
		log_error("can't create query thread");
		return rv;
	}
	return 0;
}

void cluster_dead(int ci)
{
	if (!cluster_down)
		log_error("cluster is down, exiting");
	daemon_quit = 1;
	cluster_down = 1;
}

static void loop(void)
{
	int poll_timeout = -1;
	int rv, i;
	void (*workfn) (int ci);
	void (*deadfn) (int ci);

	rv = setup_queries();
	if (rv < 0)
		goto out;

	rv = setup_listener(GFSC_SOCK_PATH);
	if (rv < 0)
		goto out;
	client_add(rv, process_listener, NULL);

	rv = setup_cluster();
	if (rv < 0)
		goto out;
	client_add(rv, process_cluster, cluster_dead);

	update_cluster();

	rv = setup_ccs();
	if (rv < 0)
		goto out;

	setup_logging();

	rv = check_uncontrolled_filesystems();
	if (rv < 0)
		goto out;

	rv = setup_uevent();
	if (rv < 0)
		goto out;
	client_add(rv, process_uevent, NULL);

	group_mode = GROUP_LIBCPG;

	if (cfgd_groupd_compat) {
		rv = setup_groupd();
		if (rv < 0)
			goto out;
		client_add(rv, process_groupd, cluster_dead);

		switch (cfgd_groupd_compat) {
		case 1:
			group_mode = GROUP_LIBGROUP;
			rv = 0;
			break;
		case 2:
			rv = set_group_mode();
			break;
		default:
			log_error("inval groupd_compat %d", cfgd_groupd_compat);
			rv = -1;
			break;
		}
		if (rv < 0)
			goto out;
	}
	log_debug("group_mode %d compat %d", group_mode, cfgd_groupd_compat);

	if (group_mode == GROUP_LIBCPG) {

		/*
		 * The new, good, way of doing things using libcpg directly.
		 * code in: cpg-new.c
		 */

		rv = setup_cpg_daemon();
		if (rv < 0)
			goto out;
		client_add(rv, process_cpg_daemon, cluster_dead);

		rv = set_protocol();
		if (rv < 0)
			goto out;

		rv = setup_dlmcontrol();
		if (rv < 0)
			goto out;
		client_add(rv, process_dlmcontrol, cluster_dead);

	} else if (group_mode == GROUP_LIBGROUP) {

		/* the stable2 default is zero plock_ownership which we need
		   to match or protocol_active will differ */
		if (using_default_plock_ownership)
			cfgd_plock_ownership = 0;

		/*
		 * The old, bad, way of doing things using libgroup.
		 * code in: cpg-old.c group.c plock.c
		 */

		rv = setup_cpg_old();
		if (rv < 0)
			goto out;
		client_add(rv, process_cpg_old, cluster_dead);

		rv = setup_misc_devices();
		if (rv < 0)
			goto out;

		rv = setup_plocks();
		if (rv < 0)
			goto out;
		plock_fd = rv;
		plock_ci = client_add(rv, process_plocks, NULL);
	}

	for (;;) {
		rv = poll(pollfd, client_maxi + 1, poll_timeout);
		if (rv == -1 && errno == EINTR) {
			if (daemon_quit && list_empty(&mountgroups))
				goto out;
			daemon_quit = 0;
			continue;
		}
		if (rv < 0) {
			log_error("poll errno %d", errno);
			goto out;
		}

		query_lock();

		for (i = 0; i <= client_maxi; i++) {
			if (client[i].fd < 0)
				continue;
			if (pollfd[i].revents & POLLIN) {
				workfn = client[i].workfn;
				workfn(i);
			}
			if (pollfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				deadfn = client[i].deadfn;
				deadfn(i);
			}
		}
		query_unlock();

		if (daemon_quit)
			break;

		query_lock();

		poll_timeout = -1;

		if (poll_dlm) {
			/* only happens for GROUP_LIBCPG */
			process_mountgroups();
			poll_timeout = 500;
		}

		if (poll_ignore_plock) {
			/* only happens for GROUP_LIBGROUP */
			if (!limit_plocks()) {
				poll_ignore_plock = 0;
				client_back(plock_ci, plock_fd);
			}
			poll_timeout = 1000;
		}

		if (dmsetup_wait) {
			update_dmsetup_wait();
			if (dmsetup_wait) {
				if (poll_timeout == -1)
					poll_timeout = 1000;
			} else {
				if (poll_timeout == 1000)
					poll_timeout = -1;
			}
		}
		query_unlock();
	}
 out:
	if (group_mode == GROUP_LIBCPG)
		close_cpg_daemon();
	else if (group_mode == GROUP_LIBGROUP) {
		close_plocks();
		close_cpg_old();
	}
	if (cfgd_groupd_compat)
		close_groupd();
	close_logging();
	close_ccs();
	close_cluster();

	if (!list_empty(&mountgroups))
		log_error("mountgroups abandoned");
}

static void lockfile(void)
{
	int fd, error;
	struct flock lock;
	char buf[33];

	memset(buf, 0, 33);

	fd = open(LOCKFILE_NAME, O_CREAT|O_WRONLY,
		  S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
	if (fd < 0) {
		fprintf(stderr, "cannot open/create lock file %s\n",
			LOCKFILE_NAME);
		exit(EXIT_FAILURE);
	}

	lock.l_type = F_WRLCK;
	lock.l_start = 0;
	lock.l_whence = SEEK_SET;
	lock.l_len = 0;

	error = fcntl(fd, F_SETLK, &lock);
	if (error) {
		fprintf(stderr, "gfs_controld is already running\n");
		exit(EXIT_FAILURE);
	}

	error = ftruncate(fd, 0);
	if (error) {
		fprintf(stderr, "cannot clear lock file %s\n", LOCKFILE_NAME);
		exit(EXIT_FAILURE);
	}

	sprintf(buf, "%d\n", getpid());

	error = write(fd, buf, strlen(buf));
	if (error <= 0) {
		fprintf(stderr, "cannot write lock file %s\n", LOCKFILE_NAME);
		exit(EXIT_FAILURE);
	}
}

static void print_usage(void)
{
	printf("Usage:\n");
	printf("\n");
	printf("gfs_controld [options]\n");
	printf("\n");
	printf("Options:\n");
	printf("\n");
	printf("  -D           Enable debugging to stderr and don't fork\n");
	printf("  -L           Enable debugging to log file\n");
	printf("  -g <num>     groupd compatibility mode, 0 off, 1 on, 2 detect\n");
	printf("               0: use libcpg, no backward compat, best performance\n");
	printf("               1: use libgroup for compat with cluster2/rhel5\n");
	printf("               2: use groupd to detect old, or mode 1, nodes that\n"
	       "               require compat, use libcpg if none found\n");
	printf("               Default is %d\n", DEFAULT_GROUPD_COMPAT);
	printf("  -w <num>     Enable (1) or disable (0) withdraw\n");
	printf("               Default is %d\n", DEFAULT_ENABLE_WITHDRAW);
	printf("  -p <num>     Enable (1) or disable (0) plock code\n");
	printf("               Default is %d\n", DEFAULT_ENABLE_PLOCK);
	printf("  -P           Enable plock debugging\n");

	printf("  -l <limit>   Limit the rate of plock operations\n");
	printf("               Default is %d, set to 0 for no limit\n", DEFAULT_PLOCK_RATE_LIMIT);
	printf("  -o <n>       Enable (1) or disable (0) plock ownership\n");
	printf("               Default is %d\n", DEFAULT_PLOCK_OWNERSHIP);
	printf("  -t <ms>      plock ownership drop resources time (milliseconds)\n");
	printf("               Default is %u\n", DEFAULT_DROP_RESOURCES_TIME);
	printf("  -c <num>     plock ownership drop resources count\n");
	printf("               Default is %u\n", DEFAULT_DROP_RESOURCES_COUNT);
	printf("  -a <ms>      plock ownership drop resources age (milliseconds)\n");
	printf("               Default is %u\n", DEFAULT_DROP_RESOURCES_AGE);
	printf("  -h           Print this help, then exit\n");
	printf("  -V           Print program version information, then exit\n");
}

#define OPTION_STRING "LDKg:w:f:q:d:p:Pl:o:t:c:a:hV"

static void read_arguments(int argc, char **argv)
{
	int cont = 1;
	int optchar;

	while (cont) {
		optchar = getopt(argc, argv, OPTION_STRING);

		switch (optchar) {

		case 'D':
			daemon_debug_opt = 1;
			break;

		case 'L':
			optd_debug_logfile = 1;
			cfgd_debug_logfile = 1;
			break;

		case 'g':
			optd_groupd_compat = 1;
			cfgd_groupd_compat = atoi(optarg);
			break;

		case 'w':
			optd_enable_withdraw = 1;
			cfgd_enable_withdraw = atoi(optarg);
			break;

		case 'p':
			optd_enable_plock = 1;
			cfgd_enable_plock = atoi(optarg);
			break;

		case 'P':
			optd_plock_debug = 1;
			cfgd_plock_debug = 1;
			break;

		case 'l':
			optd_plock_rate_limit = 1;
			cfgd_plock_rate_limit = atoi(optarg);
			break;

		case 'o':
			optd_plock_ownership = 1;
			cfgd_plock_ownership = atoi(optarg);
			break;

		case 't':
			optd_drop_resources_time = 1;
			cfgd_drop_resources_time = atoi(optarg);
			break;

		case 'c':
			optd_drop_resources_count = 1;
			cfgd_drop_resources_count = atoi(optarg);
			break;

		case 'a':
			optd_drop_resources_age = 1;
			cfgd_drop_resources_age = atoi(optarg);
			break;

		case 'h':
			print_usage();
			exit(EXIT_SUCCESS);
			break;

		case 'V':
			printf("gfs_controld %s (built %s %s)\n",
				RELEASE_VERSION, __DATE__, __TIME__);
			printf("%s\n", REDHAT_COPYRIGHT);
			exit(EXIT_SUCCESS);
			break;

		case ':':
		case '?':
			fprintf(stderr, "Please use '-h' for usage.\n");
			exit(EXIT_FAILURE);
			break;

		case EOF:
			cont = 0;
			break;

		default:
			fprintf(stderr, "unknown option: %c\n", optchar);
			exit(EXIT_FAILURE);
			break;
		};
	}

	if (getenv("GFS_CONTROLD_DEBUG")) {
		optd_debug_logfile = 1;
		cfgd_debug_logfile = 1;
	}
}

static void set_oom_adj(int val)
{
	FILE *fp;

	fp = fopen("/proc/self/oom_adj", "w");
	if (!fp)
		return;

	fprintf(fp, "%i", val);
	fclose(fp);
}

static void set_scheduler(void)
{
	struct sched_param sched_param;
	int rv;

	rv = sched_get_priority_max(SCHED_RR);
	if (rv != -1) {
		sched_param.sched_priority = rv;
		rv = sched_setscheduler(0, SCHED_RR, &sched_param);
		if (rv == -1)
			log_error("could not set SCHED_RR priority %d err %d",
				   sched_param.sched_priority, errno);
	} else {
		log_error("could not get maximum scheduler priority err %d",
			  errno);
	}
}

int main(int argc, char **argv)
{
	INIT_LIST_HEAD(&mountgroups);
	INIT_LIST_HEAD(&withdrawn_mounts);

	read_arguments(argc, argv);

	if (!daemon_debug_opt) {
		if (daemon(0, 0) < 0) {
			perror("daemon error");
			exit(EXIT_FAILURE);
		}
	}
	lockfile();
	init_logging();
	log_level(LOG_INFO, "gfs_controld %s started", RELEASE_VERSION);
	signal(SIGTERM, sigterm_handler);
	set_scheduler();
	set_oom_adj(-16);

	loop();

	unlink(LOCKFILE_NAME);
	return 0;
}

void daemon_dump_save(void)
{
	int len, i;

	len = strlen(daemon_debug_buf);

	for (i = 0; i < len; i++) {
		dump_buf[dump_point++] = daemon_debug_buf[i];

		if (dump_point == GFSC_DUMP_SIZE) {
			dump_point = 0;
			dump_wrap = 1;
		}
	}
}

int daemon_debug_opt;
int daemon_quit;
int cluster_down;
int poll_ignore_plock;
int poll_dlm;
int plock_fd;
int plock_ci;
struct list_head mountgroups;
int our_nodeid;
char *clustername;
char daemon_debug_buf[256];
char dump_buf[GFSC_DUMP_SIZE];
int dump_point;
int dump_wrap;
char plock_dump_buf[GFSC_DUMP_SIZE];
int plock_dump_len;
int dmsetup_wait;
cpg_handle_t cpg_handle_daemon;
int libcpg_flow_control_on;
int group_mode;
uint32_t plock_minor;
uint32_t old_plock_minor;
struct list_head withdrawn_mounts;
int using_default_plock_ownership;

