#include <sys/types.h>
#include <sys/un.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <linux/dlmconstants.h>

#include "libgroup.h"
#include "groupd.h"
#include "libfenced.h"
#include "libdlmcontrol.h"
#include "libgfscontrol.h"
#include "copyright.cf"

#define GROUP_LIBGROUP			2
#define GROUP_LIBCPG			3

#define MAX_NODES			128
#define MAX_LS				128
#define MAX_MG				128
#define MAX_GROUPS			128

#define OP_LIST				1
#define OP_DUMP				2
#define OP_COMPAT			3

#define DEFAULT_GROUPD_COMPAT		0

static char *prog_name;
static int operation;
static int opt_ind;
static int verbose;
static int ls_all_nodes;
static int opt_groupd_compat;
static int cfg_groupd_compat = DEFAULT_GROUPD_COMPAT;


static int do_write(int fd, void *buf, size_t count)
{
	int rv, off = 0;

 retry:
	rv = write(fd, (char *)buf + off, count);
	if (rv == -1 && errno == EINTR)
		goto retry;
	if (rv < 0)
		return rv;

	if (rv != count) {
		count -= rv;
		off += rv;
		goto retry;
	}
	return 0;
}

static int do_read(int fd, void *buf, size_t count)
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

static void print_usage(void)
{
	printf("Usage:\n");
	printf("\n");
	printf("%s [options] [compat|ls|dump]\n", prog_name);
	printf("\n");
	printf("Options:\n");
	printf("  -h               Print this help, then exit\n");
	printf("  -V               Print program version information, then exit\n");
	printf("\n");

	printf("compat             Show compatibility mode that groupd is running\n");
	printf("\n");

	printf("ls                 Show group state for fence, dlm, gfs\n");
	printf("   -g <num>        select daemons to query\n");
	printf("                   0: query fenced, dlm_controld, gfs_controld\n");
	printf("                   1: query groupd (for old compat mode)\n");
	printf("                   2: use 0 if daemons running in new mode,\n");
	printf("                   or 1 if groupd running in old mode.\n");
	printf("                   Default %d\n", DEFAULT_GROUPD_COMPAT);
	printf("   -n              Show all node information (with -g0)\n");
	printf("   -v              Show extra event information (with -g1)\n");
	printf("\n");

	printf("dump               Show debug log from groupd\n");
	printf("dump fence         Show debug log from fenced (fence_tool dump)\n");
	printf("dump dlm           Show debug log from dlm_controld (dlm_tool dump)\n");
	printf("dump gfs           Show debug log from gfs_controld (gfs_control dump)\n");
	printf("dump plocks <name> Show posix locks from dlm_controld for lockspace <name>\n");
	printf("                   (dlm_tool plocks <name>)\n");
	printf("\n");
}

#define OPTION_STRING "g:hVvn"

static void decode_arguments(int argc, char **argv)
{
	int cont = 1;
	int optchar;

	while (cont) {
		optchar = getopt(argc, argv, OPTION_STRING);

		switch (optchar) {
		
		case 'g':
			opt_groupd_compat = 1;
			cfg_groupd_compat = atoi(optarg);
			break;

		case 'n':
			ls_all_nodes = 1;
			break;

		case 'v':
			verbose = 1;
			break;

		case 'h':
			print_usage();
			exit(EXIT_SUCCESS);
			break;

		case 'V':
			printf("%s %s (built %s %s)\n",
				prog_name, RELEASE_VERSION, __DATE__, __TIME__);
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

	while (optind < argc) {
		if (strcmp(argv[optind], "dump") == 0) {
			operation = OP_DUMP;
			opt_ind = optind + 1;
			break;
		} else if (strcmp(argv[optind], "ls") == 0 ||
		           strcmp(argv[optind], "list") == 0) {
			operation = OP_LIST;
			opt_ind = optind + 1;
			break;
		} else if (strcmp(argv[optind], "compat") == 0) {
			operation = OP_COMPAT;
			opt_ind = optind + 1;
			break;
		}
		optind++;
	}

	if (!operation)
		operation = OP_LIST;
}

/* copied from grouip/daemon/gd_internal.h, must keep in sync */

#define EST_JOIN_BEGIN         1
#define EST_JOIN_STOP_WAIT     2
#define EST_JOIN_ALL_STOPPED   3
#define EST_JOIN_START_WAIT    4
#define EST_JOIN_ALL_STARTED   5
#define EST_LEAVE_BEGIN        6
#define EST_LEAVE_STOP_WAIT    7
#define EST_LEAVE_ALL_STOPPED  8
#define EST_LEAVE_START_WAIT   9
#define EST_LEAVE_ALL_STARTED 10
#define EST_FAIL_BEGIN        11
#define EST_FAIL_STOP_WAIT    12
#define EST_FAIL_ALL_STOPPED  13
#define EST_FAIL_START_WAIT   14
#define EST_FAIL_ALL_STARTED  15

static const char *ev_state_str(int state)
{
	switch (state) {
	case EST_JOIN_BEGIN:
		return "JOIN_BEGIN";
	case EST_JOIN_STOP_WAIT:
		return "JOIN_STOP_WAIT";
	case EST_JOIN_ALL_STOPPED:
		return "JOIN_ALL_STOPPED";
	case EST_JOIN_START_WAIT:
		return "JOIN_START_WAIT";
	case EST_JOIN_ALL_STARTED:
		return "JOIN_ALL_STARTED";
	case EST_LEAVE_BEGIN:
		return "LEAVE_BEGIN";
	case EST_LEAVE_STOP_WAIT:
		return "LEAVE_STOP_WAIT";
	case EST_LEAVE_ALL_STOPPED:
		return "LEAVE_ALL_STOPPED";
	case EST_LEAVE_START_WAIT:
		return "LEAVE_START_WAIT";
	case EST_LEAVE_ALL_STARTED:
		return "LEAVE_ALL_STARTED";
	case EST_FAIL_BEGIN:
		return "FAIL_BEGIN";
	case EST_FAIL_STOP_WAIT:
		return "FAIL_STOP_WAIT";
	case EST_FAIL_ALL_STOPPED:
		return "FAIL_ALL_STOPPED";
	case EST_FAIL_START_WAIT:
		return "FAIL_START_WAIT";
	case EST_FAIL_ALL_STARTED:
		return "FAIL_ALL_STARTED";
	default:
		return "unknown";
	}
}

static const char *state_str(group_data_t *data)
{
	static char buf[128];

	memset(buf, 0, sizeof(buf));

	if (!data->event_state && !data->event_nodeid)
		sprintf(buf, "none");
	else if (verbose)
		snprintf(buf, 127, "%s %d %llx %d",
			 ev_state_str(data->event_state),
			 data->event_nodeid,
			 (unsigned long long)data->event_id,
			 data->event_local_status);
	else
		snprintf(buf, 127, "%s", ev_state_str(data->event_state));

	return buf;
}

static int data_compare(const void *va, const void *vb)
{
	const group_data_t *a = va;
	const group_data_t *b = vb;
	return a->level - b->level;
}

static int member_compare(const void *va, const void *vb)
{
	const int *a = va;
	const int *b = vb;
	return *a - *b;
}

static int groupd_list(int argc, char **argv)
{
	group_data_t data[MAX_GROUPS];
	int i, j, rv, count = 0, level, ret = 0;
	char *name;
	const char *state_header;
	int type_width = 16;
	int level_width = 5;
	int name_width = 32;
	int id_width = 8;
	int state_width = 12;
	int len, max_name = 4;

	memset(&data, 0, sizeof(data));

	if (opt_ind && opt_ind < argc) {
		level = atoi(argv[opt_ind++]);
		name = argv[opt_ind];

		rv = group_get_group(level, name, data);
		count = 1;

		/* don't output if there's no group at all */
		if (data[0].id == 0 && !strlen(data[0].name) && 
		    !strlen(data[0].client_name)) {
			fprintf(stderr, "groupd has no information about "
			        "the specified group\n");
			return 1;
		}
		/* If we wanted a specific group but we are not
		   joined, print it out - but return failure to
		   the caller */
		if (data[0].member != 1)
			ret = 1;
	} else
		rv = group_get_groups(MAX_GROUPS, &count, data);

	if (rv < 0)
		return rv;

	if (!count)
		return 0;

	for (i = 0; i < count; i++) {
		len = strlen(data[i].name);
		if (len > max_name)
			max_name = len;
	}
	name_width = max_name + 1;

	if (verbose)
		state_header = "state node id local_done";
	else
		state_header = "state";
			
	qsort(&data, count, sizeof(group_data_t), data_compare);

	for (i = 0; i < count; i++) {
		if (!i)
			printf("%-*s %-*s %-*s %-*s %-*s\n",
			       type_width, "type",
			       level_width, "level",
			       name_width, "name",
			       id_width, "id",
			       state_width, state_header);

		printf("%-*s %-*d %-*s %0*x %-*s\n",
			type_width, data[i].client_name,
			level_width, data[i].level,
			name_width, data[i].name,
			id_width, data[i].id,
			state_width, state_str(&data[i]));

		qsort(&data[i].members, data[i].member_count,
		      sizeof(int), member_compare);

		printf("[");
		for (j = 0; j < data[i].member_count; j++) {
			if (j != 0)
				printf(" ");
			printf("%d", data[i].members[j]);
		}
		printf("]\n");
	}
	return ret;
}

#if 0
static int print_header_done;

static int fenced_node_compare(const void *va, const void *vb)
{
	const struct fenced_node *a = va;
	const struct fenced_node *b = vb;

	return a->nodeid - b->nodeid;
}

static void print_header(void)
{
	if (print_header_done)
		return;
	print_header_done = 1;

	printf("type         level name             id       state\n");
}

static void fenced_list(void)
{
	struct fenced_domain d;
	struct fenced_node nodes[MAX_NODES];
	struct fenced_node *np;
	int node_count;
	int rv, j;

	rv = fenced_domain_info(&d);
	if (rv < 0)
		return;

	print_header();

	printf("fence        0     %-*s %08x %d\n",
	       16, "default", 0, d.state);

	node_count = 0;
	memset(&nodes, 0, sizeof(nodes));

	rv = fenced_domain_nodes(FENCED_NODES_MEMBERS, MAX_NODES,
				 &node_count, nodes);
	if (rv < 0 || !node_count)
		goto do_nodeids;

	qsort(&nodes, node_count, sizeof(struct fenced_node),
	      fenced_node_compare);

 do_nodeids:
	printf("[");
	np = nodes;
	for (j = 0; j < node_count; j++) {
		if (j)
			printf(" ");
		printf("%d", np->nodeid);
		np++;
	}
	printf("]\n");
}

static int dlmc_node_compare(const void *va, const void *vb)
{
	const struct dlmc_node *a = va;
	const struct dlmc_node *b = vb;

	return a->nodeid - b->nodeid;
}

static void dlm_controld_list(void)
{
	struct dlmc_lockspace lss[MAX_LS];
	struct dlmc_node nodes[MAX_NODES];
	struct dlmc_node *np;
	struct dlmc_lockspace *ls;
	char *name = NULL;
	int node_count;
	int ls_count;
	int rv;
	int i, j;

	memset(lss, 0, sizeof(lss));

	if (name) {
		rv = dlmc_lockspace_info(name, lss);
		if (rv < 0)
			return;
		ls_count = 1;
	} else {
		rv = dlmc_lockspaces(MAX_LS, &ls_count, lss);
		if (rv < 0)
			return;
	}

	for (i = 0; i < ls_count; i++) {
		ls = &lss[i];

		if (!i)
			print_header();

		printf("dlm          1     %-*s %08x %x\n",
			16, ls->name, ls->global_id, ls->flags);

		node_count = 0;
		memset(&nodes, 0, sizeof(nodes));

		rv = dlmc_lockspace_nodes(ls->name, DLMC_NODES_MEMBERS,
					  MAX_NODES, &node_count, nodes);
		if (rv < 0 || !node_count)
			goto do_nodeids;

		qsort(nodes, node_count, sizeof(struct dlmc_node),
		      dlmc_node_compare);

 do_nodeids:
		printf("[");
		np = nodes;
		for (j = 0; j < node_count; j++) {
			if (j)
				printf(" ");
			printf("%d", np->nodeid);
			np++;
		}
		printf("]\n");
	}
}

static int gfsc_node_compare(const void *va, const void *vb)
{
	const struct gfsc_node *a = va;
	const struct gfsc_node *b = vb;

	return a->nodeid - b->nodeid;
}

static void gfs_controld_list(void)
{
	struct gfsc_mountgroup mgs[MAX_MG];
	struct gfsc_node nodes[MAX_NODES];
	struct gfsc_node *np;
	struct gfsc_mountgroup *mg;
	char *name = NULL;
	int node_count;
	int mg_count;
	int rv;
	int i, j;

	memset(mgs, 0, sizeof(mgs));

	if (name) {
		rv = gfsc_mountgroup_info(name, mgs);
		if (rv < 0)
			return;
		mg_count = 1;
	} else {
		rv = gfsc_mountgroups(MAX_MG, &mg_count, mgs);
		if (rv < 0)
			return;
	}

	for (i = 0; i < mg_count; i++) {
		mg = &mgs[i];

		if (!i)
			print_header();

		printf("gfs          2     %-*s %08x %x\n",
			16, mg->name, mg->global_id, mg->flags);

		node_count = 0;
		memset(&nodes, 0, sizeof(nodes));

		rv = gfsc_mountgroup_nodes(mg->name, GFSC_NODES_MEMBERS,
					   MAX_NODES, &node_count, nodes);
		if (rv < 0 || !node_count)
			goto do_nodeids;

		qsort(nodes, node_count, sizeof(struct gfsc_node),
		      gfsc_node_compare);

 do_nodeids:
		printf("[");
		np = nodes;
		for (j = 0; j < node_count; j++) {
			if (j)
				printf(" ");
			printf("%d", np->nodeid);
			np++;
		}
		printf("]\n");
	}
}
#endif

static int connect_daemon(const char *path)
{
	struct sockaddr_un sun;
	socklen_t addrlen;
	int rv, fd;

	fd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		goto out;

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	strcpy(&sun.sun_path[1], path);
	addrlen = sizeof(sa_family_t) + strlen(sun.sun_path+1) + 1;

	rv = connect(fd, (struct sockaddr *) &sun, addrlen);
	if (rv < 0) {
		close(fd);
		fd = rv;
	}
 out:
	return fd;
}

static void groupd_dump_debug(int argc, char **argv, char *inbuf)
{
	char outbuf[GROUPD_MSGLEN];
	int rv, fd;

	fd = connect_daemon(GROUPD_SOCK_PATH);
	if (fd < 0)
		return;

	memset(outbuf, 0, sizeof(outbuf));
	sprintf(outbuf, "dump");

	rv = do_write(fd, outbuf, sizeof(outbuf));
	if (rv < 0) {
		printf("dump write error %d errno %d\n", rv, errno);
		return;
	}

	do_read(fd, inbuf, GROUPD_DUMP_SIZE);

	close(fd);
}

int main(int argc, char **argv)
{
	int rv, version = 0; 

	prog_name = argv[0];
	decode_arguments(argc, argv);

	switch (operation) {
	case OP_COMPAT:
		rv = group_get_version(&version);
		if (rv < 0)
			version = -1;

		switch (version) {
		case -1:
			break;
		case -EAGAIN:
			printf("groupd compatibility mode 2 (pending)\n");
			break;
		case GROUP_LIBGROUP:
			printf("groupd compatibility mode 1\n");
			break;
		case GROUP_LIBCPG:
			printf("groupd compatibility mode 0\n");
			break;
		default:
			printf("groupd compatibility mode %d\n", version);
			break;
		}

		if (rv < 0)
			exit(EXIT_FAILURE);
		break;

	case OP_LIST:

		rv = group_get_version(&version);
		if (rv < 0)
			version = -1;

		switch (version) {
		case -1:
			version = GROUP_LIBCPG;
			break;
		case -EAGAIN:
			printf("groupd compatibility mode 2 (pending)\n");
			break;
		case GROUP_LIBGROUP:
			printf("groupd compatibility mode 1\n");
			break;
		case GROUP_LIBCPG:
			printf("groupd compatibility mode 0\n");
			break;
		default:
			printf("groupd compatibility mode %d\n", version);
			break;
		}

		if ((cfg_groupd_compat == 0) ||
		    (cfg_groupd_compat == 2 && version == GROUP_LIBCPG)) {
			/* show the new cluster3 data (from daemons) in
			   the new daemon-specific format */

			if (verbose || ls_all_nodes) {
				system("fence_tool ls -n");
				system("dlm_tool ls -n");
				system("gfs_control ls -n");
			} else {
				system("fence_tool ls");
				system("dlm_tool ls");
				system("gfs_control ls");
			}

			if (version == GROUP_LIBGROUP)
				printf("Run 'group_tool ls -g1' for groupd information.\n");
			break;
		}

		if ((cfg_groupd_compat == 1) ||
		    (cfg_groupd_compat == 2 && version == GROUP_LIBGROUP)) {
			/* show the same old cluster2 data (from groupd)
			   in the same old format as cluster2 */

			groupd_list(argc, argv);

			if (version == GROUP_LIBCPG)
				printf("Run 'group_tool ls -g0' for daemon information.\n");
			break;
		}
				
#if 0
		/* do we want to add an option that will use these functions
		   to "fake" new cluster3 data in the old cluster2 format? */
		fenced_list();
		dlm_controld_list();
		gfs_controld_list();
#endif

	case OP_DUMP:
		if (opt_ind && opt_ind < argc) {
			if (!strncmp(argv[opt_ind], "gfs", 3)) {
				char gbuf[GFSC_DUMP_SIZE];

				memset(gbuf, 0, sizeof(gbuf));

				printf("dump gfs\n");
				gfsc_dump_debug(gbuf);

				do_write(STDOUT_FILENO, gbuf, strlen(gbuf));
			}

			if (!strncmp(argv[opt_ind], "dlm", 3)) {
				char dbuf[DLMC_DUMP_SIZE];

				memset(dbuf, 0, sizeof(dbuf));

				printf("dump dlm\n");
				dlmc_dump_debug(dbuf);

				do_write(STDOUT_FILENO, dbuf, strlen(dbuf));
			}

			if (!strncmp(argv[opt_ind], "fence", 5)) {
				char fbuf[FENCED_DUMP_SIZE];

				memset(fbuf, 0, sizeof(fbuf));

				fenced_dump_debug(fbuf);

				do_write(STDOUT_FILENO, fbuf, strlen(fbuf));
			}

			if (!strncmp(argv[opt_ind], "plocks", 6)) {
				char pbuf[DLMC_DUMP_SIZE];

				if (opt_ind + 1 >= argc) {
					printf("plock dump requires name\n");
					return -1;
				}

				memset(pbuf, 0, sizeof(pbuf));

				dlmc_dump_plocks(argv[opt_ind + 1], pbuf);

				do_write(STDOUT_FILENO, pbuf, strlen(pbuf));
			}
		} else {
			char rbuf[GROUPD_DUMP_SIZE];

			memset(rbuf, 0, sizeof(rbuf));

			groupd_dump_debug(argc, argv, rbuf);

			do_write(STDOUT_FILENO, rbuf, strlen(rbuf));
		}

		break;
	}

	return 0;
}

