#ifndef _RGM_DBUS_H
#define _RGM_DBUS_H

int rgm_dbus_init(void);
int rgm_dbus_release(void);
extern int rgm_dbus_notify;

#ifdef DBUS

#define RGM_DBUS_DEFAULT 1
#define RGM_DBUS_UPDATE (rgm_dbus_notify?rgm_dbus_update:0)
int32_t rgm_dbus_update(char *key, uint64_t view, void *data, uint32_t size);

#else

#define RGM_DBUS_DEFAULT 0
#define RGM_DBUS_UPDATE NULL

#endif /* DBUS */
#endif
