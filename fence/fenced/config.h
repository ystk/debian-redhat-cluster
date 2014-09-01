#ifndef __CONFIG_DOT_H__
#define __CONFIG_DOT_H__

#define DEFAULT_GROUPD_COMPAT 0
#define DEFAULT_DEBUG_LOGFILE 0
#define DEFAULT_CLEAN_START 0
#define DEFAULT_DISABLE_DBUS 0
#define DEFAULT_SKIP_UNDEFINED 0
#define DEFAULT_POST_JOIN_DELAY 6
#define DEFAULT_POST_FAIL_DELAY 0
#define DEFAULT_OVERRIDE_TIME 3
#define DEFAULT_OVERRIDE_PATH "/var/run/cluster/fenced_override"

extern int optd_groupd_compat;
extern int optd_debug_logfile;
extern int optd_clean_start;
extern int optd_disable_dbus;
extern int optd_skip_undefined;
extern int optd_post_join_delay;
extern int optd_post_fail_delay;
extern int optd_override_time;
extern int optd_override_path;

extern int cfgd_groupd_compat;
extern int cfgd_debug_logfile;
extern int cfgd_clean_start;
extern int cfgd_disable_dbus;
extern int cfgd_skip_undefined;
extern int cfgd_post_join_delay;
extern int cfgd_post_fail_delay;
extern int cfgd_override_time;
extern const char *cfgd_override_path;

#endif

