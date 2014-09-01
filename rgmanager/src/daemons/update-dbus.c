/* DBus notifications */
#include <stdint.h>
#include <rg_dbus.h>
#include <errno.h>

#ifdef DBUS

#include <stdio.h>
#include <stdint.h>
#include <resgroup.h>
#include <poll.h>
#include <dbus/dbus.h>
#include <liblogthread.h>
#include <members.h>
#include <signal.h>


#define DBUS_RGM_NAME	"com.redhat.cluster.rgmanager"
#define DBUS_RGM_IFACE	"com.redhat.cluster.rgmanager"
#define DBUS_RGM_PATH	"/com/redhat/cluster/rgmanager"

static void * _dbus_auto_flush(void *arg);

static DBusConnection *db = NULL;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t th = 0;
#endif

/* Set this to the desired value prior to calling rgm_dbus_init() */
int rgm_dbus_notify = RGM_DBUS_DEFAULT;

/*
 * block the world when entering dbus critical sections so that
 * if we get a signal while in dbus critical (exclusive of crash
 * signals), we ignore it
 */
#define DBUS_ENTRY(set, old) \
do { \
	pthread_mutex_lock(&mu); \
	sigfillset(&set); \
	sigdelset(&set, SIGILL); \
	sigdelset(&set, SIGSEGV); \
	sigdelset(&set, SIGABRT); \
	sigdelset(&set, SIGBUS); \
	sigprocmask(SIG_SETMASK, &set, &old); \
} while(0)

#define DBUS_EXIT(old) \
do { \
	sigprocmask(SIG_SETMASK, &old, NULL); \
	pthread_mutex_unlock(&mu); \
} while(0)


int 
rgm_dbus_init(void)
#ifdef DBUS
{
	DBusConnection *dbc = NULL;
	DBusError err;
	sigset_t set, old;

	if (!rgm_dbus_notify)
		return 0;

	DBUS_ENTRY(set, old);
	if (db) {
		DBUS_EXIT(old);
		return 0;
	}

	dbus_error_init(&err);

	dbc = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
	if (!dbc) {
		logt_print(LOG_DEBUG,
			   "DBus Failed to initialize: dbus_bus_get: %s\n",
			   err.message);
		dbus_error_free(&err);
		DBUS_EXIT(old);
		return -1;
	}

	dbus_connection_set_exit_on_disconnect(dbc, FALSE);

	db = dbc;

	pthread_create(&th, NULL, _dbus_auto_flush, NULL);

	DBUS_EXIT(old);
	logt_print(LOG_DEBUG, "DBus Notifications Initialized\n");
	return 0;
}
#else
{
	errno = ENOSYS;
	return -1;
}
#endif


#ifdef DBUS
static int
_rgm_dbus_release(void)
{
	pthread_t t;

	if (!db)
		return 0;

	/* tell thread to exit - not sure how to tell dbus
	 * to wake up, so just have it poll XXX */

	/* if the thread left because the dbus connection died,
	   this block is avoided */
	if (th) {
		t = th;
		th = 0;
		pthread_join(t, NULL);
	}

	dbus_connection_close(db);
	dbus_connection_unref(db);
	db = NULL;

	logt_print(LOG_DEBUG, "DBus Released\n");
	return 0;
}
#endif


/* Clean shutdown (e.g. when exiting */
int
rgm_dbus_release(void)
#ifdef DBUS
{
	int ret;
	sigset_t set, old;

	DBUS_ENTRY(set, old);
	ret = _rgm_dbus_release();
	DBUS_EXIT(old);
	return ret;
}
#else
{
	return 0;
}
#endif


#ifdef DBUS
/* Auto-flush thread.  Since sending only guarantees queueing,
 * we need this thread to push things out over dbus in the
 * background */
static void *
_dbus_auto_flush(void *arg)
{
	sigset_t set;

	sigfillset(&set);
	sigdelset(&set, SIGILL);
	sigdelset(&set, SIGSEGV);
	sigdelset(&set, SIGABRT);
	sigdelset(&set, SIGBUS);
	sigprocmask(SIG_SETMASK, &set, NULL);

	/* DBus connection functions are thread safe */
	while (dbus_connection_read_write(db, 500)) {
		if (!th)
			break;	
	}

	th = 0;
	return NULL;
}


static int
_rgm_dbus_notify(const char *svcname,
		 const char *svcstatus,
		 const char *svcflags,
		 const char *svcowner,
		 const char *svclast)
{
	DBusMessage *msg = NULL;
	int ret = 0;
	sigset_t set, old;

	DBUS_ENTRY(set, old);

	if (!db) {
		goto out_unlock;
	}

	/* Notifications are enabled */
	ret = -1;

	/* Check to ensure the connection is still valid. If it
	 * isn't, clean up and shut down the dbus connection.
	 *
	 * The main rgmanager thread will periodically try to
	 * reinitialize the dbus notification subsystem unless
	 * the administrator ran rgmanager with the -D command
	 * line option.
	 */
	if (dbus_connection_get_is_connected(db) != TRUE) {
		goto out_unlock;
	}

	if (!th) {
		goto out_unlock;
	}

	if (!(msg = dbus_message_new_signal(DBUS_RGM_PATH,
	      				    DBUS_RGM_IFACE,
	      				    "ServiceStateChange"))) {
		goto out_unlock;
	}

	if (!dbus_message_append_args(msg,
	 			      DBUS_TYPE_STRING, &svcname,
	 			      DBUS_TYPE_STRING, &svcstatus,
	 			      DBUS_TYPE_STRING, &svcflags,
 				      DBUS_TYPE_STRING, &svcowner,
 				      DBUS_TYPE_STRING, &svclast,
	    			      DBUS_TYPE_INVALID)) {
		goto out_unlock;
	}

	dbus_connection_send(db, msg, NULL);
	ret = 0;

out_unlock:
	DBUS_EXIT(old);
	if (msg)
		dbus_message_unref(msg);

	return ret;
}


/*
 * view-formation callback function
 */
int32_t
rgm_dbus_update(char *key, uint64_t view, void *data, uint32_t size)
{
	char flags[64];
	rg_state_t *st;
	cluster_member_list_t *m = NULL;
	const char *owner;
	const char *last;
	sigset_t set, old;
	int ret = 0;

	if (!rgm_dbus_notify)
		goto out_free;
	if (view == 1)
		goto out_free;
	if (size != (sizeof(*st)))
		goto out_free;
	
	DBUS_ENTRY(set, old);
	if (!db) {
		DBUS_EXIT(old);
		goto out_free;
	}
	if (!th) {
		/* Dispatch thread died. */
		_rgm_dbus_release();
		DBUS_EXIT(old);
		goto out_free;
	}
	DBUS_EXIT(old);

	st = (rg_state_t *)data;
	swab_rg_state_t(st);

	/* Don't send transitional states */
	if (st->rs_state == RG_STATE_STARTING ||
	    st->rs_state == RG_STATE_STOPPING)
		goto out_free;

	m = member_list();
	if (!m)
		goto out_free;

	owner = memb_id_to_name(m, st->rs_owner);
	last = memb_id_to_name(m, st->rs_last_owner);

	if (!owner)
		owner = "(none)";
	if (!last)
		last = "(none)";

	flags[0] = 0;
	rg_flags_str(flags, sizeof(flags), st->rs_flags, (char *)" ");
	if (flags[0] == 0)
		snprintf(flags, sizeof(flags), "(none)");

	ret = _rgm_dbus_notify(st->rs_name,
			       rg_state_str(st->rs_state),
			       (char *)flags, owner, last);

	if (ret < 0) {
		logt_print(LOG_ERR, "Error sending update for %s; "
			   "DBus notifications disabled\n", key);
		rgm_dbus_release();
	}

out_free:
	if (m)
		free_member_list(m);
	free(data);
	return 0;
}
#endif
