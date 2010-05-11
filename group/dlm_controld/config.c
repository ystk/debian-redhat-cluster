#include <sys/types.h>
#include <asm/types.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <fcntl.h>
#include <netdb.h>
#include <limits.h>
#include <unistd.h>
#include <dirent.h>

#include "dlm_daemon.h"
#include "config.h"
#include "ccs.h"

int ccs_handle;

/* when not set in cluster.conf, a node's default weight is 1 */

#define MASTER_PATH "/cluster/dlm/lockspace[@name=\"%s\"]/master"
#define WEIGHT_PATH "/cluster/clusternodes/clusternode[@name=\"%s\"]/@weight"
#define MASTER_NAME   MASTER_PATH "/@name"
#define MASTER_WEIGHT MASTER_PATH "[@name=\"%s\"]/@weight"

/* look for node's weight in the dlm/lockspace section */

static int get_weight_lockspace(char *node, char *lockspace)
{
	char path[PATH_MAX], *str;
	int error, weight;
	int master_count = 0, node_is_master = 0;

	memset(path, 0, PATH_MAX);
	sprintf(path, MASTER_NAME, lockspace);

	while (1) {
		error = ccs_get_list(ccs_handle, path, &str);
		if (error || !str)
			break;
		master_count++;
		if (strcmp(str, node) == 0)
			node_is_master = 1;
		free(str);
	}

	/* if there are no masters, next check for a clusternode weight */

	if (!master_count)
		return -1;

	/* if there's a master and this node isn't it, it gets weight 0 */

	if (!node_is_master)
		return 0;

	/* master gets its specified weight or 1 if none is given */

	memset(path, 0, PATH_MAX);
	sprintf(path, MASTER_WEIGHT, lockspace, node);

	error = ccs_get(ccs_handle, path, &str);
	if (error || !str)
		return 1;

	weight = atoi(str);
	free(str);
	return weight;
}

/* look for node's weight on its clusternode line */

static int get_weight_clusternode(char *node, char *lockspace)
{
	char path[PATH_MAX], *str;
	int error, weight;

	memset(path, 0, PATH_MAX);
	sprintf(path, WEIGHT_PATH, node);

	error = ccs_get(ccs_handle, path, &str);
	if (error || !str)
		return -1;

	weight = atoi(str);
	free(str);
	return weight;
}

int get_weight(int nodeid, char *lockspace)
{
	char *node;
	int w;

	node = nodeid2name(nodeid);
	if (!node) {
		log_error("no name for nodeid %d", nodeid);
		w = 1;
		goto out;
	}

	w = get_weight_lockspace(node, lockspace);
	if (w >= 0)
		goto out;

	w = get_weight_clusternode(node, lockspace);
	if (w >= 0)
		goto out;

	/* default weight is 1 */
	w = 1;
 out:
	return w;
}

void read_ccs_name(const char *path, char *name)
{
	char *str;
	int error;

	error = ccs_get(ccs_handle, path, &str);
	if (error || !str)
		return;

	strcpy(name, str);

	free(str);
}

void read_ccs_yesno(const char *path, int *yes, int *no)
{
	char *str;
	int error;

	*yes = 0;
	*no = 0;

	error = ccs_get(ccs_handle, path, &str);
	if (error || !str)
		return;

	if (!strcmp(str, "yes"))
		*yes = 1;

	else if (!strcmp(str, "no"))
		*no = 1;

	free(str);
}

int read_ccs_int(const char *path, int *config_val)
{
	char *str;
	int val;
	int error;

	error = ccs_get(ccs_handle, path, &str);
	if (error || !str)
		return -1;

	val = atoi(str);

	if (val < 0) {
		log_error("ignore invalid value %d for %s", val, path);
		return -1;
	}

	*config_val = val;
	log_debug("%s is %u", path, val);
	free(str);
	return 0;
}

static void read_ccs_protocol(const char *path, int *config_val)
{
	char *str;
	int val;
	int error;

	error = ccs_get(ccs_handle, path, &str);
	if (error || !str)
		return;

	if (!strncasecmp(str, "tcp", 3))
		val = PROTO_TCP;
	else if (!strncasecmp(str, "sctp", 4))
		val = PROTO_SCTP;
	else if (!strncasecmp(str, "detect", 6))
		val = PROTO_DETECT;
	else {
		log_error("ignore invalid value %s for %s", str, path);
		return;
	}

	*config_val = val;
	log_debug("%s is %u (%s)", path, val, str);
	free(str);
}

#define DEBUG_PATH "/cluster/dlm/@log_debug"
#define TIMEWARN_PATH "/cluster/dlm/@timewarn"
#define PROTOCOL_PATH "/cluster/dlm/@protocol"
#define GROUPD_COMPAT_PATH "/cluster/group/@groupd_compat"
#define ENABLE_FENCING_PATH "/cluster/dlm/@enable_fencing"
#define ENABLE_QUORUM_PATH "/cluster/dlm/@enable_quorum"
#define ENABLE_DEADLK_PATH "/cluster/dlm/@enable_deadlk"
#define ENABLE_PLOCK_PATH "/cluster/dlm/@enable_plock"
#define PLOCK_DEBUG_PATH "/cluster/dlm/@plock_debug"
#define PLOCK_RATE_LIMIT_PATH "/cluster/dlm/@plock_rate_limit"
#define PLOCK_OWNERSHIP_PATH "/cluster/dlm/@plock_ownership"
#define DROP_RESOURCES_TIME_PATH "/cluster/dlm/@drop_resources_time"
#define DROP_RESOURCES_COUNT_PATH "/cluster/dlm/@drop_resources_count"
#define DROP_RESOURCES_AGE_PATH "/cluster/dlm/@drop_resources_age"

/* for backward-compat */
#define GFS_PLOCK_RATE_LIMIT_PATH "/cluster/gfs_controld/@plock_rate_limit"
#define GFS_PLOCK_OWNERSHIP_PATH "/cluster/gfs_controld/@plock_ownership"
#define GFS_DROP_RESOURCES_TIME_PATH "/cluster/gfs_controld/@drop_resources_time"
#define GFS_DROP_RESOURCES_COUNT_PATH "/cluster/gfs_controld/@drop_resources_count"
#define GFS_DROP_RESOURCES_AGE_PATH "/cluster/gfs_controld/@drop_resources_age"

int setup_ccs(void)
{
	int cd, rv;

	/* skip things that cannot be changed while running */
	if (ccs_handle)
		goto update;

	cd = ccs_connect();
	if (cd < 0) {
		log_error("ccs_connect error %d %d", cd, errno);
		return -1;
	}
	ccs_handle = cd;

	/* These config values are set from cluster.conf only if they haven't
	   already been set on the command line. */

	if (!optk_debug)
		read_ccs_int(DEBUG_PATH, &cfgk_debug);
	if (!optk_timewarn)
		read_ccs_int(TIMEWARN_PATH, &cfgk_timewarn);
	if (!optk_protocol)
		read_ccs_protocol(PROTOCOL_PATH, &cfgk_protocol);
	if (!optd_groupd_compat)
		read_ccs_int(GROUPD_COMPAT_PATH, &cfgd_groupd_compat);
	if (!optd_enable_fencing)
		read_ccs_int(ENABLE_FENCING_PATH, &cfgd_enable_fencing);
	if (!optd_enable_quorum)
		read_ccs_int(ENABLE_QUORUM_PATH, &cfgd_enable_quorum);
	if (!optd_enable_deadlk)
		read_ccs_int(ENABLE_DEADLK_PATH, &cfgd_enable_deadlk);
	if (!optd_enable_plock)
		read_ccs_int(ENABLE_PLOCK_PATH, &cfgd_enable_plock);
	if (!optd_plock_ownership) {
		rv = read_ccs_int(PLOCK_OWNERSHIP_PATH, &cfgd_plock_ownership);
		if (rv < 0)
			read_ccs_int(GFS_PLOCK_OWNERSHIP_PATH, &cfgd_plock_ownership);
	}

	/* The following can be changed while running */
 update:
	if (!optd_plock_debug) {
		read_ccs_int(PLOCK_DEBUG_PATH, &cfgd_plock_debug);
	}
	if (!optd_plock_rate_limit) {
		rv = read_ccs_int(PLOCK_RATE_LIMIT_PATH, &cfgd_plock_rate_limit);
		if (rv < 0)
			read_ccs_int(GFS_PLOCK_RATE_LIMIT_PATH, &cfgd_plock_rate_limit);
	}
	if (!optd_drop_resources_time) {
		rv = read_ccs_int(DROP_RESOURCES_TIME_PATH, &cfgd_drop_resources_time);
		if (rv < 0)
			read_ccs_int(GFS_DROP_RESOURCES_TIME_PATH, &cfgd_drop_resources_time);
	}
	if (!optd_drop_resources_count) {
		rv = read_ccs_int(DROP_RESOURCES_COUNT_PATH, &cfgd_drop_resources_count);
		if (rv < 0)
			read_ccs_int(GFS_DROP_RESOURCES_COUNT_PATH, &cfgd_drop_resources_count);
	}
	if (!optd_drop_resources_age) {
		rv = read_ccs_int(DROP_RESOURCES_AGE_PATH, &cfgd_drop_resources_age);
		if (rv < 0)
			read_ccs_int(GFS_DROP_RESOURCES_AGE_PATH, &cfgd_drop_resources_age);
	}

	return 0;
}

void close_ccs(void)
{
	ccs_disconnect(ccs_handle);
}

