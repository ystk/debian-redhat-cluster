#ifndef __FD_DOT_H__
#define __FD_DOT_H__

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sched.h>
#include <limits.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/time.h>

#include <openais/saAis.h>
#include <corosync/cpg.h>
#include <liblogthread.h>

#include "list.h"
#include "linux_endian.h"
#include "libfence.h"
#include "libfenced.h"
#include "fenced.h"

/* Max name length for a group, pointless since we only ever create the
   "default" group.  Regardless, set arbitrary max to match dlm's
   DLM_LOCKSPACE_LEN 64.  The libcpg limit is larger at 128; we prefix
   the fence domain name with "fenced:" to create the cpg name. */

#define MAX_GROUPNAME_LEN	64

/* Max name length for a node.  This should match libcman's
   CMAN_MAX_NODENAME_LEN which is 255. */

#define MAX_NODENAME_LEN	255

/* Maximum members of the fence domain, or cluster.  Should match
   CPG_MEMBERS_MAX in openais/cpg.h. */

#define MAX_NODES		128

/* Max string length printed on a line, for debugging/dump output. */

#define MAXLINE			256

/* group_mode */

#define GROUP_LIBGROUP          2
#define GROUP_LIBCPG            3

extern int daemon_debug_opt;
extern int daemon_quit;
extern int cluster_down;
extern struct list_head domains;
extern int cluster_quorate;
extern int cluster_quorate_from_last_update;
extern uint32_t cluster_ringid_seq;
extern uint64_t quorate_time;
extern int our_nodeid;
extern char our_name[MAX_NODENAME_LEN+1];
extern char daemon_debug_buf[256];
extern char dump_buf[FENCED_DUMP_SIZE];
extern int dump_point;
extern int dump_wrap;
extern int group_mode;

extern void daemon_dump_save(void);

#define log_level(lvl, fmt, args...) \
do { \
	snprintf(daemon_debug_buf, 255, "%ld " fmt "\n", time(NULL), ##args); \
	daemon_dump_save(); \
	logt_print(lvl, fmt "\n", ##args); \
	if (daemon_debug_opt) \
		fprintf(stderr, "%s", daemon_debug_buf); \
} while (0)

#define log_debug(fmt, args...) log_level(LOG_DEBUG, fmt, ##args)
#define log_error(fmt, args...) log_level(LOG_ERR, fmt, ##args)

#define FD_MSG_PROTOCOL		1
#define FD_MSG_START		2
#define FD_MSG_VICTIM_DONE	3
#define FD_MSG_COMPLETE		4
#define FD_MSG_EXTERNAL		5

#define FD_MFLG_JOINING		1  /* accompanies start, we are joining */
#define FD_MFLG_COMPLETE	2  /* accompanies start, we have complete info */
#define FD_MFLG_DUPLICATE_CG	4

struct fd_header {
	uint16_t version[3];
	uint16_t type;		/* FD_MSG_ */
	uint32_t nodeid;	/* sender */
	uint32_t to_nodeid;     /* recipient, 0 for all */
	uint32_t global_id;     /* global unique id for this domain */
	uint32_t flags;		/* FD_MFLG_ */
	uint32_t msgdata;       /* in-header payload depends on MSG type */
	uint32_t pad1;
	uint64_t pad2;
};

#define CGST_WAIT_CONDITIONS	1
#define CGST_WAIT_MESSAGES	2
#define CGST_WAIT_FENCING	3	/* for queries */

struct change {
	struct list_head list;
	struct list_head members;
	struct list_head removed; /* nodes removed by this change */
	int member_count;
	int joined_count;
	int remove_count;
	int failed_count;
	int state; /* CGST_ */
	int we_joined;
	int sent_start;
	uint32_t seq; /* just used as a reference when debugging */
	uint64_t create_time;
};

#define VIC_DONE_AGENT		1
#define VIC_DONE_MEMBER		2
#define VIC_DONE_OVERRIDE	3
#define VIC_DONE_EXTERNAL	4

struct node_history {
	struct list_head list;
	int nodeid;
	int check_quorum;
	uint64_t add_time;
	uint64_t left_time;
	uint64_t fail_time;
	uint64_t fence_time;
	uint64_t fence_time_local;
	uint64_t fence_external_time;
	uint64_t cluster_add_time;
	uint64_t cluster_remove_time;
	int fence_external_node;
	int fence_master;
	int fence_how; /* VIC_DONE_ */
	uint32_t last_match_seq;
	uint32_t fail_seq;
	uint32_t left_seq;
};

struct node {
	struct list_head 	list;
	int			nodeid;
	int			init_victim;
	int			local_victim_done;
	char 			name[MAX_NODENAME_LEN+1];
};

struct fd {
	struct list_head	list;
	char 			name[MAX_GROUPNAME_LEN+1];

	/* libcpg domain membership */

	cpg_handle_t		cpg_handle;
	int			cpg_client;
	int			cpg_fd;
	uint32_t		change_seq;
	uint32_t		started_count;
	struct change		*started_change;
	struct list_head	changes;
	struct list_head	node_history;
	int			init_complete;
	int			local_init_complete;
	struct cpg_ring_id	cpg_ringid;

	/* general domain membership */

	int			master;
	int			joining_group;
	int			leaving_group;
	int			current_victim; /* for queries */
	struct list_head 	victims;
	struct list_head	complete;

	/* libgroup domain membership */

	int 			last_stop;
	int 			last_start;
	int 			last_finish;
	int			first_recovery;
	int 			prev_count;
	struct list_head 	prev;
	struct list_head 	leaving;
};

/* config.c */

int setup_ccs(void);
void close_ccs(void);
void reread_ccs(void);
void read_ccs_name(const char *path, char *name);
void read_ccs_yesno(const char *path, int *yes, int *no);
void read_ccs_int(const char *path, int *config_val);
int read_ccs(struct fd *fd);

/* cpg.c */

void process_cpg_daemon(int ci);
int setup_cpg_daemon(void);
void close_cpg_daemon(void);
int set_protocol(void);
void free_cg(struct change *cg);
void node_history_init(struct fd *fd, int nodeid);
void node_history_fence(struct fd *fd, int victim, int master, int how,
			uint64_t mastertime);
void send_external(struct fd *fd, int victim);
int is_fenced_external(struct fd *fd, int nodeid);
void send_victim_done(struct fd *fd, int victim);
void process_fd_changes(void);
int fd_join(struct fd *fd);
int fd_leave(struct fd *fd);
int set_node_info(struct fd *fd, int nodeid, struct fenced_node *node);
int set_domain_info(struct fd *fd, struct fenced_domain *domain);
int set_domain_nodes(struct fd *fd, int option, int *node_count,
		     struct fenced_node **nodes);
int is_clean_daemon_member(int nodeid);
void node_history_cluster_add(int nodeid);
void node_history_cluster_remove(int nodeid);

/* group.c */

void process_groupd(int ci);
int setup_groupd(void);
void close_groupd(void);
int fd_join_group(struct fd *fd);
int fd_leave_group(struct fd *fd);
int set_node_info_group(struct fd *fd, int nodeid, struct fenced_node *node);
int set_domain_info_group(struct fd *fd, struct fenced_domain *domain);
int set_domain_nodes_group(struct fd *fd, int option, int *node_count,
			   struct fenced_node **nodes);
int set_group_mode(void);

/* main.c */

void client_dead(int ci);
int client_add(int fd, void (*workfn)(int ci), void (*deadfn)(int ci));
void free_fd(struct fd *fd);
struct fd *find_fd(char *name);
void query_lock(void);
void query_unlock(void);
void cluster_dead(int ci);

/* member_cman.c */

void process_cluster(int ci);
int setup_cluster(void);
void close_cluster(void);
int is_cluster_member_reread(int nodeid);
char *nodeid_to_name(int nodeid);
int name_to_nodeid(char *name);
struct node *get_new_node(struct fd *fd, int nodeid);
void kick_node_from_cluster(int nodeid);
void set_cman_dirty(void);

/* recover.c */

void free_node_list(struct list_head *head);
void add_complete_node(struct fd *fd, int nodeid);
int list_count(struct list_head *head);
int is_victim(struct fd *fd, int nodeid);
void delay_fencing(struct fd *fd, int node_join);
void defer_fencing(struct fd *fd);
void fence_victims(struct fd *fd);

/* logging.c */

void init_logging(void);
void setup_logging(void);
void close_logging(void);

/* dbus.c */

void fd_dbus_init(void);
void fd_dbus_exit(void);
void fd_dbus_send(const char *nodename, int nodeid, int result);

#endif				/*  __FD_DOT_H__  */

