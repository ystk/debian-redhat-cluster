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

#include "gfs_daemon.h"
#include "config.h"
#include "ccs.h"

int ccs_handle;

/* was a config value set on command line?, 0 or 1. */

int optd_groupd_compat;
int optd_debug_logfile;
int optd_enable_withdraw;
int optd_enable_plock;
int optd_plock_debug;
int optd_plock_rate_limit;
int optd_plock_ownership;
int optd_drop_resources_time;
int optd_drop_resources_count;
int optd_drop_resources_age;

/* actual config value from command line, cluster.conf, or default. */

int cfgd_groupd_compat		= DEFAULT_GROUPD_COMPAT;
int cfgd_debug_logfile		= DEFAULT_DEBUG_LOGFILE;
int cfgd_enable_withdraw	= DEFAULT_ENABLE_WITHDRAW;
int cfgd_enable_plock		= DEFAULT_ENABLE_PLOCK;
int cfgd_plock_debug		= DEFAULT_PLOCK_DEBUG;
int cfgd_plock_rate_limit	= DEFAULT_PLOCK_RATE_LIMIT;
int cfgd_plock_ownership	= DEFAULT_PLOCK_OWNERSHIP;
int cfgd_drop_resources_time	= DEFAULT_DROP_RESOURCES_TIME;
int cfgd_drop_resources_count	= DEFAULT_DROP_RESOURCES_COUNT;
int cfgd_drop_resources_age	= DEFAULT_DROP_RESOURCES_AGE;

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

#define LOCKSPACE_NODIR "/cluster/dlm/lockspace[@name=\"%s\"]/@nodir"

void read_ccs_nodir(struct mountgroup *mg, char *buf)
{
	char path[PATH_MAX];
	char *str;
	int val;
	int error;

	memset(path, 0, PATH_MAX);
	sprintf(path, LOCKSPACE_NODIR, mg->name);

	error = ccs_get(ccs_handle, path, &str);
	if (error || !str)
		return;

	val = atoi(str);

	if (val < 0) {
		log_error("ignore invalid value %d for %s", val, path);
		return;
	}

	snprintf(buf, 32, ":nodir=%d", val);

	log_debug("%s is %u", path, val);
	free(str);
}

#define GROUPD_COMPAT_PATH "/cluster/group/@groupd_compat"
#define ENABLE_WITHDRAW_PATH "/cluster/gfs_controld/@enable_withdraw"
#define ENABLE_PLOCK_PATH "/cluster/gfs_controld/@enable_plock"
#define PLOCK_DEBUG_PATH "/cluster/gfs_controld/@plock_debug"
#define PLOCK_RATE_LIMIT_PATH "/cluster/gfs_controld/@plock_rate_limit"
#define PLOCK_OWNERSHIP_PATH "/cluster/gfs_controld/@plock_ownership"
#define DROP_RESOURCES_TIME_PATH "/cluster/gfs_controld/@drop_resources_time"
#define DROP_RESOURCES_COUNT_PATH "/cluster/gfs_controld/@drop_resources_count"
#define DROP_RESOURCES_AGE_PATH "/cluster/gfs_controld/@drop_resources_age"

#define DLM_PLOCK_RATE_LIMIT_PATH "/cluster/dlm/@plock_rate_limit"
#define DLM_PLOCK_OWNERSHIP_PATH "/cluster/dlm/@plock_ownership"
#define DLM_DROP_RESOURCES_TIME_PATH "/cluster/dlm/@drop_resources_time"
#define DLM_DROP_RESOURCES_COUNT_PATH "/cluster/dlm/@drop_resources_count"
#define DLM_DROP_RESOURCES_AGE_PATH "/cluster/dlm/@drop_resources_age"

int setup_ccs(void)
{
	int cd, rv;

	if (ccs_handle)
		goto update;

	cd = ccs_connect();
	if (cd < 0) {
		log_error("ccs_connect error %d %d", cd, errno);
		return -1;
	}
	ccs_handle = cd;

	/* plock_ownership is a special case */
	if (optd_plock_ownership)
		using_default_plock_ownership = 0;
	else
		using_default_plock_ownership = 1;

	/* These config values are set from cluster.conf only if they haven't
	   already been set on the command line. */

	if (!optd_groupd_compat)
		read_ccs_int(GROUPD_COMPAT_PATH, &cfgd_groupd_compat);
	if (!optd_enable_withdraw)
		read_ccs_int(ENABLE_WITHDRAW_PATH, &cfgd_enable_withdraw);
	if (!optd_enable_plock)
		read_ccs_int(ENABLE_PLOCK_PATH, &cfgd_enable_plock);
	if (!optd_plock_ownership) {
		rv = read_ccs_int(PLOCK_OWNERSHIP_PATH, &cfgd_plock_ownership);
		if (rv < 0)
			rv = read_ccs_int(DLM_PLOCK_OWNERSHIP_PATH, &cfgd_plock_ownership);
		if (!rv)
			using_default_plock_ownership = 0;
	}

	/* The following can be changed while running */
 update:
	if (!optd_plock_debug) {
		read_ccs_int(PLOCK_DEBUG_PATH, &cfgd_plock_debug);
	}
	if (!optd_plock_rate_limit) {
		rv = read_ccs_int(PLOCK_RATE_LIMIT_PATH, &cfgd_plock_rate_limit);
		if (rv < 0)
			read_ccs_int(DLM_PLOCK_RATE_LIMIT_PATH, &cfgd_plock_rate_limit);
	}
	if (!optd_drop_resources_time) {
		rv = read_ccs_int(DROP_RESOURCES_TIME_PATH, &cfgd_drop_resources_time);
		if (rv < 0)
			read_ccs_int(DLM_DROP_RESOURCES_TIME_PATH, &cfgd_drop_resources_time);
	}
	if (!optd_drop_resources_count) {
		rv = read_ccs_int(DROP_RESOURCES_COUNT_PATH, &cfgd_drop_resources_count);
		if (rv < 0)
			read_ccs_int(DLM_DROP_RESOURCES_COUNT_PATH, &cfgd_drop_resources_count);
	}
	if (!optd_drop_resources_age) {
		rv = read_ccs_int(DROP_RESOURCES_AGE_PATH, &cfgd_drop_resources_age);
		if (rv < 0)
			read_ccs_int(DLM_DROP_RESOURCES_AGE_PATH, &cfgd_drop_resources_age);
	}

	return 0;
}

void close_ccs(void)
{
	ccs_disconnect(ccs_handle);
}

