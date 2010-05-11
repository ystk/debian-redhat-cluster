#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp_lock.h>
#include <linux/spinlock.h>
#include <linux/semaphore.h>
#include <linux/completion.h>
#include <linux/buffer_head.h>
#include <linux/proc_fs.h>
#include <linux/module.h>
#include <asm/uaccess.h>
#include <linux/genhd.h>

#include "gfs.h"
#include "glock.h"
#include "lm.h"
#include "sys.h"
#include "super.h"

char *gfs_sys_margs;
spinlock_t gfs_sys_margs_lock;

static ssize_t id_show(struct gfs_sbd *sdp, char *buf)
{
	return sprintf(buf, "%s\n", sdp->sd_vfs->s_id);
}

static ssize_t fsname_show(struct gfs_sbd *sdp, char *buf)
{
	return sprintf(buf, "%s\n", sdp->sd_fsname);
}

struct gfs_attr {
	struct attribute attr;
	ssize_t (*show)(struct gfs_sbd *, char *);
	ssize_t (*store)(struct gfs_sbd *, const char *, size_t);
};

#define GFS_ATTR(name, mode, show, store) \
static struct gfs_attr gfs_attr_##name = __ATTR(name, mode, show, store)

GFS_ATTR(id,                  0444, id_show,       NULL);
GFS_ATTR(fsname,              0444, fsname_show,   NULL);

static struct attribute *gfs_attrs[] = {
	&gfs_attr_id.attr,
	&gfs_attr_fsname.attr,
	NULL,
};

static ssize_t gfs_attr_show(struct kobject *kobj, struct attribute *attr,
			      char *buf)
{
	struct gfs_sbd *sdp = container_of(kobj, struct gfs_sbd, sd_kobj);
	struct gfs_attr *a = container_of(attr, struct gfs_attr, attr);
	return a->show ? a->show(sdp, buf) : 0;
}

static ssize_t gfs_attr_store(struct kobject *kobj, struct attribute *attr,
			       const char *buf, size_t len)
{
	struct gfs_sbd *sdp = container_of(kobj, struct gfs_sbd, sd_kobj);
	struct gfs_attr *a = container_of(attr, struct gfs_attr, attr);
	return a->store ? a->store(sdp, buf, len) : len;
}

static struct sysfs_ops gfs_attr_ops = {
	.show  = gfs_attr_show,
	.store = gfs_attr_store,
};

static struct kobj_type gfs_ktype = {
	.default_attrs = gfs_attrs,
	.sysfs_ops     = &gfs_attr_ops,
};

static struct kset *gfs_kset;

int gfs_sys_fs_add(struct gfs_sbd *sdp)
{
	struct super_block *sb = sdp->sd_vfs;
	int error;
	char ro[20];
	char spectator[20];
	char *envp[] = { ro, spectator, NULL };

	sprintf(ro, "RDONLY=%d", (sb->s_flags & MS_RDONLY) ? 1 : 0);
	sprintf(spectator, "SPECTATOR=%d", sdp->sd_args.ar_spectator ? 1 : 0);

	sdp->sd_kobj.kset = gfs_kset;
	error = kobject_init_and_add(&sdp->sd_kobj, &gfs_ktype, NULL,
				     "%s", sdp->sd_table_name);
	if (error)
		goto fail;

	error = sysfs_create_link(&sdp->sd_kobj,
				  &disk_to_dev(sb->s_bdev->bd_disk)->kobj,
				  "device");
	if (error)
		goto fail_put;

	kobject_uevent_env(&sdp->sd_kobj, KOBJ_ADD, envp);

	return 0;

fail_put:
	kobject_put(&sdp->sd_kobj);
fail:
	return error;
}

void gfs_sys_fs_del(struct gfs_sbd *sdp)
{
	kobject_put(&sdp->sd_kobj);
}

static int gfs_uevent(struct kset *kset, struct kobject *kobj,
		       struct kobj_uevent_env *env)
{
	struct gfs_sbd *sdp = container_of(kobj, struct gfs_sbd, sd_kobj);
	add_uevent_var(env, "LOCKTABLE=%s", sdp->sd_table_name);
	add_uevent_var(env, "LOCKPROTO=%s", sdp->sd_proto_name);
	if (!sdp->sd_args.ar_spectator)
		add_uevent_var(env, "JOURNALID=%u", sdp->sd_lockstruct.ls_jid);
	return 0;
}

static struct kset_uevent_ops gfs_uevent_ops = {
	.uevent = gfs_uevent,
};

int gfs_sys_init(void)
{
	gfs_sys_margs = NULL;
	spin_lock_init(&gfs_sys_margs_lock);
	gfs_kset = kset_create_and_add("gfs", &gfs_uevent_ops, fs_kobj);
	if (!gfs_kset)
		return -ENOMEM;
	return 0;
}

void gfs_sys_uninit(void)
{
	kfree(gfs_sys_margs);
	kset_unregister(gfs_kset);
}
