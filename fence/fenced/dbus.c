#include "fd.h"
#include "config.h"

#ifdef DBUS
#include <dbus/dbus.h>

#define DBUS_FENCE_NAME  "com.redhat.cluster.fence"
#define DBUS_FENCE_IFACE "com.redhat.cluster.fence"
#define DBUS_FENCE_PATH  "/com/redhat/cluster/fence"

static DBusConnection *bus = NULL;

void fd_dbus_init(void)
{
    if (!(bus = dbus_bus_get_private(DBUS_BUS_SYSTEM, NULL))) {
	    log_error("failed to get dbus connection");
    } else {
	    log_debug("connected to dbus %s", dbus_bus_get_unique_name(bus));
    }
}

void fd_dbus_exit(void)
{
    if (bus) {
	    dbus_connection_close(bus);
	    dbus_connection_unref(bus);
    }
    bus = NULL;
}

void fd_dbus_send(const char *nodename, int nodeid, int result)
{
    DBusMessage *msg = NULL;

    if (bus && !dbus_connection_read_write(bus, 1)) {
	    log_debug("disconnected from dbus");
	    fd_dbus_exit();
    }

    if (!bus) {
	    fd_dbus_init();
    }

    if (!bus) {
	    goto out;
    }

    if (!(msg = dbus_message_new_signal(DBUS_FENCE_PATH,
					DBUS_FENCE_IFACE,
					"FenceNode"))) {
	    log_error("failed to create dbus signal");
	    goto out;
    }

    if (!dbus_message_append_args(msg,
				   DBUS_TYPE_STRING, &nodename,
				   DBUS_TYPE_INT32, &nodeid,
				   DBUS_TYPE_INT32, &result,
				   DBUS_TYPE_INVALID)) {
	    log_error("failed to append args to dbus signal");
	    goto out;
    }

    dbus_connection_send(bus, msg, NULL);
    dbus_connection_flush(bus);

out:
    if (msg) {
	    dbus_message_unref(msg);
    }
}

#else

void fd_dbus_init(void)
{
}

void fd_dbus_exit(void)
{
}

void fd_dbus_send(const char *nodename, int nodeid, int result)
{
}

#endif /* DBUS */
