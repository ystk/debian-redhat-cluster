#include "fd.h"
#include "config.h"
#include "ccs.h"

int ccs_handle;

/* was a config value set on command line?, 0 or 1. */

int optd_groupd_compat;
int optd_debug_logfile;
int optd_clean_start;
int optd_disable_dbus;
int optd_skip_undefined;
int optd_post_join_delay;
int optd_post_fail_delay;
int optd_override_time;
int optd_override_path;

/* actual config value from command line, cluster.conf, or default. */

int cfgd_groupd_compat   = DEFAULT_GROUPD_COMPAT;
int cfgd_debug_logfile   = DEFAULT_DEBUG_LOGFILE;
int cfgd_clean_start     = DEFAULT_CLEAN_START;
int cfgd_disable_dbus    = DEFAULT_DISABLE_DBUS;
int cfgd_skip_undefined  = DEFAULT_SKIP_UNDEFINED;
int cfgd_post_join_delay = DEFAULT_POST_JOIN_DELAY;
int cfgd_post_fail_delay = DEFAULT_POST_FAIL_DELAY;
int cfgd_override_time   = DEFAULT_OVERRIDE_TIME;
const char *cfgd_override_path = DEFAULT_OVERRIDE_PATH;

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

void read_ccs_int(const char *path, int *config_val)
{
	char *str;
	int val;
	int error;

	error = ccs_get(ccs_handle, path, &str);
	if (error || !str)
		return;

	val = atoi(str);

	if (val < 0) {
		log_error("ignore invalid value %d for %s", val, path);
		return;
	}

	*config_val = val;
	log_debug("%s is %u", path, val);
	free(str);
}

#define GROUPD_COMPAT_PATH "/cluster/group/@groupd_compat"
#define CLEAN_START_PATH "/cluster/fence_daemon/@clean_start"
#define POST_JOIN_DELAY_PATH "/cluster/fence_daemon/@post_join_delay"
#define POST_FAIL_DELAY_PATH "/cluster/fence_daemon/@post_fail_delay"
#define OVERRIDE_PATH_PATH "/cluster/fence_daemon/@override_path"
#define OVERRIDE_TIME_PATH "/cluster/fence_daemon/@override_time"
#define METHOD_NAME_PATH "/cluster/clusternodes/clusternode[@name=\"%s\"]/fence/method[%d]/@name"

static int count_methods(char *victim)
{
	char path[PATH_MAX], *name;
	int error, i;

	for (i = 0; i < 2; i++) {
		memset(path, 0, sizeof(path));
		sprintf(path, METHOD_NAME_PATH, victim, i+1);

		error = ccs_get(ccs_handle, path, &name);
		if (error)
			break;
		free(name);
	}
	return i;
}

/* These are the options that can be changed while running. */

void reread_ccs(void)
{
	if (!optd_post_join_delay)
		read_ccs_int(POST_JOIN_DELAY_PATH, &cfgd_post_join_delay);
	if (!optd_post_fail_delay)
		read_ccs_int(POST_FAIL_DELAY_PATH, &cfgd_post_fail_delay);
	if (!optd_override_time)
		read_ccs_int(OVERRIDE_TIME_PATH, &cfgd_override_time);
}

/* called when the domain is joined, not when the daemon starts */

int read_ccs(struct fd *fd)
{
	char path[PATH_MAX];
	char *str, *name;
	int error, i = 0, count = 0;
	int num_methods;

	if (!optd_clean_start)
		read_ccs_int(CLEAN_START_PATH, &cfgd_clean_start);

	reread_ccs();

	if (!optd_override_path) {
		str = NULL;
		memset(path, 0, sizeof(path));
		sprintf(path, OVERRIDE_PATH_PATH);

		error = ccs_get(ccs_handle, path, &str);
		if (!error && str)
			cfgd_override_path = strdup(str);
		if (str)
			free(str);
	}

	if (cfgd_clean_start) {
		log_debug("clean start, skipping initial nodes");
		goto out;
	}

	for (i = 1; ; i++) {
		str = NULL;
		memset(path, 0, sizeof(path));
		sprintf(path, "/cluster/clusternodes/clusternode[%d]/@nodeid", i);

		error = ccs_get(ccs_handle, path, &str);
		if (error || !str)
			break;

		name = NULL;
		memset(path, 0, sizeof(path));
		sprintf(path, "/cluster/clusternodes/clusternode[%d]/@name", i);

		error = ccs_get(ccs_handle, path, &name);
		if (error || !name) {
			log_error("node name query failed for num %d nodeid %s",
				  i, str);
			break;
		}

		num_methods = count_methods(name);

		/* the libcpg code only uses the fd->complete list for
		   determining initial victims; the libgroup code uses
		   fd->complete more extensively */

		if (cfgd_skip_undefined && !num_methods)
			log_debug("skip %s with zero methods", name);
		else
			add_complete_node(fd, atoi(str));

		free(str);
		free(name);
		count++;
	}

	log_debug("added %d nodes from ccs", count);
 out:
	return 0;
}

int setup_ccs(void)
{
	int cd;

	cd = ccs_connect();
	if (cd < 0) {
		log_error("ccs_connect error %d %d", cd, errno);
		return -1;
	}
	ccs_handle = cd;

	if (!optd_groupd_compat)
		read_ccs_int(GROUPD_COMPAT_PATH, &cfgd_groupd_compat);

	return 0;
}

void close_ccs(void)
{
	ccs_disconnect(ccs_handle);
}

